#!/bin/bash
#
# build-production-macos.sh
#
# Drives an open-source (no Apple-internal SDK) macOS Cocoa production WebKit
# build: instrument -> collect a PGO profile from Speedometer 3 / JetStream 3 /
# MotionMark -> rebuild with full LTO + the profile. See README-macOS.md for the
# full explanation, prerequisites, and the meaning of every flag.
#
# Usage:
#   ./build-production-macos.sh all          # instrument, collect, then final build
#   ./build-production-macos.sh instrument   # phase 1 only
#   ./build-production-macos.sh collect      # phase 2 only (needs phase 1's build)
#   ./build-production-macos.sh build        # phase 3 only (needs phase 2's profile)
#
# Environment knobs:
#   PROFILE_DIR    Where collected profiles are written. Default: WebKitBuild/PGO
#   FORCE_O3       "1" (default) forces -O3 on every framework. Set "0" to keep
#                  WebKit's tuned per-target levels (JSC -O3, WebCore -O2, WebKit -Os).
#   JS3_LOCAL_COPY If set, passed to run-benchmark as --local-copy to pin a fixed
#                  JetStream3 checkout (removes per-run clone/network variance).

set -euo pipefail

WEBKIT_ROOT="$(git -C "$(dirname "$0")" rev-parse --show-toplevel)"
cd "$WEBKIT_ROOT"

ARCH="$(uname -m)"                                   # arm64 or x86_64; matches Xcode $(arch)
BUILD_DIR="$WEBKIT_ROOT/WebKitBuild/Release"
PROFILE_DIR="${PROFILE_DIR:-$WEBKIT_ROOT/WebKitBuild/PGO}"
FORCE_O3="${FORCE_O3:-1}"
BENCHMARKS=(speedometer3 jetstream3 motionmark)

banner() { printf '\n\033[1m===== %s =====\033[0m\n\n' "$*"; }

MB_BUNDLE=org.webkit.MiniBrowser
RAISER_PID=""

# The benchmark driver raw-execs MiniBrowser (to inject DYLD_FRAMEWORK_PATH) and never
# activates it, so its page is marked hidden and requestAnimationFrame is throttled,
# making Speedometer/MotionMark degenerate. Keep MiniBrowser frontmost for the run and
# disable App Nap. Mirrors the raiser in wk-tools/quiesce.sh.
start_raiser() {
    if ! /usr/bin/python3 -c "import AppKit" >/dev/null 2>&1; then
        echo "warning: python3 AppKit unavailable; MiniBrowser will not be raised (SP3/MM may be degenerate)." >&2
        return
    fi
    /usr/bin/defaults write "$MB_BUNDLE" NSAppSleepDisabled -bool YES >/dev/null 2>&1 || true
    /usr/bin/python3 - "$MB_BUNDLE" >/dev/null 2>&1 <<'PY' &
import sys, time
from AppKit import (NSRunningApplication, NSWorkspace,
                    NSApplicationActivateIgnoringOtherApps)
bid = sys.argv[1]
while True:
    front = NSWorkspace.sharedWorkspace().frontmostApplication()
    if front is None or front.bundleIdentifier() != bid:
        for app in NSRunningApplication.runningApplicationsWithBundleIdentifier_(bid):
            app.activateWithOptions_(NSApplicationActivateIgnoringOtherApps)
    time.sleep(1)
PY
    RAISER_PID=$!
    echo "raiser started (pid $RAISER_PID) - keeping MiniBrowser frontmost during the run"
}

stop_raiser() {
    [[ -n "$RAISER_PID" ]] && kill "$RAISER_PID" 2>/dev/null || true
    RAISER_PID=""
    /usr/bin/defaults delete "$MB_BUNDLE" NSAppSleepDisabled >/dev/null 2>&1 || true
}

instrument() {
    banner "Phase 1: instrumented build (profile generation, thin LTO)"
    # ENABLE_LLVM_PROFILE_GENERATION bakes -fprofile-generate into every framework;
    # each writes .profraw to /private/tmp/WebKitPGO at runtime. This build is
    # throwaway, used only to collect a profile.
    #
    # OTHER_LDFLAGS=-fprofile-generate links the clang profile runtime into *every*
    # target. The per-framework xcconfigs already add it, but tool targets that
    # statically link the instrumented libWTF.a/libbmalloc.a (lldbWebKitTester,
    # TestWTF, ...) do not, and would otherwise fail to link with undefined
    # ___llvm_profile_instrument_* symbols, aborting the build before the frameworks.
    make release ASAN=NO WK_LTO_MODE=thin ENABLE_LLVM_PROFILE_GENERATION=ON \
        OTHER_LDFLAGS="-fprofile-generate"
}

collect() {
    banner "Phase 2: collect PGO profile from ${BENCHMARKS[*]}"
    echo "Benchmarks drive a GUI MiniBrowser, kept frontmost by the built-in raiser."
    echo "You still need an awake, attached, unlocked display; a locked/asleep screen"
    echo "throttles requestAnimationFrame and makes SP3/MotionMark degenerate."
    echo "For lower noise (and to pin JS3), run wk-tools/quiesce.sh alongside this."
    echo

    if [[ -e "$PROFILE_DIR" ]]; then
        echo "error: $PROFILE_DIR already exists; collect-pgo-profiles needs an empty dir." >&2
        echo "Remove it or set PROFILE_DIR to a fresh path." >&2
        exit 1
    fi

    start_raiser
    trap stop_raiser EXIT INT TERM

    # Note the ${extra[@]+...} guard below: macOS ships bash 3.2, where expanding an
    # empty array under `set -u` is itself an "unbound variable" error.
    local extra=()
    [[ -n "${JS3_LOCAL_COPY:-}" ]] && extra+=(--local-copy "$JS3_LOCAL_COPY")

    # collect-pgo-profiles runs each benchmark with --generate-pgo-profiles, merges
    # the per-dylib .profraw into {JavaScriptCore,WebCore,WebKit}.profdata, combines
    # them weighted (SP3 0.6 / JS3 0.2 / MM 0.2, see pgo-profile), and writes the
    # lzfse-compressed result to $PROFILE_DIR/$ARCH/*.profdata.compressed.
    Tools/Scripts/collect-pgo-profiles \
        --benchmarks "${BENCHMARKS[@]}" \
        --output-directory "$PROFILE_DIR" \
        --compressed-profile-sub-path "$ARCH" \
        --build-directory "$BUILD_DIR" \
        --browser minibrowser \
        ${extra[@]+"${extra[@]}"}

    stop_raiser
    trap - EXIT INT TERM

    echo
    echo "Compressed profile: $PROFILE_DIR/$ARCH/{JavaScriptCore,WebCore,WebKit}.profdata.compressed"
}

final_build() {
    banner "Phase 3: final production build (full LTO + PGO)"
    if [[ ! -d "$PROFILE_DIR/$ARCH" ]]; then
        echo "error: no profile at $PROFILE_DIR/$ARCH; run '$0 collect' first." >&2
        exit 1
    fi

    # A clean tree is required: the flags change (instrumentation off, PGO-use on,
    # full LTO) and the instrumented objects must not be reused.
    echo "Cleaning $WEBKIT_ROOT/WebKitBuild for a from-scratch optimized build..."
    rm -rf "$WEBKIT_ROOT/WebKitBuild/Release" "$WEBKIT_ROOT/WebKitBuild/Release.build"

    # WK_ENABLE_PGO_USE=YES forces the profile-use path on without the internal SDK.
    # WK_COMPRESSED_OPTIMIZATION_PROFILE_FOLDER points the existing "Copy Profiling
    # Data" build phase at our profiles; it decompresses $ARCH/<lib>.profdata.compressed
    # into each framework's DerivedSources and the xcconfig adds -fprofile-use.
    #
    # ENABLE_USER_SCRIPT_SANDBOXING=NO: the bmalloc/WTF/JavaScriptCore "Copy Profiling
    # Data" phases run under Xcode's script sandbox, which only grants their declared
    # inputs (arm64e/x86_64, the production arches) and denies reading our actual
    # $ARCH/*.profdata.compressed. WebCore/WebKit already exclude that phase from the
    # sandbox; this extends the same treatment to the lower libraries.
    # DEBUG_INFORMATION_FORMAT=dwarf-with-dsym: WebKit's engineering Release config uses
    # plain `dwarf` for the frameworks (debug info lives in the .build/*.o files, so it is
    # debuggable in place but NOT relocatable). Forcing dwarf-with-dsym emits real
    # *.framework.dSYM bundles so symbolication survives moving the build to another machine.
    local args="WK_ENABLE_PGO_USE=YES WK_COMPRESSED_OPTIMIZATION_PROFILE_FOLDER=$PROFILE_DIR ENABLE_USER_SCRIPT_SANDBOXING=NO DEBUG_INFORMATION_FORMAT=dwarf-with-dsym"
    [[ "$FORCE_O3" == "1" ]] && args="$args WK_DEFAULT_GCC_OPTIMIZATION_LEVEL=3"

    make release ASAN=NO WK_LTO_MODE=full \
        ARGS="$args" \
        EXTRA_CFLAGS="-fno-omit-frame-pointer"

    banner "Done"
    echo "Frameworks + MiniBrowser.app + dSYMs are in $BUILD_DIR"
    echo "Run:  Tools/Scripts/run-minibrowser --release"
    echo "  or: DYLD_FRAMEWORK_PATH=$BUILD_DIR $BUILD_DIR/jsc your.js"
}

case "${1:-all}" in
    instrument) instrument ;;
    collect)    collect ;;
    build)      final_build ;;
    all)        instrument; collect; final_build ;;
    *) echo "usage: $0 {all|instrument|collect|build}" >&2; exit 2 ;;
esac
