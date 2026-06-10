// C helpers for arm64e PAC: sign unsigned function pointers from Rust
// before passing them to C APIs or storing in global variables.
//
// PAC signing only applies to Apple arm64 (where the C ABI expects key-IA,
// discriminator-0 signed pointers). On Linux/x86_64 these instructions either
// don't exist or aren't part of the ABI, so the helper is an identity
// pass-through there — the shims still need to be compiled so the cdylib's
// symbols resolve at LD_PRELOAD time.

#include <signal.h>
#include <stdlib.h>
#include <stdint.h>

static uintptr_t pac_sign_if_needed(uintptr_t ptr) {
#if defined(__aarch64__) && defined(__APPLE__)
    uintptr_t stripped;
    __asm__ volatile("mov %0, %1\n\txpaci %0" : "=r"(stripped) : "r"(ptr));
    if (stripped != ptr) return ptr;
    __asm__ volatile("paciza %0" : "+r"(ptr));
#endif
    return ptr;
}

// Weak: the host allocator (WebKit/bmalloc, Apple libmalloc) provides this. The
// hook is LD_PRELOAD'd into EVERY process in a launch chain — including ones that
// don't link such an allocator (/usr/bin/env, the run-minibrowser script's
// interpreter, bash). A strong undefined reference makes the dynamic loader abort
// those processes ("undefined symbol: malloc_logger"). Weak lets them load; the
// symbol simply resolves to NULL (i.e. &malloc_logger == 0) where it's absent.
extern void (*malloc_logger)(uint32_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t) __attribute__((weak));

// True only in processes that actually provide malloc_logger. pleiades checks
// this before doing any setup so launcher processes are left untouched.
int pleiades_has_malloc_logger(void) {
    return &malloc_logger != 0;
}

void pleiades_set_malloc_logger(void (*fn)(uint32_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t)) {
    if (&malloc_logger == 0)
        return;
    fn = (void (*)(uint32_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t))
        pac_sign_if_needed((uintptr_t)fn);
    malloc_logger = fn;
}

int pleiades_atexit(void (*callback)(void)) {
    callback = (void (*)(void))pac_sign_if_needed((uintptr_t)callback);
    return atexit(callback);
}

void (*pleiades_signal(int signum, void (*handler)(int)))(int) {
    if (handler != SIG_DFL && handler != SIG_IGN && handler != SIG_ERR)
        handler = (void (*)(int))pac_sign_if_needed((uintptr_t)handler);
    return signal(signum, handler);
}
