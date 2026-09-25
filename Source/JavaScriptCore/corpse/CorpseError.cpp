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

#if ENABLE(MYA)

#include "CorpseClient.h"

#include <stdarg.h>
#include <stdio.h>
#include <wtf/StdLibExtras.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC {
namespace Corpse {

thread_local unsigned Error::s_reportCount = 0;
thread_local Diagnostics* Diagnostics::s_current = nullptr;

static CString formatted(const char* format, va_list arguments) WTF_ATTRIBUTE_PRINTF(1, 0);
static CString formatted(const char* format, va_list arguments)
{
    va_list measuring;
    va_copy(measuring, arguments);
    int length = vsnprintf(nullptr, 0, format, measuring);
    va_end(measuring);
    if (length < 0)
        return { };
    Vector<char> buffer(static_cast<size_t>(length) + 1);
    vsnprintf(buffer.mutableSpan().data(), buffer.size(), format, arguments);
    return UTF8CString(byteCast<char8_t>(buffer.span().first(static_cast<size_t>(length))));
}

void Error::report(const char* format, ...)
{
    ++s_reportCount;
    SAFE_FPRINTF(stderr, "%s: ", Client::name());

    va_list args;
    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);

    fputc('\n', stderr);

    for (const Diagnostics* scope = Diagnostics::s_current; scope; scope = scope->m_parent)
        scope->print();
}

Diagnostics::Diagnostics(const char* format, ...)
    : m_parent(s_current)
{
    va_list args;
    va_start(args, format);
    m_operation = formatted(format, args);
    va_end(args);
    s_current = this;
}

Diagnostics::~Diagnostics()
{
    ASSERT(s_current == this);
    s_current = m_parent;
}

void Diagnostics::count(ASCIILiteral what, uint64_t by)
{
    for (auto& counter : m_counters) {
        if (counter.first == what) {
            counter.second += by;
            return;
        }
    }
    m_counters.append({ what, by });
}

uint64_t Diagnostics::total(ASCIILiteral what) const
{
    for (auto& counter : m_counters) {
        if (counter.first == what)
            return counter.second;
    }
    return 0;
}

void Diagnostics::note(const char* format, ...)
{
    va_list args;
    va_start(args, format);
    m_notes.append(formatted(format, args));
    va_end(args);
}

void Diagnostics::print() const
{
    SAFE_FPRINTF(stderr, "%s:   while %s", Client::name(), m_operation);
    ASCIILiteral separator = ": "_s;
    for (auto& [what, value] : m_counters) {
        SAFE_FPRINTF(stderr, "%s%s %llu", separator, what, static_cast<unsigned long long>(value));
        separator = ", "_s;
    }
    for (auto& note : m_notes)
        SAFE_FPRINTF(stderr, "; %s", note);
    fputc('\n', stderr);
}

} // namespace Corpse
} // namespace JSC

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(MYA)
