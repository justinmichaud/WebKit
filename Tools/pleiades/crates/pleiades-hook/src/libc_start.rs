//! Linux activation point.
//!
//! pleiades wants to start capturing exactly when the target enters `main`,
//! skipping the dynamic loader / libc / static-constructor allocations that run
//! before it. On macOS that gate is a stack scan for `main` (resolvable via
//! `dladdr`). On Linux `main` lives in the executable's `.symtab`, not `.dynsym`,
//! so `dladdr` never finds it and the gate never opens.
//!
//! Instead we interpose glibc's `__libc_start_main` — the function the C runtime
//! calls to launch the program. Because the hook is injected via `LD_PRELOAD`,
//! our definition wins over glibc's. We swap the program's `main` for a
//! trampoline that flips pleiades into active capture and then calls the real
//! `main`, and forward everything else to the real `__libc_start_main`.

use core::ffi::{c_char, c_int, c_void};
use std::sync::atomic::{AtomicPtr, Ordering};

type MainFn = unsafe extern "C" fn(c_int, *const *const c_char, *const *const c_char) -> c_int;
type StartMainFn = unsafe extern "C" fn(
    MainFn,
    c_int,
    *const *const c_char,
    *const c_void,
    *const c_void,
    *const c_void,
    *mut c_void,
) -> c_int;

static REAL_MAIN: AtomicPtr<c_void> = AtomicPtr::new(core::ptr::null_mut());

/// Runs in place of the program's `main`: activates capture, then tail-calls the
/// real `main`. By activating here (rather than inside `__libc_start_main`) we
/// match the macOS semantics — the target's static constructors, which run before
/// `main`, are still treated as pre-main noise and skipped.
unsafe extern "C" fn main_trampoline(
    argc: c_int,
    argv: *const *const c_char,
    envp: *const *const c_char,
) -> c_int {
    crate::logger::mark_main_reached();
    let real: MainFn = unsafe { core::mem::transmute(REAL_MAIN.load(Ordering::Acquire)) };
    unsafe { real(argc, argv, envp) }
}

/// `LD_PRELOAD` interposer for glibc program startup.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn __libc_start_main(
    main: MainFn,
    argc: c_int,
    argv: *const *const c_char,
    init: *const c_void,
    fini: *const c_void,
    rtld_fini: *const c_void,
    stack_end: *mut c_void,
) -> c_int {
    REAL_MAIN.store(main as *mut c_void, Ordering::Release);

    let next = unsafe { libc::dlsym(libc::RTLD_NEXT, c"__libc_start_main".as_ptr()) };
    if next.is_null() {
        // Should never happen on glibc. Fall back to running the program directly
        // (envp follows argv: &argv[argc + 1]) rather than crashing the host.
        let envp = unsafe { argv.add(argc as usize + 1) };
        return unsafe { main_trampoline(argc, argv, envp) };
    }
    let real: StartMainFn = unsafe { core::mem::transmute(next) };
    unsafe { real(main_trampoline, argc, argv, init, fini, rtld_fini, stack_end) }
}
