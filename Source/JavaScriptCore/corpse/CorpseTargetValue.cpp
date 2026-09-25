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
#include "CorpseLimits.h"
#include "CorpseSnapshot.h"
#include "CorpseSnapshotDebugInfo.h"
#include <string_view>
#include <wtf/StdLibExtras.h>

namespace JSC {
namespace Corpse {

// Itanium C++ ABI 2.5.2: the offset to top sits two words before a vtable's
// address point, and the virtual function pointers start at the address point.
static constexpr size_t offsetToTopBeforeAddressPoint = 2 * sizeof(uint64_t);

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

TargetValue TargetValue::valueAt(size_t offset, Ref<TargetType>&& type) const
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
        CORPSE_REPORT("Type '%s' is not a class, so it has no field '%s'", m_type->name(), name);
        return invalidated();
    }
    std::string_view wanted { name };
    for (TargetType::Field& field : klass->properFields) {
        if (std::string_view { field.name.span() } == wanted)
            return valueAt(field.offset, WTF::move(field.type));
    }
    CORPSE_REPORT("Type '%s' declares no field '%s'", m_type->name(), name);
    return invalidated();
}

TargetValue TargetValue::field(const TargetType::Field& field) const
{
    return valueAt(field.offset, Ref { field.type });
}

TargetValue TargetValue::base(const TargetType::Base& base) const
{
    return valueAt(base.offset, Ref { base.type });
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
    return valueAt(index * array->element->byteSize(), WTF::move(array->element));
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
    if (!readBytes(asMutableByteSpan(raw)))
        return std::nullopt;
    return Address { raw }.stripped();
}

TargetValue TargetValue::dereference() const
{
    auto pointer = pointerValue();
    if (!pointer)
        return invalidated();
    TargetType::Layout layout = m_type->layout();
    return at(*m_snapshot, *pointer, WTF::move(std::get<TargetType::Pointer>(layout).pointee));
}

bool TargetValue::readBytes(std::span<uint8_t> destination) const
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
    if (!readBytes(asMutableByteSpan(raw).first(size)))
        return std::nullopt;
    unsigned bits = size * 8;
    if (integer->isSigned && bits < 64 && ((raw >> (bits - 1)) & 1))
        raw |= ~0ull << bits;
    return static_cast<int64_t>(raw);
}

bool TargetValue::isPolymorphic() const
{
    if (!m_isValid)
        return false;
    TargetType::Layout layout = m_type->layout();
    auto* klass = std::get_if<TargetType::Class>(&layout);
    return klass && klass->isPolymorphic;
}

RefPtr<TargetType> TargetValue::dynamicTypeAt(const Snapshot& snapshot, SnapshotDebugInfo& debugInfo, Address address, Address& completeAddress)
{
    CORPSE_DIAGNOSTICS(diagnostics, "resolving the dynamic type of the object at 0x%llx", forReport(address));

    auto vptr = snapshot.read<uint64_t>(address);
    if (!vptr) {
        CORPSE_REPORT("Could not read the vtable pointer of the object at 0x%llx", forReport(address));
        return nullptr;
    }
    Address vtable = Address { *vptr }.stripped();
    auto offsetToTop = snapshot.read<int64_t>(vtable - offsetToTopBeforeAddressPoint);
    if (!offsetToTop) {
        CORPSE_REPORT("Could not read the offset to top of the vtable at 0x%llx", forReport(vtable));
        return nullptr;
    }
    completeAddress = address + static_cast<uint64_t>(*offsetToTop);
    CORPSE_NOTE(diagnostics, "vtable at 0x%llx, offset to top %lld", forReport(vtable), static_cast<long long>(*offsetToTop));

    // A base subobject's vtable holds thunks; the complete object's own
    // vtable holds its destructor.
    if (*offsetToTop) {
        vptr = snapshot.read<uint64_t>(completeAddress);
        if (!vptr) {
            CORPSE_REPORT("Could not read the vtable pointer of the complete object at 0x%llx", forReport(completeAddress));
            return nullptr;
        }
        vtable = Address { *vptr }.stripped();
        CORPSE_NOTE(diagnostics, "complete object at 0x%llx with its vtable at 0x%llx", forReport(completeAddress), forReport(vtable));
    }

    for (size_t slot = 0; slot < maxVTableSlots; ++slot) {
        auto entry = snapshot.read<uint64_t>(vtable + slot * sizeof(uint64_t));
        if (!entry) {
            CORPSE_REPORT("Could not read slot %zu of the vtable at 0x%llx", slot, forReport(vtable));
            return nullptr;
        }
        bool inImage = false;
        RefPtr<TargetType> type = debugInfo.classOfDestructor(Address { *entry }.stripped(), inImage);
        if (type)
            return type;
        if (!inImage)
            break;
        diagnostics.count("slots that are not a destructor"_s);
    }
    CORPSE_REPORT("No destructor in the vtable at 0x%llx: its class needs a virtual destructor", forReport(vtable));
    return nullptr;
}

std::optional<TargetValue> TargetValue::completeObjectAt(const Snapshot& snapshot, SnapshotDebugInfo& debugInfo, Address address)
{
    if (!address) {
        CORPSE_REPORT("There is no object at a null address");
        return std::nullopt;
    }
    Address completeAddress;
    RefPtr<TargetType> type = dynamicTypeAt(snapshot, debugInfo, address, completeAddress);
    if (!type)
        return std::nullopt;
    return at(snapshot, completeAddress, type.releaseNonNull());
}

RefPtr<TargetType> TargetValue::dynamicType() const
{
    if (!m_isValid)
        return nullptr;
    if (!isPolymorphic()) {
        CORPSE_REPORT("Type '%s' is not polymorphic, so it has no dynamic type", m_type->name());
        return nullptr;
    }
    Address completeAddress;
    return dynamicTypeAt(*m_snapshot, m_type->m_debugInfo, m_address, completeAddress);
}

TargetValue TargetValue::downcast() const
{
    if (!m_isValid)
        return invalidated();
    if (!isPolymorphic()) {
        CORPSE_REPORT("Type '%s' is not polymorphic, so it has no dynamic type", m_type->name());
        return invalidated();
    }
    Address completeAddress;
    RefPtr<TargetType> type = dynamicTypeAt(*m_snapshot, m_type->m_debugInfo, m_address, completeAddress);
    if (!type)
        return invalidated();
    return at(*m_snapshot, completeAddress, type.releaseNonNull());
}

} // namespace Corpse
} // namespace JSC

#endif // ENABLE(MYA_HEAP)
