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
#include "CorpseTargetValue.h"

#if ENABLE(MYA_HEAP)

#include "CorpseError.h"
#include "CorpseSnapshot.h"
#include "CorpseTargetDebugInfo.h"
#include <cxxabi.h>
#include <stdlib.h>
#include <string_view>
#include <wtf/Scope.h>
#include <wtf/StdLibExtras.h>
#include <wtf/text/StringCommon.h>

namespace JSC {
namespace Corpse {

// CLAUDE: all bounds should live together
// CLAUDE: all parts of this should be hooked up to the diagnostics pattern
static constexpr size_t maxTypeNameLength = 4 * KB;

// CLAUDE: Remove all things related to searching for names. We should only be able to search by rtti to avoid confusion and complexity, and we should always walk the full type hierarchy of a given class, never searching.
static bool nameIs(const TargetType& type, const char* name)
{
    return std::string_view { type.name().span() } == std::string_view { name };
}

// A direct or indirect non-virtual base of `type`, at its offset from `type`.
static std::optional<TargetType::Base> findBase(const TargetType& type, const char* name)
{
    Vector<TargetType::Base> bases = type.bases();
    for (const TargetType::Base& base : bases) {
        if (nameIs(base.type.get(), name))
            return base;
        if (auto nested = findBase(base.type.get(), name)) {
            nested->offset += base.offset;
            return nested;
        }
    }
    return std::nullopt;
}

// Whether bytes `offset` to `offset + size` are inside a value of `type`; reports when not.
static bool containsBytes(const TargetType& type, size_t offset, size_t size)
{
    size_t total = type.byteSize();
    if (offset <= total && size <= total - offset)
        return true;
    CORPSE_REPORT("Bytes %zu to %zu are outside the %zu-byte '%s'", offset, offset + size, total, type.name());
    return false;
}

template<Materialization materialization>
TargetValue<materialization>::TargetValue(const Snapshot& snapshot, Address address, Ref<TargetType>&& type, Storage&& storage)
    : m_snapshot(&snapshot)
    , m_address(address)
    , m_type(WTF::move(type))
    , m_storage(WTF::move(storage))
{
}

template<Materialization materialization>
std::optional<TargetValue<materialization>> TargetValue<materialization>::at(const Snapshot& snapshot, Address address, Ref<TargetType>&& type)
{
    if (!address)
        return std::nullopt;
    size_t size = type->byteSize();
    if (!size) {
        CORPSE_REPORT("Type '%s' has no size, so nothing can be read as it", type->name());
        return std::nullopt;
    }
    if constexpr (materialization == Materialization::Eager) {
        Vector<uint8_t> bytes(size);
        if (snapshot.read(address, bytes.mutableSpan()).empty())
            return std::nullopt;
        return TargetValue(snapshot, address, WTF::move(type), EagerStorage { WTF::move(bytes) });
    } else
        return TargetValue(snapshot, address, WTF::move(type), LazyStorage { });
}

template<Materialization materialization>
std::optional<Vector<uint8_t>> TargetValue<materialization>::readBytes(size_t offset, size_t size) const
{
    if (!containsBytes(m_type.get(), offset, size))
        return std::nullopt;
    if constexpr (materialization == Materialization::Eager)
        return Vector<uint8_t>(m_storage.bytes.subspan(offset, size));
    else {
        Vector<uint8_t> bytes(size);
        if (m_snapshot->read(m_address + offset, bytes.mutableSpan()).empty())
            return std::nullopt;
        return bytes;
    }
}

template<Materialization materialization>
std::optional<TargetValue<materialization>> TargetValue<materialization>::member(size_t offset, Ref<TargetType>&& type) const
{
    size_t size = type->byteSize();
    if (!size) {
        CORPSE_REPORT("Type '%s' has no size, so nothing can be read as it", type->name());
        return std::nullopt;
    }
    if constexpr (materialization == Materialization::Eager) {
        auto bytes = readBytes(offset, size);
        if (!bytes)
            return std::nullopt;
        return TargetValue(*m_snapshot, m_address + offset, WTF::move(type), EagerStorage { WTF::move(*bytes) });
    } else {
        if (!containsBytes(m_type.get(), offset, size))
            return std::nullopt;
        return TargetValue(*m_snapshot, m_address + offset, WTF::move(type), LazyStorage { });
    }
}

template<Materialization materialization>
std::optional<TargetValue<materialization>> TargetValue<materialization>::field(const char* name) const
{
    if (auto found = m_type->field(name))
        return member(found->offset, WTF::move(found->type));

    if (m_type->isPolymorphic()) {
        // A virtual base sits wherever the complete object puts it.
        auto complete = downcast();
        if (!complete)
            return std::nullopt;
        Vector<TargetType::Base> virtualBases = complete->type().virtualBases();
        for (const TargetType::Base& base : virtualBases) {
            if (auto found = base.type->field(name))
                return complete->member(base.offset + found->offset, WTF::move(found->type));
        }
    }

    CORPSE_REPORT("Type '%s' has no member '%s'", m_type->name(), reportableString(name));
    return std::nullopt;
}

template<Materialization materialization>
std::optional<TargetValue<materialization>> TargetValue<materialization>::base(const char* name) const
{
    if (auto found = findBase(m_type.get(), name))
        return member(found->offset, WTF::move(found->type));
    CORPSE_REPORT("Type '%s' has no non-virtual base '%s'", m_type->name(), reportableString(name));
    return std::nullopt;
}

template<Materialization materialization>
std::optional<TargetValue<materialization>> TargetValue<materialization>::virtualBase(const char* name) const
{
    // Every class with a virtual base has a vptr, so a type without one has no virtual bases.
    if (!m_type->isPolymorphic()) {
        CORPSE_REPORT("Type '%s' has no virtual bases", m_type->name());
        return std::nullopt;
    }
    auto complete = downcast();
    if (!complete)
        return std::nullopt;
    Vector<TargetType::Base> virtualBases = complete->type().virtualBases();
    for (const TargetType::Base& base : virtualBases) {
        if (nameIs(base.type.get(), name))
            return complete->member(base.offset, Ref { base.type });
    }
    CORPSE_REPORT("Type '%s' has no virtual base '%s'", complete->type().name(), reportableString(name));
    return std::nullopt;
}

template<Materialization materialization>
std::optional<TargetValue<materialization>> TargetValue<materialization>::element(size_t index) const
{
    RefPtr<TargetType> elementType = m_type->element();
    if (!elementType) {
        CORPSE_REPORT("Type '%s' is not an array", m_type->name());
        return std::nullopt;
    }
    size_t count = m_type->elementCount();
    if (index >= count) {
        CORPSE_REPORT("Element %zu is outside the %zu elements of '%s'", index, count, m_type->name());
        return std::nullopt;
    }
    return member(index * elementType->byteSize(), elementType.releaseNonNull());
}

template<Materialization materialization>
std::optional<Address> TargetValue<materialization>::pointerValue() const
{
    TargetType::Kind kind = m_type->kind();
    if (kind != TargetType::Kind::Pointer && kind != TargetType::Kind::Reference) {
        CORPSE_REPORT("Type '%s' is not a pointer", m_type->name());
        return std::nullopt;
    }
    auto bytes = readBytes(0, m_type->byteSize());
    if (!bytes)
        return std::nullopt;
    if (bytes->size() != sizeof(uint64_t)) {
        CORPSE_REPORT("'%s' is a %zu-byte pointer, and only 8-byte pointers are supported", m_type->name(), bytes->size());
        return std::nullopt;
    }
    uint64_t raw = 0;
    memcpySpan(asMutableByteSpan(raw), bytes->span());
    return Address { raw }.stripped();
}

template<Materialization materialization>
std::optional<TargetValue<materialization>> TargetValue<materialization>::dereference() const
{
    auto pointer = pointerValue();
    if (!pointer || !*pointer)
        return std::nullopt;
    RefPtr<TargetType> pointee = m_type->pointee();
    if (!pointee) {
        CORPSE_REPORT("Type '%s' points at nothing that can be read", m_type->name());
        return std::nullopt;
    }
    return at(*m_snapshot, *pointer, pointee.releaseNonNull());
}

template<Materialization materialization>
bool TargetValue<materialization>::readInto(std::span<uint8_t> destination) const
{
    if (destination.size() != m_type->byteSize()) {
        CORPSE_REPORT("Reading the %zu-byte '%s' as %zu bytes", m_type->byteSize(), m_type->name(), destination.size());
        return false;
    }
    auto bytes = readBytes(0, destination.size());
    if (!bytes)
        return false;
    memcpySpan(destination, bytes->span());
    return true;
}

template<Materialization materialization>
std::optional<int64_t> TargetValue<materialization>::integer() const
{
    TargetType::Kind kind = m_type->kind();
    if (kind != TargetType::Kind::Integer && kind != TargetType::Kind::Enum && kind != TargetType::Kind::Bool) {
        CORPSE_REPORT("Type '%s' is not an integer", m_type->name());
        return std::nullopt;
    }
    auto bytes = readBytes(0, m_type->byteSize());
    if (!bytes)
        return std::nullopt;
    size_t size = bytes->size();
    if (size != 1 && size != 2 && size != 4 && size != 8) {
        CORPSE_REPORT("'%s' is a %zu-byte integer, which does not fit in 64 bits", m_type->name(), size);
        return std::nullopt;
    }
    uint64_t raw = 0;
    memcpySpan(asMutableByteSpan(raw).first(size), bytes->span());
    unsigned bits = size * 8;
    if (m_type->isSigned() && bits < 64 && ((raw >> (bits - 1)) & 1))
        raw |= ~0ull << bits;
    return static_cast<int64_t>(raw);
}

// CLAUDE: we never read floats. Don't add any helpers not exercised by a test, we can add them as we need them.
template<Materialization materialization>
std::optional<double> TargetValue<materialization>::floatingPoint() const
{
    if (m_type->kind() != TargetType::Kind::Float) {
        CORPSE_REPORT("Type '%s' is not a floating-point type", m_type->name());
        return std::nullopt;
    }
    auto bytes = readBytes(0, m_type->byteSize());
    if (!bytes)
        return std::nullopt;
    if (bytes->size() == sizeof(float)) {
        float value = 0;
        memcpySpan(asMutableByteSpan(value), bytes->span());
        return value;
    }
    if (bytes->size() == sizeof(double)) {
        double value = 0;
        memcpySpan(asMutableByteSpan(value), bytes->span());
        return value;
    }
    CORPSE_REPORT("'%s' is a %zu-byte floating-point type, and only float and double are supported", m_type->name(), bytes->size());
    return std::nullopt;
}

template<Materialization materialization>
std::optional<Vector<uint8_t>> TargetValue<materialization>::bytes() const
{
    return readBytes(0, m_type->byteSize());
}

template<Materialization materialization>
std::optional<TargetTypeInfo> TargetValue<materialization>::typeInfo() const
{
    if (!m_type->isPolymorphic()) {
        CORPSE_REPORT("Type '%s' is not polymorphic, so it has no type_info", m_type->name());
        return std::nullopt;
    }
    auto vptrBytes = readBytes(0, sizeof(uint64_t));
    if (!vptrBytes)
        return std::nullopt;
    uint64_t vptr = 0;
    memcpySpan(asMutableByteSpan(vptr), vptrBytes->span());

    TargetTypeInfo info;
    info.vtable = Address { vptr }.stripped();
    // CLAUDE: make a struct and read it instead of doing offset math if its not too complex
    // make this whole thing self-documenting in that way
    auto offsetToTop = m_snapshot->read<int64_t>(info.vtable - 2 * sizeof(uint64_t));
    auto typeInfoPointer = m_snapshot->read<uint64_t>(info.vtable - sizeof(uint64_t));
    if (!offsetToTop || !typeInfoPointer)
        return std::nullopt;
    info.offsetToTop = *offsetToTop;
    info.typeInfo = Address { *typeInfoPointer }.stripped();

    // A type_info is its own vtable pointer followed by its name pointer.
    auto namePointer = m_snapshot->read<uint64_t>(info.typeInfo + sizeof(uint64_t));
    if (!namePointer)
        return std::nullopt;
    // libc++ sets the top bit of the name pointer when the name is not unique
    // across images; libstdc++ prefixes the name with '*' instead.
    constexpr uint64_t nonUniqueBit = 1ull << 63;
    auto name = m_snapshot->readCString(Address { *namePointer & ~nonUniqueBit }.stripped(), maxTypeNameLength);
    if (!name)
        return std::nullopt;
    std::span<const char> characters = name->span();
    if (!characters.empty() && characters.front() == '*')
        characters = characters.subspan(1);
    info.mangledName = UTF8CString(byteCast<char8_t>(characters));

    int status = 0;
    char* demangled = abi::__cxa_demangle(info.mangledName.data(), nullptr, nullptr, &status);
    auto freeDemangled = makeScopeExit([&] {
        free(demangled);
    });
    if (status || !demangled) {
        CORPSE_REPORT("The type_info name '%s' of the object at 0x%llx does not demangle", info.mangledName,
            static_cast<unsigned long long>(m_address.toTargetVMAddress()));
        return std::nullopt;
    }
    info.demangledName = UTF8CString(byteCast<char8_t>(unsafeSpan(demangled)));
    return info;
}

template<Materialization materialization>
RefPtr<TargetType> TargetValue<materialization>::dynamicType() const
{
    auto info = typeInfo();
    if (!info)
        return nullptr;
    return m_type->debugInfo().findTypeInImageContaining(info->vtable, info->demangledName.data());
}

template<Materialization materialization>
std::optional<TargetValue<materialization>> TargetValue<materialization>::downcast() const
{
    auto info = typeInfo();
    if (!info)
        return std::nullopt;
    RefPtr<TargetType> dynamicType = m_type->debugInfo().findTypeInImageContaining(info->vtable, info->demangledName.data());
    if (!dynamicType)
        return std::nullopt;
    return at(*m_snapshot, m_address + static_cast<uint64_t>(info->offsetToTop), dynamicType.releaseNonNull());
}

template class TargetValue<Materialization::Eager>;
template class TargetValue<Materialization::Lazy>;

} // namespace Corpse
} // namespace JSC

#endif // ENABLE(MYA_HEAP)
