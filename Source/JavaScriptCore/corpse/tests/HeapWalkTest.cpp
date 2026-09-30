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
#include <algorithm>
#include <bit>
#include <optional>
#include <stdint.h>
#include <wtf/HashMap.h>
#include <wtf/HexNumber.h>
#include <wtf/IterationStatus.h>
#include <wtf/NeverDestroyed.h>
#include <wtf/StdLibExtras.h>
#include <wtf/Vector.h>
#include <wtf/text/ASCIILiteral.h>

namespace JSCToolsTest {

namespace {

using JSC::Corpse::Address;
using JSC::Corpse::HeapWalk;
using JSC::Corpse::Remote;
using JSC::Corpse::Snapshot;

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
    "    name: \"corpse-heap-walk\",\n"
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
};

// Makes a VM, evaluates targetScript in it, collects, then evaluates
// lateScript, so that the live heap has cells from both sides of its last
// collection. The VM and its global object are kept for the life of the process.
Address createFixture()
{
    static HeapWalkFixture fixture;
    static LazyNeverDestroyed<MyaRoots> roots;
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

    roots.construct(vm);
    fixture.roots = std::bit_cast<uint64_t>(&roots.get());
    fixture.vm = std::bit_cast<uint64_t>(&vm);
    fixture.object = std::bit_cast<uint64_t>(object);
    fixture.structure = std::bit_cast<uint64_t>(object->structure());
    fixture.structureID = object->structureID().bits();
    fixture.date = std::bit_cast<uint64_t>(date.asCell());

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
}

} // anonymous namespace

void testHeapWalk()
{
    SuiteTracer tracer("HeapWalk");
    if (!tracer.shouldRun())
        return;

    analyzeInAndOutOfProcess(createFixture, analyze);
    analyzeAfterTargetExits(createFixture, analyze);
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
