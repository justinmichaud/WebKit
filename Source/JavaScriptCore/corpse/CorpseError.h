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

#if ENABLE(MYA)

#include <stdint.h>
#include <utility>
#include <wtf/Assertions.h>
#include <wtf/Noncopyable.h>
#include <wtf/Vector.h>
#include <wtf/text/ASCIILiteral.h>
#include <wtf/text/CString.h>

namespace JSC {
namespace Corpse {

class Diagnostics;

// Reports the library's failures. Messages are prefixed with the name the
// client set via Corpse::Client, so they read as the client's own output. A
// report made while Diagnostics scopes are open is followed by what each of
// them had done by then.
class Error {
public:
    static void report(const char* format, ...) WTF_ATTRIBUTE_PRINTF(1, 2);

    static unsigned reportCount() { return s_reportCount; }

private:
    static thread_local unsigned s_reportCount;
};

// What an operation has done so far, so that a failure can say how far it got.
// An operation opens one as a local; while it lives it is on the thread's
// stack of open scopes. Steps bump counters and add notes as they go, and
// nothing is printed unless something reports, when every open scope prints
// its counters and notes after the failure, innermost first.
class Diagnostics {
    WTF_MAKE_NONCOPYABLE(Diagnostics);
public:
    Diagnostics(const char* format, ...) WTF_ATTRIBUTE_PRINTF(2, 3);
    ~Diagnostics();

    void count(ASCIILiteral what, uint64_t by = 1);
    uint64_t total(ASCIILiteral what) const;
    void note(const char* format, ...) WTF_ATTRIBUTE_PRINTF(2, 3);

private:
    void print() const;

    CString m_operation;
    Vector<std::pair<ASCIILiteral, uint64_t>> m_counters;
    Vector<CString> m_notes;
    Diagnostics* const m_parent;

    static thread_local Diagnostics* s_current;

    friend class Error;
};

} // namespace Corpse
} // namespace JSC

// The printf-style entry points. Arguments get the format checking of
// SAFE_PRINTF, and a C string owned by a C interface (liblldb, Mach, dyld)
// is accepted as it is.
#define CORPSE_REPORT(format, ...) \
    WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN \
    ::JSC::Corpse::Error::report(format __VA_OPT__(, LOG_PRINTF_TYPE(__VA_ARGS__))) \
    WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#define CORPSE_DIAGNOSTICS(variable, format, ...) \
    WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN \
    ::JSC::Corpse::Diagnostics variable(format __VA_OPT__(, LOG_PRINTF_TYPE(__VA_ARGS__))) \
    WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#define CORPSE_NOTE(diagnostics, format, ...) \
    WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN \
    (diagnostics).note(format __VA_OPT__(, LOG_PRINTF_TYPE(__VA_ARGS__))) \
    WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(MYA)
