# mya: walking a JavaScriptCore heap from a snapshot

mya inspects a JavaScriptCore process from outside it. The corpse library
(`Source/JavaScriptCore/corpse/`) takes a snapshot of a process, and mya reads
that snapshot's memory.

This design adds three things:
- a walk of the snapshot's JS heap and the C++ objects hanging off it;
- every type taken from the process's own debug info, read through liblldb;
- a measurement of how much of the heap the walk reaches.

The deliverable is a JSC VM test case that walks a whole heap, in process and
out of process. A mya command is out of scope. The design is the least code that
keeps the full heap walk working, checked and measured. It lands as the patches
below, each with exactly the code its test needs.

## Terms

- **Target.** The process mya inspects.
- **Snapshot.** A read-only copy of the target's memory: a Darwin corpse, or, on
  Linux, a forked copy of the target (patch 7). The corpse library can also read
  a live Linux process, for snapshots the target did not cooperate in.
- **Image.** The executable or a shared library loaded in the target.
- **Fixture.** What a test's target builds before it is snapshotted, such as a
  VM with known objects, and records in a struct whose address it reports.
- **Roots.** `MyaRoots`, a class in `HeapWalkTest.cpp` whose fields give the walk
  its starting points. It exists only in the tests.
- **In process and out of process.** Each analysis test runs twice: on a
  snapshot of the test process itself, and on a snapshot of a copy of it that it
  spawns as the target and that reports its fixture over a pipe.
- **Safe point.** mya's own: a moment the target enters on purpose, at which no
  thread can be changing the heap's liveness state, and which it verifies
  before it is snapshotted (patch 8).
- **Reached and missed.** A live libpas allocation is reached if the walk reaches
  any address in it, and missed otherwise.

## Scope

Each of these keeps code out. The walk still visits every live cell, checks the
safe point, and measures its reach under all of them.

| # | Assumption | What it keeps out |
|---|---|---|
| A1 | **Exact results need the target's cooperation.** A target that wants its heap measured enters mya's safe point and is snapshotted there. Any other snapshot is walked too, but its result is marked unverified. | Inferring a safe point from a snapshot after the fact, such as by unwinding every thread's stack. |
| A2 | **C++ objects are read through the image that defines them.** The walk enters JavaScriptCore's C++ through JS cells' classes (patch 10) and vtables (patch 2), from JavaScriptCore's own debug info. The test's descriptions are used only to find the heap (patch 5). | Completing a declared-only type from another image. |
| A3 | **Only libpas is counted.** System malloc is not. | A malloc-zone enumerator on Darwin and an interposer on Linux. |
| A4 | **Debug info is not split.** `ENABLE_MYA_HEAP` turns `DEBUG_FISSION` off. | Indexing `.dwo` files, and `-gpubnames`. |
| A5 | **One reader per container**, shared by the heap walk and the reach walk. | A second copy of each container's layout. |
| A6 | **liblldb 19 or newer.** 22 is the version tested. | Workarounds for liblldb 18. |
| A7 | **Only images outside the Darwin shared cache have types.** Apple ships no DWARF for the shared cache. | Typing system objects on Darwin. |
| A8 | **libpas's sources do not change, and the target is made quiet with options.** Counting allocations uses libpas's own enumerator API. A measured target runs with `useConcurrentGC=false`, `useConcurrentJIT=false` and `useWarmUpMarkedBlocks=false`, so no thread but the mutator touches the JS heap or the objects hanging off it, and no spare blocks sit in libpas. | Hooks or accessors inside libpas; races between the snapshot and the collector or compiler threads. |
| A9 | **Linux has glibc 2.34 and Linux 5.9 or newer**, for `_Fork` and `close_range` (patch 7). Debian 12 has both. | Fallbacks for older systems. |
| A10 | **Dynamic types need the image's symbol table**, to confirm them (patch 2). Debug builds keep it; in a stripped image the walk gives polymorphic objects their static types and counts them. | Any way to confirm a dynamic type without a second, unfoldable source. |

**Why A2 holds.** Within one image, liblldb completes a declaration from the
image's own definition: a class declared in one compile unit and defined in
another reads complete. Across images it does not. So JavaScriptCore's
description of `VM` has `m_jsonCache`'s `JSONCache` complete, and the test's,
which only declares `JSONCache`, does not. The pointers to declarations left are
those to classes no compile unit of the image defines, such as
`CodeBlock::m_jitData` and, on Darwin, CoreFoundation's opaque `__CFRunLoop`
types. The walk counts them.

**Rejected: completing a declaration from its destructor.** A class's
destructor's linkage name can be resolved in the loaded images' symbol tables,
and the pointee of its `this` is a complete description of the class. It works,
but it needs a search by linkage name across every image, and a rewrite of the
name for Apple's clang, which names a destructor's declaration with the unified
`D4` variant that no symbol has. A2 needs neither.

## Rules

- **mya parses no binary formats.** liblldb reads Mach-O, ELF and DWARF. mya
  reads only fixed structs from memory, such as dyld's image list and the ELF
  program headers and `r_debug` the loader leaves there.
- **liblldb is never asked for a type by name.** Every type comes from data in
  the target:
  - a vtable gives a class's dynamic type (patch 2);
  - a field, base, pointee or template argument gives its declared type;
  - a static data member's address gives the compile unit that defines it,
    among whose types its class is the one whose member has that address's
    linkage name (patch 10).

  A name may *recognise* a type the walk already holds, to pick how to read it
  (patch 11). Names are never compared across spellings: a liblldb type name is
  compared only with another liblldb type name, and a demangled name only with
  another from the same demangler.
- **liblldb is never given a process.** mya reads all memory itself
  (`RELEASE_ASSERT(!target.GetProcess().IsValid())`), so a walk never depends
  on the target still running.
- **The production build changes as little as possible.** `ENABLE_MYA_HEAP`, on
  only in Debug developer builds, changes build flags only (listed under Build
  and test). Objects that exist only for tests, such as the roots, live in the
  tests. A source change to JavaScriptCore, `jsc` or the web process is made
  only when the walk cannot be made reliable without it.
- **A rebuilt image is refused, not misread.** mya reads each image's identity
  from the target's memory, its Mach-O UUID or its ELF GNU build-id, and passes
  it with the path to `SBTarget::AddModule`. liblldb then refuses a file whose
  identity differs (`ModuleList::GetSharedModule` drops a module whose UUID is
  not the one asked for); it never loads the file anyway.
- **Code that reads JS cells mirrors JSC's heap code.** Each routine has a
  comment naming its original.
  - Trivially copyable WTF values (`WTF::BitSet`, `Markable`,
    `BlockDirectoryBits::Segment`) are copied out whole with `as<T>()`, which
    checks the size against the target's debug info, and read with their own
    methods.
  - Routines that read through JSC's own `this` are copied, and private
    constants are copied with a comment naming them. Templating JSC's versions
    over a memory reader would add a layer to the GC's fast path to save about
    100 lines here.
- **A failure is reported where the data was needed.** One unreadable image does
  not fail the snapshot. Every count is bounded by `CorpseLimits.h`.
- **The reach is computed and printed on every test run** (patch 9), so every
  change to the walk shows its effect on it.

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

`--verbose` prints, for the HeapWalk suite, the walk's reach, in and out of
process.

**What `ENABLE_MYA_HEAP` changes.** Build flags only, each for a stated reason:
- `-fstandalone-debug`, so that each image describes the types it uses;
- `-Wl,--build-id` on Linux, so that every image has an identity (patch 6):
  lld writes none unless asked, and only some distributions' clang asks;
- `PAS_BMALLOC_HIDDEN=0`, in CMake and in Xcode (`WK_MYA_HEAP`), so that the
  tests can call libpas's enumerator (patch 9);
- `DEBUG_FISSION` off (A4);
- default symbol visibility (see Open).

## The patches

### 1. Images on Darwin

`Snapshot::images()` lists each image outside the shared cache: its load address,
dyld's path for it, and its UUID. It pairs dyld's `uuidArray` with `infoArray` by
load address. The symbol lookup shares `Snapshot::dyldAllImageInfos()`. This
patch needs no liblldb.

- A path string is read one page at a time, because it can end just before an
  unmapped page.
- Shared-cache images are not in `uuidArray` (A7). A typical process has about 9
  images outside the shared cache, out of 400.

**Test.**
- The executable and JavaScriptCore are listed at their `dladdr` bases.
- Every image has a path and a UUID, and two images have two different UUIDs.
- `malloc`'s image is not listed.
- An invalid snapshot lists no images.

### 2. Debug info and dynamic types on Darwin

`SnapshotDebugInfo::create` gives liblldb an empty target, then, for each image,
calls `AddModule(path, nullptr, uuid)` and `SetModuleLoadAddress`. liblldb cannot
load an image from its UUID alone, because no dSYM exists for Spotlight to find;
the UUID is what refuses a rebuilt file.

`dynamicTypeAt(address)` returns the class of the complete object that contains a
polymorphic object. `TargetType` describes a type's layout, which is one of:
- a class: fields with their bit offsets, direct non-virtual bases, and virtual
  bases;
- a pointer or reference;
- an integer;
- a C array: its element type and count;
- anything else.

**One `TargetType` per type.** `SnapshotDebugInfo` owns every `TargetType`, one
per liblldb type, told apart with `SBType ==`. It indexes them by name only as a
cache key. A type reads its size when it is made and its layout the first time
it is asked for, and the class of each vtable is found once, so a field reached a
million times is looked up in liblldb once. Without this, a walk of 200,000
objects takes minutes, most of it re-reading layouts from liblldb.

A flexible array member, such as `CStringBuffer`'s characters, starts where its
class ends. It is storage after the class, so it is not one of its fields.

**One source, and one independent check.**
1. **The destructor rule** gives the type. Read the vtable pointer, then the
   offset to top, to find the complete object's vtable. The first slot that
   resolves to a destructor gives the type: the pointee of that destructor's
   `this`. Every class with a virtual destructor overrides it, so its `this` is
   the complete class. liblldb resolves each slot in whichever image holds it,
   so the rule works across images.
2. **The vtable's symbol** checks it. This rule never supplies a type: it only
   accepts or refuses the one rule 1 found. The vtable pointer lies inside a symbol
   `_ZTV…`, `vtable for X`. The class rule 1 found declares its destructor with
   a linkage name, `_ZN…D1Ev` (or `D4Ev` from Apple's clang), `X::~X()`. Both
   names are demangled by `abi::__cxa_demangle`, and the class each names must
   be the same string. Anything else, including a construction vtable
   (`_ZTC…`), is reported, and the object has no dynamic type.

Rule 1 alone can be wrong without saying so, because one function can be the
destructor of two classes:
- clang at `-O1` and above makes a derived class's destructor an alias of its
  base's when the derived one does nothing more
  (`TryEmitBaseDestructorAsAlias`), and the vtable slot then resolves to the
  base's;
- ld64 folds identical `linkonce_odr` functions unless given
  `-no_deduplicate`, and lld does with `--icf`.

Neither folds a vtable: ld64 folds only code, and lld only read-only sections,
while a vtable is relocated data. `ENABLE_MYA_HEAP` builds are Debug builds at
`-O0`, where clang makes no such alias and Xcode and the clang driver pass
`-no_deduplicate`, so in them the check never refuses. Rule 2 needs only the
image's symbol table, not RTTI; an image without one gives polymorphic objects
their static types (A10).

- **Identifying a destructor.** DWARF has no destructor tag. A destructor is the
  member function named `~Class`, which is also how liblldb classifies it. The
  check is that the last `::` component of `SBFunction::GetName()` starts with
  `~`. `GetName()` is the only spelling that is the same in both builds: the
  CMake build passes `-dwarf-linkage-names=Abstract`, where `GetBaseName()` is
  null and `GetMangledName()` is the bare `~Class`.
- **Rejected: the vtable's symbol as the source.** It names the class, but
  getting the type from a name is a lookup by name across every image. Rule 1
  finds the type from code, and the name only checks it.
- **Rejected: RTTI for the check.** The `type_info` in the vtable's RTTI
  slot names the class too, but needs `-frtti`, and the vtable's own symbol
  says the same without it.
- **Precondition.** Every polymorphic class needs a virtual destructor.
  - `-Wnon-virtual-dtor -Werror` enforces this, except for classes that declare
    a protected or private non-virtual destructor, `final` classes, and
    third-party code.
  - The rule also needs full `-g`.
  - A class without a virtual destructor gets a report, never a wrong type.
  - An offline audit of all 1,456 vtables in a Debug JavaScriptCore and `jsc`
    found none wrong and 3 unresolved, all simdutf `implementation` classes,
    which use the protected non-virtual destructor idiom.
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
- **Mutation**, checked by hand: making rule 1 return the base class makes rule
  2 refuse it.
- In the HeapWalk suite, the two rules disagree on no object the walk reaches.
- A system-library object, which has no type on Darwin.
- Null and unreadable addresses.
- An image whose UUID does not match is refused.
- All of the above again after the target has been killed and reaped.

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
- **`forEachField`** visits the complete object's own fields, then those of each
  non-virtual base in turn, then those of each virtual base once, at the offset
  the complete object gives it.
- **Entry point.** `TargetValue::completeObjectAt(address)` returns the complete
  object that contains the polymorphic object at `address`, as its dynamic type.

**Test.**
- A one-bit, an unsigned 5-bit and a signed 4-bit bitfield. Reading one whole
  gives a report.
- A diamond, `Diamond : Left, Right`, with both `: virtual VirtualBase`. Every
  field is visited exactly once, with the right value.

### 4. Container readers

One reader per container, in `CorpseRemote.h` (A5). The heap walk uses it through
the typed `Remote<T>`, whose `T` only names what the walker expects and picks its
`RemoteTraits<T>`, the way `visitChildren` does for a cell. The reach walk
(patch 11) uses the same reader through a `TargetValue`. The readers reuse WTF's
public logic, and copy anything private with a comment naming it.

| Container | How it is read |
|---|---|
| `Vector` | `VectorBufferBase`'s `m_buffer` and `m_size`. |
| `HashTable` (so `HashSet` and `HashMap`) | The table size, from the metadata before the buckets (`HashTable::tableSizeOffset`, private). Typed, a bucket that WTF's own `isHashTraitsEmptyValue` or `Traits::isDeletedValue` calls empty or deleted is skipped. Untyped, the reader mirrors WTF's default `HashTraits` for the key type: zero is empty, and all ones is deleted for integer, pointer, smart-pointer and `String` keys. A table whose key traits are not `WTF::HashTraits<K>` for one of those is not read, and is counted. Skipping deleted buckets matters: WTF destroys a deleted bucket's value but does not clear it, so a raw pointer there still holds its old address. |
| `SentinelLinkedList` | From the sentinel's `m_next` back around to the sentinel. |

**Test.** A fixture in `DebugInfoTest.cpp` holds a `Vector`, a `HashSet` with
deleted entries, a `HashMap` whose deleted entry's value is a raw pointer, and a
`SentinelLinkedList`. The test reads them back in and out
of process: the vector's elements, exactly the set's and the map's remaining
entries, and the list's nodes in order.

### 5. The JS heap walk

`HeapWalk(snapshot, roots)` visits exactly the cells `MarkedSpace::forEachLiveCell`
would visit inside a `HeapIterationScope`.

**The roots.** The test records the address of its `MyaRoots` in the fixture it
passes over the address pipe.
- `MyaRoots` is polymorphic, so its type comes from its vtable.
- Its field `VM* vm` gives the VM's address and the type the heap walk reads it
  as. `JSC::VM` has no vtable, so it can only be reached as a field type. The
  reach walk does not follow it; it reaches the VM through JS cells (A2).
- Its fields `MarkedBlock::Header*` and `LocalAllocator*` are type anchors: the
  walk computes where a block's header and an allocator are, and needs their
  types, which no field reached from the test's `VM` declares. Giving the walk
  a type that no data in the target can is what the roots are for.

**The walk follows JSC's call chain.**
1. It computes, read-only, what stopping every allocator would leave:
   `MarkedSpace::stopAllocating`, then `BlockDirectory::stopAllocating`, then
   `LocalAllocator::stopAllocating`, then `MarkedBlock::Handle::stopAllocating`.
   A cell an allocator handed out since the last collection is live, but no
   bitmap records it until `stopAllocating` runs, so the walk derives it from
   each allocator's free list.
2. For each block in `m_blocks.set()`, it takes `header().m_handle` and runs
   `MarkedBlock::Handle::forEachLiveCell`, which checks each cell with `isLive`,
   with `isMarking` false: the allocated block, then the newly allocated bits,
   then the marks.
3. It visits the precise allocations that are marked or newly allocated.

The walk has no liveness rules for a collection in progress (`isMarking` and
`marksConveyLivenessDuringMarking` in JSC): at mya's safe point (patch 8) none
is. On any other snapshot the walk still runs, within `CorpseLimits.h`, and
never crashes, but it marks its result unverified.

**Reused from JSC.**
- `WTF::BitSet`'s `get`, `setEachNthBit` and `clearEachNthBit`.
- `FreeList::isSentinel` and `FreeCell::descramble`.
- `MarkedBlock`'s geometry.
- `JSValue::decode` and `isCell`.

**The one difference.** `StructureID::decode` adds
`g_jscConfig.startOfStructureHeap`. The walk derives that start from
`structureStructure`, whose StructureID names itself.

**Speed.** On a heap of 590,000 live cells, the walk takes under a second.

**Test.** The target collects, allocates more objects, then is snapshotted at
mya's safe point.
- **Agreement with JSC.** As its last step, the target runs JSC's own
  `forEachLiveCell` and records the sorted cell list. The walk must produce
  exactly that list, in process, out of process, and after the target exits.
- **Consistency.** Every live JS cell agrees with its Structure. The walk finds a
  known object, its Structure, and a Date.
- **Mutations**, checked by hand: removing any one liveness rule (allocated
  blocks, newly allocated bits, stopped allocators, free-interval clearing, or
  the precise allocations' newly allocated bit) fails the agreement test in and
  out of process.

### 6. Images and debug info on Linux

Linux does what Darwin does: it takes the loader's list and identifies each image
by its path.
1. `/proc/<pid>/auxv` gives `AT_PHDR`. The executable's `PT_PHDR` gives its
   slide, and its `PT_DYNAMIC` gives `DT_DEBUG`, which is `r_debug`. Its `r_map`
   is glibc's `link_map` list, which has the slide (`l_addr`) and the path
   (`l_name`) of every image. Only the executable's entry has no name; its path
   is `/proc/<pid>/exe`.
2. Each image's build-id is read from memory. `/proc/<pid>/maps` gives the
   address of the file's mapping at offset 0, which holds its ELF header. The
   header gives the program headers, and the `PT_NOTE` segments, at `l_addr`
   plus their `p_vaddr`, hold the `NT_GNU_BUILD_ID` note. These are three fixed
   structs (`Elf64_Ehdr`, `Elf64_Phdr`, `Elf64_Nhdr`), as the executable's
   `PT_PHDR` and `PT_DYNAMIC` already are. An image without a build-id is
   reported and skipped.
3. liblldb gets `AddModule(path, nullptr, buildID)` and
   `SetModuleLoadAddress(module, l_addr)`.

An image's `loadAddress()` is its `l_addr`, which is where its header is in a
position-independent image.

**Rejected.**
- *Opening each file through `/proc/<pid>/map_files`*, which keeps a deleted
  file readable. It needs `CAP_CHECKPOINT_RESTORE`, which makes the process
  non-dumpable; it needs a rule for which mapping holds the header; and liblldb
  caches modules by path, so every descriptor stays open for the process's life.
- *Refusing mappings that `maps` marks `(deleted)`.* The mark appears only when
  the old file was unlinked. lld, GNU ld, gold and `install` replace the file,
  but `cp` rewrites it in place, and so does mold for an executable that is not
  running, so a rebuilt image can keep its inode and no mark.
- *Comparing the mapping's device and inode with `stat` of the path.* On
  overlayfs, which containers use, `maps` reports the underlying file system's
  device and inode before Linux 6.8, and `stat` can report a per-layer pseudo
  device even after.

**liblldb 19 or newer** (A6). liblldb 18 finds a loaded section by its start
address, so of two segments that start at one address it sees only one. An ELF
`PT_TLS` segment starts where the `PT_LOAD` holding `.tdata` does, and in
libstdc++ that hides the vtables. `FindLLDB.cmake` reads the version from the
library's file name, or from its install's directory, and `ENABLE_MYA_HEAP`
refuses to configure with an older liblldb.

**Speed.** With `DEBUG_FISSION` on, liblldb indexes every `.dwo` file by hand:
one load of libJavaScriptCore's debug info takes 9.1 s and 2.4 GB. With it off
(A4), the DebugInfo suite takes 2.9 s on Debian 12 aarch64, against 2 s on the
Mac.

**System debug info.** liblldb looks for a system library's debug info in
`/usr/lib/debug/.build-id`, by build-id, so it comes from the library's `-dbgsym`
package. On Debian 12, `libstdc++6-12-dbg` does not provide it;
`libstdc++6-dbgsym`, from the `debian-debug` archive, does. debuginfod is not
used.

**Test.**
- The tests of patches 1 to 5, run on Linux.
- Each image's path names the file `dladdr` gives for it.
- `malloc`'s image is listed.
- A copy of a loaded library, rebuilt with a different build-id and copied over
  the original with `cp` after loading, is refused.
- The system-library object is a `std::runtime_error`, in libstdc++; libc has no
  polymorphic classes. It resolves through libstdc++'s separate debug info.

### 7. Linux snapshots by fork

A Linux snapshot is a copy of the target, as a Darwin corpse is. The process
forks, and the child, which does nothing but wait, is the copy. Its memory is
the parent's at the moment of the fork, copy-on-write, so the parent runs on
unaffected and the copy outlives it. The corpse library already reads another
process with `process_vm_readv`, so it reads the copy the way it reads any
Linux process. Only taking the copy and keeping it alive are new.

One mechanism serves both cases:
- **In process**, `Snapshot` of the calling process forks it.
- **Out of process**, the target forks itself, inside mya's safe point
  (patch 8), and reports the copy to the analyzer.

A `Snapshot` of any other live process still reads that process directly, as a
best-effort view for targets that do not cooperate (A1).

**`pid_t Corpse::forkCopy(int lifetime)`** takes the copy:
1. Call `_Fork()`. Unlike `fork()`, it runs no `pthread_atfork` handlers and
   resets none of glibc's internal locks, such as malloc's: nothing runs in the
   parent that could take a lock in the middle of the snapshot, and nothing in
   the child changes the copy beyond the minimum glibc needs for
   async-signal-safe calls.
2. In the child, only async-signal-safe calls, since the parent's other threads
   do not exist there and may have held any lock: move `lifetime` to descriptor
   0, `close_range(1, ~0U, 0)`, then `read` descriptor 0 until it returns 0 or
   fails for any reason but `EINTR`, then `_exit(0)`. Closing every other
   descriptor matters: a later copy would otherwise inherit the write end of an
   earlier one's `lifetime` pipe and keep that copy alive.
3. In the parent, return the copy's pid.

**The protocol is the same on both platforms.** Inside mya's safe point, the
target takes its snapshot (in process) or reports and waits until the analyzer
has taken it (out of process). On Linux, out of process, what it reports is the
copy's pid, alongside its own. The analyzer releases it once its `Snapshot`
exists; the target may then run on or exit.

`_Fork` needs glibc 2.34, and `close_range` Linux 5.9 (A9).

**Lifetime.** A copy lives until the write end of its `lifetime` pipe closes:
when its snapshot is released, or when the pipe's owner dies, so a crash leaves
nothing behind. Every pipe end is `O_CLOEXEC`, so no spawned program keeps one.
- In process, `Snapshot` makes the pipe and keeps the write end. Releasing the
  snapshot closes it and reaps the copy with `waitpid`.
- Out of process, the analyzer makes the pipe, keeps the write end and passes
  the read end to the target when it spawns it, as it passes the address pipe.
  The `Snapshot` it builds owns the write end.

**Reading the copy.** `process_vm_readv` checks `PTRACE_MODE_ATTACH_REALCREDS`.
The reader is always an ancestor of the copy, with the same user: the parent in
process, and the target's parent out of process. Yama at `ptrace_scope` 1 allows
any ancestor (`task_is_descendant` walks `real_parent`). When a target exits
before its copy, the kernel reparents the copy to the nearest living ancestor
that set `PR_SET_CHILD_SUBREAPER`, which the analyzer does at startup, so the
copy becomes its child, stays readable, and is reaped by it. Without that, the
copy would go to init and Yama would refuse the read. A copy that exits first is
a zombie of its target until the target exits or reaps it.

Everything else the corpse library reads about a Linux process is the copy's
own and matches the target: `/proc/<pid>/maps` (regions, and the mappings
patch 6 reads build-ids through), `auxv` (the images of patch 6) and `exe` are
inherited across `fork`; the kernel copies the address space's mappings and its
saved auxiliary vector into the child.

**Threads.** `fork` copies only the calling thread, so the copy has one.
`Snapshot` reads the target's thread list from `/proc/<target>/task` when it is
made, while the target waits at its safe point, as it reads any Linux process's
today: names from `comm`, run state from `stat`, and, for a thread blocked in
the kernel, the stack pointer from `syscall`. The forking thread's stack pointer
is the copy's own thread's, since the copy runs on the forking thread's stack. A
running thread has no stack pointer.

**What the copy does not have.**
- Memory marked `MADV_DONTFORK` is not in the copy, and a read of it fails and
  is reported. Memory marked `MADV_WIPEONFORK` or `MAP_DROPPABLE` reads as zero
  in the copy; glibc 2.41 and later keeps its per-thread `getrandom` state
  there. Nothing in WebKit uses any of them.
- `MAP_SHARED` memory is shared with the target, not copied, so the copy sees
  the target's later writes to it. The walk reads none: the JS heap and libpas's
  pages are private.
- The copy is not instantaneous with respect to threads still running in the
  parent: the kernel copies the page tables one range after another, and a
  thread can write to a page before its range is copied. In a quiet target
  (A8) the other threads are idle helpers and libpas's scavenger, which returns
  free pages; none changes the JS heap's liveness state. A C++ object one of
  them changes during the fork can read half-updated, which the reach tolerates.

**Cost.** Page tables for the resident memory, and each page the parent writes
afterwards. A 64 GB reservation with little resident costs little.

**Rejected.**
- *Stopping the target under ptrace.* Out of process it would work, but in
  process it cannot: ptrace refuses a process's own thread group, and stopping
  the process would stop the analysis running in it. Using ptrace out of process
  and `fork` in process would be two mechanisms where one serves, and a target
  must cooperate to be at a safe point anyway.
- *Copying the target's memory into the analyzer.* It costs the resident size
  up front, and the regions of a live target change while they are copied.
- *`fork()`.* Its `pthread_atfork` handlers run in both processes.

**Test.** In process and out of process unless noted:
- A global the target changes after the snapshot reads as it was.
- An `atfork` child handler the test registers has not run in the copy.
- The thread list names the target's parked threads (`ParkedThreads`), each with
  a stack pointer inside a stack region.
- Releasing the snapshot ends the copy, and the copy is reaped. In process, of
  two snapshots, releasing the first ends only its copy.
- Out of process, the snapshot stays readable after the target exits, so
  `analyzeAfterTargetExits`, and every test that uses it, runs on Linux.
- The Image, Region and Thread suites run on copies.

### 8. mya's safe point

A safe point is a state the target enters on purpose and checks, not one mya
infers from a snapshot.

**Entering it.** `withMyaSafePoint(vm, function)` runs in the target. It lives
with the tests and changes nothing in JavaScriptCore:
1. Take the VM's `JSLock`, so no other thread can allocate cells or sweep.
2. Take a `DeferGC`, so this thread starts no collection.
3. Verify that no collection is running: `Heap::collectionScope()` is empty
   and `MarkedSpace::isMarking()` is false. In a quiet target (A8) collections
   run only on this thread, inside an allocation or an explicit collect, so the
   check holds whenever the test is in its own code; a failure is a bug in the
   test, and is reported, never retried.
4. Set `MyaRoots::atSafePoint`, run `function`, and clear it. `function` takes
   the snapshot as patch 7's protocol says.

The calling thread is outside the heap's code by construction, since it is in
the test's.

**Reading it.** The walk reads `MyaRoots::atSafePoint` from the snapshot. When
it is set, the result is exact and the tests compare it with JSC's own. When it
is not, the walk runs anyway and marks the result unverified (A1).

**Rejected: inferring a safe point from the snapshot.** Unwinding the lock
owner's stack and looking for frames in `heap/`, libpas, the LLInt or JIT code
needs every thread's registers and a frame-pointer unwinder, and a stack cannot
tell an idle collector thread from a working one.

**Test.**
- A snapshot taken inside `withMyaSafePoint` reads `atSafePoint` set, in and
  out of process.
- Inside a full collection, from `HeapObserver::willGarbageCollect`, the safe
  point refuses to snapshot.
- A snapshot taken outside it is walked and marked unverified.

### 9. How much of the heap the walk reaches

**Goal.** One number, the reach, and a list of the largest misses that says what
to teach the walk next.

**`HeapWalk::reach(allocations, excluded, missedCount)`.** An allocation is
reached if the walk reaches any address in it.
- Every live cell reaches its allocation.
- From the roots and every live cell, the walk uses `forEachField` on each
  object. A field of class type is walked as an object in its own right; a C
  array, element by element. A pointer is followed when its pointee has a known,
  nonzero size and its start lies inside an allocation. That keeps pointers
  into static data from leading anywhere.
- A pointer to a polymorphic class is followed as the complete object's dynamic
  type, quietly; failing to find one is counted (`NoDynamicType`), and the
  pointer is followed as its static type.
- `void*` and other pointers to types without a size are counted.
- Each object is walked once for each type it is reached as.

The reach can overcount: a union's other members, and fields that are not yet
initialised, are read as their declared types, and one that happens to hold an
address inside an allocation reaches it. Nothing reads memory that no declared
type covers, such as slack at the end of an allocation.

**The reach is computed, not derived by hand.** `Reach` holds the bytes
allocated, excluded and reached, and `Reach::percent()` is
`reached / (allocated − excluded)`. Only the program's own memory is in the
denominator:
- **Excluded by the caller.** The test passes the allocations it leaks on
  purpose, and those it plants to be missed, as `excluded`. They are expected
  misses, and the test checks them as such.
- **The test's own records are not in the heap.** The target allocates its lists
  of live cells and of allocations with system malloc, which is not counted (A3).
- **No spare memory.** With `useWarmUpMarkedBlocks=false` (A8), bmalloc's
  prefault supply holds no empty 16 KB blocks for future MarkedBlocks. They hold
  no objects and are reachable only from libpas's static `slots`. Other options
  that only keep spare memory may be turned off the same way.

The verbose output prints the percentage, the three byte counts, and the largest
missed allocations.

**Counting allocations: libpas, in the target itself.** The target enumerates its
own heap and records the result in the fixture, the same way it records JSC's own
list of live cells for patch 5. Nothing reads another process's allocator.
Everything here runs inside mya's safe point (patch 8), before the snapshot.
1. After recording its JS fixture, the target takes `pas_heap_lock`, which libpas
   needs to create a root and which keeps libpas's other threads out of the heap.
   It gets a `pas_root` from `pas_root_create`.
2. It runs `pas_enumerator_create` and `pas_enumerator_enumerate_all`.
   - The reader returns the address it is given, because the memory is the
     process's own.
   - The recorder keeps each `pas_enumerator_object_record`: an address and a
     size, into a buffer allocated up front with system malloc, so the
     enumeration never allocates from the heap it is enumerating.
3. The sorted list goes into the fixture.

This covers `fastMalloc`, IsoHeaps and TZone heaps, using the production
allocator.

**Test.**
- **The enumeration.** No two enumerated allocations overlap. Every live cell is
  in one, so the JS heap's blocks are counted. The target allocates two
  `fastMalloc` objects that are reachable from the roots, in a chain, and two
  that it leaks, holding them only as integers. Each of the four is enumerated
  at its address, at least as large as it was allocated: libpas rounds up to
  its size class. A small and a large object freed just before the
  enumeration are not enumerated.
- **The reach.** The walk reaches the first two objects, and the leaked two are
  missed. A C array's last element is reached through the array.
- **The percentage.** Excluding the leaked objects raises `percent()` by exactly
  their bytes' share; the excluded bytes equal their allocations' sizes.

### 10. JS cells as their C++ classes

**Why this matters most.** Most of what a JS heap holds hangs off JS cells: a
string's characters, a code block's bytecode and metadata, a global object's C++
state. It is also how the walk reaches JavaScriptCore's own description of every
C++ object (A2).

**A cell's C++ class comes from its `ClassInfo`** (`HeapWalk::cellClass`). JS
cells have no vtable, but each `ClassInfo` is a class's static `s_info`, and the
compile unit that defines `X::s_info` uses `X` whole (`CREATE_METHOD_TABLE(X)`
takes `sizeof(X)` and `X`'s members), so it describes `X` completely:
1. Read the cell's StructureID, decode it (patch 5), and read the Structure's
   `m_classInfo`.
2. `SBTarget::ResolveSymbolContextForAddress(address, eSymbolContextVariable)`.
   A data address is in no compile unit's code ranges, so liblldb finds the
   global variable that contains it (`SymbolFileDWARF::GetGlobalAranges`) and
   fills in the compile unit that defines it. The address must be where the
   variable starts.
3. Among that compile unit's classes (`SBCompileUnit::GetTypes`), the class is
   the one whose static `s_info` (`SBType::GetStaticFieldWithName`) has the
   linkage name of the symbol at the address. liblldb computes a member's
   linkage name with clang's own mangler from the class it built, so a match
   means this class declares that variable. Exactly one class must match.
4. Check that the class's size is the `ClassInfo`'s `staticClassSize`, and that
   the class of `ClassInfo::parentClass`, found the same way, is one of its
   bases.
5. Cache the class per `ClassInfo`, and the classes per compile unit.

A `ClassInfo` that fails a step is reported, never guessed. A cell whose class is
not found is counted, and the tests require none.

**Cost.** On Darwin, liblldb resolves the address through the debug map to the
one object file that defines it. On Linux, the first data address resolved in an
image makes liblldb list every compile unit's global variables once
(`GetGlobalAranges`), which reads their DIEs but not the classes. Each compile
unit's classes are listed once.

**Rejected.**
- *Looking the class up by the symbol's demangled name* (`SBModule::FindTypes`).
  liblldb spells type names as clang prints C++98 (`Foo<Bar<int> >`) and the
  demangler does not (`Foo<Bar<int>>`), so it needs the name respelled.
- *Listing the image's classes* (`SBModule::GetTypes`). liblldb builds a type for
  every class DIE of every compile unit, and on Darwin opens every object file:
  the whole of libJavaScriptCore's debug info.
- *A hand-written table in mya* of `{ X::info(), anchor<X> }`. No macro lists
  the cell classes (`FOR_EACH_JS_DYNAMIC_CAST_JS_TYPE_OVERLOAD` names 67 of
  about 390, and `DECLARE_INFO` expands inside each class); the table would be
  kept in step by hand; and the anchor's type would be mya's description of `X`,
  whose pointees that mya only declares have no size (A2).

**Cells bigger than their class**, such as objects with inline storage, are
counted.

**Test.**
- Every live JS cell gets a class, and no class is larger than its cell. The
  fixture's object is a `JSFinalObject`, its date a `DateInstance`, the global
  object a `JSGlobalObject`, and its name a `JSString`.
- The VM's `JSONCache`, `BuiltinExecutables` and `RegExpCache`, whose classes the
  test only declares, are reached through the cells, so they are read as
  JavaScriptCore describes them (A2). The pointers to declarations the walk
  meets are counted and printed, not bounded.
- No cell is walked without a class.

### 11. Values the debug info cannot describe

The walk picks a reader by recognising the qualified name of a class it has
reached, and walks the class's members otherwise. A class's bases are walked as
values of their own, so a reader recognises a base: a `Packed<T*>` is a
`PackedAlignedPtr`. Each reader names the C++ source it mirrors. `Vector` and
`HashTable` use patch 4's readers.

| Class | How it is read |
|---|---|
| `WTF::Vector` | Every element. The buffer's allocation is reached even with no elements. |
| `WTF::HashTable` | Every bucket. The other members are walked as usual: a Debug `HashTable` has a `unique_ptr<Lock>`. |
| `WTF::LazyUniqueRef`, `WTF::LazyRef` | `m_pointer`, unless `lazyTag` or `initializingTag` is set, as the template argument's type. |
| `WTF::CompactPtr` (so `CompactRefPtr`) | `m_ptr`, decoded as `CompactPtr::decode` does. An outsized pointer, on a 36-bit build, is counted. |
| `WTF::PackedAlignedPtr` (so `Packed<T*>`) | The bytes of `m_storage`, shifted left by the alignment when `PackedAlignedPtr` stores it shifted. The alignment is the second template argument (`SBType::GetTemplateArgumentValue`). |
| `JSC::JSString` | `m_fiber`: a resolved string's `StringImpl`, unless `isRopeInPointer` is set; a rope's fibers are cells. |
| `JSC::PropertyTable` | `m_indexVector`, less `isCompactFlag`, reaches the index buffer's allocation. |

**Rejected: a reader for `std::unique_ptr<T[]>`.** It records no count: `new
T[n]` puts `n` in a cookie only when `T` has a destructor (Itanium C++ ABI 2.7),
and WTF's `UniqueArray` allocates without one. Reading to the end of the
allocation would follow the stale words in the slack libpas leaves after the
last element.

`JSString::m_fiber` is a `uintptr_t`, so its value does not give `StringImpl`'s
type. The compile unit that defines `JSString::s_info` (patch 10) uses
`StringImpl` whole, since `JSString` reads its characters. The reader takes,
from that unit's classes, the one whose static `s_emptyAtomString` has the
linkage name `_ZN3WTF10StringImpl17s_emptyAtomStringE`. It does not depend on the
order in which the walk meets types.

**Test.** Seven objects are planted behind the last element of a `Vector`, a
`HashSet`'s value, a `CompactPtr`, an initialized `LazyUniqueRef`, a
`Packed<T*>`, a `PackedAlignedPtr<T, 256>` stored shifted, and a `JSString` built with `join` (so not an atom, and only the
`JSString` holds its `StringImpl`). Each is reached only through its holder, and
each is reached; removing a reader fails its case. An eighth, held only as the
stale value of a deleted `HashMap` entry, is missed.

### 12. How much of the heap the walk types

**Goal.** Check that the walk is not broken. Patch 9's reach counts an
allocation once the walk reaches any address in it, so a walk that reaches an
object and then misreads it still scores. The typed coverage counts bytes: how
much of the memory libpas reports the walk has read as some type.

**`Reach::bytesTyped`.** The union, over every value the walk read, of
`[address, address + sizeof(type))`, clipped to the allocations, so that a field
read inside an object, or an object read as two types, counts once.
`Reach::typedPercent()` is `bytesTyped / (allocated − excluded)`, with patch 9's
denominator.

**One adjustment to the denominator.** libpas reports a MarkedBlock as one
allocation, but its free atoms are the JS heap's free memory, as free memory
between libpas's objects is libpas's. The heap walk knows each block's live
cells (patch 5), so a MarkedBlock counts as its live cells and its header.

**Why it is not 100%.** Bytes that are in an allocation but in no declared type:
- libpas rounds an allocation up to its size class;
- a `Vector` or `HashTable` buffer's unused capacity;
- storage after the end of a class: a JS object's inline properties, a
  `StringImpl`'s characters, a flexible array member;
- objects the walk does not reach (patch 9's misses).

A trailing array makes an allocation bigger than its class, so its elements
lower the coverage; they are untyped bytes, as above. The opposite, a value
whose type runs past the end of its allocation, means the walk read the object
as a type bigger than it is. Its bytes past the end are not counted, and the
report lists each such value with the field that led to it.

**The report**, printed by the HeapWalk suite's verbose output: the typed
percentage, and the allocations with the most untyped bytes, each with the type
the walk read at its start. A class that recurs there has trailing storage or
a missing reader.

**Test.** In process and out of process:
- Each planted object of patch 11 is typed whole.
- **Mutations**, checked by hand: reading cells as bare `JSCell`s (no patch 10),
  or removing a reader of patch 11, lowers the typed percentage.

## What the walk misses

On the test's VM, the misses fall into these groups, largest first:
- **Function-local static caches** that nothing reached from the VM points to:
  `intlAvailableTimeZoneEntries()::entries`, `sharedCommonThunks()::thunks`,
  `Interpreter::opcodeIDTable()::opcodeIDTable`.
- **JIT code's memory**, behind `CodePtr::m_value`, a `void*`.
- **System malloc** (A3), which shows up as anonymous memory.
- **Untyped bytes of reached allocations**, such as dead cells in a MarkedBlock.
- **WTF's `ParkingLot` table**, held by a static.
- **Classes no compile unit of their image defines**, such as
  `CodeBlock::m_jitData`'s.

Most of what is left is held only by static data. Reaching it needs roots in
static data, not better readers.

## Build gotchas

- **SB API headers.** Xcode's `LLDB.framework` has no SB headers. They come from
  Homebrew's llvm (`/opt/homebrew/opt/llvm/include`, LLVM 22).
- **Ninja.** The CMake JSCOnly port on macOS enables Swift, so it needs a real
  ninja (`/opt/homebrew/bin/ninja`).
- **ARC.** JSCOnly on macOS compiles `wtf/darwin/OSLogPrintStream.mm` without ARC
  unless that file gets `-fobjc-arc`.
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
  private. The tests use only those, so that what the framework installs does
  not change.
- **Linux workspaces.** The wk SDK ships liblldb-22 but not `liblldb-22-dev`, so
  configure finds no liblldb there and warns, and the DebugInfo and HeapWalk
  suites fail until the SDK has the headers. `wk test <ws> -- --testlibjsctools`
  needs the `--`.
- **Debian.** Debian 12's own liblldb is 18, which is too old (A6). Install
  `liblldb-22-dev` from apt.llvm.org, and `libstdc++6-dbgsym` from
  `debian-debug`, whose version must be libstdc++6's own. A build directory
  configured with an older liblldb needs `-ULLDB_INCLUDE_DIR -ULLDB_LIBRARY`.
- **liblldb and type units.** LLD before 19 concatenates the compile units'
  `.debug_names` rather than merging them, and every type unit it discards as a
  duplicate leaves an entry with a tombstone offset: 371,947 of the 385,805 in
  libJavaScriptCore. liblldb reads those entries' DIEs in the wrong unit, and
  prints errors such as "GetDIE for DIE … is outside of its CU". On Linux,
  `SnapshotDebugInfo` sets `plugin.symbol-file.dwarf.ignore-file-indexes`, so
  liblldb indexes the DWARF itself, at no measurable cost without
  `DEBUG_FISSION` (A4). For the same reason `-gpubnames` would buy nothing.
- **liblldb and function-pointer template arguments.** Under
  `-gsimple-template-names`, liblldb cannot name a class whose template argument
  is a function pointer, such as `ICUDeleter<&udat_close_72>`, and prints
  "refers to type … which was unable to be parsed". These are opaque ICU
  handles, and the walk loses nothing.
- **Reads cost a mapping each.** `Memory` maps a page for each read and releases
  it with the last reader. The walk reads per block, so it is fast; the tests'
  per-cell checks take about 100 µs a cell.

## Open

- Whether default symbol visibility under `ENABLE_MYA_HEAP` is needed, and by
  which test.
- The walk's speed, with cells read as their classes, on a 590,000-cell heap.
- How long the target's libpas enumeration takes on a large heap.
- Whether every heap the process uses, other than the JS heap's blocks, is
  enumerated on both platforms.
