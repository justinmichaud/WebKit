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
#include "HeapWalkTest.h"

#include "LibJSCToolsTestUtilities.h"

#if ENABLE(MYA_HEAP) && OS(DARWIN)

#include <JavaScriptCore/CorpseAddress.h>
#include <JavaScriptCore/CorpseHeapWalk.h>
#include <JavaScriptCore/CorpseProcess.h>
#include <JavaScriptCore/CorpseRemote.h>
#include <JavaScriptCore/CorpseSnapshot.h>
#include <JavaScriptCore/DateInstance.h>
#include <JavaScriptCore/HeapCell.h>
#include <JavaScriptCore/JSCJSValueInlines.h>
#include <JavaScriptCore/JSObject.h>
#include <JavaScriptCore/JSType.h>
#include <JavaScriptCore/StructureID.h>
#include <array>
#include <bit>
#include <errno.h>
#include <initializer_list>
#include <optional>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <stdint.h>
#include <string>
#include <string_view>
#include <sys/wait.h>
#include <unistd.h>
#include <wtf/ASCIICType.h>
#include <wtf/HashMap.h>
#include <wtf/HexNumber.h>
#include <wtf/IterationStatus.h>
#include <wtf/MonotonicTime.h>
#include <wtf/RefPtr.h>
#include <wtf/SafeStrerror.h>
#include <wtf/Seconds.h>
#include <wtf/StdLibExtras.h>
#include <wtf/Vector.h>
#include <wtf/text/CString.h>
#include <wtf/text/MakeString.h>
#include <wtf/text/StringToIntegerConversion.h>
#include <wtf/text/StringView.h>

extern char** environ;

namespace JSCToolsTest {

namespace {

using JSC::Corpse::Address;
using JSC::Corpse::HeapWalk;
using JSC::Corpse::Process;
using JSC::Corpse::Remote;
using JSC::Corpse::Snapshot;

// A JS value the walk has to find: an int32 in the fixture's inline storage.
constexpr int32_t jsMagic = 0x5eed1234;

// A C++ value the walk has to find: the double inside a DateInstance.
constexpr double dateMagic = 1234567890123;

constexpr Seconds targetReportTimeout = 60_s;

// A cell bigger than this is counted but not searched for the magic values.
constexpr size_t maximumScannedCellSize = 1 << 20;

// What the jsc target runs. The fixture is reachable from the program's scope,
// so it survives the full collection. After that collection the target
// allocates no more cells: print() writes a string that already exists, and
// readline() blocks in getc() until this test kills the target. The lines
// print() writes are the describe() output naming the cells the walk must find.
CString targetScript()
{
    return makeString(
        "const fixture = {\n"
        "    magic: "_s, jsMagic, ",\n"
        "    date: new Date("_s, static_cast<int64_t>(dateMagic), "),\n"
        "    name: \"corpse-heap-walk\",\n"
        "    children: [],\n"
        "};\n"
        "for (let i = 0; i < 4; ++i)\n"
        "    fixture.children.push({ index: i, parent: fixture, label: \"child-\" + i });\n"
        "fixture.children[0].sibling = fixture.children[1];\n"
        "const report = describe(fixture) + \"\\n\" + describe(fixture.date);\n"
        "fullGC();\n"
        "print(report);\n"
        "readline();\n"_s).utf8();
}

// A jsc process running targetScript(), killed on destruction.
class JSCTarget {
public:
    JSCTarget();
    ~JSCTarget();

    JSCTarget(const JSCTarget&) = delete;
    JSCTarget& operator=(const JSCTarget&) = delete;

    pid_t pid() const { return m_pid; }

    // The first `count` lines the target prints, or nullopt if it does not
    // print them in time.
    std::optional<Vector<std::string>> readLines(unsigned count);

private:
    pid_t m_pid { 0 };
    int m_stdoutRead { -1 };
    int m_stdinWrite { -1 }; // Held open so the target's readline() never sees EOF.
};

JSCTarget::JSCTarget()
{
    CString executablePath = Process::create(getpid())->executablePath();
    TEST_ASSERT(!executablePath.isNull(), "this process's executable path is readable");
    if (executablePath.isNull())
        return;
    std::string_view executable { executablePath.span() };
    std::string jscPath { executable.substr(0, executable.rfind('/') + 1) };
    jscPath += "jsc";

    std::array<int, 2> stdoutPipe { -1, -1 };
    std::array<int, 2> stdinPipe { -1, -1 };
    if (pipe(stdoutPipe.data())) {
        TEST_ASSERT(false, "a pipe from the target opens");
        return;
    }
    if (pipe(stdinPipe.data())) {
        TEST_ASSERT(false, "a pipe to the target opens");
        close(stdoutPipe[0]);
        close(stdoutPipe[1]);
        return;
    }

    CString script = targetScript();
    char* const arguments[] = {
        const_cast<char*>(jscPath.c_str()),
        const_cast<char*>("-e"),
        const_cast<char*>(script.data()),
        nullptr
    };
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, stdoutPipe[1], STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&actions, stdinPipe[0], STDIN_FILENO);
    for (int fd : { stdoutPipe[0], stdoutPipe[1], stdinPipe[0], stdinPipe[1] })
        posix_spawn_file_actions_addclose(&actions, fd);
    pid_t child = 0;
    int error = posix_spawn(&child, jscPath.c_str(), &actions, nullptr, arguments, environ);
    posix_spawn_file_actions_destroy(&actions);
    close(stdoutPipe[1]);
    close(stdinPipe[0]);
    TEST_ASSERT(!error, "the jsc target launches");
    if (error) {
        dataLogLn("    posix_spawn ", jscPath.c_str(), ": ", safeStrerror(error));
        close(stdoutPipe[0]);
        close(stdinPipe[1]);
        return;
    }
    m_pid = child;
    m_stdoutRead = stdoutPipe[0];
    m_stdinWrite = stdinPipe[1];
}

JSCTarget::~JSCTarget()
{
    if (m_pid > 0) {
        kill(m_pid, SIGKILL);
        while (waitpid(m_pid, nullptr, 0) < 0 && errno == EINTR) { }
    }
    if (m_stdoutRead >= 0)
        close(m_stdoutRead);
    if (m_stdinWrite >= 0)
        close(m_stdinWrite);
}

std::optional<Vector<std::string>> JSCTarget::readLines(unsigned count)
{
    if (m_pid <= 0)
        return std::nullopt;
    std::string output;
    MonotonicTime deadline = MonotonicTime::now() + targetReportTimeout;
    while (true) {
        Vector<std::string> lines;
        size_t start = 0;
        for (size_t newline = output.find('\n'); newline != std::string::npos && lines.size() < count; newline = output.find('\n', start)) {
            lines.append(output.substr(start, newline - start));
            start = newline + 1;
        }
        if (lines.size() == count)
            return lines;

        Seconds remaining = deadline - MonotonicTime::now();
        if (remaining <= 0_s)
            return std::nullopt;
        pollfd descriptor { m_stdoutRead, POLLIN, 0 };
        int ready = poll(&descriptor, 1, static_cast<int>(remaining.milliseconds()));
        if (ready < 0 && errno == EINTR)
            continue;
        if (ready <= 0)
            return std::nullopt;
        std::array<char, 4096> buffer;
        ssize_t bytesRead = read(m_stdoutRead, buffer.data(), buffer.size());
        if (bytesRead <= 0)
            return std::nullopt;
        output.append(buffer.data(), static_cast<size_t>(bytesRead));
    }
}

// The number that follows `prefix` in `line`, in `base`, or nullopt if there
// is none.
std::optional<uint64_t> numberAfter(std::string_view line, std::string_view prefix, uint8_t base)
{
    size_t start = line.find(prefix);
    if (start == std::string_view::npos)
        return std::nullopt;
    std::string_view rest = line.substr(start + prefix.size());
    if (base == 16 && rest.starts_with("0x"))
        rest = rest.substr(2);
    size_t length = 0;
    while (length < rest.size() && (base == 16 ? isASCIIHexDigit(rest[length]) : isASCIIDigit(rest[length])))
        ++length;
    std::string digits { rest.substr(0, length) };
    return WTF::parseInteger<uint64_t>(StringView::fromLatin1(digits.c_str()), base);
}

// What describe() prints for an object: "Object: 0x... with butterfly 0x...
// (Structure 0x...:[...]), StructureID: N".
struct DescribedObject {
    Address cell;
    Address structure;
    uint32_t structureID { 0 };
};

std::optional<DescribedObject> parseDescribedObject(const std::string& line)
{
    auto cell = numberAfter(line, "Object: ", 16);
    auto structure = numberAfter(line, "(Structure ", 16);
    auto structureID = numberAfter(line, "StructureID: ", 10);
    if (!cell || !structure || !structureID || *structureID > UINT32_MAX) {
        dataLogLn("    unparsable describe() output: ", line.c_str());
        return std::nullopt;
    }
    return DescribedObject { Address { *cell }, Address { *structure }, static_cast<uint32_t>(*structureID) };
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
    Vector<uint8_t> bytes(static_cast<size_t>(cell.size));
    if (m_snapshot.read(cell.address, bytes.mutableSpan()).empty()) {
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

void checkFixture(Snapshot& snapshot, const HeapWalk& heap, const LiveCells& cells, const DescribedObject& fixture, const DescribedObject& date)
{
    const LiveCell* fixtureCell = cells.cell(fixture.cell);
    TEST_ASSERT(fixtureCell && fixtureCell->type == JSC::FinalObjectType, "the walk finds the fixture, a final object");
    TEST_ASSERT(cells.cellsWithJSMagic().size() == 1 && cells.cellsWithJSMagic()[0] == fixture.cell, "the fixture is the one cell holding the JS magic");
    TEST_ASSERT(fixtureCell && fixtureCell->structureID == fixture.structureID, "the fixture's header holds the StructureID describe() printed");

    Remote<JSC::Structure> structure = heap.structure(fixture.structureID);
    TEST_ASSERT(structure.address() == fixture.structure, "the StructureID decodes to the Structure describe() printed");
    const LiveCell* structureCell = cells.cell(fixture.structure);
    TEST_ASSERT(structureCell && structureCell->type == JSC::StructureType, "the walk finds the fixture's Structure");
    auto inlineCapacity = structure.field<uint8_t>("m_inlineCapacity").integer();
    TEST_ASSERT(inlineCapacity && *inlineCapacity >= 2, "the fixture's Structure has room for the two slots read below");

    // The debug info attaches a type to a Structure's prototype slot, which is
    // what every JSValue slot looks like. The target is this build's jsc, so
    // this build's inline storage offset is the target's.
    Remote<JSC::JSValue> magicSlot = structure.field<void>("m_prototype").base<void>(0).field<JSC::JSValue>("m_value").at(fixture.cell + JSC::JSObject::offsetOfInlineStorage());
    auto magic = magicSlot.as<uint64_t>();
    TEST_ASSERT(magic && *magic == static_cast<uint64_t>(JSC::JSValue::encode(JSC::jsNumber(jsMagic))), "the fixture's first inline slot holds the JS magic");
    CellCollector magicCells;
    visitChildren(magicSlot, magicCells);
    TEST_ASSERT(magicCells.cells.isEmpty(), "an int32 holds no cell");
    CellCollector dateCells;
    visitChildren(magicSlot.offsetBy(1), dateCells);
    TEST_ASSERT(dateCells.cells.size() == 1 && dateCells.cells[0] == date.cell, "the fixture's second inline slot holds the date");

    const LiveCell* dateCell = cells.cell(date.cell);
    TEST_ASSERT(dateCell && dateCell->type == JSC::JSDateType, "the walk finds the date");
    TEST_ASSERT(cells.cellsWithDateMagic().size() == 1 && cells.cellsWithDateMagic()[0] == date.cell, "the date is the one cell holding the C++ magic");
    auto internalNumber = Remote<double>(snapshot, date.cell + JSC::DateInstance::offsetOfInternalNumber()).as<double>();
    TEST_ASSERT(internalNumber && *internalNumber == dateMagic, "the date's C++ double reads back");
}

void analyze(Snapshot& snapshot, const DescribedObject& fixture, const DescribedObject& date)
{
    HeapWalk heap(snapshot);
    TEST_ASSERT(heap.isValid(), "the target's heap is found");
    if (!heap.isValid())
        return;

    // The export trie and the debug info agree on where the VM is.
    auto exportedVM = snapshot.read<uint64_t>(snapshot.symbol("_ZN3JSC9VMManager10s_recentVME"));
    TEST_ASSERT(exportedVM && Address { *exportedVM }.stripped() == heap.vm().address(), "the walk starts from the VM that s_recentVM exports");

    LiveCells cells(snapshot, heap);
    cells.collect();
    TEST_ASSERT(cells.count() > 100, "a jsc shell has hundreds of live cells");
    cells.checkHeaders();
    checkFixture(snapshot, heap, cells, fixture, date);
}

} // anonymous namespace

void testHeapWalk()
{
    SuiteTracer tracer("HeapWalk");
    if (!tracer.shouldRun())
        return;

    JSCTarget target;
    auto lines = target.readLines(2);
    TEST_ASSERT(lines, "the jsc target prints its report");
    if (!lines)
        return;
    auto fixture = parseDescribedObject((*lines)[0]);
    auto date = parseDescribedObject((*lines)[1]);
    TEST_ASSERT(fixture && date, "the report describes the fixture and its date");
    if (!fixture || !date)
        return;

    RefPtr<Process> process = Process::create(target.pid());
    bool attached = process->attach();
    TEST_ASSERT(attached, "attaching to the jsc target succeeds");
    if (!attached)
        return;
    Snapshot snapshot(process);
    TEST_ASSERT(snapshot.isValid(), "a snapshot of the jsc target is valid");
    if (!snapshot.isValid())
        return;
    analyze(snapshot, *fixture, *date);
}

} // namespace JSCToolsTest

#else // ENABLE(MYA_HEAP) && OS(DARWIN)

namespace JSCToolsTest {

void testHeapWalk()
{
    skipSuite("HeapWalk", "corpses of another process are taken on Darwin only");
}

} // namespace JSCToolsTest

#endif // ENABLE(MYA_HEAP) && OS(DARWIN)
