# mya: walking a JavaScriptCore heap from a snapshot

mya inspects a JavaScriptCore process from outside it. The corpse library
(`Source/JavaScriptCore/corpse/`) takes a snapshot of a process: a Darwin corpse,
or the live process on Linux. mya then reads that snapshot's memory.

This design adds three things:
- a walk of the snapshot's JS heap and the C++ objects hanging off it;
- every type taken from the process's own debug info, read through liblldb;
- a measurement of how much of the heap the walk reaches.

It lands as eight small patches. Each has exactly the code its test needs.

## Rules

- **mya parses no binary formats.** liblldb reads Mach-O, ELF and DWARF. mya
  reads only fixed structs from memory, such as dyld's image list.
- **mya's source names no type or variable to look up.** A type comes from a
  vtable (a class's dynamic type) or from a field's declared type. Fields are
  reached by name, as the C++ source reaches them.
- **liblldb is never given a process.** mya reads all memory itself
  (`RELEASE_ASSERT(!target.GetProcess().IsValid())`), so a walk never depends on
  the process still running.
- **Snapshots are taken at a safe point** (patch 7). The walk may assume the
  invariants JSC keeps between collections.
- **The production build does not change.** Nothing changes in JavaScriptCore,
  `jsc` or WebKit's web process. Objects that exist only for tests, such as the
  heap walk's roots, live in the tests.
- **Code that reads JS cells mirrors JSC's heap code.** Each routine has a comment
  naming its original.
  - Trivially copyable WTF values (`WTF::BitSet`, `Markable`,
    `BlockDirectoryBits::Segment`) are copied out whole with `as<T>()`, which
    checks the size against the target's debug info, and are then read with
    their own methods.
  - Containers that point into the target have small wrappers in
    `CorpseRemote.h`. The wrappers reuse WTF's public logic, and copy anything
    private with a comment naming it.
- **A failure is reported where the data was needed.** One unreadable image does
  not fail the snapshot.

## Build and test

macOS needs the SB API headers from Homebrew's llvm (see Build gotchas). Run both
builds, because they differ in ways that matter.

```
Tools/Scripts/build-jsc --debug --xcode --export-compile-commands
DYLD_FRAMEWORK_PATH=WebKitBuild/Debug WebKitBuild/Debug/testLibJSCTools --verbose
Tools/Scripts/run-javascriptcore-tests --debug --no-build --testlibjsctools

Tools/Scripts/build-jsc --jsc-only --debug
DYLD_LIBRARY_PATH=$PWD/WebKitBuild/JSCOnly/Debug/lib WebKitBuild/JSCOnly/Debug/bin/testLibJSCTools --verbose
DYLD_LIBRARY_PATH=$PWD/WebKitBuild/JSCOnly/Debug/lib Tools/Scripts/run-javascriptcore-tests \
    --debug --no-build --testlibjsctools --root=WebKitBuild/JSCOnly/Debug/bin
```

Linux:

```
Tools/Scripts/build-jsc --jsc-only --debug --cmakeargs="-DCMAKE_COMPILE_WARNING_AS_ERROR=OFF -DCMAKE_CXX_FLAGS=-Wno-ignored-attributes" --export-compile-commands
WebKitBuild/JSCOnly/Debug/bin/testLibJSCTools --verbose
Tools/Scripts/run-javascriptcore-tests --jsc-only --debug --no-build --testlibjsctools --no-testmasm --no-testair --no-testb3 --no-testdfg --no-testapi --no-testwasmdebugger --no-jsc-stress --no-mozilla-tests
```

## The patches

### 1. Images on Darwin

`Snapshot::images()` lists each image outside the shared cache: its load address,
dyld's path for it, and its UUID. It pairs dyld's `uuidArray` with `infoArray` by
load address. The symbol lookup shares `Snapshot::dyldAllImageInfos()`. This
patch needs no liblldb.

- A path string is read one page at a time, because it can end just before an
  unmapped page.
- Shared-cache images are not in `uuidArray`, and Apple ships no DWARF for them.
  A typical process has about 9 images outside the shared cache, out of 400.

**Test.**
- The executable and JavaScriptCore are listed at their `dladdr` bases.
- Every image has a path and a UUID, and two images have two different UUIDs.
- `malloc`'s image is not listed.
- An invalid snapshot lists no images.

### 2. Debug info and dynamic types on Darwin

`SnapshotDebugInfo::create` gives liblldb an empty target, then, for each image,
calls `AddModule(path, nullptr, uuid)` and `SetModuleLoadAddress`.
`dynamicTypeAt(address)` returns the class of the complete object that contains a
polymorphic object. `TargetType` describes a type's layout, which is one of:
- a class: fields with their bit offsets, direct non-virtual bases, and virtual
  bases;
- a pointer or reference;
- an integer;
- anything else.

**The destructor rule.** Read the vtable pointer, then the offset to top, to find
the complete object's vtable. The first slot that resolves to a destructor gives
the type: the pointee of that destructor's `this`. The destructor is the one
virtual function every class overrides, so its `this` is always the complete
class. liblldb resolves each slot in whichever image holds it, so the rule works
across images.

- **Identifying a destructor.** DWARF has no destructor tag. A destructor is the
  member function named `~Class`, which is also how liblldb classifies it. The
  check is that the last `::` component of `SBFunction::GetName()` starts with
  `~`.
  - `GetName()` is the only spelling that is the same in both builds.
  - The CMake build passes `-dwarf-linkage-names=Abstract`. There,
    `GetBaseName()` is null and `GetMangledName()` is the bare `~Class`.
- **Why not the vtable's own symbol.** liblldb's dynamic typing maps
  `vtable for X` to `X` by a lookup by name, and it needs a process.
- **Precondition.** Every polymorphic class needs a virtual destructor.
  - `-Wnon-virtual-dtor -Werror` enforces this, except for classes that declare
    a protected or private non-virtual destructor, `final` classes, and
    third-party code.
  - The rule also needs full `-g` and no identical-code folding.
  - A class without a virtual destructor gets a report, never a wrong type.
  - An offline audit of all 1,456 vtables in a Debug JavaScriptCore and `jsc`
    (through `dyld_info -fixups`) found none wrong and 3 unresolved. All 3 are
    simdutf's `implementation` classes, which use the protected non-virtual
    destructor idiom.
- **Path and UUID together.** liblldb cannot load an image from its UUID alone,
  because no dSYM exists for Spotlight to find. Given a path, a wrong UUID gives
  an invalid module, so a rebuilt file is refused, never misread.
- **liblldb behaviour this relies on.**
  - Virtual-base offsets are given for the complete object.
  - A direct virtual base appears among both the direct bases and the virtual
    bases. Within one image, `SBType ==` tells them apart.
  - Anonymous-namespace types are spelled `JSCToolsTest::(anonymous namespace)::X`.

**Test.**
- A same-image object: a `StringFireDetail` held as a `FireDetail*`.
- A cross-image object: a `final` subclass defined in the test.
- A class whose slot 0 is an inherited, non-overridden virtual function. Without
  the `~` check, the base class comes back.
- A system-library object, which has no type on Darwin.
- Null and unreadable addresses.
- All of the above again after the target has been killed and reaped.

### 3. Values

`TargetValue` is an address and a type, read lazily. A failure is reported once
and gives an invalid value. Everything asked of an invalid value is silent, so a
chain of lookups reports only the step that failed.

- **Reads.**
  - `properField` returns only the fields a class declares itself.
  - `base`, `dereference` and `pointeeAt` navigate to other values.
  - `as<T>()` copies out a trivially copyable value.
  - `integer()` reads integers, and masks and sign-extends bitfields. Reading a
    bitfield any other way is reported.
- **`forEachField`** walks superclass by superclass. It visits the complete
  object's own fields, then those of each non-virtual base in turn, then those of
  each virtual base once, at the offset the complete object gives it.
- **Entry point.** `TargetValue::completeObjectAt(address)` returns the complete
  object that contains the polymorphic object at `address`, as its dynamic type.

**Test.**
- A one-bit, an unsigned 5-bit and a signed 4-bit bitfield. Reading one whole
  gives a report.
- A diamond, `Diamond : Left, Right`, with both `: virtual VirtualBase`. Every
  field is visited exactly once, with the right value.

### 4. Container wrappers

`Remote<T>` in `CorpseRemote.h` is a `TargetValue` whose `T` only names what the
walker expects. `T` picks the `RemoteTraits<T>` that says how to read it, the
way `visitChildren` does for a cell.

| Wrapper | How it reads the container |
|---|---|
| `Vector` | `m_buffer` and `m_size`. |
| `HashSet` | The table size, from the metadata before the buckets (`HashTable::tableSizeOffset`, copied because it is private). Buckets that WTF's own `isHashTraitsEmptyValue` or `Traits::isDeletedValue` calls empty or deleted are skipped. |
| `SentinelLinkedList` | From the sentinel's `m_next` back around to the sentinel. |

`CorpseLimits.h` bounds every count.

**Test.** A fixture holds a `Vector`, a `HashSet` with deleted entries, and a
`SentinelLinkedList`. The test reads them back in and out of process.

### 5. The JS heap walk on Darwin

`HeapWalk(snapshot, roots)` visits exactly the cells `MarkedSpace::forEachLiveCell`
would visit inside a `HeapIterationScope`.

**The roots.** The roots are `MyaRoots`, a class defined in `HeapWalkTest.cpp`.
The test records their address in the fixture it passes over the address pipe.
- `MyaRoots` is polymorphic, so its type comes from its vtable.
- Its field `VM* vm` gives the VM's type. `JSC::VM` has no vtable, so it can only
  be reached as a field type.
- Its fields `MarkedBlock::Header*` and `LocalAllocator*` are there only for their
  types, since no field reached from the VM has them.

**The walk follows JSC's call chain.**
1. It computes, read-only, what stopping every allocator would leave (see patch 7
   for why): `MarkedSpace::stopAllocating`, then `BlockDirectory::stopAllocating`,
   then `LocalAllocator::stopAllocating`, then
   `MarkedBlock::Handle::stopAllocating`.
2. For each block in `m_blocks.set()`, it takes `header().m_handle` and runs
   `MarkedBlock::Handle::forEachLiveCell`, which checks each cell with the locked
   path of `isLive`: the allocated block, then the newly allocated bits, then the
   marks.
3. It visits the precise allocations that are marked or newly allocated.

**Reused from JSC.**
- `WTF::BitSet`'s `get`, `setEachNthBit` and `clearEachNthBit`.
- `FreeList::isSentinel` and `FreeCell::descramble`.
- `MarkedSpace::nextVersion` and `MarkedBlock`'s geometry.
- `JSValue::decode` and `isCell`.

Routines that read through JSC's own `this` are copied. Templating JSC's versions
over a memory reader would add a layer to the GC's fast path to save about 100
lines here.

**The one difference.** `StructureID::decode` adds
`g_jscConfig.startOfStructureHeap`. The walk derives that start from
`structureStructure`, whose StructureID names itself.

**Test.**
- **Agreement with JSC.** As its last step, the target runs JSC's own
  `forEachLiveCell` and records the sorted cell list. The walk must produce
  exactly that list, in process, out of process, and after the target exits. The
  target allocates objects both before and after its last collection.
- **Consistency.** Every live JS cell agrees with its Structure. The walk finds a
  known object, its Structure, and a Date.
- **Mutations.** Removing any one liveness rule makes the lists differ: allocated
  blocks, newly allocated bits, stopped allocators, free-interval clearing, or the
  precise allocations' newly allocated bit.

### 6. Images on Linux

Linux does what Darwin does: it takes the loader's list and identifies each image
by path and build-id.
1. `/proc/<pid>/auxv` gives `AT_PHDR`. The executable's `PT_DYNAMIC` gives
   `DT_DEBUG`, which is `r_debug`. Its `r_map` is glibc's `link_map` list, which
   has the slide (`l_addr`) and the path (`l_name`) of every image.
2. Each image's build-id comes from its `PT_NOTE` in memory.
3. liblldb gets `AddModule(path, nullptr, buildID)` and
   `SetModuleLoadAddress(module, l_addr)`.

All of these are fixed ELF structs in memory. `ENABLE_MYA_HEAP` links WebKit's
own images with `-Wl,--build-id`. A library deleted or replaced after it loaded
gives no debug info, as on Darwin.

**Rejected: opening each file through `/proc/<pid>/map_files`.** That keeps a
deleted file readable, but costs more than it is worth:
- Opening a file there needs `CAP_CHECKPOINT_RESTORE` in the initial user
  namespace. File capabilities make a process and its children non-dumpable.
- Picking which mapping of a file holds its header needs a rule. In-process,
  liblldb maps whole ELF files below the loader's copy, and the kernel leaves
  holes between an executable's segments.
- liblldb caches modules by path, so each file has to keep one descriptor for
  the process's life.

**Speed.**
- **Cause.** Linux Debug developer builds use `-gsplit-dwarf` and link with
  `--gdb-index` (`DEBUG_FISSION`). liblldb reads Apple accelerator tables or
  `.debug_names`, never `.gdb_index`, so it indexes every `.dwo` file by hand. One
  load of libJavaScriptCore's debug info took 9.1 s and 2.4 GB, and the DebugInfo
  suite took 46 s, against 2 s on the Mac.
- **Fix.** Build with `-gpubnames` under `ENABLE_MYA_HEAP`, and drop liblldb's
  on-disk index cache.

**System debug info.** liblldb looks for a system library's debug info in
`/usr/lib/debug/.build-id`, by build-id, so it comes from the library's `-dbg` or
`-dbgsym` package. debuginfod is not used.

**Test.**
- The tests of patches 1 and 2, run on Linux.
- `malloc`'s image is listed, and a libc object resolves through `libc6-dbg`.
- The DebugInfo suite time, with and without `-gpubnames`.

### 7. Snapshots at a safe point

A snapshot is only taken at a safe point, where the mutator could start a
collection. At a safe point:
- no collection is in progress;
- no allocation or sweep is part-way through;
- no `stopAllocating`, `resumeAllocating` or `endMarking` is half done.

So the walk can rely on every invariant JSC keeps between collections, instead of
checking each cell. The tests snapshot a target after it collects and parks.

**Free lists survive safe points.** A cell an allocator hands out after a
collection is live, but no bitmap records it until `stopAllocating` runs. That is
why JSC runs `stopAllocating` before it iterates, and why the walk computes that
step's result from each allocator's free list (patch 5).

**The check.** The walk refuses a snapshot that is not at a safe point, and
reports it. The walk checks what it can see:
- `MarkedSpace::m_isMarking` is false;
- `Heap::m_collectionScope` is empty;
- no block header's lock is held.

Because of this check, the walk needs no liveness rules for a collection in
progress (`isMarking` and `marksConveyLivenessDuringMarking` in JSC).

**Test.** A snapshot taken inside a collection is reported and not walked.
`HeapObserver::willGarbageCollect` may run before the state the check reads is
set. If so, the test needs a GC helper thread stopped mid-mark.

### 8. How much of the heap the walk reaches

**Goal.** The walk's reach is the bytes it reaches divided by the bytes allocated,
together with a list of the largest allocations it misses. The list says what to
teach the walk next.

**Following C++ objects.** From each JS cell and C++ object it reaches, the walk
uses `forEachField` to follow every pointer whose pointee type is known. Each
address it reaches counts toward the allocation that contains it.

**Counting allocations: libpas, in the target itself.** The target enumerates its
own heap and records the result in the fixture, the same way it records JSC's own
list of live cells for patch 5. Nothing reads another process's allocator, and
WebKit doesn't change.
1. At the safe point, after recording its JS fixture, the target gets a
   `pas_root` from `pas_root_create`, or from
   `pas_root_ensure_for_libmalloc_enumeration` on Darwin.
2. It runs `pas_enumerator_create` and `pas_enumerator_enumerate_all`.
   - The reader returns the address it is given, because the memory is the
     process's own.
   - The recorder keeps each `pas_enumerator_object_record`: an address and a
     size.
3. The recorder writes into a buffer allocated up front with system malloc, so
   the enumeration never allocates from the heap it is enumerating.
4. The sorted list goes into the fixture.

This covers `fastMalloc`, IsoHeaps and TZone heaps, using the production allocator.

**System malloc** is out of scope at first, because WebKit's own heap is libpas.
If it matters later, the in-process options are libmalloc's zone enumerator on
Darwin (`malloc_get_all_zones(mach_task_self(), ...)`), and on Linux an
interposed `malloc` and `free` in the test executable.

**Open.**
- Does `pas_root_create` see every heap the process uses, on both platforms?
- How long does enumerating a VM's heap take? It runs before every snapshot.

**Test.** The fixture allocates known objects. Some are reachable from the roots
and some are leaked on purpose. Reach counts the first set and lists the second
as missed. The first number measured on a real VM becomes the baseline.

## Types declared in one image and defined in another

Field types come from the debug info of the image that described the type they
were reached through. An image often only declares a type it never uses whole,
and liblldb completes a declaration only from that same image.

For example, `testLibJSCTools` only declares
`std::pair<MarkedBlock::Handle *, MarkedBlock *>`, the element type of
`BlockDirectory::m_blocks`. A walk that reads that vector starting from the
test's `MyaRoots` fails in the CMake build. The Xcode build happens to complete
the type from another compile unit.

The walk above never reads that vector, and no patch here needs completion across
images. Walks into WebCore or system libraries will. Completion will then work
like this:
- look up the declaration's own qualified name in every image;
- accept it only if every definition agrees on size, bases and field offsets;
- refuse a type in an anonymous namespace, since another image's type of that
  name is a different type.

It lands with the first walk that needs it, which serves as its test.

## Build gotchas

- **SB API headers.** Xcode's `LLDB.framework` has no SB headers. They come from
  Homebrew's llvm (`/opt/homebrew/opt/llvm/include`, LLVM 22).
- **Ninja.** The CMake JSCOnly port on macOS enables Swift, so it needs a real
  ninja (`/opt/homebrew/bin/ninja`).
- **ARC.** JSCOnly on macOS compiles `wtf/darwin/OSLogPrintStream.mm` without ARC
  unless that file gets `-fobjc-arc`. This is a regression from 6f9fb91d518b.
- **Darwin but not Cocoa.** JSCOnly on macOS is `OS(DARWIN)` but not
  `PLATFORM(COCOA)`: `MachSendRight` is empty there, so `corpse/` must not use it.
  It also emits no dSYMs; liblldb reaches the debug info through the debug map to
  the `.o` files.
- **The build-system marker.** `build-jsc --jsc-only` or `--cmake` switches
  `WebKitBuild/BuildSystem` to CMake. The Xcode harness run then looks in
  `cmake-mac`, prints "not built", and still reports 0 failures. Run
  `build-jsc --xcode` again before the Xcode harness run.
- **The harness on macOS.** `run-javascriptcore-tests --jsc-only` is rejected on
  macOS; use `--root` as shown in Build and test.
- **CodeSign.** A rebuild after a failed build can fail in CodeSign on a stale
  `JavaScriptCore.framework/Versions/A/JavaScriptCore.cstemp`. Delete that file.
- **Warnings.** Xcode builds the tests with `-Werror=exit-time-destructors`, so a
  static fixture with a destructor needs `NeverDestroyed`. Both builds use
  `-Wnon-virtual-dtor -Werror`.
- **Linux workspaces.** The wk SDK ships liblldb-22 but not `liblldb-22-dev`, so
  `ENABLE_MYA_HEAP` is off there, and the DebugInfo and HeapWalk suites fail
  until the SDK has the headers. `wk test <ws> -- --testlibjsctools` needs the
  `--`.

## Where the branch stands

The branch is `dev/mya-heap-walk-with-uuid`. Both Mac builds pass 1,278
assertions. The heap walk matches JSC's `forEachLiveCell` on about 5,300 cells.
It differs from the plan above in four places:
- **Patch 4:** the container wrappers have no test of their own yet. Only the
  heap walk exercises them.
- **Patch 5:** the walk still has the liveness rules for a collection in
  progress, and no safe-point check.
- **Patch 6:** Linux still uses the `/proc/<pid>/map_files` version, without
  `-gpubnames`.
- **Patches 7 and 8:** not started.
