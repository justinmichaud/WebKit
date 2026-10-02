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
#include <charconv>
#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <link.h>
#include <memory>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <string_view>
#include <unistd.h>
#include <wtf/SafeStrerror.h>
#include <wtf/StdLibExtras.h>
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

// Where each file is mapped at offset 0, which is where its ELF header is,
// from /proc/<pid>/maps.
struct FileStart {
    uint64_t address;
    std::string path;
    // The kernel marks a mapping " (deleted)" when its file was unlinked, as a
    // linker, install or a package manager does when it writes a new file at the path.
    bool replaced { false };
};

static constexpr std::string_view deletedSuffix = " (deleted)";

static Vector<FileStart> fileStarts(int pid)
{
    Vector<FileStart> result;
    auto maps = readProcFile(pid, "maps");
    if (!maps) {
        CORPSE_REPORT("Could not read the memory map of pid %d", pid);
        return result;
    }
    // "start-end perms offset dev inode path", with the path, if any, last.
    std::string_view rest { *maps };
    while (!rest.empty()) {
        size_t lineEnd = std::min(rest.find('\n'), rest.size());
        std::string_view line = rest.substr(0, lineEnd);
        rest.remove_prefix(std::min(lineEnd + 1, rest.size()));
        Vector<std::string_view, 6> fields;
        while (fields.size() < 5 && !line.empty()) {
            size_t start = line.find_first_not_of(' ');
            if (start == std::string_view::npos)
                break;
            line.remove_prefix(start);
            size_t end = std::min(line.find(' '), line.size());
            fields.append(line.substr(0, end));
            line.remove_prefix(end);
        }
        size_t pathStart = line.find_first_not_of(' ');
        if (fields.size() < 5 || pathStart == std::string_view::npos || line[pathStart] != '/')
            continue;
        uint64_t start = 0;
        uint64_t offset = 0;
        std::string_view range = fields[0];
        if (std::from_chars(range.data(), range.data() + range.size(), start, 16).ec != std::errc { }
            || std::from_chars(fields[2].data(), fields[2].data() + fields[2].size(), offset, 16).ec != std::errc { } || offset)
            continue;
        std::string_view path = line.substr(pathStart);
        bool replaced = path.ends_with(deletedSuffix);
        if (replaced)
            path.remove_suffix(deletedSuffix.size());
        result.append({ start, std::string { path }, replaced });
    }
    return result;
}

// The NT_GNU_BUILD_ID note in the PT_NOTE segments of the image whose ELF
// header is at `header` and which the loader slid by `slide`.
static std::optional<Vector<uint8_t>> buildIDOf(Memory& memory, Address header, uint64_t slide)
{
    auto elfHeader = memory.ptr<Elf64_Ehdr>(header);
    if (!elfHeader || memcmp(elfHeader->e_ident, ELFMAG, SELFMAG) || !elfHeader->e_phnum || elfHeader->e_phnum > maxProgramHeaders)
        return std::nullopt;
    auto programHeaders = memory.span<Elf64_Phdr>(header + elfHeader->e_phoff, elfHeader->e_phnum);
    if (!programHeaders)
        return std::nullopt;
    for (const Elf64_Phdr& programHeader : programHeaders) {
        if (programHeader.p_type != PT_NOTE || !programHeader.p_filesz || programHeader.p_filesz > maxNoteSegmentSize)
            continue;
        auto notes = memory.span<uint8_t>(Address { slide + programHeader.p_vaddr }, programHeader.p_filesz);
        if (!notes)
            continue;
        std::span<const uint8_t> rest { notes };
        size_t alignment = programHeader.p_align == 8 ? 8 : 4;
        while (rest.size() >= sizeof(Elf64_Nhdr)) {
            Elf64_Nhdr note;
            memcpySpan(asMutableByteSpan(note), rest.first(sizeof(note)));
            size_t nameSize = roundUpToMultipleOf(alignment, note.n_namesz);
            size_t descriptorSize = roundUpToMultipleOf(alignment, note.n_descsz);
            if (nameSize + descriptorSize > rest.size() - sizeof(note))
                break;
            std::span<const uint8_t> name = rest.subspan(sizeof(note), note.n_namesz);
            std::span<const uint8_t> descriptor = rest.subspan(sizeof(note) + nameSize, note.n_descsz);
            constexpr std::array<uint8_t, 4> gnu { 'G', 'N', 'U', '\0' };
            if (note.n_type == NT_GNU_BUILD_ID && std::ranges::equal(name, std::span { gnu }) && !descriptor.empty())
                return Vector<uint8_t> { descriptor };
            rest = rest.subspan(sizeof(note) + nameSize + descriptorSize);
        }
    }
    return std::nullopt;
}

// ld.so lists every image it loaded in r_debug's link_map chain, as dyld does
// in dyld_all_image_infos. The pid is the copy's, whose memory, mappings and
// auxiliary vector are the target's.
Vector<Image> Image::collect(Snapshot& snapshot)
{
    int pid = static_cast<int>(snapshot.corpsePort());
    Memory& memory = snapshot.memory();
    auto debugStateAddress = loaderDebugState(memory, pid);
    if (!debugStateAddress)
        return { };
    auto debugState = memory.ptr<r_debug>(*debugStateAddress);
    if (!debugState) {
        CORPSE_REPORT("Could not read r_debug at 0x%llx", debugStateAddress->toTargetVMAddress());
        return { };
    }

    Vector<FileStart> starts = fileStarts(pid);
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
        // Only the executable's entry has no name. /proc/<pid>/exe marks a
        // replaced executable as maps does.
        if (path && !path->length()) {
            path = executablePathOf(pid);
            std::string_view exe { path->length() ? path->legacyCStringPointer() : "" };
            if (exe.ends_with(deletedSuffix))
                path = UTF8CString(byteCast<char8_t>(std::span { exe.data(), exe.size() - deletedSuffix.size() }));
        }
        if (!path || !path->length()) {
            Diagnostics::count(DiagnosticCounter::ImagesWithoutPath);
            continue;
        }

        // The file's lowest mapping at offset 0 at or above its slide is the
        // loader's; another, such as one liblldb maps in this process, is lower.
        std::unique_ptr<char, decltype(&free)> canonicalPath { realpath(path->legacyCStringPointer(), nullptr), &free };
        const FileStart* fileStart = nullptr;
        for (const FileStart& start : starts) {
            if (canonicalPath && start.path == canonicalPath.get() && start.address >= entry->l_addr && (!fileStart || start.address < fileStart->address))
                fileStart = &start;
        }
        if (fileStart && fileStart->replaced) {
            // The file at the path is another, which liblldb would read as this image.
            CORPSE_REPORT("The file of the image '%s' of pid %d was replaced after it was loaded", path->legacyCStringPointer(), pid);
            continue;
        }
        std::optional<uint64_t> header;
        if (fileStart)
            header = fileStart->address;
        if (!header) {
            // The vDSO, which no file holds.
            Diagnostics::count(DiagnosticCounter::ImagesWithoutFile);
            continue;
        }
        auto buildID = buildIDOf(memory, Address { *header }, entry->l_addr);
        if (!buildID) {
            CORPSE_REPORT("The image '%s' of pid %d has no build-id, so a rebuilt file could not be told from it", path->legacyCStringPointer(), pid);
            continue;
        }
        result.append(Image { Address { entry->l_addr }, WTF::move(*path), WTF::move(*buildID) });
    }
    Diagnostics::count(DiagnosticCounter::ImagesListed, listed);
    return result;
}

#endif // OS(DARWIN)

} // namespace Corpse
} // namespace JSC

#endif // ENABLE(MYA)
