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

#include <JavaScriptCore/CorpseAddress.h>
#include <JavaScriptCore/CorpseBackend.h>
#include <JavaScriptCore/CorpseRegion.h>
#include <stdint.h>
#include <wtf/Vector.h>
#include <wtf/text/CString.h>

namespace JSC {
namespace Corpse {

class Snapshot;

// A thread as the corpse describes it. Its registers and the memory they point
// into were captured at one instant, so a stack pointer here always indexes
// memory of the same moment.
class Thread {
public:
    // A stable identifier for the thread: the kernel's system-wide 64-bit
    // thread id on Darwin, the TID on Linux. An identifier, not an address.
    uint64_t id() const { return m_info.id; }

    // The thread's name, empty if it was never named.
    const CString& name() const { return m_info.name; }

    int runState() const { return m_info.runState; }
    int suspendCount() const { return m_info.suspendCount; }
    uint64_t userTimeUsec() const { return m_info.userTimeUsec; }
    uint64_t systemTimeUsec() const { return m_info.systemTimeUsec; }

    // True when the corpse carries this thread's registers. False means the
    // thread exists but cannot be walked, and registerFailure() says why; it
    // never means the thread has no stack.
    bool hasRegisters() const { return m_info.registers.isComplete; }
    RegisterFailure registerFailure() const { return m_info.registerFailure; }

    Address stackPointer() const { return m_info.registers.stackPointer; }
    Address programCounter() const { return m_info.registers.programCounter; }
    Address framePointer() const { return m_info.registers.framePointer; }

    const Region& stackRegion() const { return m_stackRegion; }
    bool hasStack() const { return m_stackRegion.size(); }

    const char* runStateDescription() const { return m_runStateDescription; }

private:
    static Vector<Thread> collect(const Snapshot&);

    ThreadInfo m_info;
    Region m_stackRegion;

    // Resolved when the thread is collected, since only the backend that
    // produced the run state can word it.
    const char* m_runStateDescription { "unknown" };

    friend class Snapshot;
};

} // namespace Corpse
} // namespace JSC

#endif // HAVE(CORPSE_SUPPORT)
