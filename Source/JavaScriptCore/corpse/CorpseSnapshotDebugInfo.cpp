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
#include "CorpseSnapshot.h"
#include <wtf/TZoneMallocInlines.h>

namespace JSC {
namespace Corpse {

WTF_MAKE_TZONE_ALLOCATED_IMPL(SnapshotDebugInfo);

SnapshotDebugInfo::SnapshotDebugInfo()
    : m_debugger(lldb::SBDebugger::Create(false))
    , m_target(m_debugger.CreateTarget(""))
{
    RELEASE_ASSERT(m_target.IsValid());
}

SnapshotDebugInfo::~SnapshotDebugInfo()
{
    m_debugger.DeleteTarget(m_target);
    lldb::SBDebugger::Destroy(m_debugger);
}

Ref<SnapshotDebugInfo> SnapshotDebugInfo::create(Snapshot& snapshot)
{
    int pid = snapshot.process()->pid();
    CORPSE_DIAGNOSTICS(diagnostics, "opening the debug info of pid %d", pid);
    lldb::SBDebugger::Initialize();
    Ref debugInfo = adoptRef(*new SnapshotDebugInfo);
    lldb::SBTarget& target = debugInfo->m_target;
    for (const Image& image : snapshot.images()) {
        lldb::SBModule module = target.AddModule(image.path().legacyCStringPointer(), nullptr, image.uuid().legacyCStringPointer());
        lldb::SBAddress header = module.GetObjectFileHeaderAddress();
        if (!module.IsValid() || !header.IsValid() || !target.SetModuleLoadAddress(module, image.loadAddress().toMachVMAddress() - header.GetFileAddress()).Success())
            CORPSE_REPORT("Could not open the debug info of '%s'", image.path().legacyCStringPointer());
    }
    return debugInfo;
}

} // namespace Corpse
} // namespace JSC

#endif // ENABLE(MYA_HEAP)
