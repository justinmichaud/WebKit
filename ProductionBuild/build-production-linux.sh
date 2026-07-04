#!/usr/bin/env bash
#
# build-production-linux.sh
#
# Linux (WebKitGTK, CMake+Ninja) equivalent of build-production-macos.sh.
# Drives an open-source production WebKit build tuned for maximum performance:
#   1. instrument  -> clang build with LLVM PGO instrumentation (-fprofile-generate)
#   2. collect     -> run Speedometer 3 / JetStream 3 / MotionMark, gather .profraw,
#                     merge into one .profdata with llvm-profdata
#   3. build       -> from-scratch build with full LTO + -fprofile-use + the profile,
#                     linked with --emit-relocs so BOLT can post-process it
#   4. bolt        -> BOLT post-link optimization of the hot libs (instrument -> run the
#                     benchmarks again -> reorder/split by profile). Optional, best-effort.
#   5. package     -> bundle the shared-library closure so the tree runs on other machines
#                     without installing a pile of -dev packages
#
# Properties delivered (see README-linux.md for the long form):
#   - Frame-pointer unwinding : -fno-omit-frame-pointer (auto on aarch64; forced anyway)
#   - Debug symbols           : -g, embedded DWARF (split-dwarf disabled -> relocatable)
#   - Max optimization        : Release => -O3; no C++ stdlib assertions; CPU-tuned; LTO; PGO; BOLT
#   - Fresh PGO profiles      : instrument -> run SP3/JS3/MM -> profile-use
#   - Full LTO                : -DLTO_MODE=full => -flto=full (monolithic)
#   - Portable / few libs     : package/ bundles the external .so closure, RPATH=$ORIGIN
#
# Environment knobs:
#   PORT            gtk (default)
#   LTO_MODE        full (default) | thin | none  -- LTO for the FINAL build
#   CPU_FLAGS       arch/tune flags. Default targets the common ISA of this box (Neoverse-N1)
#                   and a Raspberry Pi 4 (Cortex-A72): "-march=armv8-a+crc -mtune=neoverse-n1".
#                   Set "-mcpu=native" for this machine only, or "" for generic.
#   BENCH_COUNT     benchmark iterations per plan for PGO training (default 3)
#   BENCHMARKS      space list; default "speedometer3 jetstream3 motionmark1.3.1"
#   PROFILE_DIR     where profiles live. Default WebKitBuild/PGO
#   BOLT_LIBS       which libs to BOLT (default: javascriptcoregtk + webkitgtk)

set -euo pipefail

# ---------------------------------------------------------------------------
# Configuration
# ---------------------------------------------------------------------------
WEBKIT_ROOT="$(git -C "$(dirname "$0")" rev-parse --show-toplevel)"
cd "$WEBKIT_ROOT"

PORT="${PORT:-gtk}"
PORT_UPPER="$(echo "$PORT" | tr '[:lower:]' '[:upper:]')"
ARCH="$(uname -m)"
LTO_MODE="${LTO_MODE:-full}"
BENCH_COUNT="${BENCH_COUNT:-3}"
BENCHMARKS="${BENCHMARKS:-speedometer3 jetstream3 motionmark1.3.1}"

# Most-aggressive ISA that still runs on both this Neoverse-N1 and a Raspberry Pi 4
# (Cortex-A72 = ARMv8.0-A, has crc32 but no crypto/LSE/dotprod/fp16). outline-atomics
# (on by default) picks LSE at runtime on the N1 and falls back to exclusives on the A72.
CPU_FLAGS="${CPU_FLAGS:--march=armv8-a+crc -mtune=neoverse-n1}"

BUILD="$WEBKIT_ROOT/WebKitBuild/$PORT_UPPER/Release"
PROFILE_DIR="${PROFILE_DIR:-$WEBKIT_ROOT/WebKitBuild/PGO}"
PROFRAW_DIR="$PROFILE_DIR/profraw"
MERGED_PROFILE="$PROFILE_DIR/webkit-$ARCH.profdata"
BOLT_DIR="$PROFILE_DIR/bolt"
PACKAGE_DIR="$WEBKIT_ROOT/WebKitBuild/Production-$PORT_UPPER-$ARCH"

export CC="${CC:-clang}"
export CXX="${CXX:-clang++}"
LLVM_PROFDATA="$(command -v llvm-profdata || command -v llvm-profdata-18 || echo /usr/bin/llvm-profdata-18)"
LLVM_BOLT="$(command -v llvm-bolt || command -v llvm-bolt-19 || echo /usr/lib/llvm-19/bin/llvm-bolt)"
MERGE_FDATA="$(command -v merge-fdata || echo /usr/lib/llvm-19/bin/merge-fdata)"
# The bolt-19 apt package installs the instrumentation runtime under llvm-19/lib, but
# llvm-bolt looks for it at /usr/lib; point at it explicitly.
BOLT_RT_LIB="${BOLT_RT_LIB:-$(ls /usr/lib/llvm-19/lib/libbolt_rt_instr.a /usr/lib/libbolt_rt_instr.a 2>/dev/null | head -1)}"

export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"
WESTON_SOCKET="wk-pgo-wl"
WESTON_LOG="$PROFILE_DIR/weston.log"
WESTON_PIDFILE="$PROFILE_DIR/weston.pid"
DISPLAY_SOCKET=""       # chosen by pick_display()
USE_SW_GL=0

banner() { printf '\n\033[1m===== %s =====\033[0m\n\n' "$*"; }
note()   { printf '\033[36m%s\033[0m\n' "$*"; }
die()    { printf '\033[31merror: %s\033[0m\n' "$*" >&2; exit 1; }

require_clang() {
    "$CC" --version | grep -qi clang || die "CC=$CC is not clang; PGO/LTO require clang."
    [[ -x "$LLVM_PROFDATA" ]] || die "llvm-profdata not found (tried: $LLVM_PROFDATA). Run '$0 deps'."
    local rtdir; rtdir="$("$CC" --print-resource-dir)/lib/linux"
    ls "$rtdir"/libclang_rt.profile-*.a >/dev/null 2>&1 \
        || die "clang profile runtime missing under $rtdir. Run '$0 deps'."
}

# Install the packages + the one BOLT tweak this build needs. Idempotent.
deps() {
    banner "Install build dependencies (needs sudo)"
    sudo apt-get install -y --no-install-recommends \
        llvm-18 libclang-rt-18-dev python3-twisted patchelf bolt-19
    # llvm-bolt --instrument looks for its runtime at /usr/lib/libbolt_rt_instr.a, but the
    # bolt-19 package installs it under llvm-19/lib. Symlink it at the default path.
    local rt; rt="$(ls /usr/lib/llvm-19/lib/libbolt_rt_instr.a 2>/dev/null | head -1)"
    [[ -n "$rt" && ! -e /usr/lib/libbolt_rt_instr.a ]] && sudo ln -sf "$rt" /usr/lib/libbolt_rt_instr.a
    note "deps installed. WebKitGTK's own build deps come from the wkdev container."
}

# The two source/tooling changes this build requires must be present in the tree (they are
# not applied by this script). A clean checkout will lack them; fail early with instructions
# rather than produce a build that can't run from the tree or can't collect a PGO profile.
require_patches() {
    grep -q 'ENABLE(DEVELOPER_MODE)' Source/WebKit/Shared/glib/ProcessExecutablePathGLib.cpp \
        && die "patch missing: Source/WebKit/Shared/glib/ProcessExecutablePathGLib.cpp must resolve
  helper processes without ENABLE(DEVELOPER_MODE) (remove the #if guard in findWebKitProcess),
  else a dev-mode-off / relocated MiniBrowser can't find WebKitWebProcess. See README-linux.md."
    grep -q 'wait_procs' Tools/Scripts/webkitpy/benchmark_runner/browser_driver/linux_browser_driver.py \
        || die "patch missing: linux_browser_driver.py close_browsers() must SIGTERM the browser
  gracefully (psutil.wait_procs) so instrumented processes flush their profile. See README-linux.md."
    grep -q 'Bind the tree holding the helper processes' Source/WebKit/UIProcess/Launcher/glib/BubblewrapLauncher.cpp \
        || die "patch missing: BubblewrapLauncher.cpp must bind the helper-process tree into the
  sandbox without ENABLE(DEVELOPER_MODE) (remove that #if guard), else bwrap can't exec
  WebKitWebProcess from a relocated tree with the sandbox on. See README-linux.md."
}

# ---------------------------------------------------------------------------
# Display for the GUI benchmarks (SP3/MotionMark)
# ---------------------------------------------------------------------------
weston_running() { [[ -S "$XDG_RUNTIME_DIR/$WESTON_SOCKET" ]]; }

start_weston() {
    mkdir -p "$PROFILE_DIR"
    if weston_running; then note "weston already up on $WESTON_SOCKET"; return; fi
    command -v weston >/dev/null || die "weston not installed; needed for the SP3/MM display."
    note "starting headless weston on \$XDG_RUNTIME_DIR/$WESTON_SOCKET ..."
    setsid weston --backend=headless-backend.so --socket="$WESTON_SOCKET" \
        --width=1920 --height=1080 --idle-time=0 >"$WESTON_LOG" 2>&1 &
    echo $! >"$WESTON_PIDFILE"
    local tries=0
    until weston_running || (( tries > 100 )); do tries=$((tries+1)); read -t 0.1 _ </dev/null 2>/dev/null || true; done
    weston_running || die "weston did not create its socket; see $WESTON_LOG"
}

# Prefer a real host Wayland display (real GPU, real frame callbacks -> representative
# MotionMark). Fall back to a headless weston + software GL only if none is present.
pick_display() {
    if [[ -S "$XDG_RUNTIME_DIR/wayland-0" ]]; then
        DISPLAY_SOCKET="wayland-0"; USE_SW_GL=0
        note "using host Wayland display 'wayland-0' (real GPU/compositor)"
    else
        start_weston; DISPLAY_SOCKET="$WESTON_SOCKET"; USE_SW_GL=1
        note "using headless weston '$WESTON_SOCKET' with software GL (no host display found)"
    fi
}

benchmark_env() {
    export WAYLAND_DISPLAY="$DISPLAY_SOCKET"
    export GDK_BACKEND=wayland
    if [[ "$USE_SW_GL" == "1" ]]; then
        export LIBGL_ALWAYS_SOFTWARE=1 GALLIUM_DRIVER=llvmpipe
    else
        unset LIBGL_ALWAYS_SOFTWARE GALLIUM_DRIVER 2>/dev/null || true
    fi
    # The WebProcess (where JS/layout/paint run) is sandboxed by default with a private
    # /tmp, so its profile/BOLT data would be trapped inside the sandbox and lost.
    export WEBKIT_DISABLE_SANDBOX_THIS_IS_DANGEROUS=1
}

# ---------------------------------------------------------------------------
# Common cmake args
# ---------------------------------------------------------------------------
# DEVELOPER_MODE=OFF: no dev-only cost. Its stdlib-assertion tax is the real hit, and
#   that is the separate USE_CXX_STDLIB_ASSERTIONS option (on by default) -> force OFF.
#   Helper-process resolution normally needs DEVELOPER_MODE; a companion patch to
#   ProcessExecutablePathGLib.cpp lifts that so the relocatable tree still finds them.
# EXPERIMENTAL_FEATURES=OFF: stable feature set (experimental WebRTC/WebExtensions fail to
#   link in this container). THUNDER=OFF: DRM backend not installed.
COMMON_ARGS="-DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
-DDEVELOPER_MODE=OFF -DENABLE_EXPERIMENTAL_FEATURES=OFF -DUSE_CXX_STDLIB_ASSERTIONS=OFF \
-DDEBUG_FISSION=OFF -DENABLE_THUNDER=OFF"

# ---------------------------------------------------------------------------
# Phase 1: instrumented build
# ---------------------------------------------------------------------------
instrument() {
    banner "Phase 1: instrumented build ($PORT, clang, PGO generate)"
    require_clang
    require_patches
    rm -rf "$BUILD" "$WEBKIT_ROOT/WebKitBuild/$PORT_UPPER/Release.build" 2>/dev/null || true
    mkdir -p "$PROFRAW_DIR"

    # No continuous mode (%c): it yields truncated, unmergeable profiles with WebKit's many
    # DSOs on clang-18. Profiles flush on a clean process exit, guaranteed by the graceful
    # browser shutdown in collect(). No -DLTO_MODE: LTO_MODE="none" would emit invalid -flto=none.
    local flags="-g -fno-omit-frame-pointer $CPU_FLAGS"
    CFLAGS="$flags" CXXFLAGS="$flags" \
    Tools/Scripts/build-webkit --"$PORT" --release \
        --cmakeargs="$COMMON_ARGS -DENABLE_LLVM_PROFILE_GENERATION=ON -DPGO_PROFILE_DIR=$PROFRAW_DIR"

    note "instrumented build in $BUILD"
    [[ -x "$BUILD/bin/MiniBrowser" ]] || die "MiniBrowser not built in $BUILD/bin"
    [[ -x "$BUILD/bin/jsc" ]]         || die "jsc not built in $BUILD/bin"
}

# ---------------------------------------------------------------------------
# Phase 2: collect PGO profiles
# ---------------------------------------------------------------------------
RUNLOG_DIR="$PROFILE_DIR/runlogs"

run_one_benchmark() {
    local plan="$1"
    banner "run: $plan (count=$BENCH_COUNT)"
    mkdir -p "$RUNLOG_DIR"
    # Tee the raw output to a log so we can confirm a real score was produced (below); the
    # pipeline's non-zero exit (the browser's graceful SIGTERM = 143) is expected, hence || true.
    LLVM_PROFILE_FILE="$PROFRAW_DIR/${plan}_%m_%p.profraw" \
    Tools/Scripts/run-benchmark \
        --plan "$plan" \
        --browser "minibrowser-$PORT" \
        --build-directory "$BUILD" \
        --count "$BENCH_COUNT" \
        --no-adjust-unit \
        2>&1 | tee "$RUNLOG_DIR/$plan.log" | sed "s/^/[$plan] /" || true
}

# A real run prints "<Name>:Score: <number>pt" (SP3/JS3/MM all do, at least per subtest).
# Its absence means the benchmark never ran its suites -- it sat on the landing/white screen,
# crashed, or timed out (e.g. a browser too slow under instrumentation) -- so the collected
# profile/BOLT data would be non-representative. run_benchmarks() fails loudly in that case.
benchmark_scored() { grep -qaE 'Score:[^0-9]*[0-9]+(\.[0-9]+)?pt' "$RUNLOG_DIR/$1.log" 2>/dev/null; }

# Run every plan, then require that each produced a score. Shared by collect() and bolt().
run_benchmarks() {
    local phase="$1" scoreless=()
    for plan in $BENCHMARKS; do
        run_one_benchmark "$plan"
        if benchmark_scored "$plan"; then
            note "$phase: $plan scored -> $(grep -aoE '[A-Za-z0-9.-]+:Score:[^0-9]*[0-9.]+pt' "$RUNLOG_DIR/$plan.log" | head -1)"
        else
            scoreless+=("$plan")
            note "$phase: $plan produced NO score (did not run to completion)."
        fi
    done
    if (( ${#scoreless[@]} )); then
        die "$phase: no score from: ${scoreless[*]}. The benchmark(s) never ran their suites
  (landing/white screen, crash, or timeout -- e.g. too slow under instrumentation). The
  profile/layout data would be non-representative. Check the display ($DISPLAY_SOCKET) and re-run."
    fi
}

collect() {
    banner "Phase 2: collect fresh PGO profiles from: $BENCHMARKS"
    [[ -x "$BUILD/bin/MiniBrowser" ]] || die "no instrumented build; run '$0 instrument' first."
    rm -f "$PROFRAW_DIR"/*.profraw 2>/dev/null || true
    pick_display
    benchmark_env

    run_benchmarks "collect"

    local n; n=$(ls "$PROFRAW_DIR"/*.profraw 2>/dev/null | wc -l)
    [[ "$n" -gt 0 ]] || die "no .profraw produced; check the display/sandbox/flush plumbing."
    note "collected $n .profraw files"

    banner "merge -> $MERGED_PROFILE"
    "$LLVM_PROFDATA" merge -o "$MERGED_PROFILE" "$PROFRAW_DIR"/*.profraw
    note "merged profile: $("$LLVM_PROFDATA" show "$MERGED_PROFILE" 2>/dev/null | grep -i 'total functions' || true)"
}

# ---------------------------------------------------------------------------
# Phase 3: final production build (full LTO + profile-use + emit-relocs for BOLT)
# ---------------------------------------------------------------------------
final_build() {
    banner "Phase 3: final production build (full LTO=$LTO_MODE + PGO use + emit-relocs)"
    [[ -f "$MERGED_PROFILE" ]] || die "no merged profile at $MERGED_PROFILE; run '$0 collect' first."
    require_clang
    require_patches
    note "cleaning $BUILD for a from-scratch optimized build..."
    rm -rf "$BUILD" "$WEBKIT_ROOT/WebKitBuild/$PORT_UPPER/Release.build" 2>/dev/null || true

    local lto_arg=""; [[ "$LTO_MODE" != "none" ]] && lto_arg="-DLTO_MODE=$LTO_MODE"
    # -Wl,--emit-relocs keeps relocations in the output so llvm-bolt can rewrite the libs.
    local flags="-g -fno-omit-frame-pointer $CPU_FLAGS -Wl,--emit-relocs"
    CFLAGS="$flags" CXXFLAGS="$flags" \
    Tools/Scripts/build-webkit --"$PORT" --release \
        --cmakeargs="$COMMON_ARGS $lto_arg -DUSE_PGO_PROFILE=ON -DPGO_PROFILE_PATH=$MERGED_PROFILE"

    note "final build in $BUILD"
    [[ -x "$BUILD/bin/MiniBrowser" ]] || die "MiniBrowser not built in $BUILD/bin"
}

# ---------------------------------------------------------------------------
# Phase 4: BOLT post-link optimization -- EXPERIMENTAL, OFF BY DEFAULT.
# ---------------------------------------------------------------------------
# Do not add this to `all`. On this platform BOLT could not beat the compiler's PGO+LTO
# layout; the only crash-free way to profile it here regressed Speedometer 3 by ~6% (measured,
# highly significant), in every config tried. Why:
#   * BOLT --instrument is broken on aarch64 (LLVM issue #165664): its per-counter thread-local
#     store uses a mis-computed offset (mrs tpidr_el0 + movk; str [x16,x0]) and scribbles the
#     heap. Trivial interpreter-only code survives, but the instant JS tiers up to the DFG JIT
#     a compiler worker thread dies with memory corruption (bogus WTF::Vector size / libpas
#     "Large heap did not find object"), nondeterministically. --instrument-calls=false lowers
#     the rate (jsc micro-loops survive) but a real Speedometer/MotionMark WebProcess still
#     crashes within seconds. So instrumentation-mode profiling is unusable for the browser.
#   * aarch64 has no LBR, this box has no ARM SPE, and BOLT SPE support needs LLVM>=21 + perf
#     >=6.14 anyway. That leaves plain no-LBR `perf` sampling -- crash-free, but coarse (no
#     branch data; ~1/3 of a JS workload's samples land in JIT code BOLT can't see). Re-laying
#     out the already-PGO+LTO'd libs from that coarse profile made things worse, not better:
#     ext-tsp+cdsort+split measured -6.1% on SP3, and even hot/cold-split-only measured -6.4%.
# The implementation below uses the SAFE sampling path (never instruments), so running it will
# not crash -- it will just (currently) produce slower libs. It is kept for the day BOLT gains
# working aarch64 branch profiling (SPE). Run only to experiment: `./build-production-linux.sh
# bolt`; afterwards re-run `build` to restore the clean PGO+LTO libs. See README-linux.md "BOLT".
bolt_libs() {
    # Real .so files (not the symlinks) for the hottest libraries.
    ls "$BUILD"/lib/libjavascriptcoregtk-*.so.*.*.* "$BUILD"/lib/libwebkitgtk-*.so.*.*.* 2>/dev/null | grep -v -- '->'
}

# Locate a `perf` that actually runs here. The distro wrapper refuses on a mismatched kernel
# (this box runs a kernel whose linux-tools aren't installed); the versioned binary still works
# for user-space cycle sampling. perf2bolt shells out to `perf`, so expose it as `perf` on PATH.
find_perf() {
    if perf --version >/dev/null 2>&1; then command -v perf; return; fi
    ls /usr/lib/linux-tools/*/perf 2>/dev/null | head -1
}

bolt() {
    banner "Phase 4: BOLT (EXPERIMENTAL, sampling-based; OFF by default -- regressed SP3 ~6% here)"
    note "NOTE: BOLT is disabled in the default build; see the comment above bolt() and README-linux.md."
    note "      This runs the SAFE (sampling) path -- it will not crash, but currently yields slower libs."
    [[ -x "$LLVM_BOLT" ]] || die "llvm-bolt not found (install bolt-19); tried $LLVM_BOLT"
    [[ -x "$BUILD/bin/MiniBrowser" ]] || die "no final build; run '$0 build' first."
    local perf2bolt; perf2bolt="$(command -v perf2bolt || echo /usr/lib/llvm-19/bin/perf2bolt)"
    [[ -x "$perf2bolt" ]] || die "perf2bolt not found (part of the bolt-19 package)."
    local perfbin; perfbin="$(find_perf)"
    [[ -x "$perfbin" ]] || die "no usable perf binary for sampling (need linux-tools)."
    # Confirm the libs carry relocations (need -Wl,--emit-relocs at link, from Phase 3).
    local anylib; anylib="$(bolt_libs | head -1)"
    readelf -S "$anylib" 2>/dev/null | grep -q '\.rela\.text' \
        || die "libs lack .rela.text; rebuild Phase 3 (it links with --emit-relocs)."

    rm -rf "$BOLT_DIR"; mkdir -p "$BOLT_DIR/perfshim"
    ln -sf "$perfbin" "$BOLT_DIR/perfshim/perf"; export PATH="$BOLT_DIR/perfshim:$PATH"
    local -a targets; mapfile -t targets < <(bolt_libs)
    [[ ${#targets[@]} -gt 0 ]] || die "no BOLT target libs found."

    # 1) Sample the CLEAN final build running a benchmark. No instrumentation -> no crash.
    #    per-process `--inherit` follows run-benchmark's whole tree (MiniBrowser + WebProcess),
    #    so it works at perf_event_paranoid=1 without CAP_PERFMON / system-wide access.
    pick_display; benchmark_env
    local perfdata="$BOLT_DIR/bench.perf.data"
    note "sampling a Speedometer 3 run with perf (crash-free; captures the WebProcess) ..."
    "$perfbin" record -e cycles:u -F 1500 --inherit -o "$perfdata" -- \
        Tools/Scripts/run-benchmark --plan speedometer3 --browser "minibrowser-$PORT" \
            --build-directory "$BUILD" --count 1 --no-adjust-unit \
        >"$BOLT_DIR/perf-record.log" 2>&1 || note "  (run-benchmark returned nonzero; continuing)"
    [[ -s "$perfdata" ]] || { note "no perf.data captured; leaving PGO+LTO libs in place."; return 0; }

    # 2) Convert samples to BOLT profile (no-LBR) and optimize each lib in place. Skip JSC's
    #    hand-written-assembly families (LLInt/vmEntry/IPInt) -- they share one giant CFI/FDE
    #    (V8's -skip-funcs=Builtins_.* analogue), which BOLT can't split and shouldn't rewrite.
    local skip='llint_.*,vmEntry.*,.*llintPCRange.*,js_trampoline_.*,wasm_.*,ipint_.*'
    for lib in "${targets[@]}"; do
        local base; base="$(basename "$lib")"
        cp -a "$lib" "$BOLT_DIR/$base.orig"
        if ! "$perf2bolt" -p "$perfdata" -o "$BOLT_DIR/$base.fdata" -nl -ignore-build-id \
                "$BOLT_DIR/$base.orig" 2>"$BOLT_DIR/$base.perf2bolt.log"; then
            note "  WARNING: perf2bolt failed for $base; leaving PGO+LTO version in place."; continue
        fi
        note "BOLT-optimizing $base (sampling profile) ..."
        # ext-tsp block layout + profile-driven function reordering + hot/cold splitting +
        # identical-code folding. --update-debug-sections keeps DWARF valid for the samplers.
        if "$LLVM_BOLT" "$BOLT_DIR/$base.orig" -data="$BOLT_DIR/$base.fdata" -o "$lib" \
                -reorder-blocks=ext-tsp -reorder-functions=cdsort -split-functions \
                -icf=1 -skip-funcs="$skip" -update-debug-sections -dyno-stats \
                2>"$BOLT_DIR/$base.optimize.log"; then
            note "  optimized ok -> $(du -h "$lib" | awk '{print $1}')"
        else
            note "  WARNING: BOLT optimize failed for $base (see $BOLT_DIR/$base.optimize.log); restoring PGO+LTO version."
            cp -a "$BOLT_DIR/$base.orig" "$lib"
        fi
    done
    note "BOLT phase done. REMINDER: measured ~6% SLOWER on SP3 here -- re-run 'build' to restore clean PGO+LTO libs."
}

# ---------------------------------------------------------------------------
# Phase 5: package (bundle the shared-library closure for portability)
# ---------------------------------------------------------------------------
is_system_lib() {
    case "$1" in
        libc.so*|libm.so*|libdl.so*|libpthread.so*|librt.so*|libresolv.so*|\
        ld-linux*.so*|libgcc_s.so*|libutil.so*|libanl.so*|\
        libGL.so*|libGLX.so*|libGLdispatch.so*|libEGL.so*|libGLESv2.so*|\
        libdrm.so*|libgbm.so*|libgallium*|*dri.so*|libnvidia*|libcuda*) return 0 ;;
        *) return 1 ;;
    esac
}

package() {
    banner "Phase 5: package portable tree -> $PACKAGE_DIR"
    [[ -x "$BUILD/bin/MiniBrowser" ]] || die "no final build; run '$0 build' first."
    command -v patchelf >/dev/null || die "patchelf not installed."

    rm -rf "$PACKAGE_DIR"; mkdir -p "$PACKAGE_DIR"/{bin,lib}
    note "copying bin/ and lib/ ..."
    cp -a "$BUILD/bin/." "$PACKAGE_DIR/bin/"
    cp -a "$BUILD/lib/." "$PACKAGE_DIR/lib/"

    note "resolving shared-library closure (bundles all non-system deps)..."
    local seen="$PACKAGE_DIR/.seen"; : >"$seen"
    resolve() {
        ldd "$1" 2>/dev/null | awk '/=> \//{print $3}' | while read -r dep; do
            [[ -f "$dep" ]] || continue
            local base; base="$(basename "$dep")"
            is_system_lib "$base" && continue
            [[ -e "$PACKAGE_DIR/lib/$base" ]] && continue
            grep -qxF "$dep" "$seen" && continue
            echo "$dep" >>"$seen"
            cp -Ln "$dep" "$PACKAGE_DIR/lib/$base" 2>/dev/null || cp -L "$dep" "$PACKAGE_DIR/lib/$base"
        done
    }
    local pass=0 added=1
    while (( added )); do
        added=0; pass=$((pass+1))
        while IFS= read -r -d '' elf; do
            local before after; before=$(ls -1 "$PACKAGE_DIR/lib" | wc -l)
            resolve "$elf"; after=$(ls -1 "$PACKAGE_DIR/lib" | wc -l)
            (( after > before )) && added=1
        done < <(find "$PACKAGE_DIR/bin" "$PACKAGE_DIR/lib" -type f \( -perm -u+x -o -name '*.so*' \) -print0)
        (( pass > 8 )) && break
    done
    rm -f "$seen"
    note "bundled $(ls -1 "$PACKAGE_DIR/lib"/*.so* 2>/dev/null | wc -l) shared objects"

    note "patching RPATHs to \$ORIGIN (relocatable) ..."
    local rp='$ORIGIN:$ORIGIN/../lib:$ORIGIN/..:$ORIGIN/../..'
    while IFS= read -r -d '' elf; do
        patchelf --set-rpath "$rp" "$elf" 2>/dev/null || true
    done < <(find "$PACKAGE_DIR/bin" "$PACKAGE_DIR/lib" -type f \( -perm -u+x -o -name '*.so*' \) -print0)

    bundle_runtime_data
    write_launchers

    note "portable tree ready: $PACKAGE_DIR"
    tar -C "$(dirname "$PACKAGE_DIR")" -czf "$PACKAGE_DIR.tar.gz" "$(basename "$PACKAGE_DIR")"
    ls -lh "$PACKAGE_DIR.tar.gz"
}

bundle_runtime_data() {
    local pb; pb="$(pkg-config --variable=gdk_pixbuf_moduledir gdk-pixbuf-2.0 2>/dev/null || true)"
    if [[ -n "$pb" && -d "$pb" ]]; then
        mkdir -p "$PACKAGE_DIR/lib/gdk-pixbuf/loaders"
        cp -Ln "$pb"/*.so "$PACKAGE_DIR/lib/gdk-pixbuf/loaders/" 2>/dev/null || true
        find "$PACKAGE_DIR/lib/gdk-pixbuf/loaders" -name '*.so' -exec patchelf --set-rpath '$ORIGIN/../..' {} \; 2>/dev/null || true
        GDK_PIXBUF_MODULEDIR="$PACKAGE_DIR/lib/gdk-pixbuf/loaders" \
            gdk-pixbuf-query-loaders > "$PACKAGE_DIR/lib/gdk-pixbuf/loaders.cache" 2>/dev/null || true
    fi
    local gio; gio="$(pkg-config --variable=giomoduledir gio-2.0 2>/dev/null || true)"
    if [[ -n "$gio" && -d "$gio" ]]; then
        mkdir -p "$PACKAGE_DIR/lib/gio/modules"
        cp -Ln "$gio"/*.so "$PACKAGE_DIR/lib/gio/modules/" 2>/dev/null || true
        find "$PACKAGE_DIR/lib/gio/modules" -name '*.so' -exec patchelf --set-rpath '$ORIGIN/../..' {} \; 2>/dev/null || true
    fi
    if [[ -d /usr/share/glib-2.0/schemas ]]; then
        mkdir -p "$PACKAGE_DIR/share/glib-2.0/schemas"
        cp -a /usr/share/glib-2.0/schemas/*.xml "$PACKAGE_DIR/share/glib-2.0/schemas/" 2>/dev/null || true
        glib-compile-schemas "$PACKAGE_DIR/share/glib-2.0/schemas" 2>/dev/null || true
    fi
    for d in share/webkitgtk-6.0 share/wpe-webkit-2.0; do
        if [[ -d "$BUILD/../$d" ]]; then
            mkdir -p "$PACKAGE_DIR/$d"; cp -a "$BUILD/../$d/." "$PACKAGE_DIR/$d/"
        fi
    done
    return 0
}

write_launchers() {
    cat > "$PACKAGE_DIR/run-jsc.sh" <<'EOF'
#!/usr/bin/env bash
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
exec "$HERE/bin/jsc" "$@"
EOF
    cat > "$PACKAGE_DIR/run-minibrowser.sh" <<'EOF'
#!/usr/bin/env bash
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
export WEBKIT_EXEC_PATH="$HERE/bin"
export WEBKIT_INJECTED_BUNDLE_PATH="$HERE/lib"
export GDK_PIXBUF_MODULE_FILE="$HERE/lib/gdk-pixbuf/loaders.cache"
export GIO_MODULE_DIR="$HERE/lib/gio/modules"
export GSETTINGS_SCHEMA_DIR="$HERE/share/glib-2.0/schemas"
# The bubblewrap sandbox stays ON. It works from this relocated tree because the build
# includes the ProcessExecutablePathGLib.cpp + BubblewrapLauncher.cpp patches that resolve
# and bind-mount the helper processes without ENABLE(DEVELOPER_MODE). See README-linux.md.
exec "$HERE/bin/MiniBrowser" "$@"
EOF
    chmod +x "$PACKAGE_DIR/run-jsc.sh" "$PACKAGE_DIR/run-minibrowser.sh"
}

# ---------------------------------------------------------------------------
# verify
# ---------------------------------------------------------------------------
verify() {
    banner "Verify final build: $BUILD"
    local lib; lib="$(ls "$BUILD"/lib/libjavascriptcoregtk-*.so.*.*.* 2>/dev/null | grep -v -- '->' | head -1)"
    echo "== frame pointers (aarch64 prologues push x29/x30) =="
    objdump -d "$BUILD/bin/jsc" 2>/dev/null | grep -m2 -E "stp\s+x29, x30|mov\s+x29, sp" || echo "  (inspect manually)"
    echo "== CPU target (should show the tuned arch, not generic) =="
    grep -oE "\-march=[a-z0-9.+-]+|\-mtune=[a-z0-9-]+|\-mcpu=[a-z0-9-]+" "$BUILD/build.ninja" 2>/dev/null | sort -u | head
    echo "== stdlib assertions OFF (no _GLIBCXX_ASSERTIONS in compile flags) =="
    grep -q "_GLIBCXX_ASSERTIONS" "$BUILD/build.ninja" 2>/dev/null && echo "  WARNING: still enabled" || echo "  ok: not defined"
    echo "== debug info present =="
    readelf -S "$lib" 2>/dev/null | grep -q debug_info && echo "  yes: $(basename "$lib") has .debug_info" || echo "  NO debug_info"
    echo "== PGO + LTO recorded =="
    grep -E "USE_PGO_PROFILE:|LTO_MODE:" "$BUILD/CMakeCache.txt" 2>/dev/null
    echo "== BOLT applied? (BOLT adds a .bolt marker section / __bolt runtime syms) =="
    readelf -S "$lib" 2>/dev/null | grep -qiE "bolt" && echo "  yes: BOLT sections present" || echo "  (not BOLTed, or BOLT skipped)"
    echo "== jsc smoke test =="
    "$BUILD/bin/jsc" -e 'print("jsc says: " + (1+1))' || echo "  jsc failed to run"
}

case "${1:-all}" in
    deps)       deps ;;
    instrument) instrument ;;
    collect)    require_patches; collect ;;
    build)      final_build ;;
    bolt)       require_patches; bolt ;;
    package)    package ;;
    verify)     verify ;;
    # BOLT is intentionally NOT in `all`: on this platform it regressed Speedometer 3 by ~6%
    # (measured) and its instrument mode corrupts the JIT (LLVM #165664). See bolt() / README.
    all)        instrument; collect; final_build; package; verify ;;
    *) echo "usage: $0 {deps|all|instrument|collect|build|bolt|package|verify}   (bolt = experimental, off by default)" >&2; exit 2 ;;
esac
