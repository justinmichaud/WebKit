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
#include "CorpseError.h"

#if ENABLE(LIB_JSC_TOOLS)

#include "CorpseClient.h"

#include <stdarg.h>
#include <stdio.h>
#include <wtf/StdLibExtras.h>
#include <wtf/StringPrintStream.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC {
namespace Corpse {

thread_local unsigned Error::s_reportCount = 0;
thread_local Diagnostics* Diagnostics::s_current = nullptr;

void Error::report(const char* format, ...)
{
    ++s_reportCount;

    StringPrintStream out;
    out.print(Client::name(), ": ");
    va_list args;
    va_start(args, format);
    out.vprintf(format, args);
    va_end(args);
    out.print("\n", Diagnostics::context());

    SAFE_FPRINTF(stderr, "%s", out.toUTF8CString());
}

UTF8CString Diagnostics::describe(const char* format, ...)
{
    StringPrintStream out;
    va_list args;
    va_start(args, format);
    out.vprintf(format, args);
    va_end(args);
    return out.toUTF8CString();
}

Diagnostics::Diagnostics(UTF8CString&& operation)
    : m_operation(WTF::move(operation))
    , m_parent(s_current)
{
    s_current = this;
}

Diagnostics::~Diagnostics()
{
    ASSERT(s_current == this);
    s_current = m_parent;
}

UTF8CString Diagnostics::context()
{
    StringPrintStream out;
    for (const Diagnostics* scope = s_current; scope; scope = scope->m_parent)
        out.print(Client::name(), ":   while ", scope->m_operation, "\n");
    return out.toUTF8CString();
}

} // namespace Corpse
} // namespace JSC

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(LIB_JSC_TOOLS)
