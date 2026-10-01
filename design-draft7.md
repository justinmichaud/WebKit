# mya: walking a JavaScriptCore heap from a snapshot

mya inspects a JavaScriptCore process from outside it. The corpse library
(`Source/JavaScriptCore/corpse/`) takes a snapshot of a process: a Darwin corpse,
or the live process on Linux. mya then reads that snapshot's memory.

This design adds three things:
- a walk of the snapshot's JS heap and the C++ objects hanging off it;
- every type taken from the process's own debug info, read through liblldb;
- a measurement of how much of the heap the walk reaches.

It lands as small patches, each with exactly the code its test needs. All 12
are implemented. Patches 9 to 12 were designed from a measurement of what the
walk missed after patch 8, and "What the walk still misses" measures it again.

This branch represents the final state. Iterate on this, we will split it up later.

## Terms

- **Target.** The process mya inspects.
- **Snapshot.** A read-only view of the target: a Darwin corpse, or, on Linux, the
  live process.
- **Image.** The executable or a shared library loaded in the target.
- **Fixture.** What a test's target builds before it is snapshotted, such as a
  VM with known objects, and records in a struct whose address it reports.
- **Roots.** `MyaRoots`, a test class whose fields give the walk its starting
  points and types (patch 5).
- **In process and out of process.** Each analysis test runs twice: on a
  snapshot of the test process itself, and on a snapshot of a copy of it that it
  spawns as the target and that reports its fixture over a pipe.
- **Reached and missed.** A live libpas allocation is reached if the walk reaches
  any address in it, and missed otherwise (patch 8).

## Rules

- **mya parses no binary formats.** liblldb reads Mach-O, ELF and DWARF. mya
  reads only fixed structs from memory, such as dyld's image list.
- **No type is looked up by name.** A type comes from code or data in the
  target: from a vtable (a class's dynamic type), from a function's `this`, or
  from a field's declared type. Fields are reached by name, as the C++ source
  reaches them. Two narrow exceptions are checked against the target's own data:
  - a member function is found by its linkage name, the ABI's unique name for one
    symbol, in the symbol table, as the loader finds it (patch 9);
  - a JS cell's class is found from its `ClassInfo`'s symbol, and checked against
    the `ClassInfo`'s own size and parent (patch 10).

  A name in mya's source may recognise a type the walk has already reached, to
  pick how to read it (patch 11), but never finds one. The one type a reader
  needs that its value does not give, `JSString::m_fiber`'s `StringImpl`, is
  taken from the classes the walk has reached.
- **liblldb is never given a process.** mya reads all memory itself
  (`RELEASE_ASSERT(!target.GetProcess().IsValid())`), so a walk never depends on
  the process still running.
- **Snapshots are taken at a safe point** (patch 7). The walk may assume the
  invariants JSC keeps between collections.
- **The production build does not change.** Nothing changes in JavaScriptCore,
  `jsc` or WebKit's web process. Objects that exist only for tests, such as the
  heap walk's roots, live in the tests. `ENABLE_MYA_HEAP`, which is on only in
  Debug developer builds, changes build flags only: RTTI, default visibility,
  full debug info, `-gpubnames` and an exported libpas.
- **Rebuilt files are out of scope.** An image is found by the path its loader
  recorded, and is assumed to be the file that was loaded.
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

On Linux the DebugInfo suite needs libstdc++'s debug info in
`/usr/lib/debug/.build-id` (patch 6). On Debian that is `libstdc++6-dbgsym`,
from the `debian-debug` archive. Without it, the system-library test fails.

`--verbose` prints, for the HeapWalk suite, the walk's reach and the
attribution report of patch 12, in and out of process.

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

**One `TargetType` per type.** `SnapshotDebugInfo` owns every `TargetType`, one
per liblldb type. It keeps them by name only to find candidates quickly; types
of one name are told apart with `SBType ==`. A type
reads its size when it is made and its layout the first time it is asked for,
so a field reached a million times is looked up in liblldb once. The class of
each vtable is found once too. A walk of a whole heap depends on both: before,
walking 200,000 objects took 530 s, with most of it spent re-reading layouts
from liblldb.

A flexible array member, such as `CStringBuffer`'s characters, starts where its
class ends. It is storage after the class, so it is not one of its fields.

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
  because no dSYM exists for Spotlight to find.
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
- All of the above again after the target has been killed and reaped. Only
  Darwin runs these: a Linux snapshot reads the live process, so the test says
  it skips them.

### 3. Values

`TargetValue` is an address and a type, read lazily. It keeps the
`SnapshotDebugInfo` that owns its type alive. A failure is reported once and
gives an invalid value. Everything asked of an invalid value is silent, so a
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

**Test.** A fixture in `DebugInfoTest.cpp` holds a `Vector`, a `HashSet` with
deleted entries, and a `SentinelLinkedList`. The test reads them back in and out
of process: the vector's elements, exactly the set's remaining values, and the
list's nodes in order.

### 5. The JS heap walk

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
1. It checks that the snapshot is at a safe point (patch 7).
2. It computes, read-only, what stopping every allocator would leave (see patch 7
   for why): `MarkedSpace::stopAllocating`, then `BlockDirectory::stopAllocating`,
   then `LocalAllocator::stopAllocating`, then
   `MarkedBlock::Handle::stopAllocating`.
3. For each block in `m_blocks.set()`, it takes `header().m_handle` and runs
   `MarkedBlock::Handle::forEachLiveCell`, which checks each cell with the locked
   path of `isLive`, with `isMarking` false: the allocated block, then the newly
   allocated bits, then the marks.
4. It visits the precise allocations that are marked or newly allocated.

**Reused from JSC.**
- `WTF::BitSet`'s `get`, `setEachNthBit` and `clearEachNthBit`.
- `FreeList::isSentinel` and `FreeCell::descramble`.
- `MarkedBlock`'s geometry.
- `JSValue::decode` and `isCell`.

Routines that read through JSC's own `this` are copied. Templating JSC's versions
over a memory reader would add a layer to the GC's fast path to save about 100
lines here.

**The one difference.** `StructureID::decode` adds
`g_jscConfig.startOfStructureHeap`. The walk derives that start from
`structureStructure`, whose StructureID names itself.

**Speed.** On a heap of 590,000 live cells, the walk takes under a second.

**Test.**
- **Agreement with JSC.** As its last step, the target runs JSC's own
  `forEachLiveCell` and records the sorted cell list. The walk must produce
  exactly that list, in process, out of process, and after the target exits. The
  target allocates objects both before and after its last collection.
- **Consistency.** Every live JS cell agrees with its Structure. The walk finds a
  known object, its Structure, and a Date.
- **Mutations.** Removing any one liveness rule makes the lists differ: allocated
  blocks, newly allocated bits, stopped allocators, free-interval clearing, or the
  precise allocations' newly allocated bit. This is checked by hand, by making
  each change and running the suite. Each one fails the agreement test both in
  and out of process.

### 6. Images and processes on Linux

Linux does what Darwin does: it takes the loader's list and identifies each image
by its path.
1. `/proc/<pid>/auxv` gives `AT_PHDR`. The executable's `PT_PHDR` gives its
   slide, and its `PT_DYNAMIC` gives `DT_DEBUG`, which is `r_debug`. Its `r_map`
   is glibc's `link_map` list, which has the slide (`l_addr`) and the path
   (`l_name`) of every image. Only the executable's entry has no name; its path
   is `/proc/<pid>/exe`.
2. liblldb gets `AddModule(path, nullptr, nullptr)` and
   `SetModuleLoadAddress(module, l_addr)`.

These are fixed ELF structs in memory. An image's `loadAddress()` is its
`l_addr`, which is where its header is in a position-independent image.

**Other processes.** `Process::attach` takes any live pid, and the snapshot reads
it with `process_vm_readv`, which needs the permission ptrace needs. A parent has
it over its child, even with Yama's `ptrace_scope` at 1, so the out-of-process
tests run on Linux too.

**Rejected: opening each file through `/proc/<pid>/map_files`.** That keeps a
deleted file readable, but costs more than it is worth:
- Opening a file there needs `CAP_CHECKPOINT_RESTORE` in the initial user
  namespace. File capabilities make a process and its children non-dumpable.
- Picking which mapping of a file holds its header needs a rule. In-process,
  liblldb maps whole ELF files below the loader's copy, and the kernel leaves
  holes between an executable's segments.
- liblldb caches modules by path, so each file has to keep one descriptor for
  the process's life.

**Rejected: checking build-ids.** Reading each image's `NT_GNU_BUILD_ID` and
passing it to `AddModule` would refuse a file rebuilt since it was loaded. That
is out of scope.

**liblldb 19 or newer.** liblldb 18 finds a loaded section by its start address,
so of two segments that start at one address it sees only one. An ELF `PT_TLS`
segment starts where the `PT_LOAD` holding `.tdata` does. In libstdc++, that
hides the rest of the segment, including the vtables. liblldb 19 fixes this,
and 22 was checked too. `FindLLDB.cmake` reads the version from the library's
file name, or from its install's directory, and `ENABLE_MYA_HEAP` refuses to
configure with an older liblldb.

**Speed.**
- **Cause.** Linux Debug developer builds use `-gsplit-dwarf` and link with
  `--gdb-index` (`DEBUG_FISSION`). liblldb reads Apple accelerator tables or
  `.debug_names`, never `.gdb_index`, so it indexes every `.dwo` file by hand. One
  load of libJavaScriptCore's debug info took 9.1 s and 2.4 GB, and the DebugInfo
  suite took 46 s, against 2 s on the Mac.
- **Fix.** Build with `-gpubnames` under `ENABLE_MYA_HEAP`, and drop liblldb's
  on-disk index cache.
- **Measured.** On Debian 12 (aarch64, liblldb 18), with `DEBUG_FISSION` on, the
  DebugInfo suite takes 5.2 s without `-gpubnames` and 3.0 s with it; with
  `DEBUG_FISSION` off it takes 2.9 s. The 46 s above was before `TargetType`
  read each type once (patch 2).

**System debug info.** liblldb looks for a system library's debug info in
`/usr/lib/debug/.build-id`, by build-id, so it comes from the library's `-dbgsym`
package. On Debian 12, `libstdc++6-12-dbg` does not provide it: it ships a
separate unstripped libstdc++ instead. `libstdc++6-dbgsym`, from the
`debian-debug` archive, does. debuginfod is not used.

**Test.**
- The tests of patches 1 to 5, run on Linux, except those after the target exits.
- Each image's path names the file `dladdr` gives for it.
- `malloc`'s image is listed.
- The system-library object is a `std::runtime_error`, in libstdc++; libc has no
  polymorphic classes. It resolves through libstdc++'s separate debug info. This
  needs liblldb 19.
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
- no block header's `m_lock` is held. Its `CountingLock::isHeldBit` is copied,
  because it is private.

Because of this check, the walk needs no liveness rules for a collection in
progress (`isMarking` and `marksConveyLivenessDuringMarking` in JSC).

**Test.** Two snapshots away from a safe point are reported and not walked, in
process and out of process:
- One is taken inside a full collection. `HeapObserver::willGarbageCollect` runs
  once `Heap::m_collectionScope` is set, so no GC helper thread has to be stopped
  mid-mark. In process, the analysis runs inside the observer. Out of process,
  the target reports its fixture from the observer and waits there.
- The other is taken while the target holds the lock of the block that holds the
  fixture's object.

### 8. How much of the heap the walk reaches

**Goal.** The walk's reach is the bytes of the allocations it reaches divided by
the bytes allocated, together with a list of the largest allocations it misses.
The list says what to teach the walk next.

**`HeapWalk::reach(allocations, missedCount)`.** An allocation is reached if the
walk reaches any address in it.
- Every live cell reaches its allocation.
- From the roots, the walk uses `forEachField` on each object. A field of class
  type is walked as an object in its own right. A pointer is followed when its
  pointee has a known, nonzero size and the pointee lies inside an allocation.
  That keeps pointers into static data, and the other members of unions read as
  pointers, from leading anywhere.
- Each object is walked once for each type it is reached as.
- Pointees are taken at their static type. Patches 9 to 11 make the walk read
  classes from their home descriptions, JS cells as their classes, and the
  values the debug info cannot describe.

**Counting allocations: libpas, in the target itself.** The target enumerates its
own heap and records the result in the fixture, the same way it records JSC's own
list of live cells for patch 5. Nothing reads another process's allocator, and
WebKit doesn't change.
1. At the safe point, after recording its JS fixture, the target takes
   `pas_heap_lock`, which libpas needs to create a root and which keeps libpas's
   other threads out of the heap. It gets a `pas_root` from `pas_root_create`.
2. It runs `pas_enumerator_create` and `pas_enumerator_enumerate_all`.
   - The reader returns the address it is given, because the memory is the
     process's own.
   - The recorder keeps each `pas_enumerator_object_record`: an address and a
     size.
3. The recorder writes into a buffer allocated up front with system malloc, so
   the enumeration never allocates from the heap it is enumerating.
4. The sorted list goes into the fixture.

libpas's API is hidden in WebKit's builds. `ENABLE_MYA_HEAP` builds libpas with
`PAS_BMALLOC_HIDDEN=0`, in CMake and in Xcode (`WK_MYA_HEAP`), so that the tests
can call it.

This covers `fastMalloc`, IsoHeaps and TZone heaps, using the production allocator.

**System malloc** is out of scope at first, because WebKit's own heap is libpas.
If it matters later, the in-process options are libmalloc's zone enumerator on
Darwin (`malloc_get_all_zones(mach_task_self(), ...)`), and on Linux an
interposed `malloc` and `free` in the test executable.

**Baseline.** On the test's VM on Linux, the walk reaches 936,064 of 1,920,272
bytes (48.7%) in 6,957 libpas allocations.
- The allocations that hold live JS cells are 683,440 bytes (35.6%). The C++
  objects followed from the roots add 252,624 bytes (13.2%).
- The live cells themselves are 211,440 bytes. An allocation counts whole once
  any address in it is reached, and a MarkedBlock is one allocation.
- The largest misses are 140 KB, 56 KB and 40 KB allocations.
- System malloc, which is not counted, held another 852,928 bytes.

**Speed.** On a heap of 590,000 live cells and 206,000 libpas allocations, `reach`
took 1.4 to 1.7 s. That was before patches 9 to 11, which walk every cell's
class and many more C++ objects, and has not been measured since.

**Open.**
- Every live JS cell is in an enumerated allocation (see Test), so the JS heap's
  blocks are seen. Whether every other heap the process uses is, on both
  platforms, is open.
- How long enumerating a large VM's heap takes. It runs before every snapshot,
  and has not been measured.

**Test.**
- **The enumeration.** No two enumerated allocations overlap. Every live cell is
  in one, so the JS heap's blocks are counted. The target allocates two
  `fastMalloc` objects that are reachable from the roots, in a chain, and two
  that it leaks, holding them only as integers. Each of the four is enumerated
  at its address, at least as large as it was allocated: libpas rounds up to
  its size class. A small and a large object freed just before the
  enumeration are not enumerated.
- **The reach.** The walk reaches the first two objects, and lists the leaked
  two as missed.

### What the walk missed after patch 8

On the test's VM, the walk missed about 984 KB of 1.92 MB. A conservative scan,
run once by hand, attributed each missed allocation to the first word in the
libpas heap that points into it:

| Missed | First pointed to from | Cause |
|---|---|---|
| 406 KB | no word in the libpas heap | Mostly 19 all-zero 16 KiB blocks (311 KB) that are not MarkedBlocks. Also the test's own list of live cells (57 KB), and the two objects it leaks on purpose (12 KB). |
| 277 KB | a C++ object the walk reached | The field is one the walk cannot follow: see below. |
| 190 KB | only other missed allocations | Follows from the rest. |
| 110 KB | a JS cell | The walk treats every cell as a bare `JSCell`. Mostly `JSString`'s `StringImpl` (74 KB in 2,300 strings), then `NativeExecutable`, `CodeBlock`, `UnlinkedCodeBlock`, `Structure` and `JSGlobalObject`. |

The fields the walk reached and could not follow:

| Field | Missed | Why |
|---|---|---|
| `LazyUniqueRef<VM, MegamorphicCache>::m_pointer` | 140 KB | A `uintptr_t` holding a tagged pointer. |
| `std::unique_ptr<JSONCache>`, and those to `BuiltinNames`, `BuiltinExecutables`, `RegExpCache`, `BytecodeIntrinsicRegistry` and `MicrotaskCallCache` | 48 KB | The pointee is only declared in the compile unit that describes `VM`, so it has no size. |
| `std::array<NumericStrings::StringWithJSString, 1024>` and the `CacheEntryWithJSString<int>` array | 51 KB | A C array, which the walk does not enter. |
| `Vector<std::pair<MarkedBlock::Handle*, MarkedBlock*>>::m_buffer` | 8 KB | The element type is only declared. |
| `HashMap`s and `Vector`s of pointers | | Only the first bucket or element is followed. |
| `CodePtr::m_value` | | A `void*`. |

Patches 9 to 11 fix these, and patch 12 makes the scan part of mya, so that
each patch's effect is measured the same way. Each patch adds a planted case to
the fixture.

### 9. Declared types, completed

**The defect, as measured.** A compile unit describes only the types it uses
whole. Every other type it mentions, such as the pointee of a `std::unique_ptr`
to a forward-declared class, is a declaration with no size and no fields.
Within one image, liblldb completes a declaration from the image's own
definition: a class declared in one compile unit of the test and defined in
another reads complete. Across images it does not. The test executable
declares `JSC::JSONCache`, which libJavaScriptCore defines, so from the test's
description of `VM`, `m_jsonCache` points to a type of size 0. The walk takes
`VM` from the test's roots, so it stopped at every such pointer.

**Why not look the definition up by its name.** DWARF has no link from a
declaration to its definition other than the name: a forward declaration
carries no `DW_AT_signature` even with `-fdebug-types-section`. A lookup by
name can find the wrong type (one in another image, or in an anonymous
namespace), and it is slow for a WebCore-sized image. So the walk takes each
type from the code that uses it whole instead.

**A class's home description** is the description of its destructor's `this`.
A destructor destroys every member its class owns, so the image that defines it
has every owned member's type complete. `TargetType::home()` finds it once per
type, and the walk reads every class it reaches as its home description:
- The class's description lists its destructor's declaration
  (`SBTypeMemberFunction` of kind `eMemberFunctionKindDestructor`), which
  carries the destructor's linkage name. The CMake build's
  `-dwarf-linkage-names=Abstract` keeps them on declarations: `VM`'s is
  `_ZN3JSC2VMD1Ev`. Apple's clang gives a declaration the unified name
  `_ZN3JSC2VMD4Ev`, which no symbol has, so the walk tries the complete-object
  (`D1`) and then the base-object (`D2`) destructor in its place.
- The walk resolves that symbol in the images' symbol tables, in the order the
  loader searches them, and skips undefined symbols: the test executable has
  `_ZN3JSC2VMD1Ev` as an import. It takes the function at the first definition,
  and the pointee of its `this`.
- The function must be a destructor (patch 2's check), and its class must have
  the size of the description it came from; a mismatch is reported.
- A class without an out-of-line destructor is its own home, and is counted
  (`ClassesWithoutHome`).
- A pointee that is still only declared may be polymorphic. The walk tries its
  vtable (patch 2's rule, which never guesses), quietly, through
  `SnapshotDebugInfo::dynamicTypeIfAnyAt`.

**Test.**
- The test's description of `VM` declares `m_jsonCache`'s pointee with no size,
  and `VM`'s home description, another description of the same size, defines
  it. A home description is its own home.
- The roots point to the VM's `RegExpCache`, which the test only declares; its
  vtable gives `JSC::RegExpCache`, with its size, at the VM's object.
- The VM's `JSONCache`, `BuiltinExecutables` and `RegExpCache` are reached.
  JS cells also reach the VM, as JavaScriptCore describes it (patch 10), so this
  alone does not isolate the patch. What does: with home descriptions, the walk
  meets 2 pointers to declarations, one of them planted; without, 84. The test
  allows at most 4.
- Dropped from the design: two compile units of the test, which liblldb
  completes on its own, and `BlockDirectory::m_blocks`, whose element type the
  test defines.

### 10. JS cells as their C++ classes

**Why this matters most.** mya exists to explain what a JS heap holds, and most
of what it holds hangs off JS cells: a string's characters, a code block's
bytecode and metadata, a global object's C++ state.

**A cell's C++ class comes from its `ClassInfo`** (`HeapWalk::cellClass`). JS
cells have no vtable, and liblldb does not give the class that declares a
static member from its address, so this is the one place the walk goes from a
name to a type. Every answer is checked against the target's data:
1. Read the cell's StructureID, decode it (patch 5), and read the Structure's
   `m_classInfo`.
2. Resolve that address to its symbol, such as `JSC::JSString::s_info`, in the
   image that holds it (`SnapshotDebugInfo::classOfStaticMember`).
3. Look the enclosing class up by that name, in that image only. The class must
   declare a static member `s_info` whose linkage name is the symbol's. The
   demangler spells nested template arguments `>>`, and liblldb's type names
   `> >`, so the name is respelled. An offline audit of libJavaScriptCore's 391
   `s_info` symbols: 367 resolve as spelled, and the other 24, all nested
   templates such as `JSGenericTypedArrayViewPrototype<JSGenericTypedArrayView<Int8Adaptor>>`,
   resolve respelled.
4. Check that the class's size is the `ClassInfo`'s `staticClassSize`, and that
   the class of `ClassInfo::parentClass`, checked the same way, is one of its
   bases. Anything else is reported, never guessed.
5. Cache the class per `ClassInfo`.

The walk reads each cell as its class's home description.

**Cells bigger than their class**, such as objects with inline storage, are
counted: 33.6 KB of the test VM's 5,244 cells.

**Test.**
- All 5,244 live JS cells get a class (88 classes), and no class is larger than
  its cell. The fixture's object is a `JSFinalObject`, its date a
  `DateInstance`, the global object a `JSGlobalObject`, and its name a
  `JSString`.
- The name is built with `join`, so it is not an atom and only its `JSString`
  holds its `StringImpl`. That `StringImpl`'s allocation is reached. (A literal
  would be an atom, which the atom table also reaches.)
- No cell is walked without a class.

### 11. The other pointers the walk does not follow

**Values read their own way.** The walk picks a reader by recognising the
qualified name of a class it has reached, and walks the class's members
otherwise. Each reader names the C++ source it mirrors:

| Class | How it is read |
|---|---|
| `WTF::Vector` | `VectorBufferBase`'s `m_buffer` and `m_size`: every element. The buffer's allocation is reached even with no elements. |
| `WTF::HashTable` (so `HashMap` and `HashSet`) | Every bucket of `m_table`, with the size before the buckets (`tableSizeOffset`, which is private). An empty or deleted bucket points into no allocation. The other members are walked as usual: a Debug `HashTable` has a `unique_ptr<Lock>`. |
| `WTF::LazyUniqueRef`, `WTF::LazyRef` | `m_pointer`, unless `lazyTag` or `initializingTag` is set, as the template argument's type. |
| `WTF::CompactPtr` (so `CompactRefPtr`) | `m_ptr`, decoded as `CompactPtr::decode` does. An outsized pointer, on a 36-bit build, is counted. |
| `WTF::PackedAlignedPtr` (so `Packed<T*>`) | The bytes of `m_storage`, shifted left by the alignment when `PackedAlignedPtr` stores it shifted to save a byte. The alignment is the second template argument (`TargetType::templateIntegerArgument`, through `SBType::GetTemplateArgumentValue`, which needs liblldb 19). |
| `JSC::JSString` | `m_fiber`: a resolved string's `StringImpl`, unless `isRopeInPointer` is set; a rope's fibers are cells. |
| `JSC::PropertyTable` | `m_indexVector`, less `isCompactFlag`, reaches the index buffer's allocation. |
| `std::unique_ptr<T[]>` | Its pointer member's elements. It records no count: `new T[n]` puts `n` in a cookie before the elements when `T` has a destructor (Itanium C++ ABI 2.7), and without one, as WTF's `UniqueArray` allocates, the elements are read to the end of their allocation, past the last by at most libpas's rounding up to its size class. |

A class's bases are walked as values of their own, so a reader recognises a
base: a `Packed<T*>` is a `PackedAlignedPtr`.

**C arrays.** `TargetType::Layout` has an `Array`: its element type and count.
The walk visits each element.

**Dynamic types.** A pointer to a polymorphic class is followed as the complete
object's dynamic type, quietly; failing to find one is counted
(`NoDynamicType`), and the pointer is followed as its static type.

**`void*`** and other pointers to types without a size are counted.

**Test.** Eight objects are planted behind the last element of a `Vector`, a
`HashSet`'s value, the last element of a C array, a `CompactPtr`, an
initialized `LazyUniqueRef`, a `Packed<T*>`, a `PackedAlignedPtr<T, 256>`
stored shifted, and the last element of a `UniqueArray`, each reached only
through it. Each is reached; removing the readers makes the Vector, HashSet,
CompactPtr, LazyUniqueRef, shifted and UniqueArray cases fail. (The C array's
needs only the array layout.)

### 12. Attributing what the walk misses

**Goal.** Say, for every byte the walk misses, why, in terms a person can act
on: which class and field point to it, and why the walk did not follow that
field. `HeapWalk::attribute(allocations, excluded, heapPages)` does it.

**Three sources, combined.**
- **What is live, from libpas.** As it enumerates its heap, the target records
  libpas's facts with each object (`HeapWalk::AllocationFacts`): its
  `pas_object_kind` from `pas_get_object_kind`, its heap from `pas_get_heap`,
  both with `bmalloc_heap_config`, and the heap's `bmalloc_type`'s size,
  alignment and name. Both lookups take the heap lock themselves, so they run
  after the enumeration. An object of another heap config, such as the JIT
  heap's, has no heap. The target also records libpas's payload records: the
  pages it holds objects in.
- **What the walk knows.** The walk records each value it reads that is not part
  of another: the roots, each JS cell (with the cell's size), and each value
  reached through a pointer or as a container's element.
- **What the walk cannot see.** A conservative scan reads every 8-byte word at
  8-byte alignment, then every 6-byte packed pointer at 2-byte alignment that is
  not the low bytes of a word, labelled as packed. It reads:
  - every allocation, reached or not;
  - the live part of each thread's stack, from its stack pointer less the
    128-byte red zone;
  - out of process, every other readable, writable, non-executable region,
    only its resident pages. One of the test target's reservations is 64 GB, so
    reading it whole is out of the question. On Linux `/proc/<pid>/pagemap`
    says which pages are present or swapped;
  - in process, only the images' writable sections, which liblldb lists: the
    analysis's own memory is in the other regions.

  The scan leaves out what the caller excludes (the target's lists of
  allocations and live cells), and the parts of libpas's pages that hold no
  live allocation, since they are free memory with stale words.

A word that points into a missed allocation is an edge. Its owner is, in order:
- the class and field of the innermost value the walk read that holds it, found
  by descending fields, bases and arrays and skipping empty classes such as a
  `unique_ptr`'s deleter; the field's type gives the reason: an integer, a
  `void*`, a pointer to a declaration, a pointer whose pointee does not fit, or
  another type;
- a JS cell's class, past the end of the class;
- another missed allocation;
- a reached allocation, outside every value read, by its libpas type;
- a thread, by id and name;
- a symbol in an image's data (`SnapshotDebugInfo::symbolAt`);
- the region, for other memory.

**Attribution.** Each missed allocation is attributed to its most telling edge:
one in a value the walk read, then a cell, image data, a stack, untyped bytes,
other memory, and last another missed allocation. One that only other missed
allocations point to is attributed through them, to the first edge on the way
back that is not in one.

**libpas checks the walk.** libpas's facts are independent of the debug info:
- a pointer to where an allocation starts whose pointee is bigger than the
  allocation is of the wrong type;
- an object reached at the start of an allocation of a heap with a type, such
  as an IsoHeap or a TZone heap, must be no bigger than the type allows.

A failed check is listed in the report with the field that led there.

**The report** (`HeapWalk::Attribution`):
- for each missed allocation, its first 16 edges and their count, and its
  attribution;
- the missed bytes grouped by attribution: reason and owner, with image data
  grouped by symbol;
- the missed bytes by libpas heap type;
- the missed allocations with no edge at all, by size;
- every failed check.

The verbose output of the HeapWalk suite prints it. A mya command does not yet:
the walk needs roots, which only the test defines.

**Linux.** The corpse library's regions and threads now work on Linux too:
- `Region::all` reads `/proc/<pid>/maps`; `Region::findContaining` and
  `Region::allWithPageCounts` read `smaps` for the resident and dirty page
  counts. A page that was only read maps the shared zero page, which Linux does
  not count as resident.
- `Thread` lists `/proc/<pid>/task`: names from `comm`, run state and times
  from `stat`, and, for a thread blocked in the kernel, the stack pointer from
  `syscall`, which needs no ptrace. A running thread has no stack pointer to
  read, and registers are not read.

The Region and Thread suites now run on Linux.

**Test.** The fixture plants one missed object for each kind of edge. The
fixture records their addresses complemented, so that it holds no pointer to
them itself:

| Planted object | Attributed to |
|---|---|
| held only in `MyaRoots::integer`, a `uintptr_t` | an integer in `MyaRoots::integer` |
| held only by that object | the same, through a missed allocation |
| held only in `MyaRoots::opaque`, a `void*` | a pointer to a type without a size |
| held only in `MyaRoots::declared`, a pointer to a class defined nowhere | a pointer to a class that is only declared |
| held only on a parked thread's stack | the thread `myaStackHolder` |
| held only in the global `onlyInAGlobal` | image data, by its symbol |
| a 64-byte object held only in `MyaRoots::wrongType`, a pointer to a 4096-byte class | a pointer whose pointee does not fit, and a failed check naming the field |

The objects the fixture leaks on purpose are attributed to the fixture. The
missed allocations are exactly what the walk does not reach, and no missed byte
is counted twice. In process and out of process, the only failed check is the
planted one.

**The TZone case**, on Darwin only, since TZone heaps exist only there
(`USE(TZONE_MALLOC)`). The roots hold one object of a `WTF_MAKE_TZONE_ALLOCATED`
class, and the fixture another, complemented. Both are in one TZone bucket,
whose type is the class's size class (`TZone::sizeClassFor`: 208 bytes for the
200-byte class), and the missed one is counted under that bucket. A bucket's
name is generated (`n1`, `L`), so the report names a heap with a type by its
name, size class and alignment: `L (112-byte objects aligned to 16)`. Making
the expected size class wrong fails the test.

### What the walk still misses

On the test's VM on Linux, the walk now reaches 1,478,832 of 1,969,472 bytes
(75.1%), against 48.7% after patch 8. Out of process, the attribution takes 6 s
in a Debug build. In the final run it attributed all but 448 of the 488,912
missed bytes; the figures vary by a few KB from run to run:

| Missed | Attributed to | What it is |
|---|---|---|
| 311 KB in 19 allocations | image data: `slots` | bmalloc's prefault supply (`bmalloc_prefault_supply.c`), 16 KB blocks prefaulted ahead of demand. These were the unidentified zeroed blocks. |
| 71 KB in 3 | image data: the test's `fixture` | Its own list of live cells (57 KB) and the objects it leaks on purpose. |
| 34 KB in 560 | image data: `JSC::intlAvailableTimeZoneEntries()::entries` | A function-local static cache, which nothing reached from the VM points to. |
| 9.6 KB in 123 | image data: `JSC::sharedCommonThunks()::thunks` | Likewise. |
| 8.7 KB in 2 | image data: `JSC::Interpreter::opcodeIDTable()::opcodeIDTable` | Likewise. |
| 8.5 KB in 58 | anonymous memory | Outside libpas's pages: system malloc. |
| 9.8 KB in 40 | `WTF::CodePtr<…>::m_value` | A `void*` to JIT code's memory. |
| 6 KB in 2 | `MyaRoots::integer` | Planted. |
| 5.8 KB in 39 | untyped bytes of reached allocations | Words in reached allocations outside every value read, such as dead cells in a MarkedBlock. |
| 5.3 KB in 38 | image data: WTF's `hashtable` | `ParkingLot`'s table. |
| 1.2 KB in 9 | `JSC::CodeBlock::m_jitData` | A pointer to a class only declared in JavaScriptCore itself. |

The rest is under 3 KB a group: the other planted objects, GC and JIT threads'
stacks, and `JSC::theGlobalJITWorklist`. What is left for the walk is mostly
memory that only static data holds. Reaching it needs roots in static data, not
better readers.

In process, the same attribution misplaces the prefault blocks: the analysis
allocates from libpas, which refills `slots` before the scan reads it. The
report is meant to run out of process.

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
  `-Wnon-virtual-dtor -Werror`. Only Xcode builds `corpse/` with
  `-Werror -Wunsafe-buffer-usage` and `-Wunnecessary-virtual-specifier`: search a
  `Vector`'s `span()` rather than the `Vector`, whose iterators are raw pointers,
  bracket libpas's headers with `WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN`/`END` as
  `JSDollarVM.cpp` does, and give a polymorphic class a virtual destructor only
  when it is not `final`.
- **libpas's headers in Xcode.** Xcode installs only the libpas headers marked
  private, and `pas_get_heap.h`, `pas_get_object_kind.h` and
  `pas_object_kind.h` are not. The test takes the heap from `bmalloc_get_heap`
  and mirrors `pas_get_object_kind` with the headers both builds install, so
  that what the framework installs does not change.
- **Linux workspaces.** The wk SDK ships liblldb-22 but not `liblldb-22-dev`, so
  configure finds no liblldb there and warns, and the DebugInfo and HeapWalk
  suites fail until the SDK has the headers. `wk test <ws> -- --testlibjsctools` needs the
  `--`.
- **Debian.** Debian 12's own liblldb is 18, which is too old (patch 6). Install
  `liblldb-22-dev` from apt.llvm.org, and `libstdc++6-dbgsym` from
  `debian-debug`, whose version must be libstdc++6's own. A build directory
  configured with an older liblldb needs `-ULLDB_INCLUDE_DIR -ULLDB_LIBRARY`.
- **liblldb and type units.** Each compile unit's `.debug_names` lists the type
  units it emitted (`-fdebug-types-section`), and LLD before 19 concatenates
  the indexes rather than merging them. Every type unit LLD discards as a
  duplicate leaves an entry with a tombstone offset: 371,947 of the 385,805 in
  libJavaScriptCore. liblldb reads those entries' DIEs in the wrong unit, and
  prints errors such as "GetDIE for DIE … is outside of its CU" or "abbreviation
  code … too big". On Linux, `SnapshotDebugInfo` sets
  `plugin.symbol-file.dwarf.ignore-file-indexes`, so liblldb indexes the DWARF
  itself; without `DEBUG_FISSION` that costs nothing measurable here (the
  HeapWalk suite ran in 19.7 s with it and 21.2 s without). With
  `DEBUG_FISSION`, the type units are in the `.dwo` files and the indexes name
  them by signature, so `-gpubnames` may help there again; that is unmeasured.
- **liblldb and function-pointer template arguments.** Under
  `-gsimple-template-names`, liblldb cannot name a class whose template argument
  is a function pointer, such as `ICUDeleter<&udat_close_72>`, and prints
  "refers to type … which was unable to be parsed". These are opaque ICU
  handles, and the walk loses nothing.
- **Reads cost a mapping each.** `Memory` maps a page for each read and releases
  it with the last reader. A read that keeps nothing costs a `process_vm_readv`
  and an `munmap` on Linux. The walk reads per block, so it is fast; the tests'
  per-cell checks take about 100 µs a cell.

## Where the branch stands

The branch is `dev/mya-heap-walk-with-uuid`. Patches 1 to 12 are implemented,
and tested on Linux and on macOS.

On macOS (arm64, Xcode Debug build, liblldb 22 from Homebrew), testLibJSCTools
runs 1,485 assertions, and `run-javascriptcore-tests --testlibjsctools` reports no
failures; only Process translation is skipped. The walk reaches 1,499,456 of
2,066,896 bytes (72.5%). The first Mac run found:
- home descriptions failed, since Apple's clang names a destructor's
  declaration `D4` (patch 9);
- the test's bound on pointers to declarations: on Darwin, WTF's `RunLoop`
  adds CoreFoundation's opaque `__CFRunLoop`, `__CFRunLoopSource` and
  `__CFRunLoopTimer`, which no image defines, so Darwin allows 7, against 84
  without home descriptions;
- the Xcode build's warnings and headers, under Build gotchas.

The `std::unique_ptr<T[]>` and shifted `PackedAlignedPtr` readers, the TZone
case and the Mac fixes came after the Linux run below, and have not been run on
Linux.

On Linux (Debian 12, aarch64, clang 18, liblldb 22), testLibJSCTools runs 545
assertions, and `run-javascriptcore-tests --testlibjsctools` reports no
failures. All of them pass when libstdc++'s debug info is installed. Without
it, the system-library assertions fail and say why. The skipped suites and
cases are the ones printed as `SKIP`:
- tests after the target exits, since a Linux snapshot reads the live process;
- the Mach-O-only exports trie and memory arena suites, and the Symbol suite,
  which needs corpses Linux does not have yet.

Each new test was checked against a mutation, by making the change by hand and
running the suite: walking classes as reached rather than as their home
descriptions, leaving out the readers, leaving out `JSString::m_fiber`, and
reading no cell as its class each fail it.

Open:
- A mya command that prints the report. The walk needs roots, and the list of
  allocations, which only a target that records them has: a production process
  defines neither, and the Rules keep it that way.
- The walk's speed on a large heap, since patches 9 to 11.
