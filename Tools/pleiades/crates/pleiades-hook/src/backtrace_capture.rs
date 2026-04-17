unsafe extern "C" {
    fn backtrace(buffer: *mut *mut libc::c_void, size: libc::c_int) -> libc::c_int;
}

/// Raw backtrace capture — exposed for use by the activation check.
pub fn raw_backtrace(buffer: *mut *mut libc::c_void, size: libc::c_int) -> libc::c_int {
    unsafe { backtrace(buffer, size) }
}
