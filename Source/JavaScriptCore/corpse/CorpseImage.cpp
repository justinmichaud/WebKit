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
#include "CorpseProcess.h"
#include "CorpseSnapshot.h"

#if OS(DARWIN)
#include <mach-o/dyld_images.h>
#include <mach/mach.h>
#include <mach/mach_error.h>
#include <mach/task_info.h>
#else
#include <array>
#include <errno.h>
#include <fcntl.h>
#include <string_view>
#include <unistd.h>
#include <wtf/StdLibExtras.h>
#include <wtf/text/MakeString.h>
#include <wtf/text/StringToIntegerConversion.h>
#include <wtf/text/StringView.h>
#endif

namespace JSC {
namespace Corpse {

// A path read out of a corpse is bounded like every other value in it.
static constexpr size_t maxPathLength = 4 * KB;

#if OS(DARWIN)

// A process that dlopens every framework on the system reaches about 2,800
// images. Each one below costs reads out of the corpse, so an implausible count
// says the struct read was not dyld_all_image_infos.
static constexpr uint32_t maxImageCount = 16 * 1024;

Vector<Image> Image::collect(const Snapshot& snapshot)
{
    Vector<Image> result;

    if (!snapshot.isValid()) {
        CORPSE_REPORT("Cannot read images from an invalid snapshot");
        return result;
    }
    int pid = static_cast<int>(snapshot.process()->pid());

    task_dyld_info_data_t dyldInfo;
    mach_msg_type_number_t count = TASK_DYLD_INFO_COUNT;
    kern_return_t kr = task_info(snapshot.corpsePort(), TASK_DYLD_INFO, reinterpret_cast<task_info_t>(&dyldInfo), &count);
    if (kr != KERN_SUCCESS) {
        CORPSE_REPORT("Could not read dyld information for pid %d: %s (0x%x)", pid, reportableString(mach_error_string(kr)), kr);
        return result;
    }

    Address allImageInfosAddress { dyldInfo.all_image_info_addr };
    if (!allImageInfosAddress) {
        CORPSE_REPORT("dyld reports no image list for pid %d", pid);
        return result;
    }
    auto allImages = snapshot.read<dyld_all_image_infos>(allImageInfosAddress);
    if (!allImages) {
        CORPSE_REPORT("Could not read dyld_all_image_infos at 0x%llx for pid %d",
            allImageInfosAddress.toTargetVMAddress(), pid);
        return result;
    }

    Address arrayAddress = Address { allImages->infoArray }.stripped();
    uint32_t imageCount = allImages->infoArrayCount;
    if (!arrayAddress || !imageCount) {
        CORPSE_REPORT("dyld lists no images for pid %d", pid);
        return result;
    }
    if (imageCount > maxImageCount) {
        CORPSE_REPORT("dyld lists %u images for pid %d, too many to be a real image list", imageCount, pid);
        return result;
    }

    result.reserveCapacity(imageCount);
    for (uint32_t i = 0; i < imageCount; ++i) {
        Address infoAddress = arrayAddress + static_cast<uint64_t>(i) * sizeof(dyld_image_info);
        auto info = snapshot.read<dyld_image_info>(infoAddress);
        if (!info) {
            CORPSE_REPORT("Could not read image %u of %u at 0x%llx for pid %d",
                i, imageCount, infoAddress.toTargetVMAddress(), pid);
            return { };
        }
        Image image;
        image.m_loadAddress = Address { info->imageLoadAddress }.stripped();
        auto path = snapshot.readCString(Address { info->imageFilePath }.stripped(), maxPathLength);
        if (!path) {
            CORPSE_REPORT("Could not read the path of the image at 0x%llx for pid %d",
                image.m_loadAddress.toTargetVMAddress(), pid);
            return { };
        }
        image.m_path = WTF::move(*path);
        result.append(WTF::move(image));
    }
    return result;
}

#else

// CLAUDE: This seems too low-level, can we make this parsing more clear?

// The lowest mapping of each file at file offset zero is where its header
// landed; that is what /proc/<pid>/maps has in place of a loader's image list.
Vector<Image> Image::collect(const Snapshot& snapshot)
{
    Vector<Image> result;

    if (!snapshot.isValid()) {
        CORPSE_REPORT("Cannot read images from an invalid snapshot");
        return result;
    }
    int pid = static_cast<int>(snapshot.process()->pid());

    // procfs reports the file as empty, so it has to be read to its end.
    CString mapsPath = makeString("/proc/"_s, pid, "/maps"_s).utf8();
    int fd = open(mapsPath.data(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        CORPSE_REPORT("Could not open the memory map of pid %d", pid);
        return result;
    }
    Vector<uint8_t> maps;
    std::array<uint8_t, 16 * KB> chunk;
    while (true) {
        ssize_t got = read(fd, chunk.data(), chunk.size());
        if (got < 0 && errno == EINTR)
            continue;
        if (got <= 0) {
            if (got < 0)
                maps.clear();
            break;
        }
        maps.append(std::span { chunk }.first(static_cast<size_t>(got)));
    }
    close(fd);
    if (maps.isEmpty()) {
        CORPSE_REPORT("Could not read the memory map of pid %d", pid);
        return result;
    }

    std::string_view text { byteCast<char>(maps.span()) };
    while (!text.empty()) {
        size_t newline = text.find('\n');
        std::string_view line = text.substr(0, newline);
        text = newline == std::string_view::npos ? std::string_view { } : text.substr(newline + 1);

        // start-end perms offset dev inode path
        size_t dash = line.find('-');
        size_t space = line.find(' ');
        if (dash == std::string_view::npos || space == std::string_view::npos || dash > space)
            continue;
        auto start = parseInteger<uint64_t>(StringView { std::span<const char> { line.substr(0, dash) } }, 16);
        if (!start)
            continue;

        std::string_view rest = line.substr(space + 1);
        std::string_view fields[4];
        for (auto& field : fields) {
            size_t end = rest.find(' ');
            field = rest.substr(0, end);
            rest = end == std::string_view::npos ? std::string_view { } : rest.substr(end + 1);
        }
        if (fields[1] != "00000000")
            continue;
        size_t pathStart = rest.find('/');
        if (pathStart == std::string_view::npos)
            continue;
        std::string_view path = rest.substr(pathStart);
        if (path.size() > maxPathLength)
            continue;

        bool seen = false;
        for (auto& image : result) {
            if (std::string_view { image.m_path.span() } != path)
                continue;
            seen = true;
            if (Address { *start } < image.m_loadAddress)
                image.m_loadAddress = Address { *start };
        }
        if (seen)
            continue;
        Image image;
        image.m_path = UTF8CString(byteCast<char8_t>(std::span { path }));
        image.m_loadAddress = Address { *start };
        result.append(WTF::move(image));
    }
    return result;
}

#endif // OS(DARWIN)

} // namespace Corpse
} // namespace JSC

#endif // ENABLE(MYA)
