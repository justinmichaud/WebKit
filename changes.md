# Changes on `eng/mya-heapwalk-4`

Seven commits on top of `origin/main` at `ab16b1d2a772`. 89 files, +6141/−213.

This document lists every change, why it is there, and what fails without it.

## How to read the "fails without it" column

Each entry is tagged with how the claim is known:

- **[measured]** Seen in this checkout on 2026-11-23, on macOS arm64, with both the Xcode build and the CMake JSCOnly build.
- **[earlier]** Measured in October 2026 on an older base, from the project notes. Not re-measured on this base.
- **[by construction]** Follows from the code (for example a Mach-only header on Linux). Not compiled to confirm.
- **[not measured]** A test covers the code, but nobody removed the change to watch the test fail.

Two limits apply to the whole document:

- **Linux was compiled and run only at the tip of the branch** (arm64, clang 19, liblldb 21.1.5, CMake JSCOnly Debug, 2026-11-23): 25,601 assertions pass and six suites skip with a printed reason. The earlier commits were not built on Linux one by one, and most Linux claims below are still [by construction].
- **Most library changes are [not measured].** The per-commit verification proves each commit builds and passes; it does not prove each line is necessary.

## Verification of the branch as a whole

| Commit | CMake JSCOnly | Xcode |
|---|---|---|
| 1 Build mya on linux | builds, 991 assertions pass | builds, 991 pass |
| 2 mya_heap and liblldb | builds, 98,035 pass | builds, 101,115 pass |
| 3 Error handling | builds, 98,144 pass | builds, 101,224 pass |
| 4 List images and snapshots | builds, 98,598 pass | builds, 101,678 pass |
| 5 Load debuginfo | builds, 98,619 pass | builds, 101,699 pass |
| 6 Simple heap walking | builds, 30,772 pass | builds, 61,516 pass with `--website` |
| 7 Swift stub | builds, 30,832 pass | builds, 61,572 pass with `--website` |

The website suite (commits 6 and 7, full Xcode Debug WebKit build) walked the WebContent process of a MiniBrowser that had loaded `PerformanceTests/Parser/resources/html5.html`: 4,234 addresses, 6,719 edges, 2,441 objects of classes WebCore defines, no error reports.

## What is deliberately not in the branch

| Dropped | Why | Evidence |
|---|---|---|
| The old first commit (bug 325093) | Main absorbed it as `ENABLE(LIB_JSC_TOOLS)`, `ENABLE_JAVASCRIPTCORE_TOOLS` and the `jsc-all` target | Compared against main's commits |
| Moving entitlement generation out of `shell/PlatformCocoa.cmake` | Not needed. With only main's default `get-task-allow`, JSCOnly `jsc` JITs, `mya` attaches across processes, and `testLibJSCTools` passes | [measured] |
| `-O2` for libJavaScriptCoreTools in Debug | Skipped on request. It was a speed-up only (1.38x on the heap-walk suite) | [earlier] |
| `mya heap` command, `g_myaRootVM`, `Snapshot::addressOf` | Replaced by the `HeapWalkWebsite` suite, which finds the VM through the exported `VMManager::s_recentVM` | [measured] |
| `CorpseVMTest`, the `--webcore` test option | Superseded by the heap-walk suites | — |

---

## Commit 1: Build mya on linux (bug 325094)

Goal: the corpse library, `mya` and `testLibJSCTools` compile and run on Linux, with stubs where Linux has no implementation yet. Upstream's Darwin code is left as it is wherever possible.

### Build

| Change | Why | Fails without it |
|---|---|---|
| `WebKitFeatures.cmake`: tools default ON for 64-bit Linux developer builds | So `-DENABLE_JAVASCRIPTCORE_TOOLS=ON` is not needed by hand | Linux builds no `mya`. [by construction] |
| `JavaScriptCore/CMakeLists.txt`, `project.pbxproj`: list `CorpsePlatform.h` | New private header | Includers do not find it. [by construction] |

### Library

| Change | Why | Fails without it |
|---|---|---|
| New `CorpsePlatform.h`: `TaskHandle`, `invalidTaskHandle`, `isValidTaskHandle`, `KernelResult`, `kernelSuccess` | One spelling for "a task" and "a kernel result" on both systems | Headers that name `mach_port_t` / `kern_return_t` do not compile on Linux. [by construction] |
| `CorpseAddress.h`: `mach_vm_address_t` becomes `uint64_t`, `<mach/mach.h>` removed | Mach type and header do not exist on Linux | Linux compile. [by construction] |
| `CorpseMemory.*`, `CorpseMemoryPtr.h`, `CorpseMemorySpan.h`: use the platform types; Linux `Region::create` copies pages with `process_vm_readv`, Darwin keeps `mach_vm_remap_new` | Linux has no way to share a corpse's pages | Linux compile and every memory read. [by construction] |
| `CorpseProcess.*`: Linux `attach` (own process only), `detach`, `holdsLiveTask`, `isTranslated`; new `executablePath()` on both systems | Linux has no task ports; tests need the executable's path | Linux compile. `executablePath` is used by `CorpseSymbolTest`'s helper lookup. [by construction] |
| `CorpseRegion.*`: Darwin code under `OS(DARWIN)`, Linux stubs | Mach VM region calls are Darwin-only | Linux compile. [by construction] |
| `CorpseSnapshot.*`: Darwin corpse under `OS(DARWIN)`; Linux reads the live process; `TaskHandle` in the header | No corpses on Linux yet | Linux compile. [by construction] |
| `CorpseThread.cpp`: the Linux stub reports an invalid snapshot, as Darwin does | Tests count the reports of an invalid snapshot | `CorpseSnapshotTest`'s expected error count on Linux, from commit 3 on. [measured] |
| `CorpseSymbol.*`, `CorpseThread.*`: Mach-O and Mach thread code under `OS(DARWIN)`, Linux stubs that find nothing | Exports tries and Mach thread state are Darwin-only | Linux compile. [by construction] |
| `CorpseExportsTrie.cpp`: guard `&& OS(DARWIN)` | Includes `<mach-o/loader.h>` | Linux compile. [by construction] |
| `mya.cpp`: `static_cast<unsigned long long>` on four printed addresses | `uint64_t` is `unsigned long` on Linux, and the format is `%llx` | `-Wformat` error on Linux. [by construction] |

### Tests

| Change | Why | Fails without it |
|---|---|---|
| `LibJSCToolsTestUtilities.*`: `linuxSkip()`, Mach helpers under `OS(DARWIN)`, 15-character thread names on Linux | Suites that need Darwin-only features skip on Linux | Linux compile, or failures on Linux. [by construction] |
| `CorpseAddressTest`, `CorpseRegionTest`, `CorpseMemoryTest`, `CorpseExportsTrieTest`, `CorpseProcessTest`, `CorpseSnapshotTest`, `CorpseSymbolTest`, `CorpseThreadTest`: Mach casts become `uint64_t`, Darwin-only blocks guarded, Linux skips | Same | Linux compile. [by construction] |
| `CorpseSymbolTest`: helper path from `Process::executablePath()` instead of `_NSGetExecutablePath` | `_NSGetExecutablePath` is Darwin-only | Linux compile. [by construction] |

---

## Commit 2: mya_heap and liblldb as dependencies (bug 325546)

Goal: an optional feature, `ENABLE(MYA_HEAP)`, that links liblldb 21 and builds debug info liblldb can read. All build flags for the heap walk live in this commit.

### Feature switch

| Change | Why | Fails without it |
|---|---|---|
| `WebKitFeatures.cmake`: `ENABLE_MYA_HEAP`, default ON for Debug when the tools are on | The switch | No heap-walk code is built. [by construction] |
| `CommonBase.xcconfig`: `WK_MYA_HEAP` (Debug, macOS), which defines `ENABLE_MYA_HEAP=1` and `PAS_BMALLOC_HIDDEN=0` | The Xcode switch | Same. [by construction] |
| `PlatformEnable.h`: `ENABLE_MYA_HEAP` forced to 0 when the tools are off or `<lldb/API/LLDB.h>` is missing | A machine without liblldb must still build | The full WebKit Xcode build fails: the Swift build of the `JavaScriptCore_Private` module includes `CorpseSnapshotDebugInfo.h` and cannot find liblldb's headers. A bare `#undef` without the `#define … 0` fails the same way. [measured] |

### Finding and linking liblldb

| Change | Why | Fails without it |
|---|---|---|
| New `FindLLDB.cmake`; `JavaScriptCore/CMakeLists.txt` requires version 21 | Locate the LLVM 21.1.8 release liblldb (`~/Library/Developer/LLVM-21.1.8-macOS-ARM64`, or `/usr/lib/llvm-21`) | liblldb 22 loses a base class in one test type, and crashes on WebCore types. [earlier] |
| `corpse/CMakeLists.txt`: link `LLDB::LLDB` when found | Link | Link errors. [by construction] |
| `Base.xcconfig`, `Mya.xcconfig`, `TestLibJSCTools.xcconfig`, `libJavaScriptCoreTools.xcconfig`: `WK_LLDB_ROOT`, header search path, `-L` and rpath | The Xcode equivalent | Xcode compile and link. [by construction] |
| `CorpseSnapshot.cpp`: `.linker_option "-llldb"` under `ENABLE(MYA_HEAP)` on Xcode | Xcode has no optional link dependency | Link errors on Xcode when the feature is on, or a hard dependency when it is off. [by construction] |

### Debug info that liblldb can read

| Change | Why | Fails without it |
|---|---|---|
| Every `DebugRelease.xcconfig` (16 files) uses `WK_DEBUG_RELEASE_DEBUG_INFORMATION_FORMAT`, which is `dwarf-with-dsym` under `WK_MYA_HEAP` | Through a debug map, liblldb cannot type every copy of a constant a header defines | `TypeinfoTest`'s copied-header-constant check: one of two copies is untyped. [earlier] |
| `CommonBase.xcconfig`: `CLANG_ENABLE_MODULE_DEBUGGING = NO` under `WK_MYA_HEAP` | Module debugging leaves dSYMs without some definitions | 20 failures describing WebCore classes with the old `--webcore` check; JavaScriptCore passed either way. That option is gone, so no test in this branch is known to fail. [earlier] |
| `WebKitMacros.cmake`: `dsymutil` after each shared library and executable on Apple | Same reason as the dSYM format, for CMake | Same check. [earlier] |
| `WebKitMacros.cmake`: skip the PCH object under `ENABLE_MYA_HEAP` | Its types were dropped by dsymutil and its functions had no DWARF | 82 failures in `TypeinfoTest` on a clean JSCOnly build. [earlier] |
| `WebKitCommon.cmake`: strip `-dwarf-linkage-names=Abstract` | liblldb finds a global by its linkage name | 0 of 5,243 JavaScriptCore globals typed; the external-global check fails. [earlier] |
| `WebKitCommon.cmake`: strip `-gsimple-template-names` | liblldb confuses instances of one template | 53 of 19,527 destructors' `this` is another instance; the template check fails. [earlier] |
| `WebKitCommon.cmake`: `-fstandalone-debug`, `--build-id`, strip `-gsplit-dwarf` / `--gdb-index` | Linux: complete class definitions, image identity, DWARF liblldb reads | No effect on Apple clang. Linux: [not measured] |
| `WebKitCommon.cmake`, `bmalloc/PlatformCocoa.cmake`, `CommonBase.xcconfig`: `PAS_BMALLOC_HIDDEN=0` | `TypeinfoTest` calls libpas's enumerator | CMake link fails on `_pas_enumerator_*`. [earlier] |
| `JavaScriptCore.xcconfig`: TAPI verify mode relaxed under `WK_MYA_HEAP` | Exported libpas symbols are not in the text-based API | TAPI verification, per the comment in the file. [not measured] |

### Tests

| Change | Why | Fails without it |
|---|---|---|
| New `TypeinfoTest.cpp` / `.h` | One check per build setting above, plus liblldb's version and a libpas enumeration | It is the test the rows above fail in |
| `LibJSCToolsTestUtilities.*`: `analyzeInAndOutOfProcess`, `runCorpseTarget`, `--target` option in `testLibJSCTools.cpp` | Runs an analysis on this process and on a copy of it started as a target | `TypeinfoTest` could only check its own process. [by construction] |
| `LibJSCToolsTestUtilities.*`: `copiedHeaderConstant` | A second translation unit holding a copy of a header constant | The dSYM check has nothing to find. [by construction] |

Loose end: this commit also changes two `UTF8CString` constructions in `CorpseProcess.cpp` to `UTF8CString::fromUTF8`. That is code commit 1 adds, so the edit belongs there.

---

## Commit 3: Error handling and testing (bug 325303)

Goal: tests can assert exactly which errors the library reports, and a report says what the library was doing.

| Change | Why | Fails without it |
|---|---|---|
| `CorpseError.*`: `Error::reportCount()`, `report()` builds the whole message before writing | Tests count reports | `ExpectedErrors` has nothing to count. [by construction] |
| `CorpseError.*`: `Diagnostics` scope and `CORPSE_DIAGNOSTICS`; each report appends "while …" lines | A heap-walk error names the class and object being read | Reports lose their context. Covered by `CorpseDiagnosticsTest`. [not measured] |
| `CorpseError.h`: `CORPSE_REPORT` | Format-checked report call for new code, without a file-wide unsafe-buffer exemption | New files would need the exemption. [by construction] |
| New `CorpseDiagnosticsTest.cpp` / `.h` | Tests the scope nesting | — |
| `LibJSCToolsTestUtilities.*`: `ExpectedErrors`; `SuiteTracer` fails a suite that reported errors nobody asked for | Unexpected reports fail tests | Library errors would pass silently. [by construction] |
| `LibJSCToolsTestUtilities.*`: `analyzeInSeparateProcess`, started by relative path, after the target exits | More ways to run a target | Tests below. [by construction] |
| `CorpseProcessTest`, `CorpseSnapshotTest`: use `ExpectedErrors`; a target that exits early; a target started by a relative path | Coverage of the new helpers | — |

Upstream's symbol-lookup diagnostics (`CORPSE_SYMBOL_LOOKUP_DIAGNOSTICS`) are untouched by this commit. A failed lookup is still reported by `mya`, not by the library.

---

## Commit 4: List images and snapshots (bug 326803)

Goal: a snapshot lists its images and memory regions on both systems, and a cooperating target can offer a snapshot of itself.

### Library

| Change | Why | Fails without it |
|---|---|---|
| New `CorpseImage.*`: `Image` (load address, path, UUID); Darwin from dyld's UUID list, Linux from `/proc/<pid>/maps` | liblldb needs each image's path, UUID and load address | No debug info can be opened (commit 5). Covered by `CorpseSnapshotTest`. [not measured] |
| `CorpseImage.h`: `dyldAllImageInfos`, `dyldImageInfos` | One reader of dyld's image list | — |
| `CorpseSymbol.*`: `lookUpName` uses that reader; the per-step diagnostics fields for that read are removed, `images` kept | Sharing, on request. The reader reports its own failures | Two copies of the same read. Upstream's optional diagnostics still compile and run with the flag set to 1. [measured] |
| New `CorpseLimits.h`: `maxImageCount`, `maxImagePathLength`, `maxRegionCount` | Limits used by more than one file | Duplicate constants. [by construction] |
| `CorpseMemory.*`: `cString()` | Read an image path out of the target | Image paths cannot be read. Covered by `CorpseMemoryTest`. [not measured] |
| `CorpseRegion.*`: `Region::all()`; Darwin `atOrAbove`; Linux `/proc` maps parser with name, offset, inode, executable flag | List every region; Linux images are derived from regions | No Linux images. Covered by `CorpseRegionTest`. [not measured] |
| `CorpseSnapshot.cpp`: `ProcessStat` fields have default initializers | Its construction names only the first field | `-Wmissing-field-initializers` error on Linux. [measured] |
| `CorpseProcess.*`: Linux `readProcFile` | Shared by regions and process state | Linux compile. [by construction] |
| `CorpseSnapshot.*`: `images()`, `regions()`, `executablePath()` | Cached lists; dyld records a relative executable path, the kernel's is absolute | A target started by a relative path lists no executable image. Covered by `CorpseSnapshotTest`. [not measured] |
| `CorpseSnapshot.*`: `offer()`, `offerSafepoint(VM&)`, `receive()`; Linux fork-a-copy machinery | A target stops itself at a known point; on Linux the copy keeps memory as it was | Only tests call these today. [by construction] |

### Tests

| Change | Why | Fails without it |
|---|---|---|
| `CorpseSnapshotTest`: image list against dyld / `dl_iterate_phdr`, target images, snapshot copies | Coverage of the above | — |
| `CorpseRegionTest`, `CorpseMemoryTest`: all regions, C strings | Coverage | — |
| `LibJSCToolsTestUtilities.*`: targets offer snapshots, at a safepoint of their VM if they set one with `setTargetVM`; replaced executable; `makeLibraryPathAbsolute` in `testLibJSCTools.cpp` | Targets run in other working directories | A target started elsewhere loads other libraries. [by construction] |
| `TypeinfoTest`: uses `Memory::cString` instead of a local reader | Removes a duplicate | — |

Not in this commit, on purpose: the liblldb check of loaded images that the commit used to carry. It left liblldb's threads running and failed four port-count assertions later in the suite. [measured]

---

## Commit 5: Load debuginfo in lldb (bug 327160)

Goal: open the debug info of every image of a snapshot. Nothing else.

| Change | Why | Fails without it |
|---|---|---|
| New `CorpseSnapshotDebugInfo.*`: `SnapshotDebugInfo::create(Snapshot&)` adds each image to an lldb target at its load address; reports an image that does not open | The heap walk reads types from here | Nothing to read types from. [by construction] |
| `CorpseImage.*`: UUID kept as a hex string | The form liblldb's `AddModule` takes | Module lookup by UUID. [not measured] |
| `CorpseLimits.h`: `maxVTableSize` | Used by commit 6; it landed here | Nothing in this commit. It belongs in commit 6 |
| New `DebugInfoTest.cpp` / `.h` (70 lines) | Opens debug info in this process and in a target; any report fails the suite | — |
| `CorpseSnapshotTest`: UUID assertions follow the new type | — | Compile. [by construction] |

---

## Commit 6: Simple heap walking (bug 327161)

Goal: walk every object reachable from a root, reading each member as its class's debug info describes it.

### Library

| Change | Why | Fails without it |
|---|---|---|
| New `CorpseTargetType.*`: `TargetType` as a variant of class, declaration, pointer, opaque value and array; `TargetObject`, `TargetCanonicalPointer` | The walker's model of a type, one per image | — |
| `CorpseTargetType.*`: `sameClass`, `sameType`, `sameName` (ignoring template arguments), `baseAt`, `definitionOf` | Images are never matched by name alone; liblldb spells one template instance differently in different images | `JSC::VM` became two nodes when names were compared exactly; two heap-walk failures. [earlier] |
| `CorpseSnapshotDebugInfo.*`: `parameterTypes(function)` | Types are reached from addresses, never looked up by name | The walk has no type for its root. [by construction] |
| `CorpseSnapshotDebugInfo.*`: `pointee()` described on first use | A class can point to itself | Describing a class would not terminate. [by construction] |
| `CorpseSnapshotDebugInfo.*`: `canonicalPointer()` from the vtable | An object is walked as its dynamic type | Derived members are never reached. Covered by `HeapWalkTest`. [not measured] |
| `CorpseSnapshotDebugInfo.*`: class table keyed by liblldb type identity, hashed on full name, size and members | One description per class per image | Hashing the template-stripped name took the JavaScriptCore type check from 23 s to 83 s. [earlier] |
| `CorpseSnapshotDebugInfo.*`: `NeedsReader` reasons (bitfield, union, flexible array, container, atomic, Objective-C object) | Marks what debug info cannot describe, for readers to come | The walk would misread these as plain members. Covered by `HeapWalkTest`. [not measured] |
| New `CorpseIgnoredDeclarations.h`: `__CFRunLoop`, `__CFRunLoopSource` | Classes no image defines | One error report per such class on Cocoa. [earlier] |
| New `CorpseHeapWalk.*`: `HeapAnalyzer`, `walkHeap()`; edges to declared-only classes resolved after the walk | The walk itself | — |

### Tests

| Change | Why | Fails without it |
|---|---|---|
| New `HeapWalkTest.cpp` / `.h`: fixture of inheritance shapes, a recording analyzer | Coverage of the walker | — |
| `HeapWalkTest`: dynamic types read in either order | liblldb 21 answers differently for a class after reading another compile unit | That check. [earlier] |
| `HeapWalkTest`: the JavaScriptCore image is matched as `libJavaScriptCore.so` off Darwin; the `std::atomic` reader checks hold only for libc++ | libc++ holds the value as `_Atomic(T)`, libstdc++ as a plain member liblldb describes, so on Linux the walk reads through the atomic | Three failures on Linux, each in and out of process. [measured] |
| `HeapWalkTest`: `HeapWalkWebsite` suite; `--website <url>` in `testLibJSCTools.cpp` | Walks a real page in a real web content process. Darwin only | It replaces the removed `mya heap` command. Passes with zero reports. [measured] |
| `LibJSCToolsTestUtilities.*`: `functionTakingPointerTo`, `parameterClass` | Get a class's type from a function's parameter | `HeapWalkTest` has no root type. [by construction] |
| `TypeinfoTest`: class description through `SnapshotDebugInfo` | Checks every class of the test and of JavaScriptCore describes without a report | — |

---

## Commit 7: Swift support stub (bug 327624)

Goal: the walker recognizes Swift and Objective-C values it cannot read yet, and the build produces debug info for Swift modules that liblldb can use.

| Change | Why | Fails without it |
|---|---|---|
| `CorpseTargetType.h`, `CorpseSnapshotDebugInfo.cpp`: `NeedsReader::SwiftPointer`, `SwiftValue` | Swift values held through C++ interop need the Swift runtime to describe them | They would be walked as plain bytes. Covered by `HeapWalkTest`. [not measured] |
| New `Tools/Scripts/generate-mya-heap-dsym`; `WebKitMacros.cmake` calls it instead of plain `dsymutil` | dsymutil drops a Clang module's debug info when the first object that refers to it holds no code | Four failures describing WebCore on CMake; two reports in the reproducing test. [earlier] |
| New `HeapWalkImportsOnly.swift` | Reproduces that: an object with only an import, linked first | The fix above has no test. [by construction] |
| `HeapWalkImportsOnly.swift`: `@diagnose(UnusedImportAccess, as: ignored, …)` | Main makes that warning an error | The Xcode build fails on this file. [measured] |
| `HeapWalkTest`: `#if defined(JSC_SUPPORTS_SWIFT) && JSC_SUPPORTS_SWIFT`, as in `testapi.c` | Linux does not define the macro | `-Wundef` error on Linux. [measured] |
| New `HeapWalkSwift.swift`, `HeapWalkModule.h`, `HeapWalkSwiftModule.h`, `Modules/module.modulemap` | Swift objects and a C++ class used from Swift, for the fixture | — |
| New `HeapWalkObjectiveC.h` / `.m`, `HeapWalkObjectiveCPlusPlus.mm` | Objective-C objects, blocks and a lambda in an Objective-C method, for the fixture | — |
| `shell/CMakeLists.txt`, `OptionsJSCOnly.cmake`, `TestLibJSCTools.xcconfig`, `project.pbxproj` | Build the Swift and Objective-C fixtures; Swift is required for JSCOnly on Apple when the feature is on | The fixtures do not build. [by construction] |
| `HeapWalkTest`: Swift and Objective-C cases | Coverage | — |

---

## Known open points

- **Linux:** only the tip was built and tested (see above). The JSCOnly build needs `-DCMAKE_CXX_FLAGS=-Wno-error=ignored-attributes` with clang 19 for upstream WTF, and upstream gtest fails to build there, so build the `jsc`, `mya`, `testLibJSCTools` and `testLibJSCToolsHelper` targets. Thread listing and ELF symbol lookup are still stubs.
- **Style checker:** it reports `printf` on the two lines of `mya.cpp` that carry the Linux cast, an include-order finding on test files that follow upstream's own layout, and a formatter finding on the `@diagnose` line that upstream's Swift files also get.
- **Commit messages:** unchanged from before this work, so some describe things that are no longer there.
- **Misplaced hunks:** the `fromUTF8` edit in commit 2 and `maxVTableSize` in commit 5, noted above.
- **Coverage given up:** `DebugInfoTest` no longer checks dynamic-type edge cases (cross-image class, inherited first vtable slot, no virtual destructor, system-library object, replaced executable, non-polymorphic object).
