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
#include <optional>
#include <stdint.h>
#include <wtf/Function.h>
#include <wtf/IterationStatus.h>
#include <wtf/RefPtr.h>

namespace JSC {

class BlockDirectory;
class JSCell;
class JSValue;
class MarkedSpace;
class Structure;
class VM;

namespace Corpse {

// The JS heap of a VM in a corpse, walked through the target's debug info.
// The walk names two variables, VMManager::s_recentVM and the MyaRoots that
// JavaScriptCore keeps for it; every type it reads is the type of a field of
// one of those or of a value reached from them, so the layouts are the
// target's whatever it was built for.
class HeapWalk {
public:
    struct Cell {
        Address address;
        uint64_t size;
        HeapCell::Kind kind;
    };

    // The heap of the VM the target entered most recently. Invalid, and
    // reported, if the target has no VM or the snapshot no debug info.
    explicit HeapWalk(Snapshot&);

    bool isValid() const { return static_cast<bool>(m_vm); }
    Remote<VM> vm() const { return m_vm; }

    // Every cell marked in the target's last collection. A cell allocated
    // since then is not seen, so a target that parks right after a full
    // collection shows its whole live heap.
    void forEachLiveCell(const Function<IterationStatus(const Cell&)>&) const;

    // The cell at `address`, as a JSCell, and the Structure a cell's
    // StructureID bits name.
    Remote<JSCell> jsCell(Address) const;
    Remote<Structure> structure(uint32_t structureIDBits) const;

private:
    IterationStatus walkDirectory(const Remote<BlockDirectory>&, uint32_t markingVersion, const Function<IterationStatus(const Cell&)>&) const;
    IterationStatus walkBlock(const TargetValue& handle, uint32_t markingVersion, const Function<IterationStatus(const Cell&)>&) const;
    IterationStatus walkPreciseAllocations(const Remote<MarkedSpace>&, const Function<IterationStatus(const Cell&)>&) const;

    Snapshot& m_snapshot;
    RefPtr<SnapshotDebugInfo> m_debugInfo;
    Remote<VM> m_vm;
    std::optional<TargetValue> m_structure; // Any Structure, to retype from.
    std::optional<TargetValue> m_jsCell; // Any JSCell, to retype from.
    std::optional<TargetValue> m_blockHeaderPointer; // MyaRoots::markedBlockHeader, for its type.
    uint64_t m_startOfStructureHeap { 0 };
};

// A JSValue holds its cell, if it is one.
template<>
struct RemoteTraits<JSValue> {
    template<typename Visitor>
    static void visitChildren(const Remote<JSValue>& value, Visitor& visitor)
    {
        auto bits = value.as<uint64_t>();
        if (!bits)
            return;
        // JSCJSValue.h describes both encodings. With 4-byte pointers a value
        // is a payload word then a tag word, and a cell's tag is CellTag. With
        // 8-byte pointers a cell is a nonzero value with none of the NumberTag
        // and OtherTag bits.
        Address cell;
        if (value.addressByteSize() == 4) {
            if ((*bits >> 32) != 0xfffffffb)
                return;
            cell = Address { *bits & 0xffffffff };
        } else {
            if (!*bits || (*bits & 0xfffe000000000002))
                return;
            cell = Address { *bits };
        }
        visitor.visit(Remote<JSCell>(value.snapshot(), cell));
    }
};

} // namespace Corpse
} // namespace JSC

#endif // ENABLE(MYA)
