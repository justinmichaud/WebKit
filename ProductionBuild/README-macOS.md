# Production WebKit build — macOS (Apple Cocoa port, open source)

A recipe for a maximally-optimized, self-symbolicating, PGO+LTO WebKit build from
the **open-source tree only** (no Apple-internal SDK, no WebKitAdditions). It
produces the Cocoa frameworks plus `MiniBrowser.app` and the `jsc` shell.

It delivers every requested property:

| Requirement | How it is achieved |
| --- | --- |
| FP-based unwinding | `-fno-omit-frame-pointer` on every framework (frame-pointer chain stays walkable for samplers/crash reporters). Mandatory on arm64 by ABI anyway; this makes x86_64 match. |
| Debug symbols | Release already builds `DEBUG_INFORMATION_FORMAT = dwarf-with-dsym` with `GCC_GENERATE_DEBUGGING_SYMBOLS = YES`; `.dSYM` bundles land beside each framework and the in-tree binaries are not stripped. |
| All optimizations | `-O3` forced across all frameworks (WebKit's own defaults are JSC `-O3`, WebCore `-O2`, WebKit `-Os`). Set `FORCE_O3=0` to keep the tuned defaults. |
| PGO from SP3 / JS3 / MM | Instrument, run the three benchmarks in MiniBrowser, merge and weight the profiles (SP3 0.6 / JS3 0.2 / MM 0.2). |
| Full LTO + the profile | Final build uses `--lto-mode=full` and `-fprofile-use`. |
| Portable, few dylibs | Relocatable build tree: the frameworks + `MiniBrowser.app` + `jsc` run on any compatible Mac via `DYLD_FRAMEWORK_PATH`. See [Relocating the build](#relocating-the-build). |

> The macOS Cocoa port always ships as separate `.framework`s linked against many
> system frameworks; those system dependencies cannot be removed. "Portable / few
> dynamic libraries" here means a **self-contained, relocatable WebKitBuild tree**,
> not a single static binary. For a static-leaning build, that is the Linux/CMake
> port — see [Appendix: Linux](#appendix-pointers-for-the-linux-cmake-build).

## TL;DR

```bash
# One-time: make MiniBrowser collect PGO profiles (already applied in this tree;
# see "The one required harness change" below if starting from a clean checkout).

ProductionBuild/build-production-macos.sh all
```

Or run the phases individually (`instrument`, `collect`, `build`) — useful because
phase 2 needs you at the machine with a display. Everything below is what that
script runs, spelled out.

## Prerequisites

- **Xcode + command-line tools.** The build uses `xcodebuild` via the top-level `make`.
- **Metal Toolchain.** Xcode 16+/26 ships the Metal compiler as a separate component;
  without it the ANGLE shader build fails with `cannot execute tool 'metal' due to
  missing Metal Toolchain`. Install once with `xcodebuild -downloadComponent MetalToolchain`
  (check with `xcodebuild -showComponent MetalToolchain`).
- **An awake, attached, unlocked display for phase 2.** Speedometer 3 and MotionMark are
  GUI benchmarks driven through MiniBrowser. The driver raw-execs MiniBrowser (to inject
  DYLD_FRAMEWORK_PATH) and never activates it, so the script runs a background raiser that
  keeps MiniBrowser frontmost; this prevents `requestAnimationFrame` throttling, which
  matters most for MotionMark, and mirrors `wk-tools/quiesce.sh`. A locked or asleep screen
  still throttles rAF, so keep the machine awake. Run `quiesce.sh` alongside for lower
  benchmark noise and to pin the JetStream 3 checkout.
- **Benign log noise during phase 2** (neither affects the profile, which is collected from
  `/private/tmp/WebKitPGO`): repeated `CONSOLE ERROR NotAllowedError, Permission was denied`
  from Speedometer's `benchmark-runner.mjs` (MiniBrowser does not grant the Screen Wake Lock
  API, even when frontmost, and Speedometer catches the rejection), and `POST /report`
  returning HTTP 500 in `run-benchmark-http.log` (`twisted_http_server.py` returns a `str`
  where newer Twisted wants `bytes`). A single-iteration headless Speedometer run finishing
  in ~30s (with a real score, e.g. `Speedometer-3:Score: 33pt`) is expected, not degenerate.
- **Network access for phase 2** (or a pinned local copy). The benchmark plans
  clone SP3/JS3/MM from GitHub into a temp dir and serve them over a local twisted
  HTTP server. Pass `JS3_LOCAL_COPY=<path>` to pin JetStream3 (see `quiesce.sh`,
  which prints `JS3_LOCAL_COPY=`).
- **~single native arch.** This recipe targets your machine's arch (`arm64` or
  `x86_64`); universal (fat) builds are out of scope.

## What is already automated

You are mostly wiring together scripts that already exist:

- `Tools/Scripts/collect-pgo-profiles` — runs benchmarks with `--generate-pgo-profiles`,
  merges per-dylib `.profraw`, combines them with weights, compresses the result.
- `Tools/Scripts/pgo-profile` — the `merge`/`combine`/`compress` primitives it calls
  (`combine` holds the SP3 0.6 / JS3 0.2 / MM 0.2 weights).
- `Tools/Scripts/run-benchmark` — `--plan {speedometer3,jetstream3,motionmark}`,
  `--generate-pgo-profiles`, `--build-directory`, `--browser minibrowser`.
- `Tools/Scripts/build-and-collect-pgo-profiles` — Apple's internal end-to-end driver.
  We do **not** use it: it `cd`s into an `Internal/` checkout and relies on
  WebKitAdditions to auto-consume the profile. Our `build-production-macos.sh` is the
  open-source equivalent.
- The Cocoa PGO plumbing in the `.xcconfig`s + `Source/bmalloc/Scripts/copy-profiling-data.py`.

### The one required harness change

`run-benchmark --generate-pgo-profiles` only knew where Safari writes its profiles.
The instrumented frameworks bake the output path `/private/tmp/WebKitPGO` into
`__llvm_profile_filename` ([JavaScriptCore/runtime/InitializeThreading.cpp](../Source/JavaScriptCore/runtime/InitializeThreading.cpp),
[WebCore/bindings/js/ScriptController.cpp](../Source/WebCore/bindings/js/ScriptController.cpp),
[WebKit/Shared/Cocoa/WebKit2InitializeCocoa.mm](../Source/WebKit/Shared/Cocoa/WebKit2InitializeCocoa.mm)),
and the sandbox profiles allow writing there — so it is the same for **any** host
browser. The MiniBrowser driver was just missing the property that tells the harness
to look there. This tree already has the fix
([osx_minibrowser_driver.py](../Tools/Scripts/webkitpy/benchmark_runner/browser_driver/osx_minibrowser_driver.py)):

```python
    @property
    def pgo_profile_output_directories(self):
        return ['/private/tmp/WebKitPGO']
```

If you start from a clean checkout, re-apply that (it is upstreamable).

## The pipeline

### Phase 1 — instrumented build

```bash
make release ASAN=NO WK_LTO_MODE=thin ENABLE_LLVM_PROFILE_GENERATION=ON \
    OTHER_LDFLAGS="-fprofile-generate"
```

`ENABLE_LLVM_PROFILE_GENERATION=ON` adds `-fprofile-generate` to every framework;
at runtime each writes counters to `/private/tmp/WebKitPGO`. Thin LTO keeps this
throwaway build reasonably fast. The default `make release` scheme
("Everything up to WebKit + Tools") builds the frameworks **and** `MiniBrowser.app`.

`OTHER_LDFLAGS="-fprofile-generate"` is required: the per-framework xcconfigs link
the clang profile runtime, but tool targets that statically link the instrumented
`libWTF.a`/`libbmalloc.a` (e.g. `lldbWebKitTester`, `TestWTF`) do not. Without it
those tools fail to link with undefined `___llvm_profile_instrument_*` symbols, and
because they build early and fail fast, the whole build aborts before the frameworks
are relinked.

### Phase 2 — collect the profile

Quiesce first (recommended) and get a display up:

```bash
# In wk-tools; needs sudo for some steps. Prints JS3_LOCAL_COPY on the last line.
./quiesce.sh on
```

Then:

```bash
Tools/Scripts/collect-pgo-profiles \
    --benchmarks speedometer3 jetstream3 motionmark \
    --output-directory "$PWD/WebKitBuild/PGO" \
    --compressed-profile-sub-path "$(uname -m)" \
    --build-directory "$PWD/WebKitBuild/Release" \
    --browser minibrowser
    # optionally: --local-copy "$JS3_LOCAL_COPY"
```

This runs each benchmark once (`--count 1`) in the freshly built MiniBrowser,
copies the `.profraw` out of `/private/tmp/WebKitPGO`, merges them into
`JavaScriptCore.profdata` / `WebCore.profdata` / `WebKit.profdata`, combines the
three benchmark runs with the standard weights, and writes the compressed result to:

```
WebKitBuild/PGO/<arch>/{JavaScriptCore,WebCore,WebKit}.profdata.compressed
```

The `--compressed-profile-sub-path "$(uname -m)"` matters: the final build's
decompression step looks for `<folder>/<arch>/<lib>.profdata.compressed`.

Run `./quiesce.sh off` when done.

### Phase 3 — final production build

```bash
rm -rf WebKitBuild/Release WebKitBuild/Release.build      # clean: flags change

make release ASAN=NO WK_LTO_MODE=full \
    ARGS="WK_ENABLE_PGO_USE=YES \
          WK_COMPRESSED_OPTIMIZATION_PROFILE_FOLDER=$PWD/WebKitBuild/PGO \
          WK_DEFAULT_GCC_OPTIMIZATION_LEVEL=3 \
          ENABLE_USER_SCRIPT_SANDBOXING=NO \
          DEBUG_INFORMATION_FORMAT=dwarf-with-dsym" \
    EXTRA_CFLAGS="-fno-omit-frame-pointer"
```

What each piece does:

- `WK_LTO_MODE=full` → `set-webkit-configuration --lto-mode=full`, i.e. `-flto`
  (monolithic) across the frameworks.
- `WK_ENABLE_PGO_USE=YES` forces on the profile-use path that is otherwise gated to
  `USE_INTERNAL_SDK=YES` ([Source/WebKit/Configurations/BaseTarget.xcconfig](../Source/WebKit/Configurations/BaseTarget.xcconfig),
  and the per-framework `libJavaScriptCore.xcconfig` / `WebCore.xcconfig`). It adds
  `-fprofile-use=<lib>.profdata -Wno-error=backend-plugin`.
- `WK_COMPRESSED_OPTIMIZATION_PROFILE_FOLDER=…/PGO` points the existing
  "Copy Profiling Data" build phase ([copy-profiling-data.py](../Source/bmalloc/Scripts/copy-profiling-data.py))
  at our profiles. It decompresses `<arch>/<lib>.profdata.compressed` into each
  framework's `DerivedSources/<framework>/Profiling/<arch>.profdata`, which is exactly
  what `-fprofile-use` reads.
- `WK_DEFAULT_GCC_OPTIMIZATION_LEVEL=3` forces `-O3` everywhere (drop it, or set
  `FORCE_O3=0` in the script, to keep the per-target defaults; with PGO the wins from
  `-O3` on cold code are usually marginal, which is why Apple ships WebKit at `-Os`).
- `EXTRA_CFLAGS="-fno-omit-frame-pointer"` keeps frame pointers for unwinding.
- `ENABLE_USER_SCRIPT_SANDBOXING=NO` lets the "Copy Profiling Data" phase read our
  `<arch>/*.profdata.compressed`. That phase runs under Xcode's script sandbox for the
  bmalloc/WTF/JavaScriptCore targets, which grants only its declared inputs (the
  `arm64e`/`x86_64` production arches) and denies our actual build arch's file; WebCore
  and WebKit already exclude the phase from the sandbox, and this extends that to the rest.
- `DEBUG_INFORMATION_FORMAT=dwarf-with-dsym` makes the **frameworks** emit relocatable
  `*.framework.dSYM` bundles. WebKit's engineering Release config uses plain `dwarf` for
  the frameworks (debug info stays in the `WebKitBuild/*.build/**/*.o` files: debuggable
  in place, but lost if you move the build); only apps/tools default to dSYMs. Forcing this
  is what makes "debug symbols" portable alongside the relocatable build tree.

## Verifying the build

```bash
# PGO was actually applied (per framework):
find WebKitBuild/Release/DerivedSources -name '*.profdata' -path '*Profiling*'

# Frame pointers preserved (x86_64: no `mov %rsp,%rbp`-less prologues; arm64 always keeps them):
otool -tv WebKitBuild/Release/JavaScriptCore.framework/JavaScriptCore | head

# dSYMs produced:
ls -d WebKitBuild/Release/*.framework.dSYM WebKitBuild/Release/*.dSYM 2>/dev/null

# LTO: link step shows -flto in the build transcript; run `make release … VERBOSITY=noisy`
# and grep the JavaScriptCore/WebCore/WebKit link lines for `-flto`.
```

To sanity-check that `-fprofile-use` reached the compiler, build one phase with
`VERBOSITY=noisy` and grep the transcript for `-fprofile-use`.

## Relocating the build

The whole `WebKitBuild/Release` tree is self-contained for a given macOS
major version + arch:

```bash
# Copy these together; keep the layout intact:
#   *.framework  MiniBrowser.app  jsc  *.dSYM

# jsc on another machine:
DYLD_FRAMEWORK_PATH=/path/to/Release /path/to/Release/jsc script.js

# MiniBrowser: launch its binary directly with the framework path injected
# (SIP strips DYLD_* from /Applications apps, so you cannot point *system* Safari at
#  these frameworks — MiniBrowser is the supported host):
DYLD_FRAMEWORK_PATH=/path/to/Release \
    /path/to/Release/MiniBrowser.app/Contents/MacOS/MiniBrowser
```

Notes:
- Match the **macOS deployment target** to the oldest OS you must run on; a build
  made against a newer SDK/min-version may not launch on older systems.
- Carry the `.dSYM` bundles for symbolication; they are not needed to run.
- System framework dependencies (CoreFoundation, Metal, etc.) always resolve from
  the host OS and cannot be bundled.

## Fallback: consume the profile without the build-phase plumbing

If the "Copy Profiling Data" phase misbehaves in your Xcode, bypass it. Decompress
and merge the three profiles into one and inject it directly:

```bash
D=WebKitBuild/PGO/output                      # combine step already wrote merged, weighted profdata here
xcrun llvm-profdata merge -o /tmp/webkit.profdata \
    "$D/JavaScriptCore.profdata" "$D/WebCore.profdata" "$D/WebKit.profdata"

make release ASAN=NO WK_LTO_MODE=full \
    EXTRA_CFLAGS="-fno-omit-frame-pointer -fprofile-use=/tmp/webkit.profdata -Wno-error=backend-plugin" \
    OTHER_LDFLAGS="-fprofile-use=/tmp/webkit.profdata"
```

One merged profile applied globally is slightly less precise than the per-framework
profiles (clang ignores non-matching functions, hence `-Wno-error=backend-plugin`),
but it is simpler and does not depend on the internal decompression plumbing.

---

## Appendix: pointers for the Linux / CMake build

The next target is Linux (GTK or WPE port, CMake + Ninja). The concepts are the
same; the mechanics differ. Notes to save the next agent time:

**Environment.** Build inside the wkdev container (`~/Development/webkit-container-sdk`,
`source register-sdk-on-host.sh`, then `wkdev-enter`; pinned version in
`.wkdev-sdk-version`). **PGO requires clang** — the container's clang is fine; GCC is
rejected ([Source/cmake/WebKitCommon.cmake:325](../Source/cmake/WebKitCommon.cmake#L325)).

**Build.** `Tools/Scripts/build-webkit --release [--gtk|--wpe]` for full WebKit
(needed for SP3/MM), `--jsc-only` for JS3-only. Artifacts land in
`WebKitBuild/<Port>/Release/{bin,lib}`; binaries link `lib/` via RPATH, so the
`{bin,lib}` pair is already relocatable — copy it as-is (this is the Linux answer to
"few dynamic libraries / portable").

**The CMake PGO path is cleaner than Cocoa's** (added in bug 309318,
[Source/cmake/WebKitCommon.cmake:322-391](../Source/cmake/WebKitCommon.cmake#L322)) —
one merged `.profdata` for the whole build, not per-framework:

- Generate: `-DENABLE_LLVM_PROFILE_GENERATION=ON` (optionally `-DPGO_PROFILE_DIR=/dir`).
  Adds `-fprofile-generate`. **The `/private/tmp/WebKitPGO` baked path is Cocoa-only**
  (`WebKit2InitializeCocoa.mm` etc. are `#if PLATFORM(COCOA)`); on Linux profiles go
  to the `-fprofile-generate` default (cwd) unless you set `PGO_PROFILE_DIR` or the
  `LLVM_PROFILE_FILE=/path/%p_%m.profraw` env var at runtime.
- Use: `-DUSE_PGO_PROFILE=ON -DPGO_PROFILE_PATH=/abs/merged.profdata`. Conflicts with
  `ENABLE_LLVM_PROFILE_GENERATION` ([WebKitFeatures.cmake:331](../Source/cmake/WebKitFeatures.cmake#L331)).

**Full LTO.** `--lto-mode=full` (build-webkit) or `-DLTO_MODE=full`; becomes
`-flto=full` ([WebKitCompilerFlags.cmake:354](../Source/cmake/WebKitCompilerFlags.cmake#L354)).

**Frame pointers.** `-fno-omit-frame-pointer` is auto-added in `DEVELOPER_MODE` and on
ARM ([WebKitCompilerFlags.cmake:176](../Source/cmake/WebKitCompilerFlags.cmake#L176));
otherwise add it via `-DCMAKE_C_FLAGS`/`-DCMAKE_CXX_FLAGS`.

**Debug symbols.** Use `-DCMAKE_BUILD_TYPE=RelWithDebInfo` (Release + `-g`); consider
`-gsplit-dwarf` to keep link times/size sane. Confirm how `build-webkit --release`
maps to `CMAKE_BUILD_TYPE`.

**Profile collection — the open items to check:**
- `Tools/Scripts/pgo-profile`'s `merge`/`combine` use `llvm-profdata` and work on
  Linux, but `compress`/`decompress` call macOS-only `/usr/bin/compression_tool`
  ([llvm_profile_utils.py](../Tools/Scripts/webkitpy/llvm_profile_utils.py)).
  You do **not** need them on Linux: `USE_PGO_PROFILE` takes a plain `.profdata`. So:
  run benchmarks → collect `.profraw` → `llvm-profdata merge` (optionally the weighted
  `combine`) into one `.profdata` → `PGO_PROFILE_PATH`.
- The Linux MiniBrowser driver(s) under
  [Tools/Scripts/webkitpy/benchmark_runner/browser_driver/](../Tools/Scripts/webkitpy/benchmark_runner/browser_driver/)
  will likely need the same `pgo_profile_output_directories` fix **and** need
  `LLVM_PROFILE_FILE` exported into the WebKit process environment (there is no baked
  path on Linux). Verify before relying on `--generate-pgo-profiles`.
- Same display caveat: GUI benchmarks need a real (or virtual, e.g. `weston`/Xvfb with
  a compositor that actually paints) display or `requestAnimationFrame` stalls.
