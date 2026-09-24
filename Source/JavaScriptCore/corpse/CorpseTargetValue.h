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

#if ENABLE(MYA_HEAP)

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
#include <wtf/text/CString.h>

namespace JSC {
namespace Corpse {

class Snapshot;

// The std::type_info of a polymorphic object, read through its vtable pointer.
// The Itanium ABI puts the offset to the complete object and the type_info
// pointer in the two words before the vtable's first entry.
// CLAUDE: explain why we can assume Itanium ABI with sources. Give a source for this format.
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

// CLAUDE: Simplify this by assuming lazy materialization everywhere. Future work will make that map in pages to avoid mutation.
enum class Materialization : uint8_t {
    // at() copies the whole object once; field() and element() slice that copy
    // and never go back to the corpse.
    Eager,
    // Nothing is copied until a scalar is asked for, so one field of a large
    // object costs one read of that field.
    Lazy,
};

// CLAUDE: if memory could not be read, we must always report.

// A mya-side view of a value or object in a corpse.
template<Materialization materialization>
class TargetValue {
public:
    static std::optional<TargetValue> at(const Snapshot&, Address, Ref<TargetType>&&);

    Address address() const { return m_address; }
    const TargetType& type() const { return m_type.get(); }
    const Snapshot& snapshot() const { return *m_snapshot; }

    // CLAUDE: ditto re: properField
    // This class, then its non-virtual bases, then (through RTTI) its virtual bases.
    std::optional<TargetValue> field(const char* name) const;

    // A direct or indirect non-virtual base, by qualified name.
    std::optional<TargetValue> base(const char* name) const;

    // A virtual base, located through the complete object's layout: the offset
    // of a virtual base depends on the dynamic type, not the static one.
    std::optional<TargetValue> virtualBase(const char* name) const;

    // CLAUDE: ditto re: visiting
    std::optional<TargetValue> element(size_t index) const; // Array.

    std::optional<Address> pointerValue() const; // Pointer and Reference, PAC stripped.
    std::optional<TargetValue> dereference() const; // Nullopt for a null pointer.

    template<typename T>
    std::optional<T> as() const
    {
        static_assert(std::is_trivially_copyable_v<T>);
        T value;
        if (!readInto(asMutableByteSpan(value)))
            return std::nullopt;
        return value;
    }
    std::optional<int64_t> integer() const; // Integer, Enum and Bool, by the type's width and sign.
    std::optional<double> floatingPoint() const;
    std::optional<Vector<uint8_t>> bytes() const;

    // The three below need type().isPolymorphic().
    std::optional<TargetTypeInfo> typeInfo() const;
    RefPtr<TargetType> dynamicType() const; // Looked up in the image the vtable is mapped in.
    std::optional<TargetValue> downcast() const; // The complete object, as its dynamic type.

private:
    struct EagerStorage {
        Vector<uint8_t> bytes;
    };
    struct LazyStorage { };
    using Storage = std::conditional_t<materialization == Materialization::Eager, EagerStorage, LazyStorage>;

    TargetValue(const Snapshot&, Address, Ref<TargetType>&&, Storage&&);

    // CLAUDE: we should avoid reading via copies. Make sure this works in a way that will work with the future plans to have a page manager. Reading from corpse memory should be done close to the code that has a fixme for the page manager, not here.
    // Copies exactly type().byteSize() bytes; a span of another size is reported.
    bool readInto(std::span<uint8_t>) const;

    // The bytes at `offset` within this value, from the copy or from the corpse.
    std::optional<Vector<uint8_t>> readBytes(size_t offset, size_t) const;

    std::optional<TargetValue> member(size_t offset, Ref<TargetType>&&) const;

    const Snapshot* m_snapshot;
    Address m_address;
    Ref<TargetType> m_type;
    [[no_unique_address]] Storage m_storage;
};

using EagerTargetValue = TargetValue<Materialization::Eager>;
using LazyTargetValue = TargetValue<Materialization::Lazy>;

} // namespace Corpse
} // namespace JSC

#endif // ENABLE(MYA_HEAP)
