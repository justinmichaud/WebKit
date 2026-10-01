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
#include <JavaScriptCore/SourceCode.h>
#include <JavaScriptCore/StrongInlines.h>
#include <JavaScriptCore/StructureID.h>
#include <JavaScriptCore/VM.h>
#include <bmalloc/bmalloc_heap_config.h>
#include <bmalloc/bmalloc_type.h>
#include <bmalloc/pas_enumerator.h>
#include <bmalloc/pas_get_heap.h>
#include <bmalloc/pas_get_object_kind.h>
#include <bmalloc/pas_heap.h>
#include <bmalloc/pas_heap_lock.h>
#include <bmalloc/pas_root.h>
#include <algorithm>
#include <bit>
#include <optional>
#include <stdint.h>
#include <stdlib.h>
#include <string_view>
#include <unistd.h>
#include <utility>
#include <wtf/Threading.h>
#include <wtf/threads/BinarySemaphore.h>
#include <wtf/CompactPtr.h>
#include <wtf/HashMap.h>
#include <wtf/HashSet.h>
#include <wtf/HexNumber.h>
#include <wtf/LazyUniqueRef.h>
#include <wtf/IterationStatus.h>
#include <wtf/NeverDestroyed.h>
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
// them, one for each way the walk reads such a value.
enum class Planted : uint8_t {
    VectorElement, // The last element of a Vector, past its first.
    HashSetBucket, // Every bucket of a HashSet.
    ArrayElement, // The last element of a C array.
    CompactPointer, // A CompactPtr, which holds an integer.
    LazyPointer, // An initialized LazyUniqueRef, which holds an integer with tag bits.
};
constexpr size_t numberOfPlanted = static_cast<size_t>(Planted::LazyPointer) + 1;
constexpr std::array<ASCIILiteral, numberOfPlanted> plantedNames {
    "the last element of a Vector"_s,
    "a HashSet's value"_s,
    "the last element of a C array"_s,
    "a CompactPtr"_s,
    "a LazyUniqueRef"_s,
};

// Objects the walk misses, each for a reason the attribution has to give
// (patch 12). The fixture records their addresses complemented, so that it does
// not hold a pointer to them itself.
enum class Missed : uint8_t {
    InInteger, // Held only in an integer field.
    ThroughMissed, // Held only by the object held in the integer.
    BehindVoidPointer, // Held only in a void*.
    BehindDeclaration, // Held only in a pointer to a class that is defined nowhere.
    OnlyOnStack, // Held only on a parked thread's stack.
    OnlyInGlobal, // Held only in a global.
    WrongType, // A 64-byte object held only in a pointer to a 4096-byte class.
};
constexpr size_t numberOfMissed = static_cast<size_t>(Missed::WrongType) + 1;
constexpr size_t wrongTypeObjectSize = 64;

// Declared, and defined nowhere.
struct NeverDefined;

struct BigObject {
    std::array<uint8_t, 4096> bytes;
};

ReachableObject* onlyInAGlobal { nullptr };

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
    ReachableObject* reachable { nullptr };

    // Each holds a planted object, as Planted says.
    Vector<ReachableObject*> vector;
    HashSet<ReachableObject*> set;
    std::array<ReachableObject*, 3> array { };
    CompactPtr<ReachableObject> compact;
    LazyUniqueRef<MyaRoots, ReachableObject> lazy;

    // Only declared in this executable, and polymorphic in JavaScriptCore.
    JSC::RegExpCache* regExpCache { nullptr };

    // Each holds a missed object, as Missed says.
    uintptr_t integer { 0 };
    void* opaque { nullptr };
    NeverDefined* declared { nullptr };
    BigObject* wrongType { nullptr };
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
    std::array<uint64_t, 2> leaked { }; // Held only here, as integers.
    std::array<uint64_t, 2> freed { }; // A small and a large object, freed before the enumeration.
    uint64_t allocations { 0 }; // What libpas enumerates in the target, sorted.
    uint64_t allocationCount { 0 };
    std::array<uint64_t, numberOfPlanted> planted { };
    uint64_t globalObject { 0 };
    uint64_t string { 0 }; // The JSString of the fixture's name.
    uint64_t stringImpl { 0 }; // Its StringImpl.
    // Objects the VM owns through pointers to classes this executable only declares.
    uint64_t jsonCache { 0 };
    uint64_t builtinExecutables { 0 };
    uint64_t regExpCache { 0 };
    std::array<uint64_t, numberOfMissed> missed { }; // Complemented.
    // The target's own buffers, which hold the address of every allocation.
    uint64_t allocationsCapacity { 0 };
    uint64_t heapPages { 0 }; // The pages libpas holds objects in, sorted.
    uint64_t heapPageCount { 0 };
    uint64_t heapPagesCapacity { 0 };
};

// Every live libpas object of this process, as libpas enumerates a heap:
// pas_enumerator reads through the reader, which here is this process's own
// memory. The records go into buffers from system malloc, allocated up front,
// so that the enumeration allocates nothing from the heap it enumerates.
struct EnumeratedAllocations {
    HeapWalk::Allocation* objects;
    size_t objectCount;
    HeapWalk::Allocation* pages; // The pages libpas holds objects in.
    size_t pageCount;
    size_t capacity;
};

void* readOwnMemory(pas_enumerator*, void* address, size_t, void*)
{
    return address;
}

void recordObject(pas_enumerator*, void* address, size_t size, pas_enumerator_record_kind kind, void* argument)
{
    auto& records = *static_cast<EnumeratedAllocations*>(argument);
    if (kind == pas_enumerator_object_record) {
        RELEASE_ASSERT(records.objectCount < records.capacity);
        records.objects[records.objectCount++] = { Address { address }, size };
    } else if (kind == pas_enumerator_payload_record) {
        RELEASE_ASSERT(records.pageCount < records.capacity);
        records.pages[records.pageCount++] = { Address { address }, size };
    }
}

// What libpas knows of the object, as the heap's own lookups find it.
HeapWalk::AllocationFacts factsOf(void* object)
{
    HeapWalk::AllocationFacts facts;
    pas_object_kind kind = pas_get_object_kind(object, bmalloc_heap_config);
    facts.objectKind = kind;
    if (kind == pas_not_an_object_kind)
        return facts;
    pas_heap* heap = pas_get_heap(object, bmalloc_heap_config);
    if (!heap)
        return facts;
    facts.heap = std::bit_cast<uint64_t>(heap);
    facts.typeSize = static_cast<uint32_t>(pas_heap_get_type_size(heap));
    facts.typeAlignment = static_cast<uint32_t>(pas_heap_get_type_alignment(heap));
    if (heap->config_kind == pas_heap_config_kind_bmalloc && heap->type)
        facts.typeName = std::bit_cast<uint64_t>(bmalloc_type_name(reinterpret_cast<const bmalloc_type*>(heap->type)));
    return facts;
}

EnumeratedAllocations& enumerateAllocations()
{
    constexpr size_t capacity = 4 * 1024 * 1024;
    static EnumeratedAllocations records {
        static_cast<HeapWalk::Allocation*>(malloc(capacity * sizeof(HeapWalk::Allocation))), 0,
        static_cast<HeapWalk::Allocation*>(malloc(capacity * sizeof(HeapWalk::Allocation))), 0,
        capacity,
    };
    RELEASE_ASSERT(records.objects && records.pages);
    records.objectCount = 0;
    records.pageCount = 0;
    // The heap lock keeps libpas's other threads from changing the heap during the enumeration.
    pas_heap_lock_lock();
    static pas_root* root = pas_root_create();
    pas_enumerator* enumerator = pas_enumerator_create(root, readOwnMemory, nullptr, recordObject, &records,
        pas_enumerator_do_not_record_meta_records, pas_enumerator_record_payload_records, pas_enumerator_record_object_records);
    RELEASE_ASSERT(enumerator && pas_enumerator_enumerate_all(enumerator));
    pas_enumerator_destroy(enumerator);
    pas_heap_lock_unlock();
    std::span<HeapWalk::Allocation> objects { records.objects, records.objectCount };
    std::ranges::sort(objects, { }, &HeapWalk::Allocation::address);
    std::ranges::sort(std::span { records.pages, records.pageCount }, { }, &HeapWalk::Allocation::address);
    // libpas's lookups take the heap lock themselves.
    for (HeapWalk::Allocation& object : objects)
        object.facts = factsOf(std::bit_cast<void*>(static_cast<uintptr_t>(object.address.toTargetVMAddress())));
    return records;
}

// Holds `object` on the stack of a thread of its own, which then parks there
// for the life of the process. It is given complemented, so that nothing but
// that stack holds its address.
void holdOnStack(uint64_t complemented)
{
    static NeverDestroyed<BinarySemaphore> holding;
    Thread::create("myaStackHolder"_s, [complemented] {
        volatile uint64_t onStack = ~complemented;
        holding->signal();
        for (;;) {
            pause();
            UNUSED_VARIABLE(onStack);
        }
    })->detach();
    holding->wait();
}

// Makes a VM, evaluates targetScript in it, collects, then evaluates
// lateScript, so that the live heap has cells from both sides of its last
// collection. The VM and its global object are kept for the life of the process.
Address createFixture()
{
    static HeapWalkFixture fixture;
    static NeverDestroyed<std::optional<MyaRoots>> roots; // Remade with each fixture.
    static NeverDestroyed<JSC::Strong<JSC::JSGlobalObject>> root;

    JSC::initialize();
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
    fixture.roots = std::bit_cast<uint64_t>(&myaRoots);
    auto* reachable = new ReachableObject;
    reachable->next = new ReachableObject;
    myaRoots.reachable = reachable;
    fixture.reachable = { std::bit_cast<uint64_t>(reachable), std::bit_cast<uint64_t>(reachable->next) };

    auto plant = [&](Planted planted) {
        auto* object = new ReachableObject;
        fixture.planted[static_cast<size_t>(planted)] = std::bit_cast<uint64_t>(object);
        return object;
    };
    myaRoots.vector = { new ReachableObject, new ReachableObject, plant(Planted::VectorElement) };
    myaRoots.set.add(plant(Planted::HashSetBucket));
    myaRoots.array = { new ReachableObject, new ReachableObject, plant(Planted::ArrayElement) };
    myaRoots.compact = plant(Planted::CompactPointer);
    myaRoots.lazy.initLater([](MyaRoots&, LazyUniqueRef<MyaRoots, ReachableObject>& lazy) {
        lazy.set(makeUniqueRef<ReachableObject>());
    });
    fixture.planted[static_cast<size_t>(Planted::LazyPointer)] = std::bit_cast<uint64_t>(&myaRoots.lazy.get(myaRoots));
    myaRoots.regExpCache = vm.regExpCache();

    auto miss = [&](Missed missed, void* object) {
        fixture.missed[static_cast<size_t>(missed)] = ~std::bit_cast<uint64_t>(object);
        return object;
    };
    auto* inInteger = new ReachableObject;
    inInteger->next = static_cast<ReachableObject*>(miss(Missed::ThroughMissed, new ReachableObject));
    myaRoots.integer = std::bit_cast<uintptr_t>(miss(Missed::InInteger, inInteger));
    myaRoots.opaque = miss(Missed::BehindVoidPointer, new ReachableObject);
    myaRoots.declared = static_cast<NeverDefined*>(miss(Missed::BehindDeclaration, new ReachableObject));
    onlyInAGlobal = static_cast<ReachableObject*>(miss(Missed::OnlyInGlobal, new ReachableObject));
    myaRoots.wrongType = static_cast<BigObject*>(miss(Missed::WrongType, fastMalloc(wrongTypeObjectSize)));
    holdOnStack(~std::bit_cast<uint64_t>(miss(Missed::OnlyOnStack, new ReachableObject)));
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

    // Last, so that the target allocates nothing after it.
    static NeverDestroyed<Vector<uint64_t>> liveCells;
    liveCells->clear();
    {
        JSC::HeapIterationScope iterationScope(vm.heap);
        vm.heap.objectSpace().forEachLiveCell(iterationScope, [&](JSC::HeapCell* cell, JSC::HeapCell::Kind) {
            liveCells->append(std::bit_cast<uint64_t>(cell));
            return IterationStatus::Continue;
        });
    }
    std::ranges::sort(liveCells.get());
    fixture.liveCells = std::bit_cast<uint64_t>(liveCells->span().data());
    fixture.liveCellCount = liveCells->size();

    // Last before the enumeration, so that nothing reuses them first.
    for (size_t index = 0; index < freedSizes.size(); ++index) {
        void* object = fastMalloc(freedSizes[index]);
        fixture.freed[index] = std::bit_cast<uint64_t>(object);
        fastFree(object);
    }
    EnumeratedAllocations& records = enumerateAllocations();
    fixture.allocations = std::bit_cast<uint64_t>(records.objects);
    fixture.allocationCount = records.objectCount;
    fixture.allocationsCapacity = records.capacity;
    fixture.heapPages = std::bit_cast<uint64_t>(records.pages);
    fixture.heapPageCount = records.pageCount;
    fixture.heapPagesCapacity = records.capacity;
    return Address { &fixture };
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

// The reach walk counts the objects reachable from the roots, and misses those the target leaked.
void checkReach(Snapshot& snapshot, const HeapWalk& heap, const HeapWalkFixture& fixture)
{
    auto enumerated = snapshot.memory().span<HeapWalk::Allocation>(Address { fixture.allocations }, static_cast<size_t>(fixture.allocationCount));
    TEST_ASSERT(enumerated && enumerated.size(), "the allocations libpas enumerated in the target read");
    if (!enumerated)
        return;
    Vector<HeapWalk::Allocation> allocations { std::span<const HeapWalk::Allocation> { enumerated } };
    auto allocationOf = [&](uint64_t address) -> const HeapWalk::Allocation* {
        auto after = std::ranges::upper_bound(allocations, Address { address }, { }, &HeapWalk::Allocation::address);
        if (after == allocations.begin() || address - (after - 1)->address.toTargetVMAddress() >= (after - 1)->size)
            return nullptr;
        return &*(after - 1);
    };
    auto isObject = [&](uint64_t address, size_t size) {
        auto* allocation = allocationOf(address);
        return allocation && allocation->address == Address { address } && allocation->size >= size;
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
    TEST_ASSERT(!allocationOf(fixture.freed[0]) && !allocationOf(fixture.freed[1]), "libpas enumerates no freed object, small or large");
    unsigned cellsOutside = 0;
    heap.forEachLiveCell([&](const HeapWalk::Cell& cell) {
        if (!allocationOf(cell.address.toTargetVMAddress()))
            ++cellsOutside;
        return IterationStatus::Continue;
    });
    TEST_ASSERT_EQ(cellsOutside, 0u, "every live cell is in an enumerated allocation: the JS heap's blocks are libpas objects");

    HeapWalk::Reach reach = heap.reach(allocations, allocations.size());
    auto isMissed = [&](uint64_t address) {
        return reach.largestMissed.containsIf([&](const HeapWalk::Allocation& allocation) {
            return allocation.address == Address { address };
        });
    };
    // Whether the allocation that holds `address` is reached, for an object that may not start one.
    auto isReached = [&](uint64_t address) {
        auto* allocation = allocationOf(address);
        return allocation && !isMissed(allocation->address.toTargetVMAddress());
    };
    TEST_ASSERT(std::ranges::none_of(fixture.reachable, isMissed), "the reach walk reaches the objects reachable from the roots");
    TEST_ASSERT(std::ranges::all_of(fixture.leaked, isMissed), "and misses the objects the target leaked");
    TEST_ASSERT(reach.bytesReached && reach.bytesReached <= reach.bytesAllocated, "the walk reaches part of the heap");

    // Patch 9: read from its home description, the VM leads to what it owns.
    TEST_ASSERT(isReached(fixture.jsonCache) && isReached(fixture.builtinExecutables) && isReached(fixture.regExpCache),
        "the walk reaches what the VM owns through pointers to classes this executable only declares");
    // Without home descriptions, 84 pointers lead to declarations; with them, the planted one and one other.
    TEST_ASSERT(reach.notFollowed[static_cast<size_t>(HeapWalk::NotFollowed::Declaration)] <= 4, "almost no pointer the walk reaches leads to a class that is only declared");
    // Patch 10: a JS cell, read as its class, leads to what it holds. The name is
    // not an atom, so only its JSString holds its StringImpl.
    TEST_ASSERT(isReached(fixture.stringImpl), "the walk reaches the StringImpl of a JSString");
    TEST_ASSERT_EQ(reach.notFollowed[static_cast<size_t>(HeapWalk::NotFollowed::CellWithoutClass)], 0u, "the walk reads every JS cell as its class");
    // Patch 11: each planted object is behind a value the walk reads by its C++ source's logic.
    for (size_t index = 0; index < numberOfPlanted; ++index)
        TEST_ASSERT(fixture.planted[index] && !isMissed(fixture.planted[index]), makeString("the walk reaches the object behind "_s, plantedNames[index]));

    if (verbose) {
        dataLogLn("    the walk reaches ", reach.bytesReached, " of ", reach.bytesAllocated, " bytes in ", allocations.size(), " libpas allocations");
        dataLogLn("    ", reach.cellsWithClass, " JS cells read as their class, with ", reach.cellBytesBeyondClass, " bytes beyond their class");
        constexpr std::array<ASCIILiteral, HeapWalk::numberOfNotFollowedReasons> reasons {
            "pointers to declarations"_s, "pointers to types without a size"_s, "polymorphic pointees without a dynamic type"_s,
            "JS cells without a class"_s, "encoded pointers to types not reached"_s,
        };
        for (size_t index = 0; index < reasons.size(); ++index)
            dataLogLn("    not followed: ", reach.notFollowed[index], " ", reasons[index]);
        for (size_t index = 0; index < std::min<size_t>(reach.largestMissed.size(), 5); ++index)
            dataLogLn("    missed: ", reach.largestMissed[index].size, " bytes at 0x", hex(reach.largestMissed[index].address.toTargetVMAddress()));
    }
}

// Patch 12: the report says why the walk misses each planted object.
void checkAttribution(Snapshot& snapshot, const HeapWalk& heap, const HeapWalkFixture& fixture)
{
    auto readAllocations = [&](uint64_t address, uint64_t count) {
        auto records = snapshot.memory().span<HeapWalk::Allocation>(Address { address }, static_cast<size_t>(count));
        return records ? Vector<HeapWalk::Allocation> { std::span<const HeapWalk::Allocation> { records } } : Vector<HeapWalk::Allocation> { };
    };
    Vector<HeapWalk::Allocation> allocations = readAllocations(fixture.allocations, fixture.allocationCount);
    Vector<HeapWalk::Allocation> heapPages = readAllocations(fixture.heapPages, fixture.heapPageCount);
    TEST_ASSERT(!allocations.isEmpty() && !heapPages.isEmpty(), "the allocations and pages libpas enumerated in the target read");
    if (allocations.isEmpty())
        return;
    // The target's own lists, which point at every allocation and every live cell.
    Vector<HeapWalk::Allocation> excluded {
        { Address { fixture.allocations }, fixture.allocationsCapacity * sizeof(HeapWalk::Allocation) },
        { Address { fixture.heapPages }, fixture.heapPagesCapacity * sizeof(HeapWalk::Allocation) },
        { Address { fixture.liveCells }, fixture.liveCellCount * sizeof(uint64_t) },
    };
    std::ranges::sort(excluded, { }, &HeapWalk::Allocation::address);

    MonotonicTime start = MonotonicTime::now();
    HeapWalk::Attribution attribution = heap.attribute(allocations, excluded, heapPages);
    Seconds duration = MonotonicTime::now() - start;
    TEST_ASSERT(attribution.reach.bytesReached && attribution.missed.size(), "the attribution walks the heap and finds what it misses");

    unsigned factsKnown = 0;
    for (const HeapWalk::Allocation& allocation : allocations)
        factsKnown += !!allocation.facts.heap;
    TEST_ASSERT(factsKnown > allocations.size() / 2, "libpas gives the heap of most allocations");

    auto missedAt = [&](Missed missed) -> const HeapWalk::MissedAllocation* {
        Address address { ~fixture.missed[static_cast<size_t>(missed)] };
        for (const HeapWalk::MissedAllocation& allocation : attribution.missed) {
            if (allocation.allocation.address == address)
                return &allocation;
        }
        return nullptr;
    };
    auto isAttributedTo = [&](Missed missed, HeapWalk::EdgeReason reason, std::string_view owner) {
        auto* allocation = missedAt(missed);
        if (!allocation || !allocation->attribution) {
            dataLogLn("    planted object ", static_cast<unsigned>(missed), allocation ? " is missed, with no attribution" : " is not missed");
            return false;
        }
        const HeapWalk::Edge& edge = *allocation->attribution;
        bool matches = edge.reason == reason && edge.owner.contains(String::fromUTF8(std::span { owner.data(), owner.size() }));
        if (!matches)
            dataLogLn("    planted object ", static_cast<unsigned>(missed), " is attributed to ", HeapWalk::description(edge.reason), " in ", edge.owner);
        return matches;
    };
    using Reason = HeapWalk::EdgeReason;
    TEST_ASSERT(isAttributedTo(Missed::InInteger, Reason::Integer, "MyaRoots::integer"), "an object held in an integer is attributed to that field");
    TEST_ASSERT(isAttributedTo(Missed::ThroughMissed, Reason::Integer, "MyaRoots::integer") && missedAt(Missed::ThroughMissed)->isAttributedThroughMissed,
        "an object held only by a missed object is attributed through it");
    TEST_ASSERT(isAttributedTo(Missed::BehindVoidPointer, Reason::VoidPointer, "MyaRoots::opaque"), "an object held in a void* is attributed to that field");
    TEST_ASSERT(isAttributedTo(Missed::BehindDeclaration, Reason::Declaration, "MyaRoots::declared"), "an object held in a pointer to a declaration is attributed to that field");
    TEST_ASSERT(isAttributedTo(Missed::OnlyOnStack, Reason::Stack, "myaStackHolder"), "an object held only on a stack is attributed to its thread");
    TEST_ASSERT(isAttributedTo(Missed::OnlyInGlobal, Reason::ImageData, "onlyInAGlobal"), "an object held only in a global is attributed to its symbol");
    TEST_ASSERT(isAttributedTo(Missed::WrongType, Reason::PointeeDoesNotFit, "MyaRoots::wrongType"), "an object held in a pointer to a bigger class is attributed to that field");
    auto* wrongType = missedAt(Missed::WrongType);
    TEST_ASSERT(wrongType && wrongType->allocation.size < sizeof(BigObject), "libpas says the object is smaller than the class of the pointer to it");
    TEST_ASSERT(attribution.failedChecks.containsIf([](const String& check) { return check.contains("MyaRoots::wrongType"_s); }),
        "and the check against libpas's facts reports the pointer");
    TEST_ASSERT(std::ranges::all_of(fixture.leaked, [&](uint64_t leaked) {
        return attribution.missed.containsIf([&](const HeapWalk::MissedAllocation& missed) { return missed.allocation.address == Address { leaked } && missed.attribution && missed.attribution->reason == Reason::ImageData; });
    }), "the objects the target holds only in its fixture are attributed to it");

    uint64_t groupedBytes = attribution.bytesWithoutEdge;
    for (const HeapWalk::Group& group : attribution.groups)
        groupedBytes += group.bytes;
    uint64_t missedBytes = 0;
    for (const HeapWalk::MissedAllocation& missed : attribution.missed)
        missedBytes += missed.allocation.size;
    TEST_ASSERT_EQ(missedBytes, attribution.reach.bytesAllocated - attribution.reach.bytesReached, "the missed allocations are what the walk does not reach");
    TEST_ASSERT(groupedBytes <= missedBytes, "no missed byte is counted twice");

    if (!verbose)
        return;
    uint64_t attributedBytes = groupedBytes - attribution.bytesWithoutEdge;
    dataLogLn("    attribution of ", missedBytes, " missed bytes in ", attribution.missed.size(), " allocations, in ", duration.milliseconds(), " ms: ",
        attributedBytes, " attributed, ", attribution.bytesWithoutEdge, " with nothing pointing to them, ", missedBytes - groupedBytes, " only in cycles of missed allocations");
    for (size_t index = 0; index < std::min<size_t>(attribution.groups.size(), 25); ++index) {
        const HeapWalk::Group& group = attribution.groups[index];
        dataLogLn("      ", group.bytes, " bytes in ", group.count, " allocations: ", HeapWalk::description(group.reason), " in ", group.owner);
    }
    dataLogLn("    missed bytes by libpas heap type:");
    for (size_t index = 0; index < std::min<size_t>(attribution.byHeapType.size(), 6); ++index)
        dataLogLn("      ", attribution.byHeapType[index].bytes, " bytes in ", attribution.byHeapType[index].count, " allocations: ", attribution.byHeapType[index].owner);
    dataLogLn("    missed allocations nothing points to, by size:");
    for (size_t index = 0; index < std::min<size_t>(attribution.withoutEdgeBySize.size(), 6); ++index)
        dataLogLn("      ", attribution.withoutEdgeBySize[index].second, " of ", attribution.withoutEdgeBySize[index].first, " bytes");
    for (const String& check : attribution.failedChecks)
        dataLogLn("    failed check: ", check);
}

// The proper field `name` of a class, if it has one.
const TargetType::Field* properField(const TargetType& type, std::string_view name)
{
    auto* klass = std::get_if<TargetType::Class>(&type.layout());
    if (!klass)
        return nullptr;
    for (const TargetType::Field& field : klass->properFields) {
        if (std::string_view { field.name.legacyCStringPointer() } == name)
            return &field;
    }
    return nullptr;
}

// The pointee of the first pointer in `type`, depth first through its fields and bases.
const TargetType* firstPointee(const TargetType& type, unsigned depth = 0)
{
    if (auto* pointer = std::get_if<TargetType::Pointer>(&type.layout()))
        return &pointer->pointee;
    auto* klass = std::get_if<TargetType::Class>(&type.layout());
    if (!klass || depth > 8)
        return nullptr;
    for (const TargetType::Field& field : klass->properFields) {
        if (auto* pointee = firstPointee(field.type, depth + 1))
            return pointee;
    }
    for (const TargetType::Base& base : klass->bases) {
        if (auto* pointee = firstPointee(base.type, depth + 1))
            return pointee;
    }
    return nullptr;
}

// Patch 9: this executable only declares many of the classes the VM owns, and
// JavaScriptCore, which defines VM::~VM, defines them all.
void checkHomes(Snapshot& snapshot, const HeapWalk& heap, const HeapWalkFixture& fixture)
{
    const TargetType& vm = *heap.vm().type();
    const TargetType& home = vm.home();
    TEST_ASSERT(&home != &vm && home.byteSize() == vm.byteSize(), "the VM's home description is another description of the same size");
    TEST_ASSERT(&home.home() == &home, "a home description is its own home");
    auto* field = properField(vm, "m_jsonCache");
    auto* homeField = properField(home, "m_jsonCache");
    const TargetType* declared = field ? firstPointee(field->type) : nullptr;
    const TargetType* defined = homeField ? firstPointee(homeField->type) : nullptr;
    TEST_ASSERT(declared && !declared->byteSize(), "this executable only declares the class of the VM's JSONCache");
    TEST_ASSERT(defined && defined->byteSize(), "the VM's home description defines it");

    // A pointer to a polymorphic class that is only declared is read as its dynamic type.
    const TargetValue& roots = *heap.roots();
    TargetValue regExpCache = roots.properField("regExpCache");
    auto* pointer = regExpCache ? std::get_if<TargetType::Pointer>(&regExpCache.type().layout()) : nullptr;
    TEST_ASSERT(pointer && !pointer->pointee.byteSize(), "the roots point to a RegExpCache, which this executable only declares");
    auto address = regExpCache.pointerValue();
    Address completeObject;
    const TargetType* dynamicType = address ? roots.type().debugInfo().dynamicTypeIfAnyAt(snapshot, *address, completeObject) : nullptr;
    TEST_ASSERT(dynamicType && std::string_view { dynamicType->name().legacyCStringPointer() } == "JSC::RegExpCache" && dynamicType->byteSize(),
        "its vtable gives it its class, which has a size");
    TEST_ASSERT(dynamicType && completeObject == Address { fixture.regExpCache }, "and its complete object is the VM's RegExpCache");
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

    LiveCells cells(snapshot, heap);
    cells.collect();
    TEST_ASSERT(cells.count() > 100, "a VM with a global object has hundreds of live cells");
    cells.checkHeaders();
    checkFixture(snapshot, heap, cells, *fixture);
    checkAgainstJSC(snapshot, cells, *fixture);
    checkHomes(snapshot, heap, *fixture);
    checkCellClasses(heap, cells, *fixture);
    checkReach(snapshot, heap, *fixture);
    checkAttribution(snapshot, heap, *fixture);
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

void collectFixture(Address fixtureAddress, Function<void()>&& during)
{
    auto* fixture = std::bit_cast<HeapWalkFixture*>(static_cast<uintptr_t>(fixtureAddress.toTargetVMAddress()));
    JSC::VM& vm = *std::bit_cast<JSC::VM*>(static_cast<uintptr_t>(fixture->vm));
    JSC::JSLockHolder locker(vm);
    DuringCollection observer(WTF::move(during));
    vm.heap.addObserver(&observer);
    vm.heap.collectSync(JSC::CollectionScope::Full);
    vm.heap.removeObserver(&observer);
}

// For the target process: reports the fixture from inside a collection, and stays there.
Address createFixtureInCollection()
{
    Address fixture = createFixture();
    collectFixture(fixture, [fixture] {
        reportTargetObjectAndPark(fixture);
    });
    RELEASE_ASSERT_NOT_REACHED();
}

// The block of the fixture's object, whose lock an allocation, a sweep or stopAllocating holds.
WTF::CountingLock& fixtureBlockLock(Address fixtureAddress)
{
    auto* fixture = std::bit_cast<HeapWalkFixture*>(static_cast<uintptr_t>(fixtureAddress.toTargetVMAddress()));
    return JSC::MarkedBlock::blockFor(std::bit_cast<void*>(static_cast<uintptr_t>(fixture->object)))->lock();
}

// For the target process: reports the fixture while a block is locked, and keeps it locked.
Address createFixtureWithLockedBlock()
{
    Address fixture = createFixture();
    fixtureBlockLock(fixture).lock();
    reportTargetObjectAndPark(fixture);
}

void analyzeAwayFromSafePoint(Snapshot& snapshot, Address fixtureAddress)
{
    auto fixture = snapshot.memory().ptr<HeapWalkFixture>(fixtureAddress);
    TEST_ASSERT(fixture, "the target's fixture reads");
    if (!fixture)
        return;
    HeapWalk heap(snapshot, Address { fixture->roots });
    TEST_ASSERT(heap.isValid(), "the heap of a VM away from a safe point is found from its roots");
    ExpectedErrors expectedErrors;
    unsigned visited = 0;
    heap.forEachLiveCell([&](const HeapWalk::Cell&) {
        ++visited;
        return IterationStatus::Continue;
    });
    TEST_ASSERT_EQ(visited, 0u, "a snapshot away from a safe point is reported and not walked");
}

} // anonymous namespace

void testHeapWalk()
{
    SuiteTracer tracer("HeapWalk");
    if (!tracer.shouldRun())
        return;

    analyzeInAndOutOfProcess(createFixture, analyze);
    analyzeAfterTargetExits(createFixture, analyze);

    // In this process, the analysis runs away from the safe point; out of it, the target parks there.
    auto analyzeSelf = [](Address fixture) {
        SelfSnapshot self;
        if (self.isValid())
            analyzeAwayFromSafePoint(self.snapshot(), fixture);
    };
    Address fixture = createFixture();
    collectFixture(fixture, [&] {
        analyzeSelf(fixture);
    });
    analyzeInSeparateProcess(createFixtureInCollection, analyzeAwayFromSafePoint);

    fixture = createFixture();
    {
        Locker locker { fixtureBlockLock(fixture) };
        analyzeSelf(fixture);
    }
    analyzeInSeparateProcess(createFixtureWithLockedBlock, analyzeAwayFromSafePoint);
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
