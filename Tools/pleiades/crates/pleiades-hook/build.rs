fn main() {
    #[cfg(target_os = "macos")]
    {
        cc::Build::new()
            .file("pac_interpose.c")
            .compile("pac_interpose");
    }
}
