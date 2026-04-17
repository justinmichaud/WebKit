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
