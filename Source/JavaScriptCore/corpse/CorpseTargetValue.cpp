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

#if ENABLE(MYA)

#include "CorpseError.h"
#include "CorpseLimits.h"
#include "CorpseSnapshot.h"
#include "CorpseSnapshotDebugInfo.h"
#include <cxxabi.h>
#include <stdlib.h>
#include <string_view>
#include <wtf/Scope.h>
#include <wtf/StdLibExtras.h>
#include <wtf/text/StringCommon.h>

namespace JSC {
namespace Corpse {

// The two words before a vtable's address point (Itanium C++ ABI 2.5.2).
struct VTablePrefix {
    int64_t offsetToTop;
    uint64_t typeInfo;
};

// How a std::type_info starts (Itanium C++ ABI 2.9.3).
struct TypeInfoPrefix {
    uint64_t vtable;
    uint64_t name;
};

static unsigned long long forReport(Address address)
{
    return address.toTargetVMAddress();
}

TargetValue::TargetValue(const Snapshot& snapshot, Address address, Ref<TargetType>&& type, bool isValid)
    : m_snapshot(&snapshot)
    , m_address(address)
    , m_type(WTF::move(type))
    , m_isValid(isValid)
{
}

TargetValue TargetValue::at(const Snapshot& snapshot, Address address, Ref<TargetType>&& type)
{
    if (!address)
        return TargetValue(snapshot, address, WTF::move(type), false);
    if (!type->byteSize()) {
        CORPSE_REPORT("Type '%s' has no size, so nothing can be read as it", type->name());
        return TargetValue(snapshot, address, WTF::move(type), false);
    }
    return TargetValue(snapshot, address, WTF::move(type), true);
}

TargetValue TargetValue::at(Address address) const
{
    return at(*m_snapshot, address, Ref { m_type });
}

TargetValue TargetValue::offsetBy(size_t count) const
{
    if (!m_isValid)
        return invalidated();
    return at(m_address + count * m_type->byteSize());
}

TargetValue TargetValue::member(size_t offset, Ref<TargetType>&& type) const
{
    if (!m_isValid)
        return TargetValue(*m_snapshot, m_address, WTF::move(type), false);
    size_t total = m_type->byteSize();
    size_t size = type->byteSize();
    if (offset > total || size > total - offset) {
        CORPSE_REPORT("Bytes %zu to %zu are outside the %zu-byte '%s'", offset, offset + size, total, m_type->name());
        return TargetValue(*m_snapshot, m_address, WTF::move(type), false);
    }
    return at(*m_snapshot, m_address + offset, WTF::move(type));
}

TargetValue TargetValue::properField(const char* name) const
{
    if (!m_isValid)
        return invalidated();
    TargetType::Layout layout = m_type->layout();
    auto* klass = std::get_if<TargetType::Class>(&layout);
    if (!klass) {
        CORPSE_REPORT("Type '%s' is not a class, so it has no field '%s'", m_type->name(), reportableString(name));
        return invalidated();
    }
    std::string_view wanted { name };
    for (TargetType::Field& field : klass->properFields) {
        if (std::string_view { field.name.span() } == wanted)
            return member(field.offset, WTF::move(field.type));
    }
    CORPSE_REPORT("Type '%s' declares no field '%s'", m_type->name(), reportableString(name));
    return invalidated();
}

TargetValue TargetValue::field(const TargetType::Field& field) const
{
    return member(field.offset, Ref { field.type });
}

TargetValue TargetValue::base(const TargetType::Base& base) const
{
    return member(base.offset, Ref { base.type });
}

TargetValue TargetValue::element(size_t index) const
{
    if (!m_isValid)
        return invalidated();
    TargetType::Layout layout = m_type->layout();
    auto* array = std::get_if<TargetType::Array>(&layout);
    if (!array) {
        CORPSE_REPORT("Type '%s' is not an array", m_type->name());
        return invalidated();
    }
    if (index >= array->count) {
        CORPSE_REPORT("Element %zu is outside the %zu elements of '%s'", index, array->count, m_type->name());
        return invalidated();
    }
    return member(index * array->element->byteSize(), WTF::move(array->element));
}

Vector<TargetValue> TargetValue::virtualBases() const
{
    if (!m_isValid)
        return { };
    TargetType::Layout layout = m_type->layout();
    auto* klass = std::get_if<TargetType::Class>(&layout);
    if (!klass) {
        CORPSE_REPORT("Type '%s' is not a class, so it has no virtual bases", m_type->name());
        return { };
    }
    // Every class with a virtual base has a vptr, so a plain class has none.
    if (!klass->isPolymorphic)
        return { };

    TargetValue complete = downcast();
    if (!complete)
        return { };
    TargetType::Layout completeLayout = complete.type().layout();
    Vector<TargetValue> result;
    for (const TargetType::Base& base : std::get<TargetType::Class>(completeLayout).virtualBases)
        result.append(complete.base(base));
    return result;
}

std::optional<Address> TargetValue::pointerValue() const
{
    if (!m_isValid)
        return std::nullopt;
    if (!std::holds_alternative<TargetType::Pointer>(m_type->layout())) {
        CORPSE_REPORT("Type '%s' is not a pointer", m_type->name());
        return std::nullopt;
    }
    uint64_t raw = 0;
    if (!readWhole(asMutableByteSpan(raw)))
        return std::nullopt;
    return Address { raw }.stripped();
}

TargetValue TargetValue::dereference() const
{
    auto pointer = pointerValue();
    if (!pointer)
        return invalidated();
    return pointeeAt(*pointer);
}

TargetValue TargetValue::pointeeAt(Address address) const
{
    if (!m_isValid)
        return invalidated();
    TargetType::Layout layout = m_type->layout();
    auto* pointer = std::get_if<TargetType::Pointer>(&layout);
    if (!pointer) {
        CORPSE_REPORT("Type '%s' is not a pointer", m_type->name());
        return invalidated();
    }
    return at(*m_snapshot, address, WTF::move(pointer->pointee));
}

bool TargetValue::readWhole(std::span<uint8_t> destination) const
{
    if (!m_isValid)
        return false;
    if (destination.size() != m_type->byteSize()) {
        CORPSE_REPORT("Reading the %zu-byte '%s' as %zu bytes", m_type->byteSize(), m_type->name(), destination.size());
        return false;
    }
    if (m_snapshot->read(m_address, destination).empty()) {
        CORPSE_REPORT("Could not read the %zu-byte '%s' at 0x%llx", destination.size(), m_type->name(), forReport(m_address));
        return false;
    }
    return true;
}

template<typename T>
std::optional<T> TargetValue::readAt(Address address, ASCIILiteral what) const
{
    auto value = m_snapshot->read<T>(address);
    if (!value)
        CORPSE_REPORT("Could not read the %s at 0x%llx of the '%s' at 0x%llx", what, forReport(address), m_type->name(), forReport(m_address));
    return value;
}

std::optional<int64_t> TargetValue::integer() const
{
    if (!m_isValid)
        return std::nullopt;
    TargetType::Layout layout = m_type->layout();
    auto* integer = std::get_if<TargetType::Integer>(&layout);
    if (!integer) {
        CORPSE_REPORT("Type '%s' is not an integer", m_type->name());
        return std::nullopt;
    }
    size_t size = m_type->byteSize();
    if (size != 1 && size != 2 && size != 4 && size != 8) {
        CORPSE_REPORT("'%s' is a %zu-byte integer, which does not fit in 64 bits", m_type->name(), size);
        return std::nullopt;
    }
    uint64_t raw = 0;
    if (!readWhole(asMutableByteSpan(raw).first(size)))
        return std::nullopt;
    unsigned bits = size * 8;
    if (integer->isSigned && bits < 64 && ((raw >> (bits - 1)) & 1))
        raw |= ~0ull << bits;
    return static_cast<int64_t>(raw);
}

std::optional<TargetTypeInfo> TargetValue::typeInfo() const
{
    if (!m_isValid)
        return std::nullopt;
    TargetType::Layout layout = m_type->layout();
    auto* klass = std::get_if<TargetType::Class>(&layout);
    if (!klass || !klass->isPolymorphic) {
        CORPSE_REPORT("Type '%s' is not polymorphic, so it has no type_info", m_type->name());
        return std::nullopt;
    }

    auto vptr = readAt<uint64_t>(m_address, "vtable pointer"_s);
    if (!vptr)
        return std::nullopt;
    TargetTypeInfo info;
    info.vtable = Address { *vptr }.stripped();

    auto vtablePrefix = readAt<VTablePrefix>(info.vtable - sizeof(VTablePrefix), "vtable prefix"_s);
    if (!vtablePrefix)
        return std::nullopt;
    info.offsetToTop = vtablePrefix->offsetToTop;
    info.typeInfo = Address { vtablePrefix->typeInfo }.stripped();

    auto typeInfoPrefix = readAt<TypeInfoPrefix>(info.typeInfo, "type_info"_s);
    if (!typeInfoPrefix)
        return std::nullopt;

    // libc++ sets the top bit of the name pointer when the name is not unique
    // across images; libstdc++ prefixes the name with '*' instead.
    constexpr uint64_t nonUniqueBit = 1ull << 63;
    Address nameAddress = Address { typeInfoPrefix->name & ~nonUniqueBit }.stripped();
    auto name = m_snapshot->readCString(nameAddress, maxTypeNameLength);
    if (!name) {
        CORPSE_REPORT("Could not read the type_info name at 0x%llx of the '%s' at 0x%llx", forReport(nameAddress), m_type->name(), forReport(m_address));
        return std::nullopt;
    }
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
        CORPSE_REPORT("The type_info name '%s' of the object at 0x%llx does not demangle", info.mangledName, forReport(m_address));
        return std::nullopt;
    }
    info.demangledName = UTF8CString(byteCast<char8_t>(unsafeSpan(demangled)));
    return info;
}

RefPtr<TargetType> TargetValue::dynamicType() const
{
    auto info = typeInfo();
    if (!info)
        return nullptr;
    return m_type->m_debugInfo->findTypeForVTable(info->vtable, info->demangledName.data());
}

TargetValue TargetValue::downcast() const
{
    auto info = typeInfo();
    if (!info)
        return invalidated();
    RefPtr<TargetType> dynamicType = m_type->m_debugInfo->findTypeForVTable(info->vtable, info->demangledName.data());
    if (!dynamicType)
        return invalidated();
    return at(*m_snapshot, m_address + static_cast<uint64_t>(info->offsetToTop), dynamicType.releaseNonNull());
}

} // namespace Corpse
} // namespace JSC

#endif // ENABLE(MYA)
