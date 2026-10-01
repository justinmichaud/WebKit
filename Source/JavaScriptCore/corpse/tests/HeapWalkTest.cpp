/*
 * Copyright (C) 2026 Igalia S.L.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. AND ITS CONTRIBUTORS ``AS IS''
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL APPLE INC. OR ITS CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "LibJSCToolsTestUtilities.h"

#include <JavaScriptCore/CorpsePlatform.h>

#if HAVE(LLDB)

#include <JavaScriptCore/Completion.h>
#include <JavaScriptCore/CorpseAddress.h>
#include <JavaScriptCore/CorpseHeapWalk.h>
#include <JavaScriptCore/CorpseRemote.h>
#include <JavaScriptCore/CorpseSnapshot.h>
#include <JavaScriptCore/DateInstance.h>
#include <JavaScriptCore/DeferGCInlines.h>
#include <JavaScriptCore/HeapCell.h>
#include <JavaScriptCore/HeapIterationScope.h>
#include <JavaScriptCore/HeapObserver.h>
#include <JavaScriptCore/Identifier.h>
#include <JavaScriptCore/InitializeThreading.h>
#include <JavaScriptCore/JSCJSValueInlines.h>
#include <JavaScriptCore/JSGlobalObject.h>
#include <JavaScriptCore/JSLock.h>
#include <JavaScriptCore/JSObjectInlines.h>
#include <JavaScriptCore/JSType.h>
#include <JavaScriptCore/LocalAllocator.h>
#include <JavaScriptCore/MarkedBlock.h>
#include <JavaScriptCore/MarkedSpaceInlines.h>
#include <JavaScriptCore/Options.h>
#include <JavaScriptCore/SourceCode.h>
#include <JavaScriptCore/StrongInlines.h>
#include <JavaScriptCore/StructureID.h>
#include <JavaScriptCore/VM.h>
WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN
#include <bmalloc/pas_enumerator.h>
#include <bmalloc/pas_heap_lock.h>
#include <bmalloc/pas_root.h>
WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
#include <algorithm>
#include <bit>
#include <optional>
#include <stdint.h>
#include <stdlib.h>
#include <string_view>
#include <unistd.h>
#include <utility>
#include <wtf/CompactPtr.h>
#include <wtf/HashMap.h>
#include <wtf/HashSet.h>
#include <wtf/HexNumber.h>
#include <wtf/IterationStatus.h>
#include <wtf/LazyUniqueRef.h>
#include <wtf/MallocSpan.h>
#include <wtf/NeverDestroyed.h>
#include <wtf/Packed.h>
#include <wtf/StdLibExtras.h>
#include <wtf/Vector.h>
#include <wtf/text/ASCIILiteral.h>
#include <wtf/text/MakeString.h>

namespace JSCToolsTest {

namespace {

using JSC::Corpse::Address;
using JSC::Corpse::HeapWalk;
using JSC::Corpse::Remote;
using JSC::Corpse::Snapshot;
using JSC::Corpse::TargetType;
using JSC::Corpse::TargetValue;

// A JS value the walk has to find: an int32 in the fixture's inline storage.
constexpr int32_t jsMagic = 0x5eed1234;

// A C++ value the walk has to find: the double inside a DateInstance.
constexpr double dateMagic = 1234567890123;

// A cell bigger than this is counted but not searched for the magic values.
constexpr size_t maximumScannedCellSize = 1 << 20;

// What the target evaluates. The fixture hangs off the global object, so it
// survives the full collection the target takes right after.
constexpr ASCIILiteral targetScript = "globalThis.fixture = {\n"
    "    magic: 0x5eed1234,\n"
    "    date: new Date(1234567890123),\n"
    "    name: [\"corpse\", \"heap\", \"walk\"].join(\"-\"),\n"
    "    children: [],\n"
    "};\n"
    "for (let i = 0; i < 4; ++i)\n"
    "    fixture.children.push({ index: i, parent: fixture, label: \"child-\" + i });\n"
    "fixture.children[0].sibling = fixture.children[1];\n"
    "fixture;\n"_s;

// Evaluated after the collection, so that its cells are live only through the
// newly allocated bits, the allocated blocks and the free lists.
constexpr ASCIILiteral lateScript = "globalThis.late = [];\n"
    "for (let i = 0; i < 2000; ++i)\n"
    "    late.push({ index: i, label: \"late-\" + i });\n"
    "late[0];\n"_s;

constexpr std::array<size_t, 2> leakedSizes { 5000, 7000 };
constexpr std::array<size_t, 2> freedSizes { 4000, 300000 };

// C++ objects the reach walk has to find from the roots, a chain of two.
// Aligned for CompactPtr, which can drop the low bits of an address.
struct alignas(16) ReachableObject {
    WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(ReachableObject);
    WTF_ALLOW_STRUCT_COMPACT_POINTERS;
    ReachableObject* next { nullptr };
    std::array<uint8_t, 3000> bytes { };
};

// Objects behind a value the debug info cannot describe as a pointer to
// them, one for each way the walk reads such a value (patch 11).
enum class Planted : uint8_t {
    VectorElement, // The last element of a Vector, past its first.
    HashSetBucket, // Every bucket of a HashSet.
    CompactPointer, // A CompactPtr, which holds an integer.
    LazyPointer, // An initialized LazyUniqueRef, which holds an integer with tag bits.
    PackedPointer, // A Packed<T*>, which holds the pointer's low bytes.
    ShiftedPackedPointer, // A PackedAlignedPtr whose alignment lets it store the pointer shifted.
};
constexpr size_t numberOfPlanted = static_cast<size_t>(Planted::ShiftedPackedPointer) + 1;
constexpr std::array<ASCIILiteral, numberOfPlanted> plantedNames {
    "the last element of a Vector"_s,
    "a HashSet's value"_s,
    "a CompactPtr"_s,
    "a LazyUniqueRef"_s,
    "a Packed<T*>"_s,
    "a PackedAlignedPtr stored shifted"_s,
};

// Enough alignment for PackedAlignedPtr to store the pointer shifted, in a byte less.
constexpr size_t shiftedPackedAlignment = 256;

// Where HeapWalk starts. Its vtable gives its type, and its fields give the
// VM and the two types no field reached from the VM has.
class MyaRoots {
    WTF_MAKE_NONCOPYABLE(MyaRoots);
public:
    explicit MyaRoots(JSC::VM& vm)
        : vm(&vm)
    {
    }
    virtual ~MyaRoots();

    JSC::VM* const vm;
    JSC::MarkedBlock::Header* const markedBlockHeader { nullptr };
    JSC::LocalAllocator* const localAllocator { nullptr };
    // Set only inside withMyaSafePoint.
    bool atSafePoint { false };

    ReachableObject* reachable { nullptr };
    std::array<ReachableObject*, 3> array { };
    // Each holds a planted object, as Planted says.
    Vector<ReachableObject*> vector;
    HashSet<ReachableObject*> set;
    CompactPtr<ReachableObject> compact;
    LazyUniqueRef<MyaRoots, ReachableObject> lazy;
    PackedPtr<ReachableObject> packed;
    PackedAlignedPtr<ReachableObject, shiftedPackedAlignment> shiftedPacked;
    // One live entry, and a deleted one whose value still holds an object nothing else does.
    HashMap<uint64_t, ReachableObject*> map;
};

MyaRoots::~MyaRoots() = default;

// Where the target's VM put the objects of targetScript, as the target knows
// them. The analysis reads this out of the corpse and checks the walk against it.
struct HeapWalkFixture {
    uint64_t roots { 0 };
    uint64_t vm { 0 };
    uint64_t object { 0 };
    uint64_t structure { 0 };
    uint32_t structureID { 0 };
    uint64_t date { 0 };
    uint64_t lateObject { 0 };
    uint64_t liveCells { 0 }; // What JSC's own forEachLiveCell visits, sorted.
    uint64_t liveCellCount { 0 };
    std::array<uint64_t, 2> reachable { }; // Reachable from the roots.
    uint64_t arrayElement { 0 }; // The last element of a C array.
    std::array<uint64_t, 2> leaked { }; // Held only here, as integers.
    std::array<uint64_t, 2> freed { }; // A small and a large object, freed before the enumeration.
    uint64_t allocations { 0 }; // What libpas enumerates in the target, sorted.
    uint64_t allocationCount { 0 };
    std::array<uint64_t, numberOfPlanted> planted { };
    uint64_t staleInMap { 0 }; // Held only as the stale value of a deleted HashMap entry.
    uint64_t globalObject { 0 };
    uint64_t string { 0 }; // The JSString of the fixture's name.
    uint64_t stringImpl { 0 }; // Its StringImpl.
    // Objects the VM owns through pointers to classes this executable only declares.
    uint64_t jsonCache { 0 };
    uint64_t builtinExecutables { 0 };
    uint64_t regExpCache { 0 };
};

HeapWalkFixture& fixtureAt(Address address)
{
    return *std::bit_cast<HeapWalkFixture*>(static_cast<uintptr_t>(address.toTargetVMAddress()));
}

MyaRoots& rootsOf(Address fixture)
{
    return *std::bit_cast<MyaRoots*>(static_cast<uintptr_t>(fixtureAt(fixture).roots));
}

// mya's safe point: runs `function`, which takes the snapshot, where no thread
// can be changing the heap's liveness state, and records that in the roots.
// False, having said why, if the heap is in a collection: in a quiet target a
// collection runs only on this thread, so that is a bug in the test.
bool withMyaSafePoint(MyaRoots& roots, NOESCAPE const Function<void()>& function)
{
    JSC::VM& vm = *roots.vm;
    // No other thread allocates cells or sweeps, and this one starts no collection.
    JSC::JSLockHolder locker(vm);
    JSC::DeferGC deferGC(vm);
    if (vm.heap.collectionScope() || vm.heap.objectSpace().isMarking()) {
        dataLogLn("    not at a safe point: the heap is in a collection");
        return false;
    }
    roots.atSafePoint = true;
    function();
    roots.atSafePoint = false;
    return true;
}

// Every live libpas object of this process, as libpas enumerates a heap:
// pas_enumerator reads through the reader, which here is this process's own
// memory. The records go into a buffer from system malloc, allocated up front,
// so that the enumeration allocates nothing from the heap it enumerates.
struct EnumeratedAllocations {
    std::span<HeapWalk::Allocation> objects;
    size_t count;
};

void* readOwnMemory(pas_enumerator*, void* address, size_t, void*)
{
    return address;
}

void recordObject(pas_enumerator*, void* address, size_t size, pas_enumerator_record_kind kind, void* argument)
{
    auto& records = *static_cast<EnumeratedAllocations*>(argument);
    if (kind != pas_enumerator_object_record)
        return;
    RELEASE_ASSERT(records.count < records.objects.size());
    records.objects[records.count++] = { Address { address }, size };
}

constexpr size_t recordCapacity = 4 * 1024 * 1024;

std::span<HeapWalk::Allocation> enumerateAllocations()
{
    using Buffer = decltype(MallocSpan<HeapWalk::Allocation, SystemMalloc>::malloc(0));
    static NeverDestroyed<Buffer> objects { MallocSpan<HeapWalk::Allocation, SystemMalloc>::malloc(recordCapacity * sizeof(HeapWalk::Allocation)) };
    static EnumeratedAllocations records { objects->mutableSpan(), 0 };
    records.count = 0;
    // The heap lock, which libpas needs to create a root, keeps libpas's other threads out of the heap.
    pas_heap_lock_lock();
    static pas_root* root = pas_root_create();
    pas_enumerator* enumerator = pas_enumerator_create(root, readOwnMemory, nullptr, recordObject, &records,
        pas_enumerator_do_not_record_meta_records, pas_enumerator_do_not_record_payload_records, pas_enumerator_record_object_records);
    RELEASE_ASSERT(enumerator && pas_enumerator_enumerate_all(enumerator));
    pas_enumerator_destroy(enumerator);
    pas_heap_lock_unlock();
    std::span<HeapWalk::Allocation> enumerated = records.objects.first(records.count);
    std::ranges::sort(enumerated, { }, &HeapWalk::Allocation::address);
    return enumerated;
}

// Makes a VM, evaluates targetScript in it, collects, then evaluates
// lateScript, so that the live heap has cells from both sides of its last
// collection. The VM and its global object are kept for the life of the process.
Address createFixture()
{
    static HeapWalkFixture fixture;
    static NeverDestroyed<std::optional<MyaRoots>> roots; // Remade with each fixture.
    static NeverDestroyed<JSC::Strong<JSC::JSGlobalObject>> root;

    initializeQuietJSC();
    JSC::VM& vm = JSC::VM::create(JSC::HeapType::Large).leakRef();
    JSC::JSLockHolder locker(vm);
    JSC::JSGlobalObject* globalObject = JSC::JSGlobalObject::create(vm, JSC::JSGlobalObject::createStructure(vm, JSC::jsNull()));
    root->set(vm, globalObject);

    NakedPtr<JSC::Exception> exception;
    JSC::JSValue result = JSC::evaluate(globalObject, JSC::makeSource(targetScript, JSC::SourceOrigin { }, JSC::SourceTaintedOrigin::Untainted), JSC::JSValue(), exception);
    RELEASE_ASSERT(!exception);
    JSC::JSObject* object = result.getObject();
    RELEASE_ASSERT(object);
    JSC::JSValue date = object->get(globalObject, JSC::Identifier::fromString(vm, "date"_s));
    RELEASE_ASSERT(date.isCell());
    JSC::JSValue name = object->get(globalObject, JSC::Identifier::fromString(vm, "name"_s));
    RELEASE_ASSERT(name.isString() && JSC::asString(name)->tryGetValueImpl());

    roots->emplace(vm);
    MyaRoots& myaRoots = roots->value();
    fixture = { };
    fixture.roots = std::bit_cast<uint64_t>(&myaRoots);
    auto* reachable = new ReachableObject;
    reachable->next = new ReachableObject;
    myaRoots.reachable = reachable;
    fixture.reachable = { std::bit_cast<uint64_t>(reachable), std::bit_cast<uint64_t>(reachable->next) };
    myaRoots.array = { new ReachableObject, new ReachableObject, new ReachableObject };
    fixture.arrayElement = std::bit_cast<uint64_t>(myaRoots.array.back());

    auto plant = [&](Planted planted) {
        auto* object = new ReachableObject;
        fixture.planted[static_cast<size_t>(planted)] = std::bit_cast<uint64_t>(object);
        return object;
    };
    myaRoots.vector = { new ReachableObject, new ReachableObject, plant(Planted::VectorElement) };
    myaRoots.set.add(plant(Planted::HashSetBucket));
    myaRoots.compact = plant(Planted::CompactPointer);
    myaRoots.lazy.initLater([](MyaRoots&, LazyUniqueRef<MyaRoots, ReachableObject>& lazy) {
        lazy.set(makeUniqueRef<ReachableObject>());
    });
    fixture.planted[static_cast<size_t>(Planted::LazyPointer)] = std::bit_cast<uint64_t>(&myaRoots.lazy.get(myaRoots));
    myaRoots.packed = plant(Planted::PackedPointer);
    auto* shifted = new (NotNull, fastAlignedMalloc(shiftedPackedAlignment, sizeof(ReachableObject))) ReachableObject;
    fixture.planted[static_cast<size_t>(Planted::ShiftedPackedPointer)] = std::bit_cast<uint64_t>(shifted);
    myaRoots.shiftedPacked = shifted;
    static_assert(decltype(myaRoots.shiftedPacked)::isAlignmentShiftProfitable);
    auto* stale = new ReachableObject;
    fixture.staleInMap = std::bit_cast<uint64_t>(stale);
    myaRoots.map.add(1, stale);
    myaRoots.map.add(2, new ReachableObject);
    myaRoots.map.remove(1);

    fixture.globalObject = std::bit_cast<uint64_t>(globalObject);
    fixture.jsonCache = std::bit_cast<uint64_t>(&vm.jsonCache());
    fixture.builtinExecutables = std::bit_cast<uint64_t>(vm.builtinExecutables());
    fixture.regExpCache = std::bit_cast<uint64_t>(vm.regExpCache());
    fixture.leaked = { std::bit_cast<uint64_t>(fastMalloc(leakedSizes[0])), std::bit_cast<uint64_t>(fastMalloc(leakedSizes[1])) };
    fixture.vm = std::bit_cast<uint64_t>(&vm);
    fixture.object = std::bit_cast<uint64_t>(object);
    fixture.structure = std::bit_cast<uint64_t>(object->structure());
    fixture.structureID = object->structureID().bits();
    fixture.date = std::bit_cast<uint64_t>(date.asCell());
    fixture.string = std::bit_cast<uint64_t>(name.asCell());
    fixture.stringImpl = std::bit_cast<uint64_t>(JSC::asString(name)->tryGetValueImpl());

    vm.heap.collectSync(JSC::CollectionScope::Full);

    JSC::JSValue late = JSC::evaluate(globalObject, JSC::makeSource(lateScript, JSC::SourceOrigin { }, JSC::SourceTaintedOrigin::Untainted), JSC::JSValue(), exception);
    RELEASE_ASSERT(!exception && late.isCell());
    fixture.lateObject = std::bit_cast<uint64_t>(late.asCell());
    return Address { &fixture };
}

// What the analysis checks the walk against: JSC's own list of live cells,
// then libpas's allocations. At the safe point, last before the snapshot, so
// that the target allocates nothing from the heap after it. Both lists are
// in system malloc, which is not counted.
void recordFixture(Address fixtureAddress)
{
    HeapWalkFixture& fixture = fixtureAt(fixtureAddress);
    JSC::VM& vm = *rootsOf(fixtureAddress).vm;

    using Buffer = decltype(MallocSpan<uint64_t, SystemMalloc>::malloc(0));
    static NeverDestroyed<Buffer> liveCells { MallocSpan<uint64_t, SystemMalloc>::malloc(recordCapacity * sizeof(uint64_t)) };
    std::span<uint64_t> cells = liveCells->mutableSpan();
    size_t count = 0;
    {
        JSC::HeapIterationScope iterationScope(vm.heap);
        vm.heap.objectSpace().forEachLiveCell(iterationScope, [&](JSC::HeapCell* cell, JSC::HeapCell::Kind) {
            RELEASE_ASSERT(count < cells.size());
            cells[count++] = std::bit_cast<uint64_t>(cell);
            return IterationStatus::Continue;
        });
    }
    std::ranges::sort(cells.first(count));
    fixture.liveCells = std::bit_cast<uint64_t>(cells.data());
    fixture.liveCellCount = count;

    // Last before the enumeration, so that nothing reuses them first.
    for (size_t index = 0; index < freedSizes.size(); ++index) {
        void* object = fastMalloc(freedSizes[index]);
        fixture.freed[index] = std::bit_cast<uint64_t>(object);
        fastFree(object);
    }
    std::span<HeapWalk::Allocation> allocations = enumerateAllocations();
    fixture.allocations = std::bit_cast<uint64_t>(allocations.data());
    fixture.allocationCount = allocations.size();
}

// For the target process: records the fixture and reports it from the safe point, and stays there.
Address createFixtureAtSafePoint()
{
    Address fixture = createFixture();
    bool atSafePoint = withMyaSafePoint(rootsOf(fixture), [&] {
        recordFixture(fixture);
        reportTargetObjectAndPark(fixture);
    });
    RELEASE_ASSERT(atSafePoint);
    RELEASE_ASSERT_NOT_REACHED();
}

// A cell the walk reported, with what its header said if it is a JS cell.
struct LiveCell {
    HeapWalk::Cell cell;
    std::optional<JSC::JSType> type;
    std::optional<uint32_t> structureID;
};

// Every live cell of the target's heap, by address.
class LiveCells {
public:
    LiveCells(Snapshot& snapshot, const HeapWalk& heap)
        : m_snapshot(snapshot)
        , m_heap(heap)
    {
    }

    void collect();

    // Reads each JS cell's header, and checks it against its Structure.
    void checkHeaders();

    const LiveCell* cell(Address address) const
    {
        auto entry = m_cells.find(address.toTargetVMAddress());
        return entry == m_cells.end() ? nullptr : &entry->value;
    }

    size_t count() const { return m_cells.size(); }
    const HashMap<uint64_t, LiveCell>& all() const { return m_cells; }
    Vector<uint64_t> sortedAddresses() const
    {
        Vector<uint64_t> addresses = copyToVector(m_cells.keys());
        std::ranges::sort(addresses);
        return addresses;
    }
    const Vector<Address>& cellsWithJSMagic() const { return m_cellsWithJSMagic; }
    const Vector<Address>& cellsWithDateMagic() const { return m_cellsWithDateMagic; }

private:
    void scan(const HeapWalk::Cell&);

    Snapshot& m_snapshot;
    const HeapWalk& m_heap;
    HashMap<uint64_t, LiveCell> m_cells;
    Vector<Address> m_cellsWithJSMagic;
    Vector<Address> m_cellsWithDateMagic;
};

void LiveCells::scan(const HeapWalk::Cell& cell)
{
    if (cell.size > maximumScannedCellSize)
        return;
    auto bytes = m_snapshot.memory().span<uint8_t>(cell.address, static_cast<size_t>(cell.size));
    if (!bytes) {
        TEST_ASSERT(false, "a live cell reads whole");
        return;
    }
    const uint64_t encodedJSMagic = static_cast<uint64_t>(JSC::JSValue::encode(JSC::jsNumber(jsMagic)));
    const uint64_t rawDateMagic = std::bit_cast<uint64_t>(dateMagic);
    for (size_t offset = 0; offset + sizeof(uint64_t) <= bytes.size(); offset += sizeof(uint64_t)) {
        uint64_t word = 0;
        memcpySpan(asMutableByteSpan(word), bytes.subspan(offset, sizeof(word)));
        if (word == encodedJSMagic)
            m_cellsWithJSMagic.append(cell.address);
        if (word == rawDateMagic)
            m_cellsWithDateMagic.append(cell.address);
    }
}

void LiveCells::collect()
{
    unsigned jsCells = 0;
    unsigned auxiliaryCells = 0;
    m_heap.forEachLiveCell([&](const HeapWalk::Cell& cell) {
        if (JSC::isJSCellKind(cell.kind))
            ++jsCells;
        else
            ++auxiliaryCells;
        m_cells.add(cell.address.toTargetVMAddress(), LiveCell { cell, std::nullopt, std::nullopt });
        scan(cell);
        return IterationStatus::Continue;
    });
    if (verbose)
        dataLogLn("    ", jsCells, " live JS cells and ", auxiliaryCells, " live auxiliary cells");
}

// The type byte of a JSCell's header, which sits in an anonymous struct in an
// anonymous union, which the debug info lists as fields without a name.
std::optional<int64_t> jsCellType(const Remote<JSC::JSCell>& cell)
{
    return cell.field<void>("").field<void>("").field<JSC::JSType>("m_type").integer();
}

void LiveCells::checkHeaders()
{
    unsigned unreadable = 0;
    unsigned implausible = 0;
    for (LiveCell& cell : m_cells.values()) {
        if (!JSC::isJSCellKind(cell.cell.kind))
            continue;
        Remote<JSC::JSCell> header = m_heap.jsCell(cell.cell.address);
        auto structureID = header.field<void>("m_structureID").field<uint32_t>("m_bits").integer();
        auto type = jsCellType(header);
        if (!structureID || !type) {
            ++unreadable;
            continue;
        }
        if (!(*structureID & ~JSC::StructureID::nukedStructureIDBit) || *type < 0 || *type > JSC::LastJSCObjectType) {
            if (!implausible++)
                dataLogLn("    cell 0x", hex(cell.cell.address.toTargetVMAddress()), " has StructureID ", *structureID, " and type ", *type);
            continue;
        }
        cell.type = static_cast<JSC::JSType>(*type);
        cell.structureID = static_cast<uint32_t>(*structureID);
    }
    TEST_ASSERT_EQ(unreadable, 0u, "every live JS cell's header reads");
    TEST_ASSERT_EQ(implausible, 0u, "every live JS cell has a StructureID and a JSC type");

    unsigned missing = 0;
    unsigned notStructures = 0;
    unsigned mismatched = 0;
    unsigned checked = 0;
    for (const LiveCell& cell : m_cells.values()) {
        if (!cell.structureID)
            continue;
        Remote<JSC::Structure> structure = m_heap.structure(*cell.structureID);
        const LiveCell* structureCell = this->cell(structure.address());
        if (!structureCell) {
            if (!missing++)
                dataLogLn("    cell 0x", hex(cell.cell.address.toTargetVMAddress()), " points at 0x", hex(structure.address().toTargetVMAddress()), ", which is not a live cell");
            continue;
        }
        if (structureCell->type != JSC::StructureType) {
            ++notStructures;
            continue;
        }
        auto type = structure.field<void>("m_blob").field<void>("u").field<void>("fields").field<JSC::JSType>("type").integer();
        if (!type || *type != *cell.type) {
            if (!mismatched++)
                dataLogLn("    cell 0x", hex(cell.cell.address.toTargetVMAddress()), " has type ", static_cast<unsigned>(*cell.type), " but its Structure says ", type ? *type : -1);
            continue;
        }
        ++checked;
    }
    TEST_ASSERT_EQ(missing, 0u, "every live JS cell's Structure is a live cell");
    TEST_ASSERT_EQ(notStructures, 0u, "every live JS cell's Structure is a Structure");
    TEST_ASSERT_EQ(mismatched, 0u, "every live JS cell's type is the type its Structure gives");
    if (verbose)
        dataLogLn("    ", checked, " JS cells agree with their Structures");
}

// Collects the cells a walk of a value visits.
struct CellCollector {
    Vector<Address> cells;

    void visit(const Remote<JSC::JSCell>& cell) { cells.append(cell.address()); }
};

void checkFixture(Snapshot& snapshot, const HeapWalk& heap, const LiveCells& cells, const HeapWalkFixture& fixture)
{
    Address object { fixture.object };
    Address date { fixture.date };

    const LiveCell* fixtureCell = cells.cell(object);
    TEST_ASSERT(fixtureCell && fixtureCell->type == JSC::FinalObjectType, "the walk finds the fixture, a final object");
    TEST_ASSERT(cells.cellsWithJSMagic().size() == 1 && cells.cellsWithJSMagic()[0] == object, "the fixture is the one cell holding the JS magic");
    TEST_ASSERT(fixtureCell && fixtureCell->structureID == fixture.structureID, "the fixture's header holds the StructureID the target recorded");

    Remote<JSC::Structure> structure = heap.structure(fixture.structureID);
    TEST_ASSERT(structure.address() == Address { fixture.structure }, "the StructureID decodes to the Structure the target recorded");
    const LiveCell* structureCell = cells.cell(Address { fixture.structure });
    TEST_ASSERT(structureCell && structureCell->type == JSC::StructureType, "the walk finds the fixture's Structure");

    auto inlineCapacity = structure.field<uint8_t>("m_inlineCapacity").integer();
    TEST_ASSERT(inlineCapacity && *inlineCapacity >= 2, "the fixture's Structure has room for the two slots read below");

    // The debug info attaches a type to a Structure's prototype slot, which is
    // what every JSValue slot looks like. The target is this build, so this
    // build's inline storage offset is the target's.
    Remote<JSC::JSValue> magicSlot = structure.field<void>("m_prototype").base<void>(0).field<JSC::JSValue>("m_value").at(object + JSC::JSObject::offsetOfInlineStorage());
    auto magic = magicSlot.as<uint64_t>();
    TEST_ASSERT(magic && *magic == static_cast<uint64_t>(JSC::JSValue::encode(JSC::jsNumber(jsMagic))), "the fixture's first inline slot holds the JS magic");

    CellCollector magicCells;
    visitChildren(magicSlot, magicCells);
    TEST_ASSERT(magicCells.cells.isEmpty(), "an int32 holds no cell");

    CellCollector dateCells;
    visitChildren(magicSlot.offsetBy(1), dateCells);
    TEST_ASSERT(dateCells.cells.size() == 1 && dateCells.cells[0] == date, "the fixture's second inline slot holds the date");

    const LiveCell* dateCell = cells.cell(date);
    TEST_ASSERT(dateCell && dateCell->type == JSC::JSDateType, "the walk finds the date");
    TEST_ASSERT(cells.cellsWithDateMagic().size() == 1 && cells.cellsWithDateMagic()[0] == date, "the date is the one cell holding the C++ magic");
    auto internalNumber = Remote<double>(snapshot, date + JSC::DateInstance::offsetOfInternalNumber()).as<double>();
    TEST_ASSERT(internalNumber && *internalNumber == dateMagic, "the date's C++ double reads back");
}

// The reach (patch 9), the readers it needs (patches 10 and 11), and how much
// of what it reaches it reads as a type (patch 12).
void checkReach(Snapshot& snapshot, const HeapWalk& heap, const HeapWalkFixture& fixture)
{
    auto enumerated = snapshot.memory().span<HeapWalk::Allocation>(Address { fixture.allocations }, static_cast<size_t>(fixture.allocationCount));
    TEST_ASSERT(enumerated && enumerated.size(), "the allocations libpas enumerated in the target read");
    if (!enumerated)
        return;
    Vector<HeapWalk::Allocation> allocations { std::span<const HeapWalk::Allocation> { enumerated } };
    auto indexOf = [&](uint64_t address) -> std::optional<size_t> {
        auto span = allocations.span();
        auto after = std::ranges::upper_bound(span, Address { address }, { }, &HeapWalk::Allocation::address);
        if (after == span.begin() || address - (after - 1)->address.toTargetVMAddress() >= (after - 1)->size)
            return std::nullopt;
        return (after - 1) - span.begin();
    };
    auto isObject = [&](uint64_t address, size_t size) {
        auto index = indexOf(address);
        return index && allocations[*index].address == Address { address } && allocations[*index].size >= size;
    };

    unsigned overlapping = 0;
    for (size_t index = 1; index < allocations.size(); ++index) {
        if (allocations[index - 1].address + allocations[index - 1].size > allocations[index].address)
            ++overlapping;
    }
    TEST_ASSERT_EQ(overlapping, 0u, "no two enumerated allocations overlap");
    TEST_ASSERT(isObject(fixture.reachable[0], sizeof(ReachableObject)) && isObject(fixture.reachable[1], sizeof(ReachableObject))
        && isObject(fixture.leaked[0], leakedSizes[0]) && isObject(fixture.leaked[1], leakedSizes[1]),
        "libpas enumerates each of the fixture's C++ objects, at least as large as it was allocated");
    TEST_ASSERT(!indexOf(fixture.freed[0]) && !indexOf(fixture.freed[1]), "libpas enumerates no freed object, small or large");
    unsigned cellsOutside = 0;
    heap.forEachLiveCell([&](const HeapWalk::Cell& cell) {
        if (!indexOf(cell.address.toTargetVMAddress()))
            ++cellsOutside;
        return IterationStatus::Continue;
    });
    TEST_ASSERT_EQ(cellsOutside, 0u, "every live cell is in an enumerated allocation: the JS heap's blocks are libpas objects");

    // The allocations the target leaks on purpose, and the one it plants to be missed, are expected misses.
    Vector<HeapWalk::Allocation> excluded;
    uint64_t excludedBytes = 0;
    for (uint64_t address : { fixture.leaked[0], fixture.leaked[1], fixture.staleInMap }) {
        if (auto index = indexOf(address)) {
            excluded.append(allocations[*index]);
            excludedBytes += allocations[*index].size;
        }
    }
    std::ranges::sort(excluded, { }, &HeapWalk::Allocation::address);
    TEST_ASSERT_EQ(excluded.size(), 3u, "libpas enumerates the objects the target means the walk to miss");

    MonotonicTime start = MonotonicTime::now();
    HeapWalk::Reach reach = heap.reach(allocations, excluded, 10);
    Seconds duration = MonotonicTime::now() - start;
    auto isReached = [&](uint64_t address) {
        auto index = indexOf(address);
        return index && reach.isReached[*index];
    };
    TEST_ASSERT(std::ranges::all_of(fixture.reachable, isReached), "the reach walk reaches the objects reachable from the roots");
    TEST_ASSERT(isReached(fixture.arrayElement), "and the last element of a C array, through the array");
    TEST_ASSERT(std::ranges::none_of(fixture.leaked, isReached), "and misses the objects the target leaked");
    TEST_ASSERT(reach.bytesReached && reach.bytesReached <= reach.bytesAllocated - reach.bytesExcluded, "the walk reaches part of the heap");

    // The percentage counts only the program's own memory.
    TEST_ASSERT_EQ(reach.bytesExcluded, excludedBytes, "the excluded bytes are the sizes of the excluded allocations");
    HeapWalk::Reach unexcluded = heap.reach(allocations, { }, 0);
    TEST_ASSERT_EQ(unexcluded.bytesReached, reach.bytesReached, "excluding missed allocations changes what the walk reaches by nothing");
    TEST_ASSERT(!unexcluded.bytesExcluded && unexcluded.percent() == 100.0 * reach.bytesReached / reach.bytesAllocated
        && reach.percent() == 100.0 * reach.bytesReached / (reach.bytesAllocated - excludedBytes),
        "excluding them raises the percentage by exactly their bytes' share");

    // Patch 10: JS cells, read as their classes, lead to the VM as JavaScriptCore describes it.
    TEST_ASSERT(isReached(fixture.jsonCache) && isReached(fixture.builtinExecutables) && isReached(fixture.regExpCache),
        "the walk reaches what the VM owns through pointers to classes this executable only declares");
    TEST_ASSERT_EQ(reach.notFollowed[static_cast<size_t>(HeapWalk::NotFollowed::CellWithoutClass)], 0u, "the walk reads every JS cell as its class");
    // The name is not an atom, so only its JSString holds its StringImpl.
    TEST_ASSERT(isReached(fixture.stringImpl), "the walk reaches the StringImpl of a JSString");

    // Patch 11: each planted object is behind a value the walk reads by its C++ source's logic.
    for (size_t index = 0; index < numberOfPlanted; ++index)
        TEST_ASSERT(fixture.planted[index] && isReached(fixture.planted[index]), makeString("the walk reaches the object behind "_s, plantedNames[index]));
    TEST_ASSERT(!isReached(fixture.staleInMap), "and misses the one held only as the stale value of a deleted HashMap entry");

    // Patch 12: the walk reads each planted object as its class.
    for (size_t index = 0; index < numberOfPlanted; ++index) {
        auto allocation = indexOf(fixture.planted[index]);
        TEST_ASSERT(allocation && reach.bytesTypedIn[*allocation] == sizeof(ReachableObject), makeString("the walk types the whole object behind "_s, plantedNames[index]));
    }
    TEST_ASSERT(reach.bytesTyped && reach.bytesTyped <= reach.bytesAllocated - reach.bytesExcluded - reach.bytesFreeInBlocks, "the walk types part of the heap");

    if (!verbose)
        return;
    dataLogLn("    the walk reaches ", reach.percent(), "% of the heap: ", reach.bytesReached, " of ", reach.bytesAllocated, " bytes, ",
        reach.bytesExcluded, " excluded, in ", allocations.size(), " libpas allocations, in ", duration.milliseconds(), " ms");
    dataLogLn("    it reads ", reach.typedPercent(), "% of the heap as a type: ", reach.bytesTyped, " bytes, with ", reach.bytesFreeInBlocks, " free in MarkedBlocks");
    dataLogLn("    ", reach.cellsWithClass, " JS cells read as their class, with ", reach.cellBytesBeyondClass, " bytes beyond their class");
    constexpr std::array<ASCIILiteral, HeapWalk::numberOfNotFollowedReasons> reasons {
        "pointers to declarations"_s, "pointers to types without a size"_s, "polymorphic pointees without a dynamic type"_s,
        "JS cells without a class"_s, "encoded pointers to types not reached"_s, "HashTables with key traits not WTF's defaults"_s,
    };
    for (size_t index = 0; index < reasons.size(); ++index)
        dataLogLn("    not followed: ", reach.notFollowed[index], " ", reasons[index]);
    for (const HeapWalk::Allocation& missed : reach.largestMissed)
        dataLogLn("    missed: ", missed.size, " bytes at 0x", hex(missed.address.toTargetVMAddress()));
    for (const HeapWalk::Untyped& untyped : reach.mostUntyped)
        dataLogLn("    untyped: ", untyped.bytesUntyped, " of ", untyped.allocation.size, " bytes at 0x", hex(untyped.allocation.address.toTargetVMAddress()),
            ", read as ", untyped.typeAtStart.isNull() ? "nothing at its start"_s : untyped.typeAtStart);
    for (const String& overrun : reach.overruns)
        dataLogLn("    overrun: ", overrun);
}

// Patch 10: every live JS cell is read as the C++ class its ClassInfo names.
void checkCellClasses(const HeapWalk& heap, const LiveCells& cells, const HeapWalkFixture& fixture)
{
    unsigned withoutClass = 0;
    unsigned biggerThanCell = 0;
    HashSet<const TargetType*> classes;
    for (const LiveCell& cell : cells.all().values()) {
        if (!JSC::isJSCellKind(cell.cell.kind))
            continue;
        const TargetType* klass = heap.cellClass(cell.cell.address);
        if (!klass) {
            ++withoutClass;
            continue;
        }
        if (klass->byteSize() > cell.cell.size) {
            if (!biggerThanCell++)
                dataLogLn("    cell 0x", hex(cell.cell.address.toTargetVMAddress()), " is ", cell.cell.size, " bytes, and its class ", klass->name(), " ", klass->byteSize());
        }
        classes.add(klass);
    }
    TEST_ASSERT_EQ(withoutClass, 0u, "every live JS cell's ClassInfo names its C++ class");
    TEST_ASSERT_EQ(biggerThanCell, 0u, "no live JS cell is smaller than its class");
    if (verbose)
        dataLogLn("    the live JS cells are of ", classes.size(), " C++ classes");

    auto isOfClass = [&](uint64_t cell, std::string_view name) {
        const TargetType* klass = heap.cellClass(Address { cell });
        return klass && std::string_view { klass->name().legacyCStringPointer() } == name;
    };
    TEST_ASSERT(isOfClass(fixture.object, "JSC::JSFinalObject"), "the fixture is a JSFinalObject");
    TEST_ASSERT(isOfClass(fixture.date, "JSC::DateInstance"), "its date is a DateInstance");
    TEST_ASSERT(isOfClass(fixture.globalObject, "JSC::JSGlobalObject"), "the global object is a JSGlobalObject");
    TEST_ASSERT(isOfClass(fixture.string, "JSC::JSString"), "its name is a JSString");
}

// The walk and JSC's own MarkedSpace::forEachLiveCell, run in the target, must visit the same cells.
void checkAgainstJSC(Snapshot& snapshot, const LiveCells& cells, const HeapWalkFixture& fixture)
{
    auto expected = snapshot.memory().span<uint64_t>(Address { fixture.liveCells }, static_cast<size_t>(fixture.liveCellCount));
    TEST_ASSERT(expected, "the live cells JSC listed in the target read");
    if (!expected)
        return;
    Vector<uint64_t> found = cells.sortedAddresses();
    TEST_ASSERT_EQ(found.size(), expected.size(), "the walk visits as many cells as JSC's forEachLiveCell");

    unsigned missing = 0;
    unsigned extra = 0;
    size_t foundIndex = 0;
    size_t expectedIndex = 0;
    while (foundIndex < found.size() || expectedIndex < expected.size()) {
        if (expectedIndex == expected.size() || (foundIndex < found.size() && found[foundIndex] < expected[expectedIndex])) {
            if (!extra++)
                dataLogLn("    the walk visits 0x", hex(found[foundIndex]), ", which JSC does not");
            ++foundIndex;
        } else if (foundIndex == found.size() || expected[expectedIndex] < found[foundIndex]) {
            if (!missing++)
                dataLogLn("    JSC visits 0x", hex(expected[expectedIndex]), ", which the walk does not");
            ++expectedIndex;
        } else {
            ++foundIndex;
            ++expectedIndex;
        }
    }
    TEST_ASSERT_EQ(missing, 0u, "the walk visits every cell JSC's forEachLiveCell visits");
    TEST_ASSERT_EQ(extra, 0u, "the walk visits only cells JSC's forEachLiveCell visits");

    const LiveCell* late = cells.cell(Address { fixture.lateObject });
    TEST_ASSERT(late && late->type == JSC::FinalObjectType, "the walk finds an object allocated after the last collection");
}

void analyze(Snapshot& snapshot, Address fixtureAddress)
{
    auto fixture = snapshot.memory().ptr<HeapWalkFixture>(fixtureAddress);
    TEST_ASSERT(fixture, "the target's fixture reads");
    if (!fixture)
        return;

    HeapWalk heap(snapshot, Address { fixture->roots });
    TEST_ASSERT(heap.isValid(), "the target's heap is found from its roots");
    if (!heap.isValid())
        return;
    TEST_ASSERT(heap.vm().address() == Address { fixture->vm }, "the walk starts from the roots' VM");
    TEST_ASSERT(heap.isAtSafePoint(), "the target was snapshotted at mya's safe point, so the walk is exact");

    LiveCells cells(snapshot, heap);
    cells.collect();
    TEST_ASSERT(cells.count() > 100, "a VM with a global object has hundreds of live cells");
    cells.checkHeaders();
    checkFixture(snapshot, heap, cells, *fixture);
    checkAgainstJSC(snapshot, cells, *fixture);
    checkCellClasses(heap, cells, *fixture);
    checkReach(snapshot, heap, *fixture);
}

// A snapshot taken anywhere else is walked all the same, and is unverified.
void analyzeAwayFromSafePoint(Snapshot& snapshot, Address fixtureAddress)
{
    auto fixture = snapshot.memory().ptr<HeapWalkFixture>(fixtureAddress);
    TEST_ASSERT(fixture, "the target's fixture reads");
    if (!fixture)
        return;
    HeapWalk heap(snapshot, Address { fixture->roots });
    TEST_ASSERT(heap.isValid(), "the heap of a VM away from the safe point is found from its roots");
    TEST_ASSERT(!heap.isAtSafePoint(), "and the walk is marked unverified");
    unsigned visited = 0;
    heap.forEachLiveCell([&](const HeapWalk::Cell&) {
        ++visited;
        return IterationStatus::Continue;
    });
    TEST_ASSERT(visited > 100, "a snapshot away from the safe point is walked");
}

// Runs `during` inside a full collection of the fixture's VM, from
// HeapObserver::willGarbageCollect, which runs once Heap::m_collectionScope is set.
class DuringCollection final : public JSC::HeapObserver {
public:
    explicit DuringCollection(Function<void()>&& during)
        : m_during(WTF::move(during))
    {
    }

    void willGarbageCollect() final
    {
        if (auto during = std::exchange(m_during, nullptr))
            during();
    }
    void didGarbageCollect(JSC::CollectionScope) final { }

private:
    Function<void()> m_during;
};

void checkSafePointRefusesCollection(Address fixture)
{
    JSC::VM& vm = *rootsOf(fixture).vm;
    JSC::JSLockHolder locker(vm);
    std::optional<bool> atSafePoint;
    DuringCollection observer([&] {
        atSafePoint = withMyaSafePoint(rootsOf(fixture), [] { });
    });
    vm.heap.addObserver(&observer);
    vm.heap.collectSync(JSC::CollectionScope::Full);
    vm.heap.removeObserver(&observer);
    TEST_ASSERT(atSafePoint && !*atSafePoint, "inside a full collection, the safe point refuses to snapshot");
}

} // anonymous namespace

void testHeapWalk()
{
    SuiteTracer tracer("HeapWalk");
    if (!tracer.shouldRun())
        return;

    Address fixture = createFixture();
    TEST_ASSERT(!JSC::Options::useConcurrentGC() && !JSC::Options::useConcurrentJIT() && !JSC::Options::useWarmUpMarkedBlocks(),
        "the target is quiet: JSC was initialized with a measured target's options");
    std::unique_ptr<Snapshot> snapshot;
    bool atSafePoint = withMyaSafePoint(rootsOf(fixture), [&] {
        recordFixture(fixture);
        snapshot = takeSnapshot(getpid());
    });
    TEST_ASSERT(atSafePoint, "the test enters mya's safe point");
    if (snapshot)
        analyze(*snapshot, fixture);
    snapshot = nullptr;
    analyzeInSeparateProcess(createFixtureAtSafePoint, analyze);
    analyzeAfterTargetExits(createFixtureAtSafePoint, analyze);

    checkSafePointRefusesCollection(createFixture());
    analyzeInAndOutOfProcess(createFixture, analyzeAwayFromSafePoint);
}

} // namespace JSCToolsTest

#else // HAVE(LLDB)

namespace JSCToolsTest {

void testHeapWalk()
{
    SuiteTracer tracer("HeapWalk");
    if (!tracer.shouldRun())
        return;

#if ENABLE(MYA_HEAP)
    TEST_ASSERT(false, "mya_heap is enabled but liblldb's headers were not found");
#else
    skipSuite("HeapWalk", "mya_heap is not enabled");
#endif
}

} // namespace JSCToolsTest

#endif // HAVE(LLDB)
