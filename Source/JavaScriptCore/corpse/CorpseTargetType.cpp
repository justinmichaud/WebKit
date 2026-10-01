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

#if ENABLE(MYA)

#include "CorpseSnapshotDebugInfo.h"
#include <wtf/TZoneMallocInlines.h>

#if HAVE(LLDB)

#include <lldb/API/LLDB.h>
#include <wtf/StdLibExtras.h>
#include <wtf/text/StringCommon.h>

namespace JSC {
namespace Corpse {

WTF_MAKE_TZONE_ALLOCATED_IMPL(TargetType);

// A copy of a string liblldb owns, since it stays the type's to keep. An
// anonymous member has no name, and is found by the empty one.
static UTF8CString copyOf(const char* string)
{
    if (!string)
        return UTF8CString { ""_s };
    return UTF8CString(byteCast<char8_t>(unsafeSpan(string)));
}

TargetType::TargetType(SnapshotDebugInfo& debugInfo, const lldb::SBType& canonicalType)
    : m_debugInfo(debugInfo)
    , m_type(makeUniqueWithoutFastMallocCheck<lldb::SBType>(canonicalType))
    , m_byteSize(m_type->GetByteSize())
{
}

TargetType::~TargetType() = default;

UTF8CString TargetType::name() const
{
    return copyOf(m_type->GetName());
}

static bool isSignedInteger(lldb::SBType type)
{
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

static bool isIntegerBasicType(lldb::BasicType type)
{
    switch (type) {
    case lldb::eBasicTypeBool:
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
        return true;
    default:
        return false;
    }
}

const TargetType::Layout& TargetType::layout() const
{
    if (!m_layout)
        m_layout.emplace(readLayout());
    return *m_layout;
}

const TargetType& TargetType::home() const
{
    if (!m_home)
        m_home = &m_debugInfo.homeOf(*this);
    return *m_home;
}

const TargetType* TargetType::templateArgument(unsigned index) const
{
    if (index >= m_type->GetNumberOfTemplateArguments() || m_type->GetTemplateArgumentKind(index) != lldb::eTemplateArgumentKindType)
        return nullptr;
    lldb::SBType argument = m_type->GetTemplateArgumentType(index);
    if (!argument.IsValid())
        return nullptr;
    return &m_debugInfo.type(argument);
}

TargetType::Layout TargetType::readLayout() const
{
    uint32_t typeClass = m_type->GetTypeClass();

    if (typeClass & (lldb::eTypeClassClass | lldb::eTypeClassStruct | lldb::eTypeClassUnion)) {
        Class result { m_type->IsPolymorphicClass(), { }, { }, { } };

        uint32_t fieldCount = m_type->GetNumberOfFields();
        for (uint32_t index = 0; index < fieldCount; ++index) {
            lldb::SBTypeMember member = m_type->GetFieldAtIndex(index);
            uint64_t offsetInBits = member.GetOffsetInBits();
            bool isBitfield = member.IsBitfield();
            // A flexible array member, like CStringBuffer's characters, starts where
            // the class ends: it is storage after the class, not part of it.
            if (offsetInBits / 8 >= m_byteSize)
                continue;
            result.properFields.append(Field {
                copyOf(member.GetName()),
                static_cast<size_t>(offsetInBits / 8),
                m_debugInfo.type(member.GetType()),
                isBitfield ? static_cast<unsigned>(offsetInBits % 8) : 0,
                isBitfield ? member.GetBitfieldSizeInBits() : 0,
            });
        }

        uint32_t virtualBaseCount = m_type->GetNumberOfVirtualBaseClasses();
        for (uint32_t index = 0; index < virtualBaseCount; ++index) {
            lldb::SBTypeMember member = m_type->GetVirtualBaseClassAtIndex(index);
            result.virtualBases.append(Base { static_cast<size_t>(member.GetOffsetInBytes()), m_debugInfo.type(member.GetType()) });
        }

        uint32_t baseCount = m_type->GetNumberOfDirectBaseClasses();
        for (uint32_t index = 0; index < baseCount; ++index) {
            lldb::SBTypeMember member = m_type->GetDirectBaseClassAtIndex(index);
            lldb::SBType baseType = member.GetType();
            // liblldb lists a direct virtual base among the direct bases as well.
            bool isVirtual = false;
            for (uint32_t virtualIndex = 0; virtualIndex < virtualBaseCount && !isVirtual; ++virtualIndex) {
                lldb::SBType virtualBaseType = m_type->GetVirtualBaseClassAtIndex(virtualIndex).GetType();
                isVirtual = baseType == virtualBaseType;
            }
            if (isVirtual)
                continue;
            result.bases.append(Base { static_cast<size_t>(member.GetOffsetInBytes()), m_debugInfo.type(baseType) });
        }
        return result;
    }

    if (typeClass & lldb::eTypeClassArray) {
        const TargetType& element = m_debugInfo.type(m_type->GetArrayElementType());
        return Array { element, element.byteSize() ? m_byteSize / element.byteSize() : 0 };
    }

    if (typeClass & lldb::eTypeClassPointer)
        return Pointer { m_debugInfo.type(m_type->GetPointeeType()) };
    if (typeClass & lldb::eTypeClassReference)
        return Pointer { m_debugInfo.type(m_type->GetDereferencedType()) };

    if (typeClass & lldb::eTypeClassEnumeration)
        return Integer { isSignedInteger(m_type->GetEnumerationIntegerType()) };
    if ((typeClass & lldb::eTypeClassBuiltin) && isIntegerBasicType(m_type->GetBasicType()))
        return Integer { isSignedInteger(*m_type) };

    return Other { };
}

} // namespace Corpse
} // namespace JSC

#else // HAVE(LLDB)

// A complete stand-in for the liblldb class the member points to, so that the
// destructor compiles. No TargetType is made without liblldb.
namespace lldb {
class SBType { };
}

namespace JSC {
namespace Corpse {

WTF_MAKE_TZONE_ALLOCATED_IMPL(TargetType);

TargetType::~TargetType() = default;

UTF8CString TargetType::name() const
{
    RELEASE_ASSERT_NOT_REACHED();
}

const TargetType::Layout& TargetType::layout() const
{
    RELEASE_ASSERT_NOT_REACHED();
}

const TargetType& TargetType::home() const
{
    RELEASE_ASSERT_NOT_REACHED();
}

const TargetType* TargetType::templateArgument(unsigned) const
{
    RELEASE_ASSERT_NOT_REACHED();
}

} // namespace Corpse
} // namespace JSC

#endif // HAVE(LLDB)

#endif // ENABLE(MYA)
