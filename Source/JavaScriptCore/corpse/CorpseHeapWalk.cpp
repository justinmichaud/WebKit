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
#include <JavaScriptCore/BlockDirectoryBits.h>
#include <JavaScriptCore/CollectionScope.h>
#include <JavaScriptCore/FreeList.h>
#include <JavaScriptCore/MarkedBlock.h>
#include <JavaScriptCore/MarkedSpace.h>
#include <JavaScriptCore/PreciseAllocation.h>
#include <JavaScriptCore/StructureID.h>
#include <bit>
#include <span>
#include <wtf/BitSet.h>
#include <wtf/HashMap.h>
#include <wtf/Markable.h>
#include <wtf/StdLibExtras.h>
#include <wtf/Vector.h>

namespace JSC {

class Heap;
class LocalAllocator;
class PreciseAllocation;

namespace Corpse {

static unsigned long long forReport(Address address)
{
    return address.toTargetVMAddress();
}

HeapWalk::HeapWalk(Snapshot& snapshot, Address rootsAddress)
{
    m_debugInfo = SnapshotDebugInfo::create(snapshot);
    if (!m_debugInfo)
        return;
    auto roots = TargetValue::completeObjectAt(snapshot, *m_debugInfo, rootsAddress);
    if (!roots)
        return;
    m_vm = Remote<VM*>(roots->properField("vm")).dereference();
    if (!m_vm) {
        CORPSE_REPORT("The roots at 0x%llx name no VM", forReport(rootsAddress));
        return;
    }

    // StructureID::decode adds g_jscConfig.startOfStructureHeap. The Structure
    // of every Structure is the VM's structureStructure, so its StructureID
    // names itself, and its address less its ID is that start.
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

    TargetValue blockHeaderPointer = roots->properField("markedBlockHeader");
    TargetValue localAllocatorPointer = roots->properField("localAllocator");
    if (!blockHeaderPointer || !localAllocatorPointer) {
        m_vm = { };
        return;
    }
    m_blockHeaderPointer = WTF::move(blockHeaderPointer);
    m_localAllocatorPointer = WTF::move(localAllocatorPointer);
}

// What MarkedBlock::Handle::isLive reads from the MarkedSpace and the Heap.
struct MarkedSpaceState {
    HeapVersion markingVersion;
    HeapVersion newlyAllocatedVersion;
    bool isMarking;
    bool isFullCollection;
};

namespace {

using AtomBits = HeapWalk::AtomBits;

// What MarkedBlock::Handle::isLive reads from a block and its header.
struct BlockState {
    bool isAllocated;
    HeapVersion markingVersion;
    HeapVersion newlyAllocatedVersion;
    AtomBits marks;
    AtomBits newlyAllocated;
};

// MarkedBlock::marksConveyLivenessDuringMarking(HeapVersion, HeapVersion).
bool marksConveyLivenessDuringMarking(const MarkedSpaceState& space, HeapVersion myMarkingVersion)
{
    if (!space.isFullCollection)
        return false;
    return myMarkingVersion == MarkedSpace::nullVersion
        || MarkedSpace::nextVersion(myMarkingVersion) == space.markingVersion;
}

// The locked path of MarkedBlock::Handle::isLive. Nothing in a corpse runs
// concurrently, so the optimistic path it tries first gives the same answer.
bool isLive(const MarkedSpaceState& space, const BlockState& block, size_t atom)
{
    if (block.isAllocated)
        return true;

    if (block.newlyAllocatedVersion == space.newlyAllocatedVersion)
        return block.newlyAllocated.get(atom);

    if (block.markingVersion != space.markingVersion) {
        if (!space.isMarking)
            return false;
        if (!marksConveyLivenessDuringMarking(space, block.markingVersion))
            return false;
    }

    return block.marks.get(atom);
}

// BlockDirectory::is<Kind>(index): the `kind` word of segment index / bitsPerSegment.
std::optional<bool> directoryBit(const Remote<BlockDirectory>& directory, BlockDirectoryBits::Kind kind, size_t index)
{
    using Segments = Vector<BlockDirectoryBits::Segment>;
    Remote<Segments> segments = directory.field<void>("m_bits").field<Segments>("m_segments");
    auto count = RemoteTraits<Segments>::size(segments);
    if (!count)
        return std::nullopt;
    size_t segmentIndex = index >> BlockDirectoryBits::segmentShift;
    if (segmentIndex >= *count) {
        CORPSE_REPORT("Block %zu is past the %zu bit segments of the BlockDirectory at 0x%llx", index, *count, forReport(directory.address()));
        return std::nullopt;
    }
    Remote<BlockDirectoryBits::Segment> segment = RemoteTraits<Segments>::element(segments, segmentIndex);
    if (!segment)
        return std::nullopt;
    if (segment.type()->byteSize() != sizeof(BlockDirectoryBits::Segment)) {
        CORPSE_REPORT("A BlockDirectory bit segment is %zu bytes, not the %zu this build uses", segment.type()->byteSize(), sizeof(BlockDirectoryBits::Segment));
        return std::nullopt;
    }
    auto words = segment.as<BlockDirectoryBits::Segment>();
    if (!words)
        return std::nullopt;
    return (words->m_data[static_cast<unsigned>(kind)] >> (index & BlockDirectoryBits::indexMask)) & 1;
}

// The MarkedBlock::Handle fields the walk reads.
struct HandleState {
    Address block;
    size_t atomsPerCell;
    size_t startAtom;
    HeapCell::Kind kind;
    size_t index;
    bool isFreeListed;
};

std::optional<HandleState> readHandle(const Remote<MarkedBlock::Handle>& handle)
{
    auto atomsPerCell = handle.field<unsigned>("m_atomsPerCell").integer();
    auto startAtom = handle.field<unsigned>("m_startAtom").integer();
    auto kind = handle.field<void>("m_attributes").field<HeapCell::Kind>("cellKind").integer();
    auto index = handle.field<unsigned>("m_index").integer();
    auto isFreeListed = handle.field<bool>("m_isFreeListed").integer();
    Address block = handle.field<MarkedBlock*>("m_block").dereference().address();
    if (!atomsPerCell || !startAtom || !kind || !index || !isFreeListed)
        return std::nullopt;
    // Enough to keep a corrupted handle from looping forever or leaving its block.
    if (*atomsPerCell <= 0 || *startAtom < 0 || static_cast<size_t>(*startAtom) >= MarkedBlock::endAtom
        || !block || block.toTargetVMAddress() & (MarkedBlock::blockSize - 1)) {
        CORPSE_REPORT("The block handle at 0x%llx describes no block", forReport(handle.address()));
        return std::nullopt;
    }
    return HandleState { block, static_cast<size_t>(*atomsPerCell), static_cast<size_t>(*startAtom), static_cast<HeapCell::Kind>(*kind), static_cast<size_t>(*index), !!*isFreeListed };
}

// FreeList::forEachInterval.
bool forEachFreeInterval(const TargetValue& freeList, const Function<bool(Address start, Address end)>& functor)
{
    auto intervalStart = freeList.properField("m_intervalStart").pointerValue();
    auto intervalEnd = freeList.properField("m_intervalEnd").pointerValue();
    TargetValue nextInterval = freeList.properField("m_nextInterval");
    auto cell = nextInterval.pointerValue();
    auto secret = freeList.properField("m_secret").as<uint64_t>();
    if (!intervalStart || !intervalEnd || !cell || !secret)
        return false;

    Address start = *intervalStart;
    Address end = *intervalEnd;
    for (size_t count = 0; count <= MarkedBlock::atomsPerBlock; ++count) {
        if (start < end && !functor(start, end))
            return false;

        if (FreeList::isSentinel(std::bit_cast<FreeCell*>(static_cast<uintptr_t>(cell->toTargetVMAddress()))))
            return true;

        // FreeCell::advance.
        auto scrambledBits = nextInterval.pointeeAt(*cell).properField("scrambledBits").as<uint64_t>();
        if (!scrambledBits)
            return false;
        auto [offsetToNext, lengthInBytes] = FreeCell::descramble(*scrambledBits, *secret);
        start = *cell;
        end = start + lengthInBytes;
        cell = Address { cell->toTargetVMAddress() + static_cast<int64_t>(offsetToNext) };
    }
    CORPSE_REPORT("The free list at 0x%llx has more intervals than a block has atoms", forReport(freeList.address()));
    return false;
}

// MarkedBlock::Handle::stopAllocating: every cell but those on the free list is newly allocated.
std::optional<AtomBits> stopAllocatingBlock(const HandleState& handle, const TargetValue& freeList)
{
    AtomBits newlyAllocated;
    newlyAllocated.clearAll();
    newlyAllocated.setEachNthBit(handle.atomsPerCell, handle.startAtom, MarkedBlock::endAtom);

    Address blockEnd = handle.block + MarkedBlock::blockSize;
    bool readable = forEachFreeInterval(freeList, [&](Address start, Address end) {
        if (start < handle.block || end > blockEnd) {
            CORPSE_REPORT("A free interval of the block at 0x%llx is outside it", forReport(handle.block));
            return false;
        }
        // MarkedBlock::candidateAtomNumber.
        newlyAllocated.clearEachNthBit(handle.atomsPerCell, (start - handle.block) / MarkedBlock::atomSize, (end - handle.block) / MarkedBlock::atomSize);
        return true;
    });
    if (!readable)
        return std::nullopt;
    return newlyAllocated;
}

} // anonymous namespace

// MarkedSpace::forEachLiveCell, inside a HeapIterationScope, whose
// MarkedSpace::willStartIterating stops every allocator first.
void HeapWalk::forEachLiveCell(const Function<IterationStatus(const Cell&)>& functor) const
{
    if (!isValid())
        return;
    Remote<Heap> heap = m_vm.field<Heap>("heap");
    Remote<MarkedSpace> space = heap.field<MarkedSpace>("m_objectSpace");
    auto markingVersion = space.field<HeapVersion>("m_markingVersion").as<HeapVersion>();
    auto newlyAllocatedVersion = space.field<HeapVersion>("m_newlyAllocatedVersion").as<HeapVersion>();
    auto isMarking = space.field<bool>("m_isMarking").as<bool>();
    auto collectionScope = heap.field<Markable<CollectionScope>>("m_collectionScope").as<Markable<CollectionScope>>();
    if (!markingVersion || !newlyAllocatedVersion || !isMarking || !collectionScope)
        return;
    MarkedSpaceState state { *markingVersion, *newlyAllocatedVersion, *isMarking, *collectionScope && collectionScope->value() == CollectionScope::Full };

    auto newlyAllocatedAfterStop = stopAllocating(space);
    if (!newlyAllocatedAfterStop)
        return;

    using BlockSet = UncheckedKeyHashSet<MarkedBlock*>;
    IterationStatus result = IterationStatus::Continue;
    RemoteTraits<BlockSet>::forEach(space.field<void>("m_blocks").field<BlockSet>("m_set"), [&](const Remote<MarkedBlock*>& block) {
        result = walkBlock(block, *newlyAllocatedAfterStop, state, functor);
        return result;
    });
    if (result == IterationStatus::Done)
        return;
    walkPreciseAllocations(space, functor);
}

// MarkedSpace::stopAllocating: BlockDirectory::stopAllocating for each
// directory, which runs LocalAllocator::stopAllocating for each allocator. The
// result is the newly allocated bits each free-listed block ends up with.
std::optional<HashMap<uint64_t, HeapWalk::AtomBits>> HeapWalk::stopAllocating(const Remote<MarkedSpace>& space) const
{
    using Allocators = SentinelLinkedList<LocalAllocator, BasicRawSentinelNode<LocalAllocator>>;
    Remote<LocalAllocator*> allocatorPointer { TargetValue { *m_localAllocatorPointer } };
    HashMap<uint64_t, AtomBits> result;
    bool failed = false;

    // MarkedSpace::forEachDirectory.
    Remote<BlockDirectory> directory = space.field<void>("m_directories").field<BlockDirectory*>("m_first").dereference();
    for (unsigned count = 0; directory; ++count) {
        if (count >= maxBlockDirectories) {
            CORPSE_REPORT("The MarkedSpace at 0x%llx lists more than %u BlockDirectories", forReport(space.address()), maxBlockDirectories);
            return std::nullopt;
        }
        bool readable = RemoteTraits<Allocators>::forEach(directory.field<Allocators>("m_localAllocators"), [&](const Remote<BasicRawSentinelNode<LocalAllocator>>& node) {
            // A LocalAllocator's list node is its first base.
            Remote<LocalAllocator> allocator = allocatorPointer.pointeeAt(node.address());
            if (allocator.base<void>(0).address() != node.address()) {
                CORPSE_REPORT("The LocalAllocator at 0x%llx is not at its list node", forReport(node.address()));
                failed = true;
                return IterationStatus::Done;
            }

            // LocalAllocator::stopAllocating.
            Remote<MarkedBlock::Handle> currentBlock = allocator.field<MarkedBlock::Handle*>("m_currentBlock").dereference();
            if (!currentBlock)
                return IterationStatus::Continue;
            auto handle = readHandle(currentBlock);
            Remote<FreeList> freeList = allocator.field<FreeList>("m_freeList");
            if (!handle || !freeList) {
                failed = true;
                return IterationStatus::Done;
            }
            // MarkedBlock::Handle::stopAllocating returns early for a block that is not free-listed.
            if (!handle->isFreeListed)
                return IterationStatus::Continue;
            auto newlyAllocated = stopAllocatingBlock(*handle, *freeList.typed());
            if (!newlyAllocated) {
                failed = true;
                return IterationStatus::Done;
            }
            result.add(currentBlock.address().toTargetVMAddress(), *newlyAllocated);
            return IterationStatus::Continue;
        });
        if (!readable || failed)
            return std::nullopt;
        directory = directory.field<BlockDirectory*>("m_nextDirectory").dereference();
    }
    return result;
}

// The body of the loop over m_blocks in MarkedSpace::forEachLiveCell:
// MarkedBlock::handle(), then MarkedBlock::Handle::forEachLiveCell.
IterationStatus HeapWalk::walkBlock(const Remote<MarkedBlock*>& blockPointer, const HashMap<uint64_t, AtomBits>& newlyAllocatedAfterStop, const MarkedSpaceState& space, const Function<IterationStatus(const Cell&)>& functor) const
{
    auto blockAddress = blockPointer.pointerValue();
    if (!blockAddress)
        return IterationStatus::Continue;
    // MarkedBlock::header().
    Remote<MarkedBlock::Header> header = Remote<MarkedBlock::Header*>(TargetValue { *m_blockHeaderPointer }).pointeeAt(*blockAddress + MarkedBlock::headerAtom * MarkedBlock::atomSize);
    Remote<MarkedBlock::Handle> handleValue = header.field<MarkedBlock::Handle*>("m_handle").dereference();
    auto handle = readHandle(handleValue);
    if (!handle)
        return IterationStatus::Continue;
    if (handle->block != *blockAddress) {
        CORPSE_REPORT("The handle of the block at 0x%llx is the handle of 0x%llx", forReport(*blockAddress), forReport(handle->block));
        return IterationStatus::Continue;
    }

    auto markingVersion = header.field<HeapVersion>("m_markingVersion").as<HeapVersion>();
    auto newlyAllocatedVersion = header.field<HeapVersion>("m_newlyAllocatedVersion").as<HeapVersion>();
    auto marks = header.field<AtomBits>("m_marks").as<AtomBits>();
    auto newlyAllocated = header.field<AtomBits>("m_newlyAllocated").as<AtomBits>();
    // MarkedBlock::Handle::isAllocated.
    auto isAllocated = directoryBit(handleValue.field<BlockDirectory*>("m_directory").dereference(), BlockDirectoryBits::Kind::Allocated, handle->index);
    if (!markingVersion || !newlyAllocatedVersion || !marks || !newlyAllocated || !isAllocated)
        return IterationStatus::Continue;

    BlockState block { *isAllocated, *markingVersion, *newlyAllocatedVersion, *marks, *newlyAllocated };
    if (auto stopped = newlyAllocatedAfterStop.getOptional(handleValue.address().toTargetVMAddress())) {
        block.newlyAllocated = *stopped;
        block.newlyAllocatedVersion = space.newlyAllocatedVersion;
    } else if (handle->isFreeListed) {
        // MarkedBlock::Handle::isLive asserts this never happens after stopAllocating.
        CORPSE_REPORT("The free-listed block at 0x%llx has no allocator", forReport(handle->block));
        return IterationStatus::Continue;
    }

    // MarkedBlock::Handle::forEachLiveCell.
    for (size_t atom = handle->startAtom; atom < MarkedBlock::endAtom; atom += handle->atomsPerCell) {
        if (!isLive(space, block, atom))
            continue;
        // MarkedBlock::Handle::cellSize.
        Cell cell { handle->block + atom * MarkedBlock::atomSize, handle->atomsPerCell * MarkedBlock::atomSize, handle->kind };
        if (functor(cell) == IterationStatus::Done)
            return IterationStatus::Done;
    }
    return IterationStatus::Continue;
}

// The loop over m_preciseAllocations in MarkedSpace::forEachLiveCell.
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
        auto isNewlyAllocated = allocation.field<bool>("m_isNewlyAllocated").integer();
        auto cellSize = allocation.field<void>("m_cellSize").integer();
        auto kind = allocation.field<void>("m_attributes").field<HeapCell::Kind>("cellKind").integer();
        if (!isMarked || !isNewlyAllocated || !cellSize || !kind)
            continue;
        // PreciseAllocation::isLive.
        if (!*isMarked && !*isNewlyAllocated)
            continue;
        // PreciseAllocation::cell, with headerSize() from the target's layout.
        size_t headerSize = roundUpToMultipleOf(PreciseAllocation::alignment, allocation.type()->byteSize());
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

// StructureID::decode.
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
