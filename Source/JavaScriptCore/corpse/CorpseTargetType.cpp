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
#include "CorpseTargetType.h"

#if ENABLE(MYA_HEAP)

#include "CorpseError.h"
#include <lldb/API/LLDB.h>
#include <string_view>
#include <wtf/StdLibExtras.h>
#include <wtf/TZoneMallocInlines.h>
#include <wtf/text/StringCommon.h>

namespace JSC {
namespace Corpse {

WTF_MAKE_TZONE_ALLOCATED_IMPL(TargetType);

TargetType::TargetType(TargetDebugInfo& debugInfo, const lldb::SBType& type)
    : m_debugInfo(debugInfo)
    , m_type(makeUniqueWithoutFastMallocCheck<lldb::SBType>(lldb::SBType(type).GetCanonicalType()))
{
}

TargetType::~TargetType() = default;

Ref<TargetType> TargetType::wrap(const lldb::SBType& type) const
{
    return adoptRef(*new TargetType(m_debugInfo.get(), type));
}

TargetType::Kind TargetType::kind() const
{
    uint32_t typeClass = m_type->GetTypeClass();
    if (typeClass & (lldb::eTypeClassClass | lldb::eTypeClassStruct | lldb::eTypeClassUnion))
        return Kind::Class;
    if (typeClass & lldb::eTypeClassPointer)
        return Kind::Pointer;
    if (typeClass & lldb::eTypeClassReference)
        return Kind::Reference;
    if (typeClass & lldb::eTypeClassArray)
        return Kind::Array;
    if (typeClass & lldb::eTypeClassEnumeration)
        return Kind::Enum;
    if (!(typeClass & lldb::eTypeClassBuiltin))
        return Kind::Other;

    switch (m_type->GetBasicType()) {
    case lldb::eBasicTypeBool:
        return Kind::Bool;
    case lldb::eBasicTypeHalf:
    case lldb::eBasicTypeFloat:
    case lldb::eBasicTypeDouble:
    case lldb::eBasicTypeLongDouble:
        return Kind::Float;
    case lldb::eBasicTypeChar:
    case lldb::eBasicTypeSignedChar:
    case lldb::eBasicTypeUnsignedChar:
    case lldb::eBasicTypeWChar:
    case lldb::eBasicTypeSignedWChar:
    case lldb::eBasicTypeUnsignedWChar:
    case lldb::eBasicTypeChar16:
    case lldb::eBasicTypeChar32:
    case lldb::eBasicTypeChar8:
    case lldb::eBasicTypeShort:
    case lldb::eBasicTypeUnsignedShort:
    case lldb::eBasicTypeInt:
    case lldb::eBasicTypeUnsignedInt:
    case lldb::eBasicTypeLong:
    case lldb::eBasicTypeUnsignedLong:
    case lldb::eBasicTypeLongLong:
    case lldb::eBasicTypeUnsignedLongLong:
    case lldb::eBasicTypeInt128:
    case lldb::eBasicTypeUnsignedInt128:
        return Kind::Integer;
    default:
        return Kind::Other;
    }
}

CString TargetType::name() const
{
    return reportableString(m_type->GetName());
}

size_t TargetType::byteSize() const
{
    return m_type->GetByteSize();
}

bool TargetType::isSigned() const
{
    lldb::SBType type = *m_type;
    if (kind() == Kind::Enum)
        type = m_type->GetEnumerationIntegerType();

    switch (type.GetBasicType()) {
    case lldb::eBasicTypeSignedChar:
    case lldb::eBasicTypeSignedWChar:
    case lldb::eBasicTypeShort:
    case lldb::eBasicTypeInt:
    case lldb::eBasicTypeLong:
    case lldb::eBasicTypeLongLong:
    case lldb::eBasicTypeInt128:
        return true;
    case lldb::eBasicTypeChar:
    case lldb::eBasicTypeWChar:
        // Plain char and wchar_t are unsigned in the AArch64 Linux ABI.
#if OS(LINUX) && CPU(ARM64)
        return false;
#else
        return true;
#endif
    default:
        return false;
    }
}

bool TargetType::isPolymorphic() const
{
    return m_type->IsPolymorphicClass();
}

Vector<TargetType::Field> TargetType::fields() const
{
    Vector<Field> result;
    uint32_t count = m_type->GetNumberOfFields();
    result.reserveInitialCapacity(count);
    for (uint32_t index = 0; index < count; ++index) {
        lldb::SBTypeMember member = m_type->GetFieldAtIndex(index);
        result.append(Field { reportableString(member.GetName()), static_cast<size_t>(member.GetOffsetInBytes()), wrap(member.GetType()) });
    }
    return result;
}

Vector<TargetType::Base> TargetType::bases() const
{
    Vector<Base> virtualBases = this->virtualBases();
    Vector<Base> result;
    uint32_t count = m_type->GetNumberOfDirectBaseClasses();
    for (uint32_t index = 0; index < count; ++index) {
        lldb::SBTypeMember member = m_type->GetDirectBaseClassAtIndex(index);
        Ref<TargetType> base = wrap(member.GetType());
        // liblldb lists a direct virtual base among the direct bases as well.
        if (virtualBases.containsIf([&](const Base& virtualBase) { return virtualBase.type->name() == base->name(); }))
            continue;
        result.append(Base { static_cast<size_t>(member.GetOffsetInBytes()), WTF::move(base) });
    }
    return result;
}

Vector<TargetType::Base> TargetType::virtualBases() const
{
    Vector<Base> result;
    uint32_t count = m_type->GetNumberOfVirtualBaseClasses();
    result.reserveInitialCapacity(count);
    for (uint32_t index = 0; index < count; ++index) {
        lldb::SBTypeMember member = m_type->GetVirtualBaseClassAtIndex(index);
        result.append(Base { static_cast<size_t>(member.GetOffsetInBytes()), wrap(member.GetType()) });
    }
    return result;
}

std::optional<TargetType::Field> TargetType::field(const char* name) const
{
    std::string_view wanted { name };
    Vector<Field> own = fields();
    for (const Field& field : own) {
        if (std::string_view { field.name.span() } == wanted)
            return field;
    }
    Vector<Base> bases = this->bases();
    for (const Base& base : bases) {
        if (auto found = base.type->field(name)) {
            found->offset += base.offset;
            return found;
        }
    }
    return std::nullopt;
}

RefPtr<TargetType> TargetType::pointee() const
{
    switch (kind()) {
    case Kind::Pointer:
        return wrap(m_type->GetPointeeType());
    case Kind::Reference:
        return wrap(m_type->GetDereferencedType());
    default:
        return nullptr;
    }
}

RefPtr<TargetType> TargetType::element() const
{
    if (kind() != Kind::Array)
        return nullptr;
    return wrap(m_type->GetArrayElementType());
}

size_t TargetType::elementCount() const
{
    RefPtr<TargetType> elementType = element();
    if (!elementType)
        return 0;
    size_t elementSize = elementType->byteSize();
    return elementSize ? byteSize() / elementSize : 0;
}

} // namespace Corpse
} // namespace JSC

#endif // ENABLE(MYA_HEAP)
