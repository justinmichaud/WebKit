mod backtrace_capture;
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
    store::capture_image_list();
    unsafe {
        pleiades_set_malloc_logger(logger::pleiades_on_malloc);
        pleiades_atexit(flush_on_exit);
        pleiades_signal(libc::SIGINT, sigint_handler);
    }
}
