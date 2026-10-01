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
#include <JavaScriptCore/CorpseSnapshotDebugInfo.h>
#include <JavaScriptCore/CorpseTargetType.h>
#include <optional>
#include <span>
#include <stdint.h>
#include <type_traits>
#include <wtf/Function.h>
#include <wtf/Ref.h>
#include <wtf/RefPtr.h>
#include <wtf/StdLibExtras.h>
#include <wtf/Vector.h>

namespace JSC {
namespace Corpse {

class Snapshot;
class SnapshotDebugInfo;

// A value in a corpse: an address and the type to read it as. Nothing is read
// until a scalar is asked for, so a field of a large object costs one read of
// that field.
//
// A failure reports once and gives an invalid value; everything asked of an
// invalid value is invalid or nullopt without another report, so a chain of
// lookups reports the step that failed and nothing more.
class TargetValue {
public:
    // Invalid, and silent, for a null address: a null pointer is a state of
    // the heap, not a failure.
    static TargetValue at(Snapshot&, Address, const TargetType&);

    // The complete object containing the polymorphic object at `address`, as
    // its dynamic type. Reported and nullopt if there is none.
    static std::optional<TargetValue> completeObjectAt(Snapshot&, SnapshotDebugInfo&, Address);

    // A value of this type at another address, and the one `count` values on
    // from this one, for a buffer of them.
    TargetValue at(Address) const;
    TargetValue offsetBy(size_t count) const;

    bool isValid() const { return m_isValid; }
    explicit operator bool() const { return m_isValid; }

    Address address() const { return m_address; }
    const TargetType& type() const { return *m_type; }
    Snapshot& snapshot() const { return *m_snapshot; }

    // Sub-values, by type().layout(). Only the fields a class declares itself
    // are its proper fields; a base's fields are read through its subobject.
    TargetValue properField(const char* name) const;
    TargetValue field(const TargetType::Field&) const;
    TargetValue base(const TargetType::Base&) const;

    // Every field of this value, superclass by superclass: its own fields,
    // its non-virtual bases' in turn, then each virtual base's once. A virtual
    // base's offset is only known in a complete object, so this value must be one.
    void forEachField(const Function<void(const TargetType::Field&, const TargetValue&)>&) const;

    std::optional<Address> pointerValue() const; // Pointers and references, PAC stripped.
    TargetValue dereference() const;

    // For a pointer: the value it would point at if it pointed at `address`.
    TargetValue pointeeAt(Address) const;

    template<typename T>
    std::optional<T> as() const
    {
        static_assert(std::is_trivially_copyable_v<T>);
        T value;
        if (!readWhole(asMutableByteSpan(value)))
            return std::nullopt;
        return value;
    }
    std::optional<int64_t> integer() const; // By the type's width and sign.

private:
    TargetValue(Snapshot&, Address, const TargetType&, bool isValid);

    TargetValue invalidated() const { return TargetValue(*m_snapshot, m_address, *m_type, false); }

    // The value at `offset` within this one, reporting if it does not fit.
    TargetValue member(size_t offset, const TargetType&) const;

    void forEachNonVirtualField(const Function<void(const TargetType::Field&, const TargetValue&)>&) const;

    // Fills `destination`, which must be type().byteSize() long, reporting if it cannot.
    bool readWhole(std::span<uint8_t> destination) const;

    size_t bitfieldByteSize() const { return (m_bitOffset + m_bitSize + 7) / 8; }

    // The dynamic type, and where the complete object starts.
    const TargetType* dynamicType(Address& completeObject) const;

    Snapshot* m_snapshot;
    Address m_address;
    Ref<SnapshotDebugInfo> m_debugInfo; // Which owns m_type.
    const TargetType* m_type;
    bool m_isValid;
    unsigned m_bitOffset { 0 };
    unsigned m_bitSize { 0 }; // Nonzero for a bitfield, which only integer() reads.
};

} // namespace Corpse
} // namespace JSC

#endif // ENABLE(MYA)
