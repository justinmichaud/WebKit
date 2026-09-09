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
#include "CorpseProcess.h"

#if HAVE(CORPSE_SUPPORT) && OS(LINUX)

#include "CorpseError.h"
#include "CorpseProcFile.h"
#include "CorpseProcessHandle.h"

#include <errno.h>
#include <optional>
#include <signal.h>
#include <string.h>

namespace JSC {
namespace Corpse {

bool Process::isAttached() const
{
    return handle().isAttached;
}

// The kernel reuses a PID once its process is gone, so the start time is what
// tells a still-running target from one that exited and from a later process
// that inherited the PID.
bool Process::holdsLiveTask() const
{
    if (!isAttached())
        return false;
    auto stat = ProcStat::forProcess(m_pid);
    return stat && stat->number(ProcStat::startTimeField) == handle().startTime;
}

// Linux has no binary translator whose thread state would describe something
// other than the program.
bool Process::isTranslated() const
{
    return false;
}

bool Process::attach()
{
    if (isAttached()) {
        if (holdsLiveTask())
            return true;
        detach();
    }

    auto stat = ProcStat::forProcess(m_pid);
    auto startTime = stat ? stat->number(ProcStat::startTimeField) : std::nullopt;
    if (!startTime) {
        if (kill(m_pid, 0) && errno == ESRCH)
            Error::report("No process with PID %d", static_cast<int>(m_pid));
        else {
            Error::report("Could not attach to PID %u: %s -- may need to run as root "
                "or to relax /proc/sys/kernel/yama/ptrace_scope",
                static_cast<unsigned>(m_pid), strerror(errno));
        }
        return false;
    }

    handle().startTime = *startTime;
    handle().isAttached = true;
    return true;
}

void Process::detach()
{
    handle().startTime = 0;
    handle().isAttached = false;
}

CString Process::executablePath() const
{
    auto path = ProcFile::readLink(m_pid, "exe"_s);
    return path ? path->utf8() : CString { };
}

} // namespace Corpse
} // namespace JSC

#endif // HAVE(CORPSE_SUPPORT) && OS(LINUX)
