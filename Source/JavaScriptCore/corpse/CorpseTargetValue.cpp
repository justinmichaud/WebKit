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
#include "CorpseTargetType.h"
#include <string_view>
#include <wtf/StdLibExtras.h>

namespace JSC {
namespace Corpse {

static unsigned long long forReport(Address address)
{
    return address.toTargetVMAddress();
}

TargetValue::TargetValue(Snapshot& snapshot, Address address, const TargetType& type, bool isValid)
    : m_snapshot(&snapshot)
    , m_address(address)
    , m_debugInfo(type.debugInfo())
    , m_type(&type)
    , m_isValid(isValid)
{
}

TargetValue TargetValue::at(Snapshot& snapshot, Address address, const TargetType& type)
{
    if (!address)
        return TargetValue(snapshot, address, type, false);
    if (!type.byteSize()) {
        CORPSE_REPORT("Type '%s' has no size, so nothing can be read as it", type.name());
        return TargetValue(snapshot, address, type, false);
    }
    return TargetValue(snapshot, address, type, true);
}

TargetValue TargetValue::at(Address address) const
{
    return at(*m_snapshot, address, *m_type);
}

TargetValue TargetValue::offsetBy(size_t count) const
{
    if (!m_isValid)
        return invalidated();
    return at(m_address + count * m_type->byteSize());
}

TargetValue TargetValue::member(size_t offset, const TargetType& type) const
{
    if (!m_isValid)
        return TargetValue(*m_snapshot, m_address, type, false);
    size_t total = m_type->byteSize();
    size_t size = type.byteSize();
    if (offset > total || size > total - offset) {
        CORPSE_REPORT("Bytes %zu to %zu are outside the %zu-byte '%s'", offset, offset + size, total, m_type->name());
        return TargetValue(*m_snapshot, m_address, type, false);
    }
    return at(*m_snapshot, m_address + offset, type);
}

TargetValue TargetValue::properField(const char* name) const
{
    if (!m_isValid)
        return invalidated();
    const TargetType::Layout& layout = m_type->layout();
    auto* klass = std::get_if<TargetType::Class>(&layout);
    if (!klass) {
        CORPSE_REPORT("Type '%s' is not a class, so it has no field '%s'", m_type->name(), name);
        return invalidated();
    }
    std::string_view wanted { name };
    for (const TargetType::Field& field : klass->properFields) {
        if (std::string_view { field.name.legacyCStringPointer() } == wanted)
            return this->field(field);
    }
    CORPSE_REPORT("Type '%s' declares no field '%s'", m_type->name(), name);
    return invalidated();
}

TargetValue TargetValue::field(const TargetType::Field& field) const
{
    if (!field.bitSize)
        return member(field.offset, field.type);
    if (!m_isValid)
        return TargetValue(*m_snapshot, m_address, field.type, false);
    TargetValue value(*m_snapshot, m_address + field.offset, field.type, true);
    value.m_bitOffset = field.bitOffset;
    value.m_bitSize = field.bitSize;
    size_t total = m_type->byteSize();
    size_t size = value.bitfieldByteSize();
    if (size > sizeof(uint64_t) || field.offset > total || size > total - field.offset) {
        CORPSE_REPORT("The %u-bit field '%s' at bit %u of byte %zu does not fit in the %zu-byte '%s'", field.bitSize, field.name.legacyCStringPointer(), field.bitOffset, field.offset, total, m_type->name());
        return value.invalidated();
    }
    return value;
}

TargetValue TargetValue::base(const TargetType::Base& base) const
{
    return member(base.offset, base.type);
}

void TargetValue::forEachField(const Function<void(const TargetType::Field&, const TargetValue&)>& functor) const
{
    forEachNonVirtualField(functor);
    const TargetType::Layout& layout = m_type->layout();
    if (auto* klass = std::get_if<TargetType::Class>(&layout)) {
        for (const TargetType::Base& virtualBase : klass->virtualBases)
            base(virtualBase).forEachNonVirtualField(functor);
    }
}

void TargetValue::forEachNonVirtualField(const Function<void(const TargetType::Field&, const TargetValue&)>& functor) const
{
    if (!m_isValid)
        return;
    const TargetType::Layout& layout = m_type->layout();
    auto* klass = std::get_if<TargetType::Class>(&layout);
    if (!klass)
        return;
    for (const TargetType::Field& field : klass->properFields)
        functor(field, this->field(field));
    for (const TargetType::Base& base : klass->bases)
        this->base(base).forEachNonVirtualField(functor);
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
    const TargetType::Layout& layout = m_type->layout();
    auto* pointer = std::get_if<TargetType::Pointer>(&layout);
    if (!pointer) {
        CORPSE_REPORT("Type '%s' is not a pointer", m_type->name());
        return invalidated();
    }
    return at(*m_snapshot, address, pointer->pointee);
}

bool TargetValue::readWhole(std::span<uint8_t> destination) const
{
    if (!m_isValid)
        return false;
    if (m_bitSize) {
        CORPSE_REPORT("The %u-bit field of type '%s' at 0x%llx can only be read as an integer", m_bitSize, m_type->name(), forReport(m_address));
        return false;
    }
    if (destination.size() != m_type->byteSize()) {
        CORPSE_REPORT("Reading the %zu-byte '%s' as %zu bytes", m_type->byteSize(), m_type->name(), destination.size());
        return false;
    }
    auto bytes = m_snapshot->memory().span<uint8_t>(m_address, destination.size());
    if (!bytes) {
        CORPSE_REPORT("Could not read the %zu-byte '%s' at 0x%llx", destination.size(), m_type->name(), forReport(m_address));
        return false;
    }
    memcpySpan(destination, std::span<const uint8_t> { bytes });
    return true;
}

// The low `bits` bits of `raw`, as an integer of that width.
static int64_t signExtend(uint64_t raw, unsigned bits, bool isSigned)
{
    if (bits < 64)
        raw &= (1ull << bits) - 1;
    if (isSigned && bits < 64 && ((raw >> (bits - 1)) & 1))
        raw |= ~0ull << bits;
    return static_cast<int64_t>(raw);
}

std::optional<int64_t> TargetValue::integer() const
{
    if (!m_isValid)
        return std::nullopt;
    const TargetType::Layout& layout = m_type->layout();
    auto* integer = std::get_if<TargetType::Integer>(&layout);
    if (!integer) {
        CORPSE_REPORT("Type '%s' is not an integer", m_type->name());
        return std::nullopt;
    }
    if (m_bitSize) {
        auto bytes = m_snapshot->memory().span<uint8_t>(m_address, bitfieldByteSize());
        if (!bytes) {
            CORPSE_REPORT("Could not read the %u-bit field of type '%s' at 0x%llx", m_bitSize, m_type->name(), forReport(m_address));
            return std::nullopt;
        }
        uint64_t raw = 0;
        memcpySpan(asMutableByteSpan(raw).first(bytes.size()), std::span<const uint8_t> { bytes });
        return signExtend(raw >> m_bitOffset, m_bitSize, integer->isSigned);
    }
    size_t size = m_type->byteSize();
    if (size != 1 && size != 2 && size != 4 && size != 8) {
        CORPSE_REPORT("'%s' is a %zu-byte integer, which does not fit in 64 bits", m_type->name(), size);
        return std::nullopt;
    }
    uint64_t raw = 0;
    if (!readWhole(asMutableByteSpan(raw).first(size)))
        return std::nullopt;
    return signExtend(raw, size * 8, integer->isSigned);
}


std::optional<TargetValue> TargetValue::completeObjectAt(Snapshot& snapshot, SnapshotDebugInfo& debugInfo, Address address)
{
    Address completeObject;
    auto* type = debugInfo.dynamicTypeAt(snapshot, address, completeObject);
    if (!type)
        return std::nullopt;
    return at(snapshot, completeObject, *type);
}

const TargetType* TargetValue::dynamicType(Address& completeObject) const
{
    if (!m_isValid)
        return nullptr;
    const TargetType::Layout& layout = m_type->layout();
    auto* klass = std::get_if<TargetType::Class>(&layout);
    if (!klass || !klass->isPolymorphic) {
        CORPSE_REPORT("Type '%s' is not polymorphic, so it has no dynamic type", m_type->name());
        return nullptr;
    }
    return m_type->debugInfo().dynamicTypeAt(*m_snapshot, m_address, completeObject);
}

} // namespace Corpse
} // namespace JSC

#endif // ENABLE(MYA)
