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

#if HAVE(CORPSE_SUPPORT) && OS(LINUX)

#include <optional>
#include <sys/types.h>
#include <stdint.h>
#include <wtf/Vector.h>
#include <wtf/text/ASCIILiteral.h>
#include <wtf/text/StringView.h>
#include <wtf/text/WTFString.h>

namespace JSC {
namespace Corpse {

// A file under /proc. These report no size, so they cannot be read through the
// usual read-an-entire-file helpers.
class ProcFile {
public:
    static std::optional<String> read(pid_t, ASCIILiteral name);

    // The raw bytes of a /proc file, for the ones that are not text. The
    // auxiliary vector is an array of 64-bit pairs, not a string.
    static std::optional<Vector<uint8_t>> readBytes(pid_t, ASCIILiteral name);
    static std::optional<String> read(pid_t, pid_t tid, ASCIILiteral name);
    static std::optional<String> readLink(pid_t, ASCIILiteral name);
};

// A stat file, whose fields are numbered as proc(5) numbers them. Fields 1 and
// 2 are not offered: field 2 is the executable name, which may itself contain
// spaces and parentheses, so only what follows it can be split on spaces.
class ProcStat {
public:
    static std::optional<ProcStat> forProcess(pid_t);
    static std::optional<ProcStat> forThread(pid_t, pid_t tid);

    static constexpr unsigned stateField = 3;
    static constexpr unsigned userTimeField = 14;
    static constexpr unsigned systemTimeField = 15;
    static constexpr unsigned startTimeField = 22;

    std::optional<uint64_t> number(unsigned field) const;
    std::optional<char16_t> character(unsigned field) const;

private:
    static std::optional<ProcStat> parse(std::optional<String>&&);

    String m_contents;
    Vector<StringView> m_fields;
};

} // namespace Corpse
} // namespace JSC

#endif // HAVE(CORPSE_SUPPORT) && OS(LINUX)
