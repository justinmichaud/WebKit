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
#include <string>
#include <string_view>
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

// The class the `index`th argument in liblldb's own name for an instance of a
// class template names, in the image that describes the instance.
static const TargetType* templateArgumentFromName(SnapshotDebugInfo& debugInfo, const TargetType& type, std::string_view name, unsigned index)
{
    size_t start = name.find('<');
    if (start == std::string_view::npos)
        return nullptr;
    unsigned depth = 0;
    unsigned argument = 0;
    for (size_t position = ++start; position < name.size(); ++position) {
        char character = name[position];
        if (character == '<' || character == '(')
            ++depth;
        else if ((character == '>' || character == ')') && depth)
            --depth;
        else if (!depth && (character == ',' || character == '>')) {
            if (argument++ == index) {
                std::string_view spelled = name.substr(start, position - start);
                if (spelled.starts_with("const "))
                    spelled.remove_prefix(6);
                return debugInfo.classNamedBeside(type, std::string { spelled }.c_str());
            }
            if (character == '>')
                return nullptr;
            start = position + 1;
            while (start < name.size() && name[start] == ' ')
                ++start;
        }
    }
    return nullptr;
}

const TargetType* TargetType::templateArgument(unsigned index) const
{
    // liblldb gives some instances no template arguments, such as one it
    // completed from another compile unit; their names still spell them.
    if (!m_type->GetNumberOfTemplateArguments()) {
        const char* name = m_type->GetName();
        return name ? templateArgumentFromName(m_debugInfo, *this, name, index) : nullptr;
    }
    if (index >= m_type->GetNumberOfTemplateArguments() || m_type->GetTemplateArgumentKind(index) != lldb::eTemplateArgumentKindType)
        return nullptr;
    lldb::SBType argument = m_type->GetTemplateArgumentType(index);
    if (!argument.IsValid())
        return nullptr;
    return &m_debugInfo.type(argument);
}

size_t TargetType::alignment() const
{
    if (m_alignment)
        return *m_alignment;
    size_t result = static_cast<size_t>(m_type->GetByteAlign());
    const Layout& layout = this->layout();
    if (auto* klass = std::get_if<Class>(&layout)) {
        auto memberAlignment = [&](const TargetType& member) {
            const TargetType* definition = &member;
            if (std::holds_alternative<Class>(member.layout()) && !member.byteSize())
                definition = m_debugInfo.definitionInItsImage(member);
            return definition ? definition->alignment() : 1;
        };
        for (const Field& field : klass->properFields)
            result = std::max(result, memberAlignment(field.type));
        for (const Base& base : klass->bases)
            result = std::max(result, memberAlignment(base.type));
        for (const Base& base : klass->virtualBases)
            result = std::max(result, memberAlignment(base.type));
    } else if (auto* array = std::get_if<Array>(&layout))
        result = std::max(result, array->element.alignment());
    else if (std::holds_alternative<Pointer>(layout) || std::holds_alternative<Integer>(layout))
        result = std::max(result, byteSize());
    m_alignment = result;
    return result;
}

size_t TargetType::readByteSize() const
{
    m_byteSize = static_cast<size_t>(m_type->GetByteSize());
    return *m_byteSize;
}

std::optional<uint64_t> TargetType::templateIntegerArgument(unsigned index) const
{
    if (index >= m_type->GetNumberOfTemplateArguments() || m_type->GetTemplateArgumentKind(index) != lldb::eTemplateArgumentKindIntegral)
        return std::nullopt;
    lldb::SBValue argument = m_type->GetTemplateArgumentValue(*m_debugInfo.m_target, index);
    if (!argument.IsValid())
        return std::nullopt;
    lldb::SBError error;
    uint64_t value = argument.GetValueAsUnsigned(error, 0);
    if (error.Fail())
        return std::nullopt;
    return value;
}

auto TargetType::enumerators() const -> Vector<Enumerator>
{
    Vector<Enumerator> result;
    lldb::SBTypeEnumMemberList members = m_type->GetEnumMembers();
    if (!members.IsValid())
        return result;
    for (uint32_t index = 0; index < members.GetSize(); ++index) {
        lldb::SBTypeEnumMember member = members.GetTypeEnumMemberAtIndex(index);
        result.append({ copyOf(member.GetName()), member.GetValueAsSigned() });
    }
    return result;
}

std::optional<int64_t> TargetType::enumeratorValue(std::string_view name) const
{
    for (const Enumerator& enumerator : enumerators()) {
        if (std::string_view { enumerator.name.legacyCStringPointer() } == name)
            return enumerator.value;
    }
    return std::nullopt;
}

const TargetType* TargetType::staticFieldType(const char* name) const
{
    lldb::SBTypeStaticField field = m_type->GetStaticFieldWithName(name);
    if (!field.IsValid())
        return nullptr;
    lldb::SBType type = field.GetType();
    if (!type.IsValid())
        return nullptr;
    return &m_debugInfo.type(type);
}

const TargetType* TargetType::returnTypeOfMemberFunction(const char* name) const
{
    uint32_t count = m_type->GetNumberOfMemberFunctions();
    for (uint32_t index = 0; index < count; ++index) {
        lldb::SBTypeMemberFunction function = m_type->GetMemberFunctionAtIndex(index);
        const char* functionName = function.GetName();
        if (!functionName || std::string_view { functionName } != name)
            continue;
        lldb::SBType returnType = function.GetReturnType();
        return returnType.IsValid() ? &m_debugInfo.type(returnType) : nullptr;
    }
    return nullptr;
}

TargetType::Layout TargetType::readLayout() const
{
    uint32_t typeClass = m_type->GetTypeClass();

    if (typeClass & (lldb::eTypeClassClass | lldb::eTypeClassStruct | lldb::eTypeClassUnion)) {
        Class result { m_type->IsPolymorphicClass(), !!(typeClass & lldb::eTypeClassUnion), { }, { }, { } };

        uint32_t fieldCount = m_type->GetNumberOfFields();
        for (uint32_t index = 0; index < fieldCount; ++index) {
            lldb::SBTypeMember member = m_type->GetFieldAtIndex(index);
            uint64_t offsetInBits = member.GetOffsetInBits();
            bool isBitfield = member.IsBitfield();
            // A flexible array member, like CStringBuffer's characters, starts where
            // the class ends: it is storage after the class, not part of it.
            if (offsetInBits / 8 >= byteSize())
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
        return Array { element, element.byteSize() ? byteSize() / element.byteSize() : 0 };
    }

    if (typeClass & lldb::eTypeClassPointer)
        return Pointer { m_debugInfo.type(m_type->GetPointeeType()) };
    if (typeClass & lldb::eTypeClassReference)
        return Pointer { m_debugInfo.type(m_type->GetDereferencedType()) };

    if (typeClass & lldb::eTypeClassEnumeration)
        return Integer { isSignedInteger(m_type->GetEnumerationIntegerType()), true };
    if ((typeClass & lldb::eTypeClassBuiltin) && isIntegerBasicType(m_type->GetBasicType()))
        return Integer { isSignedInteger(*m_type), false };

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

const TargetType* TargetType::templateArgument(unsigned) const
{
    RELEASE_ASSERT_NOT_REACHED();
}

size_t TargetType::alignment() const
{
    RELEASE_ASSERT_NOT_REACHED();
}

size_t TargetType::readByteSize() const
{
    RELEASE_ASSERT_NOT_REACHED();
}

std::optional<uint64_t> TargetType::templateIntegerArgument(unsigned) const
{
    RELEASE_ASSERT_NOT_REACHED();
}

auto TargetType::enumerators() const -> Vector<Enumerator>
{
    RELEASE_ASSERT_NOT_REACHED();
}

std::optional<int64_t> TargetType::enumeratorValue(std::string_view) const
{
    RELEASE_ASSERT_NOT_REACHED();
}

const TargetType* TargetType::staticFieldType(const char*) const
{
    RELEASE_ASSERT_NOT_REACHED();
}

const TargetType* TargetType::returnTypeOfMemberFunction(const char*) const
{
    RELEASE_ASSERT_NOT_REACHED();
}

} // namespace Corpse
} // namespace JSC

#endif // HAVE(LLDB)

#endif // ENABLE(MYA)
