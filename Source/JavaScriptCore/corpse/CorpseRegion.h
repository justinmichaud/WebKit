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
#include <optional>
#include <stdint.h>

namespace JSC {
namespace Corpse {

class Snapshot;

// One mapped region of the corpse's address space, as the kernel described it
// when the corpse was taken.
class Region {
public:
    Region() = default;
    explicit Region(const RegionInfo&);

    // The region of `snapshot`'s corpse containing `address`, or nullopt if the
    // address falls in no region of it. The corpse is asked rather than the
    // live target, so what comes back describes the address space as it was
    // when the corpse was taken.
    static std::optional<Region> findContaining(const Snapshot&, Address);

    Address base() const { return m_info.base; }
    uint64_t size() const { return m_info.size; }
    Address end() const { return m_info.end(); }
    bool contains(Address address) const { return m_info.contains(address); }

    uint64_t pageCount() const;

    // Page accounting, where the platform reports it for this mapping.
    std::optional<uint64_t> residentPageCount() const { return m_info.residentPageCount; }
    std::optional<uint64_t> dirtyPageCount() const { return m_info.dirtyPageCount; }

    bool isReadable() const { return m_info.isReadable; }
    bool isWritable() const { return m_info.isWritable; }
    bool isExecutable() const { return m_info.isExecutable; }

private:
    RegionInfo m_info;
};

} // namespace Corpse
} // namespace JSC

#endif // HAVE(CORPSE_SUPPORT)
