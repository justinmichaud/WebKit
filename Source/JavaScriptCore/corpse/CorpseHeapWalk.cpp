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
#include "CorpseHeapWalk.h"

#if ENABLE(MYA)

#include "CorpseError.h"
#include "CorpseLimits.h"
#include <JavaScriptCore/MarkedBlock.h>
#include <JavaScriptCore/StructureID.h>
#include <wtf/StdLibExtras.h>
#include <wtf/Vector.h>

namespace JSC {

class Heap;
class PreciseAllocation;

namespace Corpse {

static unsigned long long forReport(Address address)
{
    return address.toTargetVMAddress();
}

HeapWalk::HeapWalk(Snapshot& snapshot)
    : m_snapshot(snapshot)
{
    m_debugInfo = SnapshotDebugInfo::create(snapshot);
    if (!m_debugInfo)
        return;
    auto recentVM = m_debugInfo->findVariable("JSC::VMManager::s_recentVM", snapshot);
    if (!recentVM)
        return;
    m_vm = Remote<VM*>(WTF::move(*recentVM)).dereference();
    if (!m_vm) {
        CORPSE_REPORT("The target has entered no VM");
        return;
    }

    // The Structure of every Structure is the VM's structureStructure, so its
    // StructureID names itself: its address less its ID is where the
    // structure heap starts.
    Remote<Structure> structureStructure = m_vm.field<void>("structureStructure").base<void>(0).field<Structure*>("m_cell").dereference();
    Remote<JSCell> cell = structureStructure.base<JSCell>(0);
    auto bits = cell.field<void>("m_structureID").field<uint32_t>("m_bits").integer();
    if (!bits) {
        m_vm = { };
        return;
    }
    if (*bits <= 0 || static_cast<uint64_t>(*bits) > structureStructure.address().toTargetVMAddress()) {
        CORPSE_REPORT("The structureStructure at 0x%llx has StructureID %lld", forReport(structureStructure.address()), static_cast<long long>(*bits));
        m_vm = { };
        return;
    }
    m_structure = *structureStructure.typed();
    m_jsCell = *cell.typed();
    m_startOfStructureHeap = structureStructure.address().toTargetVMAddress() - static_cast<uint64_t>(*bits);

    auto roots = m_debugInfo->findVariable("JSC::g_myaRoots", snapshot);
    if (!roots) {
        m_vm = { };
        return;
    }
    TargetValue blockHeaderPointer = roots->properField("markedBlockHeader");
    if (!blockHeaderPointer) {
        m_vm = { };
        return;
    }
    m_blockHeaderPointer = WTF::move(blockHeaderPointer);
}

void HeapWalk::forEachLiveCell(const Function<IterationStatus(const Cell&)>& functor) const
{
    if (!isValid())
        return;
    Remote<MarkedSpace> space = m_vm.field<Heap>("heap").field<MarkedSpace>("m_objectSpace");
    auto markingVersion = space.field<uint32_t>("m_markingVersion").integer();
    if (!markingVersion)
        return;

    Remote<BlockDirectory> directory = space.field<void>("m_directories").field<BlockDirectory*>("m_first").dereference();
    for (unsigned count = 0; directory; ++count) {
        if (count >= maxBlockDirectories) {
            CORPSE_REPORT("The MarkedSpace at 0x%llx lists more than %u BlockDirectories", forReport(space.address()), maxBlockDirectories);
            return;
        }
        if (walkDirectory(directory, static_cast<uint32_t>(*markingVersion), functor) == IterationStatus::Done)
            return;
        directory = directory.field<BlockDirectory*>("m_nextDirectory").dereference();
    }
    walkPreciseAllocations(space, functor);
}

IterationStatus HeapWalk::walkDirectory(const Remote<BlockDirectory>& directory, uint32_t markingVersion, const Function<IterationStatus(const Cell&)>& functor) const
{
    // Each entry pairs a block's handle with the block; a freed block leaves
    // a null entry behind.
    struct Entry;
    using Entries = Vector<Entry>;
    Remote<Entries> entries = directory.field<Entries>("m_blocks");
    auto size = RemoteTraits<Entries>::size(entries);
    if (!size)
        return IterationStatus::Continue;
    for (size_t index = 0; index < *size; ++index) {
        Remote<MarkedBlock::Handle> handle = RemoteTraits<Entries>::element(entries, index).field<MarkedBlock::Handle*>("first").dereference();
        if (!handle)
            continue;
        if (walkBlock(*handle.typed(), markingVersion, functor) == IterationStatus::Done)
            return IterationStatus::Done;
    }
    return IterationStatus::Continue;
}

IterationStatus HeapWalk::walkBlock(const TargetValue& handleValue, uint32_t markingVersion, const Function<IterationStatus(const Cell&)>& functor) const
{
    Remote<MarkedBlock::Handle> handle(TargetValue { handleValue });
    auto atomsPerCell = handle.field<unsigned>("m_atomsPerCell").integer();
    auto startAtom = handle.field<unsigned>("m_startAtom").integer();
    auto kind = handle.field<void>("m_attributes").field<HeapCell::Kind>("cellKind").integer();
    // The header is laid over the start of the block.
    Address block = handle.field<MarkedBlock*>("m_block").dereference().address();
    Remote<MarkedBlock::Header> header = Remote<MarkedBlock::Header*>(TargetValue { *m_blockHeaderPointer }).pointeeAt(block + MarkedBlock::offsetOfHeader);
    auto blockMarkingVersion = header.field<uint32_t>("m_markingVersion").integer();
    Remote<void> marks = header.field<void>("m_marks").field<void>("bits");
    if (!atomsPerCell || !startAtom || !kind || !blockMarkingVersion || !marks)
        return IterationStatus::Continue;
    // Marking touches only the blocks it finds something live in, so a block
    // still on an older version holds nothing live.
    if (static_cast<uint32_t>(*blockMarkingVersion) != markingVersion)
        return IterationStatus::Continue;

    // The block's geometry follows from its header: one mark bit per atom, in
    // little-endian words, and cells from the first atom past the header.
    Vector<uint8_t> markBytes(marks.type()->byteSize());
    if (m_snapshot.read(marks.address(), markBytes.mutableSpan()).empty()) {
        CORPSE_REPORT("Could not read the marks of the block at 0x%llx", forReport(block));
        return IterationStatus::Continue;
    }
    size_t atoms = markBytes.size() * 8;
    size_t firstAtom = roundUpToMultipleOf(MarkedBlock::atomSize, header.type()->byteSize()) / MarkedBlock::atomSize;
    bool plausible = *atomsPerCell > 0 && static_cast<size_t>(*atomsPerCell) <= atoms
        && *startAtom >= 0 && static_cast<size_t>(*startAtom) >= firstAtom && static_cast<size_t>(*startAtom) < atoms
        && !(block.toTargetVMAddress() & (atoms * MarkedBlock::atomSize - 1));
    if (!plausible) {
        CORPSE_REPORT("The block handle at 0x%llx describes no block", forReport(handle.address()));
        return IterationStatus::Continue;
    }

    size_t cellAtoms = static_cast<size_t>(*atomsPerCell);
    for (size_t atom = static_cast<size_t>(*startAtom); atom + cellAtoms <= atoms; atom += cellAtoms) {
        if (!((markBytes[atom / 8] >> (atom % 8)) & 1))
            continue;
        Cell cell { block + atom * MarkedBlock::atomSize, cellAtoms * MarkedBlock::atomSize, static_cast<HeapCell::Kind>(*kind) };
        if (functor(cell) == IterationStatus::Done)
            return IterationStatus::Done;
    }
    return IterationStatus::Continue;
}

IterationStatus HeapWalk::walkPreciseAllocations(const Remote<MarkedSpace>& space, const Function<IterationStatus(const Cell&)>& functor) const
{
    using Allocations = Vector<PreciseAllocation*>;
    Remote<Allocations> allocations = space.field<Allocations>("m_preciseAllocations");
    auto size = RemoteTraits<Allocations>::size(allocations);
    if (!size)
        return IterationStatus::Continue;
    for (size_t index = 0; index < *size; ++index) {
        Remote<PreciseAllocation> allocation = RemoteTraits<Allocations>::element(allocations, index).dereference();
        auto isMarked = allocation.field<void>("m_isMarked").as<bool>();
        auto cellSize = allocation.field<void>("m_cellSize").integer();
        auto kind = allocation.field<void>("m_attributes").field<HeapCell::Kind>("cellKind").integer();
        if (!isMarked || !cellSize || !kind)
            return IterationStatus::Continue;
        // A dead precise allocation is freed as its collection ends, so an
        // unmarked one is younger than the last collection.
        if (!*isMarked)
            continue;
        // The cell follows the header, atom aligned.
        size_t headerSize = roundUpToMultipleOf(MarkedBlock::atomSize, allocation.type()->byteSize());
        Cell cell { allocation.address() + headerSize, static_cast<uint64_t>(*cellSize), static_cast<HeapCell::Kind>(*kind) };
        if (functor(cell) == IterationStatus::Done)
            return IterationStatus::Done;
    }
    return IterationStatus::Continue;
}

Remote<JSCell> HeapWalk::jsCell(Address address) const
{
    if (!m_jsCell)
        return { };
    return Remote<JSCell>(m_jsCell->at(address));
}

Remote<Structure> HeapWalk::structure(uint32_t structureIDBits) const
{
    uint32_t bits = structureIDBits & ~StructureID::nukedStructureIDBit;
    if (!m_structure || !bits)
        return { };
    return Remote<Structure>(m_structure->at(Address { m_startOfStructureHeap + bits }));
}

} // namespace Corpse
} // namespace JSC

#endif // ENABLE(MYA)
