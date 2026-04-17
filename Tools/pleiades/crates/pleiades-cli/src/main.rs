use pleiades_core::{
    CompactEvent, ImageInfo, StackNode,
    DEFAULT_LOG_PATH, LOG_MAGIC, LOG_PATH_ENV,
};
use clap::{Parser, Subcommand};
use std::collections::HashMap;
use std::io::Read;

#[derive(Parser)]
#[command(name = "pleiades")]
#[command(about = "Analyze malloc allocation traces captured by pleiades-hook")]
struct Cli {
    /// Path to the binary log file [env: PLEIADES_DB_PATH]
    #[arg(long, env = LOG_PATH_ENV, default_value = DEFAULT_LOG_PATH)]
    log_path: String,

    #[command(subcommand)]
    command: Commands,
}

#[derive(Subcommand)]
enum Commands {
    /// Dump allocation records with offline symbolication
    Dump {
        #[arg(long, default_value = "100")]
        limit: usize,
    },
    /// Show summary statistics
    Stats,
    /// Export to Firefox Profiler JSON (viewable via `samply load`)
    Export {
        #[arg(short, long, default_value = "/tmp/pleiades-profile.json")]
        output: String,
    },
}

struct ProfileData {
    images: Vec<ImageInfo>,
    stack_nodes: Vec<StackNode>,
    events: Vec<CompactEvent>,
}

fn read_log(path: &str) -> Result<ProfileData, Box<dyn std::error::Error>> {
    let mut file = std::fs::File::open(path)?;

    let mut magic_buf = [0u8; 4];
    file.read_exact(&mut magic_buf)?;
    let magic = u32::from_ne_bytes(magic_buf);
    if magic != LOG_MAGIC {
        return Err(format!("bad magic: 0x{magic:08x}, expected 0x{LOG_MAGIC:08x}").into());
    }

    let mut len_buf = [0u8; 4];

    // Read images
    file.read_exact(&mut len_buf)?;
    let mut data = vec![0u8; u32::from_ne_bytes(len_buf) as usize];
    file.read_exact(&mut data)?;
    let images: Vec<ImageInfo> = bincode::deserialize(&data)?;

    // Read stack nodes
    file.read_exact(&mut len_buf)?;
    let mut data = vec![0u8; u32::from_ne_bytes(len_buf) as usize];
    file.read_exact(&mut data)?;
    let stack_nodes: Vec<StackNode> = bincode::deserialize(&data)?;

    // Read events
    file.read_exact(&mut len_buf)?;
    let mut data = vec![0u8; u32::from_ne_bytes(len_buf) as usize];
    file.read_exact(&mut data)?;
    let events: Vec<CompactEvent> = bincode::deserialize(&data)?;

    Ok(ProfileData { images, stack_nodes, events })
}

/// Collect IPs for a stack by walking the stack tree.
fn collect_stack_ips(nodes: &[StackNode], stack_id: u32) -> Vec<u64> {
    let mut ips = Vec::new();
    let mut current = Some(stack_id);
    while let Some(id) = current {
        let node = &nodes[id as usize];
        ips.push(node.ip);
        current = node.parent;
    }
    // ips is now innermost-first; reverse for outermost-first
    ips.reverse();
    ips
}

/// Offline symbolicator for the `dump` command.
struct Symbolicator {
    images: Vec<ImageInfo>,
    cache: HashMap<u64, String>,
}

impl Symbolicator {
    fn new(mut images: Vec<ImageInfo>) -> Self {
        images.sort_by_key(|i| i.load_address);
        Self { images, cache: HashMap::new() }
    }

    fn find_image(&self, ip: u64) -> Option<&ImageInfo> {
        let idx = self.images.partition_point(|img| img.load_address <= ip);
        if idx == 0 { return None; }
        Some(&self.images[idx - 1])
    }

    fn resolve_all_ips(&mut self, all_ips: &[u64]) {
        let mut ips_by_image: HashMap<String, Vec<(u64, u64)>> = HashMap::new();
        for &ip in all_ips {
            if self.cache.contains_key(&ip) { continue; }
            if let Some(image) = self.find_image(ip) {
                let offset = ip - image.load_address;
                ips_by_image.entry(image.path.clone()).or_default().push((ip, offset));
            }
        }
        for (path, ips) in &ips_by_image {
            self.symbolicate_image(path, ips);
        }
    }

    fn symbolicate_image(&mut self, path: &str, ips: &[(u64, u64)]) {
        let file = match std::fs::File::open(path) {
            Ok(f) => f,
            Err(_) => {
                for &(ip, _) in ips { self.cache.insert(ip, format!("<{path}>")); }
                return;
            }
        };
        let mmap = match unsafe { memmap2::Mmap::map(&file) } { Ok(m) => m, Err(_) => return };
        let obj = match object::File::parse(&*mmap) { Ok(o) => o, Err(_) => return };

        // DWARF
        let endian = if obj.is_little_endian() { gimli::RunTimeEndian::Little } else { gimli::RunTimeEndian::Big };
        let load_section = |id: gimli::SectionId| -> Result<gimli::EndianSlice<'_, gimli::RunTimeEndian>, gimli::Error> {
            use object::{Object, ObjectSection};
            let data = obj.section_by_name(id.name()).and_then(|s| s.uncompressed_data().ok()).unwrap_or(std::borrow::Cow::Borrowed(&[]));
            let slice = unsafe { std::slice::from_raw_parts(data.as_ptr(), data.len()) };
            Ok(gimli::EndianSlice::new(slice, endian))
        };
        if let Ok(dwarf) = gimli::Dwarf::load(load_section) {
            if let Ok(ctx) = addr2line::Context::from_dwarf(dwarf) {
                for &(ip, offset) in ips {
                    if self.cache.contains_key(&ip) { continue; }
                    if let Ok(mut frames) = ctx.find_frames(offset).skip_all_loads() {
                        if let Ok(Some(frame)) = frames.next() {
                            let name = frame.function.as_ref().and_then(|f| f.demangle().ok()).map(|s| s.into_owned()).unwrap_or_default();
                            if !name.is_empty() {
                                let loc = frame.location.as_ref().map(|l| format!(" ({}:{})", l.file.unwrap_or("?"), l.line.unwrap_or(0))).unwrap_or_default();
                                self.cache.insert(ip, format!("{name}{loc}"));
                                continue;
                            }
                        }
                    }
                }
            }
        }

        // Symbol table fallback
        use object::{Object, ObjectSymbol};
        let mut symbols: Vec<(u64, u64, String)> = obj.symbols().filter_map(|sym| {
            let addr = sym.address(); let size = sym.size(); let name = sym.name().ok()?;
            if addr == 0 || name.is_empty() { return None; }
            Some((addr, size, demangle(name)))
        }).collect();
        symbols.sort_by_key(|(a, _, _)| *a);

        for &(ip, offset) in ips {
            if self.cache.contains_key(&ip) { continue; }
            let idx = symbols.partition_point(|&(a, _, _)| a <= offset);
            if idx > 0 {
                let (sa, ss, ref name) = symbols[idx - 1];
                if ss == 0 || offset < sa + ss {
                    self.cache.insert(ip, format!("{}+0x{:x}", name, offset - sa));
                }
            }
        }
    }

    fn resolve(&self, ip: u64) -> &str {
        self.cache.get(&ip).map(|s| s.as_str()).unwrap_or("<unknown>")
    }
}

fn demangle(name: &str) -> String {
    if name.starts_with("_R") || name.starts_with("__R") {
        if let Ok(d) = rustc_demangle::try_demangle(name.trim_start_matches('_')) { return d.to_string(); }
    }
    if name.starts_with("_Z") || name.starts_with("__Z") {
        if let Some(d) = cpp_demangle::Symbol::new(name.as_bytes()).ok().and_then(|s| s.demangle(&cpp_demangle::DemangleOptions::default()).ok()) { return d; }
    }
    name.to_string()
}

fn event_summary(e: &CompactEvent) -> String {
    let et = e.event_type;
    if et.is_realloc() { format!("realloc(0x{:x}, {} bytes) -> 0x{:x}", e.arg2, e.arg3, e.return_val) }
    else if et.is_calloc() { format!("calloc({} bytes) -> 0x{:x}", e.arg2, e.return_val) }
    else if et.is_alloc() { format!("malloc({} bytes) -> 0x{:x}", e.arg2, e.return_val) }
    else if et.is_free() { format!("free(0x{:x})", e.arg2) }
    else { format!("0x{:04x}(0x{:x}, 0x{:x}, 0x{:x}) -> 0x{:x}", et.0, e.arg1, e.arg2, e.arg3, e.return_val) }
}

fn export_profile(data: &ProfileData, output: &str) -> Result<(), Box<dyn std::error::Error>> {
    use fxprof_processed_profile::{
        CategoryColor, Frame, FrameFlags, FrameInfo,
        Profile, ReferenceTimestamp, SamplingInterval, Timestamp,
    };

    if data.events.is_empty() { return Err("no events".into()); }

    let first_ns = data.events[0].timestamp_ns;
    let mut profile = Profile::new(
        "pleiades",
        ReferenceTimestamp::from_millis_since_unix_epoch(0.0),
        SamplingInterval::from_nanos(1000),
    );

    let memory_category = profile.add_category("Memory", CategoryColor::LightBlue);

    let pid = data.events[0].pid;
    let process = profile.add_process("target", pid, Timestamp::from_nanos_since_reference(first_ns));
    let main_thread = profile.add_thread(process, data.events[0].tid as u32, Timestamp::from_nanos_since_reference(first_ns), true);
    profile.set_thread_name(main_thread, "Main Thread");

    // Add libs (sorted by load_address for proper range computation)
    let mut sorted_images: Vec<_> = data.images.iter().collect();
    sorted_images.sort_by_key(|img| img.load_address);
    for (i, image) in sorted_images.iter().enumerate() {
        let name = std::path::Path::new(&image.path).file_name().and_then(|n| n.to_str()).unwrap_or(&image.path);
        // Use first 16 bytes for breakpad ID (Mach-O UUID or truncated ELF build-id)
        let uuid_hex = image.uuid[..16].iter().map(|b| format!("{b:02X}")).collect::<String>();
        let debug_id = fxprof_processed_profile::debugid::DebugId::from_breakpad(&format!("{uuid_hex}0")).unwrap_or_default();
        let lib = profile.add_lib(fxprof_processed_profile::LibraryInfo {
            name: name.to_string(), debug_name: name.to_string(),
            path: image.path.clone(), debug_path: image.path.clone(),
            debug_id, code_id: None, arch: None, symbol_table: None,
        });
        let end = if i + 1 < sorted_images.len() { sorted_images[i + 1].load_address } else { image.load_address + 0x10000000 };
        profile.add_lib_mapping(process, lib, image.load_address, end, 0);
    }

    let counter = profile.add_counter(process, "malloc", "Memory", "Amount of allocated memory");

    // Pre-build StackHandle for each stack node (nodes are already deduped)
    eprintln!("Building {} stack nodes...", data.stack_nodes.len());
    let mut stack_handles: Vec<Option<fxprof_processed_profile::StackHandle>> = Vec::with_capacity(data.stack_nodes.len());
    for node in &data.stack_nodes {
        let frame_info = FrameInfo {
            frame: Frame::InstructionPointer(node.ip),
            category_pair: memory_category.into(),
            flags: FrameFlags::empty(),
        };
        let frame_handle = profile.intern_frame(main_thread, frame_info);
        let parent = node.parent.map(|p| stack_handles[p as usize].unwrap());
        let sh = profile.intern_stack(main_thread, parent, frame_handle);
        stack_handles.push(Some(sh));
    }

    // Track alloc sizes so we can emit correct negative weights for free.
    // address → size
    let mut alloc_sizes: HashMap<u64, u64> = HashMap::new();

    eprintln!("Adding {} events...", data.events.len());
    for event in &data.events {
        let ts = Timestamp::from_nanos_since_reference(event.timestamp_ns);
        let stack = if (event.stack_id as usize) < stack_handles.len() {
            stack_handles[event.stack_id as usize]
        } else {
            None
        };
        let et = event.event_type;

        if et.is_realloc() {
            // Realloc = free(old_ptr) + alloc(new_ptr)
            // arg2 = old pointer, arg3 = new size, return_val = new pointer
            let old_ptr = event.arg2;
            let new_size = event.arg3;
            let new_ptr = event.return_val;

            // Free the old allocation
            let old_size = alloc_sizes.remove(&old_ptr).unwrap_or(0);
            if old_size > 0 {
                profile.add_allocation_sample(
                    main_thread, ts, stack, old_ptr, -(old_size as i64),
                );
                profile.add_counter_sample(counter, ts, -(old_size as f64), 1);
            }

            // Alloc the new one
            alloc_sizes.insert(new_ptr, new_size);
            profile.add_allocation_sample(
                main_thread, ts, stack, new_ptr, new_size as i64,
            );
            profile.add_counter_sample(counter, ts, new_size as f64, 1);
        } else if et.is_free() {
            // arg2 = pointer being freed
            let ptr = event.arg2;
            let size = alloc_sizes.remove(&ptr).unwrap_or(0);
            let weight = if size > 0 { -(size as i64) } else { -1 };
            profile.add_allocation_sample(main_thread, ts, stack, ptr, weight);
            let delta = if size > 0 { -(size as f64) } else { 0.0 };
            profile.add_counter_sample(counter, ts, delta, 1);
        } else if et.is_alloc() || et.is_calloc() {
            // arg2 = size, return_val = pointer
            let size = event.arg2;
            let ptr = event.return_val;
            alloc_sizes.insert(ptr, size);
            profile.add_allocation_sample(main_thread, ts, stack, ptr, size as i64);
            profile.add_counter_sample(counter, ts, size as f64, 1);
        }
    }

    let file = std::fs::File::create(output)?;
    serde_json::to_writer(std::io::BufWriter::new(file), &profile)?;
    eprintln!("Exported {} events to {output}", data.events.len());
    eprintln!("View with: samply load {output}");
    Ok(())
}

fn main() -> Result<(), Box<dyn std::error::Error>> {
    let cli = Cli::parse();
    let data = read_log(&cli.log_path)?;

    eprintln!("Loaded {} images, {} stack nodes, {} events",
        data.images.len(), data.stack_nodes.len(), data.events.len());

    match cli.command {
        Commands::Dump { limit } => {
            // Collect all unique IPs from stack tree for symbolication
            let all_ips: Vec<u64> = data.stack_nodes.iter().map(|n| n.ip).collect();
            let mut sym = Symbolicator::new(data.images.clone());
            eprintln!("Symbolicating {} unique frames...", all_ips.len());
            sym.resolve_all_ips(&all_ips);

            for (i, event) in data.events.iter().enumerate() {
                if i >= limit { break; }
                println!("#{i}: {}", event_summary(event));
                println!("  timestamp: {}ns  tid: {}", event.timestamp_ns, event.tid);
                let ips = collect_stack_ips(&data.stack_nodes, event.stack_id);
                for (j, ip) in ips.iter().enumerate() {
                    println!("  [{j}] 0x{ip:x}  {}", sym.resolve(*ip));
                }
                println!();
            }
        }
        Commands::Stats => {
            println!("Total events: {}", data.events.len());
            println!("Unique stack nodes: {}", data.stack_nodes.len());
            let allocs = data.events.iter().filter(|e| e.event_type.is_alloc()).count();
            let frees = data.events.iter().filter(|e| e.event_type.is_free()).count();
            let reallocs = data.events.iter().filter(|e| e.event_type.is_realloc()).count();
            let callocs = data.events.iter().filter(|e| e.event_type.is_calloc()).count();
            println!("  malloc:  {allocs}");
            println!("  calloc:  {callocs}");
            println!("  free:    {frees}");
            println!("  realloc: {reallocs}");
            let total: u64 = data.events.iter().filter(|e| e.event_type.is_alloc() || e.event_type.is_calloc()).map(|e| e.arg2).sum();
            println!("  total bytes allocated: {total}");
        }
        Commands::Export { output } => {
            export_profile(&data, &output)?;
        }
    }

    Ok(())
}
