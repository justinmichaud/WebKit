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
#include "CorpseSymbol.h"

#if HAVE(CORPSE_SUPPORT)

#include "CorpseError.h"
#include "CorpseSnapshot.h"
#include "CorpseSymbolResolver.h"

#include <wtf/StdLibExtras.h>
#include <wtf/TZoneMallocInlines.h>

namespace JSC {
namespace Corpse {

WTF_MAKE_TZONE_ALLOCATED_IMPL(Symbol);

namespace {

// A lookup that finds nothing reads every image's symbol data. This caps the
// total for one lookup, so a corpse claiming many large images cannot turn a
// single symbol lookup into unbounded copying. Measured at 101 MB for a process
// with ~2,800 images and 0.4 MB for a small one.
constexpr size_t maxTotalBytesRead = 256 * MB; // About 2.5x the measured maximum.

} // anonymous namespace

Symbol::Symbol(const Snapshot& snapshot, const char* name)
    : m_name(name ? name : "")
{
    if (!m_name.empty())
        m_address = lookUpName(snapshot);
}

// Searches the images of the corpse in the order the corpse reports them,
// stopping at the first that defines the name. Reading each image's symbols is
// the format's business; everything here is the same either way.
Address Symbol::lookUpName(const Snapshot& snapshot)
{
    if (!snapshot.isValid() || m_name.empty())
        return { };

    const auto& images = snapshot.loadedImages();
    if (images.isEmpty())
        return { };

    auto resolver = ImageSymbolResolver::create();
    if (!resolver)
        return { };

    // One budget for the whole lookup, shared across every image it touches.
    ReadBudget budget { maxTotalBytesRead };

    for (const auto& image : images) {
        if (auto address = resolver->resolveIn(snapshot, image, m_name, budget))
            return address;
        if (budget.isExhausted())
            break;
    }

    if (budget.isExhausted()) {
        Error::report("Gave up looking for symbol '%s' after reading %u MB from the corpse",
            m_name.c_str(), static_cast<unsigned>(maxTotalBytesRead / MB));
    } else
        resolver->reportFailure(snapshot, m_name);
    return { };
}

} // namespace Corpse
} // namespace JSC

#endif // HAVE(CORPSE_SUPPORT)
