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
#include "LibJSCToolsTestUtilities.h"

#include <wtf/MonotonicTime.h>
#include <wtf/StdLibExtras.h>

#if ENABLE(MYA)
#include <JavaScriptCore/CorpseError.h>
#include <JavaScriptCore/CorpseProcess.h>
#include <JavaScriptCore/CorpseSnapshot.h>
#include <JavaScriptCore/InitializeThreading.h>
#include <JavaScriptCore/Options.h>
#if OS(DARWIN)
#include <mach/mach.h>
#include <mach/mach_vm.h>
#endif
#include <array>
#include <dlfcn.h>
#include <optional>
#include <utility>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <stdlib.h>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <wtf/PtrTag.h>
#include <wtf/SafeStrerror.h>
#include <wtf/Scope.h>
#include <wtf/text/CString.h>
#include <wtf/text/MakeString.h>
#include <wtf/text/StringToIntegerConversion.h>

extern char** environ;
#endif

namespace JSCToolsTest {

unsigned assertionsRun = 0;
unsigned assertionsFailed = 0;
unsigned suitesSkipped = 0;
const char* suiteFilter = nullptr;
bool verbose = false;

static Seconds s_totalSuiteTime;

static thread_local unsigned s_expectedReports = 0;

static unsigned reportCount()
{
#if ENABLE(MYA)
    return JSC::Corpse::Error::reportCount();
#else
    return 0;
#endif
}

SuiteTracer::SuiteTracer(const char* name)
    : m_name(name)
    , m_shouldRun(!suiteFilter || std::string_view(name).contains(std::string_view(suiteFilter)))
{
    if (!m_shouldRun)
        return;
    dataLogLn("--- ", m_name);
    m_start = MonotonicTime::now();
    m_reportsAtStart = reportCount();
    s_expectedReports = 0;
}

SuiteTracer::~SuiteTracer()
{
    if (!m_shouldRun)
        return;

    unsigned reported = reportCount() - m_reportsAtStart;
    TEST_ASSERT_EQ(reported, s_expectedReports, "the library reported only the errors the suite asked for");

    Seconds elapsed = MonotonicTime::now() - m_start;
    s_totalSuiteTime += elapsed;
    if (!verbose)
        return;

    uint64_t microseconds = static_cast<uint64_t>(elapsed.microseconds());
    uint64_t fraction = microseconds % 1000;
    dataLogLn("    ran for ", microseconds / 1000, ".",
        fraction < 100 ? "0" : "", fraction < 10 ? "0" : "", fraction, " ms");
}

Seconds totalSuiteTime()
{
    return s_totalSuiteTime;
}

ExpectedErrors::ExpectedErrors(unsigned count)
    : m_count(count)
    , m_reportsAtStart(reportCount())
{
    dataLogLn("    (the next ", count, count == 1 ? " error is one" : " errors are ones", " this test asks for)");
}

ExpectedErrors::~ExpectedErrors()
{
    unsigned reported = reportCount() - m_reportsAtStart;
    TEST_ASSERT_EQ(reported, m_count, "the library reported exactly the errors this test asked for");
    s_expectedReports += reported;
}

void skipSuite(const char* name, const char* why)
{
    ++suitesSkipped;
    dataLogLn("SKIP: ", name, ": ", why);
}

bool linuxSkip(const char* name, const char* why)
{
#if OS(LINUX)
    // FIXME: Implement the corpse functionality these suites need on Linux.
    skipSuite(name, why);
    return true;
#else
    UNUSED_PARAM(name);
    UNUSED_PARAM(why);
    return false;
#endif
}

#if ENABLE(MYA) && OS(DARWIN)

unsigned machPortNameCount()
{
    mach_port_name_array_t names = nullptr;
    mach_msg_type_number_t nameCount = 0;
    mach_port_type_array_t types = nullptr;
    mach_msg_type_number_t typeCount = 0;
    auto result = mach_port_names(mach_task_self(), &names, &nameCount, &types, &typeCount);
    RELEASE_ASSERT(result == KERN_SUCCESS);

    mach_vm_deallocate(mach_task_self(), reinterpret_cast<mach_vm_address_t>(names), nameCount * sizeof(mach_port_name_t));
    mach_vm_deallocate(mach_task_self(), reinterpret_cast<mach_vm_address_t>(types), typeCount * sizeof(mach_port_type_t));
    return nameCount;
}

unsigned machPortSendRightCount(mach_port_t port)
{
    mach_port_urefs_t refs = 0;
    auto result = mach_port_get_refs(mach_task_self(), port, MACH_PORT_RIGHT_SEND, &refs);
    if (result == KERN_INVALID_NAME) {
        // A name this task does not hold is an answer -- it holds no rights under it --
        // rather than a failure. Hence, has no send right.
        return 0;
    }
    RELEASE_ASSERT(result == KERN_SUCCESS);
    return refs; // Can still be 0 (which still means no send right).
}

#endif // ENABLE(MYA) && OS(DARWIN)

#if ENABLE(MYA)

SelfSnapshot::SelfSnapshot()
{
    m_process = JSC::Corpse::Process::create(getpid());
    if (!m_process->attach()) {
        TEST_ASSERT(false, "attaching to this process succeeds");
        return;
    }
    m_snapshot = WTF::makeUnique<JSC::Corpse::Snapshot>(m_process);
    if (!m_snapshot->isValid())
        TEST_ASSERT(false, "a snapshot of this process is valid");
}

SelfSnapshot::~SelfSnapshot() = default;

bool SelfSnapshot::isValid() const
{
    return m_snapshot && m_snapshot->isValid();
}

JSC::Corpse::Snapshot& SelfSnapshot::snapshot() const
{
    return *m_snapshot;
}

RefPtr<JSC::Corpse::Process> SelfSnapshot::process() const
{
    return m_process;
}

// One control block for all parked threads, so that they can be told to stop
// together. Only one ParkedThreads is expected to be alive at a time.
static pthread_mutex_t parkMutex = PTHREAD_MUTEX_INITIALIZER;
static unsigned parkedCount = 0;
static bool parkStopping = false;

struct ParkedThreads::Thread {
    pthread_t handle { };
    std::string name;
};

static void* parkThread(void* argument)
{
    auto* thread = static_cast<ParkedThreads::Thread*>(argument);
#if OS(DARWIN)
    pthread_setname_np(thread->name.c_str());
#else
    pthread_setname_np(pthread_self(), thread->name.c_str());
#endif

    pthread_mutex_lock(&parkMutex);
    ++parkedCount;
    while (!parkStopping) {
        pthread_mutex_unlock(&parkMutex);
        usleep(1000);
        pthread_mutex_lock(&parkMutex);
    }
    pthread_mutex_unlock(&parkMutex);
    return nullptr;
}

ParkedThreads::~ParkedThreads()
{
    stopAndJoin();
}

bool ParkedThreads::spawn(const char* name)
{
    auto* thread = new Thread;
    // pthread cuts a name that does not fit, and so must this copy, so that the
    // name asked for here is the name a corpse will report.
    thread->name = std::string_view(name).substr(0, maximumNameLength);
    if (pthread_create(&thread->handle, nullptr, parkThread, thread)) {
        delete thread;
        return false;
    }
    m_threads.append(thread);
    return true;
}

bool ParkedThreads::waitUntilAllParked()
{
    // Bounded so that a thread that never starts fails the test rather than
    // hanging it.
    for (unsigned attempt = 0; attempt < 5000; ++attempt) {
        pthread_mutex_lock(&parkMutex);
        bool ready = parkedCount >= m_threads.size();
        pthread_mutex_unlock(&parkMutex);
        if (ready) {
            // A thread counts itself as parked just before it settles into its
            // wait, so give it that moment before anything reads its state.
            usleep(50 * 1000);
            return true;
        }
        usleep(1000);
    }
    return false;
}

void ParkedThreads::stopAndJoin()
{
    if (m_threads.isEmpty())
        return;

    pthread_mutex_lock(&parkMutex);
    parkStopping = true;
    pthread_mutex_unlock(&parkMutex);

    for (Thread* thread : m_threads) {
        pthread_join(thread->handle, nullptr);
        delete thread;
    }
    m_threads.clear();

    pthread_mutex_lock(&parkMutex);
    parkStopping = false;
    parkedCount = 0;
    pthread_mutex_unlock(&parkMutex);
}

using CreateTargetObject = JSC::Corpse::Address (*)();

enum class TargetAfterSnapshot : bool { KeepsRunning, Exits };

static uintptr_t executableBase()
{
    Dl_info info;
    if (!dladdr(removeCodePtrTag<void*>(&executableBase), &info))
        return 0;
    return reinterpret_cast<uintptr_t>(info.dli_fbase);
}

int runCorpseTarget(const char* offsetText)
{
    auto offset = WTF::parseInteger<uint64_t>(StringView::fromLatin1(offsetText), 16);
    uintptr_t base = executableBase();
    if (!offset || !base) {
        dataLogLn("--target needs the offset of a create function into this executable, in hex");
        return 1;
    }
    // Warning: do not ever move this into a production binary!
    auto create = tagCodePtr<CreateTargetObject, CFunctionPtrTag>(std::bit_cast<void*>(base + *offset));
    reportTargetObjectAndPark(create());
}

// The target is spawned with the read end of its copy's lifetime pipe as its
// standard input, and reports its object, then on Linux its copy, on its
// standard output.
void reportTargetObject(JSC::Corpse::Address object)
{
    uint64_t address = object.toTargetVMAddress();
    RELEASE_ASSERT(write(STDOUT_FILENO, &address, sizeof(address)) == sizeof(address));
#if !OS(DARWIN)
    uint64_t copy = static_cast<uint64_t>(JSC::Corpse::forkCopy(STDIN_FILENO));
    RELEASE_ASSERT(static_cast<pid_t>(copy) > 0);
    RELEASE_ASSERT(write(STDOUT_FILENO, &copy, sizeof(copy)) == sizeof(copy));
#endif
}

void reportTargetObjectAndPark(JSC::Corpse::Address object)
{
    reportTargetObject(object);

    // The analysis kills this process when it is done with the object.
    while (true)
        pause();
}

void initializeQuietJSC()
{
    JSC::initialize([] {
        JSC::Options::useConcurrentGC() = false;
        JSC::Options::useConcurrentJIT() = false;
        JSC::Options::useWarmUpMarkedBlocks() = false;
    });
}

std::unique_ptr<JSC::Corpse::Snapshot> takeSnapshot(pid_t pid)
{
    RefPtr<JSC::Corpse::Process> process = JSC::Corpse::Process::create(pid);
    bool attached = process->attach();
    TEST_ASSERT(attached, "attaching to the target process succeeds");
    if (!attached)
        return nullptr;
    auto snapshot = WTF::makeUnique<JSC::Corpse::Snapshot>(process);
    TEST_ASSERT(snapshot->isValid(), "a snapshot of the target process is valid");
    if (!snapshot->isValid())
        return nullptr;
    return snapshot;
}

static bool readAll(int fd, std::span<uint8_t> destination)
{
    while (!destination.empty()) {
        ssize_t length = read(fd, destination.data(), destination.size());
        if (length < 0 && errno == EINTR)
            continue;
        if (length <= 0)
            return false;
        destination = destination.subspan(static_cast<size_t>(length));
    }
    return true;
}

static void spawnAndAnalyze(const char* executable, char* const* arguments, TargetAfterSnapshot after, NOESCAPE const Function<void()>& afterReport, NOESCAPE const Function<void(JSC::Corpse::Snapshot&, JSC::Corpse::Address)>& analyze)
{
    // Every end is close-on-exec, so that no other spawned program keeps one.
    std::array<int, 2> addressPipe { -1, -1 };
    std::array<int, 2> lifetimePipe { -1, -1 };
    auto closePipes = makeScopeExit([&] {
        for (int fd : addressPipe) {
            if (fd >= 0)
                close(fd);
        }
        for (int fd : lifetimePipe) {
            if (fd >= 0)
                close(fd);
        }
    });
    auto openPipe = [](std::array<int, 2>& ends) {
        if (pipe(ends.data()))
            return false;
        for (int fd : ends)
            fcntl(fd, F_SETFD, FD_CLOEXEC);
        return true;
    };
    bool opened = openPipe(addressPipe) && openPipe(lifetimePipe);
    TEST_ASSERT(opened, "the pipes to and from the target open");
    if (!opened)
        return;

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, addressPipe[1], STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&actions, lifetimePipe[0], STDIN_FILENO);
    pid_t child = 0;
    int error = posix_spawn(&child, executable, &actions, nullptr, arguments, environ);
    posix_spawn_file_actions_destroy(&actions);
    TEST_ASSERT(!error, "the target process launches");
    if (error) {
        dataLogLn("    posix_spawn ", executable, ": ", safeStrerror(error));
        return;
    }
    close(std::exchange(lifetimePipe[0], -1));
    uint64_t copy = 0;
    auto killChild = makeScopeExit([&] {
        kill(child, SIGKILL);
        while (waitpid(child, nullptr, 0) < 0 && errno == EINTR) { }
        // A copy that ended while its target lived is a zombie this process
        // inherited as the subreaper when the target exited.
        if (copy)
            while (waitpid(static_cast<pid_t>(copy), nullptr, 0) < 0 && errno == EINTR) { }
    });

    uint64_t address = 0;
    bool reported = readAll(addressPipe[0], asMutableByteSpan(address));
#if !OS(DARWIN)
    reported = reported && readAll(addressPipe[0], asMutableByteSpan(copy)) && copy;
#endif
    TEST_ASSERT(reported, "the target reports the address of its object");
    if (!reported)
        return;
    afterReport();

#if OS(DARWIN)
    auto snapshot = takeSnapshot(child);
#else
    auto snapshot = WTF::makeUnique<JSC::Corpse::Snapshot>(JSC::Corpse::Process::create(child), static_cast<pid_t>(copy), std::exchange(lifetimePipe[1], -1));
    TEST_ASSERT(snapshot->isValid(), "a snapshot of the target process is valid");
#endif
    if (!snapshot || !snapshot->isValid())
        return;
    if (after == TargetAfterSnapshot::Exits) {
        killChild.release();
        kill(child, SIGKILL);
        while (waitpid(child, nullptr, 0) < 0 && errno == EINTR) { }
    }
    analyze(*snapshot, JSC::Corpse::Address { address });
}

static std::optional<uint64_t> createOffset(CreateTargetObject create)
{
    uintptr_t createAddress = reinterpret_cast<uintptr_t>(removeCodePtrTag(create));
    Dl_info info;
    bool inThisExecutable = dladdr(std::bit_cast<void*>(createAddress), &info) && reinterpret_cast<uintptr_t>(info.dli_fbase) == executableBase();
    TEST_ASSERT(inThisExecutable, "the target's create function is in this executable");
    if (!inThisExecutable)
        return std::nullopt;
    return createAddress - executableBase();
}

static void analyzeOutOfProcess(const char* executablePath, CreateTargetObject create, TargetAfterSnapshot after, NOESCAPE const Function<void()>& afterReport, NOESCAPE const Function<void(JSC::Corpse::Snapshot&, JSC::Corpse::Address)>& analyze)
{
    auto offset = createOffset(create);
    if (!offset)
        return;
    auto offsetText = makeString(hex(*offset)).utf8();
    char* const arguments[] = {
        const_cast<char*>(executablePath),
        const_cast<char*>("--target"),
        const_cast<char*>(offsetText.legacyCStringPointer()),
        nullptr
    };
    spawnAndAnalyze(executablePath, arguments, after, afterReport, analyze);
}

static UTF8CString thisExecutablePath()
{
    UTF8CString executablePath = JSC::Corpse::Process::create(getpid())->executablePath();
    TEST_ASSERT(!executablePath.isNull(), "this process's executable path is readable");
    return executablePath;
}

static void analyzeOutOfProcess(CreateTargetObject create, TargetAfterSnapshot after, NOESCAPE const Function<void(JSC::Corpse::Snapshot&, JSC::Corpse::Address)>& analyze)
{
    UTF8CString executablePath = thisExecutablePath();
    if (!executablePath.isNull())
        analyzeOutOfProcess(executablePath.legacyCStringPointer(), create, after, [] { }, analyze);
}

void analyzeInAndOutOfProcess(CreateTargetObject create, NOESCAPE const Function<void(JSC::Corpse::Snapshot&, JSC::Corpse::Address)>& analyze)
{
    {
        JSC::Corpse::Address object = create();
        if (auto snapshot = takeSnapshot(getpid()))
            analyze(*snapshot, object);
    }
    analyzeOutOfProcess(create, TargetAfterSnapshot::KeepsRunning, analyze);
}

void analyzeInSeparateProcess(CreateTargetObject create, NOESCAPE const Function<void(JSC::Corpse::Snapshot&, JSC::Corpse::Address)>& analyze)
{
    analyzeOutOfProcess(create, TargetAfterSnapshot::KeepsRunning, analyze);
}

void analyzeAfterTargetExits(CreateTargetObject create, NOESCAPE const Function<void(JSC::Corpse::Snapshot&, JSC::Corpse::Address)>& analyze)
{
    analyzeOutOfProcess(create, TargetAfterSnapshot::Exits, analyze);
}

static bool copyFile(const char* from, const char* to)
{
    int source = open(from, O_RDONLY | O_CLOEXEC);
    int destination = open(to, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0755);
    bool copied = source >= 0 && destination >= 0;
    std::array<uint8_t, 64 * 1024> buffer;
    while (copied) {
        ssize_t length = read(source, buffer.data(), buffer.size());
        if (length < 0 && errno == EINTR)
            continue;
        if (length <= 0) {
            copied = !length;
            break;
        }
        copied = write(destination, buffer.data(), static_cast<size_t>(length)) == length;
    }
    if (source >= 0)
        close(source);
    if (destination >= 0)
        close(destination);
    return copied;
}

void analyzeAfterExecutableReplaced(CreateTargetObject create, NOESCAPE const Function<void(JSC::Corpse::Snapshot&, JSC::Corpse::Address)>& analyze)
{
    UTF8CString executablePath = thisExecutablePath();
    if (executablePath.isNull())
        return;
    // Next to this executable, so that it finds the same libraries.
    auto copyPath = makeString(String::fromUTF8(executablePath.legacyCStringPointer()), ".replaced-"_s, getpid()).utf8();
    auto replacementPath = makeString(String::fromUTF8(copyPath.legacyCStringPointer()), ".new"_s).utf8();
    auto removeCopies = makeScopeExit([&] {
        unlink(copyPath.legacyCStringPointer());
        unlink(replacementPath.legacyCStringPointer());
    });
    bool copied = copyFile(executablePath.legacyCStringPointer(), copyPath.legacyCStringPointer());
    TEST_ASSERT(copied, "a copy of this executable is made");
    if (!copied)
        return;
    analyzeOutOfProcess(copyPath.legacyCStringPointer(), create, TargetAfterSnapshot::KeepsRunning, [&] {
        // A new file at the path, as a linker writes one; the target keeps the old one mapped.
        bool replaced = copyFile("/usr/bin/true", replacementPath.legacyCStringPointer()) && !rename(replacementPath.legacyCStringPointer(), copyPath.legacyCStringPointer());
        TEST_ASSERT(replaced, "the target's executable is replaced by another");
    }, analyze);
}

#endif // ENABLE(MYA)

} // namespace JSCToolsTest
