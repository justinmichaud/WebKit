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
#include "CorpseThread.h"

#if HAVE(CORPSE_SUPPORT)

#include "CorpseError.h"
#include "CorpseSnapshot.h"

namespace JSC {
namespace Corpse {

// The corpse already holds every thread, captured with the memory it froze;
// this only pairs each one with the region its stack pointer lands in.
Vector<Thread> Thread::collect(const Snapshot& snapshot)
{
    Vector<Thread> result;

    const Backend* backend = snapshot.backend();
    if (!backend) {
        Error::report("Cannot read threads from an invalid snapshot");
        return result;
    }

    const auto& infos = backend->threads();
    result.reserveInitialCapacity(infos.size());
    for (const auto& info : infos) {
        Thread thread;
        thread.m_info = info;
        thread.m_runStateDescription = backend->describeRunState(info.runState);

        // The stack is the region the stack pointer points into. A thread whose
        // registers were refused has no stack pointer to look up, which is why
        // hasStack() and hasRegisters() are separate questions.
        if (info.registers.isComplete && info.registers.stackPointer) {
            if (auto region = snapshot.regionContaining(info.registers.stackPointer))
                thread.m_stackRegion = *region;
        }

        result.append(WTF::move(thread));
    }
    return result;
}

} // namespace Corpse
} // namespace JSC

#endif // HAVE(CORPSE_SUPPORT)
