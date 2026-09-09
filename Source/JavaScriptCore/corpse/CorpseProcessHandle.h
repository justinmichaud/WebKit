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

#include <JavaScriptCore/CorpseProcess.h>
#include <wtf/TZoneMallocInlines.h>

#if OS(DARWIN)
#include <mach/mach.h>
#endif

namespace JSC {
namespace Corpse {

// What Process::attach() holds on to.
//
// This is the one place the difference between the platforms' notions of "a
// handle on a process" is written down, which is why it is a header of its own
// rather than a pair of members in CorpseProcess.h: everything portable talks
// to Process, and only the files of the platform that owns the handle include
// this.
class Process::Handle {
    WTF_MAKE_TZONE_ALLOCATED_INLINE(Handle);
public:
#if OS(DARWIN)
    // A send right to the target's task. Valid exactly while attached.
    mach_port_t taskPort { MACH_PORT_NULL };
#else
    // Linux hands out no handle, so attach() records what distinguishes this
    // process from a later one that inherits its PID. The start time is
    // legitimately zero for PID 1, which is why attachment is tracked
    // separately rather than inferred from it.
    unsigned long long startTime { 0 };
    bool isAttached { false };
#endif
};

} // namespace Corpse
} // namespace JSC

#endif // HAVE(CORPSE_SUPPORT)
