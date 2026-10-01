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

#include "config.h"
#include "CorpseImage.h"

#if ENABLE(MYA)

#include "CorpseError.h"
#include "CorpseLimits.h"
#include "CorpseProcess.h"
#include "CorpseSnapshot.h"
#include <algorithm>

#if OS(DARWIN)
#include <mach-o/dyld_images.h>
#include <wtf/HashMap.h>
#else
#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <link.h>
#include <unistd.h>
#include <wtf/SafeStrerror.h>
#include <wtf/text/MakeString.h>
#endif

namespace JSC {
namespace Corpse {

static std::optional<UTF8CString> readPath(Memory& memory, Address address)
{
    // Read a page at a time: the string may end just before an unmapped page.
    Vector<char8_t> characters;
    size_t pageSize = Memory::pageSize();
    while (characters.size() < maxImagePathLength) {
        Address next = address + characters.size();
        size_t toPageEnd = pageSize - (next.toTargetVMAddress() & (pageSize - 1));
        auto chunk = memory.span<char8_t>(next, std::min(toPageEnd, maxImagePathLength - characters.size()));
        if (!chunk)
            return std::nullopt;
        size_t length = std::ranges::find(chunk, u8'\0') - chunk.begin();
        characters.append(chunk.first(length));
        if (length < chunk.size())
            return UTF8CString(characters.span());
    }
    return std::nullopt;
}

#if OS(DARWIN)

// dyld lists a UUID for every image outside the shared cache. Shared-cache
// images have no debug info to find, so they are not listed here.
Vector<Image> Image::collect(Snapshot& snapshot)
{
    auto allImages = snapshot.dyldAllImageInfos();
    if (!allImages)
        return { };

    Address uuidsAddress = Address { allImages->uuidArray }.stripped();
    uint64_t uuidCount = allImages->uuidArrayCount;
    Address infosAddress = Address { allImages->infoArray }.stripped();
    uint32_t infoCount = allImages->infoArrayCount;
    if (!uuidsAddress || !uuidCount || !infosAddress || !infoCount) {
        CORPSE_REPORT("dyld_all_image_infos v%u lists %llu image UUIDs and %u images",
            allImages->version, static_cast<unsigned long long>(uuidCount), infoCount);
        return { };
    }
    if (uuidCount > maxImageCount || infoCount > maxImageCount) {
        CORPSE_REPORT("%llu image UUIDs and %u images is too many to be a real image list",
            static_cast<unsigned long long>(uuidCount), infoCount);
        return { };
    }

    Memory& memory = snapshot.memory();
    auto uuids = memory.span<dyld_uuid_info>(uuidsAddress, uuidCount);
    if (!uuids) {
        CORPSE_REPORT("Could not read the %llu image UUIDs at 0x%llx", static_cast<unsigned long long>(uuidCount), uuidsAddress.toTargetVMAddress());
        return { };
    }
    auto infos = memory.span<dyld_image_info>(infosAddress, infoCount);
    if (!infos) {
        CORPSE_REPORT("Could not read the %u image entries at 0x%llx", infoCount, infosAddress.toTargetVMAddress());
        return { };
    }
    Diagnostics::count(DiagnosticCounter::ImagesListed, uuidCount);

    HashMap<Address, Address> pathAddresses;
    for (const dyld_image_info& info : infos) {
        Address loadAddress = Address { info.imageLoadAddress }.stripped();
        if (loadAddress && loadAddress != Address::deletedValue())
            pathAddresses.add(loadAddress, Address { info.imageFilePath }.stripped());
    }

    Vector<Image> result;
    result.reserveInitialCapacity(uuids.size());
    for (const dyld_uuid_info& entry : uuids) {
        Address loadAddress = Address { entry.imageLoadAddress }.stripped();
        auto pathAddress = loadAddress && loadAddress != Address::deletedValue() ? pathAddresses.getOptional(loadAddress) : std::nullopt;
        auto path = pathAddress ? readPath(memory, *pathAddress) : std::nullopt;
        if (!path) {
            Diagnostics::count(DiagnosticCounter::ImagesWithoutPath);
            continue;
        }
        UUID uuid;
        std::ranges::copy(entry.imageUUID, uuid.begin());
        result.append(Image { loadAddress, WTF::move(*path), uuid });
    }
    return result;
}

#else

// glibc's r_debug, found as ld.so's debugger interface says: the kernel's
// auxiliary vector gives the executable's program headers, whose PT_DYNAMIC
// has a DT_DEBUG entry that ld.so points at r_debug.
static std::optional<Address> loaderDebugState(Memory& memory, int pid)
{
    ASCIICString auxvPath = makeString("/proc/"_s, pid, "/auxv"_s).ascii();
    int file = open(auxvPath.data(), O_RDONLY | O_CLOEXEC);
    if (file < 0) {
        CORPSE_REPORT("Could not open the auxiliary vector of pid %d: %s", pid, safeStrerror(errno).data());
        return std::nullopt;
    }
    std::array<Elf64_auxv_t, maxAuxiliaryVectorEntries> auxv;
    ssize_t length = read(file, auxv.data(), sizeof(auxv));
    close(file);
    Address programHeadersAddress;
    uint64_t programHeaderCount = 0;
    for (const Elf64_auxv_t& entry : std::span { auxv }.first(std::max<ssize_t>(length, 0) / sizeof(Elf64_auxv_t))) {
        if (entry.a_type == AT_PHDR)
            programHeadersAddress = Address { entry.a_un.a_val };
        else if (entry.a_type == AT_PHNUM)
            programHeaderCount = entry.a_un.a_val;
    }
    if (!programHeadersAddress || !programHeaderCount || programHeaderCount > maxProgramHeaders) {
        CORPSE_REPORT("The auxiliary vector of pid %d gives %llu program headers at 0x%llx",
            pid, static_cast<unsigned long long>(programHeaderCount), programHeadersAddress.toTargetVMAddress());
        return std::nullopt;
    }
    auto programHeaders = memory.span<Elf64_Phdr>(programHeadersAddress, programHeaderCount);
    if (!programHeaders) {
        CORPSE_REPORT("Could not read the executable's program headers at 0x%llx", programHeadersAddress.toTargetVMAddress());
        return std::nullopt;
    }

    // PT_PHDR is where the headers are before the slide.
    std::optional<uint64_t> slide;
    std::optional<uint64_t> dynamic;
    for (const Elf64_Phdr& header : programHeaders) {
        if (header.p_type == PT_PHDR)
            slide = programHeadersAddress.toTargetVMAddress() - header.p_vaddr;
        else if (header.p_type == PT_DYNAMIC)
            dynamic = header.p_vaddr;
    }
    if (!slide || !dynamic) {
        CORPSE_REPORT("The executable of pid %d has no PT_PHDR or no PT_DYNAMIC", pid);
        return std::nullopt;
    }

    for (size_t index = 0; index < maxDynamicEntries; ++index) {
        auto entry = memory.ptr<Elf64_Dyn>(Address { *slide + *dynamic } + index * sizeof(Elf64_Dyn));
        if (!entry || entry->d_tag == DT_NULL)
            break;
        if (entry->d_tag == DT_DEBUG && entry->d_un.d_ptr)
            return Address { entry->d_un.d_ptr };
    }
    CORPSE_REPORT("The executable of pid %d has no DT_DEBUG that ld.so filled in", pid);
    return std::nullopt;
}

// ld.so lists every image it loaded in r_debug's link_map chain, as dyld does
// in dyld_all_image_infos.
Vector<Image> Image::collect(Snapshot& snapshot)
{
    int pid = static_cast<int>(snapshot.process()->pid());
    Memory& memory = snapshot.memory();
    auto debugStateAddress = loaderDebugState(memory, pid);
    if (!debugStateAddress)
        return { };
    auto debugState = memory.ptr<r_debug>(*debugStateAddress);
    if (!debugState) {
        CORPSE_REPORT("Could not read r_debug at 0x%llx", debugStateAddress->toTargetVMAddress());
        return { };
    }

    Vector<Image> result;
    unsigned listed = 0;
    for (Address next { debugState->r_map }; next; ++listed) {
        if (listed >= maxImageCount) {
            CORPSE_REPORT("pid %d has more than %u images, too many to be a real image list", pid, maxImageCount);
            return { };
        }
        auto entry = memory.ptr<link_map>(next);
        if (!entry) {
            CORPSE_REPORT("Could not read the link_map at 0x%llx", next.toTargetVMAddress());
            break;
        }
        next = Address { entry->l_next };
        auto path = readPath(memory, Address { entry->l_name });
        // Only the executable's entry has no name.
        if (path && !path->length())
            path = snapshot.process()->executablePath();
        if (!path || !path->length()) {
            Diagnostics::count(DiagnosticCounter::ImagesWithoutPath);
            continue;
        }
        result.append(Image { Address { entry->l_addr }, WTF::move(*path) });
    }
    Diagnostics::count(DiagnosticCounter::ImagesListed, listed);
    return result;
}

#endif // OS(DARWIN)

} // namespace Corpse
} // namespace JSC

#endif // ENABLE(MYA)
