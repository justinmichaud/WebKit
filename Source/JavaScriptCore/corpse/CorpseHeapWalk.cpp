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
#include <JavaScriptCore/JSString.h>
#include <JavaScriptCore/MarkedBlock.h>
#include <JavaScriptCore/MarkedSpace.h>
#include <JavaScriptCore/PreciseAllocation.h>
#include <JavaScriptCore/StructureID.h>
#include <algorithm>
#include <bit>
#include <span>
#include <string_view>
#include <wtf/BitSet.h>
#include <wtf/CompactPtr.h>
#include <wtf/HashMap.h>
#include <wtf/HashSet.h>
#include <wtf/HexNumber.h>
#include <wtf/StdLibExtras.h>
#include <wtf/Vector.h>
#include <wtf/text/MakeString.h>
#include <wtf/text/WTFString.h>

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
    m_roots = *roots;
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
    auto atSafePoint = roots->properField("atSafePoint").integer();
    if (!blockHeaderPointer || !localAllocatorPointer || !atSafePoint) {
        m_vm = { };
        return;
    }
    m_blockHeaderPointer = WTF::move(blockHeaderPointer);
    m_localAllocatorPointer = WTF::move(localAllocatorPointer);
    m_isAtSafePoint = *atSafePoint;
}

// What MarkedBlock::Handle::isLive reads from the MarkedSpace.
struct MarkedSpaceState {
    HeapVersion markingVersion;
    HeapVersion newlyAllocatedVersion;
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

// The locked path of MarkedBlock::Handle::isLive, with isMarking false.
// Nothing in a corpse runs concurrently, so the optimistic path it tries first
// gives the same answer.
bool isLive(const MarkedSpaceState& space, const BlockState& block, size_t atom)
{
    if (block.isAllocated)
        return true;

    if (block.newlyAllocatedVersion == space.newlyAllocatedVersion)
        return block.newlyAllocated.get(atom);

    if (block.markingVersion != space.markingVersion)
        return false;

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
// MarkedSpace::willStartIterating stops every allocator first. It has no
// liveness rules for a collection in progress, where isMarking would be true
// and marksConveyLivenessDuringMarking would decide: at mya's safe point there
// is none, and on any other snapshot the result is unverified.
void HeapWalk::forEachLiveCell(const Function<IterationStatus(const Cell&)>& functor) const
{
    if (!isValid())
        return;
    Remote<Heap> heap = m_vm.field<Heap>("heap");
    Remote<MarkedSpace> space = heap.field<MarkedSpace>("m_objectSpace");
    auto markingVersion = space.field<HeapVersion>("m_markingVersion").as<HeapVersion>();
    auto newlyAllocatedVersion = space.field<HeapVersion>("m_newlyAllocatedVersion").as<HeapVersion>();
    if (!markingVersion || !newlyAllocatedVersion)
        return;
    MarkedSpaceState state { *markingVersion, *newlyAllocatedVersion };

    auto newlyAllocatedAfterStop = stopAllocating(space);
    if (!newlyAllocatedAfterStop)
        return;

    IterationStatus result = IterationStatus::Continue;
    forEachBlock([&](Address block) {
        result = walkBlock(block, *newlyAllocatedAfterStop, state, functor);
        return result;
    });
    if (result == IterationStatus::Done)
        return;
    walkPreciseAllocations(space, functor);
}

// The loop over m_blocks.set() in MarkedSpace::forEachLiveCell. False, and reported, if the set cannot be read.
bool HeapWalk::forEachBlock(const Function<IterationStatus(Address block)>& functor) const
{
    Remote<MarkedSpace> space = m_vm.field<Heap>("heap").field<MarkedSpace>("m_objectSpace");
    return RemoteTraits<BlockSet>::forEach(space.field<void>("m_blocks").field<BlockSet>("m_set"), [&](const Remote<MarkedBlock*>& block) {
        auto address = block.pointerValue();
        if (!address || !*address)
            return IterationStatus::Continue;
        return functor(*address);
    });
}

// MarkedBlock::header().
Remote<MarkedBlock::Header> HeapWalk::header(Address block) const
{
    return Remote<MarkedBlock::Header*>(TargetValue { *m_blockHeaderPointer }).pointeeAt(block + MarkedBlock::headerAtom * MarkedBlock::atomSize);
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
IterationStatus HeapWalk::walkBlock(Address blockAddress, const HashMap<uint64_t, AtomBits>& newlyAllocatedAfterStop, const MarkedSpaceState& space, const Function<IterationStatus(const Cell&)>& functor) const
{
    Remote<MarkedBlock::Header> header = this->header(blockAddress);
    Remote<MarkedBlock::Handle> handleValue = header.field<MarkedBlock::Handle*>("m_handle").dereference();
    auto handle = readHandle(handleValue);
    if (!handle)
        return IterationStatus::Continue;
    if (handle->block != blockAddress) {
        CORPSE_REPORT("The handle of the block at 0x%llx is the handle of 0x%llx", forReport(blockAddress), forReport(handle->block));
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

// The walk of HeapWalk::reach: every live cell as its C++ class, and every C++
// object reached from the roots or from a cell.
class ReachWalk {
public:
    ReachWalk(const HeapWalk& heap, const Vector<HeapWalk::Allocation>& allocations)
        : m_heap(heap)
        , m_snapshot(heap.m_roots->snapshot())
        , m_allocations(allocations)
    {
        m_reached.fill(false, allocations.size());
        m_typeAtStart.fill(nullptr, allocations.size());
        m_liveBytesInBlock.fill(0, allocations.size());
        m_isBlock.fill(false, allocations.size());
    }

    void run();
    void summarize(const Vector<HeapWalk::Allocation>& excluded, size_t listCount, HeapWalk::Reach&);

private:
    using NotFollowed = HeapWalk::NotFollowed;
    enum class IsObject : bool { No, Yes };

    // The allocation `size` bytes at `address` lie in, if any.
    std::optional<size_t> allocationOf(Address, uint64_t size) const;

    void notFollowed(NotFollowed reason) { ++m_notFollowed[static_cast<size_t>(reason)]; }

    // An object is a value the walk read that is not part of another one: the
    // roots, a JS cell, a block's header, or a value reached through a pointer
    // or as a container's element. Its bytes are what the walk types.
    void enqueue(const TargetValue&, IsObject = IsObject::No);
    void typed(Address, uint64_t size, const TargetType&);
    void walk(const TargetValue&);
    void walkClass(const TargetValue&);
    enum class IsComplete : bool { No, Yes };
    // Every member but `except`, which a reader reads its own way.
    void walkMembers(const TargetValue&, IsComplete, const char* except = nullptr);
    void walkBlock(Address);
    void walkCell(const HeapWalk::Cell&);

    // The pointee of the pointer `pointer`, of static type `pointee`, if it is
    // in an allocation: as its dynamic type if it has one.
    void followPointer(Address pointer, const TargetType& pointee);
    void follow(Address, const TargetType&);

    // Readers for values whose pointers the debug info cannot describe, picked
    // by the qualified name of a class the walk has reached. True if `value` was
    // one, and has been read.
    bool walkByName(const TargetValue&, std::string_view name);
    void walkVector(const TargetValue&);
    void walkHashTable(const TargetValue&);
    void walkTaggedPointer(const TargetValue&, const char* field, unsigned templateArgument, uint64_t tagMask);
    void walkCompactPointer(const TargetValue&);
    void walkPackedPointer(const TargetValue&);
    void walkJSString(const TargetValue&);
    void walkPropertyTable(const TargetValue&);

    // The class and field being walked, which an overrun names.
    String context() const;

    const HeapWalk& m_heap;
    Snapshot& m_snapshot;
    const Vector<HeapWalk::Allocation>& m_allocations;
    Vector<bool> m_reached;
    // Each object is walked once as each type it is reached as.
    HashSet<std::pair<uint64_t, uint64_t>> m_visited;
    Vector<TargetValue> m_worklist;
    struct Range {
        uint64_t begin;
        uint64_t end;
    };
    Vector<Range> m_typed; // Every object's bytes, clipped to its allocation.
    Vector<const TargetType*> m_typeAtStart; // For each allocation, the first type read where it starts.
    // A MarkedBlock is one allocation, whose free atoms are the JS heap's free memory.
    Vector<bool> m_isBlock;
    Vector<uint64_t> m_liveBytesInBlock; // Its live cells and its header.
    Vector<String> m_overruns;
    std::array<uint64_t, HeapWalk::numberOfNotFollowedReasons> m_notFollowed { };
    uint64_t m_cellsWithClass { 0 };
    uint64_t m_cellBytesBeyondClass { 0 };
    const TargetType* m_contextClass { nullptr };
    const TargetType::Field* m_contextField { nullptr };
};

String ReachWalk::context() const
{
    if (!m_contextClass)
        return "the roots"_s;
    if (!m_contextField)
        return makeString("an element of a '"_s, m_contextClass->name(), '\'');
    return makeString(m_contextClass->name(), "::"_s, m_contextField->name);
}

std::optional<size_t> ReachWalk::allocationOf(Address address, uint64_t size) const
{
    auto allocations = m_allocations.span();
    auto after = std::ranges::upper_bound(allocations, address, { }, &HeapWalk::Allocation::address);
    if (after == allocations.begin())
        return std::nullopt;
    const HeapWalk::Allocation& allocation = *(after - 1);
    if (address - allocation.address >= allocation.size || size > allocation.size - (address - allocation.address))
        return std::nullopt;
    return (after - 1) - allocations.begin();
}

void ReachWalk::typed(Address address, uint64_t size, const TargetType& type)
{
    auto index = allocationOf(address, 1);
    if (!index || !size)
        return;
    const HeapWalk::Allocation& allocation = m_allocations[*index];
    if (allocation.address == address && !m_typeAtStart[*index])
        m_typeAtStart[*index] = &type;
    uint64_t end = (allocation.address + allocation.size).toTargetVMAddress();
    if (size > end - address.toTargetVMAddress()) {
        m_overruns.append(makeString("the "_s, type.byteSize(), "-byte '"_s, type.name(), "' at 0x"_s, hex(address.toTargetVMAddress()),
            ", reached through "_s, context(), ", runs past the end of its "_s, allocation.size, "-byte allocation"_s));
        size = end - address.toTargetVMAddress();
    }
    m_typed.append({ address.toTargetVMAddress(), address.toTargetVMAddress() + size });
}

void ReachWalk::enqueue(const TargetValue& value, IsObject isObject)
{
    if (!value)
        return;
    if (!m_visited.add({ value.address().toTargetVMAddress(), std::bit_cast<uint64_t>(&value.type()) }).isNewEntry)
        return;
    m_worklist.append(value);
    if (isObject == IsObject::Yes)
        typed(value.address(), value.type().byteSize(), value.type());
}

void ReachWalk::run()
{
    auto drain = [&] {
        while (!m_worklist.isEmpty())
            walk(m_worklist.takeLast());
    };
    // The roots' VM is the test's description of it. The cells reach the VM as
    // JavaScriptCore describes it, with every type it owns complete.
    walkMembers(*m_heap.m_roots, IsComplete::Yes, "vm");
    drain();
    m_heap.forEachBlock([&](Address block) {
        walkBlock(block);
        drain();
        return IterationStatus::Continue;
    });
    m_heap.forEachLiveCell([&](const HeapWalk::Cell& cell) {
        walkCell(cell);
        drain();
        return IterationStatus::Continue;
    });
}

void ReachWalk::walkBlock(Address block)
{
    Remote<MarkedBlock::Header> header = m_heap.header(block);
    auto index = allocationOf(block, MarkedBlock::blockSize);
    if (!index || !header.typed())
        return;
    m_isBlock[*index] = true;
    m_reached[*index] = true;
    m_liveBytesInBlock[*index] += header.type()->byteSize();
    m_contextClass = nullptr;
    m_contextField = nullptr;
    enqueue(*header.typed(), IsObject::Yes);
}

void ReachWalk::walkCell(const HeapWalk::Cell& cell)
{
    auto index = allocationOf(cell.address, cell.size);
    if (index) {
        m_reached[*index] = true;
        if (m_isBlock[*index])
            m_liveBytesInBlock[*index] += cell.size;
    }
    if (!isJSCellKind(cell.kind))
        return;
    const TargetType* klass = m_heap.cellClass(cell.address);
    if (!klass) {
        notFollowed(NotFollowed::CellWithoutClass);
        return;
    }
    ++m_cellsWithClass;
    // A variable-sized cell, or a subclass that shares its base's ClassInfo, is bigger than the class.
    if (cell.size > klass->byteSize())
        m_cellBytesBeyondClass += cell.size - klass->byteSize();
    m_contextClass = nullptr;
    m_contextField = nullptr;
    enqueue(TargetValue::at(m_snapshot, cell.address, *klass), IsObject::Yes);
}

void ReachWalk::walk(const TargetValue& value)
{
    const TargetType::Layout& layout = value.type().layout();
    if (std::holds_alternative<TargetType::Class>(layout)) {
        walkClass(value);
        return;
    }
    if (auto* pointer = std::get_if<TargetType::Pointer>(&layout)) {
        if (auto address = value.pointerValue(); address && *address)
            followPointer(*address, pointer->pointee);
        return;
    }
    if (auto* array = std::get_if<TargetType::Array>(&layout)) {
        // An element of class type is an object in its own right, and the others are walked in place.
        for (size_t index = 0; index < array->count; ++index) {
            TargetValue element = TargetValue::at(m_snapshot, value.address() + index * array->element.byteSize(), array->element);
            if (std::holds_alternative<TargetType::Class>(array->element.layout()))
                enqueue(element);
            else
                walk(element);
        }
    }
}

void ReachWalk::walkClass(const TargetValue& value)
{
    m_contextClass = &value.type();
    m_contextField = nullptr;
    auto name = value.type().name();
    if (walkByName(value, std::string_view { name.legacyCStringPointer() }))
        return;
    walkMembers(value, IsComplete::Yes);
}

// TargetValue::forEachField, but with each base as a value of its own, which a
// reader may recognise, as a Packed<T*> is a PackedAlignedPtr.
void ReachWalk::walkMembers(const TargetValue& value, IsComplete isComplete, const char* except)
{
    auto* klass = std::get_if<TargetType::Class>(&value.type().layout());
    if (!klass)
        return;
    for (const TargetType::Field& field : klass->properFields) {
        // A field whose type liblldb could not parse has no size.
        if (field.bitSize || !field.type.byteSize() || (except && std::string_view { field.name.legacyCStringPointer() } == except))
            continue;
        m_contextClass = &value.type();
        m_contextField = &field;
        TargetValue fieldValue = value.field(field);
        if (std::holds_alternative<TargetType::Class>(field.type.layout()))
            enqueue(fieldValue);
        else
            walk(fieldValue);
    }
    auto walkBase = [&](const TargetType::Base& base) {
        TargetValue baseValue = value.base(base);
        if (!baseValue)
            return;
        auto name = baseValue.type().name();
        m_contextClass = &baseValue.type();
        m_contextField = nullptr;
        if (!walkByName(baseValue, std::string_view { name.legacyCStringPointer() }))
            walkMembers(baseValue, IsComplete::No);
    };
    for (const TargetType::Base& base : klass->bases)
        walkBase(base);
    // A virtual base's offset is only known in a complete object.
    if (isComplete == IsComplete::Yes) {
        for (const TargetType::Base& base : klass->virtualBases)
            walkBase(base);
    }
}

void ReachWalk::followPointer(Address address, const TargetType& pointee)
{
    auto* klass = std::get_if<TargetType::Class>(&pointee.layout());
    if (klass && !pointee.byteSize()) {
        // A class no compile unit of the image defines.
        notFollowed(NotFollowed::Declaration);
        return;
    }
    if (klass && klass->isPolymorphic) {
        Address completeObject;
        if (auto* dynamicType = pointee.debugInfo().dynamicTypeIfAnyAt(m_snapshot, address, completeObject)) {
            follow(completeObject, *dynamicType);
            return;
        }
        notFollowed(NotFollowed::NoDynamicType);
    }
    if (!pointee.byteSize()) {
        notFollowed(NotFollowed::VoidPointer);
        return;
    }
    follow(address, pointee);
}

void ReachWalk::follow(Address address, const TargetType& type)
{
    // Only into an allocation, so that a pointer into static data leads nowhere.
    auto index = allocationOf(address, type.byteSize());
    if (!index)
        return;
    m_reached[*index] = true;
    enqueue(TargetValue::at(m_snapshot, address, type), IsObject::Yes);
}

bool ReachWalk::walkByName(const TargetValue& value, std::string_view name)
{
    if (name.starts_with("WTF::Vector<")) {
        walkVector(value);
        return true;
    }
    if (name.starts_with("WTF::HashTable<")) {
        walkHashTable(value);
        return true;
    }
    if (name.starts_with("WTF::LazyUniqueRef<") || name.starts_with("WTF::LazyRef<")) {
        // LazyRef::lazyTag and initializingTag: a pointer to the function that will make the object.
        walkTaggedPointer(value, "m_pointer", 1, 0x3);
        return true;
    }
    if (name.starts_with("WTF::CompactPtr<")) {
        walkCompactPointer(value);
        return true;
    }
    if (name.starts_with("WTF::PackedAlignedPtr<")) {
        walkPackedPointer(value);
        return true;
    }
    if (name == "JSC::JSString") {
        walkJSString(value);
        return true;
    }
    if (name == "JSC::PropertyTable") {
        walkPropertyTable(value);
        return true;
    }
    return false;
}

// Every element, not only the first.
void ReachWalk::walkVector(const TargetValue& vector)
{
    auto storage = vectorStorage(vector);
    if (!storage)
        return;
    auto address = storage->buffer.pointerValue();
    if (!address || !*address)
        return;
    // An inline buffer is in the Vector itself; any other is an allocation of its
    // own, which the Vector holds even with no elements in it.
    if (auto index = allocationOf(*address, 1))
        m_reached[*index] = true;
    const TargetType& element = std::get<TargetType::Pointer>(storage->buffer.type().layout()).pointee;
    if (!element.byteSize()) {
        if (storage->size)
            notFollowed(NotFollowed::Declaration);
        return;
    }
    for (size_t index = 0; index < storage->size; ++index)
        enqueue(TargetValue::at(m_snapshot, *address + index * element.byteSize(), element), IsObject::Yes);
}

// Every bucket that is neither empty nor deleted. A deleted bucket's value has
// been destroyed but not cleared, so a raw pointer there still holds its old address.
void ReachWalk::walkHashTable(const TargetValue& table)
{
    walkMembers(table, IsComplete::Yes, "m_table");
    auto buckets = hashTableBuckets(table);
    if (!buckets || !buckets->size)
        return;
    auto address = buckets->table.pointerValue();
    const TargetType& bucket = std::get<TargetType::Pointer>(buckets->table.type().layout()).pointee;
    auto traits = defaultHashTraits(table.type());
    if (!address || !bucket.byteSize() || !traits) {
        notFollowed(NotFollowed::HashTableNotRead);
        return;
    }
    if (auto index = allocationOf(*address, 1))
        m_reached[*index] = true;
    for (unsigned index = 0; index < buckets->size; ++index) {
        Address bucketAddress = *address + static_cast<uint64_t>(index) * bucket.byteSize();
        auto isEmptyOrDeleted = isEmptyOrDeletedBucket(m_snapshot, bucketAddress, *traits);
        if (!isEmptyOrDeleted)
            return;
        if (!*isEmptyOrDeleted)
            enqueue(TargetValue::at(m_snapshot, bucketAddress, bucket), IsObject::Yes);
    }
}

// A pointer to the type of a template argument, held in an integer field with tag bits.
void ReachWalk::walkTaggedPointer(const TargetValue& value, const char* field, unsigned templateArgument, uint64_t tagMask)
{
    auto bits = value.properField(field).integer();
    if (!bits || !*bits || (static_cast<uint64_t>(*bits) & tagMask))
        return;
    const TargetType* pointee = value.type().templateArgument(templateArgument);
    if (!pointee) {
        notFollowed(NotFollowed::TypeNotReached);
        return;
    }
    followPointer(Address { static_cast<uint64_t>(*bits) }, *pointee);
}

// CompactPtr::decode.
void ReachWalk::walkCompactPointer(const TargetValue& value)
{
    auto bits = value.properField("m_ptr").integer();
    if (!bits || !*bits)
        return;
    uint64_t address = static_cast<uint64_t>(*bits);
#if HAVE(36BIT_ADDRESS)
    // An outsized pointer is encoded through a side table, which the walk does not read.
    if (address & OutsizedCompactPtr::isOutsizedBit) {
        notFollowed(NotFollowed::TypeNotReached);
        return;
    }
    address = static_cast<uint64_t>(static_cast<uint32_t>(address)) << CompactPtr<void>::bitsShift;
#endif
    const TargetType* pointee = value.type().templateArgument(0);
    if (!pointee) {
        notFollowed(NotFollowed::TypeNotReached);
        return;
    }
    followPointer(Address { address }, *pointee);
}

// PackedAlignedPtr::get: the low bytes of the pointer, in m_storage, stored
// shifted right by the alignment when that saves a byte.
void ReachWalk::walkPackedPointer(const TargetValue& value)
{
    TargetValue storage = value.properField("m_storage");
    if (!storage)
        return;
    const TargetType* pointee = value.type().templateArgument(0);
    auto alignment = value.type().templateIntegerArgument(1);
    constexpr unsigned addressWidth = OS_CONSTANT(EFFECTIVE_ADDRESS_WIDTH);
    if (!pointee || !alignment || !hasOneBitSet(*alignment) || getLSBSet(*alignment) >= addressWidth) {
        notFollowed(NotFollowed::TypeNotReached);
        return;
    }
    // PackedAlignedPtr's storageSizeWithoutAlignmentShift, storageSize and alignmentShiftSize.
    unsigned shiftIfProfitable = getLSBSet(*alignment);
    size_t sizeWithoutShift = roundUpToMultipleOf<8>(addressWidth) / 8;
    size_t sizeWithShift = roundUpToMultipleOf<8>(addressWidth - shiftIfProfitable) / 8;
    unsigned shift = sizeWithoutShift > sizeWithShift ? shiftIfProfitable : 0;
    size_t size = storage.type().byteSize();
    if (size != sizeWithShift) {
        CORPSE_REPORT("The %zu-byte PackedAlignedPtr at 0x%llx, aligned to %llu, is not the %zu bytes this build stores", size, forReport(value.address()), static_cast<unsigned long long>(*alignment), sizeWithShift);
        return;
    }
    auto bytes = m_snapshot.memory().span<uint8_t>(storage.address(), size);
    if (!bytes)
        return;
    uint64_t address = 0;
    memcpySpan(asMutableByteSpan(address).first(size), std::span<const uint8_t> { bytes });
    if (address)
        followPointer(Address { address << shift }, *pointee);
}

// PropertyTable::m_indexVector: the table's index buffer, with
// PropertyTable::isCompactFlag. The buffer is one allocation, which holds the
// indices and then the entries, of a type that depends on the flag.
void ReachWalk::walkPropertyTable(const TargetValue& table)
{
    walkMembers(table, IsComplete::Yes, "m_indexVector");
    auto bits = table.properField("m_indexVector").integer();
    if (!bits || !*bits)
        return;
    // PropertyTable::indexVectorMask, which is private.
    constexpr uint64_t isCompactFlag = 1;
    if (auto index = allocationOf(Address { static_cast<uint64_t>(*bits) & ~isCompactFlag }, 1))
        m_reached[*index] = true;
}

// JSString::m_fiber: a resolved string's String, whose StringImpl it holds,
// or, with JSString::isRopeInPointer set, a rope's first fiber, a cell.
void ReachWalk::walkJSString(const TargetValue& string)
{
    walkMembers(string, IsComplete::Yes, "m_fiber");
    auto fiber = string.properField("m_fiber").integer();
    if (!fiber || !*fiber || (static_cast<uint64_t>(*fiber) & JSString::isRopeInPointer))
        return;
    const TargetType* stringImpl = m_heap.stringImplClass(string.address());
    if (!stringImpl) {
        notFollowed(NotFollowed::TypeNotReached);
        return;
    }
    follow(Address { static_cast<uint64_t>(*fiber) }, *stringImpl);
}

void ReachWalk::summarize(const Vector<HeapWalk::Allocation>& excluded, size_t listCount, HeapWalk::Reach& result)
{
    result.notFollowed = m_notFollowed;
    result.cellsWithClass = m_cellsWithClass;
    result.cellBytesBeyondClass = m_cellBytesBeyondClass;
    result.overruns = WTF::move(m_overruns);
    result.isReached = m_reached;

    // The typed bytes, each once: an object read as two types, or a field read inside its object, counts once.
    std::ranges::sort(m_typed, { }, &Range::begin);
    result.bytesTypedIn.fill(0, m_allocations.size());
    size_t allocationIndex = 0;
    uint64_t coveredTo = 0;
    for (const Range& range : m_typed) {
        uint64_t begin = std::max(range.begin, coveredTo);
        if (begin >= range.end)
            continue;
        coveredTo = range.end;
        while (allocationIndex < m_allocations.size() && (m_allocations[allocationIndex].address + m_allocations[allocationIndex].size).toTargetVMAddress() <= begin)
            ++allocationIndex;
        // typed() clipped every range to the one allocation it starts in.
        if (allocationIndex < m_allocations.size())
            result.bytesTypedIn[allocationIndex] += range.end - begin;
    }

    auto excludedRanges = excluded.span();
    auto isExcluded = [&](const HeapWalk::Allocation& allocation) {
        auto next = std::ranges::upper_bound(excludedRanges, allocation.address, { }, [](const HeapWalk::Allocation& range) { return range.address + range.size; });
        return next != excludedRanges.end() && next->address < allocation.address + allocation.size;
    };
    Vector<HeapWalk::Allocation> missed;
    Vector<HeapWalk::Untyped> untyped;
    for (size_t index = 0; index < m_allocations.size(); ++index) {
        const HeapWalk::Allocation& allocation = m_allocations[index];
        result.bytesAllocated += allocation.size;
        if (isExcluded(allocation)) {
            result.bytesExcluded += allocation.size;
            continue;
        }
        uint64_t accounted = allocation.size;
        if (m_isBlock[index]) {
            accounted = std::min(m_liveBytesInBlock[index], allocation.size);
            result.bytesFreeInBlocks += allocation.size - accounted;
        }
        result.bytesTyped += result.bytesTypedIn[index];
        if (!m_reached[index]) {
            missed.append(allocation);
            continue;
        }
        result.bytesReached += allocation.size;
        if (accounted > result.bytesTypedIn[index])
            untyped.append({ allocation, accounted - result.bytesTypedIn[index], m_typeAtStart[index] ? makeString(m_typeAtStart[index]->name()) : String() });
    }
    std::ranges::sort(missed, std::ranges::greater { }, &HeapWalk::Allocation::size);
    missed.shrink(std::min(missed.size(), listCount));
    result.largestMissed = WTF::move(missed);
    std::ranges::sort(untyped, std::ranges::greater { }, &HeapWalk::Untyped::bytesUntyped);
    untyped.shrink(std::min(untyped.size(), listCount));
    result.mostUntyped = WTF::move(untyped);
}

HeapWalk::Reach HeapWalk::reach(const Vector<Allocation>& allocations, const Vector<Allocation>& excluded, size_t listCount) const
{
    Reach result;
    if (!isValid())
        return result;
    CORPSE_DIAGNOSTICS(diagnostics, "measuring the reach of the walk of the heap at 0x%llx", forReport(m_vm.address()));
    ReachWalk walk(*this, allocations);
    walk.run();
    walk.summarize(excluded, listCount, result);
    return result;
}

// A JS cell has no vtable, so its class comes from its Structure's ClassInfo,
// which is the s_info of the class it describes.
Remote<ClassInfo> HeapWalk::classInfoOf(Address address) const
{
    auto bits = jsCell(address).field<void>("m_structureID").field<uint32_t>("m_bits").integer();
    if (!bits)
        return { };
    return Remote<ClassInfo>(structure(static_cast<uint32_t>(*bits)).field<const ClassInfo*>("m_classInfo").dereference());
}

const TargetType* HeapWalk::cellClass(Address address) const
{
    if (!isValid())
        return nullptr;
    return classOfClassInfo(classInfoOf(address), 0);
}

const TargetType* HeapWalk::stringImplClass(Address string) const
{
    if (!m_stringImplClass) {
        // The compile unit that defines JSString::s_info reads a string's characters, so it uses StringImpl whole.
        Remote<ClassInfo> classInfo = classInfoOf(string);
        m_stringImplClass = classInfo ? m_debugInfo->classInCompileUnitOf(classInfo.address(), "s_emptyAtomString", "_ZN3WTF10StringImpl17s_emptyAtomStringE") : nullptr;
    }
    return *m_stringImplClass;
}

const TargetType* HeapWalk::classOfClassInfo(const Remote<ClassInfo>& classInfo, unsigned depth) const
{
    if (!classInfo)
        return nullptr;
    uint64_t key = classInfo.address().toTargetVMAddress();
    if (auto entry = m_classesOfClassInfos.find(key); entry != m_classesOfClassInfos.end())
        return entry->value;

    const TargetType* klass = m_debugInfo->classInCompileUnitOf(classInfo.address(), "s_info");
    // ClassInfo::staticClassSize is sizeof the class it was made for.
    auto size = klass ? classInfo.field<unsigned>("staticClassSize").integer() : std::nullopt;
    if (klass && (!size || static_cast<uint64_t>(*size) != klass->byteSize())) {
        if (size)
            CORPSE_REPORT("The ClassInfo at 0x%llx is for %lld-byte classes, but its '%s' is %zu bytes", forReport(classInfo.address()), static_cast<long long>(*size), klass->name().legacyCStringPointer(), klass->byteSize());
        klass = nullptr;
    }
    // ClassInfo::parentClass is the ClassInfo of one of its bases.
    Remote<ClassInfo> parent = klass ? classInfo.field<const ClassInfo*>("parentClass").dereference() : Remote<ClassInfo> { };
    if (klass && parent) {
        const TargetType* parentClass = depth < maxClassInfoDepth ? classOfClassInfo(parent, depth + 1) : nullptr;
        Vector<const TargetType*, 8> bases { klass };
        bool isBase = false;
        for (size_t index = 0; index < bases.size() && !isBase && parentClass; ++index) {
            isBase = bases[index] == parentClass;
            if (auto* layout = std::get_if<TargetType::Class>(&bases[index]->layout())) {
                for (const TargetType::Base& base : layout->bases)
                    bases.append(&base.type);
            }
        }
        if (!isBase) {
            if (parentClass)
                CORPSE_REPORT("The parent of the ClassInfo of '%s' at 0x%llx is for '%s', which is not one of its bases", klass->name().legacyCStringPointer(), forReport(classInfo.address()), parentClass->name().legacyCStringPointer());
            klass = nullptr;
        }
    }
    m_classesOfClassInfos.add(key, klass);
    return klass;
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
