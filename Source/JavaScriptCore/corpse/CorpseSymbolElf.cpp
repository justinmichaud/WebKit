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
#include "CorpseSymbolResolver.h"

#if HAVE(CORPSE_SUPPORT) && !HAVE(CORPSE_EXPORTS_TRIE) && OS(LINUX)

#include "CorpseElfImage.h"
#include "CorpseSnapshot.h"

#include <wtf/TZoneMallocInlines.h>

namespace JSC {
namespace Corpse {

// Resolves a name the way the dynamic loader would: against the .dynsym of the
// image. A symbol that is not exported dynamically is not found, which is the
// same limit the Mach-O exports trie has on Darwin.
class ElfResolver final : public ImageSymbolResolver {
    WTF_MAKE_TZONE_ALLOCATED_INLINE(ElfResolver);
public:
    Address resolveIn(const Snapshot& snapshot, const ImageInfo& image, std::string_view name,
        ReadBudget& budget) final
    {
        auto parsed = ElfImage::parse(snapshot, image, budget);
        if (!parsed)
            return { }; // Not an ELF image with a readable dynamic symbol table.
        return parsed->lookUp(snapshot, name, budget);
    }
};

std::unique_ptr<ImageSymbolResolver> ImageSymbolResolver::create()
{
    return makeUnique<ElfResolver>();
}

} // namespace Corpse
} // namespace JSC

#endif // HAVE(CORPSE_SUPPORT) && !HAVE(CORPSE_EXPORTS_TRIE) && OS(LINUX)
