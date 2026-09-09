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
#include "CorpseProcFile.h"

#if HAVE(CORPSE_SUPPORT) && OS(LINUX)

#include <limits.h>
#include <unistd.h>
#include <wtf/FileHandle.h>
#include <wtf/FileSystem.h>
#include <wtf/text/MakeString.h>
#include <wtf/text/StringToIntegerConversion.h>

namespace JSC {
namespace Corpse {

static std::optional<Vector<uint8_t>> readWholeFileBytes(const String& path)
{
    auto handle = FileSystem::openFile(path, FileSystem::FileOpenMode::Read);
    if (!handle)
        return std::nullopt;

    Vector<uint8_t> contents;
    std::array<uint8_t, 4096> buffer;
    while (auto bytesRead = handle.read(std::span { buffer })) {
        if (!*bytesRead)
            break;
        contents.append(std::span { buffer }.first(*bytesRead));
    }
    return contents;
}

static std::optional<String> readWholeFile(const String& path)
{
    auto contents = readWholeFileBytes(path);
    if (!contents)
        return std::nullopt;
    return String::fromUTF8(contents->span());
}

std::optional<Vector<uint8_t>> ProcFile::readBytes(pid_t pid, ASCIILiteral name)
{
    return readWholeFileBytes(makeString("/proc/"_s, pid, '/', name));
}

std::optional<String> ProcFile::read(pid_t pid, ASCIILiteral name)
{
    return readWholeFile(makeString("/proc/"_s, pid, '/', name));
}

std::optional<String> ProcFile::read(pid_t pid, pid_t tid, ASCIILiteral name)
{
    return readWholeFile(makeString("/proc/"_s, pid, "/task/"_s, tid, '/', name));
}

std::optional<String> ProcFile::readLink(pid_t pid, ASCIILiteral name)
{
    auto path = makeString("/proc/"_s, pid, '/', name);
    std::array<char, PATH_MAX> buffer;
    ssize_t length = readlink(path.utf8().data(), buffer.data(), buffer.size());
    if (length <= 0 || static_cast<size_t>(length) >= buffer.size())
        return std::nullopt;
    return String::fromUTF8(std::span { buffer }.first(length));
}

std::optional<ProcStat> ProcStat::parse(std::optional<String>&& contents)
{
    if (!contents)
        return std::nullopt;

    size_t nameEnd = contents->reverseFind(')');
    if (nameEnd == notFound)
        return std::nullopt;

    ProcStat stat;
    stat.m_contents = WTF::move(*contents);
    for (auto field : StringView { stat.m_contents }.substring(nameEnd + 1).split(' '))
        stat.m_fields.append(field);
    return stat;
}

std::optional<ProcStat> ProcStat::forProcess(pid_t pid)
{
    return parse(ProcFile::read(pid, "stat"_s));
}

std::optional<ProcStat> ProcStat::forThread(pid_t pid, pid_t tid)
{
    return parse(ProcFile::read(pid, tid, "stat"_s));
}

std::optional<uint64_t> ProcStat::number(unsigned field) const
{
    if (field < stateField || field - stateField >= m_fields.size())
        return std::nullopt;
    return parseInteger<uint64_t>(m_fields[field - stateField]);
}

std::optional<char16_t> ProcStat::character(unsigned field) const
{
    if (field < stateField || field - stateField >= m_fields.size())
        return std::nullopt;
    auto value = m_fields[field - stateField];
    if (value.length() != 1)
        return std::nullopt;
    return value[0];
}

} // namespace Corpse
} // namespace JSC

#endif // HAVE(CORPSE_SUPPORT) && OS(LINUX)
