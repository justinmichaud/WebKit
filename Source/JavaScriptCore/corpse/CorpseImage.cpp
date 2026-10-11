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

#if ENABLE(LIB_JSC_TOOLS)

#include "CorpseError.h"
#include "CorpseLimits.h"
#include "CorpseProcess.h"
#include "CorpseSnapshot.h"
#include <algorithm>

#if OS(DARWIN)
#include <mach-o/dyld_images.h>
#include <mach-o/loader.h>
#include <mach/mach.h>
#include <mach/mach_error.h>
#include <mach/task_info.h>
#include <wtf/HashMap.h>
#include <wtf/HexNumber.h>
#else
#include <elf.h>
#include <string_view>
#include <wtf/posix/POSIXExtras.h>
#endif

namespace JSC {
namespace Corpse {

#if OS(DARWIN)

// TASK_DYLD_INFO is in <mach/task_info.h>; the structures are dyld's, in <mach-o/dyld_images.h>.
Memory::Ptr<dyld_all_image_infos> dyldAllImageInfos(Snapshot& snapshot)
{
    task_dyld_info_data_t dyldInfo;
    mach_msg_type_number_t count = TASK_DYLD_INFO_COUNT;
    kern_return_t kr = task_info(snapshot.corpsePort(), TASK_DYLD_INFO, reinterpret_cast<task_info_t>(&dyldInfo), &count);
    if (kr != KERN_SUCCESS) {
        CORPSE_REPORT("Could not read dyld information: %s (0x%x)", mach_error_string(kr), kr);
        return { };
    }

    Address address { dyldInfo.all_image_info_addr };
    if (!address) {
        CORPSE_REPORT("dyld reports no image list");
        return { };
    }
    auto allImages = snapshot.memory().ptr<dyld_all_image_infos>(address);
    if (!allImages)
        CORPSE_REPORT("Could not read dyld_all_image_infos at 0x%llx", address.toMachVMAddress());
    return allImages;
}

Memory::Span<dyld_image_info> dyldImageInfos(Snapshot& snapshot, const Memory::Ptr<dyld_all_image_infos>& allImages)
{
    Address arrayAddress = Address { allImages->infoArray }.stripped();
    uint32_t imageCount = allImages->infoArrayCount;
    if (!arrayAddress || !imageCount) {
        CORPSE_REPORT("dyld_all_image_infos v%u at 0x%llx lists no images",
            allImages->version, allImages.address().toMachVMAddress());
        return { };
    }
    if (imageCount > maxImageCount) {
        CORPSE_REPORT("%u images is too many to be a real image list", imageCount);
        return { };
    }
    auto images = snapshot.memory().span<dyld_image_info>(arrayAddress, imageCount);
    if (!images) {
        CORPSE_REPORT("Could not read the %u image entries at 0x%llx", imageCount, arrayAddress.toMachVMAddress());
        return { };
    }
    return images;
}

// dyld_all_image_infos is dyld's debugger interface, in <mach-o/dyld_images.h>:
// https://github.com/apple-oss-distributions/dyld/blob/main/include/mach-o/dyld_images.h
// It lists UUIDs only for the images outside the shared cache, and only those are listed.
Vector<Image> Image::collect(Snapshot& snapshot)
{
    auto allImages = dyldAllImageInfos(snapshot);
    if (!allImages)
        return { };
    auto infos = dyldImageInfos(snapshot, allImages);
    if (!infos)
        return { };

    Address uuidsAddress = Address { allImages->uuidArray }.stripped();
    uint64_t uuidCount = allImages->uuidArrayCount;
    if (!uuidsAddress || !uuidCount || uuidCount > maxImageCount) {
        CORPSE_REPORT("dyld_all_image_infos v%u lists %llu image UUIDs at 0x%llx",
            allImages->version, static_cast<unsigned long long>(uuidCount), uuidsAddress.toMachVMAddress());
        return { };
    }
    Memory& memory = snapshot.memory();
    auto uuids = memory.span<dyld_uuid_info>(uuidsAddress, uuidCount);
    if (!uuids) {
        CORPSE_REPORT("Could not read the %llu image UUIDs at 0x%llx", static_cast<unsigned long long>(uuidCount), uuidsAddress.toMachVMAddress());
        return { };
    }

    HashMap<Address, Address> pathAddresses;
    for (const dyld_image_info& info : infos) {
        Address loadAddress = Address { info.imageLoadAddress }.stripped();
        if (decltype(pathAddresses)::isValidKey(loadAddress))
            pathAddresses.add(loadAddress, Address { info.imageFilePath }.stripped());
    }

    Vector<Image> result;
    result.reserveInitialCapacity(uuids.size());
    for (const dyld_uuid_info& entry : uuids) {
        Address loadAddress = Address { entry.imageLoadAddress }.stripped();
        auto pathAddress = decltype(pathAddresses)::isValidKey(loadAddress) ? pathAddresses.getOptional(loadAddress) : std::nullopt;
        auto path = pathAddress ? memory.cString(*pathAddress, maxImagePathLength) : std::nullopt;
        // dyld records the executable's path as it was given to exec, which may be
        // relative to the target's working directory. The kernel's is absolute.
        if (path && !path->span().empty() && path->span().front() != '/') {
            auto header = memory.ptr<mach_header_64>(loadAddress);
            path = std::nullopt;
            if (header && header->filetype == MH_EXECUTE) {
                if (!snapshot.executablePath().isNull())
                    path = snapshot.executablePath();
            }
        }
        if (!path) {
            CORPSE_REPORT("The image at 0x%llx of pid %d has no readable path, so it is not listed",
                loadAddress.toMachVMAddress(), static_cast<int>(snapshot.process()->pid()));
            continue;
        }
        result.append(Image { loadAddress, WTF::move(*path), toHexString(std::span<const uint8_t> { entry.imageUUID }).utf8() });
    }
    return result;
}

#else

// liblldb needs each image's path and load address. These come from /proc/<pid>/maps
// (proc_pid_maps(5)) as Breakpad's LinuxDumper takes them: consecutive mappings of one
// file are an image when the first is from offset 0, where the ELF header is, and one
// of them is executable. A data file has no ELF header; an ELF file mmap()ed only to be
// read has no executable mapping. An image is listed only while its path still names
// the mapped inode, so a file renamed over, unlinked or in another mount namespace is
// reported rather than read as the image. (Devices are not compared; btrfs reports
// stat() a different one than maps.)
Vector<Image> Image::collect(Snapshot& snapshot)
{
    int pid = static_cast<int>(snapshot.corpsePort());
    Memory& memory = snapshot.memory();
    const Vector<Region>& regions = snapshot.regions();
    Vector<Image> result;
    for (size_t index = 0; index < regions.size();) {
        const Region& first = regions[index];
        bool isExecutable = first.isExecutable();
        for (++index; index < regions.size() && regions[index].fileOffset() && regions[index].name() == first.name(); ++index)
            isExecutable |= regions[index].isExecutable();
        if (first.fileOffset() || !first.name().startsWith('/') || !isExecutable)
            continue;
        auto magic = memory.span<char>(first.base(), SELFMAG);
        if (!magic || !std::ranges::equal(magic, std::string_view { ELFMAG, SELFMAG }))
            continue;
        UTF8CString path = first.name().utf8();
        struct stat file;
        if (posixStat(path, &file) || file.st_ino != first.inode()) {
            CORPSE_REPORT("The image '%s' of pid %d is no longer the file at that path, so it is not listed", path.legacyCStringPointer(), pid);
            continue;
        }
        result.append(Image { first.base(), WTF::move(path), { } });
    }
    return result;
}

#endif // OS(DARWIN)

} // namespace Corpse
} // namespace JSC

#endif // ENABLE(LIB_JSC_TOOLS)
