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
#include "CorpseThread.h"

#if ENABLE(MYA)

#include "CorpseError.h"
#include "CorpseProcess.h"
#include "CorpseRegion.h"
#include "CorpseSnapshot.h"

#if OS(DARWIN)
#include <mach/mach.h>
#include <mach/mach_error.h>
#include <mach/mach_vm.h>
#include <mach/thread_act.h>
#include <mach/thread_info.h>
#include <mach/thread_status.h>
#endif
#include <optional>
#if !OS(DARWIN)
#include <charconv>
#include <dirent.h>
#include <errno.h>
#include <string_view>
#include <unistd.h>
#include <wtf/SafeStrerror.h>
#include <wtf/text/MakeString.h>
#endif

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC {
namespace Corpse {

#if OS(DARWIN)

static std::optional<Address> readStackPointer(thread_act_t thread)
{
#if CPU(ARM64)
    arm_thread_state64_t state = { };
    mach_msg_type_number_t count = ARM_THREAD_STATE64_COUNT;
    if (thread_get_state(thread, ARM_THREAD_STATE64, reinterpret_cast<thread_state_t>(&state), &count) != KERN_SUCCESS)
        return std::nullopt; // May fail e.g. for Rosetta.
    // The target's pc/lr/sp/fp arrive signed for its own ptrauth context. On an
    // arm64e build the accessors would try to authenticate them against ours and
    // trap (EXC_BAD_ACCESS / EXC_ARM_PAC_FAIL), so strip the signatures first.
    // Stripping also marks the state as unsigned, so the accessor reads it raw.
    arm_thread_state64_ptrauth_strip(state);
    return Address(arm_thread_state64_get_sp(state));
#elif CPU(X86_64)
    x86_thread_state64_t state = { };
    mach_msg_type_number_t count = x86_THREAD_STATE64_COUNT;
    if (thread_get_state(thread, x86_THREAD_STATE64, reinterpret_cast<thread_state_t>(&state), &count) != KERN_SUCCESS)
        return std::nullopt;
    return Address(state.__rsp);
#else
    UNUSED_PARAM(thread);
    return std::nullopt;
#endif
}

const char* Thread::runStateDescription() const
{
    switch (m_runState) {
    case TH_STATE_RUNNING:
        return "running";
    case TH_STATE_STOPPED:
        return "stopped";
    case TH_STATE_WAITING:
        return "waiting";
    case TH_STATE_UNINTERRUPTIBLE:
        return "uninterruptible";
    case TH_STATE_HALTED:
        return "halted";
    default:
        return "unknown";
    }
}

Vector<Thread> Thread::platformCollect(const Snapshot& snapshot)
{
    Vector<Thread> result;
    mach_port_t task = snapshot.corpsePort();
    Process* process = snapshot.process();

    thread_act_array_t threads = nullptr;
    mach_msg_type_number_t threadCount = 0;
    kern_return_t kr = task_threads(task, &threads, &threadCount);
    if (kr != KERN_SUCCESS) {
        CORPSE_REPORT("Could not read the thread list: %s (0x%x)", mach_error_string(kr), kr);
        return result;
    }
    Diagnostics::count(DiagnosticCounter::ThreadsListed, threadCount);

    result.reserveCapacity(threadCount);

    // A translated target executes as arm64 whatever its own architecture is, so its
    // threads' stack pointers belong to Rosetta's runtime rather than to the program.
    // Those addresses do land in real mappings, so reporting the region around one
    // would name a plausible but wrong stack; report no stack instead.
    bool isTranslated = process->isTranslated();
    if (isTranslated) {
        CORPSE_REPORT("Thread stacks are not available: the process runs under Rosetta"
            " translation, whose thread state does not describe the program");
    }
    for (mach_msg_type_number_t i = 0; i < threadCount; ++i) {
        Thread thread;

        thread_identifier_info_data_t identifierInfo;
        mach_msg_type_number_t count = THREAD_IDENTIFIER_INFO_COUNT;
        if (thread_info(threads[i], THREAD_IDENTIFIER_INFO, reinterpret_cast<thread_info_t>(&identifierInfo), &count) == KERN_SUCCESS)
            thread.m_id = identifierInfo.thread_id;

        thread_basic_info_data_t basicInfo;
        count = THREAD_BASIC_INFO_COUNT;
        if (thread_info(threads[i], THREAD_BASIC_INFO, reinterpret_cast<thread_info_t>(&basicInfo), &count) == KERN_SUCCESS) {
            thread.m_runState = basicInfo.run_state;
            thread.m_suspendCount = basicInfo.suspend_count;
            thread.m_userTimeUsec = static_cast<uint64_t>(basicInfo.user_time.seconds) * 1000000
                + basicInfo.user_time.microseconds;
            thread.m_systemTimeUsec = static_cast<uint64_t>(basicInfo.system_time.seconds) * 1000000
                + basicInfo.system_time.microseconds;
        }

        // Extended info is the only flavor that reports the pthread name.
        thread_extended_info_data_t extendedInfo;
        count = THREAD_EXTENDED_INFO_COUNT;
        if (thread_info(threads[i], THREAD_EXTENDED_INFO, reinterpret_cast<thread_info_t>(&extendedInfo), &count) == KERN_SUCCESS) {
            extendedInfo.pth_name[sizeof(extendedInfo.pth_name) - 1] = '\0';
            thread.m_name = extendedInfo.pth_name;
        }

        // The stack is the region the stack pointer points into.
        if (!isTranslated) {
            if (auto stackPointer = readStackPointer(threads[i])) {
                Diagnostics::count(DiagnosticCounter::ThreadStatesRead);
                thread.m_stackPointer = *stackPointer;
                if (auto region = Region::findContaining(task, thread.m_stackPointer))
                    thread.m_stackRegion = *region;
            } else
                Diagnostics::count(DiagnosticCounter::UnreadableThreadStates);
        }

        result.append(thread);
    }

    // task_threads hands us a right to each thread plus the array itself.
    for (mach_msg_type_number_t i = 0; i < threadCount; ++i)
        mach_port_deallocate(mach_task_self(), threads[i]);
    mach_vm_size_t threadsSize = threadCount * sizeof(thread_act_t);
    mach_vm_deallocate(mach_task_self(), reinterpret_cast<mach_vm_address_t>(threads), threadsSize);

    return result;
}

#else

// The state letter of /proc/<pid>/task/<tid>/stat.
const char* Thread::runStateDescription() const
{
    switch (m_runState) {
    case 'R':
        return "running";
    case 'S':
        return "sleeping";
    case 'D':
        return "uninterruptible";
    case 'T':
    case 't':
        return "stopped";
    case 'Z':
        return "zombie";
    case 'X':
        return "dead";
    case 'I':
        return "idle";
    default:
        return "unknown";
    }
}

// The fields of /proc/<pid>/task/<tid>/stat after the name, which is in
// parentheses and may hold spaces: the first is the state, and the twelfth and
// thirteenth the user and system times, in clock ticks.
static Vector<std::string_view> statFieldsAfterName(std::string_view stat)
{
    Vector<std::string_view> fields;
    size_t nameEnd = stat.rfind(')');
    if (nameEnd == std::string_view::npos)
        return fields;
    std::string_view rest = stat.substr(nameEnd + 1);
    while (!rest.empty()) {
        size_t start = rest.find_first_not_of(" \n");
        if (start == std::string_view::npos)
            break;
        rest.remove_prefix(start);
        size_t end = std::min(rest.find_first_of(" \n"), rest.size());
        fields.append(rest.substr(0, end));
        rest.remove_prefix(end);
    }
    return fields;
}

// /proc/<pid>/task/<tid>/syscall holds, for a thread blocked in the kernel,
// the syscall's number and arguments, then the stack pointer and the program
// counter; or "running".
static std::optional<Address> readStackPointer(pid_t pid, const std::string& task)
{
    auto syscall = readProcFile(pid, (task + "/syscall").c_str());
    if (!syscall || syscall->starts_with("running"))
        return std::nullopt;
    std::string_view line { *syscall };
    while (!line.empty() && (line.back() == '\n' || line.back() == ' '))
        line.remove_suffix(1);
    size_t pcStart = line.rfind(' ');
    if (pcStart == std::string_view::npos)
        return std::nullopt;
    size_t spStart = line.rfind(' ', pcStart - 1);
    std::string_view sp = line.substr(spStart == std::string_view::npos ? 0 : spStart + 1, pcStart - (spStart == std::string_view::npos ? 0 : spStart + 1));
    if (sp.starts_with("0x"))
        sp.remove_prefix(2);
    uint64_t value = 0;
    if (std::from_chars(sp.data(), sp.data() + sp.size(), value, 16).ec != std::errc { } || !value)
        return std::nullopt;
    return Address { value };
}

Vector<Thread> Thread::platformCollect(const Snapshot& snapshot)
{
    Vector<Thread> result;
    pid_t pid = snapshot.process()->pid();
    ASCIICString taskPath = makeString("/proc/"_s, pid, "/task"_s).ascii();
    DIR* directory = opendir(taskPath.data());
    if (!directory) {
        CORPSE_REPORT("Could not list the threads of pid %d: %s", static_cast<int>(pid), safeStrerror(errno).data());
        return result;
    }
    long ticksPerSecond = sysconf(_SC_CLK_TCK);
    // Read once: a JS process has a GC thread for each core.
    Vector<Region> regions = Region::allWithPageCounts(pid);
    while (struct dirent* entry = readdir(directory)) {
        uint64_t tid = 0;
        std::string_view name { entry->d_name };
        if (std::from_chars(name.data(), name.data() + name.size(), tid).ec != std::errc { } || !tid)
            continue;
        Diagnostics::count(DiagnosticCounter::ThreadsListed);
        std::string task = "task/" + std::string(name);

        Thread thread;
        thread.m_id = tid;
        if (auto comm = readProcFile(pid, (task + "/comm").c_str())) {
            if (!comm->empty() && comm->back() == '\n')
                comm->pop_back();
            thread.m_name = WTF::move(*comm);
        }
        if (auto stat = readProcFile(pid, (task + "/stat").c_str())) {
            auto fields = statFieldsAfterName(*stat);
            if (!fields.isEmpty() && !fields[0].empty())
                thread.m_runState = fields[0][0];
            uint64_t userTicks = 0;
            uint64_t systemTicks = 0;
            if (fields.size() > 12 && ticksPerSecond > 0) {
                std::from_chars(fields[11].data(), fields[11].data() + fields[11].size(), userTicks);
                std::from_chars(fields[12].data(), fields[12].data() + fields[12].size(), systemTicks);
                thread.m_userTimeUsec = userTicks * 1000000 / static_cast<uint64_t>(ticksPerSecond);
                thread.m_systemTimeUsec = systemTicks * 1000000 / static_cast<uint64_t>(ticksPerSecond);
            }
        }

        // The stack is the region the stack pointer points into. A running thread has
        // no stack pointer to read without stopping it.
        if (auto stackPointer = readStackPointer(pid, task)) {
            Diagnostics::count(DiagnosticCounter::ThreadStatesRead);
            thread.m_stackPointer = *stackPointer;
            if (auto region = Region::findContaining(regions, thread.m_stackPointer))
                thread.m_stackRegion = *region;
        }
        result.append(WTF::move(thread));
    }
    closedir(directory);
    return result;
}

#endif // OS(DARWIN)

Vector<Thread> Thread::collect(const Snapshot& snapshot)
{
    if (!snapshot.isValid()) {
        CORPSE_REPORT("Cannot read threads from an invalid snapshot");
        return { };
    }
    CORPSE_DIAGNOSTICS(diagnostics, "listing the threads of pid %d", static_cast<int>(snapshot.process()->pid()));
    Vector<Thread> threads = platformCollect(snapshot);

    if (uint64_t unreadableStates = diagnostics.value(DiagnosticCounter::UnreadableThreadStates)) {
        CORPSE_REPORT("Could not read the thread state of %llu of %llu threads:"
            " this build cannot read the target's architecture",
            static_cast<unsigned long long>(unreadableStates), static_cast<unsigned long long>(diagnostics.value(DiagnosticCounter::ThreadsListed)));
    }
    return threads;
}

} // namespace Corpse
} // namespace JSC

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(MYA)
