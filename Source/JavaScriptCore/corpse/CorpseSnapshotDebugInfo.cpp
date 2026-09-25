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
#include "CorpseSnapshotDebugInfo.h"

#if ENABLE(MYA)

#if ENABLE(MYA_HEAP)

#include "CorpseError.h"
#include "CorpseImage.h"
#include "CorpseProcess.h"
#include "CorpseSnapshot.h"
#include "CorpseTargetType.h"
#include "CorpseTargetValue.h"
#include <lldb/API/LLDB.h>
#include <mutex>
#include <wtf/Scope.h>
#include <wtf/StdLibExtras.h>
#include <wtf/TZoneMallocInlines.h>

namespace JSC {
namespace Corpse {

WTF_MAKE_TZONE_ALLOCATED_IMPL(SnapshotDebugInfo);

SnapshotDebugInfo::SnapshotDebugInfo(std::unique_ptr<lldb::SBDebugger>&& debugger, std::unique_ptr<lldb::SBTarget>&& target)
    : m_debugger(WTF::move(debugger))
    , m_target(WTF::move(target))
{
}

SnapshotDebugInfo::~SnapshotDebugInfo()
{
    m_debugger->DeleteTarget(*m_target);
    lldb::SBDebugger::Destroy(*m_debugger);
}

RefPtr<SnapshotDebugInfo> SnapshotDebugInfo::create(const Snapshot& snapshot)
{
    if (!snapshot.isValid()) {
        CORPSE_REPORT("Cannot read debug info for an invalid snapshot");
        return nullptr;
    }
    int pid = static_cast<int>(snapshot.process()->pid());
    const Vector<Image>& images = snapshot.images();
    if (images.isEmpty())
        return nullptr; // images() said why.

    static std::once_flag once;
    std::call_once(once, [] {
        lldb::SBDebugger::Initialize();
    });

    auto debugger = makeUniqueWithoutFastMallocCheck<lldb::SBDebugger>(lldb::SBDebugger::Create(false));
    if (!debugger->IsValid()) {
        CORPSE_REPORT("liblldb could not create a debugger");
        return nullptr;
    }
    // Destroying the debugger destroys its targets with it.
    auto destroyDebugger = makeScopeExit([&] {
        lldb::SBDebugger::Destroy(*debugger);
    });

    // The target starts empty, so that every module is one the dynamic loader
    // listed, at the address it listed it at.
    lldb::SBError error;
    auto target = makeUniqueWithoutFastMallocCheck<lldb::SBTarget>(debugger->CreateTarget(nullptr, nullptr, nullptr, false, error));
    if (!target->IsValid()) {
        CORPSE_REPORT("liblldb could not create a target for pid %d: %s", pid,
            reportableString(error.GetCString() ? error.GetCString() : "unknown error"));
        return nullptr;
    }

    for (const Image& image : images) {
        const char* path = image.path().data();
        auto loadAddress = static_cast<unsigned long long>(image.loadAddress().toTargetVMAddress());

        lldb::SBModule module = target->AddModule(path, nullptr, nullptr);
        if (!module.IsValid()) {
            CORPSE_REPORT("liblldb could not open the image %s of pid %d", image.path(), pid);
            return nullptr;
        }

        lldb::addr_t headerFileAddress = module.GetObjectFileHeaderAddress().GetFileAddress();
        if (headerFileAddress == LLDB_INVALID_ADDRESS) {
            CORPSE_REPORT("liblldb could not find the header of the image %s of pid %d", image.path(), pid);
            return nullptr;
        }
        lldb::SBError slideError = target->SetModuleLoadAddress(module, image.loadAddress().toTargetVMAddress() - headerFileAddress);
        if (slideError.Fail()) {
            CORPSE_REPORT("liblldb could not place the image %s of pid %d at 0x%llx: %s", image.path(), pid, loadAddress,
                reportableString(slideError.GetCString() ? slideError.GetCString() : "unknown error"));
            return nullptr;
        }
    }

    RELEASE_ASSERT(!target->GetProcess().IsValid());

    destroyDebugger.release();
    return adoptRef(*new SnapshotDebugInfo(WTF::move(debugger), WTF::move(target)));
}

RefPtr<TargetType> SnapshotDebugInfo::findType(const char* qualifiedName, lldb::SBModule& module)
{
    // liblldb resolves a declaration to the definition wherever the image has
    // one, so what comes back is incomplete only if the image never defines it.
    lldb::SBType type = module.FindFirstType(qualifiedName);
    if (!type.IsValid()) {
        CORPSE_REPORT("No type '%s' in %s", reportableString(qualifiedName), reportableString(module.GetFileSpec().GetFilename()));
        return nullptr;
    }
    if (!type.IsTypeComplete()) {
        CORPSE_REPORT("'%s' is only declared in %s", reportableString(qualifiedName), reportableString(module.GetFileSpec().GetFilename()));
        return nullptr;
    }
    return adoptRef(*new TargetType(*this, type));
}

RefPtr<TargetType> SnapshotDebugInfo::findType(const char* qualifiedName, const Image& image)
{
    lldb::SBModule module = m_target->FindModule(lldb::SBFileSpec(image.path().data(), false));
    if (!module.IsValid()) {
        CORPSE_REPORT("No image %s in the snapshot", image.path());
        return nullptr;
    }
    return findType(qualifiedName, module);
}

RefPtr<TargetType> SnapshotDebugInfo::findTypeForVTable(Address vtable, const char* demangledName)
{
    lldb::SBModule module = m_target->ResolveLoadAddress(vtable.toTargetVMAddress()).GetModule();
    if (!module.IsValid()) {
        CORPSE_REPORT("No image is mapped at 0x%llx, where the vtable of '%s' is",
            static_cast<unsigned long long>(vtable.toTargetVMAddress()), reportableString(demangledName));
        return nullptr;
    }
    return findType(demangledName, module);
}

std::optional<TargetValue> SnapshotDebugInfo::findVariable(const char* qualifiedName, const Snapshot& snapshot)
{
    lldb::SBValue variable = m_target->FindFirstGlobalVariable(qualifiedName);
    if (!variable.IsValid()) {
        CORPSE_REPORT("No variable '%s' in the snapshot's debug info", reportableString(qualifiedName));
        return std::nullopt;
    }
    lldb::addr_t address = variable.GetLoadAddress();
    if (address == LLDB_INVALID_ADDRESS) {
        CORPSE_REPORT("The variable '%s' has no address in the snapshot", reportableString(qualifiedName));
        return std::nullopt;
    }
    return TargetValue::at(snapshot, Address { address }, adoptRef(*new TargetType(*this, variable.GetType())));
}

unsigned SnapshotDebugInfo::addressByteSize() const
{
    return m_target->GetAddressByteSize();
}

} // namespace Corpse
} // namespace JSC

#else // ENABLE(MYA_HEAP)

#include "CorpseError.h"
#include "CorpseTargetType.h"
#include "CorpseTargetValue.h"
#include <wtf/TZoneMallocInlines.h>

// Complete stand-ins for the liblldb classes the members point to, so that
// the destructors compile. No instance is ever made.
namespace lldb {
class SBDebugger { };
class SBTarget { };
}

namespace JSC {
namespace Corpse {

WTF_MAKE_TZONE_ALLOCATED_IMPL(SnapshotDebugInfo);

SnapshotDebugInfo::~SnapshotDebugInfo() = default;

RefPtr<SnapshotDebugInfo> SnapshotDebugInfo::create(const Snapshot&)
{
    CORPSE_REPORT("This build has no liblldb, so it cannot read debug info");
    return nullptr;
}

RefPtr<TargetType> SnapshotDebugInfo::findType(const char*, const Image&)
{
    return nullptr;
}

RefPtr<TargetType> SnapshotDebugInfo::findTypeForVTable(Address, const char*)
{
    return nullptr;
}

std::optional<TargetValue> SnapshotDebugInfo::findVariable(const char*, const Snapshot&)
{
    return std::nullopt;
}

unsigned SnapshotDebugInfo::addressByteSize() const
{
    return 0;
}

} // namespace Corpse
} // namespace JSC

#endif // ENABLE(MYA_HEAP)

#endif // ENABLE(MYA)
