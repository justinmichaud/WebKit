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

#include <JavaScriptCore/CorpseAddress.h>
#include <JavaScriptCore/CorpseBackend.h>
#include <JavaScriptCore/CorpseSymbolResolver.h>
#include <optional>
#include <stdint.h>
#include <string_view>

namespace JSC {
namespace Corpse {

class Snapshot;

// One ELF image mapped into a corpse, read out of the corpse itself.
//
// This is the Linux counterpart of the Mach-O exports trie: it resolves a name
// against what the dynamic loader itself would resolve, which is the image's
// .dynsym. A symbol that is not exported dynamically is not found. Nothing here
// reads the loader state of the process doing the analysis, so an image of
// another process resolves as readily as one of this one.
//
// Every field the image supplies is treated as untrusted input: a count or
// offset that does not check out ends the walk rather than steering a read.
class ElfImage {
public:
    // Parses enough of the image at `info.loadAddress` to resolve names.
    // Returns nullopt for anything that is not a 64-bit ELF image with a
    // readable dynamic symbol table, which includes the vDSO-like mappings
    // that have no file behind them.
    static std::optional<ElfImage> parse(const Snapshot&, const ImageInfo&, ReadBudget&);

    // The address of `name` in this image, null if the image does not export
    // it. Undefined symbols, which name an import rather than a definition,
    // are skipped.
    Address lookUp(const Snapshot&, std::string_view name, ReadBudget&) const;

private:
    // The number of dynamic symbols the image has, as its hash table implies.
    // Nothing in an ELF image states it outright.
    static std::optional<uint32_t> countSymbolsFromGnuHash(const Snapshot&, Address table,
        ReadBudget&);

    Address m_loadAddress;
    uint64_t m_slide { 0 };
    Address m_symbolTable;
    Address m_stringTable;
    uint64_t m_stringTableSize { 0 };
    uint32_t m_symbolCount { 0 };
};

} // namespace Corpse
} // namespace JSC

#endif // HAVE(CORPSE_SUPPORT) && OS(LINUX)
