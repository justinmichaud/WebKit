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
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <wtf/SafeStrerror.h>
#include <wtf/Scope.h>
#include <wtf/text/MakeString.h>
#include <wtf/text/StringToIntegerConversion.h>
#include <wtf/text/StringView.h>
#endif

namespace JSC {
namespace Corpse {

#if OS(DARWIN)

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

struct Mapping {
    Address start;
    Address end;
};

// The kernel names each entry of map_files by the range it maps, "<start>-<end>" in hex.
static std::optional<Mapping> parseMapping(StringView name)
{
    size_t dash = name.find('-');
    if (dash == notFound)
        return std::nullopt;
    auto start = parseInteger<uint64_t>(name.left(dash), 16);
    auto end = parseInteger<uint64_t>(name.substring(dash + 1), 16);
    if (!start || !end || *end <= *start)
        return std::nullopt;
    return Mapping { Address { *start }, Address { *end } };
}

// A loaded image's header mapping ends before the file does, or is followed
// by more mappings of the file: ld.so fills the gaps between a library's
// segments with them. A file mapped whole with mmap, as a debugger in the
// process does, is one mapping at least as long as the file.
static std::optional<Address> headerOfLoadedImage(const Vector<Mapping>& mappings, uint64_t fileSize)
{
    std::optional<Address> header;
    for (const Mapping& mapping : mappings) {
        bool hasPrevious = mappings.containsIf([&](const Mapping& other) {
            return other.end == mapping.start;
        });
        bool hasNext = mappings.containsIf([&](const Mapping& other) {
            return other.start == mapping.end;
        });
        bool shorterThanFile = mapping.end - mapping.start < fileSize;
        if (!hasPrevious && (hasNext || shorterThanFile) && (!header || mapping.start < *header))
            header = mapping.start;
    }
    return header;
}

// Each mapped file is held open, so that it is read without going through its path.
Vector<Image> Image::collect(Snapshot& snapshot)
{
    int pid = static_cast<int>(snapshot.process()->pid());
    ASCIICString directoryPath = makeString("/proc/"_s, pid, "/map_files"_s).ascii();
    DIR* directory = opendir(directoryPath.data());
    if (!directory) {
        CORPSE_REPORT("Could not list the mapped files of pid %d: %s", pid, safeStrerror(errno).data());
        return { };
    }
    auto closeDirectory = makeScopeExit([&] {
        closedir(directory);
    });

    struct MappedFile {
        dev_t device;
        ino_t inode;
        uint64_t size;
        Vector<Mapping> mappings;
        UnixFileDescriptor file;
    };
    Vector<MappedFile> files;

    while (dirent* entry = readdir(directory)) {
        auto mapping = parseMapping(StringView::fromLatin1(entry->d_name));
        if (!mapping)
            continue; // "." and "..".
        Diagnostics::count(DiagnosticCounter::MappedFilesListed);

        UnixFileDescriptor file { openat(dirfd(directory), entry->d_name, O_RDONLY | O_CLOEXEC), UnixFileDescriptor::Adopt };
        if (!file) {
            if (errno == EPERM) {
                CORPSE_REPORT("Could not open the mapped files of pid %d: that needs CAP_CHECKPOINT_RESTORE", pid);
                return { };
            }
            // The mapping may have gone away since the directory was read.
            Diagnostics::count(DiagnosticCounter::UnopenableMappedFiles);
            continue;
        }
        struct stat status;
        if (fstat(file.value(), &status)) {
            Diagnostics::count(DiagnosticCounter::UnopenableMappedFiles);
            continue;
        }

        size_t index = files.findIf([&](const MappedFile& mapped) {
            return mapped.device == status.st_dev && mapped.inode == status.st_ino;
        });
        if (index != notFound) {
            files[index].mappings.append(*mapping);
            continue;
        }
        if (files.size() >= maxImageCount) {
            CORPSE_REPORT("pid %d maps more than %u files, too many to be a real process", pid, maxImageCount);
            return { };
        }
        files.append({ status.st_dev, status.st_ino, static_cast<uint64_t>(status.st_size), { *mapping }, WTF::move(file) });
    }

    // Data files, like fonts and the locale archive, are mapped too; only an
    // ELF object has its header where it is mapped.
    constexpr std::array<uint8_t, 4> elfMagic { 0x7f, 'E', 'L', 'F' };
    Memory& memory = snapshot.memory();
    Vector<Image> result;
    for (MappedFile& mapped : files) {
        auto header = headerOfLoadedImage(mapped.mappings, mapped.size);
        auto magic = header ? memory.ptr<std::array<uint8_t, 4>>(*header) : Memory::Ptr<std::array<uint8_t, 4>> { };
        if (!magic || *magic != elfMagic) {
            Diagnostics::count(DiagnosticCounter::MappedFilesWithoutELFHeader);
            continue;
        }
        result.append(Image { *header, WTF::move(mapped.file) });
    }
    Diagnostics::count(DiagnosticCounter::ImagesListed, result.size());
    return result;
}

#endif // OS(DARWIN)

} // namespace Corpse
} // namespace JSC

#endif // ENABLE(MYA)
