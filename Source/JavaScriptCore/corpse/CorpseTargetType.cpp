/*
 * Copyright (C) 2026 Apple Inc. All rights reserved.
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

#if HAVE(CORPSE_SUPPORT)

#include "CorpseProcess.h"
#include "CorpseSnapshot.h"
#include "CorpseError.h"
#include "CorpseTargetObject.h"
#include "CorpseTypeInfo.h"
#include "CorpseTypeName.h"

#include <string.h>
#include <unistd.h>
#include <wtf/Assertions.h>
#include <wtf/StdLibExtras.h>
#include <wtf/TZoneMallocInlines.h>
#include <wtf/text/CString.h>
#include <wtf/text/MakeString.h>
#include <wtf/text/StringHash.h>

#if HAVE(CORPSE_TYPE_SYSTEM)
#include <mutex>
#if OS(DARWIN)
// Part of the SDK, and reached as a framework.
#include <LLDB/LLDB.h>
#else
#include <lldb/API/LLDB.h>
#endif
#endif

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC {
namespace Corpse {

TargetType::TargetType(String name, uint64_t byteSize, Vector<TargetField>&& fields,
    bool isPolymorphic)
    : m_name(WTF::move(name))
    , m_byteSize(byteSize)
    , m_fields(WTF::move(fields))
    , m_isPolymorphic(isPolymorphic)
{
}

const TargetField* TargetType::field(StringView name) const
{
    for (const auto& f : m_fields) {
        if (f.name == name)
            return &f;
    }
    return nullptr;
}

#if HAVE(CORPSE_TYPE_SYSTEM)

namespace {

void ensureLLDBInitialized()
{
    static std::once_flag flag;
    std::call_once(flag, [] {
        lldb::SBDebugger::Initialize();
    });
}

// Whether a member holds the address of something else, which is what a walk
// over a graph of objects follows.
bool isPointerOrReference(lldb::SBType type)
{
    lldb::SBType canonical = type.GetCanonicalType();
    return canonical.IsValid() && (canonical.IsPointerType() || canonical.IsReferenceType());
}

// Anonymous unions and structs are recursed into so their members appear at the
// enclosing offset, and so are base classes, so that a class holding no members
// of its own does not describe its objects as having none. A name a derived
// class reuses from a base appears twice; bases come first, so field() answers
// with the base's.
void collectFields(lldb::SBType type, uint64_t baseOffset, Vector<TargetField>& out)
{
    for (uint32_t i = 0; i < type.GetNumberOfDirectBaseClasses(); ++i) {
        auto base = type.GetDirectBaseClassAtIndex(i);
        auto baseType = base.GetType();
        if (!baseType.IsValid())
            continue;
        collectFields(baseType, baseOffset + base.GetOffsetInBytes(), out);
    }

    for (uint32_t i = 0; i < type.GetNumberOfFields(); ++i) {
        auto member = type.GetFieldAtIndex(i);
        const char* name = member.GetName();
        auto memberType = member.GetType();
        uint64_t offset = baseOffset + member.GetOffsetInBytes();
        if (!name || !*name) {
            auto kind = memberType.GetTypeClass();
            if (kind == lldb::eTypeClassUnion || kind == lldb::eTypeClassStruct
                || kind == lldb::eTypeClassClass) {
                collectFields(memberType, offset, out);
                continue;
            }
        }
        const char* typeName = memberType.GetName();
        out.append({
            String::fromLatin1(name ? name : ""),
            String::fromLatin1(typeName ? typeName : ""),
            offset,
            static_cast<uint64_t>(memberType.GetByteSize()),
            isPointerOrReference(memberType),
        });
    }
}

// Looks through the same flattening collectFields applies, so that the members
// fields() reports and the members this resolves stay one set.
lldb::SBType findMemberType(lldb::SBType type, StringView memberName)
{
    for (uint32_t i = 0; i < type.GetNumberOfDirectBaseClasses(); ++i) {
        auto base = type.GetDirectBaseClassAtIndex(i).GetType();
        if (!base.IsValid())
            continue;
        lldb::SBType found = findMemberType(base, memberName);
        if (found.IsValid())
            return found;
    }

    for (uint32_t i = 0; i < type.GetNumberOfFields(); ++i) {
        auto member = type.GetFieldAtIndex(i);
        auto candidate = member.GetType();
        const char* name = member.GetName();
        if (!name || !*name) {
            auto kind = candidate.GetTypeClass();
            if (kind == lldb::eTypeClassUnion || kind == lldb::eTypeClassStruct
                || kind == lldb::eTypeClassClass) {
                lldb::SBType found = findMemberType(candidate, memberName);
                if (found.IsValid())
                    return found;
            }
            continue;
        }
        if (memberName == StringView::fromLatin1(name))
            return candidate;
    }
    return { };
}

} // anonymous namespace

class TypeSystem::Impl {
    WTF_MAKE_TZONE_ALLOCATED(Impl);
public:
    Impl(Snapshot& snapshot)
        : m_snapshot(snapshot)
        , m_typeInfo(snapshot)
    {
    }

    bool initialize()
    {
        Process* process = m_snapshot.process();
        if (!process)
            return false;

        m_debugger = lldb::SBDebugger::Create();
        if (!m_debugger.IsValid())
            return false;

        CString exePath = process->executablePath();
        if (exePath.isNull())
            return false;

        lldb::SBError err;
        m_target = m_debugger.CreateTarget(exePath.data(), nullptr, nullptr, true, err);
        if (!m_target.IsValid())
            return false;

        // The target is built from files and is never attached to, launched, or
        // otherwise made to own the process: mya owns the target's lifecycle,
        // and a debugger that attached would stop it out from under us. Only
        // debug info is read here; every address comes from the corpse.
        RELEASE_ASSERT_WITH_MESSAGE(!m_target.GetProcess().IsValid(),
            "The corpse type system must never attach to a process.");

        // Bring every loaded image into the target so FindTypes reaches types
        // defined in libraries the executable does not statically list.
        for (const auto& image : m_snapshot.loadedImages()) {
            if (image.path.isNull())
                continue;
            lldb::SBModule module = m_target.FindModule(lldb::SBFileSpec(image.path.data(), true));
            if (!module.IsValid())
                module = m_target.AddModule(image.path.data(), nullptr, nullptr);
            if (!module.IsValid())
                continue; // An image with no file behind it, such as the vDSO.

            // A target built from files on disk starts out with the addresses
            // those files were linked at, so an image has to be told how far it
            // was slid for the addresses it reports to be the corpse's. Images
            // LLDB already places are left alone: its answer comes from the
            // machinery that read the image, this one from the local loader.
            if (module.GetObjectFileHeaderAddress().GetLoadAddress(m_target) == LLDB_INVALID_ADDRESS)
                m_target.SetModuleLoadAddress(module, static_cast<int64_t>(image.slide));
        }
        return true;
    }

    std::optional<TargetType> findType(StringView qualifiedName)
    {
        lldb::SBType type = findCompleteType(qualifiedName);
        if (!type.IsValid())
            return std::nullopt;
        return buildTargetType(type);
    }

    std::optional<TargetType> findTypeOfPointee(StringView variableName)
    {
        auto utf8 = variableName.utf8();
        lldb::SBValueList variables = m_target.FindGlobalVariables(utf8.data(), 1);
        if (!variables.GetSize())
            return std::nullopt;
        lldb::SBType pointer = variables.GetValueAtIndex(0).GetType();
        if (!pointer.IsValid() || !pointer.IsPointerType())
            return std::nullopt;
        return buildReferencedType(pointer.GetPointeeType());
    }

    std::optional<TargetType> findTypeOfMember(StringView qualifiedTypeName, StringView memberName)
    {
        lldb::SBType member = memberType(qualifiedTypeName, memberName);
        if (!member.IsValid())
            return std::nullopt;
        return buildReferencedType(member);
    }

    std::optional<TargetType> findTypeOfMemberPointee(StringView qualifiedTypeName,
        StringView memberName)
    {
        lldb::SBType member = memberType(qualifiedTypeName, memberName);
        if (!member.IsValid())
            return std::nullopt;
        if (!isPointerOrReference(member))
            return std::nullopt;
        return buildReferencedType(member.GetCanonicalType().GetPointeeType());
    }

    // The first complete type named `qualifiedName`. FindTypes returns every
    // matching name across modules, forward declarations included, and a
    // declaration describes no layout. What tells a definition from one of
    // those is IsTypeComplete: counting data members would also reject a
    // definition that has none of its own, such as a class whose members all
    // come from a base, or which holds only static members.
    lldb::SBType findCompleteType(StringView qualifiedName)
    {
        auto utf8 = qualifiedName.utf8();
        auto types = m_target.FindTypes(utf8.data());
        for (uint32_t i = 0; i < types.GetSize(); ++i) {
            auto candidate = types.GetTypeAtIndex(i);
            if (candidate.IsValid() && candidate.IsTypeComplete())
                return candidate;
        }
        return { };
    }

    // The type of `memberName` within `qualifiedTypeName`, as the debug info
    // gives it.
    lldb::SBType memberType(StringView qualifiedTypeName, StringView memberName)
    {
        lldb::SBType owner = findCompleteType(qualifiedTypeName);
        if (!owner.IsValid())
            return { };
        return findMemberType(owner, memberName);
    }

    // A type reached through the debug info rather than asked for by name.
    // Const, volatile and typedefs belong to where it was reached from rather
    // than to the layout. What is left may still be incomplete, since a pointer
    // needs only a declaration of its pointee, but its name is the compiler's
    // own spelling, so the definition is looked up by that.
    std::optional<TargetType> buildReferencedType(lldb::SBType type)
    {
        lldb::SBType resolved = type.GetUnqualifiedType().GetCanonicalType();
        if (!resolved.IsValid())
            return std::nullopt;
        if (resolved.IsTypeComplete())
            return buildTargetType(resolved);
        const char* resolvedName = resolved.GetName();
        if (!resolvedName || !*resolvedName)
            return std::nullopt;
        return findType(StringView::fromLatin1(resolvedName));
    }

    std::optional<TargetType> buildTargetType(lldb::SBType type)
    {
        const char* resolvedName = type.GetName();
        String name = String::fromLatin1(resolvedName ? resolvedName : "");

        // Nothing about the vtable is resolved here. An object carries its own
        // type_info, so what a type's objects look like is a question asked of
        // the object rather than looked up ahead of it.
        Vector<TargetField> fields;
        collectFields(type, 0, fields);
        return TargetType {
            WTF::move(name),
            static_cast<uint64_t>(type.GetByteSize()),
            WTF::move(fields),
            type.IsPolymorphicClass(),
        };
    }

    std::optional<TargetObject> getTargetObject(Address base, const TargetType& type)
    {
        auto bytes = m_snapshot.readBytes(base, type.byteSize());
        if (!bytes)
            return std::nullopt;

        // A type with isPolymorphic() always carries an expected vptr, since
        // findType refuses one whose vtable does not resolve.
        String dynamicTypeName;
        if (type.isPolymorphic()) {
            if (bytes->size() < sizeof(uintptr_t))
                return std::nullopt;
            uintptr_t raw = 0;
            memcpy(&raw, bytes->span().data(), sizeof(uintptr_t));
            // A signed vptr arrives with its signature on; a symbol address has
            // none. Stripping is nothing where they are not signed.
            Address vptr = Address(raw).stripped();

            // The class itself, then everything it derives from. Reading a
            // derived object through a base is sound and is the ordinary case
            // when walking a heap: a pointer to a base almost always addresses
            // something more derived.
            auto hierarchy = m_typeInfo.mangledHierarchyForVPtr(vptr);
            String wanted = TypeName::normalizeTypeName(type.name());
            String actual = hierarchy.isEmpty() ? String { }
                : demangleAndNormalize(hierarchy.first());
            if (actual.isEmpty()) {
                reportVPtrMismatch(base, type, vptr);
                return std::nullopt;
            }

            if (actual != wanted) {
                bool derivesFromWanted = false;
                for (size_t i = 1; i < hierarchy.size() && !derivesFromWanted; ++i)
                    derivesFromWanted = demangleAndNormalize(hierarchy[i]) == wanted;
                if (!derivesFromWanted) {
                    reportVPtrMismatch(base, type, vptr);
                    return std::nullopt;
                }
                dynamicTypeName = WTF::move(actual);
            }
        }
        return TargetObject { base, type, WTF::move(*bytes), WTF::move(dynamicTypeName) };
    }

    // Says what is actually at an address whose vptr did not match, which is
    // the finding when a walk over a corrupt heap goes wrong. "Not the type you
    // asked for" and "not an object at all" call for different next steps, and
    // only the index can tell them apart.
    void reportVPtrMismatch(Address base, const TargetType& type, Address vptr)
    {
        auto hierarchy = m_typeInfo.mangledHierarchyForVPtr(vptr);
        String actual = hierarchy.isEmpty() ? String { }
            : demangleAndNormalize(hierarchy.first());
        if (!actual.isEmpty()) {
            Error::report("0x%llx is not a %s: its vptr 0x%llx belongs to %s",
                static_cast<unsigned long long>(base.value()), type.name().utf8().data(),
                static_cast<unsigned long long>(vptr.value()), actual.utf8().data());
            return;
        }
        if (!m_snapshot.regionContaining(vptr)) {
            Error::report("0x%llx is not a %s: its vptr 0x%llx is not mapped in the corpse",
                static_cast<unsigned long long>(base.value()), type.name().utf8().data(),
                static_cast<unsigned long long>(vptr.value()));
            return;
        }
        Error::report("0x%llx is not a %s: its vptr 0x%llx addresses no vtable the target defines",
            static_cast<unsigned long long>(base.value()), type.name().utf8().data(),
            static_cast<unsigned long long>(vptr.value()));
    }

private:
    // A name out of a type_info is mangled; a name out of the debug info is
    // spelled. Comparing them means bringing both to the same form.
    String demangleAndNormalize(const String& mangled)
    {
        auto demangled = TypeName::demangleType(mangled.utf8().data());
        if (!demangled)
            return { };
        return TypeName::normalizeTypeName(*demangled);
    }

    Snapshot& m_snapshot;

    // Where identity comes from. The target is built with RTTI, so an object
    // names its own class and lists its own bases, and neither the image files
    // nor the debug info is consulted for either.
    TypeInfoReader m_typeInfo;
    lldb::SBDebugger m_debugger;
    lldb::SBTarget m_target;
};

WTF_MAKE_TZONE_ALLOCATED_IMPL(TypeSystem::Impl);

std::unique_ptr<TypeSystem> TypeSystem::create(Snapshot& snapshot)
{
    ensureLLDBInitialized();
    auto impl = WTF::makeUnique<Impl>(snapshot);
    if (!impl->initialize())
        return nullptr;
    return std::unique_ptr<TypeSystem>(new TypeSystem(WTF::move(impl)));
}

TypeSystem::TypeSystem(std::unique_ptr<Impl> impl)
    : m_impl(WTF::move(impl))
{
}

TypeSystem::~TypeSystem() = default;

std::optional<TargetType> TypeSystem::findType(StringView qualifiedName)
{
    return m_impl->findType(qualifiedName);
}

std::optional<TargetType> TypeSystem::findTypeOfPointee(StringView variableName)
{
    return m_impl->findTypeOfPointee(variableName);
}

std::optional<TargetType> TypeSystem::findTypeOfMember(StringView qualifiedTypeName,
    StringView memberName)
{
    return m_impl->findTypeOfMember(qualifiedTypeName, memberName);
}

std::optional<TargetType> TypeSystem::findTypeOfMemberPointee(StringView qualifiedTypeName,
    StringView memberName)
{
    return m_impl->findTypeOfMemberPointee(qualifiedTypeName, memberName);
}

std::optional<TargetObject> TypeSystem::getTargetObject(Address base, const TargetType& type)
{
    return m_impl->getTargetObject(base, type);
}

#else // !HAVE(CORPSE_TYPE_SYSTEM)

class TypeSystem::Impl { };

std::unique_ptr<TypeSystem> TypeSystem::create(Snapshot&)
{
    return nullptr;
}

TypeSystem::TypeSystem(std::unique_ptr<Impl> impl)
    : m_impl(WTF::move(impl))
{
}

TypeSystem::~TypeSystem() = default;

std::optional<TargetType> TypeSystem::findType(StringView)
{
    return std::nullopt;
}

std::optional<TargetType> TypeSystem::findTypeOfPointee(StringView)
{
    return std::nullopt;
}

std::optional<TargetType> TypeSystem::findTypeOfMember(StringView, StringView)
{
    return std::nullopt;
}

std::optional<TargetType> TypeSystem::findTypeOfMemberPointee(StringView, StringView)
{
    return std::nullopt;
}

std::optional<TargetObject> TypeSystem::getTargetObject(Address, const TargetType&)
{
    return std::nullopt;
}

#endif // HAVE(CORPSE_TYPE_SYSTEM)

} // namespace Corpse
} // namespace JSC

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // HAVE(CORPSE_SUPPORT)
