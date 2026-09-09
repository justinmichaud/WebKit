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
#include "CorpseSnapshotTest.h"

#if HAVE(CORPSE_SUPPORT)

#include "LibJSCToolsTestUtilities.h"

#include <JavaScriptCore/CorpseError.h>
#include <JavaScriptCore/CorpseProcess.h>
#include <JavaScriptCore/CorpseSnapshot.h>
#include <unistd.h>
#include <wtf/Ref.h>
#include <wtf/RefPtr.h>

namespace JSCToolsTest {

using JSC::Corpse::Process;
using JSC::Corpse::Snapshot;

// A Snapshot owns a frozen copy of a process. How the platform freezes one is
// the backend's business; that a snapshot is valid or is not, keeps the process
// it came from, carries an identifier no other snapshot reuses, and gives back
// everything it took is not.
void testSnapshot()
{
    SuiteTracer tracer("Snapshot");
    if (!tracer.shouldRun())
        return;

    Ref<Process> process = Process::create(getpid());
    if (!process->attach()) {
        TEST_ASSERT(false, "attaching to this process succeeds");
        return;
    }

    unsigned firstId = 0;
    {
        Snapshot snapshot(process.ptr());
        TEST_ASSERT(snapshot.isValid(), "a snapshot of this process is valid");
        TEST_ASSERT(snapshot.process() == process.ptr(),
            "a snapshot keeps the process it came from");
        firstId = snapshot.id();
        TEST_ASSERT(firstId, "a snapshot has an identifier");

        Snapshot second(process.ptr());
        TEST_ASSERT(second.isValid(), "a second snapshot of the same process is valid");
        TEST_ASSERT(second.id() > firstId, "identifiers increase");

        // Two corpses of one process are two independent copies, not one
        // shared: reading either must not depend on the other still existing.
        TEST_ASSERT(!second.regions().isEmpty(), "a snapshot describes the target's mappings");
    }

    {
        // The two above are gone; their identifiers must not come back.
        Snapshot later(process.ptr());
        TEST_ASSERT(later.id() > firstId + 1,
            "identifiers are not reused after a snapshot is destroyed");
    }

    {
        // A snapshot of a process that was never attached has nothing to read,
        // and every accessor has to say so rather than reach for it anyway.
        JSC::Corpse::Error::Quiet quiet;
        Ref<Process> unattached = Process::create(getpid());
        Snapshot snapshot(unattached.ptr());
        TEST_ASSERT(!snapshot.isValid(), "a snapshot of an unattached process is invalid");
        TEST_ASSERT(snapshot.threads().isEmpty(), "an invalid snapshot reports no threads");
        TEST_ASSERT(snapshot.regions().isEmpty(), "an invalid snapshot reports no regions");
        TEST_ASSERT(snapshot.loadedImages().isEmpty(), "an invalid snapshot reports no images");
        TEST_ASSERT(!snapshot.symbol("malloc"), "an invalid snapshot resolves no symbol");
        TEST_ASSERT(!snapshot.readBytes(JSC::Corpse::Address { uintptr_t(&firstId) }, 1),
            "an invalid snapshot reads nothing");
    }

    {
        Snapshot snapshot(nullptr);
        TEST_ASSERT(!snapshot.isValid(), "a snapshot with no process is invalid");
    }

    {
        Snapshot snapshot(process.ptr());
        TEST_ASSERT(!snapshot.symbol(nullptr), "an unnamed symbol resolves to nothing");
        TEST_ASSERT(!snapshot.symbol(""), "an empty symbol name resolves to nothing");
    }

    {
        // A corpse costs the analysing process a handle on every platform, and
        // taking one must not leave any of them behind. Reading the threads is
        // included because that is what takes the most on Darwin.
        unsigned footprintBefore = resourceFootprint();
        for (unsigned i = 0; i < 4; ++i) {
            Snapshot snapshot(process.ptr());
            if (!snapshot.isValid()) {
                TEST_ASSERT(false, "a snapshot of this process is valid");
                break;
            }
            snapshot.threads();
        }
        TEST_ASSERT_EQ(resourceFootprint(), footprintBefore,
            "taking and destroying snapshots leaves nothing behind");
    }
}

} // namespace JSCToolsTest

#endif // HAVE(CORPSE_SUPPORT)
