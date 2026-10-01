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

#if ENABLE(MYA)

#include "CorpseError.h"
#include <array>
#include <span>

#if OS(DARWIN)
#include <errno.h>
#include <libproc.h>
#include <mach/mach.h>
#include <mach/mach_error.h>
#include <mach/mach_traps.h>
#include <signal.h>
#include <sys/proc.h>
#include <sys/sysctl.h>
#else
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <unistd.h>
#include <wtf/SafeStrerror.h>
#include <wtf/text/MakeString.h>
#endif

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC {
namespace Corpse {

#if OS(DARWIN)

// A task port name outlives the task it named: when the target exits, the right we
// hold becomes a dead name while the name itself is unchanged. MACH_PORT_VALID only
// looks at the name, so it keeps reporting the port as good. Asking the kernel which
// pid the port names is what tells a still-attached process apart from one that has
// since exited -- and, because the answer is compared against m_pid, from a later
// process that inherited the same pid.
bool Process::holdsLiveTask() const
{
    if (!isAttached())
        return false;
    int pid = -1;
    return pid_for_task(taskPort(), &pid) == KERN_SUCCESS && pid == m_pid;
}

bool Process::isTranslated() const
{
    struct kinfo_proc info;
    size_t length = sizeof info;
    int selector[4] = { CTL_KERN, KERN_PROC, KERN_PROC_PID, m_pid };
    // A pid that no longer exists is not an error here: sysctl succeeds and reports
    // that it wrote nothing, so the size has to be checked rather than the result.
    if (sysctl(selector, 4, &info, &length, nullptr, 0) || length < sizeof info)
        return false;
    return info.kp_proc.p_flag & P_TRANSLATED;
}

bool Process::attach()
{
    if (isAttached()) {
        if (holdsLiveTask())
            return true;
        // The target exited while we held its port.
        detach();
    }

    mach_port_t taskPort = MACH_PORT_NULL;
    kern_return_t kr = task_for_pid(mach_task_self(), m_pid, &taskPort);
    if (kr == KERN_SUCCESS) {
        m_taskPort = OwnedTaskHandle::adopt(taskPort);
        return true;
    }

    if (kill(m_pid, 0) && errno == ESRCH)
        Error::report("No process with PID %d", static_cast<int>(m_pid));
    else {
        Error::report("Could not attach to PID %u: %s (0x%x) -- may need to run as root "
            "or add the appropriate debugger entitlement",
            static_cast<unsigned>(m_pid), mach_error_string(kr), kr);
    }
    return false;
}

UTF8CString Process::executablePath() const
{
    std::array<char, PROC_PIDPATHINFO_MAXSIZE> path;
    int length = proc_pidpath(m_pid, path.data(), path.size());
    if (length <= 0)
        return { };
    return UTF8CString(byteCast<char8_t>(std::span { path }.first(length)));
}

#else

// The handle is the pid, which process_vm_readv reads through. Reading needs
// the permission ptrace needs, which a parent has over its child.
bool Process::holdsLiveTask() const
{
    return isAttached() && !(kill(m_pid, 0) && errno == ESRCH);
}

bool Process::isTranslated() const { return false; }

bool Process::attach()
{
    if (kill(m_pid, 0) && errno == ESRCH) {
        detach();
        Error::report("No process with PID %d", static_cast<int>(m_pid));
        return false;
    }
    m_taskPort = OwnedTaskHandle::adopt(m_pid);
    return true;
}

std::optional<std::string> readProcFile(pid_t pid, const char* path)
{
    ASCIICString fullPath = makeString("/proc/"_s, pid, '/', StringView::fromLatin1(path)).ascii();
    int file = open(fullPath.data(), O_RDONLY | O_CLOEXEC);
    if (file < 0)
        return std::nullopt;
    std::string contents;
    std::array<char, 16 * 1024> buffer;
    for (;;) {
        ssize_t length = read(file, buffer.data(), buffer.size());
        if (length < 0 && errno == EINTR)
            continue;
        if (length <= 0) {
            close(file);
            if (length < 0)
                return std::nullopt;
            return contents;
        }
        contents.append(buffer.data(), static_cast<size_t>(length));
    }
}

UTF8CString executablePathOf(pid_t pid)
{
    std::array<char, PATH_MAX> path;
    ASCIICString link = makeString("/proc/"_s, pid, "/exe"_s).ascii();
    ssize_t length = readlink(link.data(), path.data(), path.size());
    if (length <= 0 || static_cast<size_t>(length) >= path.size())
        return { };
    return UTF8CString(byteCast<char8_t>(std::span { path }.first(length)));
}

UTF8CString Process::executablePath() const
{
    return executablePathOf(m_pid);
}

pid_t forkCopy(int lifetime)
{
    // Unlike fork(), _Fork runs no pthread_atfork handlers and resets none of
    // glibc's internal locks, such as malloc's: nothing runs in this process
    // that could take a lock in the middle of the copy, and nothing in the copy
    // changes it beyond what glibc needs for async-signal-safe calls.
    pid_t copy = _Fork();
    if (copy < 0) {
        CORPSE_REPORT("Could not copy pid %d: %s", static_cast<int>(getpid()), safeStrerror(errno).data());
        return -1;
    }
    if (copy)
        return copy;

    // In the copy, only async-signal-safe calls: this process's other threads
    // do not exist here, and may have held any lock. Every other descriptor is
    // closed, so that a later copy does not keep an earlier one alive through
    // the write end of its lifetime pipe.
    if (lifetime != STDIN_FILENO)
        dup2(lifetime, STDIN_FILENO);
    close_range(STDIN_FILENO + 1, ~0U, 0);
    for (;;) {
        char byte;
        ssize_t result = read(STDIN_FILENO, &byte, 1);
        if (!result || (result < 0 && errno != EINTR))
            break;
    }
    _exit(0);
}

#endif // OS(DARWIN)

} // namespace Corpse
} // namespace JSC

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(MYA)
