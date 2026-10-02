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

#include "CorpseCellClasses.h"
#include "CorpseError.h"
#include "CorpseLimits.h"
#include "CorpseRegion.h"
#include "CorpseThread.h"
#include <JavaScriptCore/BlockDirectoryBits.h>
#include <JavaScriptCore/CollectionScope.h>
#include <JavaScriptCore/FreeList.h>
#include <JavaScriptCore/JITCode.h>
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
#include <wtf/SegmentedVector.h>
#include <wtf/HexNumber.h>
#include <wtf/StdLibExtras.h>
#include <wtf/WTFConfig.h>
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

    // Structure::s_info, which every Structure's ClassInfo is, is in JavaScriptCore's image.
    m_javaScriptCoreImage = structureStructure.field<const ClassInfo*>("m_classInfo").pointerValue().value_or(Address { });
    auto classNamed = [&](const char* name) -> const TargetType* {
        const TargetType* klass = m_javaScriptCoreImage ? m_debugInfo->classNamed(m_javaScriptCoreImage, name) : nullptr;
        if (!klass)
            CORPSE_REPORT("JavaScriptCore's image has no class '%s'", name);
        return klass;
    };
    m_blockHeaderClass = classNamed("JSC::MarkedBlock::Header");
    m_localAllocatorClass = classNamed("JSC::LocalAllocator");
    m_stringImplClass = classNamed("WTF::StringImpl");
    m_fatEntryClass = classNamed("JSC::SymbolTableEntry::FatEntry");
    m_wtfConfigClass = classNamed("WTF::Config");
    m_jscConfigClass = classNamed("JSC::Config");
    m_config = m_javaScriptCoreImage ? m_debugInfo->symbolAddress(m_javaScriptCoreImage, "g_config") : std::nullopt;
    if (!m_config)
        CORPSE_REPORT("JavaScriptCore's image has no symbol 'g_config'");
#if ENABLE(JIT)
    m_baselineJITDataClass = classNamed("JSC::BaselineJITData");
#endif
#if ENABLE(DFG_JIT)
    m_dfgJITDataClass = classNamed("JSC::DFG::JITData");
#endif
    auto atSafePoint = roots->properField("atSafePoint").integer();
    if (!m_blockHeaderClass || !m_localAllocatorClass || !m_stringImplClass || !m_fatEntryClass || !m_wtfConfigClass || !m_jscConfigClass || !m_config || !atSafePoint) {
        m_vm = { };
        return;
    }
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
    return Remote<MarkedBlock::Header>(TargetValue::at(m_roots->snapshot(), block + MarkedBlock::headerAtom * MarkedBlock::atomSize, *m_blockHeaderClass));
}

// MarkedSpace::stopAllocating: BlockDirectory::stopAllocating for each
// directory, which runs LocalAllocator::stopAllocating for each allocator. The
// result is the newly allocated bits each free-listed block ends up with.
std::optional<HashMap<uint64_t, HeapWalk::AtomBits>> HeapWalk::stopAllocating(const Remote<MarkedSpace>& space) const
{
    using Allocators = SentinelLinkedList<LocalAllocator, BasicRawSentinelNode<LocalAllocator>>;
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
            Remote<LocalAllocator> allocator { TargetValue::at(m_roots->snapshot(), node.address(), *m_localAllocatorClass) };
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
    // Fills in result.misses and result.bytesMissedByCause, after summarize.
    void explainMisses(const Vector<HeapWalk::Allocation>& excluded, const Vector<HeapWalk::Allocation>& notReferrers, HeapWalk::Reach&);

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
    void walkGlobalVariables();
    void walkStacks();
    void walkBlock(Address);
    void walkCell(const HeapWalk::Cell&);

    // The pointee of the pointer `pointer`, of static type `pointee`, if it is
    // in an allocation: as its dynamic type if it has one.
    void followPointer(Address pointer, const TargetType& pointee);
    void follow(Address, const TargetType&);

    // Readers for values whose pointers the debug info cannot describe, picked
    // by the qualified name of a class the walk has reached. True if `value` was
    // one, and has been read.
    bool walkByName(const TargetValue&);
    enum class Reader : uint8_t { None, Vector, HashTable, RobinHoodHashTable, TrailingArray, ButterflyArray, SegmentedVector, ConcurrentBufferArray, SymbolTableEntry, CodeBlock, AlignedStorage, CodePointer, LazyPointer, CompactPointer, PackedPointer, JSString, PropertyTable, Optional };
    Reader readerFor(const TargetType&);
    static Reader readerNamed(std::string_view qualifiedName);
    void walkVector(const TargetValue&);
    void walkHashTable(const TargetValue&);
    void walkRobinHoodHashTable(const TargetValue&);
    void walkTrailingArray(const TargetValue&);
    void walkButterflyArray(const TargetValue&);
    void walkCodeBlock(const TargetValue&);
    void walkSegmentedVector(const TargetValue&);
    void walkConcurrentBufferArray(const TargetValue&);
    void walkSymbolTableEntry(const TargetValue&);
    void walkAlignedStorage(const TargetValue&);
    // `count` values of type `element` from `address`, each an object of its own.
    void walkElements(Address, const TargetType& element, uint64_t count);
    void walkCodePointer(const TargetValue&);
    void walkTaggedPointer(const TargetValue&, const char* field, unsigned templateArgument, uint64_t tagMask);
    void walkCompactPointer(const TargetValue&);
    void walkPackedPointer(const TargetValue&);
    void walkJSString(const TargetValue&);
    void walkPropertyTable(const TargetValue&);
    void walkOptional(const TargetValue&);

    // The class and field being walked, which an overrun names.
    String context() const;

    const HeapWalk& m_heap;
    Snapshot& m_snapshot;
    const Vector<HeapWalk::Allocation>& m_allocations;
    Vector<bool> m_reached;
    // Each object is walked once as each type it is reached as.
    HashSet<std::pair<uint64_t, uint64_t>> m_visited;
    HashMap<const TargetType*, Reader> m_readers;
    Vector<TargetValue> m_worklist;
    struct Range {
        uint64_t begin;
        uint64_t end;
        const TargetType* type; // Read at `begin`.
    };
    Vector<Range> m_typed; // Every object's bytes, clipped to its allocation, sorted by summarize.
    // What holds the word at `address`, of an allocation the walk reached.
    String holderInReached(Address, size_t allocationIndex) const;
    struct Holder {
        HeapWalk::MissCause cause;
        String description;
        std::optional<size_t> missedHolder; // For MissCause::Missed, the allocation's index.
    };
    Holder holderOf(Address word);
    Vector<const TargetType*> m_typeAtStart; // For each allocation, the first type read where it starts.
    // A MarkedBlock is one allocation, whose free atoms are the JS heap's free memory.
    Vector<bool> m_isBlock;
    Vector<uint64_t> m_liveBytesInBlock; // Its live cells and its header.
    Vector<String> m_overruns;
    std::array<uint64_t, HeapWalk::numberOfNotFollowedReasons> m_notFollowed { };
    uint64_t m_cellsWithClass { 0 };
    uint64_t m_globalVariables { 0 };
    uint64_t m_untypedDataSymbols { 0 };
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
    m_typed.append({ address.toTargetVMAddress(), address.toTargetVMAddress() + size, &type });
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
    walkGlobalVariables();
    drain();
    walkStacks();
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

// Every global variable of every image with debug info, as its own image
// describes it, including function-local statics.
void ReachWalk::walkGlobalVariables()
{
    size_t untyped = 0;
    const Vector<Region>& regions = m_snapshot.regions();
    // Memory the target cannot write holds no address it allocated, except
    // memory it froze after writing it: g_config, which is read below.
    auto variables = m_heap.m_debugInfo->globalVariables([&](Address address) {
        auto region = Region::findContaining(regions, address);
        return region && region->isWritable();
    }, untyped);
    for (const SnapshotDebugInfo::GlobalVariable& variable : variables) {
        ++m_globalVariables;
        m_contextClass = nullptr;
        m_contextField = nullptr;
        TargetValue value = TargetValue::at(m_snapshot, variable.address, variable.type);
        if (std::holds_alternative<TargetType::Class>(variable.type.layout()))
            enqueue(value);
        else
            walk(value);
    }
    m_untypedDataSymbols = untyped;

    // g_config is an array of words, which WTF reads as its Config at
    // startOffsetOfWTFConfig (addressOfWTFConfig), and JavaScriptCore as its
    // own at the WTF Config's spaceForExtensions (addressOfJSCConfig).
    Address wtfConfig = *m_heap.m_config + WTF::startOffsetOfWTFConfig;
    enqueue(TargetValue::at(m_snapshot, wtfConfig, *m_heap.m_wtfConfigClass));
    auto* layout = std::get_if<TargetType::Class>(&m_heap.m_wtfConfigClass->layout());
    for (const TargetType::Field& field : layout ? layout->properFields : Vector<TargetType::Field> { }) {
        if (std::string_view { field.name.legacyCStringPointer() } == "spaceForExtensions")
            enqueue(TargetValue::at(m_snapshot, wtfConfig + field.offset, *m_heap.m_jscConfigClass));
    }
}

// The words of each thread's stack in use, from its stack pointer up, as
// conservative roots, as the collector scans a stack: a word inside an
// allocation reaches it, and an allocation that starts with a polymorphic
// object is read as its dynamic type. A thread whose stack pointer is not
// known has its whole stack scanned.
void ReachWalk::walkStacks()
{
    constexpr size_t wordsPerRead = 64 * 1024;
    for (const Thread& thread : m_snapshot.threads()) {
        if (!thread.hasStack())
            continue;
        Address start = thread.stackPointer() && thread.stackRegion().contains(thread.stackPointer()) ? thread.stackPointer() : thread.stackRegion().base();
        start = Address { roundUpToMultipleOf<sizeof(uint64_t)>(start.toTargetVMAddress()) };
        Address end = thread.stackRegion().end();
        while (start < end) {
            size_t count = std::min<uint64_t>(wordsPerRead, (end - start) / sizeof(uint64_t));
            if (!count)
                break;
            auto words = m_snapshot.memory().span<uint64_t>(start, count);
            start = start + count * sizeof(uint64_t);
            if (!words)
                continue;
            for (uint64_t word : std::span<const uint64_t> { words }) {
                auto index = allocationOf(Address { word }, 1);
                if (!index || m_reached[*index])
                    continue;
                m_reached[*index] = true;
                Address allocation = m_allocations[*index].address;
                Address completeObject;
                const TargetType* dynamicType = m_heap.m_debugInfo->dynamicTypeIfAnyAt(m_snapshot, allocation, completeObject);
                if (dynamicType && completeObject == allocation) {
                    m_contextClass = nullptr;
                    m_contextField = nullptr;
                    enqueue(TargetValue::at(m_snapshot, allocation, *dynamicType), IsObject::Yes);
                }
            }
        }
    }
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
        // An integer leads nowhere, and a large table of them is common in static data.
        if (std::holds_alternative<TargetType::Integer>(array->element.layout()) || std::holds_alternative<TargetType::Other>(array->element.layout()))
            return;
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
    if (walkByName(value))
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
        m_contextClass = &baseValue.type();
        m_contextField = nullptr;
        if (!walkByName(baseValue))
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
    // A pointee that runs past the end of its allocation is followed, and
    // listed as an overrun: the walk read the object as a type bigger than it is.
    auto index = allocationOf(address, 1);
    if (!index)
        return;
    m_reached[*index] = true;
    enqueue(TargetValue::at(m_snapshot, address, type), IsObject::Yes);
}

// Readers for the classes the debug info cannot describe, picked once per type
// by its qualified name.
auto ReachWalk::readerFor(const TargetType& type) -> Reader
{
    return m_readers.ensure(&type, [&] {
        auto qualifiedName = type.name();
        return readerNamed(std::string_view { qualifiedName.legacyCStringPointer() });
    }).iterator->value;
}

auto ReachWalk::readerNamed(std::string_view name) -> Reader
{
    if (name.starts_with("WTF::Vector<"))
        return Reader::Vector;
    if (name.starts_with("WTF::HashTable<"))
        return Reader::HashTable;
    if (name.starts_with("WTF::RobinHoodHashTable<"))
        return Reader::RobinHoodHashTable;
    if (name.starts_with("WTF::TrailingArray<"))
        return Reader::TrailingArray;
    if (name.starts_with("WTF::ButterflyArray<"))
        return Reader::ButterflyArray;
    if (name.starts_with("WTF::SegmentedVector<"))
        return Reader::SegmentedVector;
    if (name.starts_with("WTF::ConcurrentBuffer<") && name.ends_with(">::Array"))
        return Reader::ConcurrentBufferArray;
    if (name == "JSC::SymbolTableEntry")
        return Reader::SymbolTableEntry;
    if (name == "JSC::CodeBlock")
        return Reader::CodeBlock;
    if (name.starts_with("WTF::AlignedStorage<"))
        return Reader::AlignedStorage;
    if (name.starts_with("WTF::CodePtr<"))
        return Reader::CodePointer;
    if (name.starts_with("WTF::LazyUniqueRef<") || name.starts_with("WTF::LazyRef<"))
        return Reader::LazyPointer;
    if (name.starts_with("WTF::CompactPtr<"))
        return Reader::CompactPointer;
    if (name.starts_with("WTF::PackedAlignedPtr<"))
        return Reader::PackedPointer;
    if (name == "JSC::JSString")
        return Reader::JSString;
    if (name == "JSC::PropertyTable")
        return Reader::PropertyTable;
    if (name.starts_with("std::optional<") || name.starts_with("std::__1::optional<"))
        return Reader::Optional;
    return Reader::None;
}

bool ReachWalk::walkByName(const TargetValue& value)
{
    switch (readerFor(value.type())) {
    case Reader::None:
        return false;
    case Reader::Vector:
        walkVector(value);
        return true;
    case Reader::HashTable:
        walkHashTable(value);
        return true;
    case Reader::RobinHoodHashTable:
        walkRobinHoodHashTable(value);
        return true;
    case Reader::TrailingArray:
        walkTrailingArray(value);
        return true;
    case Reader::ButterflyArray:
        walkButterflyArray(value);
        return true;
    case Reader::SegmentedVector:
        walkSegmentedVector(value);
        return true;
    case Reader::ConcurrentBufferArray:
        walkConcurrentBufferArray(value);
        return true;
    case Reader::SymbolTableEntry:
        walkSymbolTableEntry(value);
        return true;
    case Reader::CodeBlock:
        walkCodeBlock(value);
        return true;
    case Reader::AlignedStorage:
        walkAlignedStorage(value);
        return true;
    case Reader::CodePointer:
        walkCodePointer(value);
        return true;
    case Reader::LazyPointer:
        // LazyRef::lazyTag and initializingTag: a pointer to the function that will make the object.
        walkTaggedPointer(value, "m_pointer", 1, 0x3);
        return true;
    case Reader::CompactPointer:
        walkCompactPointer(value);
        return true;
    case Reader::PackedPointer:
        walkPackedPointer(value);
        return true;
    case Reader::JSString:
        walkJSString(value);
        return true;
    case Reader::PropertyTable:
        walkPropertyTable(value);
        return true;
    case Reader::Optional:
        walkOptional(value);
        return true;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// Whether a std::optional holds a value: libstdc++'s
// _Optional_payload_base::_M_engaged or libc++'s
// __optional_destruct_base::__engaged_, a few bases and members down.
static std::optional<bool> isEngagedOptional(const TargetValue& value, unsigned depth = 0)
{
    auto* klass = std::get_if<TargetType::Class>(&value.type().layout());
    if (!klass || depth > 4)
        return std::nullopt;
    for (const TargetType::Field& field : klass->properFields) {
        std::string_view name { field.name.legacyCStringPointer() };
        if (name == "_M_engaged" || name == "__engaged_") {
            auto engaged = value.field(field).integer();
            if (!engaged)
                return std::nullopt;
            return !!*engaged;
        }
    }
    for (const TargetType::Field& field : klass->properFields) {
        if (auto engaged = isEngagedOptional(value.field(field), depth + 1))
            return engaged;
    }
    for (const TargetType::Base& base : klass->bases) {
        if (auto engaged = isEngagedOptional(value.base(base), depth + 1))
            return engaged;
    }
    return std::nullopt;
}

// A std::optional's value only when it holds one. An empty optional's storage
// holds whatever was there before.
void ReachWalk::walkOptional(const TargetValue& optional)
{
    auto engaged = isEngagedOptional(optional);
    if (!engaged) {
        CORPSE_REPORT("The std::optional at 0x%llx has no engaged flag this walk knows", forReport(optional.address()));
        return;
    }
    if (*engaged)
        walkMembers(optional, IsComplete::Yes);
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

// Every bucket, empty, deleted or live, as the bucket's type. An empty bucket
// holds its traits' empty value, and, in an ENABLE(MYA_HEAP) build, a deleted
// one holds its deleted key and zeros (hashTraitsDeleteBucket), so no bucket
// holds a stale address, and the table's traits are not needed.
void ReachWalk::walkHashTable(const TargetValue& table)
{
    walkMembers(table, IsComplete::Yes, "m_table");
    auto buckets = hashTableBuckets(table);
    if (!buckets || !buckets->size)
        return;
    auto address = buckets->table.pointerValue();
    if (!address)
        return;
    if (auto index = allocationOf(*address, 1))
        m_reached[*index] = true;
    const TargetType& bucket = std::get<TargetType::Pointer>(buckets->table.type().layout()).pointee;
    if (!bucket.byteSize()) {
        notFollowed(NotFollowed::Declaration);
        return;
    }
    for (unsigned index = 0; index < buckets->size; ++index)
        enqueue(TargetValue::at(m_snapshot, *address + static_cast<uint64_t>(index) * bucket.byteSize(), bucket), IsObject::Yes);
}

void ReachWalk::walkElements(Address address, const TargetType& element, uint64_t count)
{
    if (!count)
        return;
    if (!element.byteSize()) {
        notFollowed(NotFollowed::Declaration);
        return;
    }
    for (uint64_t index = 0; index < count; ++index)
        enqueue(TargetValue::at(m_snapshot, address + index * element.byteSize(), element), IsObject::Yes);
}

// Where `base` is in an object of class `derived`, among its non-virtual bases.
static std::optional<size_t> baseOffset(const TargetType& derived, const TargetType& base, unsigned depth = 0)
{
    if (&derived == &base)
        return 0;
    auto* klass = std::get_if<TargetType::Class>(&derived.layout());
    if (!klass || depth > 32)
        return std::nullopt;
    for (const TargetType::Base& candidate : klass->bases) {
        if (auto offset = baseOffset(candidate.type, base, depth + 1))
            return candidate.offset + *offset;
    }
    return std::nullopt;
}

// SegmentedVector::addressAt: the first InlineCapacity elements in
// m_inlineStorageMember, then the segments of m_segments, sizeOfSegment(i)
// elements each, m_size in all. It has no other members.
void ReachWalk::walkSegmentedVector(const TargetValue& vector)
{
    const TargetType* element = vector.type().templateArgument(0);
    auto segmentSize = vector.type().templateIntegerArgument(1);
    auto inlineCapacity = vector.type().templateIntegerArgument(2);
    auto growthPolicy = vector.type().templateIntegerArgument(3);
    auto size = vector.properField("m_size").integer();
    if (!element || !segmentSize || !*segmentSize || !inlineCapacity || !growthPolicy || !size) {
        notFollowed(NotFollowed::TypeNotReached);
        return;
    }
    uint64_t remaining = static_cast<uint64_t>(*size);
    if (*inlineCapacity) {
        // InlineStorageData::m_data, AlignedStorage<T>s, whose slots past m_size hold no element.
        uint64_t count = std::min(remaining, *inlineCapacity);
        walkElements(vector.properField("m_inlineStorageMember").address(), *element, count);
        remaining -= count;
    }
    auto storage = vectorStorage(vector.properField("m_segments"));
    auto segments = storage ? storage->buffer.pointerValue() : std::nullopt;
    if (!segments || !*segments)
        return;
    if (auto index = allocationOf(*segments, 1))
        m_reached[*index] = true;
    bool doubling = *growthPolicy == static_cast<uint64_t>(SegmentedVectorGrowthPolicy::Doubling);
    for (size_t segmentIndex = 0; segmentIndex < storage->size && remaining; ++segmentIndex) {
        // SegmentedVector::sizeOfSegment.
        uint64_t capacity = doubling ? *segmentSize << std::min<size_t>(segmentIndex, 48) : *segmentSize;
        // A SegmentPtr is a unique_ptr with an empty deleter: the segment's address.
        auto segment = m_snapshot.memory().ptr<uint64_t>(*segments + segmentIndex * sizeof(uint64_t));
        if (!segment || !*segment)
            return;
        if (auto index = allocationOf(Address { *segment }, 1))
            m_reached[*index] = true;
        uint64_t count = std::min(capacity, remaining);
        walkElements(Address { *segment }, *element, count);
        remaining -= count;
    }
}

// ConcurrentBuffer::Array: `size` elements in `data`, which is declared with one.
void ReachWalk::walkConcurrentBufferArray(const TargetValue& array)
{
    auto size = array.properField("size").integer();
    TargetValue data = array.properField("data");
    auto* elements = data ? std::get_if<TargetType::Array>(&data.type().layout()) : nullptr;
    if (!size || !elements)
        return;
    if (*size < 0 || static_cast<uint64_t>(*size) > maxVectorSize) {
        CORPSE_REPORT("The ConcurrentBuffer array at 0x%llx claims %lld elements", forReport(array.address()), static_cast<long long>(*size));
        return;
    }
    walkElements(data.address(), elements->element, static_cast<uint64_t>(*size));
}

// SymbolTableEntry::m_bits: a FatEntry*, unless SymbolTableEntry::SlimFlag is set (SymbolTableEntry::isFat).
void ReachWalk::walkSymbolTableEntry(const TargetValue& entry)
{
    constexpr uint64_t slimFlag = 1; // SymbolTableEntry::SlimFlag, which is private.
    auto bits = entry.properField("m_bits").integer();
    if (!bits || !*bits || (static_cast<uint64_t>(*bits) & slimFlag))
        return;
    follow(Address { static_cast<uint64_t>(*bits) }, *m_heap.m_fatEntryClass);
}

// RobinHoodHashTable: every bucket of m_table, m_tableSize of them. It has no
// deleted buckets: a removal shifts the buckets after it back, and an empty
// bucket holds its traits' empty value.
void ReachWalk::walkRobinHoodHashTable(const TargetValue& table)
{
    walkMembers(table, IsComplete::Yes, "m_table");
    TargetValue buckets = table.properField("m_table");
    auto address = buckets.pointerValue();
    auto size = table.properField("m_tableSize").integer();
    if (!address || !*address || !size)
        return;
    if (*size < 0 || static_cast<uint64_t>(*size) > maxHashTableSize) {
        CORPSE_REPORT("The RobinHoodHashTable at 0x%llx claims %lld buckets", forReport(table.address()), static_cast<long long>(*size));
        return;
    }
    if (auto index = allocationOf(*address, 1))
        m_reached[*index] = true;
    walkElements(*address, std::get<TargetType::Pointer>(buckets.type().layout()).pointee, static_cast<uint64_t>(*size));
}

// TrailingArray<Derived, T>, a base of Derived: m_size elements of T after the
// Derived object, at TrailingArray::offsetOfData().
void ReachWalk::walkTrailingArray(const TargetValue& array)
{
    walkMembers(array, IsComplete::No);
    const TargetType* derived = array.type().templateArgument(0);
    const TargetType* element = array.type().templateArgument(1);
    auto size = array.properField("m_size").integer();
    if (!derived || !element || !element->alignment()) {
        notFollowed(NotFollowed::TypeNotReached);
        return;
    }
    if (!size || *size < 0 || static_cast<uint64_t>(*size) > maxVectorSize)
        return;
    auto offset = baseOffset(*derived, array.type());
    if (!offset) {
        notFollowed(NotFollowed::TypeNotReached);
        return;
    }
    Address data = array.address() - *offset + roundUpToMultipleOf(element->alignment(), derived->byteSize());
    walkElements(data, *element, static_cast<uint64_t>(*size));
}

// ButterflyArray<Derived, LeadingType, TrailingType>, a base of Derived:
// m_leadingSize elements of LeadingType just before the Derived object, and
// m_trailingSize of TrailingType after it, at offsetOfTrailingData().
void ReachWalk::walkButterflyArray(const TargetValue& array)
{
    walkMembers(array, IsComplete::No);
    const TargetType* derived = array.type().templateArgument(0);
    const TargetType* leading = array.type().templateArgument(1);
    const TargetType* trailing = array.type().templateArgument(2);
    auto leadingSize = array.properField("m_leadingSize").integer();
    auto trailingSize = array.properField("m_trailingSize").integer();
    if (!derived || !leading || !trailing || !trailing->alignment()) {
        notFollowed(NotFollowed::TypeNotReached);
        return;
    }
    auto offset = baseOffset(*derived, array.type());
    if (!offset) {
        notFollowed(NotFollowed::TypeNotReached);
        return;
    }
    if (!leadingSize || !trailingSize || *leadingSize < 0 || *trailingSize < 0 || static_cast<uint64_t>(*leadingSize) > maxVectorSize || static_cast<uint64_t>(*trailingSize) > maxVectorSize)
        return;
    Address object = array.address() - *offset;
    walkElements(object - static_cast<uint64_t>(*leadingSize) * leading->byteSize(), *leading, static_cast<uint64_t>(*leadingSize));
    walkElements(object + roundUpToMultipleOf(trailing->alignment(), derived->byteSize()), *trailing, static_cast<uint64_t>(*trailingSize));
}

// CodeBlock::m_jitData, a void*: CodeBlock::baselineJITData() or dfgJITData(),
// by whether the JITCode's type is an optimizing tier.
void ReachWalk::walkCodeBlock(const TargetValue& codeBlock)
{
    walkMembers(codeBlock, IsComplete::No, "m_jitData");
    auto jitData = codeBlock.properField("m_jitData").pointerValue();
    if (!jitData || !*jitData)
        return;
    TargetValue jitCode = codeBlock.properField("m_jitCode").properField("m_ptr").dereference();
    auto jitType = jitCode.properField("m_jitType").integer();
    if (!jitType)
        return;
    const TargetType* type = JITCode::isOptimizingJIT(static_cast<JITType>(*jitType)) ? m_heap.m_dfgJITDataClass : m_heap.m_baselineJITDataClass;
    if (!type) {
        notFollowed(NotFollowed::TypeNotReached);
        return;
    }
    follow(*jitData, *type);
}

// AlignedStorage::get(), which NeverDestroyed and LazyNeverDestroyed hold their
// object in: m_storage, as the first template argument. A LazyNeverDestroyed
// not yet constructed is zeros, which lead nowhere.
void ReachWalk::walkAlignedStorage(const TargetValue& storage)
{
    TargetValue bytes = storage.properField("m_storage");
    const TargetType* type = storage.type().templateArgument(0);
    if (!bytes)
        return;
    if (!type || !type->byteSize()) {
        notFollowed(NotFollowed::TypeNotReached);
        return;
    }
    TargetValue value = TargetValue::at(m_snapshot, bytes.address(), *type);
    if (std::holds_alternative<TargetType::Class>(type->layout()))
        enqueue(value, IsObject::Yes);
    else
        walk(value);
}

// CodePtr::m_value: a tagged pointer into JIT code, which has no type. It
// reaches the allocation it is in, whose bytes stay untyped.
void ReachWalk::walkCodePointer(const TargetValue& codePointer)
{
    auto value = codePointer.properField("m_value").pointerValue();
    if (!value || !*value)
        return;
    if (auto index = allocationOf(*value, 1))
        m_reached[*index] = true;
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
    follow(Address { static_cast<uint64_t>(*fiber) }, *m_heap.m_stringImplClass);
}

void ReachWalk::summarize(const Vector<HeapWalk::Allocation>& excluded, size_t listCount, HeapWalk::Reach& result)
{
    result.notFollowed = m_notFollowed;
    result.cellsWithClass = m_cellsWithClass;
    result.cellBytesBeyondClass = m_cellBytesBeyondClass;
    result.globalVariables = m_globalVariables;
    result.untypedDataSymbols = m_untypedDataSymbols;
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
        if (!m_reached[index])
            continue;
        result.bytesReached += allocation.size;
        if (accounted > result.bytesTypedIn[index])
            untyped.append({ allocation, accounted - result.bytesTypedIn[index], m_typeAtStart[index] ? makeString(m_typeAtStart[index]->name()) : String() });
    }
    std::ranges::sort(untyped, std::ranges::greater { }, &HeapWalk::Untyped::bytesUntyped);
    untyped.shrink(std::min(untyped.size(), listCount));
    result.mostUntyped = WTF::move(untyped);
}

// The deepest field of `type` that covers `offset`, as "Class::field".
static String fieldAt(const TargetType& type, uint64_t offset)
{
    const TargetType* current = &type;
    String result;
    for (unsigned depth = 0; depth < 64; ++depth) {
        if (auto* array = std::get_if<TargetType::Array>(&current->layout())) {
            if (!array->element.byteSize())
                break;
            offset %= array->element.byteSize();
            current = &array->element;
            continue;
        }
        auto* klass = std::get_if<TargetType::Class>(&current->layout());
        if (!klass)
            break;
        const TargetType* next = nullptr;
        for (const TargetType::Field& field : klass->properFields) {
            if (!field.bitSize && offset >= field.offset && offset < field.offset + field.type.byteSize()) {
                result = makeString(current->name(), "::"_s, field.name);
                offset -= field.offset;
                next = &field.type;
                break;
            }
        }
        for (size_t index = 0; !next && index < klass->bases.size(); ++index) {
            const TargetType::Base& base = klass->bases[index];
            if (offset >= base.offset && offset < base.offset + base.type.byteSize()) {
                offset -= base.offset;
                next = &base.type;
            }
        }
        if (!next)
            break;
        current = next;
    }
    return result;
}

String ReachWalk::holderInReached(Address word, size_t allocationIndex) const
{
    const HeapWalk::Allocation& allocation = m_allocations[allocationIndex];
    uint64_t address = word.toTargetVMAddress();
    // The innermost value the walk read that covers the word: the last to start at or before it.
    auto ranges = m_typed.span();
    auto after = std::ranges::upper_bound(ranges, address, { }, &Range::begin);
    for (auto range = after; range != ranges.begin();) {
        --range;
        if (range->end <= address)
            continue;
        if (range->begin < allocation.address.toTargetVMAddress())
            break;
        String field = fieldAt(*range->type, address - range->begin);
        if (!field.isNull())
            return makeString("the field "_s, field, ", which the walk does not follow, at offset "_s, address - range->begin, " of a '"_s, range->type->name(), "' at 0x"_s, hex(range->begin));
        return makeString("offset "_s, address - range->begin, " of a '"_s, range->type->name(), "' at 0x"_s, hex(range->begin), ", in no field of it"_s);
    }
    return makeString("offset "_s, word - allocation.address, " of the "_s, allocation.size, "-byte allocation at 0x"_s, hex(allocation.address.toTargetVMAddress()),
        ", in no value the walk read; it read "_s, m_typeAtStart[allocationIndex] ? makeString('\'', m_typeAtStart[allocationIndex]->name(), "' at its start"_s) : "nothing at its start"_s);
}

auto ReachWalk::holderOf(Address word) -> Holder
{
    using MissCause = HeapWalk::MissCause;
    if (auto index = allocationOf(word, sizeof(uint64_t))) {
        if (m_reached[*index])
            return { MissCause::Reached, holderInReached(word, *index), std::nullopt };
        return { MissCause::Missed, makeString("the missed "_s, m_allocations[*index].size, "-byte allocation at 0x"_s, hex(m_allocations[*index].address.toTargetVMAddress())), *index };
    }
    String symbol = m_heap.m_debugInfo->symbolAt(word);
    if (!symbol.isNull())
        return { MissCause::StaticData, makeString("the symbol "_s, symbol), std::nullopt };
    auto region = Region::findContaining(m_snapshot.regions(), word);
    String name = region ? region->name() : String();
    return { MissCause::OtherMemory, makeString("0x"_s, hex(word.toTargetVMAddress()), " in "_s, name.isEmpty() ? "anonymous memory"_s : name), std::nullopt };
}

// Every word of the snapshot's readable memory whose value lies inside a missed
// allocation is a referrer of it; the best of them says what holds it.
void ReachWalk::explainMisses(const Vector<HeapWalk::Allocation>& excluded, const Vector<HeapWalk::Allocation>& notReferrers, HeapWalk::Reach& result)
{
    using MissCause = HeapWalk::MissCause;
    Vector<size_t> missed;
    for (size_t index = 0; index < m_allocations.size(); ++index) {
        if (!m_reached[index])
            missed.append(index);
    }
    if (missed.isEmpty())
        return;
    CORPSE_DIAGNOSTICS(diagnostics, "explaining the %zu allocations the walk missed", missed.size());
    uint64_t lowest = m_allocations[missed.first()].address.toTargetVMAddress();
    uint64_t highest = (m_allocations[missed.last()].address + m_allocations[missed.last()].size).toTargetVMAddress();

    // For each allocation, the best holder found: the first cause MissCause lists.
    Vector<std::optional<Holder>> holders(m_allocations.size());
    constexpr size_t maxReferrers = 8;
    Vector<Vector<Address>> referrers(m_allocations.size());
    const Vector<Thread>& threads = m_snapshot.threads();
    auto ranges = notReferrers.span();
    auto isNotReferrer = [&](Address word) {
        auto next = std::ranges::upper_bound(ranges, word, { }, [](const HeapWalk::Allocation& range) { return range.address + range.size; });
        if (next != ranges.end() && next->address <= word)
            return true;
        // A word below a thread's stack pointer is left over from a frame that has returned.
        for (const Thread& thread : threads) {
            if (thread.hasStack() && thread.stackPointer() && thread.stackRegion().contains(word) && word < thread.stackPointer())
                return true;
        }
        return false;
    };
    auto consider = [&](Address word, uint64_t value) {
        if (value < lowest || value >= highest)
            return;
        auto index = allocationOf(Address { value }, 1);
        // A word of the allocation itself does not hold it.
        if (!index || m_reached[*index] || allocationOf(word, 1) == index || isNotReferrer(word))
            return;
        if (referrers[*index].size() < maxReferrers)
            referrers[*index].append(word);
        std::optional<Holder>& best = holders[*index];
        if (best && best->cause == MissCause::Reached)
            return;
        Holder holder = holderOf(word);
        if (!best || holder.cause < best->cause)
            best = WTF::move(holder);
    };

    constexpr size_t wordsPerRead = 64 * 1024;
    for (const Region& region : m_snapshot.regions()) {
        if (!region.isReadable())
            continue;
        for (auto [start, size] : region.residentParts(m_snapshot.corpsePort())) {
            for (uint64_t offset = 0; offset + sizeof(uint64_t) <= size;) {
                size_t count = std::min<uint64_t>(wordsPerRead, (size - offset) / sizeof(uint64_t));
                Address chunk = start + offset;
                offset += count * sizeof(uint64_t);
                auto words = m_snapshot.memory().span<uint64_t>(chunk, count);
                if (!words)
                    continue;
                std::span<const uint64_t> values { words };
                for (size_t index = 0; index < values.size(); ++index)
                    consider(chunk + index * sizeof(uint64_t), values[index]);
            }
        }
    }

    auto excludedRanges = excluded.span();
    auto isExcluded = [&](const HeapWalk::Allocation& allocation) {
        auto next = std::ranges::upper_bound(excludedRanges, allocation.address, { }, [](const HeapWalk::Allocation& range) { return range.address + range.size; });
        return next != excludedRanges.end() && next->address < allocation.address + allocation.size;
    };
    Vector<size_t> missIndexOf(m_allocations.size());
    for (size_t index : missed) {
        HeapWalk::Miss miss { m_allocations[index], MissCause::NoReferrer, "nothing"_s, { }, { }, isExcluded(m_allocations[index]), m_allocations[index].size };
        Address completeObject;
        if (auto* dynamicType = m_heap.m_debugInfo->dynamicTypeIfAnyAt(m_snapshot, m_allocations[index].address, completeObject))
            miss.content = makeString(dynamicType->name());
        miss.referrers = WTF::move(referrers[index]);
        if (holders[index]) {
            miss.cause = holders[index]->cause;
            miss.holder = holders[index]->description;
        }
        missIndexOf[index] = result.misses.size();
        result.misses.append(WTF::move(miss));
        if (!result.misses.last().isExcluded)
            result.bytesMissedByCause[static_cast<size_t>(result.misses.last().cause)] += m_allocations[index].size;
    }
    // A miss only other misses hold is counted in what the first miss up its chain that something else holds holds.
    for (size_t index : missed) {
        size_t current = index;
        for (unsigned depth = 0; depth < missed.size(); ++depth) {
            const std::optional<Holder>& holder = holders[current];
            if (!holder || holder->cause != MissCause::Missed || *holder->missedHolder == index)
                break;
            current = *holder->missedHolder;
        }
        if (current != index)
            result.misses[missIndexOf[current]].bytesHeld += m_allocations[index].size;
    }
    std::ranges::sort(result.misses, std::ranges::greater { }, &HeapWalk::Miss::bytesHeld);
}

HeapWalk::Reach HeapWalk::reach(const Vector<Allocation>& allocations, const Vector<Allocation>& excluded, const Vector<Allocation>& notReferrers, size_t listCount) const
{
    Reach result;
    if (!isValid())
        return result;
    CORPSE_DIAGNOSTICS(diagnostics, "measuring the reach of the walk of the heap at 0x%llx", forReport(m_vm.address()));
    // The walk reads many small values, many to a page.
    Memory& memory = m_roots->snapshot().memory();
    memory.keepRecentMappings(4096);
    ReachWalk walk(*this, allocations);
    walk.run();
    walk.summarize(excluded, listCount, result);
    walk.explainMisses(excluded, notReferrers, result);
    memory.keepRecentMappings(0);
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
    Remote<ClassInfo> classInfo = classInfoOf(address);
    if (!classInfo)
        return nullptr;
    uint64_t key = classInfo.address().toTargetVMAddress();
    if (auto klass = cellClasses().getOptional(key))
        return *klass;
    if (m_reportedClassInfos.add(key).isNewEntry) {
        String symbol = m_debugInfo->symbolAt(classInfo.address());
        CORPSE_REPORT("The ClassInfo at 0x%llx, '%s', is not the s_info of a class in mya's list of cell classes", forReport(classInfo.address()), symbol.isNull() ? "no symbol" : symbol.utf8().legacyCStringPointer());
    }
    return nullptr;
}

// Each class of mya's list, by name in JavaScriptCore's image, and its s_info
// by its linkage name there, checked against the ClassInfo's size and parent.
const HashMap<uint64_t, const TargetType*>& HeapWalk::cellClasses() const
{
    if (m_cellClasses)
        return *m_cellClasses;
    CORPSE_DIAGNOSTICS(diagnostics, "finding the classes of mya's list of cell classes");
    HashMap<uint64_t, const TargetType*> classes;
    auto add = [&](const char* name) {
        // A class not built on this platform finds nothing.
        const TargetType* klass = m_debugInfo->classNamed(m_javaScriptCoreImage, name);
        auto classInfo = klass ? m_debugInfo->staticMemberAddress(m_javaScriptCoreImage, *klass, "s_info") : std::nullopt;
        if (!classInfo)
            return;
        if (!classes.add(classInfo->toTargetVMAddress(), klass).isNewEntry)
            CORPSE_REPORT("Two classes of mya's list of cell classes have the s_info at 0x%llx", forReport(*classInfo));
    };
#define CORPSE_ADD_CELL_CLASS(name) add(name);
    FOR_EACH_JSC_CELL_CLASS(CORPSE_ADD_CELL_CLASS)
#undef CORPSE_ADD_CELL_CLASS

    Vector<uint64_t> refused;
    for (auto& [address, klass] : classes) {
        Remote<ClassInfo> classInfo = Remote<Structure>(TargetValue { *m_structure }).field<const ClassInfo*>("m_classInfo").pointeeAt(Address { address });
        if (!isClassInfoOf(classInfo, *klass, classes))
            refused.append(address);
    }
    for (uint64_t address : refused)
        classes.remove(address);
    m_cellClasses = WTF::move(classes);
    return *m_cellClasses;
}

// Whether `classInfo` is the ClassInfo of `klass`: ClassInfo::staticClassSize
// is sizeof the class it was made for, and ClassInfo::parentClass is the
// ClassInfo of one of its bases. Reported if not.
bool HeapWalk::isClassInfoOf(const Remote<ClassInfo>& classInfo, const TargetType& klass, const HashMap<uint64_t, const TargetType*>& classes) const
{
    auto size = classInfo.field<unsigned>("staticClassSize").integer();
    if (!size) {
        CORPSE_REPORT("Could not read the class size of the ClassInfo at 0x%llx", forReport(classInfo.address()));
        return false;
    }
    if (static_cast<uint64_t>(*size) != klass.byteSize()) {
        CORPSE_REPORT("The ClassInfo at 0x%llx is for %lld-byte classes, but its '%s' is %zu bytes", forReport(classInfo.address()), static_cast<long long>(*size), klass.name().legacyCStringPointer(), klass.byteSize());
        return false;
    }
    auto parent = classInfo.field<const ClassInfo*>("parentClass").pointerValue();
    if (!parent)
        return false;
    if (!*parent)
        return true;
    auto parentClass = classes.getOptional(parent->toTargetVMAddress());
    if (!parentClass) {
        CORPSE_REPORT("The parent of the ClassInfo of '%s' at 0x%llx is not the s_info of a class in mya's list", klass.name().legacyCStringPointer(), forReport(classInfo.address()));
        return false;
    }
    Vector<const TargetType*, 8> bases { &klass };
    for (size_t index = 0; index < bases.size() && index < maxClassInfoDepth * 4; ++index) {
        if (bases[index] == *parentClass)
            return true;
        if (auto* layout = std::get_if<TargetType::Class>(&bases[index]->layout())) {
            for (const TargetType::Base& base : layout->bases)
                bases.append(&base.type);
        }
    }
    CORPSE_REPORT("The parent of the ClassInfo of '%s' at 0x%llx is for '%s', which is not one of its bases", klass.name().legacyCStringPointer(), forReport(classInfo.address()), (*parentClass)->name().legacyCStringPointer());
    return false;
}

Vector<String> HeapWalk::unlistedCellClasses() const
{
    Vector<String> result;
    if (!isValid())
        return result;
    const auto& classes = cellClasses();
    // "6s_infoE" ends the linkage name of every X::s_info.
    for (const SnapshotDebugInfo::Symbol& symbol : m_debugInfo->dataSymbolsEndingWith(m_javaScriptCoreImage, "6s_infoE")) {
        if (!classes.contains(symbol.address.toTargetVMAddress()))
            result.append(symbol.name);
    }
    return result;
}

size_t HeapWalk::listedCellClassCount() const
{
    return isValid() ? cellClasses().size() : 0;
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
