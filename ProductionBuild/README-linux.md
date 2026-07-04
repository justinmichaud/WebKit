# Production WebKit build — Linux (WebKitGTK, open source)

A maximally-optimized, self-symbolicating, **PGO + full-LTO** WebKitGTK build from the
open-source tree, built with **clang** inside the
[wkdev container](https://github.com/Igalia/webkit-container-sdk). It produces the
WebKitGTK shared libraries plus `MiniBrowser` and the `jsc` shell, and a relocatable
tarball that runs on other machines without installing a pile of `-dev` packages.

This is the Linux counterpart of [`README-macOS.md`](README-macOS.md). The *concepts* are
identical (instrument → collect a fresh profile from Speedometer 3 / JetStream 3 /
MotionMark → rebuild with full LTO + the profile); the *mechanics* differ because Linux
uses CMake + Ninja, not Xcode.

It delivers every requested property:

| Requirement | How it is achieved on Linux |
| --- | --- |
| FP-based unwinding | `-fno-omit-frame-pointer` on every object. Auto-added on aarch64 (`ARM` branch of [WebKitCompilerFlags.cmake](../Source/cmake/WebKitCompilerFlags.cmake#L174)); we also force it via `CFLAGS`/`CXXFLAGS` so x86_64 matches. Frame-pointer chain stays walkable for samplers (samply, perf) and crash reporters. |
| Debug symbols | `-g` on every object, **embedded** DWARF. We pass `-DDEBUG_FISSION=OFF` so split-DWARF is *not* used — debug info lives inside the `.so`/binaries, so it survives relocation to another machine (split-DWARF would leave it behind in `.dwo` files). |
| All optimizations | `CMAKE_BUILD_TYPE=Release` ⇒ `-O3 -DNDEBUG`. **`-DUSE_CXX_STDLIB_ASSERTIONS=OFF`** removes the libstdc++ hardening checks (`_GLIBCXX_ASSERTIONS`, on by default) that bounds/precondition-check every std container access. "Maximum performance at all costs" — no stdlib assertions, `-O3`, CPU-tuned, full LTO, PGO. (BOLT was investigated and left **off** — it regressed SP3 ~6% here; see Phase 4.) |
| CPU tuning | `-march=armv8-a+crc -mtune=neoverse-n1` (the `CPU_FLAGS` knob) — the most aggressive ISA that still runs on both this Neoverse-N1 host and a Raspberry Pi 4 (Cortex-A72 = ARMv8.0-A: crc32 yes, no crypto/LSE/dotprod/fp16), tuned for the N1. `outline-atomics` (on by default) uses LSE on the N1 and falls back to load/store-exclusive on the A72 from the same binary. Only affects AOT C++ (WebCore/DOM/layout/bmalloc); JIT'd JS already targets the host CPU at runtime. |
| PGO from SP3 / JS3 / MM | Instrument with `-fprofile-generate`, run the three benchmarks `--count 3` in MiniBrowser on a real GPU-backed Wayland display, gather `.profraw`, `llvm-profdata merge` into one `.profdata`. |
| Full LTO + the profile | Final build uses `-DLTO_MODE=full` (`-flto=full`, monolithic) and `-fprofile-use`, linked with `-Wl,--emit-relocs` (kept so the optional BOLT experiment can post-process the libs). |
| BOLT | **Off by default — investigated and dropped.** `--instrument` corrupts the JIT on aarch64 (LLVM #165664) and the only crash-free profile (no-LBR `perf` sampling) is too coarse to beat PGO+LTO — it measured ~6% *slower* on SP3. The `bolt` phase is retained (rewritten to the safe sampling path) for future use. See **Phase 4** for the full write-up. |
| Portable, few libs | The `{bin,lib}` tree already links intra-project libs via `RPATH`. The `package` phase additionally bundles the **entire external shared-library closure** into `lib/`, rewrites `RPATH=$ORIGIN`, and tarballs it — so it runs on another Linux box without the wkdev deps. |
| No system-library perf lottery | Everything performance-critical (JSC/WebCore/WebKit) is compiled *by us* at `-O3`+LTO+PGO. External deps come from the controlled wkdev container and are **bundled**, so the exact libraries the build was tuned against travel with it — no target machine can substitute a differently-compiled (slower, or debug) system library. |

> On Linux "portable / few dynamic libraries" means a **self-contained, relocatable
> tree**: the WebKit libraries + their bundled dependency closure + `MiniBrowser` + `jsc`.
> A handful of host-ABI libraries (glibc, the GPU/GL driver stack) always resolve from the
> host and cannot be bundled — see [Relocating the build](#relocating-the-build).

## TL;DR

```bash
# From inside the wkdev container, at the WebKit checkout root:
ProductionBuild/build-production-linux.sh all
```

Or run the phases individually (`instrument`, `collect`, `build`, `package`, `verify`).
Phase 2 (`collect`) is the one that needs a display; everything below is what the script
does, spelled out.

## Prerequisites

- **The wkdev container.** Build *inside* it
  (`~/Development/webkit-container-sdk`, `source register-sdk-on-host.sh`, `wkdev-enter`).
  It provides the controlled dependency set (GTK 4, GLib, Cairo, ICU, GStreamer, …) that
  WebKit links against. This recipe was validated on `wkdev` 2.53 (Ubuntu 24.04, aarch64).
- **clang + the profile runtime + `llvm-profdata`.** PGO and LTO require clang
  ([WebKitCommon.cmake](../Source/cmake/WebKitCommon.cmake#L325) rejects GCC). The stock
  container clang lacks the pieces PGO needs; install them once:
  ```bash
  sudo apt-get install -y llvm-18 libclang-rt-18-dev
  ```
  `libclang-rt-18-dev` provides `libclang_rt.profile-<arch>.a` (without it
  `-fprofile-generate` cannot even link — WebKit's `HAVE_CLANG_PROFILE_RUNTIME` check
  fatal-errors). `llvm-18` provides `llvm-profdata`. The script verifies both up front.
- **A display that actually paints, for phase 2.** SP3/MotionMark are GUI benchmarks; a
  surface that never receives frame callbacks throttles `requestAnimationFrame` and makes
  the run degenerate. The script auto-starts a **headless weston** compositor
  (`--backend=headless-backend.so`) which runs a real repaint loop and delivers frame
  callbacks (~60 Hz). If the container already forwards the host's Wayland/X display, that
  works too, but a login session with no active output does *not* deliver frame callbacks —
  weston is the reliable choice.
- **No GPU? Software GL is fine.** The script exports `LIBGL_ALWAYS_SOFTWARE=1`
  `GALLIUM_DRIVER=llvmpipe` so WebKit's accelerated compositing initialises on Mesa's
  software rasteriser (`swrast`) instead of failing on an absent hardware EGL device. This
  exercises the same code paths for PGO; only wall-clock MotionMark scores are lower.
- **`python3-twisted`, for phase 2.** `run-benchmark` serves the benchmark over a local
  twisted HTTP server: `sudo apt-get install -y python3-twisted`.
- **Network (or a pinned copy), for phase 2.** The plans clone SP3/JS3/MM from GitHub.
  Pass a local copy to a plan with `--local-copy` if the box is offline.

## Key differences from the macOS recipe

The Linux CMake PGO path (added in [bug 309318](https://bugs.webkit.org/show_bug.cgi?id=309318),
[WebKitCommon.cmake:322](../Source/cmake/WebKitCommon.cmake#L322)) is **cleaner** than
Cocoa's: **one merged `.profdata` for the whole build**, not per-framework compressed
profiles.

- **No `run-benchmark --generate-pgo-profiles`.** That flag is implemented only by the
  macOS browser drivers; the Linux GTK/WPE drivers `raise NotImplementedError`. So we do
  **not** use `collect-pgo-profiles` (its `compress`/`decompress` steps also call the
  macOS-only `/usr/bin/compression_tool`). Instead we set `LLVM_PROFILE_FILE` in the
  MiniBrowser environment ourselves and `llvm-profdata merge` the raw counters. The reusable
  cross-platform pieces are just `llvm-profdata merge`.
- **`LLVM_PROFILE_FILE` is mandatory.** JavaScriptCore and WebCore bake
  `/private/tmp/WebKitPGO/…` into `__llvm_profile_filename` even on Linux (the relevant code
  in `InitializeThreading.cpp` / `ScriptController.cpp` is *not* `PLATFORM(COCOA)`-guarded,
  only its iOS branch differs). That path does not exist on Linux, so we override it at
  runtime with `LLVM_PROFILE_FILE=<dir>/%m_%p.profraw`, which wins over the baked name.
- **Continuous mode (`%c`) does NOT work; we flush on clean exit instead.** `%c` needs
  `-mllvm -runtime-counter-relocation`, and even then produces *truncated, unmergeable*
  profiles with WebKit's many instrumented DSOs on clang-18 (`counter_bias` is per-DSO but
  the runtime assumes one). So we do not use it. LLVM's profile runtime otherwise writes only
  on a clean `exit()` (atexit). We make that happen — see the two harness requirements below.
- **Two harness requirements for the WebProcess profile** (the profile that matters most —
  JS/layout/paint run in the WebProcess, not the UI process):
  1. **Disable the sandbox:** `WEBKIT_DISABLE_SANDBOX_THIS_IS_DANGEROUS=1`. By default the
     WebProcess runs in a bubblewrap sandbox with a private `/tmp`, so its `.profraw` is
     written *inside* the sandbox and lost. (`WEBKIT_FORCE_SANDBOX=0` no longer disables it.)
  2. **Shut the browser down gracefully, not `SIGKILL`.** `run-benchmark`'s Linux driver
     `close_browsers()` originally `SIGKILL`ed the browser and all children, so no atexit ran
     and no profile was written. We patch it to `SIGTERM` the GtkApplication UI process (which
     quits cleanly, closing the Web/Network IPC connections so those exit cleanly too and
     flush), wait, then force-kill stragglers. **This is the one required, upstreamable harness
     change** ([linux_browser_driver.py](../Tools/Scripts/webkitpy/benchmark_runner/browser_driver/linux_browser_driver.py) —
     the Linux analog of the macOS `pgo_profile_output_directories` change). Re-apply it on a
     clean checkout.
- **`build-webkit --lto-mode` is a no-op for GTK.** It is consumed only on the Xcode path.
  For CMake, LTO comes from `-DLTO_MODE=full|thin` (or `set-webkit-configuration
  --lto-mode`). Note `LTO_MODE=none` is a *bug trap*: it is a truthy string that yields the
  invalid clang flag `-flto=none`; to disable LTO you must omit the variable entirely.
- **Developer mode OFF (for perf), plus a one-line engine patch so the build stays
  relocatable.** Two coupled facts:
  - Developer mode's only real *runtime* cost is that it turned on the stdlib assertions — but
    those are the independent `USE_CXX_STDLIB_ASSERTIONS` option (see the table), which we
    disable directly. Its 56 `ENABLE(DEVELOPER_MODE)` code sites are all cold (accessibility,
    filesystem, network setup), so with assertions off, dev-mode-off buys little *runtime* but
    is the cleaner production posture and skips building the test binaries.
  - The catch: two dev-mode-gated pieces of helper-process handling are needed to run from a
    relocated tree, so **two engine patches** lift the `#if ENABLE(DEVELOPER_MODE)` gate (both
    upstreamable):
    - [ProcessExecutablePathGLib.cpp](../Source/WebKit/Shared/glib/ProcessExecutablePathGLib.cpp)
      — *finds* `WebKitWebProcess`/`WebKitNetworkProcess` via `WEBKIT_EXEC_PATH` /
      next-to-the-executable. Without it, dev-mode-off hard-codes the installed
      `/usr/local/libexec` and MiniBrowser aborts.
    - [BubblewrapLauncher.cpp](../Source/WebKit/UIProcess/Launcher/glib/BubblewrapLauncher.cpp)
      — *bind-mounts* that tree into the bubblewrap **sandbox** namespace. Without it, with the
      sandbox on (the default), `bwrap` can't exec the WebProcess from a relocated path
      (`bwrap: execvp .../WebKitWebProcess: No such file or directory`). This keeps the sandbox
      working in the portable bundle rather than forcing it off.
  - **`-DENABLE_EXPERIMENTAL_FEATURES=OFF`** selects the CI-tested stable feature set. Not just
    tidiness: in this container/checkout two experimental features fail to link — GStreamer
    WebRTC (`gst_webrtc_error_quark()` undefined, a C/C++ linkage mismatch against the installed
    `libgstwebrtc`) and WebExtensions (`WebKit::toJSError` 2-arg overload undefined on GTK).
    Both are `PRIVATE ${ENABLE_EXPERIMENTAL_FEATURES}`. SP3/JS3/MM exercise core JS/layout/paint,
    so the profile stays fully representative.
  - `-DENABLE_THUNDER=OFF` (OpenCDM/Thunder DRM backend not installed / not needed). Frame
    pointers stay on via the `ARM` branch plus our explicit `-fno-omit-frame-pointer`.

## The pipeline

### Phase 1 — instrumented build

```bash
CFLAGS="-g -fno-omit-frame-pointer -march=armv8-a+crc -mtune=neoverse-n1" \
CXXFLAGS="$CFLAGS" \
Tools/Scripts/build-webkit --gtk --release \
  --cmakeargs="-DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
    -DDEVELOPER_MODE=OFF -DENABLE_EXPERIMENTAL_FEATURES=OFF -DUSE_CXX_STDLIB_ASSERTIONS=OFF \
    -DDEBUG_FISSION=OFF -DENABLE_THUNDER=OFF \
    -DENABLE_LLVM_PROFILE_GENERATION=ON -DPGO_PROFILE_DIR=<dir>"
```

`-O3` comes from `Release`; `-g`, frame pointers and the CPU flags come from `CFLAGS`.
(Flags with spaces go through `CFLAGS`/`CXXFLAGS`, **not** `--cmakeargs`: `build-webkit`
interpolates cmakeargs into a single shell string, so a `-Dfoo="a b c"` value would be
re-split by the shell. Only single-token `-D…` go in `--cmakeargs`.)

### Phase 2 — collect the profile

The collect phase uses a real host Wayland display (`wayland-0`) if one is present (real GPU,
real frame callbacks), else falls back to a headless weston + software GL. Then, per benchmark:

```bash
LLVM_PROFILE_FILE="<dir>/<plan>_%m_%p.profraw" \
WAYLAND_DISPLAY=wayland-0 GDK_BACKEND=wayland \
WEBKIT_DISABLE_SANDBOX_THIS_IS_DANGEROUS=1 \
Tools/Scripts/run-benchmark --plan <speedometer3|jetstream3|motionmark1.3.1> \
  --browser minibrowser-gtk --build-directory WebKitBuild/GTK/Release --count 3
```

(The patched driver closes the browser gracefully so each process flushes its profile.)
Then merge:

```bash
llvm-profdata merge -o WebKitBuild/PGO/webkit-<arch>.profdata WebKitBuild/PGO/profraw/*.profraw
```

Even if a benchmark's result-reporting is noisy, the instrumented code still *ran* and flushed
its counters on the graceful exit — the profile is representative regardless.

### Phase 3 — final production build (full LTO + profile-use)

```bash
rm -rf WebKitBuild/GTK/Release        # flags change: clean rebuild

CFLAGS="-g -fno-omit-frame-pointer -march=armv8-a+crc -mtune=neoverse-n1 -Wl,--emit-relocs" \
CXXFLAGS="$CFLAGS" \
Tools/Scripts/build-webkit --gtk --release \
  --cmakeargs="-DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
    -DDEVELOPER_MODE=OFF -DENABLE_EXPERIMENTAL_FEATURES=OFF -DUSE_CXX_STDLIB_ASSERTIONS=OFF \
    -DDEBUG_FISSION=OFF -DENABLE_THUNDER=OFF -DLTO_MODE=full \
    -DUSE_PGO_PROFILE=ON -DPGO_PROFILE_PATH=WebKitBuild/PGO/webkit-<arch>.profdata"
```

`USE_PGO_PROFILE` adds `-fprofile-use=<profdata> -Wno-error=backend-plugin`; `LTO_MODE=full`
adds `-flto=full` to compile and link; `-Wl,--emit-relocs` keeps relocations so BOLT can
rewrite the libs. (Full LTO of libwebkitgtk is the long pole — largely serial and RAM-hungry
(~30 GB); drop to `LTO_MODE=thin` if memory-constrained.)

### Phase 4 — BOLT (EXPERIMENTAL, **off by default** — it regressed SP3 ~6% here)

**Bottom line:** BOLT is not part of the shipped build. It is not in `all`. On this platform
(aarch64, `bolt-19`/LLVM 19) BOLT could not beat the compiler's PGO+LTO layout, and the only
profiling mode that doesn't crash regressed Speedometer 3 by ~6%. The `bolt` phase is kept,
rewritten to the safe sampling path, only so the recipe is ready if BOLT gains working aarch64
branch profiling later. Full investigation:

**1. `--instrument` mode corrupts the JIT (LLVM issue #165664).** BOLT-instrumenting
`libjavascriptcoregtk` and running any real JS makes a DFG/FTL JIT **worker thread** crash the
instant code tiers up — nondeterministically, with memory-corruption signatures (`WTF::CrashOnOverflow`
= a bogus `Vector` size one run; libpas `pas panic: … Large heap did not find object` = a bad free
the next). Trivial interpreter-only code (`2+2`, which never leaves the LLInt) is fine; anything
that reaches the DFG dies. Root cause is upstream **[#165664](https://github.com/llvm/llvm-project/issues/165664)**:
BOLT's aarch64 instrumentation writes each counter via `mrs x16, tpidr_el0; str w17,[x16,x0]`
with a `movk`-built offset that is mis-computed, so counter stores land outside their region and
scribble the heap. It's the *instrumentation runtime* that's broken, not the JIT — BOLT-*optimize*
(static rewrite, `-data=…`) of the very same lib is correct and passes tier-up. `--instrument-calls=false`
lowers the crash rate (jsc micro-loops survive) but a real Speedometer 3 / MotionMark WebProcess
still crashes within ~5 s. So instrumentation-mode collection is unusable for the browser here.

**2. No branch-accurate sampling either.** aarch64 has no LBR; this box has no ARM SPE; and BOLT's
SPE support needs LLVM ≥ 21 + perf ≥ 6.14 anyway (we have `bolt-19` + perf 6.8). That leaves plain
no-LBR `perf` sampling — crash-free (it never instruments), and it *works*: `perf record -e cycles:u
--inherit` around the benchmark captures the WebProcess, and `perf2bolt -nl` turns it into a profile.
Coverage on SP3 is decent (~48 % of samples land in the two target libs; ~1/3 is invisible JIT code).

**3. …but the coarse profile makes BOLT *hurt*.** Re-laying-out the already-PGO+LTO'd libs from a
no-LBR profile (no branch counts → reordering is largely guesswork) measured, on Speedometer 3 vs
the clean PGO+LTO build (headless weston, software GL, count=6, Welch t-test):

| BOLT config (sampling profile)                                   | SP3 vs PGO+LTO |
|------------------------------------------------------------------|----------------|
| `ext-tsp` blocks + `cdsort` funcs + split + ICF (full)           | **−6.1 %** (t=−19.7) |
| hot/cold split only, no reordering                               | **−6.4 %** (t=−15.1) |

Even the most conservative transform regressed. The compiler's PGO+LTO layout is already good;
BOLT with strictly worse (no-branch) data degrades it. So BOLT stays off until branch-accurate
aarch64 profiling (SPE) is available.

**What the `bolt` phase does now (if you run it to experiment):** finds a working `perf`, samples a
clean Speedometer 3 run (`perf record --inherit`, no instrumentation → no crash), `perf2bolt -nl`
per hot lib, then `llvm-bolt … -reorder-blocks=ext-tsp -reorder-functions=cdsort -split-functions
-icf=1 -skip-funcs='llint_.*,vmEntry.*,…' -update-debug-sections`. The `-skip-funcs` excludes JSC's
hand-written-assembly LLInt/vmEntry/IPInt families (one giant shared CFI/FDE BOLT can't split — the
`~2389 "in conflict with FDE … Skipping"` messages; harmless, and the V8 `-skip-funcs=Builtins_.*`
analogue). It's best-effort per lib and leaves the PGO+LTO version in place on any failure. Afterwards
re-run `build` (or `package` from a fresh `build`) to restore the clean PGO+LTO libs. Needs Phase 3's
`--emit-relocs`. `llvm-bolt`/`perf2bolt` come from `bolt-19` (BOLT works at the ELF level, so a newer
BOLT processes the clang-18 binary).

### Phase 5 — package (portability)

Copies `{bin,lib}` (the PGO+LTO libs; BOLT is off), walks the `ldd` closure of every ELF, copies
each non-host external `.so` into `lib/`, rewrites every ELF's `RPATH` to a multi-entry
`$ORIGIN` list (covers `bin/`, `lib/`, and `lib/webkit2gtk-6.0/`), bundles GdkPixbuf loaders +
GIO modules + GSettings schemas with generated caches, writes `run-jsc.sh` /
`run-minibrowser.sh` launchers, and tarballs the tree.

## Verifying the build

```bash
ProductionBuild/build-production-linux.sh verify
```

checks: frame-pointer prologues in `jsc`, a `.debug_info` section in the libraries,
`USE_PGO_PROFILE`/`LTO_MODE` in `CMakeCache.txt`, and a `jsc` smoke test. Manual checks:

```bash
# PGO + LTO actually on the compile lines:
grep -o -- '-fprofile-use=[^ ]*' WebKitBuild/GTK/Release/build.ninja | head -1
grep -o -- '-flto=full'          WebKitBuild/GTK/Release/build.ninja | head -1

# Frame pointers (aarch64 prologues push x29/x30):
objdump -d WebKitBuild/GTK/Release/bin/jsc | grep -m2 'stp.*x29, x30'

# Embedded (not split) debug info:
readelf -S WebKitBuild/GTK/Release/lib/libjavascriptcoregtk-6.0.so | grep debug_info
```

## Relocating the build

The `package` phase produces `WebKitBuild/Production-GTK-<arch>[.tar.gz]`, a self-contained
tree:

```bash
# jsc on another machine (no WebKit deps installed):
/path/to/Production-GTK-<arch>/run-jsc.sh script.js

# MiniBrowser (needs a Wayland or X display on the target):
/path/to/Production-GTK-<arch>/run-minibrowser.sh https://example.com
```

Notes:
- The bundled `lib/` carries the full dependency closure (GTK, GLib, Cairo, ICU, GStreamer,
  …) so the target does **not** need the wkdev `-dev` packages. Debug symbols are embedded
  in each `.so`, so profiling/symbolication works on the target too.
- What still resolves from the host and is **not** bundled: glibc / the dynamic loader
  (ABI-tied to the target kernel) and the GPU/GL driver stack (`libGL`, `libEGL`, Mesa DRI /
  vendor drivers). Any normal Linux desktop has these. Fonts/`fontconfig` config also come
  from the host.
- Match or predate the target's glibc: a tree built against a newer glibc will not start on
  an older one. Build on the oldest OS you must support (or in a matching container).
