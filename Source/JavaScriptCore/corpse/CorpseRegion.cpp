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
#include "CorpseRegion.h"

#if ENABLE(MYA)

#include "CorpseError.h"
#include <algorithm>
#include "CorpseLimits.h"

#if OS(DARWIN)
#include <mach/mach_vm.h>
#else
#include "CorpseProcess.h"
#include <array>
#include <charconv>
#include <fcntl.h>
#include <string_view>
#include <unistd.h>
#include <wtf/text/MakeString.h>
#endif

namespace JSC {
namespace Corpse {

#if OS(DARWIN)

uint64_t Region::pageCount() const
{
    return vm_kernel_page_size ? m_size / vm_kernel_page_size : 0;
}

// The region at or above `address`, descending into submaps.
static std::optional<std::pair<Region, vm_region_submap_info_data_64_t>> regionAtOrAbove(mach_port_t task, Address address)
{
    mach_vm_address_t regionAddress = 0;
    mach_vm_size_t regionSize = 0;
    vm_region_submap_info_data_64_t info;
    for (natural_t depth = 0; ; ++depth) {
        regionAddress = address.toTargetVMAddress();
        regionSize = 0;
        natural_t depthLimit = depth; // We tell the kernel how deep we want to go. Kernel tells us how deep it can go.
        mach_msg_type_number_t infoCount = VM_REGION_SUBMAP_INFO_COUNT_64;
        kern_return_t kr = mach_vm_region_recurse(task, &regionAddress, &regionSize,
            &depthLimit, reinterpret_cast<vm_region_recurse_info_t>(&info), &infoCount);
        if (kr != KERN_SUCCESS)
            return std::nullopt;
        if (!info.is_submap)
            break;
    }
    return std::pair { Region::make(Address(regionAddress), static_cast<size_t>(regionSize), info.protection & VM_PROT_READ, info.protection & VM_PROT_WRITE, info.protection & VM_PROT_EXECUTE), info };
}

std::optional<Region> Region::findContaining(mach_port_t task, Address address)
{
    // mach_vm_region_recurse reports the region at or above the address it is given,
    // so the result only describes `address` if it turns out to contain it.
    auto found = regionAtOrAbove(task, address);
    if (!found || !found->first.contains(address))
        return std::nullopt;
    Region region = found->first;
    region.m_residentPageCount = found->second.pages_resident;
    region.m_dirtyPageCount = found->second.pages_dirtied;
    return region;
}

Vector<Region> Region::all(mach_port_t task)
{
    Vector<Region> result;
    Address address;
    while (auto found = regionAtOrAbove(task, address)) {
        if (result.size() >= maxRegionCount) {
            CORPSE_REPORT("The task lists more than %zu regions", maxRegionCount);
            return { };
        }
        if (found->first.end() <= address)
            break;
        Region region = found->first;
        region.m_residentPageCount = found->second.pages_resident;
        region.m_dirtyPageCount = found->second.pages_dirtied;
        result.append(region);
        address = region.end();
    }
    return result;
}

Vector<std::pair<Address, size_t>> Region::residentParts(mach_port_t) const
{
    // Region::all reads each region's resident page count, but not which pages they are.
    if (!m_residentPageCount)
        return { };
    return { { m_base, m_size } };
}

#else

uint64_t Region::pageCount() const
{
    return m_size / static_cast<size_t>(getpagesize());
}

// One line of /proc/<pid>/maps: "start-end perms offset dev inode   name".
static std::optional<Region> parseMapsLine(std::string_view line)
{
    auto field = [&]() {
        size_t start = line.find_first_not_of(' ');
        if (start == std::string_view::npos)
            return std::string_view { };
        line.remove_prefix(start);
        size_t end = std::min(line.find(' '), line.size());
        std::string_view result = line.substr(0, end);
        line.remove_prefix(end);
        return result;
    };
    std::string_view range = field();
    std::string_view permissions = field();
    field(); // Offset.
    field(); // Device.
    field(); // Inode.
    size_t nameStart = line.find_first_not_of(' ');
    std::string_view name = nameStart == std::string_view::npos ? std::string_view { } : line.substr(nameStart);

    size_t dash = range.find('-');
    uint64_t start = 0;
    uint64_t end = 0;
    if (dash == std::string_view::npos || permissions.size() < 3
        || std::from_chars(range.data(), range.data() + dash, start, 16).ec != std::errc { }
        || std::from_chars(range.data() + dash + 1, range.data() + range.size(), end, 16).ec != std::errc { }
        || end <= start)
        return std::nullopt;
    return Region::make(Address { start }, static_cast<size_t>(end - start), permissions[0] == 'r', permissions[1] == 'w', permissions[2] == 'x',
        String::fromUTF8(std::span { name.data(), name.size() }));
}

Vector<Region> Region::all(int pid)
{
    auto maps = readProcFile(pid, "maps");
    if (!maps) {
        CORPSE_REPORT("Could not read the memory map of pid %d", pid);
        return { };
    }
    Vector<Region> result;
    std::string_view rest { *maps };
    while (!rest.empty()) {
        size_t end = std::min(rest.find('\n'), rest.size());
        if (auto region = parseMapsLine(rest.substr(0, end))) {
            if (result.size() >= maxRegionCount) {
                CORPSE_REPORT("The memory map of pid %d lists more than %zu regions", pid, maxRegionCount);
                return { };
            }
            result.append(WTF::move(*region));
        }
        rest.remove_prefix(std::min(end + 1, rest.size()));
    }
    return result;
}

Vector<std::pair<Address, size_t>> Region::residentParts(int pid) const
{
    // /proc/<pid>/pagemap has a 64-bit entry for each page: bit 63 is set for
    // a page in memory, and bit 62 for one in swap.
    constexpr uint64_t present = 1ull << 63;
    constexpr uint64_t swapped = 1ull << 62;
    uint64_t pageSize = static_cast<uint64_t>(getpagesize());
    ASCIICString path = makeString("/proc/"_s, pid, "/pagemap"_s).ascii();
    int file = open(path.data(), O_RDONLY | O_CLOEXEC);
    if (file < 0)
        return { { m_base, m_size } };

    Vector<std::pair<Address, size_t>> result;
    std::array<uint64_t, 64 * 1024> entries;
    uint64_t firstPage = m_base.toTargetVMAddress() / pageSize;
    uint64_t pageCount = m_size / pageSize;
    for (uint64_t page = 0; page < pageCount;) {
        uint64_t batch = std::min<uint64_t>(entries.size(), pageCount - page);
        ssize_t length = pread(file, entries.data(), batch * sizeof(uint64_t), static_cast<off_t>((firstPage + page) * sizeof(uint64_t)));
        if (length <= 0) {
            close(file);
            return { { m_base, m_size } };
        }
        uint64_t read = static_cast<uint64_t>(length) / sizeof(uint64_t);
        for (uint64_t index = 0; index < read; ++index) {
            if (!(entries[index] & (present | swapped)))
                continue;
            Address address = m_base + (page + index) * pageSize;
            if (!result.isEmpty() && result.last().first + result.last().second == address)
                result.last().second += pageSize;
            else
                result.append({ address, static_cast<size_t>(pageSize) });
        }
        page += read;
    }
    close(file);
    return result;
}

// The "<field>: <n> kB" lines of a region's entry in /proc/<pid>/smaps, in pages.
static uint64_t smapsPages(std::string_view entry, std::string_view field)
{
    uint64_t pages = 0;
    size_t position = 0;
    while ((position = entry.find(field, position)) != std::string_view::npos) {
        bool atLineStart = !position || entry[position - 1] == '\n';
        position += field.size();
        if (!atLineStart || position >= entry.size() || entry[position] != ':')
            continue;
        size_t digits = entry.find_first_not_of(' ', position + 1);
        uint64_t kilobytes = 0;
        if (digits != std::string_view::npos)
            std::from_chars(entry.data() + digits, entry.data() + entry.size(), kilobytes);
        pages += kilobytes * 1024 / static_cast<uint64_t>(getpagesize());
    }
    return pages;
}

Vector<Region> Region::allWithPageCounts(int pid)
{
    auto smaps = readProcFile(pid, "smaps");
    if (!smaps) {
        CORPSE_REPORT("Could not read the memory map of pid %d", pid);
        return { };
    }
    // An entry is its maps line, then lines of "Field: value", up to the next maps line.
    Vector<Region> result;
    std::string_view rest { *smaps };
    while (!rest.empty()) {
        size_t end = std::min(rest.find('\n'), rest.size());
        auto region = parseMapsLine(rest.substr(0, end));
        rest.remove_prefix(std::min(end + 1, rest.size()));
        if (!region)
            continue;
        size_t entryEnd = 0;
        while (entryEnd < rest.size()) {
            size_t lineEnd = std::min(rest.find('\n', entryEnd), rest.size());
            if (parseMapsLine(rest.substr(entryEnd, lineEnd - entryEnd)))
                break;
            entryEnd = lineEnd + 1;
        }
        std::string_view entry = rest.substr(0, std::min(entryEnd, rest.size()));
        region->m_residentPageCount = smapsPages(entry, "Rss");
        region->m_dirtyPageCount = smapsPages(entry, "Private_Dirty") + smapsPages(entry, "Shared_Dirty");
        if (result.size() >= maxRegionCount) {
            CORPSE_REPORT("The memory map of pid %d lists more than %zu regions", pid, maxRegionCount);
            return { };
        }
        result.append(WTF::move(*region));
        rest.remove_prefix(std::min(entryEnd, rest.size()));
    }
    return result;
}

std::optional<Region> Region::findContaining(int pid, Address address)
{
    return findContaining(allWithPageCounts(pid), address);
}

#endif // OS(DARWIN)

std::optional<Region> Region::findContaining(const Vector<Region>& regions, Address address)
{
    auto span = regions.span();
    auto after = std::ranges::upper_bound(span, address, { }, &Region::base);
    if (after == span.begin() || !(after - 1)->contains(address))
        return std::nullopt;
    return *(after - 1);
}

} // namespace Corpse
} // namespace JSC

#endif // ENABLE(MYA)
