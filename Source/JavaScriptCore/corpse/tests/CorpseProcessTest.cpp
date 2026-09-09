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
#include "CorpseProcessTest.h"

#if HAVE(CORPSE_SUPPORT)

#include "LibJSCToolsTestUtilities.h"

#include <JavaScriptCore/CorpseError.h>
#include <JavaScriptCore/CorpseProcess.h>
#include <errno.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#include <wtf/Ref.h>

#if OS(DARWIN)
#include <JavaScriptCore/CorpseProcessHandle.h>
#include <mach/mach.h>
#endif

namespace JSCToolsTest {

using JSC::Corpse::Process;

namespace {

// A pid that is certainly not in use: a child that has been reaped. Returns 0 if no
// child could be made, reporting why, since the caller only sees the missing pid.
pid_t reapedChildPid()
{
    pid_t child = fork();
    if (child < 0) {
        dataLogLn("    could not fork: ", strerror(errno));
        return 0;
    }
    if (!child)
        _exit(0);

    int status = 0;
    // EINTR leaves the child unreaped, so resume rather than report a failure.
    while (waitpid(child, &status, 0) < 0 && errno == EINTR) { }
    return child;
}

} // anonymous namespace

// What attaching means is the platform's business; what it promises is not. A
// Process is not attached until it attaches, says so while it is, holds the
// target only while the target lives, and gives back whatever it took.
void testProcess()
{
    SuiteTracer tracer("Process");
    if (!tracer.shouldRun())
        return;

    {
        Ref<Process> process = Process::create(getpid());
        TEST_ASSERT(!process->isAttached(), "a new Process is not attached");
        TEST_ASSERT_EQ(process->pid(), getpid(), "a Process keeps the pid it was given");

        TEST_ASSERT(process->attach(), "attaching to this process succeeds");
        TEST_ASSERT(process->isAttached(), "attaching leaves the Process attached");
        TEST_ASSERT(process->holdsLiveTask(), "an attached Process names this very process");
        TEST_ASSERT(process->attach(), "attaching an already attached Process succeeds");

        process->detach();
        TEST_ASSERT(!process->isAttached(), "detaching releases the handle");
        TEST_ASSERT(!process->holdsLiveTask(), "a detached Process holds nothing");

        TEST_ASSERT(process->attach(), "a detached Process can attach again");
        process->detach();
        process->detach();
        TEST_ASSERT(!process->isAttached(), "detaching twice is harmless");
    }

    {
        // The executable path is read from the OS rather than from argv, so it
        // is absolute and names a file that exists.
        Ref<Process> process = Process::create(getpid());
        TEST_ASSERT(process->attach(), "attaching to this process succeeds");
        auto path = process->executablePath();
        TEST_ASSERT(!path.isNull(), "an attached Process reports its executable path");
        if (!path.isNull()) {
            TEST_ASSERT(path.length() && path.data()[0] == '/',
                "the executable path is absolute");
        }
    }

    {
        // This process is not translated, whatever the platform: a build that
        // runs these tests is running as itself.
        Ref<Process> process = Process::create(getpid());
        TEST_ASSERT(process->attach(), "attaching to this process succeeds");
        TEST_ASSERT(!process->isTranslated(), "this process is not translated");
    }

    {
        // A pid nobody holds cannot be attached to, and saying so is the answer
        // rather than a crash or a false success.
        pid_t gone = reapedChildPid();
        TEST_ASSERT(gone, "a child could be forked and reaped");
        if (gone) {
            // The refusal reports why, which is the point of it, but this one
            // was asked for and so is not news.
            JSC::Corpse::Error::Quiet quiet;
            Ref<Process> process = Process::create(gone);
            TEST_ASSERT(!process->attach(), "attaching to a reaped pid fails");
            TEST_ASSERT(!process->isAttached(), "a failed attach leaves the Process unattached");
            TEST_ASSERT(!process->holdsLiveTask(), "a failed attach holds no target");
        }
    }

    {
        // Attaching takes something from the OS on every platform, and every
        // path out of an attach has to give it back.
        unsigned footprintBefore = resourceFootprint();
        for (unsigned i = 0; i < 4; ++i) {
            Ref<Process> process = Process::create(getpid());
            process->attach();
            process->detach();
            process->attach();
            // Left attached, so the destructor is what has to release it.
        }
        TEST_ASSERT_EQ(resourceFootprint(), footprintBefore,
            "attaching and destroying Processes leaves nothing behind");
    }

#if OS(DARWIN)
    {
        // Attaching takes a send right to the target's task port. Attaching to
        // this very process yields the name this task already holds for itself,
        // so the kernel adds a reference to that name rather than handing out a
        // new one: a right that is never given back shows up in the reference
        // count and not in the size of the name space.
        unsigned namesBefore = machPortNameCount();
        mach_port_t port = MACH_PORT_NULL;
        unsigned refsAttached = 0;
        {
            Ref<Process> process = Process::create(getpid());
            TEST_ASSERT(process->attach(), "attaching to this process succeeds");
            port = process->handle().taskPort;
            refsAttached = machPortSendRightCount(port);
            TEST_ASSERT(refsAttached, "an attached Process holds a send right to the task port");

            process->attach();
            TEST_ASSERT_EQ(machPortSendRightCount(port), refsAttached,
                "attaching an already attached Process takes no further right");
            process->detach();
            TEST_ASSERT_EQ(machPortSendRightCount(port), refsAttached - 1,
                "detaching gives the right back");
            process->detach();
            TEST_ASSERT_EQ(machPortSendRightCount(port), refsAttached - 1,
                "detaching twice gives nothing further back");
            process->attach(); // Left attached, so the destructor has to release it.
        }
        TEST_ASSERT_EQ(machPortSendRightCount(port), refsAttached - 1,
            "destroying an attached Process gives its right back");
        TEST_ASSERT_EQ(machPortNameCount(), namesBefore,
            "attaching and detaching leaves the port name space as it was");
    }
#endif // OS(DARWIN)
}

} // namespace JSCToolsTest

#endif // HAVE(CORPSE_SUPPORT)
