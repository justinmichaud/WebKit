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

#if HAVE(CORPSE_SUPPORT) && OS(DARWIN)

#include "CorpseError.h"
#include "CorpseProcess.h"

#include <mach-o/dyld_images.h>
#include <mach-o/loader.h>
#include <mach/mach.h>
#include <mach/mach_error.h>
#include <mach/mach_vm.h>
#include <mach/task_info.h>
#include <mach/thread_act.h>
#include <mach/thread_info.h>
#include <mach/thread_status.h>
#include <string.h>
#include <wtf/StdLibExtras.h>
#include <wtf/TZoneMallocInlines.h>

#if CPU(ARM64E)
#include <ptrauth.h>
#endif

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC {
namespace Corpse {

namespace {

// Counts and sizes read out of a corpse bound loops and allocations, so they
// are checked before they are used. A value past one of these says the struct
// we read was not what we thought it was, in which case nothing in it is worth
// chasing.
constexpr uint32_t maxImageCount = 16 * 1024;
constexpr uint32_t maxLoadCommands = 4 * 1024;

// A corpse on Darwin: task_generate_corpse hands back a port onto a frozen copy
// of the target's address space. The kernel takes the thread states with it, so
// unlike Linux there is nothing to stop and nothing to hold.
class DarwinBackend final : public Backend {
    WTF_MAKE_TZONE_ALLOCATED_INLINE(DarwinBackend);
public:
    DarwinBackend(pid_t targetPid, bool isTranslated)
        : m_targetPid(targetPid)
        , m_isTranslated(isTranslated)
    {
    }

    ~DarwinBackend() final
    {
        if (MACH_PORT_VALID(m_corpsePort))
            mach_port_deallocate(mach_task_self(), m_corpsePort);
    }

    bool capture(Process& process)
    {
        kern_return_t kr = task_generate_corpse(process.taskPort(), &m_corpsePort);
        if (kr != KERN_SUCCESS) {
            m_corpsePort = MACH_PORT_NULL;
            if (!process.holdsLiveTask()) {
                Error::report("Could not snapshot PID %d: the process has terminated",
                    static_cast<int>(m_targetPid));
            } else {
                Error::report("Could not snapshot PID %d: %s (0x%x)",
                    static_cast<int>(m_targetPid), mach_error_string(kr), kr);
            }
            return false;
        }

        collectThreads();
        return true;
    }

    bool read(Address address, std::span<uint8_t> into) const final
    {
        if (!MACH_PORT_VALID(m_corpsePort))
            return false;
        if (into.empty())
            return true;

        mach_vm_size_t got = 0;
        kern_return_t kr = mach_vm_read_overwrite(m_corpsePort, address.value(), into.size(),
            reinterpret_cast<mach_vm_address_t>(into.data()), &got);
        return kr == KERN_SUCCESS && got == into.size();
    }

    const Vector<ThreadInfo>& threads() const final { return m_threads; }

    Vector<RegionInfo> regions() const final
    {
        Vector<RegionInfo> result;
        if (!MACH_PORT_VALID(m_corpsePort))
            return result;

        mach_vm_address_t cursor = 0;
        while (true) {
            mach_vm_size_t size = 0;
            vm_region_submap_info_data_64_t info;
            natural_t depth = 0;
            mach_msg_type_number_t infoCount = VM_REGION_SUBMAP_INFO_COUNT_64;
            kern_return_t kr = mach_vm_region_recurse(m_corpsePort, &cursor, &size, &depth,
                reinterpret_cast<vm_region_recurse_info_t>(&info), &infoCount);
            if (kr != KERN_SUCCESS || !size)
                break;

            if (info.is_submap) {
                // Descend into the submap rather than past it, so its mappings
                // are reported individually.
                ++depth;
                continue;
            }

            RegionInfo region;
            region.base = Address { cursor };
            region.size = size;
            region.residentPageCount = info.pages_resident;
            region.dirtyPageCount = info.pages_dirtied;
            region.isReadable = info.protection & VM_PROT_READ;
            region.isWritable = info.protection & VM_PROT_WRITE;
            region.isExecutable = info.protection & VM_PROT_EXECUTE;
            result.append(region);

            // Guard against a kernel answer that would not advance the walk.
            mach_vm_address_t next = cursor + size;
            if (next <= cursor)
                break;
            cursor = next;
        }
        return result;
    }

    // dyld's image list, read out of the corpse rather than from this process,
    // so it describes the target even when the target is not us.
    Vector<ImageInfo> images() const final
    {
        Vector<ImageInfo> result;
        if (!MACH_PORT_VALID(m_corpsePort))
            return result;

        task_dyld_info_data_t dyldInfo;
        mach_msg_type_number_t count = TASK_DYLD_INFO_COUNT;
        if (task_info(m_corpsePort, TASK_DYLD_INFO,
            reinterpret_cast<task_info_t>(&dyldInfo), &count) != KERN_SUCCESS) {
            return result;
        }
        if (!dyldInfo.all_image_info_addr)
            return result;

        dyld_all_image_infos allImageInfos;
        if (!readInto(Address { dyldInfo.all_image_info_addr }, allImageInfos))
            return result;
        if (!allImageInfos.infoArray || !allImageInfos.infoArrayCount)
            return result;
        if (allImageInfos.infoArrayCount > maxImageCount) {
            Error::report("Ignoring the image list of pid %d: dyld reports %u images",
                static_cast<int>(m_targetPid), allImageInfos.infoArrayCount);
            return result;
        }

        Address arrayBase = Address { reinterpret_cast<uint64_t>(allImageInfos.infoArray) }.stripped();
        result.reserveInitialCapacity(allImageInfos.infoArrayCount);
        for (uint32_t i = 0; i < allImageInfos.infoArrayCount; ++i) {
            dyld_image_info entry;
            if (!readInto(arrayBase + i * sizeof(dyld_image_info), entry))
                continue;
            if (!entry.imageLoadAddress || !entry.imageFilePath)
                continue;

            ImageInfo image;
            image.loadAddress = Address { reinterpret_cast<uint64_t>(entry.imageLoadAddress) }.stripped();
            auto path = readCString(Address { reinterpret_cast<uint64_t>(entry.imageFilePath) }.stripped());
            if (path.isNull())
                continue;
            image.path = WTF::move(path);
            image.slide = slideOfImageAt(image.loadAddress).value_or(0);
            result.append(WTF::move(image));
        }
        return result;
    }

    const char* describeRunState(int state) const final
    {
        switch (state) {
        case TH_STATE_RUNNING: return "running";
        case TH_STATE_STOPPED: return "stopped";
        case TH_STATE_WAITING: return "waiting";
        case TH_STATE_UNINTERRUPTIBLE: return "uninterruptible";
        case TH_STATE_HALTED: return "halted";
        default: return "unknown";
        }
    }

private:
    // How far an image was slid: where its header sits, less the address its
    // __TEXT segment was linked at. Everything is read out of the corpse and
    // none of it is trusted.
    std::optional<uint64_t> slideOfImageAt(Address base) const
    {
        mach_header_64 header;
        if (!readInto(base, header))
            return std::nullopt;
        if (header.magic != MH_MAGIC_64)
            return std::nullopt;
        if (!header.ncmds || header.ncmds > maxLoadCommands)
            return std::nullopt;

        Address cursor = base + sizeof(mach_header_64);
        for (uint32_t i = 0; i < header.ncmds; ++i) {
            load_command command;
            if (!readInto(cursor, command))
                return std::nullopt;
            if (command.cmdsize < sizeof(load_command))
                return std::nullopt;

            if (command.cmd == LC_SEGMENT_64) {
                segment_command_64 segment;
                if (!readInto(cursor, segment))
                    return std::nullopt;
                if (!strncmp(segment.segname, SEG_TEXT, sizeof(segment.segname)))
                    return base.value() - segment.vmaddr;
            }
            cursor = cursor + command.cmdsize;
        }
        return std::nullopt;
    }

    static std::optional<ThreadRegisters> readRegisters(thread_act_t thread)
    {
#if CPU(ARM64)
        arm_thread_state64_t state = { };
        mach_msg_type_number_t count = ARM_THREAD_STATE64_COUNT;
        if (thread_get_state(thread, ARM_THREAD_STATE64,
            reinterpret_cast<thread_state_t>(&state), &count) != KERN_SUCCESS) {
            return std::nullopt;
        }
        // The target's pc/lr/sp/fp arrive signed for its own ptrauth context. On
        // an arm64e build the accessors would try to authenticate them against
        // ours and trap (EXC_BAD_ACCESS / EXC_ARM_PAC_FAIL), so strip the
        // signatures first. Stripping also marks the state as unsigned, so the
        // accessors read it raw.
        arm_thread_state64_ptrauth_strip(state);

        ThreadRegisters registers;
        registers.stackPointer = Address { arm_thread_state64_get_sp(state) };
        registers.programCounter = Address { arm_thread_state64_get_pc(state) };
        registers.framePointer = Address { arm_thread_state64_get_fp(state) };
        registers.isComplete = true;
        return registers;
#elif CPU(X86_64)
        x86_thread_state64_t state = { };
        mach_msg_type_number_t count = x86_THREAD_STATE64_COUNT;
        if (thread_get_state(thread, x86_THREAD_STATE64,
            reinterpret_cast<thread_state_t>(&state), &count) != KERN_SUCCESS) {
            return std::nullopt;
        }

        ThreadRegisters registers;
        registers.stackPointer = Address { state.__rsp };
        registers.programCounter = Address { state.__rip };
        registers.framePointer = Address { state.__rbp };
        registers.isComplete = true;
        return registers;
#else
        UNUSED_PARAM(thread);
        return std::nullopt;
#endif
    }

    void collectThreads()
    {
        thread_act_array_t threads = nullptr;
        mach_msg_type_number_t threadCount = 0;
        kern_return_t kr = task_threads(m_corpsePort, &threads, &threadCount);
        if (kr != KERN_SUCCESS) {
            Error::report("Could not read the thread list for pid %d: %s (0x%x)",
                static_cast<int>(m_targetPid), mach_error_string(kr), kr);
            return;
        }

        // A translated target executes as arm64 whatever its own architecture
        // is, so its threads' stack pointers belong to Rosetta's runtime rather
        // than to the program. Those addresses land in real mappings, so
        // reporting one would name a plausible but wrong stack.
        if (m_isTranslated) {
            Error::report("Thread stacks for pid %d are not available: the process runs"
                " under Rosetta translation, whose thread state does not describe the program",
                static_cast<int>(m_targetPid));
        }

        m_threads.reserveInitialCapacity(threadCount);
        for (mach_msg_type_number_t i = 0; i < threadCount; ++i) {
            ThreadInfo info;

            thread_identifier_info_data_t identifierInfo;
            mach_msg_type_number_t count = THREAD_IDENTIFIER_INFO_COUNT;
            if (thread_info(threads[i], THREAD_IDENTIFIER_INFO,
                reinterpret_cast<thread_info_t>(&identifierInfo), &count) == KERN_SUCCESS) {
                info.id = identifierInfo.thread_id;
            }

            thread_basic_info_data_t basicInfo;
            count = THREAD_BASIC_INFO_COUNT;
            if (thread_info(threads[i], THREAD_BASIC_INFO,
                reinterpret_cast<thread_info_t>(&basicInfo), &count) == KERN_SUCCESS) {
                info.runState = basicInfo.run_state;
                info.suspendCount = basicInfo.suspend_count;
                info.userTimeUsec = static_cast<uint64_t>(basicInfo.user_time.seconds) * 1000000
                    + basicInfo.user_time.microseconds;
                info.systemTimeUsec = static_cast<uint64_t>(basicInfo.system_time.seconds) * 1000000
                    + basicInfo.system_time.microseconds;
            }

            // Extended info is the only flavor that reports the pthread name.
            thread_extended_info_data_t extendedInfo;
            count = THREAD_EXTENDED_INFO_COUNT;
            if (thread_info(threads[i], THREAD_EXTENDED_INFO,
                reinterpret_cast<thread_info_t>(&extendedInfo), &count) == KERN_SUCCESS) {
                extendedInfo.pth_name[sizeof(extendedInfo.pth_name) - 1] = '\0';
                info.name = CString(extendedInfo.pth_name);
            }

            if (m_isTranslated)
                info.registerFailure = RegisterFailure::Translated;
            else if (auto registers = readRegisters(threads[i]))
                info.registers = *registers;
            else
                info.registerFailure = RegisterFailure::Unsupported;

            m_threads.append(WTF::move(info));
        }

        unsigned unsupported = 0;
        for (const auto& thread : m_threads) {
            if (thread.registerFailure == RegisterFailure::Unsupported)
                ++unsupported;
        }
        if (unsupported) {
            Error::report("Could not read the thread state of %u of %u threads in pid %d:"
                " this build cannot read the target's architecture",
                unsupported, static_cast<unsigned>(threadCount), static_cast<int>(m_targetPid));
        }

        // task_threads hands us a right to each thread plus the array itself.
        for (mach_msg_type_number_t i = 0; i < threadCount; ++i)
            mach_port_deallocate(mach_task_self(), threads[i]);
        mach_vm_deallocate(mach_task_self(), reinterpret_cast<mach_vm_address_t>(threads),
            threadCount * sizeof(thread_act_t));
    }

    pid_t m_targetPid { -1 };
    bool m_isTranslated { false };
    mach_port_t m_corpsePort { MACH_PORT_NULL };
    Vector<ThreadInfo> m_threads;
};

} // anonymous namespace

std::unique_ptr<Backend> Backend::capture(Process& process)
{
    if (!process.isAttached())
        return nullptr;
    auto backend = makeUnique<DarwinBackend>(process.pid(), process.isTranslated());
    if (!backend->capture(process))
        return nullptr;
    return backend;
}

} // namespace Corpse
} // namespace JSC

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // HAVE(CORPSE_SUPPORT) && OS(DARWIN)
