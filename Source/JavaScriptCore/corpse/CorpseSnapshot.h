/*
 * Copyright (C) 2026 Apple Inc. All rights reserved.
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

#if HAVE(CORPSE_SUPPORT)

#include <JavaScriptCore/CorpseAddress.h>
#include <JavaScriptCore/CorpseBackend.h>
#include <JavaScriptCore/CorpseProcess.h>
#include <JavaScriptCore/CorpseRegion.h>
#include <JavaScriptCore/CorpseSymbol.h>
#include <JavaScriptCore/CorpseTargetObject.h>
#include <JavaScriptCore/CorpseTargetType.h>
#include <JavaScriptCore/CorpseThread.h>
#include <memory>
#include <optional>
#include <span>
#include <wtf/DoublyLinkedList.h>
#include <wtf/HashMap.h>
#include <wtf/RefPtr.h>
#include <wtf/TZoneMalloc.h>
#include <wtf/Vector.h>
#include <wtf/text/CString.h>
#include <wtf/text/StringHash.h>
#include <wtf/text/StringView.h>
#include <wtf/text/WTFString.h>

namespace JSC {
namespace Corpse {

// Owns a corpse: a frozen, read-only copy of a process's address space taken
// together with the register state of every thread in it. What the platform
// does to produce one is the Backend's business; nothing here knows.
//
// Neither platform copies the memory itself, so the cost does not grow with it
// and the target is not stopped for the length of a copy.
// Check isValid() to see whether acquisition succeeded.
//
// Snapshots are linked into a DoublyLinkedList by their owner. The list is
// intrusive and does not own its nodes: whoever appends a Snapshot must remove
// it from the list before destroying it.
class Snapshot : public DoublyLinkedListNode<Snapshot> {
    WTF_MAKE_TZONE_ALLOCATED(Snapshot);
public:
    explicit Snapshot(RefPtr<Process>);
    ~Snapshot();

    Snapshot(const Snapshot&) = delete;
    Snapshot& operator=(const Snapshot&) = delete;
    Snapshot(Snapshot&& other) = delete;

    bool isValid() const { return !!m_backend; }

    // A monotonically increasing identifier assigned at construction. IDs are
    // never reused, so they stay stable as snapshots are added and removed.
    unsigned id() const { return m_id; }

    Process* process() const { return m_process.get(); }

    // The platform half of this corpse, null when acquisition failed. Callers
    // inside the library use this; callers outside should not need it.
    const Backend* backend() const { return m_backend.get(); }

    // The threads captured in this corpse, paired with their stacks on the
    // first call.
    const Vector<Thread>& threads();

    // The address of `name` in this corpse, null if it is not there.
    Address symbol(const char* name);

    // Copies `length` bytes from `address` in the corpse. Returns nullopt on
    // any short-copy or when the allocation for a length taken from the corpse
    // does not succeed.
    std::optional<Vector<uint8_t>> readBytes(Address, size_t length) const;

    // Copies into a caller's buffer, so a read on a hot path need not allocate.
    // Returns false on any short copy, leaving `into` unspecified.
    bool read(Address, std::span<uint8_t> into) const;

    // Reads one trivially-copyable value out of the corpse.
    template<typename T> bool readInto(Address address, T& out) const
    {
        return m_backend && m_backend->readInto(address, out);
    }

    // A NUL-terminated string out of the corpse, null if it does not end within
    // `maxLength`.
    CString readCString(Address address, size_t maxLength = 4 * KB) const
    {
        return m_backend ? m_backend->readCString(address, maxLength) : CString { };
    }

    // The mappings of the corpse, in ascending address order, as they were when
    // it was taken.
    const Vector<RegionInfo>& regions() const { return m_regions; }

    // The mapping containing `address`, or nullopt if it falls in none. This is
    // the check that makes a walk over a corrupt heap safe: an address that
    // lands in no mapping is not worth reading, and one that lands in the wrong
    // kind of mapping is not worth believing.
    std::optional<Region> regionContaining(Address) const;

    // The images mapped into the corpse. Read from the corpse itself, so this
    // describes the target whether or not the target is this process.
    const Vector<ImageInfo>& loadedImages() const { return m_images; }

    // Finds the layout the target's debug info gives for `qualifiedName`
    // (namespaces separated by "::"). Returns nullopt when the target's debug
    // info does not describe it. The first call spins up the type system,
    // which is why this is not const.
    std::optional<TargetType> findType(StringView qualifiedName);

    // The layout of what the target's global pointer `variableName` points to:
    // how a caller names a type without spelling it. The debug info knows a
    // template by every argument its instantiation has, defaulted ones
    // included, so naming a pointer leaves that spelling to the compiler. The
    // pointer need not point anywhere.
    std::optional<TargetType> findTypeOfPointee(StringView variableName);

    // The layout of the type of `memberName` within `qualifiedTypeName`. Every
    // member TargetType::fields() reports is reachable here, bases and
    // anonymous unions included.
    std::optional<TargetType> findTypeOfMember(StringView qualifiedTypeName, StringView memberName);

    // Binds an address in the corpse to a type, reading the object's bytes
    // once. A subsequent get() slices those bytes without reading again.
    // Returns nullopt when the corpse read short-copies.
    std::optional<TargetObject> getTargetObject(Address, const TargetType&);

    // One step of a walk over a graph of objects. Reports nothing for a member
    // that is not a pointer, holds a null one, or addresses something the debug
    // info does not describe.
    std::optional<TargetObject> follow(const TargetObject&, const TargetField&);

private:
    static unsigned s_nextId;

    TypeSystem* typeSystem();

    RefPtr<Process> m_process;
    std::unique_ptr<Backend> m_backend;
    unsigned m_id;

    // Taken once with the corpse, so every lookup sees the same address space
    // and no lookup costs a re-parse. Sorted by base address.
    Vector<RegionInfo> m_regions;
    Vector<ImageInfo> m_images;

    std::optional<Vector<Thread>> m_threads;
    HashMap<String, std::unique_ptr<Symbol>> m_symbols;
    std::unique_ptr<TypeSystem> m_typeSystem;

    Snapshot* m_prev { nullptr }; // Required by DoublyLinkedListNode.
    Snapshot* m_next { nullptr }; // Required by DoublyLinkedListNode.

    friend class WTF::DoublyLinkedListNode<Snapshot>;
};

} // namespace Corpse
} // namespace JSC

#endif // HAVE(CORPSE_SUPPORT)
