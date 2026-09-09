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

#pragma once

#include <JavaScriptCore/CorpsePlatform.h>

#if HAVE(CORPSE_SUPPORT)

#include <memory>
#include <sys/types.h>
#include <wtf/Assertions.h>
#include <wtf/Ref.h>
#include <wtf/RefCounted.h>
#include <wtf/text/CString.h>

namespace JSC {
namespace Corpse {

// A target process identified by PID. attach() acquires whatever handle the
// platform needs to speak about it and detach() releases it, keeping the PID so
// the same Process can be reattached.
//
// What that handle is belongs to the platform and is not spelled here: on
// Darwin it is the Mach task port, on Linux there is none and attach() records
// the process's start time so a reused PID is not mistaken for this process.
// Whichever it is lives in Handle, which only the platform's own files define.
class Process final : public RefCounted<Process> {
public:
    static Ref<Process> create(pid_t pid) { return adoptRef(*new Process(pid)); }

    ~Process();

    bool attach();
    void detach();

    pid_t pid() const { return m_pid; }

    bool isAttached() const;

    // The target process may have terminated while we still hold the handle.
    bool holdsLiveTask() const;

    // True if the target runs under a binary translator. Such a process
    // executes as another architecture whatever its own is, so its thread state
    // describes the translator rather than the program and cannot be read as
    // the program's. Only Darwin has one; elsewhere this is always false.
    bool isTranslated() const;

    // The absolute path to the target's executable image, or a null CString if
    // the OS refuses to report one. The platform mechanism (proc_pidpath on
    // Darwin, readlink on /proc on Linux) is hidden behind this call.
    CString executablePath() const;

    // The platform's handle on the target. Defined only by the files of the
    // platform that owns it, and reached only by them; everything portable uses
    // the calls above.
    class Handle;
    Handle& handle() const { return *m_handle; }

private:
    explicit Process(pid_t pid);

    pid_t m_pid;
    std::unique_ptr<Handle> m_handle;
};

} // namespace Corpse
} // namespace JSC

#endif // HAVE(CORPSE_SUPPORT)
