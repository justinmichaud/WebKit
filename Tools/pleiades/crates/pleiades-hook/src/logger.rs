use crate::backtrace_capture;
use crate::reentrancy::ReentrancyGuard;
use crate::store;
use pleiades_core::MallocEventType;
use std::sync::atomic::{AtomicBool, Ordering};

static ACTIVATED: AtomicBool = AtomicBool::new(false);

/// Check if `main` appears in the current call stack (portable: dladdr works on both platforms).
fn stack_contains_main() -> bool {
    const MAX: usize = 64;
    let mut buf = [std::ptr::null_mut::<libc::c_void>(); MAX];
    let count = backtrace_capture::raw_backtrace(buf.as_mut_ptr(), MAX as libc::c_int);
    let count = count.max(0) as usize;

    for &ip in &buf[..count] {
        let mut info: libc::Dl_info = unsafe { std::mem::zeroed() };
        if unsafe { libc::dladdr(ip, &mut info) } != 0 && !info.dli_sname.is_null() {
            let s = info.dli_sname;
            unsafe {
                if *s.add(0) == b'm' as i8
                    && *s.add(1) == b'a' as i8
                    && *s.add(2) == b'i' as i8
                    && *s.add(3) == b'n' as i8
                    && *s.add(4) == 0
                {
                    return true;
                }
            }
        }
    }
    false
}

/// The malloc_logger callback — same signature on all platforms.
/// The target process provides the `malloc_logger` function pointer.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn pleiades_on_malloc(
    event_type: u32,
    arg1: usize,
    arg2: usize,
    arg3: usize,
    return_val: usize,
) {
    let _guard = match ReentrancyGuard::try_enter() {
        Some(g) => g,
        None => return,
    };

    if !ACTIVATED.load(Ordering::Acquire) {
        if !stack_contains_main() {
            return;
        }
        ACTIVATED.store(true, Ordering::Release);
    }

    let timestamp_ns = monotonic_time_ns();

    const MAX_FRAMES: usize = 64;
    let mut buf = [0u64; MAX_FRAMES];
    let mut count = 0usize;
    {
        let mut raw_buf = [std::ptr::null_mut::<libc::c_void>(); MAX_FRAMES];
        let n = backtrace_capture::raw_backtrace(raw_buf.as_mut_ptr(), MAX_FRAMES as libc::c_int);
        let n = n.max(0) as usize;
        let skip = 4.min(n);
        for i in skip..n {
            buf[count] = raw_buf[i] as u64;
            count += 1;
        }
    }

    let tid = current_thread_id();
    let pid = unsafe { libc::getpid() } as u32;

    // Catch panics at the FFI boundary to prevent aborting the host process.
    let _ = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
        store::record_event(
            timestamp_ns,
            MallocEventType(event_type),
            arg1 as u64, arg2 as u64, arg3 as u64, return_val as u64,
            &buf[..count],
            tid, pid,
        );
    }));
}

// --- macOS: mach_absolute_time + pthread_threadid_np ---

#[cfg(target_os = "macos")]
mod platform {
    #[repr(C)]
    struct MachTimebaseInfo {
        numer: u32,
        denom: u32,
    }

    unsafe extern "C" {
        fn mach_absolute_time() -> u64;
        fn mach_timebase_info(info: *mut MachTimebaseInfo) -> i32;
        fn pthread_threadid_np(thread: libc::pthread_t, thread_id: *mut u64) -> libc::c_int;
    }

    static TIMEBASE_INIT: std::sync::Once = std::sync::Once::new();
    static mut TIMEBASE: MachTimebaseInfo = MachTimebaseInfo { numer: 0, denom: 0 };

    pub fn monotonic_time_ns() -> u64 {
        TIMEBASE_INIT.call_once(|| unsafe {
            mach_timebase_info(&raw mut TIMEBASE);
        });
        let ticks = unsafe { mach_absolute_time() };
        ticks * unsafe { TIMEBASE.numer } as u64 / unsafe { TIMEBASE.denom } as u64
    }

    pub fn current_thread_id() -> u64 {
        let mut tid: u64 = 0;
        unsafe { pthread_threadid_np(0 as libc::pthread_t, &mut tid) };
        tid
    }
}

// --- Linux: clock_gettime + gettid ---

#[cfg(target_os = "linux")]
mod platform {
    pub fn monotonic_time_ns() -> u64 {
        let mut ts = libc::timespec { tv_sec: 0, tv_nsec: 0 };
        unsafe { libc::clock_gettime(libc::CLOCK_MONOTONIC, &mut ts) };
        ts.tv_sec as u64 * 1_000_000_000 + ts.tv_nsec as u64
    }

    pub fn current_thread_id() -> u64 {
        unsafe { libc::syscall(libc::SYS_gettid) as u64 }
    }
}

fn monotonic_time_ns() -> u64 {
    platform::monotonic_time_ns()
}

fn current_thread_id() -> u64 {
    platform::current_thread_id()
}
