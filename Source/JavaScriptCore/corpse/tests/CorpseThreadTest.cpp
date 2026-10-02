/*
 * Copyright (C) 2026 Apple Inc. All rights reserved.
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

#if ENABLE(MYA)

#include "LibJSCToolsTestUtilities.h"

#include <JavaScriptCore/CorpseRegion.h>
#include <JavaScriptCore/CorpseSnapshot.h>
#include <JavaScriptCore/CorpseThread.h>
#include <string>
#include <string_view>
#include <wtf/NeverDestroyed.h>

namespace JSCToolsTest {

using JSC::Corpse::Address;
using JSC::Corpse::Snapshot;
using JSC::Corpse::Thread;

static constexpr const char* alphaName = "jsctools alpha";
static constexpr const char* betaName = "jsctools beta";
// Longer than a pthread name can hold, so that truncation is exercised.
static constexpr const char* longName =
    "jsctools a thread whose name is far too long to fit in the space a pthread name has";

static ParkedThreads& parkedThreads()
{
    static NeverDestroyed<ParkedThreads> parked;
    return parked;
}

struct ParkedThreadsFixture {
    uint64_t count;
};

// In the target: threads parked under known names, until the process exits.
static Address createParkedThreads()
{
    static ParkedThreadsFixture fixture { };
    ParkedThreads& parked = parkedThreads();
    bool spawned = parked.spawn(alphaName) && parked.spawn(betaName) && parked.spawn(longName);
    if (!spawned || !parked.waitUntilAllParked())
        return { };
    fixture.count = parked.count();
    return Address { &fixture };
}

static void analyzeParkedThreads(Snapshot& snapshot, Address address)
{
    auto fixture = snapshot.memory().ptr<ParkedThreadsFixture>(address);
    TEST_ASSERT(fixture && fixture->count == 3, "the target's threads parked themselves");
    if (!fixture)
        return;

    const Vector<Thread>& threads = snapshot.threads();
    TEST_ASSERT(threads.size() >= 1 + fixture->count,
        "the snapshot holds at least the target's own threads");

    bool foundAlpha = false;
    bool foundBeta = false;
    bool foundTruncated = false;
    std::string expectedTruncated(std::string_view(longName).substr(0, ParkedThreads::maximumNameLength));

    for (const Thread& thread : threads) {
        bool parked = true;
        if (thread.name() == alphaName)
            foundAlpha = true;
        else if (thread.name() == betaName)
            foundBeta = true;
        else if (thread.name() == expectedTruncated)
            foundTruncated = true;
        else
            parked = false;

        TEST_ASSERT(thread.id(), "every thread has an identifier");
        TEST_ASSERT(thread.name().length() <= ParkedThreads::maximumNameLength,
            "no thread name is longer than a pthread name can be");

        // A parked thread is blocked, so its stack pointer was read. A running
        // thread's may not be. The stack is the region the stack pointer points
        // into, so if both were read they have to agree.
        if (parked)
            TEST_ASSERT(thread.stackPointer(), "a parked thread has a stack pointer");
        if (thread.stackPointer()) {
            TEST_ASSERT(thread.hasStack(), "a thread with a stack pointer has a stack region");
            if (thread.hasStack()) {
                TEST_ASSERT(thread.stackRegion().contains(thread.stackPointer()),
                    "a thread's stack pointer lies inside its stack region");
                TEST_ASSERT(thread.stackRegion().pageCount() >= thread.stackRegion().residentPageCount(),
                    "a stack has at least as many pages as it has resident");
            }
        }

        TEST_ASSERT(!std::string_view(thread.runStateDescription()).empty(),
            "a thread's run state has a name");
    }

    TEST_ASSERT(foundAlpha, "a named thread appears in the snapshot under its name");
    TEST_ASSERT(foundBeta, "a second named thread appears under its name");
    TEST_ASSERT(foundTruncated, "an overlong thread name appears cut to what a pthread name holds");

    // Reading the threads is the expensive part, so it happens once.
    const Vector<Thread>& again = snapshot.threads();
    TEST_ASSERT(&again == &threads, "the threads of a snapshot are read once and kept");
}

void testThreads()
{
    SuiteTracer tracer("Thread");
    if (!tracer.shouldRun())
        return;

    analyzeInAndOutOfProcess(createParkedThreads, analyzeParkedThreads);
    // The target's threads end with it; this process's are stopped here.
    parkedThreads().stopAndJoin();
}

} // namespace JSCToolsTest

#endif // ENABLE(MYA)
