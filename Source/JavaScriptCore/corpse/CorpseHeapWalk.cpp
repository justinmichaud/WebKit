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
#include "CorpseRegion.h"
#include "CorpseThread.h"
#include <JavaScriptCore/BlockDirectoryBits.h>
#include <JavaScriptCore/CollectionScope.h>
#include <JavaScriptCore/FreeList.h>
#include <JavaScriptCore/JSString.h>
#include <JavaScriptCore/MarkedBlock.h>
#include <JavaScriptCore/MarkedSpace.h>
#include <JavaScriptCore/PreciseAllocation.h>
#include <JavaScriptCore/StructureID.h>
#include <bit>
#include <limits>
#include <span>
#include <tuple>
#include <unistd.h>
#if OS(DARWIN)
#include <pthread.h>
#endif
#include <wtf/BitSet.h>
#include <algorithm>
#include <wtf/HashMap.h>
#include <wtf/Deque.h>
#include <wtf/HashSet.h>
#include <wtf/Markable.h>
#include <wtf/StdLibExtras.h>
#include <string_view>
#include <wtf/CompactPtr.h>
#include <wtf/HexNumber.h>
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
    if (!blockHeaderPointer || !localAllocatorPointer) {
        m_vm = { };
        return;
    }
    m_blockHeaderPointer = WTF::move(blockHeaderPointer);
    m_localAllocatorPointer = WTF::move(localAllocatorPointer);
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

// The locked path of MarkedBlock::Handle::isLive, at a safe point, where
// isMarking is false. Nothing in a corpse runs concurrently, so the optimistic
// path it tries first gives the same answer.
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
// MarkedSpace::willStartIterating stops every allocator first.
void HeapWalk::forEachLiveCell(const Function<IterationStatus(const Cell&)>& functor) const
{
    if (!isValid())
        return;
    Remote<Heap> heap = m_vm.field<Heap>("heap");
    Remote<MarkedSpace> space = heap.field<MarkedSpace>("m_objectSpace");
    auto markingVersion = space.field<HeapVersion>("m_markingVersion").as<HeapVersion>();
    auto newlyAllocatedVersion = space.field<HeapVersion>("m_newlyAllocatedVersion").as<HeapVersion>();
    if (!markingVersion || !newlyAllocatedVersion || !isAtSafePoint(heap, space))
        return;
    MarkedSpaceState state { *markingVersion, *newlyAllocatedVersion };

    auto newlyAllocatedAfterStop = stopAllocating(space);
    if (!newlyAllocatedAfterStop)
        return;

    IterationStatus result = IterationStatus::Continue;
    RemoteTraits<BlockSet>::forEach(space.field<void>("m_blocks").field<BlockSet>("m_set"), [&](const Remote<MarkedBlock*>& block) {
        result = walkBlock(block, *newlyAllocatedAfterStop, state, functor);
        return result;
    });
    if (result == IterationStatus::Done)
        return;
    walkPreciseAllocations(space, functor);
}

// Where the mutator could start a collection: no collection is in progress, and
// no block is locked by an allocation, a sweep or stopAllocating. The walk relies
// on every invariant JSC keeps between collections, so it walks nothing else.
bool HeapWalk::isAtSafePoint(const Remote<Heap>& heap, const Remote<MarkedSpace>& space) const
{
    auto isMarking = space.field<bool>("m_isMarking").as<bool>();
    auto collectionScope = heap.field<Markable<CollectionScope>>("m_collectionScope").as<Markable<CollectionScope>>();
    if (!isMarking || !collectionScope)
        return false;
    if (*isMarking || *collectionScope) {
        CORPSE_REPORT("The snapshot is not at a safe point: the heap at 0x%llx is in a collection", forReport(heap.address()));
        return false;
    }

    // CountingLock::isHeldBit, which is private.
    constexpr unsigned isHeldBit = 1;
    bool atSafePoint = true;
    bool readable = RemoteTraits<BlockSet>::forEach(space.field<void>("m_blocks").field<BlockSet>("m_set"), [&](const Remote<MarkedBlock*>& block) {
        auto blockAddress = block.pointerValue();
        auto lock = blockAddress ? header(*blockAddress).field<void>("m_lock").field<unsigned>("m_word").as<unsigned>() : std::nullopt;
        if (lock && *lock & isHeldBit)
            CORPSE_REPORT("The snapshot is not at a safe point: the block at 0x%llx is locked", forReport(*blockAddress));
        atSafePoint = lock && !(*lock & isHeldBit);
        return atSafePoint ? IterationStatus::Continue : IterationStatus::Done;
    });
    return readable && atSafePoint;
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
IterationStatus HeapWalk::walkBlock(const Remote<MarkedBlock*>& blockPointer, const HashMap<uint64_t, AtomBits>& newlyAllocatedAfterStop, const MarkedSpaceState& space, const Function<IterationStatus(const Cell&)>& functor) const
{
    auto blockAddress = blockPointer.pointerValue();
    if (!blockAddress)
        return IterationStatus::Continue;
    Remote<MarkedBlock::Header> header = this->header(*blockAddress);
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
    }

    void run();

    const Vector<bool>& reached() const { return m_reached; }
    void fill(HeapWalk::Reach&) const;

    // A value the walk read that is not part of another one: the roots, a JS
    // cell, or a value reached through a pointer or as a container's element.
    struct Object {
        uint64_t address;
        uint64_t extent; // A JS cell's is the cell's, which may be bigger than its class.
        const TargetType* type;
        bool isCell;
    };
    // By address.
    Vector<Object> takeObjects();
    const Vector<String>& failedChecks() const { return m_failedChecks; }

private:
    using NotFollowed = HeapWalk::NotFollowed;
    enum class IsObject : bool { No, Yes };

    // The allocation `size` bytes at `address` lie in, if any.
    std::optional<size_t> allocationOf(Address, uint64_t size) const;

    void notFollowed(NotFollowed reason) { ++m_notFollowed[static_cast<size_t>(reason)]; }

    void enqueue(const TargetValue&, IsObject = IsObject::No);
    void walk(const TargetValue&);
    void walkClass(const TargetValue&);
    enum class IsComplete : bool { No, Yes };
    // Every member but `except`, which a reader reads its own way.
    void walkMembers(const TargetValue&, IsComplete, const char* except = nullptr);
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
    void walkUniqueArray(const TargetValue&);
    void walkJSString(const TargetValue&);
    void walkPropertyTable(const TargetValue&);

    const HeapWalk& m_heap;
    Snapshot& m_snapshot;
    const Vector<HeapWalk::Allocation>& m_allocations;
    Vector<bool> m_reached;
    // Each object is walked once as each type it is reached as.
    HashSet<std::pair<uint64_t, uint64_t>> m_visited;
    Vector<TargetValue> m_worklist;
    // The classes the walk has reached that a reader needs the type of, by name.
    HashMap<String, const TargetType*> m_reachedClasses;
    std::array<uint64_t, HeapWalk::numberOfNotFollowedReasons> m_notFollowed { };
    uint64_t m_cellsWithClass { 0 };
    uint64_t m_cellBytesBeyondClass { 0 };
    Vector<Object> m_objects;
    // The class and field being walked, which a failed check names.
    const TargetType* m_contextClass { nullptr };
    const TargetType::Field* m_contextField { nullptr };
    Vector<String> m_failedChecks;

    String context() const;
};

String ReachWalk::context() const
{
    if (!m_contextClass)
        return "the roots"_s;
    if (!m_contextField)
        return makeString("an element of a '"_s, m_contextClass->name(), '\'');
    return makeString(m_contextClass->name(), "::"_s, m_contextField->name);
}

Vector<ReachWalk::Object> ReachWalk::takeObjects()
{
    std::ranges::sort(m_objects, { }, &Object::address);
    return WTF::move(m_objects);
}

std::optional<size_t> ReachWalk::allocationOf(Address address, uint64_t size) const
{
    auto allocations = m_allocations.span();
    auto after = std::ranges::upper_bound(allocations, address, { }, &HeapWalk::Allocation::address);
    if (after == allocations.begin())
        return std::nullopt;
    const HeapWalk::Allocation& allocation = *(after - 1);
    if (address - allocation.address > allocation.size || size > allocation.size - (address - allocation.address))
        return std::nullopt;
    return (after - 1) - allocations.begin();
}

void ReachWalk::enqueue(const TargetValue& value, IsObject isObject)
{
    if (!value)
        return;
    if (!m_visited.add({ value.address().toTargetVMAddress(), std::bit_cast<uint64_t>(&value.type()) }).isNewEntry)
        return;
    m_worklist.append(value);
    if (isObject == IsObject::Yes)
        m_objects.append({ value.address().toTargetVMAddress(), value.type().byteSize(), &value.type(), false });
}

void ReachWalk::run()
{
    // The C++ objects first, so that the readers of the cells find the types they need.
    enqueue(*m_heap.m_roots, IsObject::Yes);
    auto drain = [&] {
        while (!m_worklist.isEmpty())
            walk(m_worklist.takeLast());
    };
    drain();
    m_heap.forEachLiveCell([&](const HeapWalk::Cell& cell) {
        walkCell(cell);
        drain();
        return IterationStatus::Continue;
    });
}

void ReachWalk::walkCell(const HeapWalk::Cell& cell)
{
    if (auto index = allocationOf(cell.address, cell.size))
        m_reached[*index] = true;
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
    const TargetType& home = klass->home();
    m_objects.append({ cell.address.toTargetVMAddress(), std::max<uint64_t>(cell.size, home.byteSize()), &home, true });
    enqueue(TargetValue::at(m_snapshot, cell.address, home));
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

void ReachWalk::walkClass(const TargetValue& reachedValue)
{
    // Patch 9: the class as the image that defines its destructor describes it.
    const TargetType& home = reachedValue.type().home();
    TargetValue value = &home == &reachedValue.type() ? reachedValue : TargetValue::at(m_snapshot, reachedValue.address(), home);
    auto name = home.name();
    std::string_view qualifiedName { name.legacyCStringPointer() };
    if (qualifiedName == "WTF::StringImpl")
        m_reachedClasses.add("WTF::StringImpl"_s, &home);
    if (walkByName(value, qualifiedName))
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
        if (field.bitSize || (except && std::string_view { field.name.legacyCStringPointer() } == except))
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
    const TargetType::Layout& layout = pointee.layout();
    auto* klass = std::get_if<TargetType::Class>(&layout);
    // A declaration has no layout, so whether it is polymorphic is not known.
    bool isDeclaration = klass && !pointee.byteSize();
    if (isDeclaration || (klass && klass->isPolymorphic)) {
        // Patch 2's rule, which never guesses: only a vtable that names a class does.
        Address completeObject;
        if (auto* dynamicType = pointee.debugInfo().dynamicTypeIfAnyAt(m_snapshot, address, completeObject)) {
            follow(completeObject, *dynamicType);
            return;
        }
        notFollowed(isDeclaration ? NotFollowed::Declaration : NotFollowed::NoDynamicType);
        if (isDeclaration)
            return;
    }
    if (!pointee.byteSize()) {
        notFollowed(NotFollowed::VoidPointer);
        return;
    }
    follow(address, pointee);
}

void ReachWalk::follow(Address address, const TargetType& type)
{
    // Only into an allocation, so that neither a pointer into static data nor
    // a union's other member read as a pointer leads anywhere.
    auto index = allocationOf(address, type.byteSize());
    if (!index) {
        // libpas's facts check the walk's: a pointer to where an allocation starts
        // whose pointee does not fit in it is of the wrong type.
        auto start = allocationOf(address, 1);
        if (start && m_allocations[*start].address == address) {
            m_failedChecks.append(makeString(context(), " points to the "_s, m_allocations[*start].size, "-byte allocation at 0x"_s, hex(address.toTargetVMAddress()),
                ", too small for the "_s, type.byteSize(), "-byte '"_s, type.name(), '\''));
        }
        return;
    }
    const HeapWalk::Allocation& allocation = m_allocations[*index];
    // An object of a heap with a type, such as an IsoHeap or a TZone heap, is of a class the type fits.
    const HeapWalk::AllocationFacts& facts = allocation.facts;
    if (allocation.address == address && facts.typeSize > 1 && type.byteSize() > roundUpToMultipleOf(std::max<uint32_t>(facts.typeAlignment, 1), facts.typeSize)) {
        m_failedChecks.append(makeString(context(), " points to an object of a heap of "_s, facts.typeSize, "-byte objects at 0x"_s, hex(address.toTargetVMAddress()),
            ", too small for the "_s, type.byteSize(), "-byte '"_s, type.name(), '\''));
    }
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
    if (name.starts_with("std::unique_ptr<") || name.starts_with("std::__1::unique_ptr<")) {
        const TargetType* elements = value.type().templateArgument(0);
        if (!elements || !std::holds_alternative<TargetType::Array>(elements->layout()))
            return false;
        walkUniqueArray(value);
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

// VectorBufferBase's m_buffer and m_size: every element, not only the first.
void ReachWalk::walkVector(const TargetValue& vector)
{
    // The buffer and count live in VectorBufferBase, the base of the Vector's base.
    TargetValue storage = vector;
    for (unsigned depth = 0; depth < 2; ++depth) {
        auto* klass = std::get_if<TargetType::Class>(&storage.type().layout());
        if (!klass || klass->bases.isEmpty())
            return;
        storage = storage.base(klass->bases[0]);
    }
    TargetValue buffer = storage.properField("m_buffer");
    auto size = storage.properField("m_size").integer();
    auto address = buffer.pointerValue();
    if (!size || !address || !*address || *size < 0)
        return;
    if (*size > maxVectorSize) {
        CORPSE_REPORT("The Vector at 0x%llx claims %lld elements", forReport(vector.address()), static_cast<long long>(*size));
        return;
    }
    // An inline buffer is in the Vector itself; any other is an allocation of its
    // own, which the Vector holds even with no elements in it.
    if (auto index = allocationOf(*address, 1))
        m_reached[*index] = true;
    const TargetType& element = std::get<TargetType::Pointer>(buffer.type().layout()).pointee;
    if (!element.byteSize()) {
        if (*size)
            notFollowed(NotFollowed::Declaration);
        return;
    }
    for (int64_t index = 0; index < *size; ++index)
        enqueue(TargetValue::at(m_snapshot, *address + static_cast<uint64_t>(index) * element.byteSize(), element), IsObject::Yes);
}

// Every bucket of HashTable's m_table. An empty bucket holds an empty value, and a
// deleted one a deleted value, and neither points into an allocation.
void ReachWalk::walkHashTable(const TargetValue& table)
{
    walkMembers(table, IsComplete::Yes, "m_table");
    TargetValue buckets = table.properField("m_table");
    auto address = buckets.pointerValue();
    if (!address || !*address)
        return;
    // HashTable::tableSizeOffset, which is private: an unsigned before the buckets.
    auto tableSize = m_snapshot.memory().ptr<unsigned>(*address - sizeof(unsigned));
    if (!tableSize) {
        CORPSE_REPORT("Could not read the size of the HashTable at 0x%llx", forReport(table.address()));
        return;
    }
    if (*tableSize > maxHashTableSize) {
        CORPSE_REPORT("The HashTable at 0x%llx claims %u buckets", forReport(table.address()), *tableSize);
        return;
    }
    const TargetType& bucket = std::get<TargetType::Pointer>(buckets.type().layout()).pointee;
    if (!bucket.byteSize()) {
        notFollowed(NotFollowed::Declaration);
        return;
    }
    if (auto index = allocationOf(*address, bucket.byteSize() * static_cast<uint64_t>(*tableSize)))
        m_reached[*index] = true;
    for (unsigned index = 0; index < *tableSize; ++index)
        enqueue(TargetValue::at(m_snapshot, *address + static_cast<uint64_t>(index) * bucket.byteSize(), bucket), IsObject::Yes);
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

// std::unique_ptr<T[]>, which records no count. new T[n] puts n in a cookie
// just before the elements when T has a destructor (Itanium C++ ABI 2.7), and
// the cookie starts the allocation. With no cookie, as WTF's UniqueArray
// allocates, the elements are read to the end of their allocation, past the
// last one by at most libpas's rounding up to its size class.
void ReachWalk::walkUniqueArray(const TargetValue& array)
{
    constexpr unsigned maxDepth = 8;
    std::optional<TargetValue> pointer;
    Function<void(const TargetValue&, unsigned)> findPointer = [&](const TargetValue& value, unsigned depth) {
        auto* klass = std::get_if<TargetType::Class>(&value.type().layout());
        if (pointer || !klass || depth > maxDepth)
            return;
        for (const TargetType::Field& field : klass->properFields) {
            if (std::holds_alternative<TargetType::Pointer>(field.type.layout())) {
                pointer = value.field(field);
                return;
            }
            findPointer(value.field(field), depth + 1);
        }
        for (const TargetType::Base& base : klass->bases)
            findPointer(value.base(base), depth + 1);
    };
    findPointer(array, 0);
    if (!pointer)
        return;
    auto address = pointer->pointerValue();
    if (!address || !*address)
        return;
    const TargetType& element = std::get<TargetType::Pointer>(pointer->type().layout()).pointee;
    if (!element.byteSize()) {
        notFollowed(std::holds_alternative<TargetType::Class>(element.layout()) ? NotFollowed::Declaration : NotFollowed::VoidPointer);
        return;
    }
    auto holder = allocationOf(*address, element.byteSize());
    if (!holder)
        return;
    const HeapWalk::Allocation& allocation = m_allocations[*holder];
    uint64_t count = (allocation.address + allocation.size - *address) / element.byteSize();
    if (*address - allocation.address >= sizeof(uint64_t)) {
        auto cookie = m_snapshot.memory().ptr<uint64_t>(*address - sizeof(uint64_t));
        if (cookie && *cookie <= count)
            count = *cookie;
    }
    m_reached[*holder] = true;
    for (uint64_t index = 0; index < std::min<uint64_t>(count, maxVectorSize); ++index)
        enqueue(TargetValue::at(m_snapshot, *address + index * element.byteSize(), element), IsObject::Yes);
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
    const TargetType* stringImpl = m_reachedClasses.get("WTF::StringImpl"_s);
    if (!stringImpl) {
        notFollowed(NotFollowed::TypeNotReached);
        return;
    }
    follow(Address { static_cast<uint64_t>(*fiber) }, *stringImpl);
}

void ReachWalk::fill(HeapWalk::Reach& result) const
{
    result.notFollowed = m_notFollowed;
    result.cellsWithClass = m_cellsWithClass;
    result.cellBytesBeyondClass = m_cellBytesBeyondClass;
}

// The totals of a walk, and its `missedCount` largest misses.
static void summarize(const ReachWalk& walk, const Vector<HeapWalk::Allocation>& allocations, size_t missedCount, HeapWalk::Reach& result)
{
    walk.fill(result);
    Vector<HeapWalk::Allocation> missed;
    for (size_t index = 0; index < allocations.size(); ++index) {
        result.bytesAllocated += allocations[index].size;
        if (walk.reached()[index])
            result.bytesReached += allocations[index].size;
        else
            missed.append(allocations[index]);
    }
    std::ranges::sort(missed, std::ranges::greater { }, &HeapWalk::Allocation::size);
    missed.shrink(std::min(missed.size(), missedCount));
    result.largestMissed = WTF::move(missed);
}

HeapWalk::Reach HeapWalk::reach(const Vector<Allocation>& allocations, size_t missedCount) const
{
    Reach result;
    if (!isValid())
        return result;
    ReachWalk walk(*this, allocations);
    walk.run();
    summarize(walk, allocations, missedCount, result);
    return result;
}

ASCIILiteral HeapWalk::description(EdgeReason reason)
{
    switch (reason) {
    case EdgeReason::Integer:
        return "an integer"_s;
    case EdgeReason::VoidPointer:
        return "a pointer to a type without a size"_s;
    case EdgeReason::Declaration:
        return "a pointer to a class that is only declared"_s;
    case EdgeReason::PointeeDoesNotFit:
        return "a pointer whose pointee does not fit in its allocation"_s;
    case EdgeReason::OtherField:
        return "a field of another type"_s;
    case EdgeReason::BeyondCellClass:
        return "a JS cell, past the end of its class"_s;
    case EdgeReason::UntypedBytes:
        return "a reached allocation, outside every value the walk read"_s;
    case EdgeReason::ImageData:
        return "an image's data"_s;
    case EdgeReason::Stack:
        return "a thread's stack"_s;
    case EdgeReason::OtherMemory:
        return "other memory"_s;
    case EdgeReason::MissedAllocation:
        return "another missed allocation"_s;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

namespace {

using EdgeReason = HeapWalk::EdgeReason;

// A missed allocation keeps this many of its edges, and counts the rest.
constexpr size_t maxEdgesKept = 16;

// libpas's pas_object_kind, which mya does not include.
ASCIILiteral objectKindName(uint8_t kind)
{
    constexpr std::array<ASCIILiteral, 7> names {
        "not in a bmalloc heap"_s, "small segregated"_s, "medium segregated"_s, "small bitfit"_s, "medium bitfit"_s, "marge bitfit"_s, "large"_s,
    };
    return kind < names.size() ? names[kind] : "of an unknown kind"_s;
}

struct Range {
    uint64_t begin;
    uint64_t end;
};

// Sorted, and merged where they overlap or touch.
Vector<Range> merged(Vector<Range>&& ranges)
{
    std::ranges::sort(ranges, { }, &Range::begin);
    Vector<Range> result;
    for (const Range& range : ranges) {
        if (!result.isEmpty() && range.begin <= result.last().end)
            result.last().end = std::max(result.last().end, range.end);
        else
            result.append(range);
    }
    return result;
}

class Attributor {
public:
    Attributor(Snapshot& snapshot, SnapshotDebugInfo& debugInfo, const Vector<HeapWalk::Allocation>& allocations, const Vector<bool>& reached, Vector<ReachWalk::Object>&& objects, HeapWalk::Attribution& result)
        : m_snapshot(snapshot)
        , m_debugInfo(debugInfo)
        , m_allocations(allocations)
        , m_objects(WTF::move(objects))
        , m_result(result)
    {
        m_missedIndex.fill(notMissed, allocations.size());
        Vector<size_t> missed;
        for (size_t index = 0; index < allocations.size(); ++index) {
            if (!reached[index])
                missed.append(index);
        }
        std::ranges::sort(missed, [&](size_t a, size_t b) { return allocations[a].size > allocations[b].size; });
        for (size_t index : missed) {
            m_missedIndex[index] = m_result.missed.size();
            m_result.missed.append({ allocations[index], { }, 0, std::nullopt, false });
        }
        // Only a word that points into a missed allocation is an edge, so that is all a word is looked up in.
        for (size_t index = 0; index < m_result.missed.size(); ++index) {
            uint64_t begin = m_result.missed[index].allocation.address.toTargetVMAddress();
            m_missedRanges.append({ begin, begin + m_result.missed[index].allocation.size, index });
        }
        std::ranges::sort(m_missedRanges, { }, &MissedRange::begin);
        for (const ReachWalk::Object& object : m_objects)
            m_maxObjectExtent = std::max(m_maxObjectExtent, object.extent);
    }

    void scan(const Vector<HeapWalk::Allocation>& excluded, const Vector<HeapWalk::Allocation>& heapPages);
    void attribute();
    void group();

private:
    static constexpr size_t notMissed = std::numeric_limits<size_t>::max();

    enum class SourceKind : uint8_t { Allocation, Stack, Region };
    struct Source {
        SourceKind kind;
        size_t allocation { 0 };
        String label; // A thread, for a stack; a region's name.
    };

    void scanRange(uint64_t begin, uint64_t end, const Source&);
    void scanRangeSkipping(uint64_t begin, uint64_t end, const Vector<Range>& skip, const Source&);
    void found(uint64_t at, uint64_t value, bool isPacked, const Source&);
    const ReachWalk::Object* objectContaining(uint64_t address) const;
    std::pair<EdgeReason, String> describeField(const ReachWalk::Object&, uint64_t offset) const;
    String heapTypeName(const HeapWalk::AllocationFacts&);

    Snapshot& m_snapshot;
    SnapshotDebugInfo& m_debugInfo;
    const Vector<HeapWalk::Allocation>& m_allocations;
    Vector<ReachWalk::Object> m_objects;
    uint64_t m_maxObjectExtent { 0 };
    HeapWalk::Attribution& m_result;
    Vector<size_t> m_missedIndex; // For each allocation, its index in m_result.missed, or notMissed.
    struct MissedRange {
        uint64_t begin;
        uint64_t end;
        size_t missed;
    };
    Vector<MissedRange> m_missedRanges; // By address.

    // The index in m_result.missed of the missed allocation `value` points into.
    std::optional<size_t> missedAt(uint64_t value) const
    {
        if (m_missedRanges.isEmpty() || value < m_missedRanges.first().begin || value >= m_missedRanges.last().end)
            return std::nullopt;
        auto ranges = m_missedRanges.span();
        auto after = std::ranges::upper_bound(ranges, value, { }, &MissedRange::begin);
        if (after == ranges.begin() || value >= (after - 1)->end)
            return std::nullopt;
        return (after - 1)->missed;
    }
    HashMap<uint64_t, String> m_typeNames;
};

const ReachWalk::Object* Attributor::objectContaining(uint64_t address) const
{
    // Objects nest, as a Vector's element in the Vector's owner, so the innermost one is the one the word is in.
    auto objects = m_objects.span();
    auto after = std::ranges::upper_bound(objects, address, { }, &ReachWalk::Object::address);
    const ReachWalk::Object* innermost = nullptr;
    for (auto candidate = after; candidate != objects.begin();) {
        --candidate;
        if (address - candidate->address >= m_maxObjectExtent)
            break;
        if (address - candidate->address < candidate->extent && (!innermost || candidate->extent < innermost->extent))
            innermost = &*candidate;
    }
    return innermost;
}

// The field of `type` that holds the byte at `offset`, through nested classes, bases and arrays.
struct FieldAt {
    const TargetType* owner;
    const TargetType::Field* field;
    const TargetType* type; // The field's type, or its element type for an array.
};

// A class with no data, such as a stateless deleter, which may share its
// address with a member that has data.
static bool isEmptyClass(const TargetType& type, unsigned depth = 0)
{
    auto* klass = std::get_if<TargetType::Class>(&type.layout());
    if (!klass || !klass->properFields.isEmpty() || klass->isPolymorphic || depth > 32)
        return false;
    return std::ranges::all_of(klass->bases, [&](const TargetType::Base& base) { return isEmptyClass(base.type, depth + 1); });
}

static std::optional<FieldAt> fieldAt(const TargetType& type, uint64_t offset, unsigned depth)
{
    if (depth > 32)
        return std::nullopt;
    auto* klass = std::get_if<TargetType::Class>(&type.layout());
    if (!klass)
        return std::nullopt;
    for (const TargetType::Field& field : klass->properFields) {
        uint64_t size = field.bitSize ? (field.bitOffset + field.bitSize + 7) / 8 : field.type.byteSize();
        if (offset < field.offset || offset - field.offset >= size || isEmptyClass(field.type))
            continue;
        const TargetType* fieldType = &field.type;
        uint64_t inner = offset - field.offset;
        while (auto* array = std::get_if<TargetType::Array>(&fieldType->layout())) {
            if (!array->element.byteSize())
                break;
            inner %= array->element.byteSize();
            fieldType = &array->element;
        }
        if (std::holds_alternative<TargetType::Class>(fieldType->layout())) {
            if (auto nested = fieldAt(fieldType->home(), inner, depth + 1))
                return nested;
        }
        return FieldAt { &type, &field, fieldType };
    }
    for (const TargetType::Base& base : klass->bases) {
        if (offset >= base.offset && offset - base.offset < base.type.byteSize() && !isEmptyClass(base.type)) {
            if (auto nested = fieldAt(base.type, offset - base.offset, depth + 1))
                return nested;
        }
    }
    for (const TargetType::Base& base : klass->virtualBases) {
        if (offset >= base.offset && offset - base.offset < base.type.byteSize()) {
            if (auto nested = fieldAt(base.type, offset - base.offset, depth + 1))
                return nested;
        }
    }
    return std::nullopt;
}

std::pair<EdgeReason, String> Attributor::describeField(const ReachWalk::Object& object, uint64_t offset) const
{
    const TargetType& type = *object.type;
    if (object.isCell && offset >= type.byteSize())
        return { EdgeReason::BeyondCellClass, type.name() };
    auto field = fieldAt(type, offset, 0);
    if (!field)
        return { EdgeReason::OtherField, makeString(type.name(), " (padding)"_s) };
    String owner = makeString(field->owner->name(), "::"_s, field->field->name);
    const TargetType::Layout& layout = field->type->layout();
    if (field->field->bitSize || std::holds_alternative<TargetType::Integer>(layout))
        return { EdgeReason::Integer, owner };
    if (auto* pointer = std::get_if<TargetType::Pointer>(&layout)) {
        if (pointer->pointee.byteSize())
            return { EdgeReason::PointeeDoesNotFit, owner };
        if (std::holds_alternative<TargetType::Class>(pointer->pointee.layout()))
            return { EdgeReason::Declaration, owner };
        return { EdgeReason::VoidPointer, owner };
    }
    return { EdgeReason::OtherField, owner };
}

String Attributor::heapTypeName(const HeapWalk::AllocationFacts& facts)
{
    if (!facts.heap)
        return makeString("objects "_s, objectKindName(facts.objectKind));
    return m_typeNames.ensure(facts.typeName, [&] {
        // A C string in the target, read up to the end of its page.
        if (facts.typeName) {
            uint64_t pageEnd = (facts.typeName | 4095) + 1;
            auto bytes = m_snapshot.memory().span<char>(Address { facts.typeName }, std::min<uint64_t>(pageEnd - facts.typeName, 256));
            if (bytes) {
                std::string_view characters { bytes.data(), bytes.size() };
                characters = characters.substr(0, characters.find('\0'));
                // A TZone bucket's name is generated, so its size class and alignment are what say what it holds.
                if (!characters.empty() && facts.typeSize > 1)
                    return makeString(String::fromUTF8(std::span { characters.data(), characters.size() }), " ("_s, facts.typeSize, "-byte objects aligned to "_s, facts.typeAlignment, ')');
                if (!characters.empty())
                    return String::fromUTF8(std::span { characters.data(), characters.size() });
            }
        }
        return makeString("a heap of "_s, facts.typeSize, "-byte objects"_s);
    }).iterator->value;
}

void Attributor::found(uint64_t at, uint64_t value, bool isPacked, const Source& source)
{
    auto target = missedAt(value);
    if (!target)
        return;
    // A missed allocation that holds a pointer to itself says nothing about why it is missed.
    if (source.kind == SourceKind::Allocation && m_missedIndex[source.allocation] == *target)
        return;
    HeapWalk::MissedAllocation& missed = m_result.missed[*target];
    ++missed.edgeCount;
    if (missed.edges.size() >= maxEdgesKept)
        return;

    HeapWalk::Edge edge { Address { at }, EdgeReason::OtherMemory, isPacked, { }, std::nullopt };
    if (auto* object = objectContaining(at)) {
        std::tie(edge.reason, edge.owner) = describeField(*object, at - object->address);
    } else if (source.kind == SourceKind::Allocation) {
        if (m_missedIndex[source.allocation] != notMissed) {
            edge.reason = EdgeReason::MissedAllocation;
            edge.fromMissed = m_missedIndex[source.allocation];
            edge.owner = makeString("a missed allocation of "_s, heapTypeName(m_allocations[source.allocation].facts));
        } else {
            edge.reason = EdgeReason::UntypedBytes;
            edge.owner = makeString("reached allocations of "_s, heapTypeName(m_allocations[source.allocation].facts));
        }
    } else if (source.kind == SourceKind::Stack) {
        edge.reason = EdgeReason::Stack;
        edge.owner = source.label;
    } else if (String symbol = m_debugInfo.symbolAt(Address { at }); !symbol.isNull()) {
        edge.reason = EdgeReason::ImageData;
        edge.owner = WTF::move(symbol);
    } else {
        edge.reason = EdgeReason::OtherMemory;
        edge.owner = source.label.isEmpty() ? "anonymous memory"_s : source.label;
    }
    missed.edges.append(WTF::move(edge));
}

// Every 8-byte word at 8-byte alignment, then every 6-byte packed pointer at
// 2-byte alignment that is not the low bytes of a word already seen.
void Attributor::scanRange(uint64_t begin, uint64_t end, const Source& source)
{
    constexpr uint64_t chunkSize = 64 * KB;
    constexpr uint64_t pageSize = 4 * KB;
    begin = roundUpToMultipleOf<sizeof(uint64_t)>(begin);
    end &= ~static_cast<uint64_t>(sizeof(uint64_t) - 1);
    for (uint64_t chunk = begin; chunk < end;) {
        uint64_t chunkEnd = std::min(end, roundUpToMultipleOf(chunkSize, chunk + 1));
        auto bytes = m_snapshot.memory().span<uint8_t>(Address { chunk }, static_cast<size_t>(chunkEnd - chunk));
        if (!bytes) {
            // Some of it is unreadable: try it a page at a time.
            if (chunkEnd - chunk > pageSize) {
                for (uint64_t page = chunk; page < chunkEnd;) {
                    uint64_t pageEnd = std::min(chunkEnd, roundUpToMultipleOf(pageSize, page + 1));
                    scanRange(page, pageEnd, source);
                    page = pageEnd;
                }
            }
            chunk = chunkEnd;
            continue;
        }
        std::span<const uint8_t> data { bytes };
        for (size_t offset = 0; offset + sizeof(uint64_t) <= data.size(); offset += sizeof(uint64_t)) {
            uint64_t word = 0;
            memcpySpan(asMutableByteSpan(word), data.subspan(offset, sizeof(word)));
            if (word)
                found(chunk + offset, word, false, source);
        }
        for (size_t offset = 0; offset + 6 <= data.size(); offset += 2) {
            uint64_t packed = 0;
            memcpySpan(asMutableByteSpan(packed).first(6), data.subspan(offset, 6));
            if (!packed)
                continue;
            if (!(offset % sizeof(uint64_t)) && offset + sizeof(uint64_t) <= data.size()) {
                uint64_t word = 0;
                memcpySpan(asMutableByteSpan(word), data.subspan(offset, sizeof(word)));
                if (word == packed)
                    continue;
            }
            found(chunk + offset, packed, true, source);
        }
        chunk = chunkEnd;
    }
}

void Attributor::scanRangeSkipping(uint64_t begin, uint64_t end, const Vector<Range>& skip, const Source& source)
{
    auto ranges = skip.span();
    auto next = std::ranges::upper_bound(ranges, begin, { }, &Range::end);
    uint64_t position = begin;
    for (; next != ranges.end() && next->begin < end; ++next) {
        if (next->begin > position)
            scanRange(position, next->begin, source);
        position = std::max(position, next->end);
    }
    if (position < end)
        scanRange(position, end, source);
}

void Attributor::scan(const Vector<HeapWalk::Allocation>& excluded, const Vector<HeapWalk::Allocation>& heapPages)
{
    auto rangesOf = [](const Vector<HeapWalk::Allocation>& allocations) {
        Vector<Range> ranges;
        for (const HeapWalk::Allocation& allocation : allocations)
            ranges.append({ allocation.address.toTargetVMAddress(), allocation.address.toTargetVMAddress() + allocation.size });
        return ranges;
    };
    Vector<Range> excludedRanges = merged(rangesOf(excluded));
    auto isExcluded = [&](const HeapWalk::Allocation& allocation) {
        uint64_t begin = allocation.address.toTargetVMAddress();
        auto next = std::ranges::upper_bound(excludedRanges, begin, { }, &Range::end);
        return next != excludedRanges.end() && next->begin < begin + allocation.size;
    };

    // Every allocation, reached or not.
    for (size_t index = 0; index < m_allocations.size(); ++index) {
        const HeapWalk::Allocation& allocation = m_allocations[index];
        if (isExcluded(allocation))
            continue;
        scanRange(allocation.address.toTargetVMAddress(), allocation.address.toTargetVMAddress() + allocation.size, { SourceKind::Allocation, index, { } });
    }

    // The live part of each thread's stack. In process, this thread's is the analysis's own.
    bool isInProcess = m_snapshot.process()->pid() == getpid();
    Vector<Range> stacks;
    for (const Thread& thread : m_snapshot.threads()) {
        if (!thread.hasStack())
            continue;
        stacks.append({ thread.stackRegion().base().toTargetVMAddress(), thread.stackRegion().end().toTargetVMAddress() });
#if OS(LINUX)
        bool isAnalysisThread = isInProcess && thread.id() == static_cast<uint64_t>(gettid());
#else
        uint64_t ownID = 0;
        pthread_threadid_np(nullptr, &ownID);
        bool isAnalysisThread = isInProcess && thread.id() == ownID;
#endif
        if (isAnalysisThread || !thread.stackPointer())
            continue;
        // Below the stack pointer, a leaf function may use the 128-byte red zone.
        uint64_t begin = std::max(thread.stackPointer().toTargetVMAddress() - 128, thread.stackRegion().base().toTargetVMAddress());
        String label = thread.name().empty() ? makeString("thread "_s, thread.id()) : makeString("thread "_s, thread.id(), " ("_s, String::fromUTF8(thread.name().c_str()), ')');
        scanRange(begin, thread.stackRegion().end().toTargetVMAddress(), { SourceKind::Stack, 0, WTF::move(label) });
    }

    // The other writable regions, leaving out what the allocations and stacks
    // covered, what is excluded, and libpas's free memory. In process, only the
    // images' writable sections: the other regions hold the analysis's own memory too.
    Vector<Range> skip = rangesOf(m_allocations);
    skip.appendVector(rangesOf(heapPages));
    skip.appendVector(excludedRanges);
    skip.appendVector(stacks);
    skip = merged(WTF::move(skip));
    if (isInProcess) {
        for (auto [base, size] : m_debugInfo.writableSections())
            scanRangeSkipping(base.toTargetVMAddress(), base.toTargetVMAddress() + size, skip, { SourceKind::Region, 0, { } });
        return;
    }
    for (const Region& region : m_snapshot.regions()) {
        if (!region.isReadable() || !region.isWritable() || region.isExecutable())
            continue;
        Source source { SourceKind::Region, 0, region.name() };
        for (auto [base, size] : region.residentParts(m_snapshot.corpsePort()))
            scanRangeSkipping(base.toTargetVMAddress(), base.toTargetVMAddress() + size, skip, source);
    }
}

// The order an edge is preferred in, to attribute a missed allocation: an edge
// in a value the walk read says most, and one in another missed allocation least.
unsigned preference(const HeapWalk::Edge& edge)
{
    unsigned rank = 0;
    switch (edge.reason) {
    case EdgeReason::Integer:
    case EdgeReason::VoidPointer:
    case EdgeReason::Declaration:
    case EdgeReason::PointeeDoesNotFit:
    case EdgeReason::OtherField:
        rank = 0;
        break;
    case EdgeReason::BeyondCellClass:
        rank = 1;
        break;
    case EdgeReason::ImageData:
        rank = 2;
        break;
    case EdgeReason::Stack:
        rank = 3;
        break;
    case EdgeReason::UntypedBytes:
        rank = 4;
        break;
    case EdgeReason::OtherMemory:
        rank = 5;
        break;
    case EdgeReason::MissedAllocation:
        rank = 7;
        break;
    }
    return rank * 2 + edge.isPacked;
}

void Attributor::attribute()
{
    // A missed allocation that only other missed allocations point to is
    // attributed through them, to the first edge on the way back that is not in one.
    Vector<Vector<size_t>> pointedToFrom(m_result.missed.size());
    Deque<size_t> attributed;
    for (size_t index = 0; index < m_result.missed.size(); ++index) {
        HeapWalk::MissedAllocation& missed = m_result.missed[index];
        std::ranges::sort(missed.edges, { }, [](const HeapWalk::Edge& edge) { return std::pair { preference(edge), edge.at.toTargetVMAddress() }; });
        for (const HeapWalk::Edge& edge : missed.edges) {
            if (edge.fromMissed)
                pointedToFrom[*edge.fromMissed].append(index);
        }
        if (!missed.edges.isEmpty() && missed.edges[0].reason != EdgeReason::MissedAllocation) {
            missed.attribution = missed.edges[0];
            attributed.append(index);
        }
    }
    while (!attributed.isEmpty()) {
        size_t source = attributed.takeFirst();
        for (size_t target : pointedToFrom[source]) {
            HeapWalk::MissedAllocation& missed = m_result.missed[target];
            if (missed.attribution)
                continue;
            missed.attribution = m_result.missed[source].attribution;
            missed.isAttributedThroughMissed = true;
            attributed.append(target);
        }
    }
}

void Attributor::group()
{
    HashMap<std::pair<unsigned, String>, size_t> groups;
    HashMap<String, size_t> byHeapType;
    HashMap<uint64_t, uint64_t> withoutEdge;
    for (const HeapWalk::MissedAllocation& missed : m_result.missed) {
        uint64_t size = missed.allocation.size;
        String typeName = heapTypeName(missed.allocation.facts);
        auto heapEntry = byHeapType.ensure(typeName, [&] {
            m_result.byHeapType.append({ EdgeReason::OtherMemory, typeName, 0, 0 });
            return m_result.byHeapType.size() - 1;
        });
        m_result.byHeapType[heapEntry.iterator->value].bytes += size;
        ++m_result.byHeapType[heapEntry.iterator->value].count;

        if (!missed.attribution) {
            if (!missed.edgeCount) {
                m_result.bytesWithoutEdge += size;
                withoutEdge.add(size, 0).iterator->value++;
            }
            continue;
        }
        const HeapWalk::Edge& edge = *missed.attribution;
        // A symbol, without how far into it the word is: the elements of one array are one group.
        String owner = edge.owner;
        if (edge.reason == EdgeReason::ImageData) {
            if (size_t offset = owner.reverseFind("+0x"_s); offset != notFound)
                owner = owner.left(offset);
        }
        auto entry = groups.ensure({ static_cast<unsigned>(edge.reason), owner }, [&] {
            m_result.groups.append({ edge.reason, owner, 0, 0 });
            return m_result.groups.size() - 1;
        });
        m_result.groups[entry.iterator->value].bytes += size;
        ++m_result.groups[entry.iterator->value].count;
    }
    std::ranges::sort(m_result.groups, std::ranges::greater { }, &HeapWalk::Group::bytes);
    std::ranges::sort(m_result.byHeapType, std::ranges::greater { }, &HeapWalk::Group::bytes);
    for (auto& [size, count] : withoutEdge)
        m_result.withoutEdgeBySize.append({ size, count });
    std::ranges::sort(m_result.withoutEdgeBySize, std::ranges::greater { }, [](const auto& entry) { return entry.first * entry.second; });
}

} // anonymous namespace

HeapWalk::Attribution HeapWalk::attribute(const Vector<Allocation>& allocations, const Vector<Allocation>& excluded, const Vector<Allocation>& heapPages) const
{
    Attribution result;
    if (!isValid())
        return result;
    CORPSE_DIAGNOSTICS(diagnostics, "attributing what the walk of the heap at 0x%llx misses", forReport(m_vm.address()));
    ReachWalk walk(*this, allocations);
    walk.run();
    summarize(walk, allocations, 0, result.reach);
    result.failedChecks = walk.failedChecks();
    Attributor attributor(m_roots->snapshot(), *m_debugInfo, allocations, walk.reached(), walk.takeObjects(), result);
    attributor.scan(excluded, heapPages);
    attributor.attribute();
    attributor.group();
    return result;
}

// Patch 10: a JS cell has no vtable, so its class comes from its Structure's
// ClassInfo, which is the s_info of the class it describes.
const TargetType* HeapWalk::cellClass(Address address) const
{
    if (!isValid())
        return nullptr;
    auto bits = jsCell(address).field<void>("m_structureID").field<uint32_t>("m_bits").integer();
    if (!bits)
        return nullptr;
    Remote<const ClassInfo*> classInfo = structure(static_cast<uint32_t>(*bits)).field<const ClassInfo*>("m_classInfo");
    return classOfClassInfo(Remote<ClassInfo>(classInfo.dereference()), 0);
}

const TargetType* HeapWalk::classOfClassInfo(const Remote<ClassInfo>& classInfo, unsigned depth) const
{
    if (!classInfo)
        return nullptr;
    uint64_t key = classInfo.address().toTargetVMAddress();
    if (auto entry = m_classesOfClassInfos.find(key); entry != m_classesOfClassInfos.end())
        return entry->value;

    const TargetType* klass = m_debugInfo->classOfStaticMember(classInfo.address(), "s_info");
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
