# Validate the mya_heap build-config claims on Linux (dev/mya-heap-walk-with-uuid)

Work in a Linux wk workspace on the branch `dev/mya-heap-walk-with-uuid`. Build and test only
through `wk build` / `wk test` / `wk run` / `wk status`. Do not commit; leave changes in the working
tree and report.

## Why

An agent proposed Linux and CMake build-configuration changes for `ENABLE_MYA_HEAP`. They have been
taken out of the PR (`eng/mya-rtti-libilldb`), because build changes risk breaking production builds
and none was shown to be needed. Each one comes back only if a testLibJSCTools check
(`Source/JavaScriptCore/corpse/tests/HeapWalkTest.cpp`, `DebugInfoTest.cpp` or another suite in
`corpse/tests/`) calls an lldb or libpas API that **fails without the option**, and you **confirm
that failure by building without it**. A claim that is only about speed needs before/after
measurements instead.

For each claim, report one of:
- **Confirmed**: the test check, the build without the option, and the exact failure.
- **Refuted**: what you ran and why it shows no difference.
- **Perf only**: timings, each run several times.

Measure before theorising: when something fails, take the measurement before proposing a cause.

Record the clang version, the lld version and the liblldb version (with the package that provides
it) for every result. Several of these claims depend on the toolchain.

`TypeinfoTest.cpp` no longer exists. The suites are named on testLibJSCTools' command line
(`testLibJSCTools HeapWalk`, `DebugInfo`, `HeapDump`, ...); with no name it runs them all. HeapWalk,
HeapDump and DebugInfo are the ones that read debug info.

## What macOS settled (2026-10-06), for context

Measured on macOS 27 with Apple clang 2100 and Xcode's `LLDB.framework` (lldb-2103.0.34.103), with a
real build without each option and a full testLibJSCTools run. The full write-up is the "Build
changes" section of `design.md`. Use it as a hypothesis on Linux, not a result: the toolchain differs.

| Option | macOS result |
|---|---|
| strip `-gsimple-template-names` | **Confirmed**: 10 failures with the flag (HeapWalkTest.cpp:1060, 1061, 1116: values read as types bigger than their allocations; closures' call operators not found). A toy did **not** reproduce it, so do not trust a toy here either. |
| strip `-mllvm -dwarf-linkage-names=Abstract` | **Confirmed**: 20 failures. The mechanism is wider than "implicit destructor": every instance of a class template (`JSC::WeakGCMap<...>`, `WeakGCSet<...>`, `WTF::Detail::CallableWrapper<...>`, `ObjectPropertyChangeAdaptiveWatchpoint<...>`) "declares no member function with a linkage name", so the walk cannot name the class of a vtable's destructor. Failures at HeapWalkTest.cpp:1030, 1051, 1072, 1116. |
| drop `-fpch-debuginfo` | **Confirmed, Apple only**: through the debug map, HeapDump misses `UnlinkedProgramCodeBlock -> JSString/JSCellButterfly` edges (HeapWalkTest.cpp:1577) in 5 of 7 runs with it, 0 of 8 without. |
| `PAS_BMALLOC_HIDDEN=0` | **Confirmed**: without it, `mya` fails to link on `_pas_enumerator_create`, `_pas_enumerator_destroy`, `_pas_enumerator_enumerate_all`. |
| `-fstandalone-debug` | **No-op on macOS**: Darwin's clang already defaults to it; every suite passes without the flag. Linux is the open case (claim 8). |
| RTTI, default visibility | **Not needed**: passes with `-fno-rtti` and `-fvisibility=hidden` on every compile. |
| `-O2` for `libJavaScriptCoreTools` in Debug | **Perf only**: HeapWalk suite 21.1-21.8 s at `-O2`, 29.1-29.3 s at `-O0` (1.4x, not the 2x its comment claimed). |
| CMake `dsymutil` after each link | **Dropped**: Apple only, and unneeded once `-fpch-debuginfo` is gone. |

Also learned on macOS, possibly relevant here:
- **liblldb versions matter for correctness, not just features.** Homebrew's liblldb 22.1.8 (upstream
  LLVM) segfaults in `clang::ASTContext::getASTRecordLayout`, called from `SBType::GetByteSize` in
  `ReachWalk::walkGlobalVariables`, during the HeapDump suite, once the TypeFields suite has loaded
  WebCore into the process. Xcode's lldb passes the same build. Still being diagnosed on macOS.
  TypeFields is Darwin-only, so a JSCOnly Linux run cannot hit this path; say so if you see anything
  like it, for example in a `mya heap` walk of a GTK or WPE web process.
- **Never mix lldb headers and library from different builds.** Enumerator values shift between
  releases (Homebrew 22 vs Xcode: 86 of 879 differ). `FindLLDB.cmake` takes the library from the
  headers' own install; confirm on each distro that the configure picks a matching pair, and report
  the include directory and library path CMake chose.

## Baseline first

Build JSCOnly Debug with `ENABLE_MYA_HEAP=ON` and DEVELOPER_MODE, then run testLibJSCTools. That
answers whether the branch as it now stands passes on Linux. Its CMake MYA_HEAP block
(`WebKitCommon.cmake`) has:
- `-fstandalone-debug`
- `-mllvm -dwarf-linkage-names=Abstract` and `-gsimple-template-names` removed from the global flags
- `add_compile_definitions(PAS_BMALLOC_HIDDEN=0)`
- `LINKER:--build-id` when not on Apple
- `DEBUG_FISSION` off
- no RTTI and no default visibility
- `find_package(LLDB 19)`

Check in `compile_commands.json` that each of these took effect, and that `-fpch-debuginfo` is
absent. Run with the distro's liblldb (for example 18 on Ubuntu 24.04) and with a newer one from
apt.llvm.org. Any later claim that turns out to be needed should show up as a failure here first.

## Claims to validate

### 1. liblldb 19 or newer
In `Source/JavaScriptCore/CMakeLists.txt`, `find_package(LLDB 19)` plus a `FATAL_ERROR` below 19:
> liblldb 18 cannot resolve addresses in an ELF segment that a PT_TLS segment starts at, such as
> libstdc++'s vtables; 19 can.

Find the API that fails. Most likely it is `SBTarget::ResolveLoadAddress` on a vtable address, then
`GetSymbol()` (see `vtableClassName` in `CorpseSnapshotDebugInfo.cpp`). Show it failing with 18
and passing with 19.

Also check JSC's own vtables. lld usually puts `.tdata` at the start of the RELRO segment that holds
`.data.rel.ro`, so libJavaScriptCore's vtables may be affected too, not only libstdc++'s.

Separately, a hard `FATAL_ERROR` would stop every build machine on a distro that ships lldb 18.
Report which distros the bots use.

### 2. LLDB version detection in `FindLLDB.cmake`
The non-Apple branch reads the version from the library's file name or install directory:
> The SB headers carry no version, but the installed library's name does (liblldb-18.so.1 on Debian),
> as does the directory of an install (/usr/lib/llvm-18).

(The Apple branch now links Xcode's `LLDB.framework` and reads its version from the framework's
`Info.plist`; it does not affect Linux.) This is only needed if claim 1 holds. If it does, check the
parsing on Debian, Ubuntu, Fedora (`lldb-devel`) and apt.llvm.org layouts. The error message's
advice to "Install liblldb-22-dev from apt.llvm.org, then configure again with -ULLDB_INCLUDE_DIR
-ULLDB_LIBRARY" needs checking too.

### 3. `--build-id` at link time
`add_link_options("LINKER:--build-id")` when not on Apple:
> The build-id is what refuses an image rebuilt since it was loaded. lld writes none unless asked,
> and only some distributions' clang asks.

This belongs with the image-identity check (`CorpseImage.cpp`, which reports "has no build-id").
Show which toolchains leave out `.note.gnu.build-id` (`readelf -n`), and that the check fails
without it.

### 4. Turning off `DEBUG_FISSION` under MYA_HEAP
The `-gsplit-dwarf` / `--gdb-index` block moved from `OptionsCommon.cmake` to `WebKitCommon.cmake`
so that MYA_HEAP could turn it off:
> liblldb reads .debug_names but not .gdb_index, so with split DWARF it indexes every .dwo file by
> hand whenever it loads an image.

This is about speed. Time the heap walk and testLibJSCTools with fission on and off, several runs
each. Also check that the results are correct with fission on, because liblldb does read `.dwo`
files.

### 5. `-gsimple-template-names` stripped
Confirmed on macOS in the real build (above). On Linux, build JSCOnly Debug with the flag kept (take
the `string(REPLACE "-gsimple-template-names" ...)` line out of `WebKitCommon.cmake`) and run the
full testLibJSCTools. Report the failing assertions and their counts next to the macOS ones.

### 6. `-mllvm -dwarf-linkage-names=Abstract` stripped
Confirmed on macOS in the real build (above). On Linux, build with the flag kept and run the full
suite. Look for "declares no member function with a linkage name" in the output, and report which
classes it names.

### 7. `-fpch-debuginfo` dropped
Not applicable on Linux: `_WEBKIT_ADD_PCH_OBJECT` returns early unless the compiler is clang on
Apple, so a Linux build never passes the flag. Confirm by its absence from `compile_commands.json`,
and stop there.

### 8. `-fstandalone-debug`
A no-op on macOS, so Linux is where it is needed or not. Clang on Linux defaults to limited debug
info: without the flag, a class with a vtable is described in full only where its key function is,
so a type defined in libJavaScriptCore would be complete only there, and the test executable's own
compile units would only declare it. The DebugInfo suite's cross-image checks
(`CrossImageFireDetail`, `LaterDestructor` in `DebugInfoTest.cpp`) and the HeapWalk suite are the
likely places to fail. Build without the flag and run the full suite.

### 9. RTTI and default visibility removed, and `PAS_BMALLOC_HIDDEN=0`
Confirm that testLibJSCTools and `mya` link on Linux with hidden visibility plus
`PAS_BMALLOC_HIDDEN=0`, and fail to link without the define (on macOS: `_pas_enumerator_create`,
`_pas_enumerator_destroy`, `_pas_enumerator_enumerate_all`).

### 10. `-O2` for `libJavaScriptCoreTools` in Debug (perf)
`target_compile_options(JavaScriptCoreTools PRIVATE -O2)` in `corpse/CMakeLists.txt`. Time the
HeapWalk suite with and without it, three runs each.

## Notes

- `wk build <preset> --detach`, then `wk status --wait` and `wk status --log`. Do not hand-roll
  `build-webkit` or `ninja` in the workspace.
- A changed global compiler flag rebuilds everything, so decide each variant's flags before starting
  its build. Build one variant per configure, and change only one option per variant.
- One testLibJSCTools run can pass by luck: the `-fpch-debuginfo` failure on macOS showed in 5 of 7
  runs. When a variant passes, run it at least three times before calling the claim refuted.
- Heap-walk runs use several GB. Run one at a time.
