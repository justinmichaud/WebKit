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
#include <memory>
#include <stddef.h>
#include <string_view>

namespace JSC {
namespace Corpse {

class Snapshot;

// What a lookup may still copy out of a corpse. A corpse is read precisely when
// the target may be corrupt, so every count and size in it is a number that
// could send a reader anywhere; the budget is the backstop that keeps a bad one
// from turning a lookup into unbounded copying. One budget covers a whole
// lookup, so a corpse claiming many large images cannot multiply the cost by
// its image count.
class ReadBudget {
public:
    explicit ReadBudget(size_t bytes)
        : m_remaining(bytes)
    {
    }

    bool take(size_t length)
    {
        if (length > m_remaining) {
            m_exhausted = true;
            return false;
        }
        m_remaining -= length;
        return true;
    }

    bool isExhausted() const { return m_exhausted; }

private:
    size_t m_remaining { 0 };
    bool m_exhausted { false };
};

// Resolves an exported name against the images of a corpse, one image at a
// time. One of these is made per lookup, so a format that wants to say how far
// a failed search got has somewhere to keep that.
//
// This is the whole of what symbol lookup needs from an object-file format.
// Which images there are, in what order they are searched, and when to give up
// are the same questions whatever the format, and Symbol answers them once.
class ImageSymbolResolver {
public:
    // The resolver for this platform's object-file format.
    static std::unique_ptr<ImageSymbolResolver> create();

    virtual ~ImageSymbolResolver() = default;

    // The address of `name` in `image`, null if that image does not export it.
    // What "export" means is the format's business: the dynamic symbols of an
    // ELF image, the exports trie of a Mach-O one. A symbol the loader itself
    // could not resolve is not found either way.
    virtual Address resolveIn(const Snapshot&, const ImageInfo&, std::string_view name,
        ReadBudget&) = 0;

    // Called once when a lookup has searched every image and found nothing, so
    // a format can report how far it got. The default says nothing.
    virtual void reportFailure(const Snapshot&, std::string_view name) const { }
};

} // namespace Corpse
} // namespace JSC

#endif // HAVE(CORPSE_SUPPORT)
