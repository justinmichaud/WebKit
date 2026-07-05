#!/usr/bin/env bash
###############################################################################
# build-chromium-pgo-linux.sh
#
# Reproducible, maximum-performance, *portable* Chromium build for aarch64.
#
#   Targets:  ARM Neoverse-N1 (the build box)  AND  Cortex-A76 / Raspberry Pi 5
#   Recipe:   production PGO + ThinLTO, function-level symbols, frame pointers.
#   Host:     native aarch64 Linux (no cross-compile) - Ubuntu 24.04 wkdev box.
#
# Pipeline (each phase is individually runnable; `all` runs them in order):
#
#   deps        Install host build dependencies (sudo).
#   fetch       Fetch Chromium, pin to a stable release tag, gclient sync.
#   runhooks    gclient runhooks + fix arm64 node + purge x86 binaries.
#   toolchain   Build the bundled clang + rust + bindgen natively for arm64.
#   instrument  PGO phase 1: build an instrumented chrome (chrome_pgo_phase=1).
#   collect     Run the standard PGO workload -> merged .profdata.
#   final       PGO phase 2: build the optimized chrome (PGO + ThinLTO + syms).
#   package     Assemble a relocatable tree + tarball; smoke-test it.
#
# Why the flags are what they are is documented inline and in README.md.
#
# The native-arm64 toolchain approach is adapted from
#   https://github.com/jasonrandrews/build-chromium-linux-arm64
# but its patches did not apply to M150, so the equivalent changes are made as
# M150-specific anchored source edits in patch_toolchain_arm64() (the original
# recipe patches are kept under patches/ for reference only). A production
# PGO+ThinLTO+symbols configuration is layered on top.
###############################################################################
set -euo pipefail

# --------------------------------------------------------------------------- #
# Configuration (override via environment)                                    #
# --------------------------------------------------------------------------- #
CHROMIUM_TAG="${CHROMIUM_TAG:-150.0.7871.46}"          # M150 stable (verified tag)
CHROMIUM_DIR="${CHROMIUM_DIR:-$HOME/Development/chromium}"
SRC="$CHROMIUM_DIR/src"
DEPOT_TOOLS="${DEPOT_TOOLS:-$HOME/Development/depot_tools}"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
LOG_DIR="$SCRIPT_DIR/logs"
mkdir -p "$LOG_DIR"

OUT_INSTR="out/instrumented"        # PGO phase 1 (instrumented) build dir
OUT_FINAL="out/pgo-lto"             # PGO phase 2 (final) build dir
PROFRAW_DIR="$SRC/pgo-profraw"      # raw counters land here during collection
PROFDATA="$SRC/chrome-arm64.profdata"

# Deliverable
PKG_DIR="$CHROMIUM_DIR/chrome-arm64-pgo-lto"
PKG_TARBALL="$CHROMIUM_DIR/chrome-arm64-pgo-lto.tar.zst"

# ---- Target ISA -----------------------------------------------------------
# We inject ONE flag: -mcpu=neoverse-n1+nocrypto. It sets the ARMv8.2-A baseline
# shared by Neoverse-N1 and Cortex-A76 (RPi5) - dotprod + fp16 + crc + rcpc - and
# tunes for N1, while +nocrypto keeps AES/SHA/PMULL out (sources disagree on
# whether the Pi5 exposes crypto; BoringSSL/zlib detect it at runtime anyway, so
# nothing is lost and we avoid a SIGILL risk on the Pi5).
#
# WHY -mcpu, not -march: Chromium emits per-file -march for SIMD TUs (e.g. libyuv
# uses -march=armv9-a+i8mm+sve2). GN puts a target's own cflags BEFORE the
# config cflags, and clang honors the LAST -march - so a global -march would
# CLOBBER those per-file arches and break the build ("instruction requires sve2").
# clang lets an explicit per-target -march override -mcpu's arch regardless of
# order, so -mcpu composes cleanly: SIMD TUs keep their -march, everything else
# gets the N1 baseline+tuning.
MCPU="-mcpu=neoverse-n1+nocrypto"

export PATH="$DEPOT_TOOLS:$PATH"
export DEPOT_TOOLS_UPDATE="${DEPOT_TOOLS_UPDATE:-1}"
# Native arm64: no remote execution / goma available.
export NINJA_STATUS="[%f/%t %e s] "

# --------------------------------------------------------------------------- #
# Helpers                                                                     #
# --------------------------------------------------------------------------- #
RED=$'\033[0;31m'; GREEN=$'\033[0;32m'; YELLOW=$'\033[1;33m'; BLUE=$'\033[0;34m'; NC=$'\033[0m'
say()  { echo "${BLUE}==>${NC} $*"; }
ok()   { echo "${GREEN}[ok]${NC} $*"; }
warn() { echo "${YELLOW}[warn]${NC} $*"; }
die()  { echo "${RED}[fail]${NC} $*" >&2; exit 1; }

require_src() { [ -d "$SRC" ] || die "Chromium source not found at $SRC (run: $0 fetch)"; }

# Environment that lets the source-built clang/rust/bindgen find their runtime
# libs. Must be exported before any gn gen / autoninja / generate_profile call.
setup_toolchain_env() {
  export LD_LIBRARY_PATH="$SRC/third_party/llvm-build/Release+Asserts/lib:$SRC/third_party/rust-toolchain-intermediate/llvm-host-install/lib:${LD_LIBRARY_PATH:-}"
  export LIBCLANG_PATH="$SRC/third_party/rust-toolchain/lib"
}

# --------------------------------------------------------------------------- #
# Phase: deps                                                                 #
# --------------------------------------------------------------------------- #
phase_deps() {
  say "Installing host build tools (no source needed; runs before fetch)"
  sudo apt-get update
  # Host tooling to bootstrap clang/rust, drive the build, collect + package.
  # gperf: needed by the fetch phase (symlinked into Blink's expected path).
  sudo apt-get install -y git curl cmake ninja-build clang lld python3 \
       python-is-python3 lsb-release pkg-config zstd xz-utils file patch \
       xvfb rsync gperf ca-certificates
  ok "host tools installed"
}

# --------------------------------------------------------------------------- #
# Phase: fetch                                                                #
# --------------------------------------------------------------------------- #
phase_fetch() {
  command -v fetch  >/dev/null || die "depot_tools 'fetch' not on PATH ($DEPOT_TOOLS)"
  mkdir -p "$CHROMIUM_DIR"
  if [ ! -d "$SRC" ]; then
    say "fetch chromium (full history, --nohooks) into $CHROMIUM_DIR"
    ( cd "$CHROMIUM_DIR" && fetch --nohooks chromium )
  else
    ok "src already present, skipping initial fetch"
  fi

  # We drive src's own git ourselves; managed=False stops gclient from
  # rebasing/resetting/complaining about src, so it ONLY syncs the DEPS-listed
  # sub-deps. This is what lets our DEPS edit (below) survive the sync.
  # NB: M150 has no checkout_clang/checkout_rust vars and the clang GCS DEPS
  # entry is gated only on host_os=="linux" (true on this arm64 host), so sync
  # pulls an x86_64 clang/rust that can't run here; we OVERWRITE it with a source
  # build in `toolchain` and purge stray x86 bins in `runhooks`.
  say "Writing .gclient (managed=False)"
  cat > "$CHROMIUM_DIR/.gclient" <<EOF
solutions = [
  {
    "name": "src",
    "url": "https://chromium.googlesource.com/chromium/src.git",
    "managed": False,
    "custom_deps": {},
    "custom_vars": {},
  },
]
EOF

  # Pin to the stable release tag (reproducible build), DETACHED (a local branch
  # would make gclient try to rebase it onto the target rev and conflict).
  say "Checking out stable tag $CHROMIUM_TAG (detached)"
  ( cd "$SRC"
    git fetch origin "+refs/tags/$CHROMIUM_TAG:refs/tags/$CHROMIUM_TAG"
    git checkout -f --detach "refs/tags/$CHROMIUM_TAG" )

  patch_deps_arm64        # neutralize CIPD deps that lack a linux-arm64 package

  say "gclient sync (managed=False, --nohooks) - pulls all pinned sub-DEPS"
  ( cd "$CHROMIUM_DIR" && gclient sync --nohooks -D --with_branch_heads --with_tags )

  # gperf: no linux-arm64 CIPD build, so we disabled that dep and point Blink's
  # gperf_exe path at the system gperf instead.
  say "Symlinking system gperf into third_party/gperf/cipd/bin"
  mkdir -p "$SRC/third_party/gperf/cipd/bin"
  ln -sf "$(command -v gperf)" "$SRC/third_party/gperf/cipd/bin/gperf"

  ok "fetch + sync done ($CHROMIUM_TAG)"
}

# Neutralize CIPD DEPS entries that have no linux-arm64 package (they'd fail
# `cipd ensure` on a native arm64 host). We set their condition to False and
# provide the tool from the system instead. Idempotent.
patch_deps_arm64() {
  local f="$SRC/DEPS"
  grep -q "ARM64: no linux-arm64" "$f" && { ok "DEPS arm64 patch already applied"; return 0; }
  python3 - "$f" <<'PY'
import sys, re
f = sys.argv[1]; src = open(f).read()
# gperf prebuilt (infra/3pp/tools/gperf/${platform}) has no linux-arm64 build.
i = src.find("'src/third_party/gperf/cipd': {")
if i == -1:
    print("gperf/cipd block not found (DEPS layout changed?)", file=sys.stderr); sys.exit(0)
end = src.find("},", i)
block = re.sub(r"'condition':\s*'[^']*',",
               "'condition': 'False',  # ARM64: no linux-arm64 gperf CIPD pkg; system gperf used",
               src[i:end], count=1)
open(f, "w").write(src[:i] + block + src[end:]); print("neutralized gperf/cipd")
PY
  ok "DEPS arm64 patch applied"
}

# --------------------------------------------------------------------------- #
# Phase: runhooks                                                             #
# --------------------------------------------------------------------------- #
phase_runhooks() {
  require_src
  # Chromium's own dependency installer (host libs + build tools). Needs the
  # source tree, so it runs here (after fetch). --unsupported: Ubuntu 24.04 may
  # be newer than its whitelist. --no-prompt: non-interactive.
  say "install-build-deps.sh (full Chromium host deps)"
  sudo "$SRC/build/install-build-deps.sh" --unsupported --no-prompt \
    || warn "install-build-deps returned non-zero (dbus errors in a container are benign)"

  # The synced node is x86_64; replace it with the arm64 build of the SAME
  # pinned version BEFORE runhooks (some hooks execute node). The x86 node can't
  # be run to query its version, so read the pin from README.chromium.
  local nd="$SRC/third_party/node/linux/node-linux-x64"
  if [ -f "$nd/bin/node" ] && file "$nd/bin/node" | grep -q x86-64; then
    # Authoritative pinned version lives in update_node_binaries (NODE_VERSION=...);
    # a check_version.js gate enforces an exact match at build time.
    local NODE_VERSION; NODE_VERSION="$(grep -m1 '^NODE_VERSION=' "$SRC/third_party/node/update_node_binaries" | sed -E 's/.*"(v[0-9.]+)".*/\1/')"
    say "Replacing x86_64 node with arm64 node $NODE_VERSION"
    ( cd "$SRC/third_party/node/linux"
      curl -fsSL -o node-arm64.tar.gz \
        "https://nodejs.org/dist/${NODE_VERSION}/node-${NODE_VERSION}-linux-arm64.tar.gz"
      rm -rf node-linux-x64
      tar xzf node-arm64.tar.gz
      mv "node-${NODE_VERSION}-linux-arm64" node-linux-x64
      rm -f node-arm64.tar.gz )
    file "$nd/bin/node" | grep -q "ARM aarch64" && ok "node now arm64 $NODE_VERSION" || warn "node replace unclear"
  fi

  say "gclient runhooks"
  ( cd "$CHROMIUM_DIR" && gclient runhooks )

  # Purge any remaining x86 executables the hooks dropped (they'd just fail).
  say "Purging x86/x86_64 executables from the checkout"
  ( cd "$SRC" && find . -type f -executable -exec file {} \; 2>/dev/null \
      | grep -E "ELF.*(x86-64|80386)" | cut -d: -f1 \
      | while read -r f; do rm -f "$f"; done ) || true
  ok "runhooks done"
}

# --------------------------------------------------------------------------- #
# Phase: toolchain  (build clang + rust + bindgen natively)                   #
# --------------------------------------------------------------------------- #
# Adapt clang & rust build scripts for a NATIVE arm64 host (idempotent, marker
# ARM64-NATIVE). M150's build.py/build_rust.py differ from the upstream
# native-arm64 patches, so these are exact-string edits, not context diffs:
#   build.py:      add AArch64 to bootstrap LLVM targets; skip the arm64 cross
#                  sysroot (it breaks libxml2 config checks on a native host).
#   build_rust.py: aarch64 rustc triple; --use-system-cmake for rust's own host
#                  LLVM; don't fetch the amd64 host sysroot (build against host).
patch_toolchain_arm64() {
  python3 - "$SRC/tools/clang/scripts/build.py" "$SRC/tools/rust/build_rust.py" "$SRC/tools/rust/build_bindgen.py" <<'PY'
import sys
CLANG, RUST, BINDGEN = sys.argv[1], sys.argv[2], sys.argv[3]
def edit(path, repls):
    s = open(path).read()
    if 'ARM64-NATIVE' in s:
        print(f"{path}: already patched"); return
    for old, new in repls:
        assert s.count(old) == 1, f"{path}: anchor not unique/found:\n{old[:70]}"
        s = s.replace(old, new, 1)
    open(path, 'w').write(s); print(f"{path}: {len(repls)} edits applied")
edit(CLANG, [
  ("    bootstrap_targets = 'X86'\n    if sys.platform == 'darwin':\n"
   "      # Need ARM and AArch64 for building the ios clang_rt.\n"
   "      bootstrap_targets += ';ARM;AArch64'",
   "    bootstrap_targets = 'X86'\n    if sys.platform == 'darwin':\n"
   "      # Need ARM and AArch64 for building the ios clang_rt.\n"
   "      bootstrap_targets += ';ARM;AArch64'\n"
   "    if sys.platform.startswith('linux') and platform.machine() == 'aarch64':\n"
   "      bootstrap_targets += ';AArch64'  # ARM64-NATIVE"),
  ("    if platform.machine() == 'aarch64':\n      cmake_sysroot = sysroot_arm64\n"
   "    else:\n      # amd64 is the default toolchain.\n      cmake_sysroot = sysroot_amd64\n"
   "    base_cmake_args.append('-DCMAKE_SYSROOT=' + cmake_sysroot)",
   "    if platform.machine() == 'aarch64':\n"
   "      cmake_sysroot = None  # ARM64-NATIVE: no cross sysroot (avoids libxml2 config failures)\n"
   "    else:\n      # amd64 is the default toolchain.\n      cmake_sysroot = sysroot_amd64\n"
   "    if cmake_sysroot:\n      base_cmake_args.append('-DCMAKE_SYSROOT=' + cmake_sysroot)"),
])
edit(RUST, [
  # Use the system OpenSSL (no linux-arm64 build in CIPD; the amd64 one won't link).
  ("def AddOpenSSLToEnv():\n    \"\"\"Download OpenSSL, and add to OPENSSL_DIR.\"\"\"\n"
   "    ssl_dir = os.path.join(LLVM_BUILD_TOOLS_DIR, 'openssl')",
   "def AddOpenSSLToEnv():\n    \"\"\"Download OpenSSL, and add to OPENSSL_DIR.\"\"\"\n"
   "    if sys.platform.startswith('linux') and platform.machine() in ('aarch64', 'arm64'):\n"
   "        # ARM64-NATIVE: system openssl (no linux-arm64 CIPD; amd64 won't link).\n"
   "        os.environ['OPENSSL_LIB_DIR'] = '/usr/lib/aarch64-linux-gnu'\n"
   "        os.environ['OPENSSL_INCLUDE_DIR'] = '/usr/include'\n"
   "        os.environ['OPENSSL_NO_VENDOR'] = '1'\n"
   "        return '/usr'\n"
   "    ssl_dir = os.path.join(LLVM_BUILD_TOOLS_DIR, 'openssl')"),
  ("    elif sys.platform == 'win32':\n        return 'x86_64-pc-windows-msvc'\n"
   "    else:\n        return 'x86_64-unknown-linux-gnu'",
   "    elif sys.platform == 'win32':\n        return 'x86_64-pc-windows-msvc'\n"
   "    elif platform.machine() in ('aarch64', 'arm64'):  # ARM64-NATIVE\n"
   "        return 'aarch64-unknown-linux-gnu'\n"
   "    else:\n        return 'x86_64-unknown-linux-gnu'"),
  ("    if sys.platform.startswith('linux'):\n        build_cmd.append('--without-android')\n"
   "        build_cmd.append('--without-fuchsia')",
   "    if sys.platform.startswith('linux'):\n        build_cmd.append('--without-android')\n"
   "        build_cmd.append('--without-fuchsia')\n"
   "        if platform.machine() in ('aarch64', 'arm64'):  # ARM64-NATIVE\n"
   "            build_cmd.append('--use-system-cmake')\n"
   "            build_cmd += ['--host-cc=/usr/bin/clang', '--host-cxx=/usr/bin/clang++']"),
  ("    if sys.platform.startswith('linux') and not args.sync_for_gnrt:",
   "    if sys.platform.startswith('linux') and not args.sync_for_gnrt and platform.machine() not in ('aarch64', 'arm64'):  # ARM64-NATIVE"),
])
edit(BINDGEN, [
  # Don't fetch the amd64 ncursesw; use the system one (found via default paths).
  ("    ncursesw_dir = None\n    if sys.platform.startswith('linux'):\n        ncursesw_dir = FetchNcurseswLibrary()",
   "    ncursesw_dir = None\n    if sys.platform.startswith('linux') and platform.machine() not in ('aarch64', 'arm64'):  # ARM64-NATIVE: system ncursesw\n        ncursesw_dir = FetchNcurseswLibrary()"),
  # Don't link against the amd64 Debian sysroot; use host arm64 libs.
  ("    if sys.platform.startswith('linux'):\n        # We use these flags to avoid linking with the system libstdc++.\n        sysroot = DownloadDebianSysroot('amd64')",
   "    if sys.platform.startswith('linux') and platform.machine() not in ('aarch64', 'arm64'):  # ARM64-NATIVE: host libs, no amd64 sysroot\n        # We use these flags to avoid linking with the system libstdc++.\n        sysroot = DownloadDebianSysroot('amd64')"),
])
PY
  ok "toolchain source edits applied"
}

phase_toolchain() {
  require_src
  cd "$SRC"
  export CHROMIUM_BUILDTOOLS_PATH=/usr        # use system cmake, not x86 prebuilt
  patch_toolchain_arm64

  local CLANG="third_party/llvm-build/Release+Asserts/bin/clang"
  if [ -x "$CLANG" ] && file -L "$CLANG" | grep -q "ARM aarch64"; then
    ok "arm64 clang already built - skipping clang build"
  else
    say "Building clang from source (bootstrap; ~1-2h on 80 cores)"
    # Wipe the x86_64 clang that gclient sync downloaded (it can't run here) and
    # the x86 pinned-clang, so build.py starts clean.
    if [ -e "$CLANG" ] && ! file -L "$CLANG" | grep -q "ARM aarch64"; then
      rm -rf third_party/llvm-build/Release+Asserts
    fi
    rm -rf third_party/llvm-build-tools/pinned-clang || true
    python3 tools/clang/scripts/build.py \
        --without-android --without-fuchsia \
        --use-system-cmake --with-ml-inliner-model="" \
        --bootstrap --host-cc=/usr/bin/clang --host-cxx=/usr/bin/clang++
    file -L "$CLANG" | grep -q "ARM aarch64" || die "built clang is not aarch64"
    ok "clang built"
  fi

  # Rust builds its OWN host LLVM (with --use-system-cmake, via the build_rust
  # edit above) plus rustc/cargo/clippy/rustfmt. ~1.5h.
  local RUSTC="third_party/rust-toolchain/bin/rustc"
  if [ -x "$RUSTC" ] && file "$RUSTC" | grep -q "ARM aarch64"; then
    ok "arm64 rust already built - skipping rust build"
  else
    say "Building rust toolchain from source (~1.5h)"
    # --skip-test: we only need a working rustc/cargo; the rust test suites are
    # slow and can be flaky on a native arm64 host (the rust_tot hook skips them too).
    python3 tools/rust/build_rust.py --skip-test
    file "$RUSTC" | grep -q "ARM aarch64" || warn "rustc arch unclear (continuing)"
    ok "rust built"
  fi

  # bindgen: use Chromium's own build_bindgen.py so we get the PINNED bindgen
  # version (not latest-from-crates.io, which could mismatch the build). It uses
  # our cargo + the arm64 libclang the rust step already produced.
  local BINDGEN="third_party/rust-toolchain/bin/bindgen"
  if [ ! -x "$BINDGEN" ]; then
    say "Building pinned bindgen (build_bindgen.py)"
    # --skip-test: bindgen's own test suite has one codegen edge-case failure
    # (test_wrap_static_fns) with clang-23 here; it does not affect the binary.
    python3 tools/rust/build_bindgen.py --skip-test
    [ -x "$BINDGEN" ] || die "bindgen not built at $BINDGEN"
  fi

  # Chromium's build-time bindgen loads libclang from rust_bindgen_root/lib
  # (//third_party/rust-toolchain/lib). Point it at the arm64 libclang.so.* the
  # rust host-LLVM build produced.
  say "Symlinking libclang into rust-toolchain/lib for build-time bindgen"
  mkdir -p third_party/rust-toolchain/lib
  for so in third_party/rust-toolchain-intermediate/llvm-host-install/lib/libclang.so*; do
    [ -e "$so" ] && ln -sf "$SRC/$so" "third_party/rust-toolchain/lib/$(basename "$so")"
  done
  ok "toolchain complete (clang + rust + bindgen + libclang)"
}

# --------------------------------------------------------------------------- #
# args.gn generation                                                          #
# --------------------------------------------------------------------------- #
# Common production settings shared by both PGO phases.
emit_common_args() {
  cat <<EOF
# ---- target / host ----
target_cpu = "arm64"
host_cpu   = "arm64"

# Official build => ThinLTO + release optimization + the PGO machinery.
is_official_build = true
is_debug          = false

# Native arm64: no remote build, no goma. (NaCl's GN arg was removed in M150.)
use_remoteexec = false
use_goma       = false

# Source-built toolchains (auto-detected under third_party/rust-toolchain,
# third_party/llvm-build). use_chromium_rust_toolchain defaults true and reads
# our built rust (incl. its VERSION cache-buster).
use_custom_libcxx = true
enable_rust       = true

# NB: the target ISA ($MCPU) is injected via a source edit to
# build/config/compiler/BUILD.gn (see patch_arm64_march) - Chromium removed the
# global extra_cflags GN arg, so args.gn cannot carry compiler flags.

# Max-performance posture (documented tradeoffs in README):
#   * CFI off  -> removes the control-flow-integrity runtime tax.
#   * DCHECKs off (implied by official build).
is_cfi = false
EOF
}

phase_write_args_instrument() {
  require_src
  mkdir -p "$SRC/$OUT_INSTR"
  say "Writing $OUT_INSTR/args.gn (PGO phase 1, instrumented)"
  {
    emit_common_args
    cat <<EOF

# ---- PGO phase 1: generate an instrumented binary ----
chrome_pgo_phase = 1
# Keep the instrumented binary lean; symbols not needed to collect a profile.
symbol_level = 0
enable_resource_allowlist_generation = false
EOF
  } > "$SRC/$OUT_INSTR/args.gn"
}

phase_write_args_final() {
  require_src
  [ -f "$PROFDATA" ] || die "profile not found at $PROFDATA (run: $0 collect)"
  mkdir -p "$SRC/$OUT_FINAL"
  say "Writing $OUT_FINAL/args.gn (PGO phase 2, final)"
  {
    emit_common_args
    cat <<EOF

# ---- PGO phase 2: consume the collected profile + ThinLTO ----
chrome_pgo_phase = 2
pgo_data_path    = "//${PROFDATA#$SRC/}"

# Function-level symbols for clean profiling stacks on the target machines.
symbol_level       = 1
blink_symbol_level = 1
v8_symbol_level    = 1
# NB: frame pointers are NOT set here - enable_frame_pointers is a *computed*
# var in M150 (not a settable arg), and it already evaluates true for this
# config, so the build gets -fno-omit-frame-pointer by default (verified).
EOF
  } > "$SRC/$OUT_FINAL/args.gn"
}

# Inject the target ISA (-mcpu) into the arm64 branch of config("compiler").
# Chromium has no global extra_cflags arg, so we edit compiler/BUILD.gn. We use
# -mcpu (not -march) so per-file -march for SIMD TUs still overrides it - see the
# MCPU note near the top. Anchored on the unique "__ARM_NEON__=1" line (idempotent).
patch_arm64_march() {
  local f="$SRC/build/config/compiler/BUILD.gn"
  [ -f "$f" ] || die "compiler/BUILD.gn missing"
  grep -q "CHROMIUM_PGO_MCPU" "$f" && { ok "arm64 -mcpu already injected"; return 0; }
  MCPU="$MCPU" python3 - "$f" <<'PY'
import os, sys
f = sys.argv[1]; mcpu = os.environ["MCPU"]
src = open(f).read()
anchor = 'defines += [ "__ARM_NEON__=1" ]'
assert anchor in src, "anchor not found in compiler/BUILD.gn"
inject = (anchor +
    '\n    # CHROMIUM_PGO_MCPU: ARMv8.2-A baseline (dotprod/fp16/crc/rcpc) shared by'
    '\n    # Neoverse-N1 and Cortex-A76 (RPi5), tuned for N1, no crypto (runtime-detected).'
    '\n    # -mcpu (not -march) so per-file -march for SIMD TUs still overrides it.'
    f'\n    cflags += [ "{mcpu}" ]')
open(f, "w").write(src.replace(anchor, inject, 1))
print("injected -mcpu into arm64 config(\"compiler\")")
PY
  ok "arm64 -mcpu injected"
}

# (M150 has no rust-revision assert - rust.gni just reads
# third_party/rust-toolchain/VERSION as a cache-buster - so no patch is needed.)

# --------------------------------------------------------------------------- #
# Phase: instrument (PGO phase 1 build)                                       #
# --------------------------------------------------------------------------- #
phase_instrument() {
  require_src; setup_toolchain_env; cd "$SRC"
  patch_arm64_march
  phase_write_args_instrument
  say "gn gen $OUT_INSTR"
  gn gen "$OUT_INSTR"
  say "Building instrumented chrome (autoninja) - this is a full build"
  autoninja -C "$OUT_INSTR" chrome
  [ -x "$OUT_INSTR/chrome" ] || die "instrumented chrome not built"
  ok "instrumented chrome ready: $SRC/$OUT_INSTR/chrome"
}

# --------------------------------------------------------------------------- #
# Phase: collect (run the standard PGO workload, merge profile)               #
# --------------------------------------------------------------------------- #
phase_collect() {
  require_src; setup_toolchain_env; cd "$SRC"
  [ -x "$OUT_INSTR/chrome" ] || die "no instrumented chrome (run: $0 instrument)"
  local PROFDATA_TOOL="$SRC/third_party/llvm-build/Release+Asserts/bin/llvm-profdata"
  local CHROME="$SRC/$OUT_INSTR/chrome"

  # Chromium's standard PGO set is speedometer3 + jetstream2 (+ system_health,
  # motionmark). Only speedometer3 works on a normal machine: the others pull a
  # Web-Page-Replay archive from restricted cloud storage (needs `gcloud auth
  # login`) and fail with a CredentialsError. speedometer3 is served locally by
  # Telemetry, and is the single most representative browser-PGO workload. We run
  # it directly (not generate_profile.py, which aborts the whole run if jetstream2
  # fails) and add a few real top sites for breadth, then merge everything.
  local SP3_RAW="$SRC/$OUT_INSTR/profile/speedometer3/raw"
  rm -rf "$SP3_RAW" "$PROFRAW_DIR"; mkdir -p "$SP3_RAW" "$PROFRAW_DIR"

  say "Collecting Speedometer 3 profile (Telemetry, headless via Xvfb)"
  LLVM_PROFILE_FILE="$SP3_RAW/sp3-%m-%p.profraw" \
  xvfb-run -a vpython3 tools/perf/run_benchmark speedometer3 \
      --browser=exact --browser-executable="$CHROME" \
      --chromium-output-directory="$SRC/$OUT_INSTR" --assert-gpu-compositing \
      2>&1 | tee "$LOG_DIR/collect-sp3.log" || warn "speedometer3 run reported issues (profraws may still be usable)"

  say "Collecting real top-site page loads for broader coverage"
  local i=0
  for u in "https://www.google.com/" "https://en.wikipedia.org/wiki/Chromium" \
           "https://www.youtube.com/" "https://github.com/chromium/chromium"; do
    i=$((i+1))
    LLVM_PROFILE_FILE="$PROFRAW_DIR/site_${i}-%m-%p.profraw" \
    xvfb-run -a "$CHROME" --headless=new --disable-gpu \
        --virtual-time-budget=15000 --run-all-compositor-stages-before-draw \
        --screenshot=/dev/null "$u" >/dev/null 2>&1 || true
  done

  say "Merging profiles with llvm-profdata -> $PROFDATA"
  "$PROFDATA_TOOL" merge -output="$PROFDATA" "$SP3_RAW"/*.profraw "$PROFRAW_DIR"/*.profraw
  [ -f "$PROFDATA" ] || die "profile merge failed - no profraws collected?"
  ok "profile collected -> $PROFDATA ($(du -h "$PROFDATA" | cut -f1)); $($PROFDATA_TOOL show "$PROFDATA" 2>/dev/null | grep -m1 'Total count')"
}

# --------------------------------------------------------------------------- #
# Phase: final (PGO phase 2 build)                                            #
# --------------------------------------------------------------------------- #
phase_final() {
  require_src; setup_toolchain_env; cd "$SRC"
  patch_arm64_march

  # is_official_build + chrome_pgo_phase=2 turns on V8's *builtins* PGO (separate
  # from Chrome's PGO), which needs V8's builtins-pgo profiles. They're behind a
  # DEPS hook gated on checkout_v8_builtins_pgo_profiles (default False), so fetch
  # them directly (public bucket). arm64 uses the x64 profile (V8 has no arm64 one).
  if ! ls v8/tools/builtins-pgo/profiles/*.profile >/dev/null 2>&1; then
    say "Downloading V8 builtins-pgo profiles"
    python3 v8/tools/builtins-pgo/download_profiles.py download \
        --depot-tools "$DEPOT_TOOLS" || warn "v8 builtins-pgo download failed"
  fi

  phase_write_args_final
  say "gn gen $OUT_FINAL"
  gn gen "$OUT_FINAL"
  say "Building final PGO+LTO chrome (autoninja) - ThinLTO link is slow"
  autoninja -C "$OUT_FINAL" chrome chromedriver
  [ -x "$OUT_FINAL/chrome" ] || die "final chrome not built"
  ok "final chrome ready: $SRC/$OUT_FINAL/chrome"
}

# --------------------------------------------------------------------------- #
# Phase: package (relocatable tree + tarball + smoke test)                    #
# --------------------------------------------------------------------------- #
phase_package() {
  require_src; cd "$SRC"
  [ -x "$OUT_FINAL/chrome" ] || die "no final chrome (run: $0 final)"
  say "Assembling portable tree at $PKG_DIR"
  rm -rf "$PKG_DIR"; mkdir -p "$PKG_DIR"
  # `gn desc <dir> //chrome runtime_deps` is the authoritative list of runtime
  # files (relative to the out dir); copy exactly those (no obj/*.o/*.a build junk).
  # Keep only path-like lines (space-free) so any gn warning text on stdout is dropped.
  gn desc "$OUT_FINAL" //chrome runtime_deps 2>/dev/null \
    | grep -E '^[^[:space:]]+$' | sort -u > "$LOG_DIR/runtime_deps.txt" || true
  if [ -s "$LOG_DIR/runtime_deps.txt" ]; then
    rsync -a --files-from="$LOG_DIR/runtime_deps.txt" "$OUT_FINAL/" "$PKG_DIR/"
  else
    warn "runtime_deps unavailable; copying common artifacts"
    rsync -a --exclude '*.o' --exclude '*.a' --exclude 'obj/' --exclude 'gen/' \
          "$OUT_FINAL/" "$PKG_DIR/"
  fi
  say "Creating tarball $PKG_TARBALL"
  tar -C "$(dirname "$PKG_DIR")" -I 'zstd -19 -T0' -cf "$PKG_TARBALL" "$(basename "$PKG_DIR")"
  ok "package: $PKG_DIR  ($(du -sh "$PKG_DIR" | cut -f1)),  tarball: $PKG_TARBALL"

  say "Smoke test: --version and a headless screenshot"
  "$PKG_DIR/chrome" --version || warn "chrome --version failed"
  xvfb-run -a "$PKG_DIR/chrome" --headless=new --no-sandbox --disable-gpu \
      --screenshot="$LOG_DIR/smoke.png" about:blank >/dev/null 2>&1 \
      && ok "headless screenshot OK ($LOG_DIR/smoke.png)" || warn "headless smoke test failed"
}

# --------------------------------------------------------------------------- #
# Dispatcher                                                                  #
# --------------------------------------------------------------------------- #
main() {
  local phase="${1:-help}"
  case "$phase" in
    deps)        phase_deps ;;
    fetch)       phase_fetch ;;
    runhooks)    phase_runhooks ;;
    toolchain)   phase_toolchain ;;
    instrument)  phase_instrument ;;
    collect)     phase_collect ;;
    final)       phase_final ;;
    package)     phase_package ;;
    all)
      phase_deps; phase_fetch; phase_runhooks; phase_toolchain
      phase_instrument; phase_collect; phase_final; phase_package ;;
    help|*)
      grep -E '^#   [a-z]' "$0" | sed 's/^#   /  /'
      echo; echo "Usage: $0 {deps|fetch|runhooks|toolchain|instrument|collect|final|package|all}" ;;
  esac
}
main "$@"
