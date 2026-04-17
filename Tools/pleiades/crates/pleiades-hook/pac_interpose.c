// C helpers for arm64e PAC: sign unsigned function pointers from Rust
// before passing them to C APIs or storing in global variables.
//
// On arm64 (non-PAC), paciza/xpaci are NOPs — harmless pass-through.

#include <signal.h>
#include <stdlib.h>
#include <stdint.h>

static uintptr_t pac_sign_if_needed(uintptr_t ptr) {
    uintptr_t stripped;
    __asm__ volatile("mov %0, %1\n\txpaci %0" : "=r"(stripped) : "r"(ptr));
    if (stripped != ptr) return ptr;
    __asm__ volatile("paciza %0" : "+r"(ptr));
    return ptr;
}

extern void (*malloc_logger)(uint32_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t);

void pleiades_set_malloc_logger(void (*fn)(uint32_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t)) {
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
