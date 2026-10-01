# mya: walking a JavaScriptCore heap from a snapshot

mya inspects a JavaScriptCore process from outside it. The corpse library
(`Source/JavaScriptCore/corpse/`) takes a snapshot of a process: a Darwin corpse,
or the live process on Linux. mya then reads that snapshot's memory.

This design adds three things:
- a walk of the snapshot's JS heap and the C++ objects hanging off it;
- every type taken from the process's own debug info, read through liblldb;
- a measurement of how much of the heap the walk reaches.

It lands as small patches, each with exactly the code its test needs. Patches 1
to 8 are implemented. Patches 9 to 12 are designed, under "The next patches",
from a measurement of what the walk still misses.

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
  pick how to read it (patch 11), but never finds one.
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
- Pointees are taken at their static type. Patches 9 to 11 say what the walk
  does not follow yet.

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
takes 1.4 to 1.7 s.

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

## The next patches

### What the walk misses, and why

On the test's VM, the walk misses about 984 KB of 1.92 MB. A conservative scan,
run once by hand, attributed each missed allocation to the first word in the
libpas heap that points into it:

| Missed | First pointed to from | Cause |
|---|---|---|
| 406 KB | no word in the libpas heap | Mostly 19 all-zero 16 KB blocks that are not MarkedBlocks, still unidentified. Also the test's own list of live cells (57 KB), and the two objects it leaks on purpose (12 KB). |
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

Patches 9 to 11 fix these, and patch 12 makes the scan a tool, so that each
patch's effect is measured the same way. Each patch adds a planted case to the
fixture, and the attribution test (patch 12) proves the walk follows it.

### 9. Declared types, completed

**The defect.** A compile unit describes only the types it uses whole. Every
other type it mentions, such as the pointee of a `std::unique_ptr` to a
forward-declared class, is a declaration with no size and no fields. liblldb
does not complete a declaration from another compile unit on its own, so today
the walk silently stops at every such pointer. Many of JSC's C++ objects are
reached only through such pointers (see the table above), and WebCore's will be
too. A walk
that stops there cannot measure anything, and one that stops silently hides it.

**Why not look the definition up by its name.** DWARF has no link from a
declaration to its definition other than the name: a forward declaration
carries no `DW_AT_signature` even with `-fdebug-types-section`. A lookup by
name can find the wrong type (one in another image, or in an anonymous
namespace), and it is slow for a WebCore-sized image. So the walk takes each
type from the code that uses it whole instead.

**A class's home description.** A function's `this` is described by the
compile unit that compiled the function. A destructor destroys every member
its class owns, so in its compile unit every owned member's type is complete:
the `std::unique_ptr<JSONCache>` that `VM` owns is complete in the compile unit
of `VM::~VM`, though not in the test's, from which the walk took `VM`. The walk
reads every class from its home description, its destructor's `this`:
1. **A polymorphic class** gets it from its vtable, as patch 2 does: no names.
2. **Any other class** gets it from its destructor's declaration, which the
   class's description lists (`SBType::GetMemberFunctionAtIndex`). The
   declaration carries the destructor's linkage name
   (`SBTypeMemberFunction::GetMangledName`). The walk resolves that symbol in the
   images' symbol tables, in the order the loader would, takes the function
   there, and the pointee of its `this`.
   - A linkage name is the ABI's name for one symbol, which the linker and the
     loader also use, so it names exactly one function; the walk never matches
     type names.
   - The function found must be a destructor (patch 2's check), and its class
     must have the same size as the description it came from.
   - This needs the declaration's linkage name. The Xcode build emits linkage
     names everywhere. The CMake build passes `-dwarf-linkage-names=Abstract`,
     which should keep them on declarations; that is the first thing this patch
     checks.
3. **A pointee that is only declared** may still be polymorphic. The walk tries
   its vtable first (patch 2's rule, which never guesses), then its owner's home
   description.

A class without an out-of-line destructor has no home description; it is read
as it is, and the walk counts what that leaves out (patch 12). Each class's home
description is found once per snapshot and cached.

**What this does not fix.** A raw pointer to a declared, non-polymorphic class
that its owner does not destroy may still be a declaration in the owner's home
description. Patch 12 counts these, by field, so that each can be taught.

**Test.**
- A class declared in one compile unit of the test and defined in another is
  owned through a `std::unique_ptr` by a class whose destructor is in the second
  compile unit. Reached from the roots, its fields read.
- A polymorphic class only declared where it is pointed to is read as its
  dynamic type.
- The element type of `BlockDirectory::m_blocks`, declared in the test and
  defined in JavaScriptCore, is reached from `MarkedSpace`'s home description.
- In the reach test, the `VM`'s `unique_ptr` members above are reached.

### 10. JS cells as their C++ classes

**Why this matters most.** mya exists to explain what a JS heap holds, and most
of what it holds hangs off JS cells: a string's characters, a code block's
bytecode and metadata, a global object's C++ state. The walk visits every live
cell (patch 5), but as a bare `JSCell`, so it follows nothing out of one. Every
pointer out of a JS cell must be followed.

**A cell's C++ class comes from its `ClassInfo`.** Every cell's Structure holds
the `ClassInfo` of its class (`Structure::m_classInfo`), and a `ClassInfo` is
the static member `s_info` of the class it describes. JS cells have no vtable,
and liblldb does not give the class that declares a static member from its
address, so this is the one place the walk goes from a name to a type. Every
answer is checked against the `ClassInfo` itself:
1. Read the cell's StructureID, decode it (patch 5), and read the Structure's
   `m_classInfo`.
2. Resolve that address to its symbol, such as `JSC::JSString::s_info`, in the
   image that holds it.
3. Look the enclosing class, `JSC::JSString`, up by that name in that image
   only, then take that class's home description (patch 9). This lookup is the
   exception the Rules allow.
4. Check, against the target's data:
   - the class has a static member `s_info` whose qualified name is the symbol's;
   - its size is the `ClassInfo`'s `staticClassSize`, which is `sizeof` the
     class it was made for;
   - the class of `ClassInfo::parentClass`, checked the same way, is one of its
     bases.

   Anything else is reported, never guessed.
5. Cache the class per `ClassInfo`, as the class of a vtable is cached.

**Cells bigger than their class.** A cell's size comes from its block. A cell may
be bigger than its class's `byteSize()`:
- variable-sized cells, such as a `JSFinalObject` with inline storage, a
  `JSLexicalEnvironment` with its variables, or a `JSCellButterfly`;
- a subclass that adds fields but reuses its base's `ClassInfo`.

The walk reads the class's fields and counts the rest of the cell as not
understood, by class (patch 12). Inline storage holds `JSValue`s, which point to
cells that are visited anyway.

**Edges out of a typed cell.**
- `WriteBarrier<T>`, `JSValue` and pointers to cells lead to cells the walk
  already visits; following them is harmless and gives the attribution its edges.
- Pointers to C++ objects are followed as in patch 8, which is what reaches a
  `CodeBlock`'s `UnlinkedCodeBlock` data, a global object's C++ members, and so
  on.
- Encoded fields, first of all `JSString::m_fiber`, need patch 11. A resolved
  string's fiber is its `StringImpl*`, and a rope's has `isRopeInPointer` set and
  holds its fibers compactly.

**Test.**
- Every live JS cell gets a C++ class, and each class is no larger than its
  cell. The fixture's object is a `JSFinalObject`, its date a `DateInstance`, and
  the global object a `JSGlobalObject`.
- A string made in the target (`"corpse-heap-walk"`) is a `JSString`, and its
  `StringImpl`'s allocation is reached.
- In the reach test, what JS cells lead to (110 KB above) is reached, except what
  patch 11 still has to teach.

### 11. The other pointers the walk does not follow

**Values read their own way.** Some values hold pointers the debug info cannot
describe: a tagged or compact pointer in an integer, a union, a buffer whose
length is in another field. `CorpseRemote.h` already reads some by the C++
source's own logic. The walk will pick a `RemoteTraits` by recognising the type
it has reached, by its qualified name, and fall back to `forEachField` otherwise.
In order of what they miss:
- `LazyUniqueRef` and `LazyRef` (`m_pointer`, with its tag bits).
- `JSString::m_fiber`, and `JSRopeString`'s compact fibers.
- `Vector`, `HashTable` (so `HashMap` and `HashSet`), `RefPtr`, `std::unique_ptr`
  and `std::unique_ptr<T[]>`, `CompactPtr`, `PackedPtr` and `CompactRefPtr`:
  every element or bucket, not only the first.
- Each wrapper takes its logic from the C++ source and names it, as the
  wrappers in patch 4 do. Each has a planted case in the fixture.

**C arrays.** `TargetType::Layout` gains an array: its element type and count,
from `SBType::GetArrayElementType` and the size. `forEachField` visits each
element of an array field.

**Dynamic types.** A pointer whose pointee is polymorphic is followed as the
complete object's dynamic type (patch 2), which is cached per vtable. Failing to
find it, such as for a class without a virtual destructor, is counted, not
reported: in a walk of a whole heap, it is an expected case.

**`void*`** and other pointers to types without a size are only followed through
a wrapper. Otherwise they are counted.

### 12. Attributing what the walk misses

**Goal.** Say, for every byte the walk misses, why, in terms a person can act
on: which class and field point to it, and why the walk did not follow that
field. The table above was made once, by hand, with a throwaway scan; its
figures are the baseline to beat. This makes that scan part of mya, and tests
it.

**Three sources, combined.**
- **What is live, from libpas.** The target's enumeration (patch 8) is what
  "missed" means: every byte is in a live libpas object, and the walk either
  reaches it or not. libpas also knows facts about each object that the walk
  does not: its heap, its size class, its page kind (segregated, bitfit or
  large), and, for a heap with a type, its `bmalloc_type`, with a size, an
  alignment and a name. An IsoHeap's type is one class. A TZone heap's is a
  bucket that several classes of one size and alignment share
  (`TZoneHeapManager`), so it narrows an object to a few classes, not one. The
  target records these facts with each object, from `pas_get_heap` and the
  heap's type, while it enumerates.
- **What the walk knows.** Whenever the walk sees a field it does not follow, it
  records an edge not followed: the value's class, the field, the address it
  holds if that lands in an allocation, and a reason: the pointee is an
  uncompleted declaration, the field is an array, an integer, a `void*`, a
  container past its first element, a polymorphic pointee without a dynamic
  type, a cell bigger than its class, or a pointer outside every allocation.
- **What the walk cannot see.** A conservative scan reads every word of every
  allocation, reached or not, and every word of the snapshot's other memory:
  images' writable data, thread stacks and registers (`Snapshot::threads()`), and
  the rest of the snapshot's regions. A word that points into a missed
  allocation is an edge. Words in a visited value are attributed to its class
  and field. Words in a live JS cell are attributed to the cell's class (patch
  10) and field. A word in an image's data is named by the symbol there, and a
  word on a stack by its thread.

**libpas checks the walk.** libpas's facts are independent of the debug info, so
the report also checks every object the walk reached:
- an object reached as a class must fit its allocation: the class's size is at
  most the allocation's, and in a segregated heap, its size class is the
  allocation's;
- an object reached in an IsoHeap or TZone heap must be of a class the heap's
  type fits: the same size class and alignment.

A failed check is a wrong type, so it is reported, with the field that led there.
This checks patches 9 and 10 too, from outside the debug info.

**The report.** `HeapWalk::reach` returns, with its totals:
- for each missed allocation, its edges: where each is, as a class and field, a
  cell's class, a symbol, a thread, or another missed allocation;
- the missed bytes grouped by the first edge's class, field and reason, largest
  first, which is the table above;
- the missed bytes by libpas heap and type, which explains an allocation that
  nothing points to, such as the 311 KB of zeroed 16 KB blocks above;
- the missed bytes with no edge at all, by size;
- every check libpas failed.

A missed allocation that only other missed allocations point to is attributed
through them: to the first edge on the way back that is not itself a missed
allocation. The report is the verbose output of the reach test, and a mya
command prints it for any snapshot.

**Avoiding what fooled the hand-made scan.** The throwaway scan found pointers
in the analysis's own memory: in process, in mya's local copies of the target's
pages, and out of process, in the fixture's list of allocations, which points
at every allocation. The tool:
- scans the snapshot's regions, never mya's own memory, so it runs out of
  process, or in process with mya's own mappings left out;
- leaves out the fixture's own buffers, which the fixture records;
- reads 8-byte words at 8-byte alignment, and, as a second pass, 6-byte packed
  pointers at 2-byte alignment, labelled as such.

**What it needs.** Two parts of the corpse library that only Darwin has:
- the snapshot's regions (`CorpseRegion`). On Linux they come from
  `/proc/<pid>/maps`, which a Linux snapshot can read, since the process is live;
- the snapshot's threads (`CorpseThread`), for stacks and registers. On Linux
  they come from `/proc/<pid>/task`, and their registers from ptrace.

**Test.**
- The fixture plants one missed object for each reason: behind an uncompleted
  declaration, in an integer, in a C array, as a `Vector`'s second element,
  behind a `void*`, only on a stack, and only in a global. The report attributes
  each to its reason, class and field, or to its thread or symbol. As patches 9
  to 11 teach the walk, each planted case moves from missed to reached.
- An object of a `WTF_MAKE_TZONE_ALLOCATED` class is reported with its TZone
  bucket, and a missed one is counted under it.
- A pointer planted with the wrong declared type, to an object of another size,
  fails libpas's check and is reported.

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
  configure finds no liblldb there and warns, and the DebugInfo and HeapWalk
  suites fail until the SDK has the headers. `wk test <ws> -- --testlibjsctools` needs the
  `--`.
- **Debian.** Debian 12's own liblldb is 18, which is too old (patch 6). Install
  `liblldb-22-dev` from apt.llvm.org, and `libstdc++6-dbgsym` from
  `debian-debug`, whose version must be libstdc++6's own. A build directory
  configured with an older liblldb needs `-ULLDB_INCLUDE_DIR -ULLDB_LIBRARY`.
- **Reads cost a mapping each.** `Memory` maps a page for each read and releases
  it with the last reader. A read that keeps nothing costs a `process_vm_readv`
  and an `munmap` on Linux. The walk reads per block, so it is fast; the tests'
  per-cell checks take about 100 µs a cell.

## Where the branch stands

The branch is `dev/mya-heap-walk-with-uuid`. Patches 1 to 8 are implemented;
9 to 12 are the next ones, and none of them is started. The 311 KB of zeroed
16 KB blocks that nothing points to are not identified yet; patch 12's libpas
facts should say what they are.

On Linux (Debian 12, aarch64, clang 18, liblldb 22), testLibJSCTools runs 416
assertions. All of them pass when libstdc++'s debug info is installed. Without
it, the system-library assertions fail and say why. The skipped suites and
cases are the ones printed as `SKIP`:
- tests after the target exits, since a Linux snapshot reads the live process;
- the Mach-O-only exports trie and memory arena suites, and the Region, Thread and
  Symbol suites, which need corpses Linux does not have yet.

The Mac builds have not been run since these changes. The Darwin code that
changed is: `TargetType` and `TargetValue`, the per-vtable cache,
`FindLLDB.cmake`'s version (from Homebrew's `liblldb.<version>.dylib`),
`CorpseImage.h`, the tests, `PlatformCocoa.cmake` and `CommonBase.xcconfig`.
