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
#include <optional>
#include <stdint.h>
#include <utility>
#include <wtf/BitSet.h>
#include <wtf/Function.h>
#include <wtf/HashMap.h>
#include <wtf/IterationStatus.h>
#include <wtf/RefPtr.h>
#include <wtf/Vector.h>

namespace JSC {

class BlockDirectory;
class Heap;
class JSCell;
class MarkedSpace;
class Structure;
class VM;

namespace Corpse {

struct MarkedSpaceState;

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

    // The cells MarkedSpace::forEachLiveCell would visit in the target inside
    // a HeapIterationScope, which first stops every allocator. The snapshot
    // must be at a safe point: otherwise this reports it and visits nothing.
    void forEachLiveCell(const Function<IterationStatus(const Cell&)>&) const;

    // The cell at `address`, as a JSCell, and the Structure a cell's
    // StructureID bits name.
    Remote<JSCell> jsCell(Address) const;
    Remote<Structure> structure(uint32_t structureIDBits) const;

    using AtomBits = WTF::BitSet<MarkedBlock::atomsPerBlock>;

    struct Allocation {
        Address address;
        uint64_t size;
    };
    struct Reach {
        uint64_t bytesAllocated { 0 };
        uint64_t bytesReached { 0 }; // Of the allocations the walk reaches any address in.
        Vector<Allocation> largestMissed; // Largest first.
    };

    // How much of `allocations`, sorted by address, the walk reaches: every
    // live cell, and every C++ object reached from the roots through a pointer
    // whose pointee type is known and that lands inside an allocation.
    Reach reach(const Vector<Allocation>&, size_t missedCount) const;

private:
    using BlockSet = UncheckedKeyHashSet<MarkedBlock*>;

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
