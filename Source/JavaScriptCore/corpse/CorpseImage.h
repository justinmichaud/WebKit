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
#include <wtf/text/CString.h>

namespace JSC {
namespace Corpse {

class Snapshot;

// One image (the executable or a shared library) mapped into a snapshot.
class Image {
public:
    // Where the image's header is mapped. On Linux this is link_map's l_addr,
    // the slide, which is where the header is in a position-independent image.
    Address loadAddress() const { return m_loadAddress; }

    // What the loader recorded when it loaded the image.
    const UTF8CString& path() const { return m_path; }

#if OS(DARWIN)
    using UUID = std::array<uint8_t, 16>;
    const UUID& uuid() const { return m_uuid; }
#else
    // The NT_GNU_BUILD_ID note of the image as it is mapped.
    const Vector<uint8_t>& buildID() const { return m_buildID; }
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
#else
    Image(Address loadAddress, UTF8CString&& path, Vector<uint8_t>&& buildID)
        : m_loadAddress(loadAddress)
        , m_path(WTF::move(path))
        , m_buildID(WTF::move(buildID))
    {
    }
#endif

    Address m_loadAddress;
    UTF8CString m_path;
#if OS(DARWIN)
    UUID m_uuid;
#else
    Vector<uint8_t> m_buildID;
#endif
};

} // namespace Corpse
} // namespace JSC

#endif // ENABLE(MYA)
