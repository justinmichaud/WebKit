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

#if ENABLE(MYA_HEAP)

#include "CorpseError.h"
#include "CorpseImage.h"
#include "CorpseProcess.h"
#include "CorpseSnapshot.h"
#include "CorpseTargetType.h"
#include <lldb/API/LLDB.h>
#include <mutex>
#include <string_view>
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

RefPtr<SnapshotDebugInfo> SnapshotDebugInfo::create(Snapshot& snapshot)
{
    if (!snapshot.isValid()) {
        CORPSE_REPORT("Cannot read debug info for an invalid snapshot");
        return nullptr;
    }
    int pid = static_cast<int>(snapshot.process()->pid());
    const Vector<Image>& images = snapshot.images();
    if (images.isEmpty())
        return nullptr; // images() said why.
    CORPSE_DIAGNOSTICS(diagnostics, "opening the debug info of pid %d", pid);
    diagnostics.count("images"_s, images.size());

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
        CORPSE_REPORT("liblldb could not create a target: %s", error.GetCString() ? error.GetCString() : "unknown error");
        return nullptr;
    }

    for (const Image& image : images) {
        const char* path = image.path().data();
        auto loadAddress = static_cast<unsigned long long>(image.loadAddress().toTargetVMAddress());

        lldb::SBModule module = target->AddModule(path, nullptr, nullptr);
        if (!module.IsValid()) {
            CORPSE_REPORT("liblldb could not open the image %s", image.path());
            return nullptr;
        }
        diagnostics.count("images opened"_s);

        lldb::addr_t headerFileAddress = module.GetObjectFileHeaderAddress().GetFileAddress();
        if (headerFileAddress == LLDB_INVALID_ADDRESS) {
            CORPSE_REPORT("liblldb could not find the header of the image %s", image.path());
            return nullptr;
        }
        lldb::SBError slideError = target->SetModuleLoadAddress(module, image.loadAddress().toTargetVMAddress() - headerFileAddress);
        if (slideError.Fail()) {
            CORPSE_REPORT("liblldb could not place the image %s at 0x%llx: %s", image.path(), loadAddress,
                slideError.GetCString() ? slideError.GetCString() : "unknown error");
            return nullptr;
        }
        diagnostics.count("images placed"_s);
    }

    RELEASE_ASSERT(!target->GetProcess().IsValid());

    destroyDebugger.release();
    return adoptRef(*new SnapshotDebugInfo(WTF::move(debugger), WTF::move(target)));
}

static bool isDestructor(lldb::SBFunction& function)
{
    // Itanium C++ ABI 5.1.3: only the complete (D1) and deleting (D0)
    // destructors are in a vtable. A method named D0 mangles the same way, so
    // the demangled name settles it: its last component starts with '~'.
    const char* mangled = function.GetMangledName();
    if (!mangled)
        return false;
    std::string_view mangledView { mangled };
    if (!mangledView.ends_with("D0Ev") && !mangledView.ends_with("D1Ev"))
        return false;
    const char* name = function.GetName();
    if (!name)
        return false;
    std::string_view nameView { name };
    size_t lastScope = nameView.rfind("::");
    return lastScope != std::string_view::npos && lastScope + 2 < nameView.size() && nameView[lastScope + 2] == '~';
}

RefPtr<TargetType> SnapshotDebugInfo::classOfDestructor(Address function, bool& inImage)
{
    lldb::SBAddress address = m_target->ResolveLoadAddress(function.toTargetVMAddress());
    inImage = address.GetModule().IsValid();
    if (!inImage)
        return nullptr;
    lldb::SBFunction destructor = address.GetFunction();
    if (!destructor.IsValid() || !isDestructor(destructor))
        return nullptr;

    // A destructor's only parameter is `this`.
    lldb::SBValueList parameters = destructor.GetBlock().GetVariables(*m_target, true, false, false);
    if (parameters.GetSize() != 1) {
        CORPSE_REPORT("The destructor at 0x%llx has %u parameters in its debug info", static_cast<unsigned long long>(function.toTargetVMAddress()), parameters.GetSize());
        return nullptr;
    }
    lldb::SBType type = parameters.GetValueAtIndex(0).GetType().GetPointeeType();
    if (!type.IsValid() || !type.IsTypeComplete()) {
        CORPSE_REPORT("The class of the destructor at 0x%llx is incomplete in its debug info", static_cast<unsigned long long>(function.toTargetVMAddress()));
        return nullptr;
    }
    return adoptRef(*new TargetType(*this, type));
}

} // namespace Corpse
} // namespace JSC

#endif // ENABLE(MYA_HEAP)
