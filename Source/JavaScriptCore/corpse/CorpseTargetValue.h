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
#include <JavaScriptCore/CorpseTargetType.h>
#include <optional>
#include <span>
#include <stdint.h>
#include <type_traits>
#include <wtf/Ref.h>
#include <wtf/RefPtr.h>
#include <wtf/StdLibExtras.h>
#include <wtf/Vector.h>
#include <wtf/text/ASCIILiteral.h>
#include <wtf/text/CString.h>

namespace JSC {
namespace Corpse {

class Snapshot;

// The std::type_info of a polymorphic object, read through its vtable pointer.
// libc++ and libstdc++, the C++ runtimes of macOS and Linux, both implement the
// Itanium C++ ABI (https://itanium-cxx-abi.github.io/cxx-abi/abi.html), whose
// vtable layout (section 2.5.2) and type_info layout (section 2.9.3) are what
// is read here.
struct TargetTypeInfo {
    Address vtable;
    Address typeInfo;

    // Added to the object's address, gives the complete object. Zero unless the
    // object is a base subobject other than the primary one.
    int64_t offsetToTop { 0 };

    // As typeid(T).name() spells it, with the runtime's non-unique marker removed.
    CString mangledName;
    CString demangledName;
};

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
    static TargetValue at(const Snapshot&, Address, Ref<TargetType>&&);

    // A value of this type at another address, and the one `count` values on
    // from this one, for a buffer of them.
    TargetValue at(Address) const;
    TargetValue offsetBy(size_t count) const;

    bool isValid() const { return m_isValid; }
    explicit operator bool() const { return m_isValid; }

    Address address() const { return m_address; }
    const TargetType& type() const { return m_type.get(); }
    const Snapshot& snapshot() const { return *m_snapshot; }

    // Sub-values, by type().layout(). Only the fields a class declares itself
    // are its proper fields; a base's fields are read through its subobject.
    TargetValue properField(const char* name) const;
    TargetValue field(const TargetType::Field&) const;
    TargetValue base(const TargetType::Base&) const;
    TargetValue element(size_t index) const;

    // The virtual bases of the complete object this value is part of, where
    // that object puts them. A virtual base's offset depends on the dynamic
    // type, so this is the only way to reach one.
    Vector<TargetValue> virtualBases() const;

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

    // The three below need a polymorphic type.
    std::optional<TargetTypeInfo> typeInfo() const;
    RefPtr<TargetType> dynamicType() const;
    TargetValue downcast() const; // The complete object, as its dynamic type.

private:
    TargetValue(const Snapshot&, Address, Ref<TargetType>&&, bool isValid);

    TargetValue invalidated() const { return TargetValue(*m_snapshot, m_address, Ref { m_type }, false); }

    // The value at `offset` within this one, reporting if it does not fit.
    TargetValue member(size_t offset, Ref<TargetType>&&) const;

    // Fills `destination`, which must be type().byteSize() long, reporting if it cannot.
    bool readWhole(std::span<uint8_t> destination) const;

    template<typename T> std::optional<T> readAt(Address, ASCIILiteral what) const;

    const Snapshot* m_snapshot;
    Address m_address;
    Ref<TargetType> m_type;
    bool m_isValid;
};

} // namespace Corpse
} // namespace JSC

#endif // ENABLE(MYA)
