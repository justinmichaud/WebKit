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

#pragma once

#include <JavaScriptCore/CorpsePlatform.h>

#if ENABLE(MYA)

#include <JavaScriptCore/CorpseAddress.h>
#include <JavaScriptCore/CorpseRemote.h>
#include <JavaScriptCore/CorpseSnapshot.h>
#include <JavaScriptCore/CorpseSnapshotDebugInfo.h>
#include <JavaScriptCore/CorpseTargetValue.h>
#include <JavaScriptCore/HeapCell.h>
#include <JavaScriptCore/JSCJSValue.h>
#include <JavaScriptCore/MarkedBlock.h>
#include <array>
#include <optional>
#include <stdint.h>
#include <utility>
#include <wtf/BitSet.h>
#include <wtf/Function.h>
#include <wtf/HashMap.h>
#include <wtf/IterationStatus.h>
#include <wtf/RefPtr.h>
#include <wtf/Vector.h>
#include <wtf/text/ASCIILiteral.h>
#include <wtf/text/WTFString.h>

namespace JSC {

class BlockDirectory;
struct ClassInfo;
class Heap;
class JSCell;
class MarkedSpace;
class Structure;
class VM;

namespace Corpse {

struct MarkedSpaceState;
class ReachWalk;

// The JS heap of a VM in a corpse, walked through the target's debug info.
// The walk starts from a roots object, whose type comes from its vtable; every
// other type it reads is the type of a field reached from there, so the
// layouts are the target's whatever it was built for.
class HeapWalk {
public:
    struct Cell {
        Address address;
        uint64_t size;
        HeapCell::Kind kind;
    };

    // `roots` is an object of a polymorphic class with the fields `JSC::VM* vm`,
    // `JSC::MarkedBlock::Header* markedBlockHeader` and
    // `JSC::LocalAllocator* localAllocator`; only the last two's types are
    // read. Invalid, and reported, if the roots name no VM or the snapshot has
    // no debug info.
    HeapWalk(Snapshot&, Address roots);

    bool isValid() const { return static_cast<bool>(m_vm); }
    Remote<VM> vm() const { return m_vm; }
    // The roots, as their dynamic type. Null if the walk is invalid.
    const TargetValue* roots() const { return isValid() ? &*m_roots : nullptr; }

    // The cells MarkedSpace::forEachLiveCell would visit in the target inside
    // a HeapIterationScope, which first stops every allocator. The snapshot
    // must be at a safe point: otherwise this reports it and visits nothing.
    void forEachLiveCell(const Function<IterationStatus(const Cell&)>&) const;

    // The cell at `address`, as a JSCell, and the Structure a cell's
    // StructureID bits name.
    Remote<JSCell> jsCell(Address) const;
    Remote<Structure> structure(uint32_t structureIDBits) const;

    // The C++ class of the JS cell at `address`: the class whose s_info is its
    // Structure's ClassInfo, checked against that ClassInfo's size and parent.
    // Null, having reported why once per ClassInfo, if there is none.
    const TargetType* cellClass(Address) const;

    using AtomBits = WTF::BitSet<MarkedBlock::atomsPerBlock>;

    // What libpas knows of an allocation, independently of the debug info,
    // which the target records with it as it enumerates its heap.
    struct AllocationFacts {
        uint64_t heap { 0 }; // Its pas_heap, or 0 if it is in no bmalloc heap.
        uint64_t typeName { 0 }; // The name of the heap's bmalloc_type, a C string in the target, or 0.
        uint32_t typeSize { 0 }; // The heap's type's size and alignment, or 0. A primitive heap's type is 1 byte.
        uint32_t typeAlignment { 0 };
        uint8_t objectKind { 0 }; // A pas_object_kind; 0, pas_not_an_object_kind, for an object of no bmalloc heap.
    };
    struct Allocation {
        Address address;
        uint64_t size;
        AllocationFacts facts { };
    };
    // Why the walk did not follow a value that may point somewhere.
    enum class NotFollowed : uint8_t {
        Declaration, // A pointer to a class that is only declared, with no dynamic type.
        VoidPointer, // A pointer to a type without a size, such as void.
        NoDynamicType, // A pointer to a polymorphic class whose object has no dynamic type; followed as its static type.
        CellWithoutClass, // A JS cell whose ClassInfo names no class.
        TypeNotReached, // An encoded pointer whose pointee type the walk has not reached.
    };
    static constexpr size_t numberOfNotFollowedReasons = static_cast<size_t>(NotFollowed::TypeNotReached) + 1;

    struct Reach {
        uint64_t bytesAllocated { 0 };
        uint64_t bytesReached { 0 }; // Of the allocations the walk reaches any address in.
        Vector<Allocation> largestMissed; // Largest first.
        std::array<uint64_t, numberOfNotFollowedReasons> notFollowed { };
        uint64_t cellsWithClass { 0 };
        uint64_t cellBytesBeyondClass { 0 }; // In cells bigger than their class, such as objects with inline storage.
    };

    // How much of `allocations`, sorted by address, the walk reaches: every
    // live cell, each JS cell as its C++ class, and every C++ object reached
    // from the roots or from a cell through a pointer whose pointee type is
    // known and that lands inside an allocation. Each class is read as its
    // home description.
    Reach reach(const Vector<Allocation>&, size_t missedCount) const;

    // Why a word that points into a missed allocation did not lead the walk there.
    enum class EdgeReason : uint8_t {
        Integer, // It is in an integer field.
        VoidPointer, // In a pointer to a type without a size, such as void.
        Declaration, // In a pointer to a class that is only declared.
        PointeeDoesNotFit, // In a pointer whose pointee does not fit in the allocation it points into.
        OtherField, // In a field of another type, such as a floating-point number.
        BeyondCellClass, // In a JS cell, past the end of its class.
        UntypedBytes, // In a reached allocation, outside every value the walk read.
        ImageData, // In an image's data, outside every value the walk read.
        Stack, // On a thread's stack.
        OtherMemory, // In another region of the snapshot.
        MissedAllocation, // In another missed allocation.
    };
    static constexpr size_t numberOfEdgeReasons = static_cast<size_t>(EdgeReason::MissedAllocation) + 1;
    static ASCIILiteral description(EdgeReason);

    // A word, or a 6-byte packed pointer, that points into a missed allocation.
    struct Edge {
        Address at;
        EdgeReason reason;
        bool isPacked { false }; // A 6-byte pointer at 2-byte alignment, found by the second pass.
        // Where the word is: a class and its field, a JS cell's class, a symbol, a thread, or a region.
        String owner;
        std::optional<size_t> fromMissed; // For EdgeReason::MissedAllocation, the index of that missed allocation.
    };
    struct MissedAllocation {
        Allocation allocation;
        Vector<Edge> edges; // The first few, by address; edgeCount counts them all.
        size_t edgeCount { 0 };
        // The first edge not in a missed allocation, on the way back through any
        // missed allocations that point to this one; nullopt if there is none.
        std::optional<Edge> attribution;
        bool isAttributedThroughMissed { false };
    };
    struct Group {
        EdgeReason reason;
        String owner;
        uint64_t bytes { 0 };
        uint64_t count { 0 };
    };
    struct Attribution {
        Reach reach;
        Vector<MissedAllocation> missed; // Largest first.
        Vector<Group> groups; // The missed bytes by attribution, largest first.
        Vector<Group> byHeapType; // The missed bytes by libpas heap type, largest first; reason is unused.
        Vector<std::pair<uint64_t, uint64_t>> withoutEdgeBySize; // Sizes of the missed allocations nothing points to, and how many have each, largest total first.
        uint64_t bytesWithoutEdge { 0 };
        Vector<String> failedChecks; // Where libpas's facts say the walk read an object as the wrong type.
    };

    // Says, for every missed allocation, which words point into it and why the
    // walk did not follow them. It scans every word of every allocation, the
    // writable regions of the images, the live part of each thread's stack,
    // and, out of process, the snapshot's other writable regions. It leaves out
    // `excluded`, such as the memory that holds the list of allocations, and
    // the parts of `heapPages`, the pages libpas holds objects in, that hold
    // no live allocation, since they are free. Both are sorted by address. It
    // also checks each object the walk read as a class against libpas's facts
    // about its allocation.
    Attribution attribute(const Vector<Allocation>&, const Vector<Allocation>& excluded, const Vector<Allocation>& heapPages) const;

private:
    friend class ReachWalk;
    using BlockSet = UncheckedKeyHashSet<MarkedBlock*>;

    const TargetType* classOfClassInfo(const Remote<ClassInfo>&, unsigned depth) const;

    bool isAtSafePoint(const Remote<Heap>&, const Remote<MarkedSpace>&) const;
    Remote<MarkedBlock::Header> header(Address block) const;
    std::optional<HashMap<uint64_t, AtomBits>> stopAllocating(const Remote<MarkedSpace>&) const;
    IterationStatus walkBlock(const Remote<MarkedBlock*>&, const HashMap<uint64_t, AtomBits>& newlyAllocatedAfterStop, const MarkedSpaceState&, const Function<IterationStatus(const Cell&)>&) const;
    IterationStatus walkPreciseAllocations(const Remote<MarkedSpace>&, const Function<IterationStatus(const Cell&)>&) const;

    RefPtr<SnapshotDebugInfo> m_debugInfo;
    std::optional<TargetValue> m_roots;
    Remote<VM> m_vm;
    std::optional<TargetValue> m_structure; // Any Structure, to retype from.
    std::optional<TargetValue> m_jsCell; // Any JSCell, to retype from.
    std::optional<TargetValue> m_blockHeaderPointer; // The roots' markedBlockHeader, for its type.
    std::optional<TargetValue> m_localAllocatorPointer; // The roots' localAllocator, for its type.
    uint64_t m_startOfStructureHeap { 0 };
    mutable HashMap<uint64_t, const TargetType*> m_classesOfClassInfos;
};

// A JSValue holds its cell, if it is one, as SlotVisitor::appendUnbarriered(JSValue) visits it.
template<>
struct RemoteTraits<JSValue> {
    template<typename Visitor>
    static void visitChildren(const Remote<JSValue>& value, Visitor& visitor)
    {
        auto bits = value.as<EncodedJSValue>();
        if (!bits)
            return;
        JSValue decoded = JSValue::decode(*bits);
        if (!decoded || !decoded.isCell())
            return;
        Address cell { static_cast<uint64_t>(*bits) };
        visitor.visit(Remote<JSCell>(value.snapshot(), cell));
    }
};

} // namespace Corpse
} // namespace JSC

#endif // ENABLE(MYA)
