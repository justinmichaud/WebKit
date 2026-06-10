use serde::{Deserialize, Serialize};

/// Wraps the raw `type` bitmask passed to `malloc_logger`.
///
/// Known bits from Apple's malloc internals:
///   - 0x02: allocation
///   - 0x04: deallocation
///   - 0x08: indicates zone-based operation
///   - 0x40: cleared memory (calloc)
#[derive(Debug, Clone, Copy, Serialize, Deserialize, PartialEq, Eq)]
pub struct MallocEventType(pub u32);

impl MallocEventType {
    pub fn is_alloc(self) -> bool {
        self.0 & 0x02 != 0 && self.0 & 0x04 == 0
    }

    pub fn is_free(self) -> bool {
        self.0 & 0x04 != 0 && self.0 & 0x02 == 0
    }

    pub fn is_realloc(self) -> bool {
        self.0 & 0x02 != 0 && self.0 & 0x04 != 0
    }

    pub fn is_calloc(self) -> bool {
        self.0 & 0x40 != 0
    }
}

impl std::fmt::Display for MallocEventType {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        if self.is_realloc() {
            write!(f, "realloc")
        } else if self.is_calloc() {
            write!(f, "calloc")
        } else if self.is_alloc() {
            write!(f, "malloc")
        } else if self.is_free() {
            write!(f, "free")
        } else {
            write!(f, "0x{:04x}", self.0)
        }
    }
}

/// A single allocation event record.
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct AllocationRecord {
    pub timestamp_ns: u64,
    pub event_type: MallocEventType,
    /// arg1: typically the malloc zone pointer
    pub arg1: u64,
    /// arg2: size for malloc/calloc, pointer for free, old pointer for realloc
    pub arg2: u64,
    /// arg3: new size for realloc, 0 otherwise
    pub arg3: u64,
    /// return_val: the newly allocated pointer (0 for free)
    pub return_val: u64,
    /// Raw instruction pointers (no symbolication — deferred to CLI)
    pub frames: Vec<u64>,
    /// Thread ID (from pthread_threadid_np)
    pub tid: u64,
    /// Process ID (from getpid)
    pub pid: u32,
}

impl AllocationRecord {
    /// Allocation size in bytes, if applicable.
    pub fn size(&self) -> Option<u64> {
        if self.event_type.is_realloc() {
            Some(self.arg3)
        } else if self.event_type.is_alloc() || self.event_type.is_calloc() {
            Some(self.arg2)
        } else {
            None
        }
    }

    /// The pointer being freed or realloced.
    pub fn freed_pointer(&self) -> Option<u64> {
        if self.event_type.is_free() || self.event_type.is_realloc() {
            Some(self.arg2)
        } else {
            None
        }
    }

    /// The memory address relevant to this event (for alloc/free correlation).
    pub fn memory_address(&self) -> u64 {
        if self.event_type.is_free() {
            self.arg2
        } else {
            self.return_val
        }
    }

    /// Signed weight in bytes for profiler (positive = alloc, negative = free).
    pub fn weight_bytes(&self) -> i64 {
        if self.event_type.is_free() {
            // Free: we don't know the size, use -1 as a marker
            // The profiler correlates by memoryAddress to compute retained
            -1
        } else if self.event_type.is_realloc() {
            self.arg3 as i64
        } else {
            self.size().unwrap_or(0) as i64
        }
    }

    /// Format a human-readable summary of the event.
    pub fn summary(&self) -> String {
        if self.event_type.is_realloc() {
            format!(
                "realloc(0x{:x}, {} bytes) -> 0x{:x}",
                self.arg2, self.arg3, self.return_val
            )
        } else if self.event_type.is_calloc() {
            format!(
                "calloc({} bytes) -> 0x{:x}",
                self.arg2, self.return_val
            )
        } else if self.event_type.is_alloc() {
            format!(
                "malloc({} bytes) -> 0x{:x}",
                self.arg2, self.return_val
            )
        } else if self.event_type.is_free() {
            format!("free(0x{:x})", self.arg2)
        } else {
            format!(
                "0x{:04x}(arg1=0x{:x}, arg2=0x{:x}, arg3=0x{:x}) -> 0x{:x}",
                self.event_type.0, self.arg1, self.arg2, self.arg3, self.return_val
            )
        }
    }
}

/// Compact allocation event (on-disk format). Stack is stored as an index
/// into a shared stack tree, not as a full backtrace.
#[derive(Debug, Clone, Copy, Serialize, Deserialize)]
pub struct CompactEvent {
    pub timestamp_ns: u64,
    pub event_type: MallocEventType,
    pub arg1: u64,
    pub arg2: u64,
    pub arg3: u64,
    pub return_val: u64,
    pub stack_id: u32,
    pub tid: u64,
    pub pid: u32,
}

/// Stack tree node. Stacks are stored as a linked list: each node has
/// a frame IP and an optional parent index.
#[derive(Debug, Clone, Copy, Serialize, Deserialize)]
pub struct StackNode {
    pub parent: Option<u32>,
    pub ip: u64,
}

/// Info about a loaded dylib in the target process.
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct ImageInfo {
    pub path: String,
    pub load_address: u64,
    /// Build ID: Mach-O UUID (16 bytes, zero-padded) or ELF GNU build-id (up to 20 bytes)
    pub uuid: [u8; 20],
}

pub const DEFAULT_LOG_PATH: &str = "/tmp/pleiades-alloc.bin";
pub const LOG_PATH_ENV: &str = "PLEIADES_DB_PATH";
/// Magic bytes at the start of the log file: "PLD2" (version 2 — stack-dedup format)
pub const LOG_MAGIC: u32 = 0x424D4B32;

/// Split a log-path template into the (prefix, suffix) that bracket the pid.
///
/// A `%p` in the template marks where the pid goes. Without one, the pid is
/// inserted before the extension of the final path component (or appended if
/// there is none). `resolve_log_path` and `match_log_path` share this so the
/// writer and reader can never disagree on the naming scheme.
fn split_log_template(template: &str) -> (String, String) {
    if let Some(i) = template.find("%p") {
        return (template[..i].to_string(), template[i + 2..].to_string());
    }
    let slash = template.rfind('/').map_or(0, |i| i + 1);
    let file = &template[slash..];
    match file.rfind('.') {
        // dot > 0 so we don't treat a dotfile (".foo") as having an extension.
        Some(dot) if dot > 0 => {
            let cut = slash + dot; // index of the '.' within `template`
            (format!("{}.", &template[..cut]), template[cut..].to_string())
        }
        _ => (format!("{template}."), String::new()),
    }
}

/// Sanitize a process name for use as a single filename path segment, so the
/// `.<name>.<pid>.` scheme stays unambiguously parseable (no `/` or `.`).
pub fn sanitize_name(name: &str) -> String {
    let s: String = name
        .chars()
        .map(|c| if c.is_ascii_alphanumeric() || c == '-' || c == '_' { c } else { '_' })
        .collect();
    if s.is_empty() { "unknown".to_string() } else { s }
}

/// A per-process capture file's identity, recovered from its filename.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct LogTag {
    /// Process name (e.g. `WebKitWebProcess`). Empty for a pid-only `%p` template.
    pub name: String,
    pub pid: u32,
}

/// Expand a log-path template for one process. Keeps per-process captures from
/// clobbering each other when a single `LD_PRELOAD` spans multiple processes
/// (e.g. WebKit's UI / Web / Network processes), and embeds the process name so
/// the files are self-identifying.
///
/// `/tmp/pleiades-alloc.bin` → `/tmp/pleiades-alloc.<name>.<pid>.bin`.
/// A `%p` in the template is replaced by the pid verbatim (no name inserted —
/// the caller chose an explicit layout): `/tmp/run-%p.bin` → `/tmp/run-<pid>.bin`.
pub fn resolve_log_path(template: &str, name: &str, pid: u32) -> String {
    let (prefix, suffix) = split_log_template(template);
    if template.contains("%p") {
        format!("{prefix}{pid}{suffix}")
    } else {
        format!("{prefix}{}.{pid}{suffix}", sanitize_name(name))
    }
}

/// If `candidate` is a per-process file produced from `template`, return its
/// identity (process name + pid). Lets the reader discover the concrete files a
/// template expanded to and tell the WebContent process from the others.
pub fn match_log_path(template: &str, candidate: &str) -> Option<LogTag> {
    let (prefix, suffix) = split_log_template(template);
    let middle = candidate.strip_prefix(&prefix)?.strip_suffix(&suffix)?;
    if template.contains("%p") {
        return Some(LogTag { name: String::new(), pid: middle.parse().ok()? });
    }
    // middle == "<name>.<pid>"; name is sanitized so the last '.' splits the pid.
    let dot = middle.rfind('.')?;
    Some(LogTag { name: middle[..dot].to_string(), pid: middle[dot + 1..].parse().ok()? })
}

#[cfg(test)]
mod log_path_tests {
    use super::*;

    fn tag(name: &str, pid: u32) -> Option<LogTag> {
        Some(LogTag { name: name.to_string(), pid })
    }

    #[test]
    fn default_inserts_name_and_pid_before_extension() {
        assert_eq!(
            resolve_log_path("/tmp/pleiades-alloc.bin", "WebKitWebProcess", 42),
            "/tmp/pleiades-alloc.WebKitWebProcess.42.bin"
        );
        assert_eq!(
            match_log_path("/tmp/pleiades-alloc.bin", "/tmp/pleiades-alloc.WebKitWebProcess.42.bin"),
            tag("WebKitWebProcess", 42)
        );
        // The old pid-less file must not be mistaken for a per-process file.
        assert_eq!(match_log_path("/tmp/pleiades-alloc.bin", "/tmp/pleiades-alloc.bin"), None);
    }

    #[test]
    fn percent_p_placeholder_is_pid_only() {
        assert_eq!(resolve_log_path("/tmp/run-%p.bin", "MiniBrowser", 7), "/tmp/run-7.bin");
        assert_eq!(match_log_path("/tmp/run-%p.bin", "/tmp/run-7.bin"), tag("", 7));
    }

    #[test]
    fn name_with_slashes_is_sanitized() {
        assert_eq!(
            resolve_log_path("/tmp/p.bin", "weird/name.x", 3),
            "/tmp/p.weird_name_x.3.bin"
        );
        assert_eq!(
            match_log_path("/tmp/p.bin", "/tmp/p.weird_name_x.3.bin"),
            tag("weird_name_x", 3)
        );
    }
}
