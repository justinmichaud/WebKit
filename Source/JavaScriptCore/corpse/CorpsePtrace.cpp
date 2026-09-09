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
#include "CorpsePtrace.h"

#if HAVE(CORPSE_SUPPORT) && OS(LINUX)

#include "CorpseError.h"

#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/ptrace.h>
#include <sys/uio.h>
#include <sys/user.h>
#include <sys/wait.h>
#include <unistd.h>
#include <wtf/FileSystem.h>
#include <wtf/StdLibExtras.h>
#include <wtf/text/MakeString.h>
#include <wtf/text/StringToIntegerConversion.h>

// Yama's opt-in: naming a process that may trace this one. Old headers may not
// carry it, and its value is part of the kernel ABI.
#ifndef PR_SET_PTRACER
#define PR_SET_PTRACER 0x59616d61
#endif

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC {
namespace Corpse {

namespace {

// A target with more threads than this is not one we can describe, and the
// count bounds a shared mapping sized before the target is stopped.
constexpr uint32_t maxThreads = 4096;

// One thread's slot in the mapping the helper writes and the parent reads.
// Plain data with no indirection, because the helper may not allocate.
struct SharedEntry {
    pid_t tid;
    uint64_t stackPointer;
    uint64_t programCounter;
    uint64_t framePointer;
    uint8_t isComplete;
    uint8_t failure; // A RegisterFailure.
    uint8_t isSeized;
};

struct SharedBlock {
    uint32_t count;
    SharedEntry entries[];
};

size_t sharedBlockSize(uint32_t count)
{
    return sizeof(SharedBlock) + count * sizeof(SharedEntry);
}

// read and write return early when a signal arrives; a byte that sequences two
// processes has to actually be transferred.
bool writeByte(int fd)
{
    char byte = 0;
    ssize_t written;
    while ((written = write(fd, &byte, 1)) < 0 && errno == EINTR) { }
    return written == 1;
}

bool readByte(int fd)
{
    char byte = 0;
    ssize_t got;
    while ((got = read(fd, &byte, 1)) < 0 && errno == EINTR) { }
    return got == 1;
}

RegisterFailure failureForErrno(int error)
{
    // Yama's scope, a missing CAP_SYS_PTRACE and a target owned by someone else
    // all arrive as one of these, and all mean the same thing to a caller: the
    // OS would let us look, but this process is not allowed to.
    if (error == EPERM || error == EACCES)
        return RegisterFailure::PermissionDenied;
    return RegisterFailure::Unavailable;
}

// Reads one stopped thread's registers into its slot. Async-signal-safe, so it
// is usable from the helper after the fork.
void readRegistersInto(SharedEntry& entry)
{
    struct user_regs_struct regs;
    memset(&regs, 0, sizeof(regs));
    struct iovec iov { &regs, sizeof(regs) };
    if (ptrace(PTRACE_GETREGSET, entry.tid, reinterpret_cast<void*>(NT_PRSTATUS), &iov) < 0) {
        entry.failure = static_cast<uint8_t>(failureForErrno(errno));
        return;
    }

#if CPU(ARM64)
    entry.stackPointer = regs.sp;
    entry.programCounter = regs.pc;
    entry.framePointer = regs.regs[29];
    entry.isComplete = 1;
#elif CPU(X86_64)
    entry.stackPointer = regs.rsp;
    entry.programCounter = regs.rip;
    entry.framePointer = regs.rbp;
    entry.isComplete = 1;
#else
    entry.failure = static_cast<uint8_t>(RegisterFailure::Unsupported);
#endif
}

// Stops one thread and reads it. Async-signal-safe.
void stopAndRead(SharedEntry& entry)
{
    if (ptrace(PTRACE_SEIZE, entry.tid, nullptr, nullptr) < 0) {
        entry.failure = static_cast<uint8_t>(failureForErrno(errno));
        return;
    }
    entry.isSeized = 1;

    if (ptrace(PTRACE_INTERRUPT, entry.tid, nullptr, nullptr) < 0) {
        entry.failure = static_cast<uint8_t>(failureForErrno(errno));
        return;
    }

    // __WALL because the threads of a target are not children of whoever traces
    // them, and without it waitpid would ignore them.
    int status = 0;
    pid_t waited;
    while ((waited = waitpid(entry.tid, &status, __WALL)) < 0 && errno == EINTR) { }
    if (waited < 0 || !WIFSTOPPED(status)) {
        entry.failure = static_cast<uint8_t>(RegisterFailure::Unavailable);
        return;
    }

    readRegistersInto(entry);
}

void detachAll(SharedBlock& block)
{
    for (uint32_t i = 0; i < block.count; ++i) {
        if (block.entries[i].isSeized)
            ptrace(PTRACE_DETACH, block.entries[i].tid, nullptr, nullptr);
    }
}

// The helper's whole life. Runs in a fork of a multithreaded process, so every
// call below is async-signal-safe: no allocation, no logging, no locks.
[[noreturn]] void runHelper(SharedBlock& block, int goRead, int doneWrite)
{
    // A helper must never outlive the process it traces; a tracer that does
    // would leave the target stopped forever.
    prctl(PR_SET_PDEATHSIG, SIGKILL);

    // Wait until the parent has named us with PR_SET_PTRACER, or the seizes
    // below would race it and be refused.
    if (!readByte(goRead))
        _exit(1);

    for (uint32_t i = 0; i < block.count; ++i)
        stopAndRead(block.entries[i]);

    // The target is now quiescent; the parent takes its copy of the memory.
    if (!writeByte(doneWrite)) {
        detachAll(block);
        _exit(1);
    }

    // Hold the stop until the parent says it has what it needs.
    readByte(goRead);
    detachAll(block);
    _exit(0);
}

// The tids of `pid`, with `excludedTid` left out. Read before any thread is
// stopped, so this may allocate.
Vector<pid_t> threadIdsOf(pid_t pid, pid_t excludedTid)
{
    Vector<pid_t> tids;
    for (auto& entry : FileSystem::listDirectory(makeString("/proc/"_s, pid, "/task"_s))) {
        auto tid = parseInteger<pid_t>(entry);
        if (!tid || *tid == excludedTid)
            continue;
        if (tids.size() >= maxThreads) {
            Error::report("Target %d has more than %u threads; the rest are not described",
                static_cast<int>(pid), maxThreads);
            break;
        }
        tids.append(*tid);
    }
    return tids;
}

} // anonymous namespace

std::unique_ptr<ThreadStopper> ThreadStopper::stopAllThreads(pid_t pid, pid_t excludedTid)
{
    auto stopper = std::unique_ptr<ThreadStopper>(new ThreadStopper);

    Vector<pid_t> tids = threadIdsOf(pid, excludedTid);
    if (tids.isEmpty())
        return stopper;

    uint32_t count = tids.size();
    bool tracingSelf = pid == getpid();

    // The helper writes its results here. Shared and anonymous so it exists
    // before the fork and is visible to both sides afterwards, which is what
    // lets the helper report without allocating.
    void* mapping = mmap(nullptr, sharedBlockSize(count), PROT_READ | PROT_WRITE,
        MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (mapping == MAP_FAILED) {
        Error::report("Could not stop the threads of %d: %s",
            static_cast<int>(pid), strerror(errno));
        return stopper;
    }

    auto& block = *static_cast<SharedBlock*>(mapping);
    memset(mapping, 0, sharedBlockSize(count));
    block.count = count;
    for (uint32_t i = 0; i < count; ++i)
        block.entries[i].tid = tids[i];

    auto publish = [&] {
        stopper->m_threads.reserveInitialCapacity(count);
        for (uint32_t i = 0; i < count; ++i) {
            const auto& entry = block.entries[i];
            StoppedThread thread;
            thread.tid = entry.tid;
            thread.failure = static_cast<RegisterFailure>(entry.failure);
            if (entry.isComplete) {
                thread.registers.stackPointer = Address { entry.stackPointer };
                thread.registers.programCounter = Address { entry.programCounter };
                thread.registers.framePointer = Address { entry.framePointer };
                thread.registers.isComplete = true;
                thread.failure = RegisterFailure::None;
                stopper->m_holdsStop = true;
            }
            stopper->m_threads.append(WTF::move(thread));
        }
    };

    if (!tracingSelf) {
        // A different thread group can be traced from here, so no helper is
        // needed and the stop is held by this process until release().
        for (uint32_t i = 0; i < count; ++i) {
            stopAndRead(block.entries[i]);
            if (block.entries[i].isSeized)
                stopper->m_directlyTraced.append(block.entries[i].tid);
        }
        publish();
        munmap(mapping, sharedBlockSize(count));
        return stopper;
    }

    // Tracing this process means a helper, because Linux refuses to let a
    // thread trace a sibling in its own thread group.
    int goPipe[2];
    int donePipe[2];
    if (pipe2(goPipe, O_CLOEXEC)) {
        Error::report("Could not stop the threads of %d: %s",
            static_cast<int>(pid), strerror(errno));
        munmap(mapping, sharedBlockSize(count));
        return stopper;
    }
    if (pipe2(donePipe, O_CLOEXEC)) {
        Error::report("Could not stop the threads of %d: %s",
            static_cast<int>(pid), strerror(errno));
        close(goPipe[0]);
        close(goPipe[1]);
        munmap(mapping, sharedBlockSize(count));
        return stopper;
    }

    pid_t helper = fork();
    if (helper < 0) {
        Error::report("Could not stop the threads of %d: %s",
            static_cast<int>(pid), strerror(errno));
        close(goPipe[0]);
        close(goPipe[1]);
        close(donePipe[0]);
        close(donePipe[1]);
        munmap(mapping, sharedBlockSize(count));
        return stopper;
    }

    if (!helper) {
        close(goPipe[1]);
        close(donePipe[0]);
        runHelper(block, goPipe[0], donePipe[1]);
    }

    close(goPipe[0]);
    close(donePipe[1]);

    // Yama's default scope lets only a descendant trace this process, and the
    // helper is one, but it still has to be named before it may act.
    prctl(PR_SET_PTRACER, helper, 0, 0, 0);

    if (!writeByte(goPipe[1]) || !readByte(donePipe[0])) {
        Error::report("Could not stop the threads of %d: the helper did not report back",
            static_cast<int>(pid));
        close(goPipe[1]);
        close(donePipe[0]);
        kill(helper, SIGKILL);
        int status = 0;
        while (waitpid(helper, &status, 0) < 0 && errno == EINTR) { }
        prctl(PR_SET_PTRACER, 0, 0, 0, 0);
        publish();
        munmap(mapping, sharedBlockSize(count));
        return stopper;
    }

    publish();

    // The mapping is done being written, but the helper still holds the stop.
    stopper->m_helper = helper;
    stopper->m_goPipe = goPipe[1];
    stopper->m_donePipe = donePipe[0];
    munmap(mapping, sharedBlockSize(count));

    if (!stopper->m_holdsStop) {
        // Nothing was actually stopped, so there is no stop to hold; let the
        // helper go now rather than leaving it parked.
        stopper->release();
    }
    return stopper;
}

void ThreadStopper::release()
{
    for (pid_t tid : m_directlyTraced)
        ptrace(PTRACE_DETACH, tid, nullptr, nullptr);
    m_directlyTraced.clear();

    if (m_helper > 0) {
        // Letting the helper return from its second read is what detaches every
        // thread; closing the pipe would do it too, but the byte says the
        // corpse was taken rather than that we died.
        writeByte(m_goPipe);
        int status = 0;
        while (waitpid(m_helper, &status, 0) < 0 && errno == EINTR) { }
        m_helper = -1;
        prctl(PR_SET_PTRACER, 0, 0, 0, 0);
    }

    if (m_goPipe >= 0) {
        close(m_goPipe);
        m_goPipe = -1;
    }
    if (m_donePipe >= 0) {
        close(m_donePipe);
        m_donePipe = -1;
    }
    m_holdsStop = false;
}

ThreadStopper::~ThreadStopper()
{
    release();
}

} // namespace Corpse
} // namespace JSC

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // HAVE(CORPSE_SUPPORT) && OS(LINUX)
