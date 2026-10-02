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
#include <JavaScriptCore/GCSegmentedArray.h>
#include <JavaScriptCore/JITCode.h>
#include <JavaScriptCore/JSCellButterfly.h>
#include <JavaScriptCore/JSString.h>
#include <JavaScriptCore/MarkedBlock.h>
#include <JavaScriptCore/MarkedSpace.h>
#include <JavaScriptCore/PreciseAllocation.h>
#include <JavaScriptCore/StrongBlock.h>
#include <JavaScriptCore/StructureID.h>
#include <JavaScriptCore/WeakBlock.h>
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

#if ENABLE(MYA_HEAP)
WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN
#include <bmalloc/pas_enumerator.h>
#include <bmalloc/pas_root.h>
WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
#endif

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
    : m_snapshot(&snapshot)
{
    m_debugInfo = SnapshotDebugInfo::create(snapshot);
    if (!m_debugInfo)
        return;
    auto roots = TargetValue::completeObjectAt(snapshot, *m_debugInfo, rootsAddress);
    if (!roots)
        return;
    m_roots = *roots;
    Remote<VM> vm = Remote<VM*>(roots->properField("vm")).dereference();
    if (!vm) {
        CORPSE_REPORT("The roots at 0x%llx name no VM", forReport(rootsAddress));
        return;
    }
    auto atSafePoint = roots->properField("atSafePoint").integer();
    if (!atSafePoint)
        return;
    initialize(snapshot, WTF::move(vm), atSafePoint);
}

HeapWalk::HeapWalk(Snapshot& snapshot, Address vmAddress, Address javaScriptCoreImage)
    : m_snapshot(&snapshot)
{
    m_debugInfo = SnapshotDebugInfo::create(snapshot);
    if (!m_debugInfo)
        return;
    const TargetType* vmClass = m_debugInfo->classNamed(javaScriptCoreImage, "JSC::VM");
    if (!vmClass) {
        CORPSE_REPORT("The image at 0x%llx has no class 'JSC::VM'", forReport(javaScriptCoreImage));
        return;
    }
    initialize(snapshot, Remote<VM>(TargetValue::at(snapshot, vmAddress, *vmClass)), 0);
}

void HeapWalk::initialize(Snapshot&, Remote<VM>&& vm, std::optional<int64_t> atSafePoint)
{
    // StructureID::decode adds g_jscConfig.startOfStructureHeap. The Structure
    // of every Structure is the VM's structureStructure, so its StructureID
    // names itself, and its address less its ID is that start.
    Remote<Structure> structureStructure = vm.field<void>("structureStructure").base<void>(0).field<Structure*>("m_cell").dereference();
    Remote<JSCell> cell = structureStructure.base<JSCell>(0);
    auto bits = cell.field<void>("m_structureID").field<uint32_t>("m_bits").integer();
    if (!bits)
        return;
    if (*bits <= 0 || static_cast<uint64_t>(*bits) > structureStructure.address().toTargetVMAddress()) {
        CORPSE_REPORT("The structureStructure at 0x%llx has StructureID %lld", forReport(structureStructure.address()), static_cast<long long>(*bits));
        return;
    }
    m_structure = *structureStructure.typed();
    m_jsCell = *cell.typed();
    m_startOfStructureHeap = structureStructure.address().toTargetVMAddress() - static_cast<uint64_t>(*bits);
    m_structureIDOffset = cell.field<void>("m_structureID").field<uint32_t>("m_bits").address() - cell.address();
    Remote<const ClassInfo*> classInfoField = structureStructure.field<const ClassInfo*>("m_classInfo");
    if (!classInfoField)
        return;
    m_classInfoOffset = classInfoField.address() - structureStructure.address();
    Remote<uint8_t> inlineCapacityField = structureStructure.field<uint8_t>("m_inlineCapacity");
    if (!inlineCapacityField)
        return;
    m_inlineCapacityOffset = inlineCapacityField.address() - structureStructure.address();

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
    m_jsValueClass = classNamed("JSC::JSValue");
    m_finalObjectClass = classNamed("JSC::JSFinalObject");
    m_preciseAllocationClass = classNamed("JSC::PreciseAllocation");
    m_ropeStringClass = classNamed("JSC::JSRopeString");
    m_cellButterflyClass = classNamed("JSC::JSCellButterfly");
    m_lexicalEnvironmentClass = classNamed("JSC::JSLexicalEnvironment");
    m_stringImplOwnerClass = classNamed("JSC::JSString");
    m_watchpointSetClass = classNamed("JSC::WatchpointSet");
    m_propertyTableEntryClass = classNamed("JSC::PropertyTableEntry");
    m_compactPropertyTableEntryClass = classNamed("JSC::CompactPropertyTableEntry");
    m_weakImplClass = classNamed("JSC::WeakImpl");
    m_expressionInfoChapterClass = classNamed("JSC::ExpressionInfo::Chapter");
    m_expressionInfoEncodedInfoClass = classNamed("JSC::ExpressionInfo::EncodedInfo");
    // The pointee of WTF::StringImpl's m_data8.
    if (auto* stringImpl = m_stringImplClass ? std::get_if<TargetType::Class>(&m_stringImplClass->layout()) : nullptr) {
        for (const TargetType::Base& base : stringImpl->bases) {
            auto* shape = std::get_if<TargetType::Class>(&base.type.layout());
            for (const TargetType::Field& field : shape ? shape->properFields : Vector<TargetType::Field> { }) {
                auto* members = std::get_if<TargetType::Class>(&field.type.layout());
                for (const TargetType::Field& member : members && field.name.isEmpty() ? members->properFields : Vector<TargetType::Field> { }) {
                    auto* pointer = std::get_if<TargetType::Pointer>(&member.type.layout());
                    if (pointer && std::string_view { member.name.legacyCStringPointer() } == "m_data8")
                        m_byteType = &pointer->pointee;
                }
            }
        }
    }
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
    if (!m_blockHeaderClass || !m_localAllocatorClass || !m_stringImplClass || !m_fatEntryClass || !m_wtfConfigClass || !m_jscConfigClass || !m_jsValueClass || !m_finalObjectClass || !m_preciseAllocationClass || !m_ropeStringClass || !m_cellButterflyClass || !m_lexicalEnvironmentClass || !m_stringImplOwnerClass || !m_watchpointSetClass || !m_propertyTableEntryClass || !m_compactPropertyTableEntryClass || !m_weakImplClass
        || !m_expressionInfoChapterClass || !m_expressionInfoEncodedInfoClass || !m_byteType || !m_config || !atSafePoint)
        return;
    m_isAtSafePoint = *atSafePoint;
    m_vm = WTF::move(vm);
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
    return Remote<MarkedBlock::Header>(TargetValue::at(snapshot(), block + MarkedBlock::headerAtom * MarkedBlock::atomSize, *m_blockHeaderClass));
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
            Remote<LocalAllocator> allocator { TargetValue::at(snapshot(), node.address(), *m_localAllocatorClass) };
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

auto HeapWalk::preciseAllocations() const -> Vector<PreciseAllocationState>
{
    Vector<PreciseAllocationState> result;
    if (!isValid())
        return result;
    using Allocations = Vector<PreciseAllocation*>;
    Remote<Allocations> allocations = m_vm.field<Heap>("heap").field<MarkedSpace>("m_objectSpace").field<Allocations>("m_preciseAllocations");
    auto size = RemoteTraits<Allocations>::size(allocations);
    for (size_t index = 0; size && index < *size; ++index) {
        Remote<PreciseAllocation> allocation = RemoteTraits<Allocations>::element(allocations, index).dereference();
        auto isMarked = allocation.field<void>("m_isMarked").as<bool>();
        auto isNewlyAllocated = allocation.field<bool>("m_isNewlyAllocated").integer();
        auto cellSize = allocation.field<void>("m_cellSize").integer();
        if (!allocation || !isMarked || !isNewlyAllocated || !cellSize)
            continue;
        size_t headerSize = roundUpToMultipleOf(PreciseAllocation::alignment, allocation.type()->byteSize());
        result.append({ allocation.address(), allocation.address() + headerSize, static_cast<uint64_t>(*cellSize), *isMarked || *isNewlyAllocated });
    }
    return result;
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
        Cell cell { allocation.address() + headerSize, static_cast<uint64_t>(*cellSize), static_cast<HeapCell::Kind>(*kind), allocation.address() };
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
        , m_snapshot(heap.snapshot())
        , m_allocations(allocations)
    {
        m_reached.fill(false, allocations.size());
        m_typeAtStart.fill(nullptr, allocations.size());
        m_reachedBy.fill({ nullptr, nullptr, nullptr }, allocations.size());
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
    // Records what reached an allocation first, which the untyped report names.
    void markReached(size_t index)
    {
        if (m_reached[index])
            return;
        m_reached[index] = true;
        m_reachedBy[index] = { m_contextClass, m_contextField, m_rootKind };
    }
    String reachedBy(size_t allocationIndex) const;
    std::optional<uint32_t> lexicalEnvironmentScopeSize(Address, const TargetType& environmentClass);
    std::optional<std::pair<uint64_t, uint64_t>> m_scopeOffsets; // Of the SymbolTable in an environment, and of its maxScopeOffset.
    // The live auxiliary cell `address` lies in, if any.
    const HeapWalk::Cell* auxiliaryCellContaining(Address) const;

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
    void walkInPlace(Address, const TargetType&, const TargetType* owner, const TargetType::Field*);

    struct Slot {
        uint64_t offset;
        enum class Kind : uint8_t { Pointer, Reader, Array } kind;
        const TargetType* type; // The pointee, or the value a reader or the array walk reads.
        const TargetType* owner; // The class whose field it is, which an overrun names.
        const TargetType::Field* field;
    };
    using Plan = Vector<Slot>;
    const Plan& planFor(const TargetType&, IsComplete, const char* except);
    void buildPlan(Plan&, const TargetType&, uint64_t offset, IsComplete, const char* except, unsigned depth);
    void addToPlan(Plan&, const TargetType&, uint64_t offset, const TargetType& owner, const TargetType::Field*, unsigned depth);
    void walkPlan(Address, const Plan&);
    void walkGlobalVariables();
    void walkStacks();
    void scanConservatively(Address start, Address end);
    void walkBlock(Address);
    void walkCell(const HeapWalk::Cell&);

    // The pointee of the pointer `pointer`, of static type `pointee`, if it is
    // in an allocation: as its dynamic type if it has one.
    void followPointer(Address pointer, const TargetType& pointee);
    void follow(Address, const TargetType&);
    void followUntyped(Address);

    // Readers for values whose pointers the debug info cannot describe, picked
    // by the qualified name of a class the walk has reached. True if `value` was
    // one, and has been read.
    bool walkByName(const TargetValue&);
    enum class Reader : uint8_t { None, Vector, HashTable, RobinHoodHashTable, TrailingArray, ButterflyArray, SegmentedVector, ConcurrentBufferArray, SymbolTableEntry, CodeBlock, AlignedStorage, CodePointer, LazyPointer, CompactPointer, PackedPointer, JSString, PropertyTable, Optional, StringImpl, ObjectButterfly, StrongBlock, InlineWatchpointSet, ArraySegment, WeakBlock, ExpressionInfo, ArrayBufferView, ArrayBufferContents, Variant, HasOwnPropertyCache, ImmutableStyleProperties, StyleProperties, InlineMap };
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
    bool leadsNowhere(const TargetType&);
    void walkCodePointer(const TargetValue&);
    void walkTaggedPointer(const TargetValue&, const char* field, unsigned templateArgument, uint64_t tagMask);
    void walkCompactPointer(const TargetValue&);
    void walkPackedPointer(const TargetValue&);
    void walkJSString(const TargetValue&);
    void walkPropertyTable(const TargetValue&);
    void walkOptional(const TargetValue&);
    void walkStringImpl(const TargetValue&);
    void walkObjectButterfly(const TargetValue&);
    void walkStrongBlock(const TargetValue&);
    void walkInlineWatchpointSet(const TargetValue&);
    void walkArraySegment(const TargetValue&);
    void walkWeakBlock(const TargetValue&);
    void walkExpressionInfo(const TargetValue&);
    void walkArrayBufferView(const TargetValue&);
    void walkArrayBufferContents(const TargetValue&);
    void walkVariant(const TargetValue&);
    // SnapshotDebugInfo::classNamedBeside, once per neighbor and name.
    const TargetType* classBeside(const TargetType& neighbor, const char* name)
    {
        return m_classesBeside.ensure({ &neighbor, name }, [&] {
            return m_heap.m_debugInfo->classNamedBeside(neighbor, name);
        }).iterator->value;
    }
    HashMap<std::pair<const TargetType*, const char*>, const TargetType*> m_classesBeside;
    void walkHasOwnPropertyCache(const TargetValue&);
    void walkImmutableStyleProperties(const TargetValue&);
    void walkStyleProperties(const TargetValue&);
    void walkInlineMap(const TargetValue&);
    // The pointer at `field`'s address in `value`, read whole: a CagedPtr holds its full address.
    std::optional<Address> rawPointer(const TargetValue&, const char* field);

    // The class and field being walked, which an overrun names.
    String context() const;

    const HeapWalk& m_heap;
    Snapshot& m_snapshot;
    const Vector<HeapWalk::Allocation>& m_allocations;
    Vector<bool> m_reached;
    // Each object is walked once as each type it is reached as.
    HashSet<std::pair<uint64_t, uint64_t>> m_visited;
    HashMap<const TargetType*, Reader> m_readers;
    // For complete objects and for base subobjects, by type and the member left out.
    std::array<HashMap<std::pair<const TargetType*, const char*>, std::unique_ptr<Plan>>, 2> m_plans;
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
    struct ReachedBy {
        const TargetType* klass;
        const TargetType::Field* field;
        const char* root; // What the walk was reading from, if it was in no class.
    };
    Vector<ReachedBy> m_reachedBy; // For each allocation.
    const char* m_rootKind { "the roots" };
    Vector<HeapWalk::Cell> m_auxiliaryCells; // Sorted.
    HashMap<const TargetType*, uint64_t> m_bytesBeyondClass;
    // Where StringImpl keeps its length, flags and characters, by StringImpl's type.
    struct StringImplLayout {
        uint64_t lengthOffset;
        uint64_t flagsOffset;
        uint64_t dataOffset; // Of m_data8, which shares its storage with m_data16.
        const TargetType* latin1; // The pointee of m_data8.
        const TargetType* utf16; // The pointee of m_data16.
    };
    HashMap<const TargetType*, std::optional<StringImplLayout>> m_stringImplLayouts;
    HashMap<const TargetType*, std::optional<uint64_t>> m_butterflyOffsets; // Of AuxiliaryBarrier::m_value, by the object's type.
    // A MarkedBlock or a precise allocation is one allocation, whose memory outside its live cells is the JS heap's free memory.
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
    Address m_contextSlot; // The word that holds the pointer being followed, when the walk knows it.
    std::pair<Address, const TargetType*> m_contextObject { }; // The value whose members are being walked.
    Vector<String> m_clippedArrays;
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

// The field named `name` of `type` or of a non-virtual base, looking into
// unnamed members, such as an anonymous union. `offset` is where the class that
// declares it is in `type`.
static const TargetType::Field* findField(const TargetType& type, std::string_view name, uint64_t& offset, const TargetType** declaringClass = nullptr, unsigned depth = 0)
{
    auto* klass = std::get_if<TargetType::Class>(&type.layout());
    if (!klass || depth > 8)
        return nullptr;
    for (const TargetType::Field& field : klass->properFields) {
        std::string_view fieldName { field.name.legacyCStringPointer() };
        if (fieldName == name) {
            if (declaringClass)
                *declaringClass = &type;
            return &field;
        }
        if (fieldName.empty()) {
            uint64_t inner = 0;
            if (auto* found = findField(field.type, name, inner, declaringClass, depth + 1)) {
                offset += field.offset + inner;
                return found;
            }
        }
    }
    for (const TargetType::Base& base : klass->bases) {
        uint64_t inner = 0;
        if (auto* found = findField(base.type, name, inner, declaringClass, depth + 1)) {
            offset += base.offset + inner;
            return found;
        }
    }
    return nullptr;
}

void ReachWalk::run()
{
    auto drain = [&] {
        while (!m_worklist.isEmpty())
            walk(m_worklist.takeLast());
    };
    // The roots' VM is the test's description of it. The cells reach the VM as
    // JavaScriptCore describes it, with every type it owns complete.
    m_rootKind = m_heap.m_roots ? "the roots" : "the VM";
    if (m_heap.m_roots)
        walkMembers(*m_heap.m_roots, IsComplete::Yes, "vm");
    else if (auto* vm = m_heap.m_vm.typed())
        follow(vm->address(), vm->type());
    drain();
    walkGlobalVariables();
    drain();
    m_rootKind = "a thread's stack";
    walkStacks();
    drain();
    // A precise allocation holds its header and, while it is live, its cell;
    // the cell of one that is dead, and not yet swept, is the JS heap's free memory.
    m_rootKind = "a PreciseAllocation";
    for (const HeapWalk::PreciseAllocationState& precise : m_heap.preciseAllocations()) {
        auto index = allocationOf(precise.header, 1);
        if (!index)
            continue;
        markReached(*index);
        m_isBlock[*index] = true;
        m_liveBytesInBlock[*index] += (precise.cell - precise.header) + (precise.isLive ? precise.cellSize : 0);
        m_contextClass = nullptr;
        m_contextField = nullptr;
        enqueue(TargetValue::at(m_snapshot, precise.header, *m_heap.m_preciseAllocationClass), IsObject::Yes);
        drain();
    }
    m_rootKind = "a MarkedBlock";
    m_heap.forEachBlock([&](Address block) {
        walkBlock(block);
        drain();
        return IterationStatus::Continue;
    });
    Vector<HeapWalk::Cell> cells;
    m_heap.forEachLiveCell([&](const HeapWalk::Cell& cell) {
        cells.append(cell);
        if (!isJSCellKind(cell.kind))
            m_auxiliaryCells.append(cell);
        return IterationStatus::Continue;
    });
    std::ranges::sort(m_auxiliaryCells, { }, &HeapWalk::Cell::address);
    m_rootKind = "a live cell";
    for (const HeapWalk::Cell& cell : cells) {
        walkCell(cell);
        drain();
    }
}

const HeapWalk::Cell* ReachWalk::auxiliaryCellContaining(Address address) const
{
    auto cells = m_auxiliaryCells.span();
    auto after = std::ranges::upper_bound(cells, address, { }, &HeapWalk::Cell::address);
    if (after == cells.begin() || address - (after - 1)->address >= (after - 1)->size)
        return nullptr;
    return &*(after - 1);
}

// SymbolTable::scopeSize() of the lexical environment's JSSymbolTableObject::m_symbolTable: its maxScopeOffset plus one.
std::optional<uint32_t> ReachWalk::lexicalEnvironmentScopeSize(Address environment, const TargetType& klass)
{
    if (!m_scopeOffsets) {
        m_scopeOffsets = std::pair<uint64_t, uint64_t> { 0, 0 };
        uint64_t holder = 0;
        auto* symbolTable = findField(klass, "m_symbolTable", holder);
        uint64_t cellOffset = 0;
        auto* cellField = symbolTable ? findField(symbolTable->type, "m_cell", cellOffset) : nullptr;
        auto* pointer = cellField ? std::get_if<TargetType::Pointer>(&cellField->type.layout()) : nullptr;
        uint64_t maxScopeHolder = 0;
        auto* maxScopeOffset = pointer ? findField(pointer->pointee, "m_maxScopeOffset", maxScopeHolder) : nullptr;
        uint64_t offsetHolder = 0;
        auto* offset = maxScopeOffset ? findField(maxScopeOffset->type, "m_offset", offsetHolder) : nullptr;
        if (!offset)
            return std::nullopt;
        m_scopeOffsets = std::pair<uint64_t, uint64_t> { holder + symbolTable->offset + cellOffset + cellField->offset, maxScopeHolder + maxScopeOffset->offset + offsetHolder + offset->offset };
    }
    if (!m_scopeOffsets->first)
        return std::nullopt;
    auto table = m_snapshot.memory().ptr<uint64_t>(environment + m_scopeOffsets->first);
    if (!table || !*table)
        return std::nullopt;
    auto maxScopeOffset = m_snapshot.memory().ptr<uint32_t>(Address { *table }.stripped() + m_scopeOffsets->second);
    if (!maxScopeOffset)
        return std::nullopt;
    // ScopeOffset's invalid offset is UINT_MAX, so the sum is the scope size either way.
    return *maxScopeOffset + 1;
}

String ReachWalk::reachedBy(size_t allocationIndex) const
{
    auto [klass, field, root] = m_reachedBy[allocationIndex];
    if (!klass)
        return String::fromLatin1(root ? root : "nothing");
    if (!field)
        return makeString("a '"_s, klass->name(), '\'');
    return makeString(klass->name(), "::"_s, field->name);
}

// Every global variable of every image with debug info, as its own image
// describes it, including function-local statics.
void ReachWalk::walkGlobalVariables()
{
    size_t untyped = 0;
    Vector<SnapshotDebugInfo::Range> merged;
    const Vector<Region>& regions = m_snapshot.regions();
    // Memory the target cannot write holds no address it allocated, except
    // memory it froze after writing it: g_config, which is read below.
    auto variables = m_heap.m_debugInfo->globalVariables([&](Address address) {
        auto region = Region::findContaining(regions, address);
        return region && region->isWritable();
    }, untyped, merged);
    m_rootKind = "a global variable";
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
    // The statics a compiler merged into one symbol, such as libpas's, are read as a stack is.
    m_rootKind = "a block of statics merged into one symbol";
    m_contextClass = nullptr;
    m_contextField = nullptr;
    for (const SnapshotDebugInfo::Range& range : merged)
        scanConservatively(range.address, range.address + std::min<uint64_t>(range.size, maxConservativeScanSize));

    // g_config is an array of words, which WTF reads as its Config at
    // startOffsetOfWTFConfig (addressOfWTFConfig), and JavaScriptCore as its
    // own at the WTF Config's spaceForExtensions (addressOfJSCConfig).
    m_rootKind = "WebConfig::g_config";
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
    for (const Thread& thread : m_snapshot.threads()) {
        if (!thread.hasStack())
            continue;
        Address start = thread.stackPointer() && thread.stackRegion().contains(thread.stackPointer()) ? thread.stackPointer() : thread.stackRegion().base();
        m_contextClass = nullptr;
        m_contextField = nullptr;
        scanConservatively(start, thread.stackRegion().end());
    }
}

// Every aligned word from `start` to `end` that points into an allocation reaches it.
void ReachWalk::scanConservatively(Address start, Address end)
{
    constexpr size_t wordsPerRead = 64 * 1024;
    start = Address { roundUpToMultipleOf<sizeof(uint64_t)>(start.toTargetVMAddress()) };
    while (start < end) {
        size_t count = std::min<uint64_t>(wordsPerRead, (end - start) / sizeof(uint64_t));
        if (!count)
            break;
        auto words = m_snapshot.memory().span<uint64_t>(start, count);
        start = start + count * sizeof(uint64_t);
        if (!words)
            continue;
        for (uint64_t word : std::span<const uint64_t> { words })
            followUntyped(Address { word }.stripped());
    }
}

void ReachWalk::walkBlock(Address block)
{
    Remote<MarkedBlock::Header> header = m_heap.header(block);
    auto index = allocationOf(block, MarkedBlock::blockSize);
    if (!index || !header.typed())
        return;
    m_isBlock[*index] = true;
    markReached(*index);
    m_liveBytesInBlock[*index] += header.type()->byteSize();
    m_contextClass = nullptr;
    m_contextField = nullptr;
    enqueue(*header.typed(), IsObject::Yes);
}

void ReachWalk::walkCell(const HeapWalk::Cell& cell)
{
    m_contextClass = nullptr;
    m_contextField = nullptr;
    auto index = allocationOf(cell.address, cell.size);
    if (index) {
        markReached(*index);
        // A precise allocation's live bytes were counted with its header.
        if (m_isBlock[*index] && !cell.preciseAllocation)
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
    uint64_t classSize = klass->byteSize();
    // JSFinalObject::allocationSize: its inline storage, as many JSValues as its Structure's inline capacity, follows the class.
    if (klass == m_heap.m_finalObjectClass) {
        auto bits = m_snapshot.memory().ptr<uint32_t>(cell.address + m_heap.m_structureIDOffset);
        auto inlineCapacity = bits ? m_snapshot.memory().ptr<uint8_t>(Address { m_heap.m_startOfStructureHeap + (*bits & ~StructureID::nukedStructureIDBit) } + m_heap.m_inlineCapacityOffset) : Memory::Ptr<uint8_t> { };
        if (inlineCapacity && *inlineCapacity) {
            uint64_t inlineBytes = std::min<uint64_t>(static_cast<uint64_t>(*inlineCapacity) * sizeof(EncodedJSValue), cell.size - std::min(cell.size, classSize));
            typed(cell.address + classSize, inlineBytes, *m_heap.m_jsValueClass);
            classSize += inlineBytes;
        }
    }
    // JSCellButterfly::allocationSize: its vector, of vectorLength JSValues, at offsetOfData().
    if (klass == m_heap.m_cellButterflyClass) {
        auto vectorLength = m_snapshot.memory().ptr<uint32_t>(cell.address + JSCellButterfly::offsetOfVectorLength());
        uint64_t data = JSCellButterfly::offsetOfData();
        if (vectorLength && data <= cell.size) {
            uint64_t bytes = std::min<uint64_t>(static_cast<uint64_t>(*vectorLength) * sizeof(EncodedJSValue), cell.size - data);
            typed(cell.address + data, bytes, *m_heap.m_jsValueClass);
            classSize = std::max(classSize, data + bytes);
        }
    }
    // JSLexicalEnvironment::allocationSize: its variables, of its SymbolTable's scopeSize() JSValues, at offsetOfVariables().
    if (klass == m_heap.m_lexicalEnvironmentClass) {
        if (auto scopeSize = lexicalEnvironmentScopeSize(cell.address, *klass)) {
            uint64_t variables = roundUpToMultipleOf<sizeof(EncodedJSValue)>(klass->byteSize());
            uint64_t bytes = std::min<uint64_t>(static_cast<uint64_t>(*scopeSize) * sizeof(EncodedJSValue), cell.size - std::min(cell.size, variables));
            typed(cell.address + variables, bytes, *m_heap.m_jsValueClass);
            classSize = std::max(classSize, variables + bytes);
        }
    }
    // A JSRopeString shares JSString's ClassInfo, but is allocated from its own
    // subspace, of its own size (JSRopeString::subspaceFor), and stays one once resolved.
    if (klass == m_heap.m_stringImplOwnerClass && cell.size >= m_heap.m_ropeStringClass->byteSize() && cell.size > klass->byteSize()) {
        klass = m_heap.m_ropeStringClass;
        classSize = std::max<uint64_t>(classSize, klass->byteSize());
    }
    // A variable-sized cell, or a subclass that shares its base's ClassInfo, is bigger than the class.
    if (cell.size > classSize) {
        m_cellBytesBeyondClass += cell.size - classSize;
        m_bytesBeyondClass.add(klass, 0).iterator->value += cell.size - classSize;
    }
    enqueue(TargetValue::at(m_snapshot, cell.address, *klass), IsObject::Yes);
}

void ReachWalk::walk(const TargetValue& value)
{
    walkInPlace(value.address(), value.type(), nullptr, nullptr);
}

void ReachWalk::walkClass(const TargetValue& value)
{
    m_contextClass = &value.type();
    m_contextField = nullptr;
    if (walkByName(value))
        return;
    walkMembers(value, IsComplete::Yes);
}

// A value that is part of the object being walked, at `address`: a class is
// walked through its plan or its reader, a pointer is followed, and an array's
// elements are walked in place.
void ReachWalk::walkInPlace(Address address, const TargetType& type, const TargetType* owner, const TargetType::Field* field)
{
    const TargetType::Layout& layout = type.layout();
    if (std::holds_alternative<TargetType::Class>(layout)) {
        m_contextClass = &type;
        m_contextField = nullptr;
        if (readerFor(type) != Reader::None) {
            walkByName(TargetValue::at(m_snapshot, address, type));
            return;
        }
        auto saved = std::exchange(m_contextObject, std::pair<Address, const TargetType*> { address, &type });
        walkPlan(address, planFor(type, IsComplete::Yes, nullptr));
        m_contextObject = saved;
        return;
    }
    if (auto* pointer = std::get_if<TargetType::Pointer>(&layout)) {
        auto word = m_snapshot.memory().ptr<uint64_t>(address);
        if (!word || !*word)
            return;
        m_contextClass = owner;
        m_contextField = field;
        m_contextSlot = address;
        followPointer(Address { *word }.stripped(), pointer->pointee);
        m_contextSlot = { };
        return;
    }
    if (auto* array = std::get_if<TargetType::Array>(&layout)) {
        // An integer leads nowhere, and a large table of them is common in static data.
        if (!array->count || !array->element.byteSize() || leadsNowhere(array->element))
            return;
        // An array the debug info declares larger than the readable memory it starts in, such as a
        // reservation, is read only as far as that memory goes.
        auto region = Region::findContaining(m_snapshot.regions(), address);
        if (!region || !region->isReadable())
            return;
        uint64_t count = std::min<uint64_t>(array->count, (region->end() - address) / array->element.byteSize());
        if (count < array->count) {
            m_clippedArrays.append(makeString("the "_s, array->count, "-element '"_s, type.name(), "' at 0x"_s, hex(address.toTargetVMAddress()),
                ", reached through "_s, context(), ", of which "_s, count, " are in readable memory"_s));
        }
        for (uint64_t index = 0; index < count; ++index)
            walkInPlace(address + index * array->element.byteSize(), array->element, owner, field);
    }
}

// TargetValue::forEachField, but with each base as a value of its own, which a
// reader may recognise, as a Packed<T*> is a PackedAlignedPtr.
void ReachWalk::walkMembers(const TargetValue& value, IsComplete isComplete, const char* except)
{
    auto saved = std::exchange(m_contextObject, std::pair<Address, const TargetType*> { value.address(), &value.type() });
    walkPlan(value.address(), planFor(value.type(), isComplete, except));
    m_contextObject = saved;
}

// The members of a class, flattened once per class into the values that may
// lead somewhere: pointers, values a reader reads, and arrays of either.
// Members that are integers, or classes of integers, lead nowhere.
auto ReachWalk::planFor(const TargetType& type, IsComplete isComplete, const char* except) -> const Plan&
{
    auto& plans = m_plans[isComplete == IsComplete::Yes];
    std::pair<const TargetType*, const char*> key { &type, except };
    if (auto* plan = plans.get(key))
        return *plan;
    auto plan = makeUnique<Plan>();
    buildPlan(*plan, type, 0, isComplete, except, 0);
    return *plans.add(key, WTF::move(plan)).iterator->value;
}

void ReachWalk::buildPlan(Plan& plan, const TargetType& type, uint64_t offset, IsComplete isComplete, const char* except, unsigned depth)
{
    auto* klass = std::get_if<TargetType::Class>(&type.layout());
    // A class nests its members a few deep; a cycle would be a liblldb bug.
    if (!klass || depth > 64)
        return;
    for (const TargetType::Field& field : klass->properFields) {
        // A field whose type liblldb could not parse has no size.
        if (field.bitSize || !field.type.byteSize() || (except && std::string_view { field.name.legacyCStringPointer() } == except))
            continue;
        addToPlan(plan, field.type, offset + field.offset, type, &field, depth);
    }
    auto addBase = [&](const TargetType::Base& base) {
        if (readerFor(base.type) != Reader::None)
            plan.append({ offset + base.offset, Slot::Kind::Reader, &base.type, &base.type, nullptr });
        else
            buildPlan(plan, base.type, offset + base.offset, IsComplete::No, nullptr, depth + 1);
    };
    for (const TargetType::Base& base : klass->bases)
        addBase(base);
    // A virtual base's offset is only known in a complete object.
    if (isComplete == IsComplete::Yes) {
        for (const TargetType::Base& base : klass->virtualBases)
            addBase(base);
    }
}

void ReachWalk::addToPlan(Plan& plan, const TargetType& type, uint64_t offset, const TargetType& owner, const TargetType::Field* field, unsigned depth)
{
    const TargetType::Layout& layout = type.layout();
    if (std::holds_alternative<TargetType::Class>(layout)) {
        if (readerFor(type) != Reader::None)
            plan.append({ offset, Slot::Kind::Reader, &type, &owner, field });
        else
            buildPlan(plan, type, offset, IsComplete::Yes, nullptr, depth + 1);
        return;
    }
    if (auto* pointer = std::get_if<TargetType::Pointer>(&layout)) {
        plan.append({ offset, Slot::Kind::Pointer, &pointer->pointee, &owner, field });
        return;
    }
    if (auto* array = std::get_if<TargetType::Array>(&layout)) {
        if (!array->count || !array->element.byteSize() || std::holds_alternative<TargetType::Integer>(array->element.layout()) || std::holds_alternative<TargetType::Other>(array->element.layout()))
            return;
        plan.append({ offset, Slot::Kind::Array, &type, &owner, field });
    }
}

void ReachWalk::walkPlan(Address address, const Plan& plan)
{
    if (plan.isEmpty())
        return;
    // An object's pointers are read in one mapping, which covers every one of them.
    uint64_t end = 0;
    for (const Slot& slot : plan) {
        if (slot.kind == Slot::Kind::Pointer)
            end = std::max<uint64_t>(end, slot.offset + sizeof(uint64_t));
    }
    auto bytes = end ? m_snapshot.memory().span<uint8_t>(address, static_cast<size_t>(end)) : Memory::Span<uint8_t> { };
    for (const Slot& slot : plan) {
        Address slotAddress = address + slot.offset;
        switch (slot.kind) {
        case Slot::Kind::Pointer: {
            uint64_t word = 0;
            if (bytes) {
                memcpySpan(asMutableByteSpan(word), std::span<const uint8_t> { bytes }.subspan(slot.offset, sizeof(word)));
            } else if (auto read = m_snapshot.memory().ptr<uint64_t>(slotAddress))
                word = *read;
            if (!word)
                break;
            m_contextClass = slot.owner;
            m_contextField = slot.field;
            m_contextSlot = slotAddress;
            followPointer(Address { word }.stripped(), *slot.type);
            m_contextSlot = { };
            break;
        }
        case Slot::Kind::Reader:
            m_contextClass = slot.type;
            m_contextField = nullptr;
            walkByName(TargetValue::at(m_snapshot, slotAddress, *slot.type));
            break;
        case Slot::Kind::Array:
            walkInPlace(slotAddress, *slot.type, slot.owner, slot.field);
            break;
        }
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
        followUntyped(address);
        return;
    }
    follow(address, pointee);
}

// A pointer whose type says nothing of its pointee, such as a void*, reaches
// the allocation it points into. An allocation that starts with a polymorphic
// object is read as its dynamic type, and any other stays untyped.
void ReachWalk::followUntyped(Address address)
{
    auto index = allocationOf(address, 1);
    if (!index || m_reached[*index])
        return;
    markReached(*index);
    Address allocation = m_allocations[*index].address;
    Address completeObject;
    const TargetType* dynamicType = m_heap.m_debugInfo->dynamicTypeIfAnyAt(m_snapshot, allocation, completeObject);
    if (dynamicType && completeObject == allocation)
        enqueue(TargetValue::at(m_snapshot, allocation, *dynamicType), IsObject::Yes);
}

void ReachWalk::follow(Address address, const TargetType& type)
{
    // Only into an allocation, so that a pointer into static data leads nowhere.
    auto index = allocationOf(address, 1);
    if (!index)
        return;
    // A pointee that runs past the end of its allocation is not an object of
    // its type, such as the other member of a union: it is listed as an
    // overrun, and neither reached nor read.
    const HeapWalk::Allocation& allocation = m_allocations[*index];
    if (type.byteSize() > (allocation.address + allocation.size) - address) {
        notFollowed(NotFollowed::DoesNotFit);
        m_overruns.append(makeString("the "_s, type.byteSize(), "-byte '"_s, type.name(), "' at 0x"_s, hex(address.toTargetVMAddress()),
            ", reached through "_s, context(), m_contextSlot ? makeString(" at 0x"_s, hex(m_contextSlot.toTargetVMAddress())) : String(),
            m_contextObject.second ? makeString(" in a '"_s, m_contextObject.second->name(), "' at 0x"_s, hex(m_contextObject.first.toTargetVMAddress())) : String(),
            ", runs past the end of its "_s, allocation.size, "-byte allocation at 0x"_s, hex(allocation.address.toTargetVMAddress())));
        return;
    }
    markReached(*index);
    // A pointer to an integer at the start of an allocation is a buffer of them, such as a malloc'ed string's characters.
    if (allocation.address == address && type.byteSize() && std::holds_alternative<TargetType::Integer>(type.layout())) {
        if (m_visited.add({ address.toTargetVMAddress(), std::bit_cast<uint64_t>(&type) }).isNewEntry)
            typed(address, allocation.size - allocation.size % type.byteSize(), type);
        return;
    }
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
    if (name == "WTF::StringImpl")
        return Reader::StringImpl;
    if (name == "JSC::JSObjectWithButterfly")
        return Reader::ObjectButterfly;
    if (name == "JSC::StrongBlock")
        return Reader::StrongBlock;
    if (name == "JSC::InlineWatchpointSet")
        return Reader::InlineWatchpointSet;
    if (name.starts_with("JSC::GCArraySegment<"))
        return Reader::ArraySegment;
    if (name == "JSC::WeakBlock")
        return Reader::WeakBlock;
    if (name == "JSC::ExpressionInfo")
        return Reader::ExpressionInfo;
    if (name == "JSC::JSArrayBufferView")
        return Reader::ArrayBufferView;
    if (name == "JSC::ArrayBufferContents")
        return Reader::ArrayBufferContents;
    if (name.starts_with("mpark::detail::base<"))
        return Reader::Variant;
    if (name == "JSC::HasOwnPropertyCache")
        return Reader::HasOwnPropertyCache;
    if (name == "WebCore::ImmutableStyleProperties")
        return Reader::ImmutableStyleProperties;
    if (name == "WebCore::StyleProperties")
        return Reader::StyleProperties;
    if (name.starts_with("WTF::InlineMap<"))
        return Reader::InlineMap;
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
    case Reader::StringImpl:
        walkStringImpl(value);
        return true;
    case Reader::ObjectButterfly:
        walkObjectButterfly(value);
        return true;
    case Reader::StrongBlock:
        walkStrongBlock(value);
        return true;
    case Reader::InlineWatchpointSet:
        walkInlineWatchpointSet(value);
        return true;
    case Reader::ArraySegment:
        walkArraySegment(value);
        return true;
    case Reader::WeakBlock:
        walkWeakBlock(value);
        return true;
    case Reader::ExpressionInfo:
        walkExpressionInfo(value);
        return true;
    case Reader::ArrayBufferView:
        walkArrayBufferView(value);
        return true;
    case Reader::ArrayBufferContents:
        walkArrayBufferContents(value);
        return true;
    case Reader::Variant:
        walkVariant(value);
        return true;
    case Reader::HasOwnPropertyCache:
        walkHasOwnPropertyCache(value);
        return true;
    case Reader::ImmutableStyleProperties:
        walkImmutableStyleProperties(value);
        return true;
    case Reader::StyleProperties:
        walkStyleProperties(value);
        return true;
    case Reader::InlineMap:
        walkInlineMap(value);
        return true;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// Whether a std::optional holds a value: libstdc++'s
// _Optional_payload_base::_M_engaged or libc++'s
// __optional_destruct_base::__engaged_, up to six bases and members down.
static std::optional<bool> isEngagedOptional(const TargetValue& value, unsigned depth = 0)
{
    auto* klass = std::get_if<TargetType::Class>(&value.type().layout());
    if (!klass || depth > 8)
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
        CORPSE_REPORT("The '%s' at 0x%llx has no engaged flag this walk knows", optional.type().name().legacyCStringPointer(), forReport(optional.address()));
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
        markReached(*index);
    const TargetType& element = std::get<TargetType::Pointer>(storage->buffer.type().layout()).pointee;
    if (!element.byteSize()) {
        if (storage->size)
            notFollowed(NotFollowed::Declaration);
        return;
    }
    walkElements(*address, element, storage->size);
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
        markReached(*index);
    const TargetType& bucket = std::get<TargetType::Pointer>(buckets->table.type().layout()).pointee;
    if (!bucket.byteSize()) {
        notFollowed(NotFollowed::Declaration);
        return;
    }
    walkElements(*address, bucket, buckets->size);
}

void ReachWalk::walkElements(Address address, const TargetType& element, uint64_t count)
{
    if (!count)
        return;
    if (!element.byteSize()) {
        notFollowed(NotFollowed::Declaration);
        return;
    }
    // Elements that lead nowhere, such as a buffer of integers, are read as one run.
    if (leadsNowhere(element)) {
        typed(address, count * element.byteSize(), element);
        return;
    }
    for (uint64_t index = 0; index < count; ++index)
        enqueue(TargetValue::at(m_snapshot, address + index * element.byteSize(), element), IsObject::Yes);
}

// Whether a value of `type` holds nothing the walk follows: no pointer, and nothing a reader reads.
bool ReachWalk::leadsNowhere(const TargetType& type)
{
    const TargetType::Layout& layout = type.layout();
    if (std::holds_alternative<TargetType::Integer>(layout) || std::holds_alternative<TargetType::Other>(layout))
        return true;
    if (auto* array = std::get_if<TargetType::Array>(&layout))
        return leadsNowhere(array->element);
    if (std::holds_alternative<TargetType::Class>(layout))
        return readerFor(type) == Reader::None && planFor(type, IsComplete::Yes, nullptr).isEmpty();
    return false;
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
        markReached(*index);
    bool doubling = *growthPolicy == static_cast<uint64_t>(SegmentedVectorGrowthPolicy::Doubling);
    for (size_t segmentIndex = 0; segmentIndex < storage->size && remaining; ++segmentIndex) {
        // SegmentedVector::sizeOfSegment.
        uint64_t capacity = doubling ? *segmentSize << std::min<size_t>(segmentIndex, 48) : *segmentSize;
        // A SegmentPtr is a unique_ptr with an empty deleter: the segment's address.
        auto segment = m_snapshot.memory().ptr<uint64_t>(*segments + segmentIndex * sizeof(uint64_t));
        if (!segment || !*segment)
            return;
        if (auto index = allocationOf(Address { *segment }, 1))
            markReached(*index);
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
        markReached(*index);
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
        markReached(*index);
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
    // PropertyTable::isCompactFlag, which is private.
    constexpr uint64_t isCompactFlag = 1;
    bool isCompact = static_cast<uint64_t>(*bits) & isCompactFlag;
    Address buffer { static_cast<uint64_t>(*bits) & ~isCompactFlag };
    if (auto index = allocationOf(buffer, 1))
        markReached(*index);
    // PropertyTable::dataSize, which is private: m_indexSize indices, then (m_indexSize >> 1) + 1 entries.
    auto indexSize = table.properField("m_indexSize").integer();
    const TargetType* entry = isCompact ? m_heap.m_compactPropertyTableEntryClass : m_heap.m_propertyTableEntryClass;
    const TargetType* indexType = isCompact ? m_heap.m_byteType : &table.properField("m_indexSize").type();
    if (!indexSize || *indexSize <= 0 || static_cast<uint64_t>(*indexSize) > maxHashTableSize || !entry || !indexType)
        return;
    uint64_t indexBytes = static_cast<uint64_t>(*indexSize) * indexType->byteSize();
    typed(buffer, indexBytes, *indexType);
    walkElements(buffer + indexBytes, *entry, (static_cast<uint64_t>(*indexSize) >> 1) + 1);
}

// GCArraySegment::data(): the slots after the segment, to blockSize, of which
// the GCSegmentedArray uses only the slots below its top; the others hold
// whatever they held, so the walk reads them as their type and follows none.
void ReachWalk::walkArraySegment(const TargetValue& segment)
{
    walkMembers(segment, IsComplete::Yes);
    const TargetType* element = segment.type().templateArgument(0);
    uint64_t headerSize = segment.type().byteSize();
    if (!element || !element->byteSize() || headerSize >= GCArraySegment<const JSCell*>::blockSize)
        return;
    typed(segment.address() + headerSize, (GCArraySegment<const JSCell*>::blockSize - headerSize) / element->byteSize() * element->byteSize(), *element);
}

// WeakBlock::weakImpls(): weakImplCount() WeakImpls after the block, to blockSize.
void ReachWalk::walkWeakBlock(const TargetValue& block)
{
    walkMembers(block, IsComplete::Yes);
    const TargetType* weakImpl = m_heap.m_weakImplClass;
    if (!weakImpl || !weakImpl->byteSize())
        return;
    uint64_t first = (block.type().byteSize() + weakImpl->byteSize() - 1) / weakImpl->byteSize();
    uint64_t capacity = WeakBlock::blockSize / weakImpl->byteSize();
    if (first < capacity)
        walkElements(block.address() + first * weakImpl->byteSize(), *weakImpl, capacity - first);
}

// ExpressionInfo::chapters() and encodedInfo(): m_numberOfChapters Chapters
// after the ExpressionInfo, then m_numberOfEncodedInfo and
// m_numberOfEncodedInfoExtensions EncodedInfos.
void ReachWalk::walkExpressionInfo(const TargetValue& info)
{
    walkMembers(info, IsComplete::Yes);
    auto chapters = info.properField("m_numberOfChapters").integer();
    auto encodedInfo = info.properField("m_numberOfEncodedInfo").integer();
    auto extensions = info.properField("m_numberOfEncodedInfoExtensions").integer();
    const TargetType* chapter = m_heap.m_expressionInfoChapterClass;
    const TargetType* encoded = m_heap.m_expressionInfoEncodedInfoClass;
    if (!chapters || !encodedInfo || !extensions || !chapter || !encoded || *chapters < 0 || *encodedInfo < 0 || *extensions < 0
        || static_cast<uint64_t>(*chapters) > maxVectorSize || static_cast<uint64_t>(*encodedInfo + *extensions) > maxVectorSize)
        return;
    Address start = info.address() + info.type().byteSize();
    walkElements(start, *chapter, static_cast<uint64_t>(*chapters));
    walkElements(start + static_cast<uint64_t>(*chapters) * chapter->byteSize(), *encoded, static_cast<uint64_t>(*encodedInfo + *extensions));
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

// StringImpl's characters: in the StringImpl itself, at tailOffset(), for a
// BufferInternal one whose m_data8 points there; in a buffer of their own, or a
// literal, which m_data8 points to, for the others; and in another StringImpl,
// which the tail points to, for BufferSubstring.
void ReachWalk::walkStringImpl(const TargetValue& string)
{
    walkMembers(string, IsComplete::Yes);
    // StringImpl's s_hashMaskBufferOwnership and s_hashFlag8BitBuffer, which are private.
    constexpr uint32_t bufferOwnershipMask = 3;
    constexpr uint32_t is8BitFlag = 1u << 2;
    const auto& layout = m_stringImplLayouts.ensure(&string.type(), [&]() -> std::optional<StringImplLayout> {
        uint64_t lengthOffset = 0;
        uint64_t flagsOffset = 0;
        uint64_t data8Offset = 0;
        uint64_t data16Offset = 0;
        auto* length = findField(string.type(), "m_length", lengthOffset);
        auto* flags = findField(string.type(), "m_hashAndFlags", flagsOffset);
        auto* data8 = findField(string.type(), "m_data8", data8Offset);
        auto* data16 = findField(string.type(), "m_data16", data16Offset);
        auto* latin1 = data8 ? std::get_if<TargetType::Pointer>(&data8->type.layout()) : nullptr;
        auto* utf16 = data16 ? std::get_if<TargetType::Pointer>(&data16->type.layout()) : nullptr;
        if (!length || !flags || !latin1 || !utf16 || !latin1->pointee.byteSize() || !utf16->pointee.byteSize())
            return std::nullopt;
        return StringImplLayout { lengthOffset + length->offset, flagsOffset + flags->offset, data8Offset + data8->offset, &latin1->pointee, &utf16->pointee };
    }).iterator->value;
    if (!layout)
        return;
    uint64_t lengthOffset = layout->lengthOffset;
    uint64_t flagsOffset = layout->flagsOffset;
    auto lengthValue = m_snapshot.memory().ptr<uint32_t>(string.address() + lengthOffset);
    auto flagsValue = m_snapshot.memory().ptr<uint32_t>(string.address() + flagsOffset);
    if (!lengthValue || !flagsValue)
        return;
    // StringImpl::tailOffset.
    uint64_t tail = flagsOffset + sizeof(uint32_t);
    switch (*flagsValue & bufferOwnershipMask) {
    case WTF::StringImpl::BufferInternal: {
        const TargetType& character = (*flagsValue & is8BitFlag) ? *layout->latin1 : *layout->utf16;
        tail = roundUpToMultipleOf(character.alignment(), tail);
        auto data = m_snapshot.memory().ptr<uint64_t>(string.address() + layout->dataOffset);
        if (data && Address { *data }.stripped() == string.address() + tail)
            typed(string.address() + tail, static_cast<uint64_t>(*lengthValue) * character.byteSize(), character);
        return;
    }
    case WTF::StringImpl::BufferSubstring:
        if (auto parent = m_snapshot.memory().ptr<uint64_t>(string.address() + roundUpToMultipleOf<alignof(void*)>(tail)); parent && *parent)
            follow(Address { *parent }.stripped(), string.type());
        return;
    default:
        return;
    }
}

// JSObjectWithButterfly::m_butterfly: out-of-line properties before the
// butterfly, and an indexing header and indexed properties from it, in one
// auxiliary cell, each word a JSValue, or a double in a double array.
void ReachWalk::walkObjectButterfly(const TargetValue& object)
{
    walkMembers(object, IsComplete::No, "m_butterfly");
    const auto& offset = m_butterflyOffsets.ensure(&object.type(), [&]() -> std::optional<uint64_t> {
        TargetValue pointer = object.properField("m_butterfly").properField("m_value");
        if (!pointer)
            return std::nullopt;
        return pointer.address() - object.address();
    }).iterator->value;
    auto word = offset ? m_snapshot.memory().ptr<uint64_t>(object.address() + *offset) : Memory::Ptr<uint64_t> { };
    if (!word || !*word)
        return;
    std::optional<Address> butterfly = Address { *word }.stripped();
    // A butterfly without indexed properties points just past its last out-of-line property.
    const HeapWalk::Cell* cell = auxiliaryCellContaining(*butterfly - 1);
    if (!cell)
        return;
    if (auto index = allocationOf(cell->address, 1))
        markReached(*index);
    typed(cell->address, cell->size - cell->size % sizeof(EncodedJSValue), *m_heap.m_jsValueClass);
}

// StrongBlock: its header, then a JSValue slot for each Strong handle, to the end of the block.
void ReachWalk::walkStrongBlock(const TargetValue& block)
{
    walkMembers(block, IsComplete::Yes);
    uint64_t headerSize = block.type().byteSize();
    if (headerSize < StrongBlock::blockSize)
        typed(block.address() + headerSize, StrongBlock::blockSize - headerSize, *m_heap.m_jsValueClass);
}

// InlineWatchpointSet::m_data: a fat WatchpointSet*, unless IsThinFlag is set (InlineWatchpointSet::isFat).
void ReachWalk::walkInlineWatchpointSet(const TargetValue& set)
{
    constexpr uint64_t isThinFlag = 1; // InlineWatchpointSet::IsThinFlag, which is private.
    auto bits = set.properField("m_data").integer();
    if (!bits || !*bits || (static_cast<uint64_t>(*bits) & isThinFlag))
        return;
    followPointer(Address { static_cast<uint64_t>(*bits) }.stripped(), *m_heap.m_watchpointSetClass);
}

std::optional<Address> ReachWalk::rawPointer(const TargetValue& value, const char* field)
{
    TargetValue member = value.properField(field);
    if (!member)
        return std::nullopt;
    auto word = m_snapshot.memory().ptr<uint64_t>(member.address());
    if (!word || !*word)
        return std::nullopt;
    return Address { *word }.stripped();
}

// JSArrayBufferView::m_vector, a CagedBarrierPtr<Gigacage::Primitive, void>: a
// FastTypedArray's elements, in an auxiliary cell of their own, which are
// bytes; or an ArrayBuffer's, which its ArrayBufferContents reads.
void ReachWalk::walkArrayBufferView(const TargetValue& view)
{
    walkMembers(view, IsComplete::No);
    auto vector = rawPointer(view, "m_vector");
    if (!vector)
        return;
    if (const HeapWalk::Cell* cell = auxiliaryCellContaining(*vector)) {
        if (auto index = allocationOf(cell->address, 1))
            markReached(*index);
        typed(cell->address, cell->size, *m_heap.m_byteType);
    }
}

// ArrayBufferContents::m_data, a CagedPtr<Gigacage::Primitive, void>: m_sizeInBytes bytes.
void ReachWalk::walkArrayBufferContents(const TargetValue& contents)
{
    walkMembers(contents, IsComplete::Yes);
    auto data = rawPointer(contents, "m_data");
    TargetValue sizeField = contents.properField("m_sizeInBytes");
    auto size = sizeField ? m_snapshot.memory().ptr<uint64_t>(sizeField.address()) : Memory::Ptr<uint64_t> { };
    if (!data || !size || !*size)
        return;
    auto index = allocationOf(*data, 1);
    if (!index)
        return;
    markReached(*index);
    const HeapWalk::Allocation& allocation = m_allocations[*index];
    typed(*data, std::min<uint64_t>(*size, (allocation.address + allocation.size) - *data), *m_heap.m_byteType);
}

// mpark::detail::base, which WTF::Variant is: the alternative index_ names, of
// the template argument of that index, in the bytes of data_. A valueless
// variant's index_ is all ones.
void ReachWalk::walkVariant(const TargetValue& variant)
{
    auto index = variant.properField("index_").integer();
    TargetValue data = variant.properField("data_");
    if (!index || !data || *index < 0)
        return;
    const TargetType* alternative = variant.type().templateArgument(static_cast<unsigned>(*index));
    if (!alternative) {
        if (*index != static_cast<int64_t>((1ull << (8 * variant.properField("index_").type().byteSize())) - 1))
            notFollowed(NotFollowed::TypeNotReached);
        return;
    }
    if (alternative->byteSize() > data.type().byteSize()) {
        CORPSE_REPORT("The %zu-byte alternative %lld of the variant at 0x%llx is bigger than its storage", alternative->byteSize(), static_cast<long long>(*index), forReport(variant.address()));
        return;
    }
    walkInPlace(data.address(), *alternative, &variant.type(), nullptr);
}

// HasOwnPropertyCache: HasOwnPropertyCache::size Entries, from its address; the class itself has no members.
void ReachWalk::walkHasOwnPropertyCache(const TargetValue& cache)
{
    constexpr uint64_t size = 2 * 1024; // HasOwnPropertyCache::size, which is private.
    if (auto* entry = classBeside(cache.type(), "JSC::HasOwnPropertyCache::Entry"))
        walkElements(cache.address(), *entry, size);
}

// ImmutableStyleProperties::metadataSpan() and valueSpan(): m_arraySize
// StylePropertyMetadatas at m_storage, then as many PackedPtr<const CSSValue>s.
void ReachWalk::walkImmutableStyleProperties(const TargetValue& properties)
{
    walkMembers(properties, IsComplete::Yes);
    uint64_t sizeOffset = 0;
    const TargetType* declaringClass = nullptr;
    auto* arraySize = findField(properties.type(), "m_arraySize", sizeOffset, &declaringClass);
    TargetValue storage = properties.properField("m_storage");
    auto* metadata = classBeside(properties.type(), "WebCore::StylePropertyMetadata");
    auto* value = classBeside(properties.type(), "WTF::Packed<const WebCore::CSSValue *>");
    if (!arraySize || !storage || !metadata || !value)
        return;
    auto count = TargetValue::at(m_snapshot, properties.address() + sizeOffset, *declaringClass).field(*arraySize).integer();
    if (!count || *count < 0 || static_cast<uint64_t>(*count) > maxVectorSize)
        return;
    walkElements(storage.address(), *metadata, static_cast<uint64_t>(*count));
    walkElements(storage.address() + static_cast<uint64_t>(*count) * metadata->byteSize(), *value, static_cast<uint64_t>(*count));
}

// StyleProperties has no vtable: m_isMutable says whether it is a
// MutableStyleProperties or an ImmutableStyleProperties, which it starts.
void ReachWalk::walkStyleProperties(const TargetValue& properties)
{
    walkMembers(properties, IsComplete::No);
    uint64_t offset = 0;
    const TargetType* declaringClass = nullptr;
    auto* isMutableField = findField(properties.type(), "m_isMutable", offset, &declaringClass);
    if (!isMutableField)
        return;
    auto isMutable = TargetValue::at(m_snapshot, properties.address() + offset, *declaringClass).field(*isMutableField).integer();
    if (!isMutable)
        return;
    if (auto* subclass = classBeside(properties.type(), *isMutable ? "WebCore::MutableStyleProperties" : "WebCore::ImmutableStyleProperties"))
        enqueue(TargetValue::at(m_snapshot, properties.address(), *subclass), IsObject::Yes);
}

// InlineMap::isInline(): with m_capacity at its InlineCapacity, m_size entries
// in m_storage's inlineEntries; otherwise m_capacity entries, empty, deleted or
// live, at m_storage's hashedData.entries.
void ReachWalk::walkInlineMap(const TargetValue& map)
{
    walkMembers(map, IsComplete::Yes, "m_storage");
    auto capacity = map.properField("m_capacity").integer();
    auto size = map.properField("m_size").integer();
    auto inlineCapacity = map.type().templateIntegerArgument(2);
    TargetValue storage = map.properField("m_storage");
    TargetValue entries = storage.properField("hashedData").properField("entries");
    auto* entry = entries ? std::get_if<TargetType::Pointer>(&entries.type().layout()) : nullptr;
    if (!capacity || !size || !inlineCapacity || !entry || *capacity < 0 || *size < 0 || static_cast<uint64_t>(*capacity) > maxHashTableSize) {
        notFollowed(NotFollowed::TypeNotReached);
        return;
    }
    if (static_cast<uint64_t>(*capacity) == *inlineCapacity) {
        walkElements(storage.address(), entry->pointee, std::min<uint64_t>(static_cast<uint64_t>(*size), *inlineCapacity));
        return;
    }
    auto address = entries.pointerValue();
    if (!address || !*address)
        return;
    if (auto index = allocationOf(*address, 1))
        markReached(*index);
    walkElements(*address, entry->pointee, static_cast<uint64_t>(*capacity));
}

void ReachWalk::summarize(const Vector<HeapWalk::Allocation>& excluded, size_t listCount, HeapWalk::Reach& result)
{
    result.notFollowed = m_notFollowed;
    result.cellsWithClass = m_cellsWithClass;
    result.cellBytesBeyondClass = m_cellBytesBeyondClass;
    result.globalVariables = m_globalVariables;
    result.untypedDataSymbols = m_untypedDataSymbols;
    result.overruns = WTF::move(m_overruns);
    result.clippedArrays = WTF::move(m_clippedArrays);
    for (auto& [klass, bytes] : m_bytesBeyondClass)
        result.cellBytesBeyondClassByClass.append({ makeString(klass->name()), bytes });
    std::ranges::sort(result.cellBytesBeyondClassByClass, std::ranges::greater { }, &std::pair<String, uint64_t>::second);
    result.cellBytesBeyondClassByClass.shrink(std::min<size_t>(result.cellBytesBeyondClassByClass.size(), listCount));
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
    HashMap<const TargetType*, uint64_t> untypedByType;
    HashMap<String, uint64_t> untypedByReacher;
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
        if (accounted > result.bytesTypedIn[index]) {
            uint64_t bytes = accounted - result.bytesTypedIn[index];
            untyped.append({ allocation, bytes, m_typeAtStart[index] ? makeString(m_typeAtStart[index]->name()) : String(), reachedBy(index) });
            if (m_typeAtStart[index])
                untypedByType.add(m_typeAtStart[index], 0).iterator->value += bytes;
            else
                untypedByReacher.add(untyped.last().reachedBy, 0).iterator->value += bytes;
        }
    }
    for (auto& [type, bytes] : untypedByType)
        result.untypedByKind.append({ makeString('\'', type->name(), "' at the start"_s), bytes });
    for (auto& [reacher, bytes] : untypedByReacher)
        result.untypedByKind.append({ makeString("nothing at the start, reached by "_s, reacher), bytes });
    std::ranges::sort(result.untypedByKind, std::ranges::greater { }, &std::pair<String, uint64_t>::second);
    result.untypedByKind.shrink(std::min(result.untypedByKind.size(), listCount));
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
        ", in no value the walk read; it read "_s, m_typeAtStart[allocationIndex] ? makeString('\'', m_typeAtStart[allocationIndex]->name(), "' at its start"_s) : "nothing at its start"_s,
        ", reached by "_s, reachedBy(allocationIndex));
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
    // Most words are no address, or the address of something reached, so a
    // word is first looked up among the misses alone.
    auto missedAllocations = missed.span();
    auto missedIndexOf = [&](uint64_t value) -> std::optional<size_t> {
        auto after = std::ranges::upper_bound(missedAllocations, value, { }, [&](size_t index) { return m_allocations[index].address.toTargetVMAddress(); });
        if (after == missedAllocations.begin())
            return std::nullopt;
        const HeapWalk::Allocation& allocation = m_allocations[*(after - 1)];
        if (value - allocation.address.toTargetVMAddress() >= allocation.size)
            return std::nullopt;
        return *(after - 1);
    };
    auto consider = [&](Address word, uint64_t value) {
        if (value < lowest || value >= highest)
            return;
        auto index = missedIndexOf(value);
        if (!index)
            return;
        // A word of the allocation itself does not hold it.
        const HeapWalk::Allocation& allocation = m_allocations[*index];
        if ((word >= allocation.address && word - allocation.address < allocation.size) || isNotReferrer(word))
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
    // Memory the target cannot write holds no address it allocated: a word
    // there that matches one, as in a constant table, holds nothing.
    for (const Region& region : m_snapshot.regions()) {
        if (!region.isReadable() || !region.isWritable())
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
    Memory& memory = snapshot().memory();
    memory.keepRecentMappings(4096);
    ReachWalk walk(*this, allocations);
    walk.run();
    walk.summarize(excluded, listCount, result);
    walk.explainMisses(excluded, notReferrers, result);
    memory.keepRecentMappings(0);
    return result;
}

#if ENABLE(MYA_HEAP)

namespace {

// pas_enumerator's reader: a local copy of the snapshot's bytes, valid until
// the next read, as libmalloc's memory_reader_t promises.
struct LibpasEnumeration {
    Memory& memory;
    Vector<uint8_t> copy;
    Vector<HeapWalk::Allocation> objects;
    bool failed { false };
};

void* readSnapshotMemory(pas_enumerator*, void* address, size_t size, void* argument)
{
    auto& enumeration = *static_cast<LibpasEnumeration*>(argument);
    auto bytes = enumeration.memory.span<uint8_t>(Address { address }, size);
    if (!bytes) {
        enumeration.failed = true;
        return nullptr;
    }
    enumeration.copy = Vector<uint8_t> { std::span<const uint8_t> { bytes } };
    return enumeration.copy.mutableSpan().data();
}

void recordLibpasObject(pas_enumerator*, void* address, size_t size, pas_enumerator_record_kind kind, void* argument)
{
    if (kind == pas_enumerator_object_record)
        static_cast<LibpasEnumeration*>(argument)->objects.append({ Address { address }, size });
}

} // anonymous namespace

auto HeapWalk::libpasAllocations() const -> std::optional<Vector<Allocation>>
{
    if (!isValid())
        return std::nullopt;
    CORPSE_DIAGNOSTICS(diagnostics, "enumerating libpas's heap from its root for libmalloc's enumeration");
    // libpas is linked into JavaScriptCore, so this process's libpas is the
    // target's, with the root's layout its enumerator reads.
    auto rootSymbol = m_debugInfo->symbolAddress(m_javaScriptCoreImage, "pas_root_for_libmalloc_enumeration");
    auto root = rootSymbol ? snapshot().memory().ptr<uint64_t>(*rootSymbol) : Memory::Ptr<uint64_t> { };
    if (!root || !*root) {
        CORPSE_REPORT("JavaScriptCore's image has no root for libmalloc's enumeration of libpas");
        return std::nullopt;
    }
    auto magic = snapshot().memory().ptr<uint64_t>(Address { *root });
    if (!magic || *magic != PAS_ROOT_MAGIC) {
        CORPSE_REPORT("The libpas root at 0x%llx is not one", static_cast<unsigned long long>(*root));
        return std::nullopt;
    }
    LibpasEnumeration enumeration { snapshot().memory(), { }, { } };
    pas_enumerator* enumerator = pas_enumerator_create(std::bit_cast<pas_root*>(static_cast<uintptr_t>(*root)), readSnapshotMemory, &enumeration, recordLibpasObject, &enumeration,
        pas_enumerator_do_not_record_meta_records, pas_enumerator_do_not_record_payload_records, pas_enumerator_record_object_records);
    bool enumerated = enumerator && pas_enumerator_enumerate_all(enumerator);
    if (enumerator)
        pas_enumerator_destroy(enumerator);
    if (!enumerated || enumeration.failed) {
        CORPSE_REPORT("libpas could not enumerate the heap of the root at 0x%llx", static_cast<unsigned long long>(*root));
        return std::nullopt;
    }
    std::ranges::sort(enumeration.objects, { }, &Allocation::address);
    return WTF::move(enumeration.objects);
}

#else // ENABLE(MYA_HEAP)

auto HeapWalk::libpasAllocations() const -> std::optional<Vector<Allocation>>
{
    CORPSE_REPORT("This build does not export libpas's enumerator: it is not an ENABLE(MYA_HEAP) build");
    return std::nullopt;
}

#endif // ENABLE(MYA_HEAP)

// A JS cell has no vtable, so its class comes from its Structure's ClassInfo,
// which is the s_info of the class it describes. Read at offsets found once,
// since every live cell is read this way.
std::optional<Address> HeapWalk::classInfoOf(Address address) const
{
    auto bits = snapshot().memory().ptr<uint32_t>(address + m_structureIDOffset);
    uint32_t structureIDBits = bits ? *bits & ~StructureID::nukedStructureIDBit : 0;
    if (!structureIDBits)
        return std::nullopt;
    auto classInfo = snapshot().memory().ptr<uint64_t>(Address { m_startOfStructureHeap + structureIDBits } + m_classInfoOffset);
    if (!classInfo || !*classInfo)
        return std::nullopt;
    return Address { *classInfo }.stripped();
}

const TargetType* HeapWalk::cellClass(Address address) const
{
    if (!isValid())
        return nullptr;
    auto classInfo = classInfoOf(address);
    if (!classInfo)
        return nullptr;
    return classOfClassInfo(*classInfo, 0);
}

// The class whose s_info the ClassInfo is, checked against the ClassInfo's
// size and parent. Null, having reported why once, if there is none.
const TargetType* HeapWalk::classOfClassInfo(Address address, unsigned depth) const
{
    uint64_t key = address.toTargetVMAddress();
    if (auto klass = m_cellClasses.getOptional(key))
        return *klass;
    if (depth > maxClassInfoDepth) {
        CORPSE_REPORT("The ClassInfo at 0x%llx has more than %u ancestors", forReport(address), maxClassInfoDepth);
        return nullptr;
    }
    Remote<ClassInfo> classInfo = Remote<Structure>(TargetValue { *m_structure }).field<const ClassInfo*>("m_classInfo").pointeeAt(address);
    // CREATE_METHOD_TABLE fills the method table with the class's own functions, or those it inherits.
    Vector<Address, 32> methods;
    if (auto* info = classInfo.typed()) {
        info->properField("methodTable").forEachField([&](const TargetType::Field&, const TargetValue& method) {
            if (auto function = method.pointerValue(); function && *function)
                methods.append(*function);
        });
    }
    const TargetType* klass = m_debugInfo->classOfStaticMember(address, "s_info", methods.span());
    if (klass && !isClassInfoOf(classInfo, *klass, depth))
        klass = nullptr;
    m_cellClasses.add(key, klass);
    return klass;
}

// Whether `classInfo` is the ClassInfo of `klass`: ClassInfo::staticClassSize
// is sizeof the class it was made for, and ClassInfo::parentClass is the
// ClassInfo of one of its bases. Reported if not.
bool HeapWalk::isClassInfoOf(const Remote<ClassInfo>& classInfo, const TargetType& klass, unsigned depth) const
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
    const TargetType* parentClass = classOfClassInfo(*parent, depth + 1);
    if (!parentClass) {
        CORPSE_REPORT("The parent of the ClassInfo of '%s' at 0x%llx names no class", klass.name().legacyCStringPointer(), forReport(classInfo.address()));
        return false;
    }
    // A class of one image, such as a WebCore wrapper, describes its bases
    // with that image's types, and its parent's ClassInfo may be in another.
    auto parentName = parentClass->name();
    Vector<const TargetType*, 8> bases { &klass };
    for (size_t index = 0; index < bases.size() && index < maxClassInfoDepth * 4; ++index) {
        if (bases[index] == parentClass || bases[index]->name() == parentName)
            return true;
        if (auto* layout = std::get_if<TargetType::Class>(&bases[index]->layout())) {
            for (const TargetType::Base& base : layout->bases)
                bases.append(&base.type);
        }
    }
    CORPSE_REPORT("The parent of the ClassInfo of '%s' at 0x%llx is for '%s', which is not one of its bases", klass.name().legacyCStringPointer(), forReport(classInfo.address()), parentClass->name().legacyCStringPointer());
    return false;
}

Vector<String> HeapWalk::classInfosWithoutClass(size_t& count) const
{
    Vector<String> result;
    count = 0;
    if (!isValid())
        return result;
    // "6s_infoE" ends the linkage name of every X::s_info.
    for (const SnapshotDebugInfo::Symbol& symbol : m_debugInfo->dataSymbolsEndingWith(m_javaScriptCoreImage, "6s_infoE")) {
        ++count;
        if (!classOfClassInfo(symbol.address, 0))
            result.append(symbol.name);
    }
    return result;
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
