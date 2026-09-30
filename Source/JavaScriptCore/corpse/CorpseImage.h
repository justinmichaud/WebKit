/*
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

#if ENABLE(MYA)

#include <JavaScriptCore/CorpseAddress.h>
#include <array>
#include <stdint.h>
#include <wtf/Vector.h>
#if OS(DARWIN)
#include <wtf/text/CString.h>
#else
#include <wtf/unix/UnixFileDescriptor.h>
#endif

namespace JSC {
namespace Corpse {

class Snapshot;

// One image (the executable or a shared library) mapped into a snapshot.
class Image {
public:
    Address loadAddress() const { return m_loadAddress; } // Where the image's header is mapped.

#if OS(DARWIN)
    using UUID = std::array<uint8_t, 16>;

    // What dyld recorded when it loaded the image.
    const UTF8CString& path() const { return m_path; }
    const UUID& uuid() const { return m_uuid; }
#else
    // The mapped file itself, held open by the snapshot.
    int fileDescriptor() const { return m_file.value(); }
#endif

private:
    friend class Snapshot;
    static Vector<Image> collect(Snapshot&);

#if OS(DARWIN)
    Image(Address loadAddress, UTF8CString&& path, const UUID& uuid)
        : m_loadAddress(loadAddress)
        , m_path(WTF::move(path))
        , m_uuid(uuid)
    {
    }

    Address m_loadAddress;
    UTF8CString m_path;
    UUID m_uuid;
#else
    Image(Address loadAddress, UnixFileDescriptor&& file)
        : m_loadAddress(loadAddress)
        , m_file(WTF::move(file))
    {
    }

    Address m_loadAddress;
    UnixFileDescriptor m_file;
#endif
};

} // namespace Corpse
} // namespace JSC

#endif // ENABLE(MYA)
