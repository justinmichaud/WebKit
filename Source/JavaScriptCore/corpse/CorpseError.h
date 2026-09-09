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

#pragma once

#include <JavaScriptCore/CorpsePlatform.h>

#if HAVE(CORPSE_SUPPORT)

#include <wtf/Assertions.h>

namespace JSC {
namespace Corpse {

// Reports the library's diagnostics. Messages are prefixed with the name the
// client set via Corpse::Client, so they read as the client's own output.
class Error {
public:
    static void report(const char* format, ...) WTF_ATTRIBUTE_PRINTF(1, 2);

    // Suppresses reports for as long as one of these is alive on this thread.
    //
    // A walk over a heap asks many questions whose answer is no: whether an
    // address holds an object of some type, whether a pointer is worth
    // following. Those are ordinary outcomes of the walk, not things to tell
    // the user about, and reporting each one would bury the findings that
    // matter. A caller that is probing takes one of these; a caller that is
    // answering a question the user asked does not.
    class Quiet {
    public:
        Quiet();
        ~Quiet();

        Quiet(const Quiet&) = delete;
        Quiet& operator=(const Quiet&) = delete;
    };

private:
    // Nesting is counted rather than flagged, so an inner scope does not turn
    // reporting back on for the outer one.
    static thread_local unsigned s_quietDepth;
};

} // namespace Corpse
} // namespace JSC

#endif // HAVE(CORPSE_SUPPORT)
