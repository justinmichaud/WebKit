mod backtrace_capture;
#[cfg(target_os = "linux")]
mod libc_start;
mod logger;
mod reentrancy;
mod store;

unsafe extern "C" {
    /// C helpers that PAC-sign function pointer arguments before passing
    /// to the real C APIs. Defined in pac_interpose.c, compiled via build.rs.
    fn pleiades_set_malloc_logger(
        fn_ptr: unsafe extern "C" fn(u32, usize, usize, usize, usize),
    );
    fn pleiades_atexit(callback: extern "C" fn()) -> libc::c_int;
    fn pleiades_signal(
        signum: libc::c_int,
        handler: unsafe extern "C" fn(libc::c_int),
    ) -> *mut libc::c_void;
    /// Nonzero only in processes whose allocator provides `malloc_logger`
    /// (weak symbol). See pac_interpose.c.
    fn pleiades_has_malloc_logger() -> libc::c_int;
}

extern "C" fn flush_on_exit() {
    store::flush_to_disk();
}

unsafe extern "C" fn sigint_handler(_sig: libc::c_int) {
    store::flush_to_disk();
    unsafe { libc::_exit(128 + libc::SIGINT); }
}

#[ctor::ctor]
fn init() {
    // The hook is LD_PRELOAD'd into every process in the launch chain. Only
    // activate where the allocator actually provides malloc_logger (WebKit/
    // bmalloc); in launcher processes (/usr/bin/env, perl, bash, …) the weak
    // symbol is absent, so do nothing — no hooks, no output files.
    if unsafe { pleiades_has_malloc_logger() } == 0 {
        return;
    }
    store::capture_image_list();
    unsafe {
        pleiades_set_malloc_logger(logger::pleiades_on_malloc);
        pleiades_atexit(flush_on_exit);
        pleiades_signal(libc::SIGINT, sigint_handler);
    }
}
