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
#include "CorpseTargetDebugInfo.h"

#if ENABLE(MYA_HEAP)

#include "CorpseError.h"
#include "CorpseImage.h"
#include "CorpseProcess.h"
#include "CorpseSnapshot.h"
#include "CorpseTargetType.h"
#include <lldb/API/LLDB.h>
#include <mutex>
#include <wtf/Scope.h>
#include <wtf/StdLibExtras.h>
#include <wtf/TZoneMallocInlines.h>

namespace JSC {
namespace Corpse {

WTF_MAKE_TZONE_ALLOCATED_IMPL(TargetDebugInfo);

TargetDebugInfo::TargetDebugInfo(std::unique_ptr<lldb::SBDebugger>&& debugger, std::unique_ptr<lldb::SBTarget>&& target)
    : m_debugger(WTF::move(debugger))
    , m_target(WTF::move(target))
{
}

TargetDebugInfo::~TargetDebugInfo()
{
    m_debugger->DeleteTarget(*m_target);
    lldb::SBDebugger::Destroy(*m_debugger);
}

RefPtr<TargetDebugInfo> TargetDebugInfo::create(const Snapshot& snapshot)
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

    // liblldb only ever reads files here: nothing above may have given it a process.
    RELEASE_ASSERT(!target->GetProcess().IsValid());

    destroyDebugger.release();
    return adoptRef(*new TargetDebugInfo(WTF::move(debugger), WTF::move(target)));
}

// CLAUDE: ditto, should not be needed
RefPtr<TargetType> TargetDebugInfo::onlyCompleteType(lldb::SBTypeList& candidates, const char* qualifiedName, const char* where)
{
    lldb::SBType found;
    unsigned completeCount = 0;
    for (uint32_t index = 0; index < candidates.GetSize(); ++index) {
        lldb::SBType candidate = candidates.GetTypeAtIndex(index);
        // A declaration has no layout, so it does not count.
        if (!candidate.IsTypeComplete())
            continue;
        ++completeCount;
        found = candidate;
    }
    if (!completeCount) {
        CORPSE_REPORT("No complete definition of '%s' in %s", reportableString(qualifiedName), reportableString(where));
        return nullptr;
    }
    if (completeCount > 1) {
        CORPSE_REPORT("%u complete definitions of '%s' in %s", completeCount, reportableString(qualifiedName), reportableString(where));
        return nullptr;
    }
    return adoptRef(*new TargetType(*this, found));
}

RefPtr<TargetType> TargetDebugInfo::findType(const char* qualifiedName)
{
    lldb::SBTypeList candidates = m_target->FindTypes(qualifiedName);
    return onlyCompleteType(candidates, qualifiedName, "the images of the snapshot");
}

RefPtr<TargetType> TargetDebugInfo::findType(const char* qualifiedName, const char* imagePath)
{
    lldb::SBModule module = m_target->FindModule(lldb::SBFileSpec(imagePath, false));
    if (!module.IsValid()) {
        CORPSE_REPORT("No image %s in the snapshot", reportableString(imagePath));
        return nullptr;
    }
    lldb::SBTypeList candidates = module.FindTypes(qualifiedName);
    return onlyCompleteType(candidates, qualifiedName, imagePath);
}

RefPtr<TargetType> TargetDebugInfo::findTypeInImageContaining(Address address, const char* qualifiedName)
{
    lldb::SBModule module = m_target->ResolveLoadAddress(address.toTargetVMAddress()).GetModule();
    if (!module.IsValid()) {
        CORPSE_REPORT("No image is mapped at 0x%llx, where '%s' would be defined",
            static_cast<unsigned long long>(address.toTargetVMAddress()), reportableString(qualifiedName));
        return nullptr;
    }
    lldb::SBTypeList candidates = module.FindTypes(qualifiedName);
    return onlyCompleteType(candidates, qualifiedName, module.GetFileSpec().GetFilename());
}

unsigned TargetDebugInfo::moduleCount() const
{
    return m_target->GetNumModules();
}

} // namespace Corpse
} // namespace JSC

#endif // ENABLE(MYA_HEAP)
