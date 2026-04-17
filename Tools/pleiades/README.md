# pleiades

Record your memory (allocations) via pleiades!

Memory allocation tracing tool. Injects a dylib/shared library into a target process to hook `malloc_logger`, capturing every allocation event with backtraces. Data is stored in a compact binary format with deduplicated stack traces, then exported to Firefox Profiler JSON for visualization via `samply`.

## Quick Start

```bash
# Build
make build

# Capture allocations (macOS)
DYLD_INSERT_LIBRARIES=./target/libpleiades_hook.dylib ./my_target

# Capture allocations (Linux)
LD_PRELOAD=./target/release/libpleiades_hook.so ./my_target

# View stats
./target/release/pleiades-cli stats

# Dump with symbolication
./target/release/pleiades-cli dump --limit 20

# Export to Firefox Profiler and view
./target/release/pleiades-cli export -o /tmp/profile.json
samply load /tmp/profile.json
```

## Architecture

```
pleiades-hook (cdylib)          pleiades-cli
┌─────────────────────┐          ┌──────────────────────┐
│ malloc_logger hook   │          │ stats   - summary    │
│ reentrancy guard     │  ──────> │ dump    - symbolicate│
│ stack prefix tree    │ bin file │ export  - profiler   │
│ at-exit flush        │          │           JSON       │
└─────────────────────┘          └──────────────────────┘
                                          │
                                          ▼
                                   samply load → Firefox Profiler
```

- **pleiades-hook** — `cdylib` injected via `DYLD_INSERT_LIBRARIES` (macOS) or `LD_PRELOAD` (Linux). Hooks `malloc_logger` to capture allocation events with raw backtraces. Stacks are deduplicated in memory using a prefix tree. Data is flushed to a binary file at process exit via `atexit`.
- **pleiades-core** — Shared types for the binary format.
- **pleiades-cli** — CLI tool for analysis:
  - `stats` — allocation counts and total bytes
  - `dump` — per-event dump with offline symbolication (addr2line + Mach-O/ELF symbol tables)
  - `export` — generates Firefox Profiler JSON with `nativeAllocations` table for `samply load`

## Platform Support

| Platform | Hook mechanism | Library injection | Build |
|----------|---------------|-------------------|-------|
| macOS arm64 | `malloc_logger` pointer | `DYLD_INSERT_LIBRARIES` | `make build` (standard target) |
| macOS arm64e | `malloc_logger` with PAC signing | `DYLD_INSERT_LIBRARIES` | `make build` (custom target + `-Z build-std`) |
| Linux x86_64/aarch64 | `malloc_logger` pointer | `LD_PRELOAD` | `make build` |

### macOS ARM64E Notes

ARM64E processes (like WebKit/JSC) use Pointer Authentication Codes (PAC). Two workarounds are applied automatically:

1. **Function pointer PAC**: The `malloc_logger` pointer is signed with `paciza` (key IA, discriminator 0) via inline assembly, matching what the C ABI expects.
2. **TLS workaround**: A custom target (`arm64e-apple-darwin-notlv.json`) sets `has-thread-local: false`, forcing `thread_local!` to use POSIX pthread keys instead of broken Mach-O TLV on arm64e.

The Makefile builds both arm64 and arm64e slices and combines them into a universal fat binary via `lipo`.

## Prerequisites

Nightly Rust and `rust-src` are configured in `rust-toolchain.toml` — `rustup` installs them automatically.

## Environment Variables

| Variable | Default | Description |
|----------|---------|-------------|
| `PLEIADES_DB_PATH` | `/tmp/pleiades-alloc.bin` | Output path for the binary log file |

## Example: Profiling WebKit's JSC

```bash
# Build
make build

# Capture
VM=~/dev/OpenSource/WebKitBuild/Release
DYLD_INSERT_LIBRARIES=./target/libpleiades_hook.dylib \
    DYLD_FRAMEWORK_PATH=$VM $VM/jsc -e "print('hello')"

# Analyze
./target/release/pleiades-cli stats
./target/release/pleiades-cli dump --limit 10

# View in Firefox Profiler
./target/release/pleiades-cli export -o /tmp/profile.json
samply load /tmp/profile.json
```

## Binary Log Format

The hook writes a compact binary file at process exit:

```
[4 bytes]  Magic: 0x424D4B32 ("PLD2")
[4 bytes]  Image list length (bincode)
[N bytes]  Vec<ImageInfo>: path + load_address + build_id per loaded library
[4 bytes]  Stack tree length (bincode)
[N bytes]  Vec<StackNode>: deduplicated stack prefix tree
[4 bytes]  Events length (bincode)
[N bytes]  Vec<CompactEvent>: timestamp, type, args, stack_id, tid, pid
```

Stacks are stored as a linked-list prefix tree — common call stack prefixes are shared, reducing a typical 11M-event capture from 3.6GB to ~200KB.
