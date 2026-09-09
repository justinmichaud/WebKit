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

#if HAVE(CORPSE_SUPPORT) && OS(LINUX)

#include <JavaScriptCore/CorpseBackend.h>
#include <memory>
#include <sys/types.h>
#include <wtf/Vector.h>

namespace JSC {
namespace Corpse {

// The registers of one stopped thread, as PTRACE_GETREGSET reported them.
struct StoppedThread {
    pid_t tid { 0 };
    ThreadRegisters registers;
    RegisterFailure failure { RegisterFailure::None };
};

// Holds every thread of a target in ptrace-stop and reads its registers.
//
// The stop is what makes a corpse coherent. Memory frozen while the threads run
// on describes no moment that ever existed: a stack pointer captured before the
// freeze can index a frame the freeze already unwound. So the caller stops the
// target, takes its copy of the memory while the stop holds, and only then lets
// the target go.
//
// Linux will not let a thread ptrace a sibling in its own thread group, so
// stopping the current process is done by a forked helper that traces its own
// parent. That needs the parent to name the helper with PR_SET_PTRACER, which
// stopAllThreads does, and which is why this works under the default Yama
// ptrace_scope of 1. Under a stricter scope, or without the capability to trace
// another process, the stop fails and each thread is reported with the reason
// rather than being dropped.
//
// Everything the helper runs after the fork is async-signal-safe: a fork of a
// multithreaded process inherits every lock another thread held, so allocating
// or logging there would deadlock against a thread that no longer exists.
class ThreadStopper {
public:
    // Stops every thread of `pid` other than `excludedTid` and reads their
    // registers. Pass the calling thread as `excludedTid` when `pid` is this
    // process: it cannot stop itself, and it is the thread that has to keep
    // running to take the corpse. Pass 0 for any other target.
    //
    // Never returns nullptr: a stopper that could not stop anything still
    // reports the threads it found and why each one has no registers.
    static std::unique_ptr<ThreadStopper> stopAllThreads(pid_t, pid_t excludedTid);

    ~ThreadStopper();

    ThreadStopper(const ThreadStopper&) = delete;
    ThreadStopper& operator=(const ThreadStopper&) = delete;

    // True if at least one thread was stopped, so the target is quiescent.
    bool holdsStop() const { return m_holdsStop; }

    const Vector<StoppedThread>& threads() const { return m_threads; }

    // Lets the target run again. Called by the destructor; call it explicitly
    // to end the stop as soon as the corpse has been taken.
    void release();

private:
    ThreadStopper() = default;

    // The helper that traces this process, and the pipes that sequence it.
    // -1 once released.
    pid_t m_helper { -1 };
    int m_goPipe { -1 };
    int m_donePipe { -1 };

    // Set when this stopper traced the target directly rather than through a
    // helper, in which case releasing means detaching here.
    Vector<pid_t> m_directlyTraced;

    Vector<StoppedThread> m_threads;
    bool m_holdsStop { false };
};

} // namespace Corpse
} // namespace JSC

#endif // HAVE(CORPSE_SUPPORT) && OS(LINUX)
