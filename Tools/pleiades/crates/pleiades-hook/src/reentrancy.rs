use std::ffi::c_void;

static mut TLS_KEY: libc::pthread_key_t = 0;
static TLS_INIT: std::sync::Once = std::sync::Once::new();

fn tls_key() -> libc::pthread_key_t {
    TLS_INIT.call_once(|| unsafe {
        libc::pthread_key_create(&raw mut TLS_KEY, None);
    });
    unsafe { TLS_KEY }
}

/// RAII reentrancy guard using raw pthread TLS (allocation-free).
///
/// Returns `None` from `try_enter()` if the current thread is already
/// inside the malloc_logger callback, preventing infinite recursion.
pub struct ReentrancyGuard(());

impl ReentrancyGuard {
    #[inline]
    pub fn try_enter() -> Option<Self> {
        let key = tls_key();
        unsafe {
            if !libc::pthread_getspecific(key).is_null() {
                return None;
            }
            libc::pthread_setspecific(key, 1usize as *const c_void);
            Some(ReentrancyGuard(()))
        }
    }
}

impl Drop for ReentrancyGuard {
    #[inline]
    fn drop(&mut self) {
        let key = tls_key();
        unsafe {
            libc::pthread_setspecific(key, std::ptr::null());
        }
    }
}
