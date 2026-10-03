# mya: walking a JavaScriptCore heap from a snapshot

mya inspects a JavaScriptCore process from outside it. The corpse library
(`Source/JavaScriptCore/corpse/`) takes a snapshot of a process, and mya reads
that snapshot's memory.

This design adds four things:
- a walk of the snapshot's JS heap and of the C++ objects hanging off it;
- every type taken from the process's own debug info, read through liblldb;
- a measurement of how much of the heap the walk reaches and reads as a type,
  with an explanation of every allocation it misses;
- `mya heap`, which walks the heap of any process built with
  `ENABLE_MYA_HEAP`, such as a web process.

The tests are a JSC VM test case that walks a whole heap, in process and out of
process, at a safe point the target enters. `mya heap` walks a target that did
not cooperate, and says so. The design is the least code that keeps the full
heap walk working, checked, measured and fast. It lands as the patches below,
each with exactly the code its test needs.

## Terms

- **Target.** The process mya inspects.
- **Snapshot.** A read-only copy of the target's memory: a Darwin corpse, or, on
  Linux, a forked copy of the target (patch 7). The corpse library can also read
  a live Linux process, for snapshots the target did not cooperate in.
- **Image.** The executable or a shared library loaded in the target.
- **Fixture.** What a test's target builds before it is snapshotted, such as a
  VM with known objects, and records in a struct whose address it reports.
- **Roots.** `MyaRoots`, a class in `HeapWalkTest.cpp` whose fields give the walk
  its starting points in a test. It exists only in the tests. `mya heap` starts
  from a VM instead (patch 16).
- **In process and out of process.** Each analysis test runs twice: on a
  snapshot of the test process itself, and on a snapshot of a copy of it that it
  spawns as the target and that reports its fixture over a pipe.
- **Safe point.** mya's own: a moment the target enters on purpose, at which no
  thread can be changing the heap's liveness state, and which it verifies
  before it is snapshotted (patch 8).
- **Reached and missed.** A live libpas allocation is reached if the walk reaches
  any address in it, and missed otherwise.
- **Typed.** A byte of a live libpas allocation is typed if the walk read a value
  of some type that covers it.

## Scope

Each of these keeps code out. The walk still visits every live cell, checks the
safe point, and measures its reach under all of them.

| # | Assumption | What it keeps out |
|---|---|---|
| A1 | **Exact results need the target's cooperation.** A target that wants its heap measured enters mya's safe point and is snapshotted there. Any other snapshot is walked too, but its result is marked unverified. | Inferring a safe point from a snapshot after the fact, such as by unwinding every thread's stack. |
| A2 | **C++ objects are read through the image that defines them.** The walk enters JavaScriptCore's C++ through JS cells' classes (patch 10) and vtables (patch 2), and every other image's through its global variables and vtables (patch 13). | Completing a declared-only type from another image. |
| A3 | **Only libpas is counted.** System malloc is not. | A malloc-zone enumerator for libmalloc on Darwin and an interposer on Linux. |
| A4 | **Debug info is not split.** `ENABLE_MYA_HEAP` turns `DEBUG_FISSION` off. | Indexing `.dwo` files, and `-gpubnames`. |
| A5 | **One reader per container**, shared by the heap walk and the reach walk. | A second copy of each container's layout. |
| A6 | **liblldb 19 or newer.** 22 is the version tested. | Workarounds for liblldb 18. |
| A7 | **Only images outside the Darwin shared cache have types.** Apple ships no DWARF for the shared cache. | Typing system objects on Darwin. |
| A8 | **libpas's sources do not change, and a measured target is made quiet with options.** Counting allocations uses libpas's own enumerator API. A measured target runs with `useConcurrentGC=false`, `useConcurrentJIT=false` and `useWarmUpMarkedBlocks=false`, so no thread but the mutator touches the JS heap or the objects hanging off it, and no spare blocks sit in libpas. | Hooks or accessors inside libpas; races between the snapshot and the collector or compiler threads. |
| A9 | **Linux has glibc 2.34 and Linux 5.9 or newer**, for `_Fork` and `close_range` (patch 7). Debian 12 has both. | Fallbacks for older systems. |
| A10 | **Dynamic types need the image's symbol table**, to confirm them (patch 2). Debug builds keep it; in a stripped image the walk gives polymorphic objects their static types and counts them. | Any way to confirm a dynamic type without a second, unfoldable source. |
| A11 | **On Linux, an image's file is not overwritten in place while the target runs.** A new file renamed over it, as linkers, `install` and package managers write one, is detected (patch 6). | Any check for a file rewritten in place. |
| A12 | **mya runs the target's own build of JavaScriptCore.** libpas is linked into JavaScriptCore, and its enumerator reads a target's heap through structs whose layout is its own (patch 9). | A second, version-tolerant libpas enumerator. |

**Why A2 holds.** Within one image, liblldb completes a declaration from the
image's own definition: a class declared in one compile unit and defined in
another reads complete. Across images it does not. So JavaScriptCore's
description of `VM` has `m_jsonCache`'s `JSONCache` complete, and the test's,
which only declares `JSONCache`, does not. The walk reads the test's `VM` only
to find the heap; the cells lead to the VM again, as JavaScriptCore describes
it. liblldb does not complete every declaration from the image's definition,
though: in WebCore, a `UniqueRef<CSS::ColorMix>` in a `CSS::Color` variant
points to a `ColorMix` liblldb leaves declared, while the same image defines it.
So a pointer to a declaration is followed as the class the declaration's own
image defines under its liblldb name (`SnapshotDebugInfo::definitionInItsImage`).
liblldb knows no image for some declarations, `ColorMix` among them; one of
those is followed only if exactly one image defines a class of its name, since
images built with different options may define one class differently. On
google.com this follows 100 more pointers, and reaches 0.06% more of the heap. The pointers to declarations left are those to
classes no compile unit of the image defines, such as CoreFoundation's opaque
`__CFRunLoop` types on Darwin. The walk counts them.

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
- **Every type comes from data in the target, or from the image that defines
  it.**
  - A vtable gives a class's dynamic type (patch 2).
  - A field, base, pointee or template argument gives its declared type.
  - A global variable's symbol gives its type, as the debug info describes the
    variable at that address (patch 13).
  - A cell's ClassInfo is a class's `s_info`, and the symbol at that address
    names the class (patch 10). The name is only a candidate: the class is
    accepted only if liblldb's linkage name for its `s_info` is the symbol's.
  - Each of the few classes the walk computes an address for, rather than
    reading one, such as `JSC::MarkedBlock::Header`, is asked for by name in
    JavaScriptCore's image, the one image that defines it.

  A name may *recognise* a type the walk already holds, to pick how to read it
  (patch 11). Names are never compared across spellings: a liblldb type name is
  compared only with another liblldb type name, and a demangled name only with
  another from the same demangler. A demangled name is given to liblldb only as
  a candidate that a linkage name then confirms.
- **liblldb is never given a process.** mya reads all memory itself
  (`RELEASE_ASSERT(!target.GetProcess().IsValid())`), so a walk never depends
  on the target still running.
- **The production build changes as little as possible.** `ENABLE_MYA_HEAP`, on
  only in Debug builds (on macOS, and in Linux developer builds), changes build
  flags (listed under Build and test) and `hashTraitsDeleteBucket` in WTF
  (patch 11). Objects
  that exist only for tests, such as the roots, live in the tests. A source
  change to JavaScriptCore, `jsc` or the web process is made only when the walk
  cannot be made reliable without it.
- **A rebuilt image is refused, not misread.** mya reads each image's identity
  from the target's memory, its Mach-O UUID or its ELF GNU build-id, and passes
  it with the path to `SBTarget::AddModule`. liblldb then refuses a file whose
  identity differs (`ModuleList::GetSharedModule` drops a module whose UUID is
  not the one asked for); it never loads the file anyway. On Linux a file
  replaced by rename is caught before that, by its mapping (patch 6, A11).
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
- **The reach and the typed coverage are computed and printed on every test
  run** (patches 9 and 12), so every change to the walk shows its effect on
  them.

## Build and test

Build on the machine you are on, the harness is broken. Do not build on a
different machine or in a workspace for now.

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

A web process (patch 16):

```
Tools/Scripts/build-webkit --debug
Tools/Scripts/run-minibrowser --debug https://www.google.com
printf 'snapshot --pid <WebContent pid>\nheap\n' | DYLD_FRAMEWORK_PATH=WebKitBuild/Debug WebKitBuild/Debug/mya
```

On Linux the DebugInfo suite needs libstdc++'s debug info in
`/usr/lib/debug/.build-id` (patch 6). On Debian that is `libstdc++6-dbgsym`,
from the `debian-debug` archive. Without it, the system-library test fails.

`--verbose` prints, for the HeapWalk suite, the walk's reach and typed coverage,
in and out of process, with the largest misses and untyped allocations.

On Darwin, mya and the tests link Xcode's `LLDB.framework`, which records an
os_signpost and an os_log message for every SB API call. A process started with
`OS_ACTIVITY_MODE=disable` skips them: the HeapWalk suite takes 9.8 s with it
and 21 s without, and a web process's walk 27.4 s against 28.7 s.

The TypeFields suite loads WebCore from beside JavaScriptCore into the test
process (patch 11), and is skipped when WebCore is not built.

**What `ENABLE_MYA_HEAP` changes.** Build flags, each for a stated reason, and
one line of WTF:
- `-fstandalone-debug`, so that each image describes the types it uses (in
  CMake; Darwin's clang defaults to it);
- in CMake, no `-dwarf-linkage-names=Abstract`, under which an implicit
  destructor has no linkage name (patch 2);
- dSYMs on Darwin (`dwarf-with-dsym` in Xcode for JavaScriptCore, WebCore,
  WebKit and WebGPU, `dsymutil` after each link in CMake): liblldb finds the
  compile unit of a global variable only in a dSYM, not through the debug map;
  and with compilation caching (`COMPILATION_CACHE_ENABLE_CACHING`) an object
  file's debug info refers to its precompiled header's in the cache, which only
  dsymutil reads. Without WebCore's dSYM, liblldb prints "Unable to locate module
  needed for external types" and every type WebCore takes from its prefix
  header is a declaration;
- `-Wl,--build-id` on Linux, so that every image has an identity (patch 6):
  lld writes none unless asked, and only some distributions' clang asks;
- `PAS_BMALLOC_HIDDEN=0`, in CMake and in Xcode (`WK_MYA_HEAP`), so that mya
  and the tests can call libpas's enumerator (patch 9);
- `DEBUG_FISSION` off (A4);
- on Darwin, default symbol visibility, and so, in Xcode, no TAPI
  (`SUPPORTS_TEXT_BASED_API=NO`), whose export lists it no longer matches. On
  Linux every test passes with symbols hidden, because the walk reads the
  symbol table, which keeps them, so CMake hides them there as usual (see
  Next steps);
- `-O2` for `libJavaScriptCoreTools` in a Debug build: a walk reads every value
  of a heap through WTF's containers, which at `-O0` take twice as long.
  JavaScriptCore itself stays as the configuration says;
- `hashTraitsDeleteBucket` zeroes a deleted bucket's destroyed value (patch 11).

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
calls `AddModule(path, nullptr, uuid)` and `SetModuleLoadAddress`. liblldb finds
each image's dSYM next to it, by path, or its object files through the debug
map; the UUID is what refuses a rebuilt file.

`dynamicTypeAt(address)` returns the class of the complete object that contains a
polymorphic object. `TargetType` describes a type's layout, which is one of:
- a class: fields with their offsets, direct non-virtual bases, and virtual
  bases;
- a pointer or reference;
- an integer;
- a C array: its element type and count;
- anything else.

**One `TargetType` per type.** `SnapshotDebugInfo` owns every `TargetType`, one
per liblldb type, told apart with `SBType ==`. It indexes them by name only as a
cache key. A type reads its size when it is made and its layout the first time
it is asked for, so a field reached a million times is looked up in liblldb
once.

**One lookup per first word.** A vtable fixes both where its complete object
starts (its offset to top) and the class it belongs to, so `dynamicTypeAt`
caches both by the object's first word, including first words that are no
vtable, so a heap makes one liblldb lookup per distinct first word.

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
   a linkage name, `_ZN…D1Ev` (or `D4Ev` from Apple's clang), `X::~X()`; a
   class whose destructor is implicit, which liblldb does not list, is checked
   with another member it declares. Both names are demangled by liblldb, with
   LLVM's demangler, and the class each names must be the same string.
   Anything else, including a construction vtable (`_ZTC…`), is reported, and
   the object has no dynamic type.

   - *Not `abi::__cxa_demangle`.* Debian 12's cannot demangle a C++20
     constraint, which `RunLoop::Timer`'s constructors put in their lambdas'
     vtable names.
   - *Unnamed local types are compared by their enclosing function.* liblldb
     computes a member's linkage name with clang's mangler from the class it
     rebuilt, in which a function's lambdas are numbered again (`$_15` becomes
     `$_0`) and the enclosing function is spelled as instantiated, without its
     scope: `Timer(WebCore::Document&, void (WebCore::Document::*)())::$_0`
     against the symbol table's `WebCore::Timer::Timer<WebCore::Document,
     WebCore::Document>(WebCore::Document&, void
     (WebCore::Document::*)())::'lambda'()`. So a template or function argument
     that is an unnamed local type (`$_N`, `'lambda'…`, `{lambda…#N}`,
     `'unnamed…'`) is compared as its enclosing function's unqualified name and
     parameters, which both spellings give, and its own number is a wildcard.
   - *liblldb unifies classes of one name and size.* Every
     `WTF::Detail::CallableWrapper<(unnamed class), void>` has that name in
     liblldb, so two of one size share a type: the destructor of a
     `Timer<NavigationScheduler>`'s closure gives the class of a
     `Timer<Document>`'s, whose capture is a different type. Without the
     enclosing function, rule 2 accepted it, and the walk read 38 captures of
     a web process as the wrong type. With it, rule 2 refuses them and reports
     each: 42 `Timer` closures on google.com, which have no dynamic type and are
     read as their static type. Two lambdas of one function, of one size, are
     still not told apart.

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
  `~`. `GetName()` is the only spelling that is the same in every build: a CMake
  build without `ENABLE_MYA_HEAP` passes `-dwarf-linkage-names=Abstract`, where
  `GetBaseName()` is null and `GetMangledName()` is the bare `~Class`.
- **Rejected: the vtable's symbol as the source.** It names the class, but
  getting the type from a name is a lookup by name across every image. Rule 1
  finds the type from code, and the name only checks it.
- **Rejected: RTTI for the check.** The `type_info` in the vtable's RTTI
  slot names the class too, but needs `-frtti`, and the vtable's own symbol
  says the same without it.
- **Precondition.** Every polymorphic class needs a virtual destructor.
  - In Xcode builds, `-Wnon-virtual-dtor -Werror`
    (`GCC_WARN_NON_VIRTUAL_DESTRUCTOR`) enforces this, except for classes that declare
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
- In the HeapWalk suite, the two rules disagree on no object the walk reaches
  (the suite fails on any report it did not ask for),
  including the 18 lambda wrappers of `Heap::addCoreConstraints` and
  `RunLoop::Timer`'s, whose vtable name has a C++20 constraint.
- In a web process, no overrun comes through a closure's capture (patch 11).
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
| `HashTable` (so `HashSet` and `HashMap`) | The table size, from the metadata before the buckets (`HashTable::tableSizeOffset`, private). Typed, a bucket that WTF's own `isHashTraitsEmptyValue` or `Traits::isDeletedValue` calls empty or deleted is skipped. Untyped, every bucket is read as the bucket's type (patch 11). |
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
- `bool atSafePoint` says whether the target was at mya's safe point (patch 8).

The classes whose addresses the walk computes rather than reads, a block's
`MarkedBlock::Header` and a `LocalAllocator` from its list node, are asked for
by name in JavaScriptCore's image, which is the image that holds
`Structure::s_info` and so the image every Structure's ClassInfo points into.

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
   reported and skipped. So is one whose mapping `maps` marks `(deleted)`, as
   is `/proc/<pid>/exe`'s link: its file was replaced after it was loaded, and
   the path names the new one.
3. liblldb gets `AddModule(path, nullptr, buildID)` and
   `SetModuleLoadAddress(module, l_addr)`.

An image's `loadAddress()` is its `l_addr`, which is where its header is in a
position-independent image.

**Rejected.**
- *Opening each file through `/proc/<pid>/map_files`*, which keeps a deleted
  file readable. It needs `CAP_CHECKPOINT_RESTORE`, which makes the process
  non-dumpable; it needs a rule for which mapping holds the header; and liblldb
  caches modules by path, so every descriptor stays open for the process's life.
- *Detecting a file rewritten in place*, which leaves no `(deleted)` mark.
  `cp` does this, and so does mold for an executable that is not running.
  The target's clean pages are the file's page cache, so the rewrite changes
  them too: measured on Linux 6.1, a library's in-memory build-id reads as the
  new file's after `cp`, and the process then crashes in the library's exit
  handlers. Its build-id matches the new file, so liblldb accepts it. A11 leaves
  this out.
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
- The target's executable, replaced after loading by a new file renamed over
  it, is reported and left out of the images, while the target runs and after
  it exits, and the objects of its other images still have their types.
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

**Entering it.** `withMyaSafePoint(roots, function)` runs in the target, on the
roots' VM. It lives with the tests and changes nothing in JavaScriptCore:
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
is not, or the walk starts from a VM (patch 16), the walk runs anyway and marks
the result unverified (A1).

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

**`HeapWalk::reach(allocations, excluded, notReferrers, listCount)`.** An
allocation is reached if the walk reaches any address in it.
- Every live cell reaches its allocation.
- From the roots, every global variable (patch 13), every thread's stack and
  every live cell, the walk reads each object through its class's plan
  (patch 15): its pointers, and the values a reader reads (patch 11). A pointer
  is followed when its pointee has a known, nonzero size and its start lies
  inside an allocation. That keeps pointers into static data from leading
  anywhere.
- A pointer to a polymorphic class is followed as the complete object's dynamic
  type, quietly; failing to find one is counted (`NoDynamicType`), and the
  pointer is followed as its static type.
- A pointer to a type without a size, such as `void*`, is counted, and reaches
  the allocation it points into; an allocation that starts with a polymorphic
  object is then read as its dynamic type, and any other stays untyped. TZone's
  `s_heapRef`, a `void*`, holds its heap's metadata this way.
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
  no objects and are reachable only from libpas's static `slots`.

**Counting allocations: libpas's own enumerator.** libpas registers a malloc
zone named "WebKit Malloc" in every Darwin process it runs in
(`pas_root_ensure_for_libmalloc_enumeration`), whose `pas_root` it keeps in the
global `pas_root_for_libmalloc_enumeration`. That root is what libmalloc's
remote enumeration, as `heap` and `leaks` use it, hands to
`pas_root_enumerate_for_libmalloc`. `HeapWalk::libpasAllocations()` does the
same from the snapshot:
1. Read `pas_root_for_libmalloc_enumeration` in JavaScriptCore's image, and
   check the root's `PAS_ROOT_MAGIC`.
2. Run `pas_enumerator_create` on that remote root and
   `pas_enumerator_enumerate_all`. The reader copies the snapshot's bytes into
   a buffer that stays valid until the next read, as libmalloc's
   `memory_reader_t` promises. The recorder keeps each object record.
3. Sort the records by address.

The enumerator interprets the root's structs with mya's own libpas, which is
why mya must be the target's build (A12). It covers `fastMalloc`, IsoHeaps and
TZone heaps, using the production allocator.

The test's target also enumerates its own heap, in process, inside mya's safe
point, the same way, with a reader that returns the address it is given, and
records the result in the fixture. The test compares the two.

**Test.**
- **The enumeration.** No two enumerated allocations overlap. Every live cell is
  in one, so the JS heap's blocks are counted. The target allocates two
  `fastMalloc` objects that are reachable from the roots, in a chain, and two
  that it leaks, holding them only as integers. Each of the four is enumerated
  at its address, at least as large as it was allocated: libpas rounds up to
  its size class. A small and a large object freed just before the
  enumeration are not enumerated. One object from each way WTF allocates from
  libpas (small, large, aligned and compact `fastMalloc`, a TZone class and the
  primitive Gigacage), leaked and excluded, is enumerated at its address.
- **From the snapshot.** The enumeration from the snapshot agrees with the
  target's own to within the allocations the target makes or frees after its
  own enumeration (1%), and finds every object above at its address.
- **The reach.** The walk reaches the first two objects, and the leaked two are
  missed. A C array's last element is reached through the array.
- **The percentage.** Excluding the leaked objects raises `percent()` by exactly
  their bytes' share; the excluded bytes equal their allocations' sizes.

### 10. JS cells as their C++ classes

**Why this matters most.** Most of what a JS heap holds hangs off JS cells: a
string's characters, a code block's bytecode and metadata, a global object's C++
state, a DOM wrapper's DOM object. It is also how the walk reaches
JavaScriptCore's own description of every C++ object (A2).

**A cell's C++ class comes from its `ClassInfo`** (`HeapWalk::cellClass`). JS
cells have no vtable, but each `ClassInfo` is a class's static `s_info`:
1. Read the cell's StructureID, decode it (patch 5), and read the Structure's
   `m_classInfo`, at offsets found once from `structureStructure`.
2. The symbol at that address is `X::s_info`, with its linkage name. Its
   demangled name, less `::s_info`, is a candidate name for `X`, spelled with
   C++98's `> >` as liblldb spells types.
3. `SBModule::FindTypes` in the image that holds the symbol finds the classes of
   that name. The class is the one whose static `s_info`
   (`SBType::GetStaticFieldWithName`) has the symbol's linkage name. liblldb
   computes a member's linkage name with clang's own mangler from the class it
   built, so a match means this class declares that variable, whatever the
   spelling of its name.
4. A class template's instance whose arguments the demangler spells
   differently from liblldb, such as an enumerator, `(JSC::ArrayBufferSharingMode)0`
   against `JSC::ArrayBufferSharingMode::Default`, is found among the classes of
   a compile unit that has its code: the ClassInfo's method table holds the
   class's own functions, or those it inherits, and a code address finds its
   compile unit by the unit's code ranges. Among that unit's classes
   (`SBCompileUnit::GetTypes`, cached per unit) whose name starts with the
   template's, the one whose `s_info` has the symbol's linkage name is the class.
5. Check that the class's size is the `ClassInfo`'s `staticClassSize`, and that
   the class of `ClassInfo::parentClass`, found the same way, is one of its
   bases.
6. Cache the class per `ClassInfo`.

A `ClassInfo` that fails a step is reported, never guessed. A cell whose class is
not found is counted, and the tests require none. The same steps find a WebCore
wrapper's class in WebCore's image (patch 16).

**Rejected.**
- *Resolving the `s_info`'s address to its compile unit*
  (`ResolveSymbolContextForAddress` with `eSymbolContextVariable`). It is what
  liblldb offers for a data address, but with `.debug_names` it lists every
  global variable of the compile unit by scanning the whole name index, for
  each address: 140 s of a 220 s walk of a large heap went to 54 template
  instances. A code address resolves through the code ranges at once.
- *Listing the image's classes* (`SBModule::GetTypes`). liblldb builds a type for
  every class DIE of every compile unit, and on Darwin opens every object file:
  the whole of libJavaScriptCore's debug info.
- *A hand-written table in mya* of the cell classes. No macro lists them
  (`FOR_EACH_JS_DYNAMIC_CAST_JS_TYPE_OVERLOAD` names 67 of about 390, and
  `DECLARE_INFO` expands inside each class); the table would be kept in step by
  hand; and it would not cover WebCore's wrappers.

**Test.**
- Every live JS cell gets a class, and no class is larger than its cell. The
  fixture's object is a `JSFinalObject`, its date a `DateInstance`, the global
  object a `JSGlobalObject`, and its name a `JSString`.
- Every `s_info` symbol in JavaScriptCore's image, over 300 of them, names its
  class, including the 54 instances of class templates.
- The VM's `JSONCache`, `BuiltinExecutables` and `RegExpCache`, whose classes the
  test only declares, are reached through the cells, so they are read as
  JavaScriptCore describes them (A2).
- No cell is walked without a class.

### 11. Values the debug info cannot describe

The walk picks a reader by recognising the qualified name of a class it has
reached, and walks the class's members otherwise. A class's bases are walked as
values of their own, so a reader recognises a base: a `Packed<T*>` is a
`PackedAlignedPtr`, and every JS object with a butterfly is a
`JSObjectWithButterfly`. Each reader names the C++ source it mirrors. `Vector`
and `HashTable` use patch 4's readers.

| Class | How it is read |
|---|---|
| `WTF::Vector` | Every element. The buffer's allocation is reached even with no elements. |
| `WTF::HashTable` | Every bucket, empty, deleted or live, as the bucket's type. The other members are walked as usual: a Debug `HashTable` has a `unique_ptr<Lock>`. |
| `WTF::RobinHoodHashTable` | Every bucket of `m_table`, `m_tableSize` of them. It has no deleted buckets. |
| `WTF::SegmentedVector` | `SegmentedVector::addressAt`: the inline elements, then each segment's. |
| `WTF::TrailingArray`, `WTF::ButterflyArray` | `m_size` elements after the derived object, at `offsetOfData()`; or `m_leadingSize` before it and `m_trailingSize` after it. |
| `WTF::ConcurrentBuffer<T>::Array` | `size` elements of `data`. |
| `WTF::AlignedStorage` (so `NeverDestroyed`) | `m_storage`, as the template argument. |
| `WTF::LazyUniqueRef`, `WTF::LazyRef` | `m_pointer`, unless `lazyTag` or `initializingTag` is set, as the template argument's type. |
| `WTF::CompactPtr` (so `CompactRefPtr`) | `m_ptr`, decoded as `CompactPtr::decode` does. An outsized pointer, on a 36-bit build, is counted. |
| `WTF::PackedAlignedPtr` (so `Packed<T*>`) | The bytes of `m_storage`, shifted left by the alignment's log2 when `PackedAlignedPtr` stores it shifted. The alignment is the second template argument (`SBType::GetTemplateArgumentValue`). |
| `WTF::CodePtr` | `m_value`, a pointer into JIT code: it reaches the allocation it is in, whose bytes stay untyped. It is read whole, as `CodePtr`'s one member: liblldb describes some instances, such as `NativeExecutable`'s, as empty one-byte classes. |
| `WTF::CompactPointerTuple` | The low 48 bits of `m_data` (`maxNumberOfBitsInPointer`), as the first template argument. |
| `mpark::detail::base` (so `WTF::Variant`) | The alternative `index_` names, as that template argument, in the bytes of `data_`. A valueless variant's `index_` is all ones. |
| `WTF::InlineMap` | With `m_capacity` at its `InlineCapacity` (`isInline()`), `m_size` entries in `m_storage.inlineEntries`; otherwise `m_capacity` entries at `m_storage.hashedData.entries`. |
| `WTF::StringImpl` | Its members, then its characters: for `BufferInternal`, `m_length` of them at `tailOffset()`, as `m_data8`'s or `m_data16`'s pointee by `s_hashFlag8BitBuffer`; for `BufferSubstring`, the StringImpl in its tail. A `BufferOwned` buffer is reached through `m_data8`. |
| `std::optional` | Its value member, `_M_value` (libstdc++) or `__val_` (libc++, six bases down), only when `_M_engaged` or `__engaged_` is set. An empty optional's storage holds whatever was there before. |
| `JSC::JSValue` | `u`, as `JSValue::isCell` decodes it: a cell reaches its allocation. |
| `WTF::StringImplShape` | `m_data8`, or `m_data16` without `s_hashFlag8BitBuffer`: a `BufferOwned` string's buffer, which is how it is reached. |
| `JSC::JSString` | `m_fiber`: a resolved string's `StringImpl`, unless `isRopeInPointer` is set; a rope's fibers are cells. |
| `JSC::PropertyTable` | `m_indexVector`, less `isCompactFlag`: the index buffer, `m_indexSize` indices of a byte or of 32 bits, then `(m_indexSize >> 1) + 1` `CompactPropertyTableEntry`s or `PropertyTableEntry`s (`PropertyTable::dataSize`, private). |
| `JSC::InlineWatchpointSet` | `m_data`: a fat `WatchpointSet*`, unless `IsThinFlag` is set. |
| `JSC::UnlinkedFunctionExecutable` | With `m_isCached`, `m_decoder`; otherwise `m_unlinkedCodeBlockForCall` and `m_unlinkedCodeBlockForConstruct`. |
| `JSC::PropertyCondition` | The kind in `m_header`'s high bits: `u.prototype` for the kinds `hasPrototype()` names, `u.equivalence` for `Equivalence`, and the integers of `u.presence` otherwise. |
| `JSC::HashTableValue` | The member of `m_values` its accessors read for its `m_attributes`. |
| `JSC::InlineCacheHandler` | Its members but its union, whose cells and module slot the handler's `AccessCase` chose and which the GC does not trace through it. |
| `JSC::JSCellButterfly` | Its members but `m_header`, whose `IndexingHeader` is always its lengths. |
| `JSC::MarkedSpace` | Its members but `m_preciseAllocationsForThisCollectionBegin` and `End`, which `prepareForMarking` points into `m_preciseAllocations` for a collection and leaves after it: the buffer may have moved, and the end is no element. |
| `JSC::SymbolTableEntry` | `m_bits`: a `FatEntry*`, unless `SlimFlag` is set. |
| `JSC::CodeBlock` | `m_jitData`, a `void*`: a `BaselineJITData` or a `DFG::JITData` by the JIT code's type. |
| `JSC::JSObjectWithButterfly` | `m_butterfly`: the auxiliary cell that holds the butterfly, found among the live auxiliary cells, as JSValues: out-of-line properties before the butterfly, and the indexing header and indexed properties from it. A double array's elements are doubles in JSValue-sized slots. The cell is the one that holds the byte before the IndexingHeader, `sizeof(IndexingHeader)` before the butterfly, unless `Structure::hasIndexingHeader` (indexed properties, or a wasteful typed array), when it holds the header: without one, the butterfly points past the end of its cell. |
| `JSC::StructureID` | `StructureID::decode`: the cell it names. |
| `JSC::JSFunction` | `m_executableOrRareData`, less `rareDataTag`: its executable or its `FunctionRareData`. |
| `JSC::StructureChain` | `m_vector`'s `StructureID`s, up to the first zero (`head()`). |
| `JSC::LazyProperty` | As `WTF::LazyRef`: `m_pointer`, unless `lazyTag` or `initializingTag` is set. |
| `JSC::WriteBarrierBase<Unknown>` | `m_value`, as a `JSValue`. |
| `JSC::StrongBlock` | Its header, then a JSValue slot per `Strong` handle, to the end of the block. |
| `JSC::WeakBlock` | `weakImpls()`: `weakImplCount()` `WeakImpl`s after the block. |
| `JSC::GCArraySegment<T>` | `data()`: the slots after the segment, to `blockSize`, read as `T` and not followed: only those below the `GCSegmentedArray`'s top are in use. |
| `JSC::ExpressionInfo` | `chapters()` and `encodedInfo()`: `m_numberOfChapters` `Chapter`s after it, then `m_numberOfEncodedInfo` and `m_numberOfEncodedInfoExtensions` `EncodedInfo`s. |
| `JSC::HasOwnPropertyCache` | `HasOwnPropertyCache::size` (private) `Entry`s from its address; the class has no members. |
| `JSC::JSArrayBufferView` | `m_vector`, a `CagedBarrierPtr`, which holds its full address: a `FastTypedArray`'s auxiliary cell, read as bytes. |
| `JSC::ArrayBufferContents` | `m_data`, a `CagedPtr`: `m_sizeInBytes` bytes. |
| `WebCore::ImmutableStyleProperties` | `metadataSpan()` and `valueSpan()`: `m_arraySize` `StylePropertyMetadata`s at `m_storage`, then as many `Packed<const CSSValue*>`s. `m_storage` only marks where they start, so an object with none is smaller than its class (`objectSize`), and fits an allocation that size. |
| `WebCore::CSSSelector` | `m_data`: `rareData` with `m_hasRareData`, else `tagQName` for a `Match::Tag` selector, else `value` (`~CSSSelector`). |
| `WebCore::CSSPrimitiveValue` | `m_value.calc` for a `CSSUnitType::Calc` value; otherwise a number. |
| `WebCore::CSS::PrimitiveData` | `payload.calc` when the index's storage is `indexStorageForCalc`, `UnitTraits::count`, which WebCore asserts is one more than the last enumerator of the raw type's unit: a member, or a static one for a type of one unit. |
| `WebCore::CSSValue`, `WebCore::StyleRuleBase`, `WebCore::NodeRareData`, `WebCore::StyleProperties` | The subclass the type field names (below). |

More storage follows a cell or an allocation, and is read where the walk meets
it:
- a `JSFinalObject`'s inline storage, as many JSValues as its Structure's
  `m_inlineCapacity` (`JSFinalObject::allocationSize`);
- a `JSCellButterfly`'s vector, `vectorLength` JSValues at `offsetOfData()`;
- a `JSLexicalEnvironment`'s variables, its SymbolTable's `scopeSize()`
  JSValues at `offsetOfVariables()`;
- a cell of `JSRopeString`'s size whose ClassInfo is `JSString`'s, which is a
  rope or a resolved one, read as `JSRopeString`: a rope shares `JSString`'s
  ClassInfo but is allocated from its own subspace (`JSRopeString::subspaceFor`);
- the `PreciseAllocation` header before a precise allocation's cell
  (`PreciseAllocation::cell()`), at the address the precise allocation list
  gives, which `tryCreate` may have moved half an alignment into its memory;
- a buffer of integers, such as a malloc'ed string's characters: a pointer to an
  integer type at the start of an allocation types the whole allocation.

Elements that lead nowhere, integers or classes of integers, are typed as one
run rather than one by one.

**One change to WTF.** A deleted bucket of a `HashTable` holds its deleted key
and, after `~T()`, whatever its value's bytes were: a raw pointer there still
holds its old address. Reading every bucket as its type would follow it. In an
`ENABLE(MYA_HEAP)` build, `hashTraitsDeleteBucket` zeroes the destroyed value
before it writes the deleted key (`clearDestroyedHashTableValue`), so every
bucket reads as its type, and the reader needs no table's traits.

**Rejected: a reader for `std::unique_ptr<T[]>`.** It records no count: `new
T[n]` puts `n` in a cookie only when `T` has a destructor (Itanium C++ ABI 2.7),
and WTF's `UniqueArray` allocates without one. Reading to the end of the
allocation would follow the stale words in the slack libpas leaves after the
last element.

`JSString::m_fiber` is a `uintptr_t`, so its value does not give `StringImpl`'s
type, and `SymbolTableEntry::m_bits` an `intptr_t`: the walk asks
JavaScriptCore's image for `WTF::StringImpl` and
`JSC::SymbolTableEntry::FatEntry` by name (Rules). A reader of a WebCore class
asks the image that describes that class (`classNamedBeside`).

**Unions are read only through a reader.** liblldb describes a union as a class
whose members all start at its start (`TargetType::Class::isUnion`). A union
member is live only when the class that holds the union says so, by a field of
its own, and a walk that reads every member follows pointers that are not
pointers. On google.com, before unions were left out, every overrun traced came
from one: `UnlinkedFunctionExecutable`'s code block read as its `RefPtr<Decoder>`
(864 "184-byte `SourceProvider`s in a 64-byte allocation"), and
`CSSSelector::DataUnion`'s members not in use (3,143).

So a plan leaves a union out and counts it where the walk meets it, unless a
reader on the class that holds it, or on one of that class's bases, picks the
live member (the readers above). `Reach::unreadUnions` lists the unions met
without one, by type and holding field. Two kinds need no reader: a union whose
members lead nowhere, and one whose members that lead somewhere are all pointers
at its start to types without a size, such as `sigaction`'s handlers, which
hold one word read the same way whichever member is live. On google.com the
only union left is bison's `YYSTYPE`, a global variable whose live member
nothing records after parsing.

A pointer whose pointee type is bigger than what is left of the allocation it
points into is listed as an overrun and counted (`DoesNotFit`), and is neither
followed nor reached. The count is zero on google.com and in the tests, which
require it.

**Hierarchies with a type field.** `CSSValue` and `StyleRuleBase` have no
vtable: `m_classType` (`CSSValue::ClassType`) and `m_type` (`StyleRuleType`)
name each object's subclass, and the walk read them as their base, missing what
their subclasses hold. `NodeRareData` (`m_isElementRareData`) and
`StyleProperties` (`m_isMutable`) do the same with a bool. No rule from data
ties an enumerator to its class, as a ClassInfo does a cell's, so
`HeapWalk::typeFieldHierarchies()` holds a table from enumerator, or `false` and
`true`, to subclass for each. The two enumerations' tables are generated from
`CSSValue::visitDerived` and `StyleRuleBase::visitDerived`; entries compiled
only under a feature flag are marked, and `StyleRuleType::Margin` names no
subclass. The reader reads the object as the subclass its type field names,
when it fits the allocation. The TypeFields suite loads WebCore and checks the
tables against its debug info: every enumerator has an entry, so a new subclass
fails it, and every entry's class exists and derives from the base. On
google.com, reading `CSSValue` and `StyleRuleBase` raised the reach from 97.34%
to 99.32%, and `NodeRareData` and the definitions of declarations (A2) to
99.57%.

**Test.** Ten objects are planted behind the last element of a `Vector`, a
`HashSet`'s value, a `CompactPtr`, an initialized `LazyUniqueRef`, a
`Packed<T*>`, a `PackedAlignedPtr<T, 256>` stored shifted, a `HashMap` that has
had a deletion, a `NeverDestroyed<T*>`, a `CodePtr` and a `PropertyTable`'s
index vector, and a `JSString` built with `join` (so not an atom, and only the
`JSString` holds its `StringImpl`). Each is reached only through its holder,
and each is reached; removing a reader fails its case. One more, held only as
the stale value of a deleted `HashMap` entry, is missed, and so is one held only
by the stale payload of a reset `std::optional<T*>`. No `JSFinalObject` has
bytes beyond its class and inline storage. No pointer is followed to a type
bigger than its pointee's allocation (`DoesNotFit` is zero).

### 12. How much of the heap the walk types

**Goal.** Check that the walk is not broken. Patch 9's reach counts an
allocation once the walk reaches any address in it, so a walk that reaches an
object and then misreads it still scores. The typed coverage counts bytes: how
much of the memory libpas reports the walk has read as some type.

**`Reach::bytesTyped`.** The union, over every value the walk read, of
`[address, address + sizeof(type))`, clipped to the allocations, so that a field
read inside an object, or an object read as two types, counts once.
`Reach::typedPercent()` is `bytesTyped / (allocated − excluded − free in
blocks)`.

**One adjustment to the denominator.** libpas reports a MarkedBlock as one
allocation, but its free atoms are the JS heap's free memory, as free memory
between libpas's objects is libpas's. The heap walk knows each block's live
cells (patch 5), so a MarkedBlock counts as its live cells and its header. A
precise allocation counts as its `PreciseAllocation` header and, if it is live,
its cell: the cell of a dead one not yet swept is free memory too.

**Why it is not 100%.** Bytes that are in an allocation but in no declared type:
- libpas rounds an allocation up to its size class;
- a `Vector` or `HashTable` buffer's unused capacity;
- storage after the end of a class that no reader reads (patch 11), counted
  per class in `cellBytesBeyondClassByClass` for JS cells;
- objects the walk does not reach (patch 9's misses), and allocations it reaches
  only through an untyped pointer.

A trailing array makes an allocation bigger than its class, so its elements
lower the coverage until a reader reads them. The opposite, a value whose type
runs past the end of its allocation, means the walk read the object as a type
bigger than it is. Its bytes past the end are not counted, and the report lists
each such value with the field that led to it.

**The report**, printed by the HeapWalk suite's verbose output and by
`mya heap`: the typed percentage; the untyped bytes by kind, which is the type
read at an allocation's start or, for an allocation read as nothing there, what
reached it (a field, a stack, a global variable, a block of merged statics); the
classes of JS cells with the most bytes beyond what the walk reads; the
allocations with the most untyped bytes; and the overruns, grouped by their text
less its numbers. A class that recurs there has trailing storage or a missing
reader.

**Test.** In process and out of process:
- Each planted object of patch 11 is typed whole, and the `StringImpl` as its
  class and its characters.
- **Mutations**, checked by hand: reading cells as bare `JSCell`s (no patch 10),
  or removing a reader of patch 11, lowers the typed percentage.

### 13. Global variables and stacks as roots

Most of what a process holds that no JS cell reaches is held by static data:
singletons, caches, `NeverDestroyed` objects and TZone's heap references. The
walk reads them as roots.

**Global variables.** For every image with debug info, every data symbol whose
address is in writable memory is looked up as a variable by its own name
(`SBModule::FindGlobalVariables`), and kept when the variable's address is the
symbol's. liblldb lists a global by its linkage name too, so the name needs no
spelling of its own. A function-local static, `_ZZ…`, is found in its function,
whose symbol is a prefix of its name, among the static variables of the
function's blocks. A data symbol that is no variable, such as a guard variable
or a vtable, is counted.

LLVM's GlobalMerge merges a file's statics into one symbol, `_MergedGlobals.N`,
whose variables the debug info places inside it with no symbol of their own;
libpas's prefault supply keeps its spare MarkedBlocks in one. The walk scans such
a block's words as it scans a stack. A Mach-O symbol has no size, so liblldb
ends a symbol where the next one starts; scanning every symbol that is no
variable this way would scan the typed variables after a guard variable, whose
integers can look like addresses.

Memory the target cannot write holds no address it allocated, except memory it
froze after writing it: `WebConfig::g_config`, which WTF reads as its `Config`
at `startOffsetOfWTFConfig` and JavaScriptCore as its own at the WTF Config's
`spaceForExtensions`. The walk reads it as both.

**Stacks.** Each thread's stack in use, from its stack pointer up, is scanned as
the collector scans one: a word inside an allocation reaches it, and an
allocation that starts with a polymorphic object is read as its dynamic type.

**Arrays have a size, and the walk never reads past it.** Every array the walk
reads is a member of a value whose allocation or variable holds it whole, so an
element in memory that cannot be read means the walk misread the value that
holds the array. One web-process walk made 130 million failed mappings, element
by element, until pointers that do not fit (patch 11) were left unfollowed. An
array that runs past the readable memory it starts in is not read at all, and
is listed in `Reach::unreadableArrays` with the field that led to it, the value
that holds it and what reached that value. The tests require the list to be
empty; it is on google.com too.

**Test.** The walk reads over a thousand global variables, and misses no
allocation the test does not exclude, but for what the thread at the safe point
holds in its pthread specific data (patch 14). No array runs past readable
memory.

### 14. Explaining every miss

**`Reach::misses`.** Every missed allocation, with what holds it: the first of
these that holds a word whose value lies inside it, scanning every word of the
snapshot's writable memory.

| Cause | Holder |
|---|---|
| `Reached` | A word of an allocation the walk reached: a field it does not read as a pointer, named as `Class::field`, or bytes no type it read covers. |
| `StaticData` | A word in an image's data, in a symbol the walk does not read. |
| `OtherMemory` | A word anywhere else, such as libpas's metadata or a thread's control block. |
| `Missed` | Only words of other missed allocations: it is part of what they hold. |
| `NoReferrer` | No word anywhere: it is leaked, or held only in a form that is not a pointer. |

Read-only memory holds no address the target allocated, so a constant table
whose words happen to look like one is not a holder. A word below a thread's
stack pointer is left over from a frame that has returned, and the caller's own
records of the heap (`notReferrers`) are no holder either. A miss that only
other misses hold is counted in what its first holder up the chain holds, so the
list, most bytes held first, names what to teach the walk next.

The scan looks a word up among the missed allocations alone, so it costs a
range check per word of writable memory.

**Test.** Each of the five planted misses is explained with its cause and its
holder: an integer field of a reached object; nothing, for one whose address is
kept only XORed; that object, for the one it holds; nothing, for the stale value
of a deleted `HashMap` entry; and `createFixture()::roots`, for the stale payload
of a reset `std::optional`. The only misses left are what the thread at the safe
point holds in its pthread specific data, such as `ParkingLot`'s `ThreadData`,
which the C library keeps in its thread control block, in a layout it does not
publish.

### 15. Speed

**Goal.** A heap of a million cells is walked, reached, typed and explained in
under 10 s, in a Debug build.

- **Plans.** Each class is flattened, once, into a plan: the offsets of its
  pointers, with their pointee types, of the values a reader reads, and of its
  arrays of either. Members that are integers, or classes of integers, lead
  nowhere and are left out. An object is then walked with one read of its
  pointers' bytes and a loop over the plan, with no name lookups and no
  `TargetValue` per field. There is one plan for a complete object and one for a
  base subobject, whose virtual bases are not at known offsets, and one for each
  member a reader leaves out.
- **Cells.** A cell's StructureID and its Structure's ClassInfo are read at
  offsets found once.
- **Dynamic types.** One liblldb lookup per distinct first word (patch 2).
- **Readers.** A reader that finds a member by name, such as `StringImpl`'s and
  the butterfly's, finds it once per type.
- **Mappings.** `Memory`, for every reader, maps each read that fits in an
  aligned 1 MB window as that whole window, keeps the most recent Regions
  mapped after their last reader, and serves later reads from them. It keeps
  4,096 on Darwin, where a Region shares the snapshot's pages and costs address
  space, and 256 on Linux, where it is a copy. A window that runs into memory
  that cannot be mapped is remembered, and reads in it map page by page; a page
  that cannot be mapped is remembered with its error. Mapping each read's own
  pages took 46 s of a 107 s web-process walk. `CorpseMemoryTest` checks the
  windows, what is kept and the bound, and, with `keepRecentMappings(0)`, the
  page-by-page mappings in a window with a hole.
- **Misses.** Each word of writable memory is looked up among the misses alone
  (patch 14).
- **`-O2`** for `libJavaScriptCoreTools` in a Debug build.

**Measured** (Debug, macOS 27 arm64, 10 cores; `mya heap` on a `jsc` process
holding about a million cells, `large-heap.js` below, run as a measured target
always is: `--useConcurrentGC=0 --useConcurrentJIT=0 --useWarmUpMarkedBlocks=0`).
Spare blocks are always off when measuring, in `jsc` and in a web process
alike: they are free memory libpas holds for MarkedBlocks to come, and only
lower the typed percentage.

| Step | Before | After |
|---|---|---|
| Open the debug info | 2,156 ms | 635 ms |
| Walk the 761,762 live cells | 108 ms | 73 ms |
| Enumerate libpas's 171,233 allocations (38.8 MB) from the snapshot | n/a | 9 ms |
| Reach, type and explain | 186,197 ms | 2,938 ms |
| In all | 188,478 ms | 3,653 ms (3.85 s of wall clock, with attaching and the corpse) |

and 99.96% reached, 96.70% typed. (With warm-up blocks left on, the prefault
supply's 26 spare blocks are 426 KB of untyped memory, and 95.54% typed.) Of the
186 s before, 140 s went to finding the compile units of 54 class template
instances (patch 10).

Those numbers are with Homebrew's liblldb 22. With Xcode's `LLDB.framework`, the
same walk takes 5.7 s (724 ms to open the debug info, 5,002 ms to reach, type
and explain), and 4.0 to 4.9 s with `OS_ACTIVITY_MODE=disable` (Build and
test); the reach and the typed coverage are unchanged. The unions, readers and
mapping policy of patches 11 and 15 cost nothing measurable: the same build
without them took 4.1 to 5.0 s.

```
// large-heap.js: about a million live cells of the common kinds, then idle.
globalThis.keep = [];
for (let i = 0; i < 100000; ++i) {
    keep.push({ index: i, label: "item-" + i, values: [i, i + 1, i + 2],
        nested: { x: i * 1.5, y: String(i) }, date: (i % 100) ? null : new Date(i),
        fn: (i % 50) ? null : function() { return i; } });
}
globalThis.map = new Map();
for (let i = 0; i < 50000; ++i)
    map.set("key" + i, [i]);
globalThis.typed = [];
for (let i = 0; i < 2000; ++i)
    typed.push(new Uint8Array(256));
gc();
print("ready " + keep.length);
sleepSeconds(100000);
```

```
cp WebKitBuild/Debug/jsc /tmp/jsc-debuggable   # jsc is not signed get-task-allow
codesign -f -s - --entitlements <allow-jit + get-task-allow plist> /tmp/jsc-debuggable
DYLD_FRAMEWORK_PATH=WebKitBuild/Debug /tmp/jsc-debuggable --useConcurrentGC=0 \
    --useConcurrentJIT=0 --useWarmUpMarkedBlocks=0 large-heap.js &
printf 'snapshot --pid <pid>\nheap\n' | DYLD_FRAMEWORK_PATH=WebKitBuild/Debug WebKitBuild/Debug/mya
```

The HeapWalk suite's fixture, at mya's safe point: 5,249 live JS cells and 78
auxiliary cells, 7,248 libpas allocations; 99.71% reached, all of the misses
being what the safe-point thread keeps in its pthread specific data; 96.05%
typed; 2.2 to 2.9 s for the reach. The whole testLibJSCTools run is 2,011
assertions in 56 s; the HeapWalk suite takes 21 s of it, 9.8 s with
`OS_ACTIVITY_MODE=disable`.

### 16. `mya heap`: walking any process

`mya heap [<vm>]` walks the JS heap of the snapshot in use: from the VM at
`<vm>`, or from WebCore's main thread VM (`WebCore::g_commonVMOrNull`), or else
the VM JavaScriptCore last entered (`JSC::VMManager::s_recentVM`).

- `HeapWalk(snapshot, vm, javaScriptCoreImage)` reads the VM as
  JavaScriptCore's own image describes `JSC::VM`, so it is complete (A2). The
  reach walk starts from the VM itself, then the global variables, the stacks
  and the cells, as for roots.
- The allocations are libpas's, enumerated from the snapshot (patch 9). There
  is nothing excluded, and no records of the caller's.
- The walk is never at mya's safe point, so it is unverified (A1): a collection,
  a compiler thread or the scavenger may be in the middle of changing what it
  reads. Nothing it reads can crash it, and every count is bounded.
- It prints the time each step takes, the reach, the typed coverage, what was
  not followed and why, the misses by cause with the largest of them and what
  holds each, the classes of cells with bytes beyond what it reads, and the
  allocations with the most untyped bytes.

A target must let mya read it: a local macOS build signs WebContent with
`com.apple.security.get-task-allow` (`process-entitlements.sh`), and `jsc`,
which is not, needs a copy signed with it.

**Measured: google.com.** MiniBrowser from the same Debug build, on
https://www.google.com, idle after loading; `mya heap` on its WebContent
process, with Xcode's `LLDB.framework`. Spare blocks are turned off through
XPC's environment prefix, which reaches the WebContent process:

```
JSC_useWarmUpMarkedBlocks=0 __XPC_JSC_useWarmUpMarkedBlocks=0 \
    Tools/Scripts/run-minibrowser --debug https://www.google.com
```

| | |
|---|---|
| Live cells | 40,990, 3.5 MB; 36,731 JS cells, every one read as its class |
| libpas allocations | 72,497, 14.2 MB |
| Reached | 99.57% |
| Typed | 86.18% (of the 12.9 MB that is not free memory in the JS heap) |
| Not followed | 0 pointers to types that do not fit; 991 to declarations no image defines once; 1 union read by no reader (`YYSTYPE`) |
| Global variables read | 24,721, and 9,017 data symbols that are no variable, or whose type liblldb gives no size, such as Swift's |
| Time | 5,707 ms to open the debug info, 8 ms to walk the cells, 7 ms to enumerate libpas, 30,180 ms to reach, type and explain: 35.9 s in all |

The time varies with how long the page has been idle: the same build took 28.5 s
on a page left idle for minutes. Most of it is liblldb's: completing WebCore's
types (about 8 s, in `IsPolymorphicClass`, `GetByteSize` and `IsTypeComplete`),
opening the debug info (3.5 s), finding 24,763 global variables one symbol at a
time (about 2.6 s in `FindGlobalVariables`), and resolving vtables (about 2.9 s).
Listing every global variable at once is slower: one `SBTarget::FindGlobalVariables`
with a regular expression that matches every name took 91 s on this process
and returned 744,694 variables.

## 17. Heap dump

**Goal.** A heap dump Web Inspector reads, of every libpas allocation and every
reference the walk reads, checked against JSC's own heap snapshot, reconciled
with the target's footprint, and diffable.

**`mya heapdump <file> [<vm>]`** writes a GCDebugging heap snapshot, version 3,
the format `HeapSnapshotBuilder` writes for `generateHeapSnapshotForGCDebugging()`
and Web Inspector's `HeapSnapshot.js` reads (`HeapWalk::heapDump`, `HeapDump::json`):
- a node per live cell, named by its class; per other libpas allocation, named by
  the type read at its start, `(untyped)`, or `(missed, <cause>)`; for a
  MarkedBlock or a precise allocation, what is left of it less its cells; and
  per root the references come from (`<a global variable>`, `<a thread's stack>`,
  ...), each a child of JSC's `<root>` and listed in `roots` with its name;
- an edge per reference the reach walk reads (`HeapWalk::Reference`), from the
  slot, or the value a reader reads, to the address it holds, resolved to the
  cell or allocation that holds each; references held in JSValue slots
  (inline storage, butterflies, lexical environments, `WriteBarrier<Unknown>`)
  are recorded from the object that owns them;
- missed allocations and block leftovers are internal nodes (flag 1), which Web
  Inspector routes no root path through; every other node is not, so a path to
  a root may pass through C++ objects.

**Test (HeapDump suite).**
- In process, at mya's safe point, after JSC's own GCDebugging snapshot of the
  same heap: every libpas allocation is exactly one node, and the nodes' bytes
  are the allocations'; every cell JSC lists is a node; and every edge between
  cells JSC lists is a path in the dump through C++ objects alone (a code
  block's constants are cell, `FixedVector` storage, then string). 10,702 edges
  between 5,245 cells, none missing. Removing the `StructureID` reader leaves
  5,301 missing.
- Out of process, two snapshots of a quiet target, which allocates eight
  `ReachableObject`s between them: the second dump has a node for each, typed
  as its class, and no other new node, and none is gone.
  `withMyaSafePoint` is a template, since a `WTF::Function` allocates.
- The same target's footprint: the dump's bytes are the objects in libpas's
  pages in the footprint plus those elsewhere; libpas records what all but at
  most 256 KB of its pages hold; and the footprint is libpas's pages, the other
  private memory and the tagged ledgers, but for less than 2 MB of page tables.

**Measured: SP3 in MiniBrowser.** One iteration of a subtest
(`index.html?suites=<subtest>&startAutomatically&iterationCount=1`, served
locally from Speedometer's `release/3.1` branch), idle after it; `heapdump`
then `memory` on the WebContent process:

| | TodoMVC-React-Complex-DOM | Editor-CodeMirror | NewsSite-Next |
|---|---|---|---|
| Nodes, edges | 255,504, 570,031 | 76,181, 187,706 | 169,618, 440,835 |
| Dump bytes (libpas objects) | 29.4 MB | 13.3 MB | 22.6 MB |
| Live cells | 1.3 MB | 1.9 MB | 3.6 MB |
| Footprint | 48.5 MB | 30.4 MB | 43.6 MB |
| libpas pages in it | 34.9 MB | 17.3 MB | 29.5 MB |
| of them free payload | 6.4 MB | 5.4 MB | 8.8 MB |

Web Inspector's own `HeapSnapshot` class loads the React dump in 3 s (in the
`jsc` shell, with a `console` shim), and finds root paths through C++ objects:
an `HTMLDivElement` is held by a `RenderBlockFlow`, held by the `LocalFrameView`
of a `LocalFrame`, held by a global variable.

**The footprint, measured.** `mya memory` on the React run, against `footprint`:

| | mya | `footprint` |
|---|---|---|
| Footprint | 47,201 KB | 46 MB |
| libpas (WebKit Malloc) | 34,976 KB: 27,524 objects, 6,456 free payload, 1,010 metadata, 48 unrecorded | 34 MB, 8.4 MB reclaimable |
| system malloc small | 1,616 KB | 1,616 KB, 6.1 MB reclaimable |
| graphics | 608 KB (ledger) | 608 KB |
| left over | 785 KB | 929 KB of page tables |

Three facts the numbers rest on:
- **Reading a corpse changes it.** mya maps a snapshot's pages shared, and a
  read of a page the target never touched makes it resident, and dirty, in the
  corpse: a 128 MB libpas region with 26 resident pages had 1,309 after a walk.
  Copy-on-write mappings are worse: the live target's page queries then report
  270 MB of libpas pages dirty. So a snapshot records its regions, their page
  dispositions (`mach_vm_page_range_query`) and its `phys_footprint` when it is
  taken, before anything reads it.
- **A page can be dirty and reusable.** libpas and system malloc mark freed
  pages `MADV_FREE_REUSABLE`; a region's dirty count includes some of them,
  and the footprint does not. A region's footprint is its pages dirty and not
  reusable, and those compressed.
- **No interface outside the kernel reports page tables**, so they are what is
  left.

**Options** for what "matches the rss" means:
1. *`phys_footprint` (done).* What jetsam and Activity Monitor count. libpas's
   share is split exactly; the rest is per VM tag, plus the tagged ledgers, and
   the page tables are what is left. Resident size is not reconcilable: 1 GB of
   it is clean, shared `__TEXT` and `__LINKEDIT`.
2. *Add the footprint to the dump* as synthetic nodes (libpas free payload and
   metadata, each VM tag, the page tables), so the dump's bytes are the
   footprint, less the object bytes in pages outside it, which a node can list.
3. *Split system malloc* (3.4 MB counted, 1.6 MB in the footprint) by
   enumerating its zones from the snapshot with libmalloc's remote
   introspection, which A3 leaves out.
4. *Attribute images' dirty data* (`untagged`, 2.8 MB, mostly `__DATA` and
   `__DATA_DIRTY`) to the global variables the walk already reads.

## What the walk misses

As `mya heap` reports it for google.com, without reading anything by hand.

**Not reached: 0.43%, 61 KB.**
- 40 KB held by an allocation the walk reached, in bytes it read as no type.
  Most are held by allocations reached only through an untyped pointer: an
  800-byte allocation the VM holds through a `void*`, and the objects a
  `WeakPtrImpl` holds through its `void* m_ptr`, such as a 2,560-byte one that
  holds a `PageInspectorController`. A `WeakPtr<T>` knows `T`, but its impl,
  shared by every `WeakPtr` to the object, does not.
- 18 KB held only by other misses, through those.
- 3 KB held by other memory, such as libSystem's thread control blocks, and
  128 bytes held by nothing a word of memory points to.

**Reached but not typed: 13.82%, 1.8 MB.**
- 286 KB of JIT code, reached through `CodePtr`s: machine code, with no C++ type.
- 258 KB of byte buffers read as `unsigned char` at their start; the largest are
  bytecode buffers that `CodeBlock::m_instructionsRawPointer` reaches, of which
  the walk types the part a pointer to their start or a reader covers.
- 166 KB of `StringImpl`s, beyond their header and characters: size-class slack.
- 138 KB of 16-byte allocations that each hold a one-byte `WTF::Lock`, reached
  through a Debug `HashTable`'s `std::unique_ptr<Lock>`: libpas's size-class
  slack.
- 105 KB of MarkedBlocks, in live cells beyond what their class and readers say.
- 47 KB of `CSS::BoxShadow`s and 34 KB of `MQ::MediaQuery`s, beyond their class.
- 48 KB of live precise allocations, reached through the precise allocation
  list (`BasicRawSentinelNode<PreciseAllocation>`), whose cells the walk reads
  as no type.
- The rest, in pieces under 26 KB: Vectors' unused capacity, and
  `EmbeddedFixedVector`s and `ExpressionInfo`s beyond their elements.

**Read as something they are not.** Nothing: no pointer runs past its
pointee's allocation, and no array past readable memory. 42 `Timer` closures
whose class liblldb unifies with another's are refused and read as their static
type (patch 2).

On the large JSC heap, the misses are 17 KB, held by anonymous memory outside
any allocation, the holder the HeapWalk suite identifies as the thread's pthread
specific data; and the untyped bytes are `StringImpl` size-class slack (979 KB),
the prefault supply's spare blocks (426 KB, reached through a block of merged
statics), and JIT code.

Most of what is left is not memory a C++ type describes: slack, unused capacity,
spare blocks and machine code. What is described but not read is what untyped
pointers hold.

## Build gotchas

- **SB API headers.** Xcode's `LLDB.framework` has no SB headers. They come from
  Homebrew's llvm (`/opt/homebrew/opt/llvm/include`, LLVM 22), and the Xcode
  build links the framework (`WK_LLDB_LDFLAGS` in `Base.xcconfig`, and
  `-framework LLDB` autolinked from `CorpseSnapshot.cpp`), with an rpath to
  Xcode's `SharedFrameworks` and one to the Command Line Tools'. Homebrew's
  liblldb 22.1.8 segfaults in `CXXRecordDecl::setBases` completing some WebCore
  types during a walk. The SB API is binary compatible: the symbols mya and the
  tests import are all exported by lldb-2103, every enumerator they use has the
  same value, and an SB function only LLVM 22 has fails at link time. The
  framework is not documented as public API, loads Python and Swift's compiler
  libraries at launch, and records a signpost and a log message per SB call
  (Build and test). The CMake build on macOS still links Homebrew's liblldb.
- **Ninja.** The CMake JSCOnly port on macOS enables Swift, so it needs a real
  ninja (`/opt/homebrew/bin/ninja`).
- **ARC.** JSCOnly on macOS compiles `wtf/darwin/OSLogPrintStream.mm` without ARC
  unless that file gets `-fobjc-arc`.
- **Darwin but not Cocoa.** JSCOnly on macOS is `OS(DARWIN)` but not
  `PLATFORM(COCOA)`: `MachSendRight` is empty there, so `corpse/` must not use it.
  It emits no dSYMs of its own; under `ENABLE_MYA_HEAP` CMake runs `dsymutil`
  after each link.
- **The build-system marker.** `build-jsc --jsc-only` or `--cmake` switches
  `WebKitBuild/BuildSystem` to CMake. The Xcode harness run then looks in
  `cmake-mac`, prints "not built", and still reports 0 failures. Run
  `build-jsc --xcode` again before the Xcode harness run.
- **The harness on macOS.** `run-javascriptcore-tests --jsc-only` is rejected on
  macOS; use `--root` as shown in Build and test.
- **CodeSign.** A rebuild after a failed build can fail in CodeSign on a stale
  `JavaScriptCore.framework/Versions/A/JavaScriptCore.cstemp`. Delete that file.
- **A rebuilt framework.** A running target whose JavaScriptCore was rebuilt
  under it no longer runs the file on disk, and liblldb refuses the file by its
  UUID: "The image at … has no class 'JSC::VM'". Restart the target. A
  `build-jsc` that changes `libJavaScriptCoreTools` can relink JavaScriptCore.
- **Attaching.** `task_for_pid` needs the target signed with
  `get-task-allow`; `jsc` is not (patch 16).
- **Warnings.** Xcode builds the tests with `-Werror=exit-time-destructors`, so a
  static fixture with a destructor needs `NeverDestroyed`. Only Xcode uses
  `-Wnon-virtual-dtor`, so a CMake build does not catch a missing virtual
  destructor. Only Xcode builds `corpse/` with
  `-Werror -Wunsafe-buffer-usage` and `-Wunnecessary-virtual-specifier`: search a
  `Vector`'s `span()` rather than the `Vector`, whose iterators are raw pointers,
  bracket libpas's headers with `WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN`/`END` as
  `JSDollarVM.cpp` does, and give a polymorphic class a virtual destructor only
  when it is not `final`.
- **libpas's headers in Xcode.** Xcode installs only the libpas headers marked
  private. mya and the tests use only those, so that what the framework installs
  does not change.
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

## Next steps

For the next agent. Each task says what is known, how to see it, and what done
looks like. The diagnostics they need are in the tree: an overrun names the
word that held the pointer and the value being walked
(`... through Class::field at 0x... in a 'Holder' at 0x...`), `mya heap` prints
one example of each kind of overrun, the unions it read no member of and the
arrays it could not read, and a miss names what holds it. To look at a live
target, attach Xcode's lldb to it (`xcrun lldb -p <pid>`) and read the
addresses a report names.

1. **The heap dump's footprint options (patch 17).** Pick among options 2 to 4
   there. The SP3 runs are scripted but not part of a test suite: a test that
   drives MiniBrowser would need the harness to run a GUI.
2. **Untyped pointers that a type could follow.** `WeakPtrImpl::m_ptr` is a
   `void*`, but every `WeakPtr<T>` that shares the impl knows `T`; the VM holds
   an 800-byte allocation through a `void*` whose misses are 8 KB. These are
   most of the 40 KB of misses held by reached allocations.
3. **Closures liblldb unifies.** 42 `Timer` closures on google.com have no
   dynamic type, because liblldb gives every
   `CallableWrapper<(unnamed class), void>` of one size one class (patch 2), and
   their captures go unread. Done: their captures are read as their own
   closures', perhaps found through the closure's own call operator.
4. **A web process's walk takes 28 to 36 s**, most of it liblldb's (patch 16).
   Listing global variables at once is slower, so what is left is fewer SB
   calls per symbol and per type.
5. **`OS_ACTIVITY_MODE` for the tests.** Xcode's `LLDB.framework` doubles the
   HeapWalk suite unless the process starts with `OS_ACTIVITY_MODE=disable`,
   which the harness does not set.
6. **Linux.** Nothing here was built or run on Linux. `Memory` keeps 256
   copied windows there, and the TypeFields suite is skipped.
7. Still open from before:
    - Whether default symbol visibility under `ENABLE_MYA_HEAP` is needed on
      Darwin. On Linux it is not: with every symbol hidden (8,644 exported from
      libJavaScriptCore, against 252,098), all assertions pass and the reach is
      unchanged.
    - Linux is not measured since the walk's speed-ups. Draft 7 measured
      240 µs a cell there; the same changes apply, and `Memory`'s windows copy
      rather than map there (`process_vm_readv`).
