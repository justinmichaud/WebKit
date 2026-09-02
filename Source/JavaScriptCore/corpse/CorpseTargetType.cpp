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

#if (OS(MACOS) || USE(APPLE_INTERNAL_SDK)) && !PLATFORM(MACCATALYST) && !PLATFORM(IOS_FAMILY_SIMULATOR)

#include "CorpseProcess.h"
#include "CorpseSnapshot.h"
#include "CorpseTargetObject.h"

#include <string.h>
#include <wtf/Assertions.h>
#include <wtf/StdLibExtras.h>
#include <wtf/text/CString.h>

#if OS(MACOS) && !PLATFORM(MACCATALYST)
#include <LLDB/LLDB.h>
#include <mutex>
#define CORPSE_HAS_LLDB 1
#endif

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC {
namespace Corpse {

TargetType::TargetType(String name, uint64_t byteSize, Vector<TargetField>&& fields,
    bool isPolymorphic, Address expectedVTableAddress)
    : m_name(WTF::move(name))
    , m_byteSize(byteSize)
    , m_fields(WTF::move(fields))
    , m_isPolymorphic(isPolymorphic)
    , m_expectedVTableAddress(expectedVTableAddress)
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

#if CORPSE_HAS_LLDB

namespace {

void ensureLLDBInitialized()
{
    static std::once_flag flag;
    std::call_once(flag, [] {
        lldb::SBDebugger::Initialize();
    });
}

// Anonymous unions and structs are recursed into so their members appear at
// the enclosing offset, which is how a name like m_data8 reaches the caller.
void collectFields(lldb::SBType type, uint64_t baseOffset, Vector<TargetField>& out)
{
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
        });
    }
}

// A vtable symbol demangles to "vtable for <qualifiedName>"; LLDB indexes
// symbols by their C++ names, so search by the demangled form and take the
// first hit.
Address findVTableAddress(lldb::SBTarget& target, const char* qualifiedName)
{
    lldb::SBSymbolContextList list = target.FindSymbols(qualifiedName, lldb::eSymbolTypeData);
    for (uint32_t i = 0; i < list.GetSize(); ++i) {
        auto ctx = list.GetContextAtIndex(i);
        auto symbol = ctx.GetSymbol();
        if (!symbol.IsValid())
            continue;
        const char* symbolName = symbol.GetName();
        if (!symbolName)
            continue;
        StringView view = StringView::fromLatin1(symbolName);
        if (!view.startsWith("vtable for "_s))
            continue;
        auto addr = symbol.GetStartAddress();
        if (!addr.IsValid())
            continue;
        lldb::addr_t loaded = addr.GetLoadAddress(target);
        if (loaded == LLDB_INVALID_ADDRESS)
            loaded = addr.GetFileAddress();
        if (loaded == LLDB_INVALID_ADDRESS)
            continue;
        return Address(loaded);
    }
    return { };
}

} // anonymous namespace

class TypeSystem::Impl {
public:
    Impl(Snapshot& snapshot)
        : m_snapshot(snapshot)
    {
    }

    bool initialize()
    {
        Process* process = m_snapshot.process();
        if (!process)
            return false;

        RELEASE_ASSERT_WITH_MESSAGE(process->pid() == getpid(),
            "Corpse type introspection is wired up for self-corpse only; cross-process is not implemented yet.");

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

        // Bring every loaded image into the target so FindTypes reaches types
        // defined in dylibs the executable does not statically list.
        for (const auto& path : m_snapshot.loadedImagePaths()) {
            if (path.isNull())
                continue;
            if (m_target.FindModule(lldb::SBFileSpec(path.data(), true)).IsValid())
                continue;
            m_target.AddModule(path.data(), nullptr, nullptr);
        }
        return true;
    }

    std::optional<TargetType> findType(StringView qualifiedName)
    {
        auto utf8 = qualifiedName.utf8();
        auto types = m_target.FindTypes(utf8.data());
        // FindTypes returns every matching name across modules; a forward
        // declaration matches but carries no fields, so pick the first hit
        // with a body.
        for (uint32_t i = 0; i < types.GetSize(); ++i) {
            auto t = types.GetTypeAtIndex(i);
            if (!t.IsValid() || !t.GetNumberOfFields())
                continue;
            bool isPolymorphic = t.IsPolymorphicClass();
            Address vtableAddress;
            if (isPolymorphic) {
                const char* resolvedName = t.GetName();
                if (resolvedName)
                    vtableAddress = findVTableAddress(m_target, resolvedName);
                // A polymorphic type without a resolvable vtable symbol
                // cannot support validate(), so refuse it here rather than
                // hand a caller a TargetType that silently skips its check.
                if (!vtableAddress)
                    return std::nullopt;
            }
            Vector<TargetField> fields;
            collectFields(t, 0, fields);
            return TargetType {
                String::fromLatin1(t.GetName() ? t.GetName() : ""),
                static_cast<uint64_t>(t.GetByteSize()),
                WTF::move(fields),
                isPolymorphic,
                vtableAddress,
            };
        }
        return std::nullopt;
    }

    std::optional<TargetObject> getTargetObject(Address base, const TargetType& type)
    {
        auto bytes = m_snapshot.readBytes(base, type.byteSize());
        if (!bytes)
            return std::nullopt;

        // findType refuses polymorphic types with no resolvable vtable, so a
        // TargetType with isPolymorphic() always carries an expected vtable
        // address here. A non-polymorphic type has no vptr to check, so it
        // passes through untouched.
        if (type.isPolymorphic()) {
            if (bytes->size() < sizeof(uintptr_t))
                return std::nullopt;
            uintptr_t vptr = 0;
            memcpy(&vptr, bytes->data(), sizeof(uintptr_t));
            if (Address(vptr) != type.expectedVTableAddress())
                return std::nullopt;
        }
        return TargetObject { base, type, WTF::move(*bytes) };
    }

private:
    Snapshot& m_snapshot;
    lldb::SBDebugger m_debugger;
    lldb::SBTarget m_target;
};

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

std::optional<TargetObject> TypeSystem::getTargetObject(Address base, const TargetType& type)
{
    return m_impl->getTargetObject(base, type);
}

#else // !CORPSE_HAS_LLDB

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

std::optional<TargetObject> TypeSystem::getTargetObject(Address, const TargetType&)
{
    return std::nullopt;
}

#endif // CORPSE_HAS_LLDB

} // namespace Corpse
} // namespace JSC

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // (OS(MACOS) || USE(APPLE_INTERNAL_SDK)) && !PLATFORM(MACCATALYST) && !PLATFORM(IOS_FAMILY_SIMULATOR)
