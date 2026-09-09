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
#include "CorpseBackend.h"

#if HAVE(CORPSE_SUPPORT) && OS(LINUX)

#include "CorpseError.h"
#include "CorpseProcFile.h"
#include "CorpseProcess.h"
#include "CorpsePtrace.h"

#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <unistd.h>
#include <wtf/ASCIICType.h>
#include <wtf/FileSystem.h>
#include <wtf/HashMap.h>
#include <wtf/HashSet.h>
#include <wtf/Seconds.h>
#include <wtf/StackPointer.h>
#include <wtf/TZoneMallocInlines.h>
#include <wtf/Threading.h>
#include <wtf/text/MakeString.h>
#include <wtf/text/StringHash.h>
#include <wtf/text/StringToIntegerConversion.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC {
namespace Corpse {

namespace {

// "7f8e4c000000-7f8e4c021000 r-xp 00000000 08:01 1234  /lib/libc.so.6": the
// extent and permissions of one mapping, shared by maps and smaps.
struct MappingHeader {
    uint64_t begin { 0 };
    uint64_t end { 0 };
    bool isReadable { false };
    bool isWritable { false };
    bool isExecutable { false };
    StringView path;
};

std::optional<MappingHeader> parseMappingHeader(StringView line)
{
    auto dash = line.find('-');
    if (dash == notFound)
        return std::nullopt;
    auto space = line.find(' ', dash);
    if (space == notFound)
        return std::nullopt;

    auto begin = parseInteger<uint64_t>(line.substring(0, dash), 16);
    auto end = parseInteger<uint64_t>(line.substring(dash + 1, space - dash - 1), 16);
    if (!begin || !end || *end < *begin)
        return std::nullopt;

    MappingHeader header;
    header.begin = *begin;
    header.end = *end;

    auto perms = line.substring(space + 1, 4);
    if (perms.length() == 4) {
        header.isReadable = perms[0] == 'r';
        header.isWritable = perms[1] == 'w';
        header.isExecutable = perms[2] == 'x';
    }

    // The path is the sixth field and may itself contain spaces, so it is what
    // remains after the five that cannot.
    size_t cursor = space + 1;
    for (unsigned field = 0; field < 4; ++field) {
        cursor = line.find(' ', cursor);
        if (cursor == notFound)
            return header;
        while (cursor < line.length() && line[cursor] == ' ')
            ++cursor;
    }
    if (cursor < line.length())
        header.path = line.substring(cursor);
    return header;
}

// "Rss:  128 kB": one of the accounting lines smaps appends to a mapping.
std::optional<std::pair<StringView, uint64_t>> parseMappingField(StringView line)
{
    auto colon = line.find(':');
    if (colon == notFound)
        return std::nullopt;
    auto value = line.substring(colon + 1).trim(isASCIIWhitespace<char16_t>);
    auto space = value.find(' ');
    if (space != notFound)
        value = value.substring(0, space);
    auto number = parseInteger<uint64_t>(value);
    if (!number)
        return std::nullopt;
    return std::make_pair(line.substring(0, colon), *number);
}

// Bounds on values read out of a corpse. Each says the structure we read was
// not what we thought it was, in which case nothing in it is worth following.
constexpr uint16_t maxProgramHeaders = 256;
constexpr uint64_t maxDynamicEntries = 4096;
constexpr unsigned maxImages = 16 * 1024;

Seconds ticksToSeconds(uint64_t ticks)
{
    long ticksPerSecond = sysconf(_SC_CLK_TCK);
    return ticksPerSecond > 0 ? Seconds { static_cast<double>(ticks) / ticksPerSecond } : 0_s;
}

// A corpse on Linux: a copy-on-write fork of the target, held alive by a pipe
// the child blocks on. Forking costs a page-table copy rather than a copy of
// the memory, and the copy is frozen from the moment it exists because the only
// code the child runs is the block below.
class ForkedCorpse {
public:
    ~ForkedCorpse() { release(); }

    bool capture(pid_t reportAs)
    {
        int pipeEnds[2];
        if (pipe2(pipeEnds, O_CLOEXEC)) {
            Error::report("Could not snapshot PID %d: %s",
                static_cast<int>(reportAs), strerror(errno));
            return false;
        }

        pid_t child = fork();
        if (child < 0) {
            int forkError = errno;
            close(pipeEnds[0]);
            close(pipeEnds[1]);
            Error::report("Could not snapshot PID %d: %s",
                static_cast<int>(reportAs), strerror(forkError));
            return false;
        }

        if (!child) {
            // A fork of a multithreaded process holds every lock another thread
            // held at fork time, forever. Nothing below may allocate, log, or
            // take a lock.
            close(pipeEnds[1]);

            // A corpse must not outlive the process analysing it, even a killed one.
            prctl(PR_SET_PDEATHSIG, SIGKILL);

            char ignored;
            while (read(pipeEnds[0], &ignored, 1) < 0 && errno == EINTR) { }
            _exit(0);
        }

        close(pipeEnds[0]);
        m_keepAlive = pipeEnds[1];
        m_pid = child;
        return true;
    }

    void release()
    {
        if (m_keepAlive >= 0) {
            close(m_keepAlive);
            m_keepAlive = -1;
        }
        if (m_pid <= 0)
            return;

        // The kill is what ends the child, since a corpse taken after this one
        // holds a copy of the keep-alive end. Waiting is what keeps it from
        // staying a zombie for the life of this process.
        kill(m_pid, SIGKILL);
        int status = 0;
        while (waitpid(m_pid, &status, 0) < 0 && errno == EINTR) { }
        m_pid = -1;
    }

    pid_t pid() const { return m_pid; }

private:
    pid_t m_pid { -1 };
    int m_keepAlive { -1 };
};

class LinuxBackend final : public Backend {
    WTF_MAKE_TZONE_ALLOCATED_INLINE(LinuxBackend);
public:
    LinuxBackend(pid_t targetPid)
        : m_targetPid(targetPid)
    {
    }

    // Freezes the target and reads its threads under one stop, so that the two
    // describe the same instant.
    bool capture()
    {
        bool capturingSelf = m_targetPid == getpid();

        // The calling thread cannot be stopped: it is the one that has to run
        // to take the corpse. Its own registers it can read directly, which is
        // both cheaper and exact.
        pid_t callingThread = capturingSelf ? static_cast<pid_t>(WTF::Thread::currentID()) : 0;

        // Sampled before anything is stopped: a ptrace-stopped thread reports
        // itself as traced rather than as whatever it was doing, so a stop
        // taken first would erase the very state this describes. Only the
        // registers and the memory have to name one instant, and they do.
        auto bookkeeping = readBookkeeping();

        auto stopper = ThreadStopper::stopAllThreads(m_targetPid, callingThread);

        Vector<ThreadInfo> threads;
        threads.reserveInitialCapacity(stopper->threads().size() + (capturingSelf ? 1 : 0));
        for (const auto& stopped : stopper->threads())
            threads.append(describeThread(stopped.tid, stopped.registers, stopped.failure, bookkeeping));

        if (capturingSelf)
            threads.append(describeCallingThread(callingThread, bookkeeping));

        if (!capturingSelf) {
            // Taking a no-copy corpse of another process means making that
            // process fork, which needs a syscall injected into it. Until that
            // exists, say so plainly rather than hand back a corpse of the
            // wrong address space.
            Error::report("Could not snapshot PID %d: taking a corpse of another process"
                " is not implemented on this platform yet", static_cast<int>(m_targetPid));
            return false;
        }

        if (!m_corpse.capture(m_targetPid))
            return false;

        // The corpse now holds the memory the registers point into, so the
        // target may run again.
        stopper->release();

        m_threads = WTF::move(threads);
        reportMissingRegisters();
        return true;
    }

    bool read(Address address, std::span<uint8_t> into) const final
    {
        if (m_corpse.pid() <= 0)
            return false;
        if (into.empty())
            return true;

        // A range that runs off the end of a mapping comes back as a short read
        // rather than an error, so the count is what has to be checked.
        struct iovec local { into.data(), into.size() };
        struct iovec remote {
            reinterpret_cast<void*>(static_cast<uintptr_t>(address.value())),
            into.size()
        };
        ssize_t got = process_vm_readv(m_corpse.pid(), &local, 1, &remote, 1, 0);
        return got >= 0 && static_cast<size_t>(got) == into.size();
    }

    const Vector<ThreadInfo>& threads() const final { return m_threads; }

    // smaps is maps with each mapping's accounting appended, so one pass gives
    // both the extent of every region and how much of it is resident and dirty.
    Vector<RegionInfo> regions() const final
    {
        Vector<RegionInfo> result;
        auto contents = ProcFile::read(m_corpse.pid(), "smaps"_s);
        if (!contents)
            return result;

        auto defaultPageSizeKB = static_cast<uint64_t>(getpagesize()) / 1024;
        uint64_t pageSizeKB = defaultPageSizeKB;
        for (auto line : StringView { *contents }.split('\n')) {
            if (auto header = parseMappingHeader(line)) {
                RegionInfo region;
                region.base = Address { header->begin };
                region.size = header->end - header->begin;
                region.isReadable = header->isReadable;
                region.isWritable = header->isWritable;
                region.isExecutable = header->isExecutable;
                result.append(region);
                pageSizeKB = defaultPageSizeKB;
                continue;
            }

            if (result.isEmpty())
                continue;
            auto field = parseMappingField(line);
            if (!field)
                continue;

            auto& region = result.last();
            if (field->first == "KernelPageSize"_s && field->second)
                pageSizeKB = field->second;
            else if (!pageSizeKB)
                continue;
            else if (field->first == "Rss"_s)
                region.residentPageCount = field->second / pageSizeKB;
            else if (field->first == "Shared_Dirty"_s || field->first == "Private_Dirty"_s)
                region.dirtyPageCount = region.dirtyPageCount.value_or(0) + field->second / pageSizeKB;
        }
        return result;
    }

    // The images the target's loader has mapped.
    //
    // The loader's own list is the authority, not /proc/maps: a process can map
    // an image file for its own reasons, and a debugger reading types does
    // exactly that, so a mapping of a path says nothing about whether the
    // loader placed it or where. Asking the loader also gives each image's
    // slide outright rather than inferring one.
    //
    // /proc/maps remains the fallback for a target whose loader cannot be
    // reached, such as a static binary.
    Vector<ImageInfo> images() const final
    {
        if (auto fromLoader = imagesFromLinkMap(); !fromLoader.isEmpty())
            return fromLoader;
        return imagesFromMappings();
    }

    Vector<ImageInfo> imagesFromMappings() const
    {
        Vector<ImageInfo> result;
        auto contents = ProcFile::read(m_corpse.pid(), "maps"_s);
        if (!contents)
            return result;

        HashSet<String> seen;
        for (auto line : StringView { *contents }.split('\n')) {
            auto header = parseMappingHeader(line);
            if (!header || header->path.isEmpty())
                continue;
            // Anonymous and pseudo mappings carry no file: "[stack]", "[vdso]"
            // and the like are bracketed, and a deleted file has no image left
            // on disk to read debug info from.
            if (header->path.startsWith('['))
                continue;

            String path = header->path.toString();
            // maps lists a file's segments in ascending order, so the first
            // mapping of a path is the one its header sits in.
            if (!seen.add(path).isNewEntry)
                continue;

            ImageInfo image;
            image.path = path.utf8();
            image.loadAddress = Address { header->begin };
            image.slide = slideOfImageAt(image.loadAddress).value_or(0);
            result.append(WTF::move(image));
        }
        return result;
    }

    const char* describeRunState(int state) const final
    {
        // The letter `ps` shows, worded as the Darwin descriptions where they agree.
        switch (state) {
        case 'R': return "running";
        case 'S': return "waiting";
        case 'D': return "uninterruptible";
        case 'T': return "stopped";
        case 't': return "traced";
        case 'Z': return "zombie";
        case 'X': return "dead";
        case 'I': return "idle";
        default: return "unknown";
        }
    }

    // Walks the loader's list of images, out of the corpse.
    //
    // The route is the one every debugger takes: the auxiliary vector says
    // where the executable's program headers are, PT_DYNAMIC among them holds
    // DT_DEBUG, that points at the loader's r_debug, and r_debug heads a linked
    // list with one node per image. Every value is read out of the corpse and
    // none is trusted: a list that does not check out ends the walk and leaves
    // images() to fall back on the mappings.
    Vector<ImageInfo> imagesFromLinkMap() const
    {
        Vector<ImageInfo> result;

        auto programHeaders = auxiliaryValue(AT_PHDR);
        auto programHeaderCount = auxiliaryValue(AT_PHNUM);
        if (!programHeaders || !programHeaderCount)
            return result;
        if (*programHeaderCount > maxProgramHeaders)
            return result;

        // The executable's own PT_DYNAMIC. Its p_vaddr is a linked address, and
        // the difference between where the headers are and where they were
        // linked to be is the executable's slide.
        std::optional<uint64_t> dynamicVirtualAddress;
        std::optional<uint64_t> programHeaderVirtualAddress;
        for (uint64_t i = 0; i < *programHeaderCount; ++i) {
            Elf64_Phdr segment;
            if (!readInto(Address { *programHeaders } + i * sizeof(Elf64_Phdr), segment))
                return result;
            if (segment.p_type == PT_PHDR)
                programHeaderVirtualAddress = segment.p_vaddr;
            else if (segment.p_type == PT_DYNAMIC)
                dynamicVirtualAddress = segment.p_vaddr;
        }
        if (!dynamicVirtualAddress || !programHeaderVirtualAddress)
            return result;

        uint64_t executableSlide = *programHeaders - *programHeaderVirtualAddress;
        Address dynamic { *dynamicVirtualAddress + executableSlide };

        // DT_DEBUG is the one dynamic entry the loader writes at startup, and
        // what it writes is the address of its r_debug.
        std::optional<uint64_t> debugAddress;
        for (uint64_t i = 0; i < maxDynamicEntries; ++i) {
            Elf64_Dyn entry;
            if (!readInto(dynamic + i * sizeof(Elf64_Dyn), entry))
                return result;
            if (entry.d_tag == DT_NULL)
                break;
            if (entry.d_tag == DT_DEBUG) {
                debugAddress = entry.d_un.d_ptr;
                break;
            }
        }
        if (!debugAddress || !*debugAddress)
            return result;

        struct RDebug {
            int32_t version;
            uint32_t padding;
            uint64_t map;
        } debug;
        if (!readInto(Address { *debugAddress }, debug))
            return result;
        // Version 1 is what every glibc has published; anything else is not a
        // structure we know how to read.
        if (debug.version != 1 || !debug.map)
            return result;

        // The public prefix of the loader's link_map. Only these fields are
        // part of the published ABI, and only these are read.
        struct LinkMap {
            uint64_t addr;     // The image's slide.
            uint64_t name;     // char* to its path.
            uint64_t ld;       // Its PT_DYNAMIC.
            uint64_t next;
            uint64_t previous;
        };

        CString executablePath;
        if (auto path = ProcFile::readLink(m_targetPid, "exe"_s))
            executablePath = path->utf8();
        uint64_t cursor = debug.map;
        for (unsigned visited = 0; cursor && visited < maxImages; ++visited) {
            LinkMap node;
            if (!readInto(Address { cursor }, node))
                break;

            ImageInfo image;
            image.slide = node.addr;
            // The loader names the main executable with an empty string, and
            // anything else unnamed has no file behind it.
            CString path = node.name ? readCString(Address { node.name }) : CString();
            if (path.isNull() || !path.length())
                path = visited ? CString() : executablePath;
            if (!path.isNull() && path.length()) {
                image.path = WTF::move(path);
                // An image's header sits at its first PT_LOAD, which for every
                // image the loader maps is where the slide places address zero
                // of the link. l_ld is inside the image, so the header address
                // is derived from the segments rather than assumed.
                image.loadAddress = headerAddressOf(Address { node.ld }, node.addr);
                result.append(WTF::move(image));
            }

            if (node.next == cursor)
                break; // A list that points at itself is not one to follow.
            cursor = node.next;
        }
        return result;
    }

    // Where an image's ELF header sits, given its PT_DYNAMIC and its slide. The
    // dynamic segment names no base, so the header is found by checking the
    // candidate the slide implies.
    Address headerAddressOf(Address dynamic, uint64_t slide) const
    {
        Address candidate { slide };
        Elf64_Ehdr header;
        if (readInto(candidate, header) && !memcmp(header.e_ident, ELFMAG, SELFMAG))
            return candidate;
        UNUSED_PARAM(dynamic);
        return { };
    }

    // One value from the target's auxiliary vector, which the kernel wrote at
    // exec and which says where the executable's headers are.
    std::optional<uint64_t> auxiliaryValue(uint64_t type) const
    {
        auto contents = ProcFile::readBytes(m_targetPid, "auxv"_s);
        if (!contents)
            return std::nullopt;
        auto bytes = contents->span();
        for (size_t offset = 0; offset + 2 * sizeof(uint64_t) <= bytes.size();
            offset += 2 * sizeof(uint64_t)) {
            uint64_t entry = 0;
            uint64_t value = 0;
            memcpy(&entry, bytes.data() + offset, sizeof(entry));
            memcpy(&value, bytes.data() + offset + sizeof(uint64_t), sizeof(value));
            if (entry == AT_NULL)
                break;
            if (entry == type)
                return value;
        }
        return std::nullopt;
    }

private:
    // Everything here is read out of the corpse and none of it is trusted: an
    // image header that does not check out yields no slide rather than a wrong
    // one.
    std::optional<uint64_t> slideOfImageAt(Address base) const
    {
        Elf64_Ehdr header;
        if (!readInto(base, header))
            return std::nullopt;
        if (memcmp(header.e_ident, ELFMAG, SELFMAG))
            return std::nullopt;
        if (header.e_ident[EI_CLASS] != ELFCLASS64)
            return std::nullopt;
        if (!header.e_phoff || !header.e_phnum)
            return std::nullopt;
        if (header.e_phentsize != sizeof(Elf64_Phdr))
            return std::nullopt;

        // A program header table is small; a count that says otherwise means
        // the bytes are not a header we should be reading.
        if (header.e_phnum > maxProgramHeaders)
            return std::nullopt;

        for (uint16_t i = 0; i < header.e_phnum; ++i) {
            Elf64_Phdr segment;
            if (!readInto(base + header.e_phoff + i * sizeof(Elf64_Phdr), segment))
                return std::nullopt;
            if (segment.p_type != PT_LOAD)
                continue;
            // The first PT_LOAD is the one the image's base corresponds to, so
            // the difference between the two is how far it was slid.
            return base.value() - segment.p_vaddr;
        }
        return std::nullopt;
    }

    // What /proc says about a thread, apart from its registers. Sampled for
    // every thread at once, before any of them is stopped.
    struct Bookkeeping {
        CString name;
        int runState { 0 };
        uint64_t userTimeUsec { 0 };
        uint64_t systemTimeUsec { 0 };
    };
    using BookkeepingMap = HashMap<pid_t, Bookkeeping, WTF::IntHash<pid_t>,
        WTF::UnsignedWithZeroKeyHashTraits<pid_t>>;

    BookkeepingMap readBookkeeping() const
    {
        BookkeepingMap result;
        for (auto& entry : FileSystem::listDirectory(makeString("/proc/"_s, m_targetPid, "/task"_s))) {
            auto tid = parseInteger<pid_t>(entry);
            if (!tid)
                continue;

            Bookkeeping bookkeeping;
            if (auto name = ProcFile::read(m_targetPid, *tid, "comm"_s))
                bookkeeping.name = name->trim(isASCIIWhitespace<char16_t>).utf8();
            if (auto stat = ProcStat::forThread(m_targetPid, *tid)) {
                bookkeeping.runState = stat->character(ProcStat::stateField).value_or(0);
                bookkeeping.userTimeUsec = ticksToSeconds(
                    stat->number(ProcStat::userTimeField).value_or(0)).microsecondsAs<uint64_t>();
                bookkeeping.systemTimeUsec = ticksToSeconds(
                    stat->number(ProcStat::systemTimeField).value_or(0)).microsecondsAs<uint64_t>();
            }
            result.add(*tid, WTF::move(bookkeeping));
        }
        return result;
    }

    ThreadInfo describeThread(pid_t tid, const ThreadRegisters& registers,
        RegisterFailure failure, const BookkeepingMap& bookkeeping) const
    {
        ThreadInfo info;
        info.id = static_cast<uint64_t>(tid);
        info.registers = registers;
        info.registerFailure = failure;

        // A thread that started after the sample has no entry, and is reported
        // with its registers but no name or run state.
        auto entry = bookkeeping.find(tid);
        if (entry != bookkeeping.end()) {
            info.name = entry->value.name;
            info.runState = entry->value.runState;
            info.userTimeUsec = entry->value.userTimeUsec;
            info.systemTimeUsec = entry->value.systemTimeUsec;
        }
        return info;
    }

    // The thread taking the corpse. It is running, so its own registers are
    // exact and need no stop; reading them here is what keeps it from being the
    // one thread with no stack.
    ThreadInfo describeCallingThread(pid_t tid, const BookkeepingMap& bookkeeping) const
    {
        ThreadRegisters registers;
        registers.stackPointer = Address { currentStackPointer() };
        registers.framePointer = Address { __builtin_frame_address(0) };
        registers.programCounter = Address { __builtin_return_address(0) };
        registers.isComplete = true;
        return describeThread(tid, registers, RegisterFailure::None, bookkeeping);
    }

    // A thread with no registers has no stack, and a walk that quietly skipped
    // it would look like a walk that found nothing there.
    void reportMissingRegisters() const
    {
        unsigned denied = 0;
        unsigned unavailable = 0;
        unsigned unsupported = 0;
        for (const auto& thread : m_threads) {
            switch (thread.registerFailure) {
            case RegisterFailure::PermissionDenied: ++denied; break;
            case RegisterFailure::Unsupported: ++unsupported; break;
            case RegisterFailure::Unavailable: ++unavailable; break;
            case RegisterFailure::Translated:
            case RegisterFailure::None: break;
            }
        }

        if (denied) {
            Error::report("Could not read the registers of %u of %zu threads in pid %d:"
                " the OS refused. Check /proc/sys/kernel/yama/ptrace_scope, which must be"
                " 0 or 1 for a process to be traced by its own child.",
                denied, m_threads.size(), static_cast<int>(m_targetPid));
        }
        if (unsupported) {
            Error::report("Could not read the registers of %u of %zu threads in pid %d:"
                " this build cannot decode the target's architecture",
                unsupported, m_threads.size(), static_cast<int>(m_targetPid));
        }
        if (unavailable) {
            Error::report("Could not read the registers of %u of %zu threads in pid %d:"
                " the threads exited or the kernel reported no state for them",
                unavailable, m_threads.size(), static_cast<int>(m_targetPid));
        }
    }

    pid_t m_targetPid { -1 };
    ForkedCorpse m_corpse;
    Vector<ThreadInfo> m_threads;
};

} // anonymous namespace

std::unique_ptr<Backend> Backend::capture(Process& process)
{
    if (!process.isAttached())
        return nullptr;
    auto backend = makeUnique<LinuxBackend>(process.pid());
    if (!backend->capture())
        return nullptr;
    return backend;
}

} // namespace Corpse
} // namespace JSC

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // HAVE(CORPSE_SUPPORT) && OS(LINUX)
