/*
 * Copyright (C) 2026 Apple Inc. All rights reserved.
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
#include <optional>
#include <stdint.h>
#include <wtf/Vector.h>
#include <wtf/text/WTFString.h>

namespace JSC {
namespace Corpse {

// One mapped region of a task's address space, as the kernel describes it.
class Region {
public:
    // The region containing `address`, or nullopt if not found in any region.
    static std::optional<Region> findContaining(TaskHandle, Address);

    // Every mapped region of the task, by address, without their page counts.
    // Empty, having reported why, if they cannot be listed.
    static Vector<Region> all(TaskHandle);

    // The region containing `address` among `regions`, which are by address.
    static std::optional<Region> findContaining(const Vector<Region>&, Address);

#if !OS(DARWIN)
    // Region::all, with their page counts, which costs more to read.
    static Vector<Region> allWithPageCounts(int pid);
#endif

    Address base() const { return m_base; }
    size_t size() const { return m_size; }
    Address end() const { return m_base + m_size; }
    bool contains(Address address) const { return address >= m_base && address < end(); }

    uint64_t pageCount() const;
    uint64_t residentPageCount() const { return m_residentPageCount; }
    uint64_t dirtyPageCount() const { return m_dirtyPageCount; }

    // The parts of the region whose pages are in memory or swapped out, which
    // are all that can hold anything but zeros: a large reservation is mostly
    // untouched. The whole region if the kernel cannot say.
    Vector<std::pair<Address, size_t>> residentParts(TaskHandle) const;

    bool isReadable() const { return m_isReadable; }
    bool isWritable() const { return m_isWritable; }
    bool isExecutable() const { return m_isExecutable; }

    // What the kernel names the region by: on Linux, the path of the file it
    // maps, or a label such as "[stack]". Empty for anonymous memory, and on Darwin.
    const String& name() const { return m_name; }

    static Region make(Address base, size_t size, bool isReadable, bool isWritable, bool isExecutable, String&& name = { })
    {
        Region region;
        region.m_base = base;
        region.m_size = size;
        region.m_isReadable = isReadable;
        region.m_isWritable = isWritable;
        region.m_isExecutable = isExecutable;
        region.m_name = WTF::move(name);
        return region;
    }

private:
    Address m_base;
    size_t m_size { 0 };
    uint64_t m_residentPageCount { 0 };
    uint64_t m_dirtyPageCount { 0 };
    bool m_isReadable { false };
    bool m_isWritable { false };
    bool m_isExecutable { false };
    String m_name;
};

} // namespace Corpse
} // namespace JSC

#endif // ENABLE(MYA)
