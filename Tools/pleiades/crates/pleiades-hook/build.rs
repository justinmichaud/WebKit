fn main() {
    // pac_interpose.c provides the pleiades_{atexit,signal,set_malloc_logger}
    // shims the cdylib links against. It must be compiled on every platform
    // (not just macOS) or those symbols stay undefined and LD_PRELOAD/dlopen
    // fails to load the library. The PAC signing inside is gated to Apple arm64.
    println!("cargo:rerun-if-changed=pac_interpose.c");
    cc::Build::new()
        .file("pac_interpose.c")
        .compile("pac_interpose");
}
