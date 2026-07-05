# Portable PGO + LTO Chromium build for aarch64 (Neoverse-N1 + Raspberry Pi 5)

A reproducible recipe that builds a **maximum-performance, portable** Chromium
from source on this native **aarch64** box and produces a binary that runs on
both the **Arm Neoverse-N1** build machine and a **Raspberry Pi 5 (Cortex-A76)**.

The whole thing is driven by [`build-chromium-pgo-linux.sh`](build-chromium-pgo-linux.sh).
Every phase is individually runnable; `all` runs them in order.

```
./build-chromium-pgo-linux.sh all          # full pipeline
./build-chromium-pgo-linux.sh fetch         # or run one phase at a time
./build-chromium-pgo-linux.sh toolchain
...
```

---

## What gets built

* **Chromium `150.0.7871.46`** (M150 stable — pinned git tag for reproducibility).
* **PGO** (Profile-Guided Optimization): a first *instrumented* build is run
  against **Speedometer 3** (Chromium's standard PGO benchmark; see note below)
  plus a handful of real top sites; the merged profile drives a second
  *optimized* build.
* **ThinLTO**: cross-module inlining/optimization (enabled by
  `is_official_build=true`, which is what Chrome releases ship).
* **Function-level symbols** (`symbol_level=1`) + **frame pointers**
  (`enable_frame_pointers=true`) — clean `perf`/profiler stacks on the target
  machines without the size/link blow-up of full line-level debug info.
* Tuned for the **ARMv8.2-A** ISA shared by Neoverse-N1 and Cortex-A76.

Deliverable: a relocatable tree `~/Development/chromium/chrome-arm64-pgo-lto/`
and a `.tar.zst` of it.

---

## The build host

* `wkdev64.moose` — Ubuntu 24.04, **aarch64 Neoverse-N1**, 80 cores, 125 GB RAM.
* This box **is itself the wkdev container** (nested podman/`wkdev-enter` won't
  work — build directly, as the WebKit production build does).
* `depot_tools` is cloned to `~/Development/depot_tools`.
* Chromium is checked out at `~/Development/chromium/src`.

---

## Why native-arm64 Chromium needs extra work

Chromium officially **cross-compiles** arm64 from an x86-64 host, so a native
aarch64 build hits several sharp edges that this recipe works around:

1. **The prebuilt toolchains are x86-64 only.** Chromium's `gclient` hooks
   normally download a prebuilt clang and rust that cannot execute on aarch64.
   We set `checkout_clang=False` / `checkout_rust=False` in `.gclient` and
   **build clang, rust and bindgen from source natively** (the `toolchain`
   phase). This is the long pole of the build (~2–4 h) but it is the only way
   to get a matching toolchain for `is_official_build` + PGO + ThinLTO.
2. **`node` is downloaded x86-64.** We replace it in place with the arm64 build.
3. **The clang/rust bootstrap scripts inject an amd64/arm64 *cross* sysroot**
   that breaks native config checks (e.g. libxml2). Three small vendored
   patches (`patches/arm64-*.patch`, from
   [jasonrandrews/build-chromium-linux-arm64](https://github.com/jasonrandrews/build-chromium-linux-arm64))
   disable that and add the `AArch64` LLVM target to the bootstrap.
4. **No reclient / goma on arm64** — `use_remoteexec=false`, `use_goma=false`.
   Everything is built locally (fine on 80 cores).
5. **gn's rust-revision assert** trips against a self-built rust; it is
   neutralized (`patch_rust_revision_check`).
6. **A CIPD host tool has no `linux-arm64` package.** `gclient sync`'s
   `cipd ensure` fails on `infra/3pp/tools/gperf/linux-arm64` (only amd64 is
   published). `patch_deps_arm64` sets that DEPS entry's condition to `False`
   and we symlink the system `gperf` into `third_party/gperf/cipd/bin/gperf`
   (the path Blink's `gperf_exe` expects). A `custom_deps: None` override does
   *not* work here — gclient aggregates all cipd deps into one ensure file.
7. **`.gclient` uses `"managed": False`.** We drive `src`'s own git (detached at
   the release tag, plus the gperf DEPS edit); `managed=False` stops gclient from
   rebasing/resetting/`git-clean`-ing `src` (which would revert the DEPS edit and,
   with a local branch, hit rebase conflicts). gclient then only syncs the
   DEPS-listed sub-dependencies. Sync is run **without `--reset`/`--force`**.
8. **`node` is pinned to a specific version** (read from
   `third_party/node/README.chromium`, e.g. 22.11.0) and there is a
   `check_version.py` gate, so we install exactly that arm64 build.

---

## Targeting **both** Neoverse-N1 and Raspberry Pi 5 — and portability

### ISA baseline
Both cores are **ARMv8.2-A** and both implement `dotprod`, `fp16`, `crc` and
`rcpc`, so we inject a single flag:

```
-mcpu=neoverse-n1+nocrypto
```

This sets the ARMv8.2-A baseline (dotprod/fp16/crc/rcpc) and tunes for N1, and
runs correctly on **either** machine.

* **Why `-mcpu`, not `-march` — this bit us and is the interesting part.**
  Chromium emits a **per-file `-march`** for SIMD translation units (e.g. libyuv
  compiles `row_sve.cc` with `-march=armv9-a+i8mm+sve2`). GN places a target's
  own `cflags` **before** the `config` `cflags`, and clang honours the **last**
  `-march` on the command line — so a *global* `-march` injected via a config
  lands last and **clobbers** those per-file arches, breaking the build with
  *"instruction requires: sve2 or sme"*. clang lets an explicit per-target
  `-march` override `-mcpu`'s architecture **regardless of order**, so `-mcpu`
  composes cleanly: SIMD TUs keep their `-march`, everything else gets the N1
  baseline + tuning. (Verified empirically before committing to the build.)
* **Crypto (AES/SHA/PMULL) is deliberately excluded** (`+nocrypto`). Sources
  disagree on whether the Pi 5 exposes the Arm crypto extensions, and enabling
  them risks the compiler emitting `PMULL`/`AES`/`SHA` that would `SIGILL` on a
  core that lacks them. Chromium's crypto (BoringSSL) and CRC/hashing paths do
  **runtime** CPU detection via `getauxval`, so **no performance is lost** — the
  hot paths still use hardware AES/PMULL at runtime *if the CPU has it*.
* Tuning for N1 only affects *scheduling*, so the Pi 5 still runs correctly.
  Change `MCPU` at the top of the script to `-mcpu=cortex-a76+nocrypto` if the
  Pi 5 is the machine you care most about.

### glibc floor (the real portability constraint)
A binary's minimum glibc is set by whatever it was linked against.

* Chromium's default `use_sysroot=true` with `target_cpu=arm64` builds against
  Chromium's bundled **Debian arm64 sysroot** (old glibc, ~2.31), giving a very
  low floor that runs on essentially any modern arm64 Linux, **including older
  Raspberry Pi OS**. This recipe uses that default when it works on the native
  host.
* If the sysroot misbehaves on the native host, the fallback is to build against
  the **host** libraries (Ubuntu 24.04 → glibc **2.39** floor). This session was
  configured for that case ("all target machines are modern, glibc ≥ 2.39").

---

## Pipeline phases

| Phase        | What it does | Rough time |
|--------------|--------------|-----------|
| `deps`       | `apt` host deps + Chromium `install-build-deps.sh --unsupported` | 5 min |
| `fetch`      | `fetch chromium` (full history), pin to the stable tag, `gclient sync` | 30–90 min |
| `runhooks`   | `gclient runhooks`, swap in arm64 `node`, purge x86 executables | 10–30 min |
| `toolchain`  | Build clang (bootstrap) + host libclang + rust + bindgen | 2–4 h |
| `instrument` | PGO phase 1: `chrome_pgo_phase=1` instrumented `chrome` | 1–3 h |
| `collect`    | Run Speedometer 3 + top sites → `chrome-arm64.profdata` | 20–40 min |
| `final`      | PGO phase 2: `chrome_pgo_phase=2` + ThinLTO + symbols + frame ptrs | 2–4 h |
| `package`    | Relocatable tree + `.tar.zst` + headless smoke test | 10 min |

Expect the full run to be an **overnight-scale** job.

---

## The GN configuration

Shared by both PGO phases (`emit_common_args` in the script):

```gn
target_cpu = "arm64"
host_cpu   = "arm64"
is_official_build = true      # ThinLTO + release opt + PGO machinery (what Chrome ships)
is_debug          = false
use_remoteexec = false        # no reclient on arm64
use_goma       = false
enable_nacl    = false        # deprecated; skip
use_custom_libcxx    = true
enable_rust          = true
rust_prebuilt_stdlib = false  # we built rust ourselves
is_cfi = false                # drop the control-flow-integrity runtime tax (perf)
```

The **target ISA** (`-mcpu=neoverse-n1+nocrypto`) is *not* an `args.gn` flag —
Chromium removed the global `extra_cflags` GN arg. It is injected by
`patch_arm64_march`, a small idempotent edit that appends the `cflags` to the
`current_cpu == "arm64"` branch of `config("compiler")` in
`build/config/compiler/BUILD.gn` (anchored on the unique `__ARM_NEON__=1` line).
See the ISA-baseline section above for why it's `-mcpu`, not `-march`.

Phase 1 (instrumented) adds:
```gn
chrome_pgo_phase = 1
symbol_level = 0                              # instrumented binary stays lean
enable_resource_allowlist_generation = false
```

Phase 2 (final) adds:
```gn
chrome_pgo_phase = 2
pgo_data_path    = "//chrome-arm64.profdata"
symbol_level       = 1        # function-level symbols
blink_symbol_level = 1        # ...including Blink (where much web time is spent)
v8_symbol_level    = 1
enable_frame_pointers = true
```

> All GN arg names above were validated against the checked-out M150 source; a
> quick `gn gen` on the instrumented dir catches any mismatch before a long build.

### Deliberate tradeoffs (flip these if you disagree)
* **`is_cfi=false`** — Control-Flow Integrity costs a few % and is a security
  hardening feature. We disable it for "as fast as possible". Re-enable for a
  security-hardened build.
* **`symbol_level=1` everywhere** — great profiling stacks, but a larger binary
  and slower link than `symbol_level=0`. Set to `0` for the smallest/fastest
  build, or `2` for full source-line debugging.
* **`proprietary_codecs`** left at the Chromium default (off). Enable
  `proprietary_codecs=true ffmpeg_branding="Chrome"` if you need H.264/AAC and
  have the licensing sorted.

---

## Running the result on the Pi 5 / another machine

```bash
scp chrome-arm64-pgo-lto.tar.zst pi@raspberrypi:
tar -I zstd -xf chrome-arm64-pgo-lto.tar.zst
./chrome-arm64-pgo-lto/chrome --version
```

If the sandbox complains on the target (no user-namespace support), run with
`--no-sandbox` or enable `kernel.unprivileged_userns_clone=1`.

---

## Reproducing from scratch

```bash
git clone https://chromium.googlesource.com/chromium/tools/depot_tools.git \
    ~/Development/depot_tools
cd ~/Development/chromium-pgo-build
./build-chromium-pgo-linux.sh all
```

Override anything via env, e.g. a different version or tune target:

```bash
CHROMIUM_TAG=151.0.xxxx.xx MTUNE=-mtune=cortex-a76 ./build-chromium-pgo-linux.sh all
```

---

## PGO profile: what's in it, and what isn't

The `collect` phase trains on **Speedometer 3** (run directly via Telemetry's
`run_benchmark`, served locally) plus four real top sites (Google, Wikipedia,
YouTube, GitHub). The merged profile is ~275 MB / ~27 billion counts, dominated
by Speedometer 3's JS/DOM/layout/paint workload.

Chromium's *full* standard PGO set also includes **jetstream2**, **system_health**
and **motionmark**, but on a normal machine those **fail** — their Telemetry page
sets pull a Web-Page-Replay archive from a **restricted** GCS bucket and error
with `CredentialsError: ... gcloud auth login` (Google-partner access only). So
they're deliberately excluded; Speedometer 3 is the single most representative
public browser-PGO benchmark, and the top sites add breadth. The script runs
Speedometer 3 *directly* (rather than `tools/pgo/generate_profile.py`, which
aborts the whole run when jetstream2 can't fetch its archive). If you *do* have
`gcloud` access, running `generate_profile.py --run-public-benchmarks-only` (or
the full set) would add jetstream2 (and more) to the profile.

## Provenance / credits

* Native-arm64 fetch/toolchain/hooks approach is adapted from
  **jasonrandrews/build-chromium-linux-arm64** (its patches didn't apply to
  M150, so the equivalent changes are re-expressed as M150-specific anchored
  source edits in `patch_toolchain_arm64`; the originals are under `patches/`).
* PGO flow follows Chromium's **`docs/pgo.md`**
  (`chrome_pgo_phase` 1 → collect → 2, `pgo_data_path`).
* The production PGO+LTO+symbols+ISA configuration and packaging are specific to
  this recipe.
