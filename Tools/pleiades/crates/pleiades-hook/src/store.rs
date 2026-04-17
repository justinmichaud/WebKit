use pleiades_core::{CompactEvent, ImageInfo, MallocEventType, StackNode, LOG_MAGIC};
use std::collections::HashMap;
use std::sync::atomic::{AtomicPtr, Ordering};
use std::sync::Mutex;

/// Stack dedup lookup — only the HashMap needs Mutex protection.
/// The actual StackNode data lives in a lock-free boxcar::Vec
/// so flush_to_disk can read it without locking (signal-safe).
struct TrieLookup {
    map: HashMap<(Option<u32>, u64), u32>,
}

/// Pre-allocated buffers for signal-safe flushing.
/// Allocated at init (when malloc is safe), atomically claimed at flush.
struct FlushBuffers {
    images_bin: Vec<u8>,
    write_buf: Vec<u8>,
}

static TRIE: Mutex<Option<TrieLookup>> = Mutex::new(None);
static STACK_NODES: AtomicPtr<boxcar::Vec<StackNode>> = AtomicPtr::new(std::ptr::null_mut());
static EVENTS: AtomicPtr<boxcar::Vec<CompactEvent>> = AtomicPtr::new(std::ptr::null_mut());
static FLUSH_BUF: AtomicPtr<FlushBuffers> = AtomicPtr::new(std::ptr::null_mut());

fn get_boxcar<T>(ptr: &AtomicPtr<boxcar::Vec<T>>) -> Option<&'static boxcar::Vec<T>> {
    let p = ptr.load(Ordering::Acquire);
    if p.is_null() { None } else { Some(unsafe { &*p }) }
}

/// Buffered writer over a raw fd using a pre-allocated Vec.
/// No heap allocation as long as writes stay within the existing capacity.
struct BufFdWriter {
    fd: libc::c_int,
    buf: Vec<u8>,
}

impl BufFdWriter {
    fn new(fd: libc::c_int, buf: Vec<u8>) -> Self {
        Self { fd, buf }
    }

    fn flush(&mut self) {
        if !self.buf.is_empty() {
            unsafe { libc::write(self.fd, self.buf.as_ptr().cast(), self.buf.len()); }
            self.buf.clear();
        }
    }

    fn write_all(&mut self, data: &[u8]) {
        let mut offset = 0;
        while offset < data.len() {
            let remaining = self.buf.capacity() - self.buf.len();
            let chunk = (data.len() - offset).min(remaining);
            self.buf.extend_from_slice(&data[offset..offset + chunk]);
            offset += chunk;
            if self.buf.len() == self.buf.capacity() {
                self.flush();
            }
        }
    }

    /// Current byte offset in the file. Must flush before calling.
    fn offset(&self) -> libc::off_t {
        unsafe { libc::lseek(self.fd, 0, libc::SEEK_CUR) }
    }
}

impl std::io::Write for BufFdWriter {
    fn write(&mut self, data: &[u8]) -> std::io::Result<usize> {
        self.write_all(data);
        Ok(data.len())
    }
    fn flush(&mut self) -> std::io::Result<()> {
        BufFdWriter::flush(self);
        Ok(())
    }
}

// ============================================================
// Platform-specific image list capture
// ============================================================

#[cfg(target_os = "macos")]
mod image_list {
    use super::*;

    unsafe extern "C" {
        fn _dyld_image_count() -> u32;
        fn _dyld_get_image_name(image_index: u32) -> *const libc::c_char;
        fn _dyld_get_image_header(image_index: u32) -> *const libc::c_void;
    }

    fn extract_macho_uuid(header: *const u8) -> [u8; 20] {
        const MH_MAGIC_64: u32 = 0xFEEDFACF;
        const LC_UUID: u32 = 0x1B;
        let mut uuid = [0u8; 20];
        unsafe {
            let magic = *(header as *const u32);
            if magic != MH_MAGIC_64 { return uuid; }
            let ncmds = *(header.add(16) as *const u32);
            let mut cmd_ptr = header.add(32);
            for _ in 0..ncmds {
                let cmd = *(cmd_ptr as *const u32);
                let cmdsize = *(cmd_ptr.add(4) as *const u32);
                if cmd == LC_UUID {
                    // Mach-O UUID is 16 bytes; copy into first 16 bytes of [u8; 20]
                    std::ptr::copy_nonoverlapping(cmd_ptr.add(8), uuid.as_mut_ptr(), 16);
                    return uuid;
                }
                cmd_ptr = cmd_ptr.add(cmdsize as usize);
            }
        }
        uuid
    }

    pub fn capture() -> Vec<ImageInfo> {
        let count = unsafe { _dyld_image_count() };
        let mut images = Vec::new();

        for i in 0..count {
            let name_ptr = unsafe { _dyld_get_image_name(i) };
            let header_ptr = unsafe { _dyld_get_image_header(i) };
            if name_ptr.is_null() || header_ptr.is_null() { continue; }

            let mut len = 0usize;
            unsafe { while *name_ptr.add(len) != 0 { len += 1; } }
            let name = unsafe { std::slice::from_raw_parts(name_ptr as *const u8, len) };
            let path = String::from_utf8_lossy(name).into_owned();
            let uuid = extract_macho_uuid(header_ptr as *const u8);

            images.push(ImageInfo { path, load_address: header_ptr as u64, uuid });
        }

        images
    }
}

#[cfg(target_os = "linux")]
mod image_list {
    use super::*;

    /// Callback data for dl_iterate_phdr.
    struct IterateData {
        images: Vec<ImageInfo>,
    }

    unsafe extern "C" fn callback(
        info: *mut libc::dl_phdr_info,
        _size: libc::size_t,
        data: *mut libc::c_void,
    ) -> libc::c_int {
        let data = &mut *(data as *mut IterateData);
        let info = &*info;

        let name_ptr = info.dlpi_name;
        if name_ptr.is_null() { return 0; }
        let mut len = 0usize;
        while *name_ptr.add(len) != 0 { len += 1; }
        if len == 0 { return 0; } // skip main executable (empty name)
        let name = std::slice::from_raw_parts(name_ptr as *const u8, len);
        let path = String::from_utf8_lossy(name).into_owned();

        let load_address = info.dlpi_addr as u64;

        // Extract GNU build-id from PT_NOTE segments
        let mut uuid = [0u8; 20];
        for i in 0..info.dlpi_phnum as usize {
            let phdr = &*info.dlpi_phdr.add(i);
            if phdr.p_type == libc::PT_NOTE {
                let mut offset = 0usize;
                let note_start = (info.dlpi_addr as usize + phdr.p_vaddr as usize) as *const u8;
                let note_size = phdr.p_memsz as usize;
                while offset + 12 <= note_size {
                    let namesz = *(note_start.add(offset) as *const u32) as usize;
                    let descsz = *(note_start.add(offset + 4) as *const u32) as usize;
                    let note_type = *(note_start.add(offset + 8) as *const u32);
                    let name_off = offset + 12;
                    let desc_off = name_off + ((namesz + 3) & !3);
                    // NT_GNU_BUILD_ID = 3, name = "GNU\0" (namesz = 4)
                    if note_type == 3 && namesz == 4 {
                        let copy_len = descsz.min(20);
                        std::ptr::copy_nonoverlapping(
                            note_start.add(desc_off), uuid.as_mut_ptr(), copy_len,
                        );
                        break;
                    }
                    offset = desc_off + ((descsz + 3) & !3);
                }
            }
        }

        data.images.push(ImageInfo { path, load_address, uuid });
        0
    }

    pub fn capture() -> Vec<ImageInfo> {
        let mut data = IterateData { images: Vec::new() };
        unsafe {
            libc::dl_iterate_phdr(Some(callback), &mut data as *mut IterateData as *mut libc::c_void);
        }
        data.images
    }
}

// ============================================================
// Common code
// ============================================================

pub fn capture_image_list() {
    let images = image_list::capture();

    // Pre-serialize images and allocate flush buffers (malloc is safe here).
    let images_bin = bincode::serialize(&images).unwrap_or_default();
    let flush = Box::new(FlushBuffers {
        images_bin,
        write_buf: Vec::with_capacity(8 * 1024),
    });
    FLUSH_BUF.store(Box::into_raw(flush), Ordering::Release);

    *TRIE.lock().unwrap() = Some(TrieLookup {
        map: HashMap::with_capacity(64 * 1024),
    });

    let nodes = Box::new(boxcar::Vec::with_capacity(64 * 1024));
    STACK_NODES.store(Box::into_raw(nodes), Ordering::Release);

    let events = Box::new(boxcar::Vec::with_capacity(1024 * 1024));
    EVENTS.store(Box::into_raw(events), Ordering::Release);
}

pub fn record_event(
    timestamp_ns: u64,
    event_type: MallocEventType,
    arg1: u64, arg2: u64, arg3: u64, return_val: u64,
    ips: &[u64],
    tid: u64, pid: u32,
) {
    let Some(nodes) = get_boxcar(&STACK_NODES) else { return };

    // Intern stack: Mutex protects only the dedup HashMap.
    // New StackNodes are pushed to a lock-free boxcar.
    let Ok(mut guard) = TRIE.try_lock() else { return };
    let Some(trie) = guard.as_mut() else { return };
    let stack_id = {
        let mut parent: Option<u32> = None;
        for &ip in ips.iter().rev() {
            let key = (parent, ip);
            let id = if let Some(&existing) = trie.map.get(&key) {
                existing
            } else {
                let id = nodes.push(StackNode { parent, ip }) as u32;
                trie.map.insert(key, id);
                id
            };
            parent = Some(id);
        }
        parent.unwrap_or(0)
    };
    drop(guard);

    // Push event to boxcar (lock-free, outside the Mutex)
    let Some(events) = get_boxcar(&EVENTS) else { return };
    events.push(CompactEvent {
        timestamp_ns, event_type,
        arg1, arg2, arg3, return_val,
        stack_id, tid, pid,
    });
}

/// Flush all recorded data to disk. Allocation-free: uses pre-allocated
/// buffers claimed atomically via FLUSH_BUF.swap(null). Safe to call
/// from a signal handler (no malloc, no Mutex).
pub fn flush_to_disk() {
    // Atomically claim the flush buffers. Second call gets null → returns.
    let buf_ptr = FLUSH_BUF.swap(std::ptr::null_mut(), Ordering::AcqRel);
    if buf_ptr.is_null() { return; }
    let buf = unsafe { &mut *buf_ptr };

    let Some(nodes_box) = get_boxcar(&STACK_NODES) else { return };
    let Some(events_box) = get_boxcar(&EVENTS) else { return };

    // Snapshot counts before iterating. Only emit this many elements
    // (boxcar may grow during iteration from other threads' in-flight pushes).
    let node_count = nodes_box.count();
    let event_count = events_box.count();

    // Open output file.
    let env_key = c"PLEIADES_DB_PATH";
    let path_ptr = unsafe { libc::getenv(env_key.as_ptr()) };
    let c_path = if !path_ptr.is_null() {
        path_ptr
    } else {
        c"/tmp/pleiades-alloc.bin".as_ptr()
    };
    let fd = unsafe { libc::open(c_path, libc::O_WRONLY | libc::O_CREAT | libc::O_TRUNC, 0o644) };
    if fd < 0 { return; }

    let mut w = BufFdWriter::new(fd, std::mem::take(&mut buf.write_buf));

    // Magic
    w.write_all(&LOG_MAGIC.to_ne_bytes());

    // Images section (pre-serialized at init)
    let images_len = buf.images_bin.len() as u32;
    w.write_all(&images_len.to_ne_bytes());
    w.write_all(&buf.images_bin);

    // Stack nodes section — write placeholder, serialize, patch length via lseek.
    write_section(&mut w, nodes_box, node_count);

    // Events section
    write_section(&mut w, events_box, event_count);

    w.flush();
    unsafe { libc::close(fd); }

    // Log to stderr (no format!(), no allocation)
    {
        let mut nb = [0u8; 20];
        unsafe {
            let m = b"[pleiades-hook] flushing ";
            libc::write(2, m.as_ptr().cast(), m.len());
            let s = fmt_usize(event_count, &mut nb);
            libc::write(2, s.as_ptr().cast(), s.len());
            let m = b" events, ";
            libc::write(2, m.as_ptr().cast(), m.len());
            let s = fmt_usize(node_count, &mut nb);
            libc::write(2, s.as_ptr().cast(), s.len());
            let m = b" stack nodes\n";
            libc::write(2, m.as_ptr().cast(), m.len());
        }
    }
}

/// Write a boxcar section: [u32 byte_len][u64 count][elements...]
/// Uses lseek to patch byte_len after serialization.
fn write_section<T: serde::Serialize + Copy>(
    w: &mut BufFdWriter, boxcar: &boxcar::Vec<T>, count: usize,
) {
    // Write placeholder for section byte length
    w.write_all(&0u32.to_ne_bytes());
    w.flush();
    let section_start = w.offset();

    // Write element count (bincode Vec header: u64 length)
    w.write_all(&(count as u64).to_ne_bytes());

    // Serialize each element through the buffered writer
    for (i, (_, item)) in boxcar.iter().enumerate() {
        if i >= count { break; }
        let _ = bincode::serialize_into(&mut *w, &*item);
    }

    // Patch the section byte length
    w.flush();
    let section_end = w.offset();
    let section_len = (section_end - section_start) as u32;
    unsafe {
        libc::lseek(w.fd, section_start - 4, libc::SEEK_SET);
        libc::write(w.fd, (&section_len as *const u32).cast(), 4);
        libc::lseek(w.fd, section_end, libc::SEEK_SET);
    }
}

fn fmt_usize(mut val: usize, buf: &mut [u8; 20]) -> &[u8] {
    if val == 0 { return b"0"; }
    let mut i = buf.len();
    while val > 0 {
        i -= 1;
        buf[i] = b'0' + (val % 10) as u8;
        val /= 10;
    }
    &buf[i..]
}
