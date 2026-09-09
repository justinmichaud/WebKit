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
#include <string>
#include <wtf/TZoneMalloc.h>

namespace JSC {
namespace Corpse {

class Snapshot;

// A symbol looked up in a corpse by name. The lookup happens on construction.
//
// What a lookup can reach is the object-file format's business, and
// ImageSymbolResolver is where that is written down. Either way it reaches
// what the loader itself would resolve and no more: the dynamic symbols of an
// ELF image, the exports trie of a Mach-O one. A symbol the linker hid is not
// found, and saying so is the honest answer rather than reaching for it by some
// other route.
class Symbol {
    WTF_MAKE_TZONE_ALLOCATED(Symbol);
public:
    Symbol(const Snapshot&, const char* name);

    const std::string& name() const { return m_name; }

    Address address() const { return m_address; } // Null means not found.
    bool isValid() const { return static_cast<bool>(m_address); }

private:
    Address lookUpName(const Snapshot&);

    std::string m_name;
    Address m_address;
};

} // namespace Corpse
} // namespace JSC

#endif // HAVE(CORPSE_SUPPORT)
