/*
 * Copyright (C) 2026 Apple Inc. All rights reserved.
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

#include <JavaScriptCore/CorpsePlatform.h>

#if ENABLE(MYA)

#include <JavaScriptCore/CorpseAddress.h>
#include <JavaScriptCore/CorpseClient.h>
#include <JavaScriptCore/CorpseError.h>
#include <JavaScriptCore/CorpseHeapWalk.h>
#include <JavaScriptCore/CorpseProcess.h>
#include <JavaScriptCore/CorpseRegion.h>
#include <JavaScriptCore/CorpseSnapshot.h>
#include <JavaScriptCore/CorpseThread.h>
#include <algorithm>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <memory>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <vector>
#if OS(DARWIN)
#include <mach/mach.h>
#include <mach/mach_vm.h>
#endif
#include <wtf/ASCIICType.h>
#include <wtf/Assertions.h>
#include <wtf/CheckedArithmetic.h>
#include <wtf/DoublyLinkedList.h>
#include <wtf/HashMap.h>
#include <wtf/MonotonicTime.h>
#include <wtf/Ref.h>
#include <wtf/RefPtr.h>
#include <wtf/SafeStrerror.h>
#include <wtf/StdLibExtras.h>
#include <wtf/Vector.h>
#include <wtf/text/CString.h>
#include <wtf/text/MakeString.h>
#include <wtf/text/StringBuilder.h>

#if HAVE(READLINE)
// readline/history.h has a Function typedef that conflicts with WTF::Function;
// rename it across these includes to avoid the clash.
#define Function ReadlineFunction
#include <readline/history.h>
#include <readline/readline.h>
#undef Function
#endif

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

using JSC::Corpse::Address;
using JSC::Corpse::Process;
using JSC::Corpse::Snapshot;
using JSC::Corpse::Thread;

namespace Mya {

// A lexer over a null-terminated string. Parsing methods skip leading
// whitespace and advance past whatever they consume; a failed parse leaves the
// position where it was. A Lexer is essentially made up of a position in the
// string. So copying one is how you look ahead without committing.
class Lexer {
public:
    explicit Lexer(const char* text)
        : m_at(text)
    {
    }

    void skipWhitespace()
    {
        while (isTabOrSpace(*m_at))
            ++m_at;
    }

    // True if only whitespace remains.
    bool atEnd()
    {
        skipWhitespace();
        return !*m_at;
    }

    // The next non-whitespace character, or '\0' at end of input.
    char peek()
    {
        skipWhitespace();
        return *m_at;
    }

    // Consumes and returns the next whitespace-delimited token, which is empty
    // at end of input.
    std::string_view nextToken()
    {
        skipWhitespace();
        const char* start = m_at;
        while (*m_at && !isTabOrSpace(*m_at))
            ++m_at;
        return std::string_view(start, static_cast<size_t>(m_at - start));
    }

    // Consumes the next token only if it matches word.
    bool consumeToken(const char* word)
    {
        Lexer probe = *this;
        if (probe.nextToken() != word)
            return false;
        *this = probe;
        return true;
    }

    // Consumes the next non-whitespace character only if it matches c.
    bool consumeChar(char c)
    {
        skipWhitespace();
        if (*m_at != c)
            return false;
        ++m_at;
        return true;
    }

    // Consumes a positive number from the specified `minimum` upwards, but capped at INT_MAX.
    template<long minimum = 0, typename T>
    bool consumeUint32(T& out)
    {
        skipWhitespace();
        if (!isASCIIDigit(*m_at))
            return false; // Rejects cases like -0, -1, +3, which strtol allows.
        errno = 0;
        char* end = nullptr;
        long value = strtol(m_at, &end, 10);
        if (end == m_at || errno || value < minimum || value > INT_MAX)
            return false;
        m_at = end;
        out = static_cast<T>(value);
        return true;
    }

    bool consumePID(pid_t& pid) { return consumeUint32<1>(pid); }

private:
    const char* m_at;
};

class Shell {
public:
    ~Shell()
    {
        cleanup();
    }

    int run(int argc, char** argv)
    {
        JSC::Corpse::Client::setName("mya"_s);
        auto action = parseArguments(argc, argv);
        switch (action) {
        case ContinuationAction::Continue:
            openHistory();
            runInteractive();
            return 0;
        case ContinuationAction::Exit:
            return 0;
        case ContinuationAction::Error:
            return 1;
        }
        RELEASE_ASSERT_NOT_REACHED();
        return 1;
    }

private:
    static constexpr const char* prompt = ">>> ";
    static constexpr const char* historyDirName = ".mya";
    static constexpr const char* historyFileName = "history";
    static constexpr unsigned defaultMaxHistoryEntries = 50;
    static constexpr unsigned minHistorySize = 5;

    // The history file records the target max history entries in its first line, followed by
    // historical commands. To avoid re-writing the file on every new command, we allow the file
    // to exceed the max entries by maxOverflowEntries, before we do a re-write to purge
    // the extra entries. We will keep appending to the same file until the re-write is needed.
    static constexpr const char* maxEntriesHeaderPrefix = "max entries ";
    static constexpr unsigned maxOverflowEntries = 100;

    static void printUsage(FILE* out)
    {
        fputs("Mya (MY-uh /ˈmaɪə/) - MemorY Analyzer\n", out);
        fputs("Usage:\n", out);
        fputs("  mya [--pid|-p <pid>]\n", out);
        fputs("  mya [--help|-h [<command>]]\n", out);
        fputs("Commands:\n", out);
        fputs("  attach [--pid|-p] <pid>   Set the target PID and attach\n", out);
        fputs("  detach                    Detach from the current PID\n", out);
        fputs("  status (st)               Show whether mya is attached\n", out);
        fputs("  snapshot (sn, snap) ...   Capture and manage snapshots\n", out);
        fputs("  thread (th) ...           Inspect the threads in a snapshot\n", out);
        fputs("  heap [<vm>]               Walk the JS heap of the VM at <vm>, or of WebCore's main VM\n", out);
        fputs("  heapdump <file> [<vm>]    Write the heap as a GCDebugging heap snapshot Web Inspector reads\n", out);
#if OS(DARWIN)
        fputs("  memory [<vm>]             Account for the target's footprint by VM tag, and for libpas's pages\n", out);
#endif
        fputs("  p[/x] &<symbol>           Print a symbol's address, /x for hex\n", out);
        fputs("  history (hi, hist) ...    Show and manage the command history\n", out);
        fputs("  help [<command>]          Show this help, or help for <command>\n", out);
        fputs("  quit (q, exit)            Exit mya\n", out);
        fputs("\n", out);
        fputs("  Use `help snapshot`, `help thread` or `help history` for their subcommands.\n", out);
        fputs("\n", out);
    }

    static void printThreadUsage(FILE* out)
    {
        fputs("thread - inspect the threads captured in a snapshot\n", out);
        fputs("  thread list (li)          List the threads in the snapshot in use\n", out);
        fputs("\n", out);
        fputs("  `thread` may be abbreviated as `th`, and lists by default.\n", out);
        fputs("  Threads are read from the snapshot in use; see `help snapshot`.\n", out);
        fputs("\n", out);
    }

    static void printSnapshotUsage(FILE* out)
    {
        fputs("snapshot - capture and manage snapshots of a process\n", out);
        fputs("  snapshot                  Capture a snapshot of the current process\n", out);
        fputs("  snapshot --pid|-p <pid>   Attach to <pid> and capture a snapshot of it\n", out);
        fputs("  snapshot <n>              Switch to using snapshot <n>\n", out);
        fputs("  snapshot list (li)        List captured snapshots (* marks the one in use)\n", out);
        fputs("  snapshot info (inf) <n>   Show details of snapshot <n>\n", out);
        fputs("  snapshot delete (del) <n> Delete snapshot <n>\n", out);
        fputs("  snapshot diff <a> <b>     Diff snapshot <a> against snapshot <b>\n", out);
        fputs("\n", out);
        fputs("  `snapshot` may be abbreviated as `sn` or `snap`.\n", out);
        fputs("  Capturing a snapshot switches to using it.\n", out);
        fputs("\n", out);
    }

    static void printHistoryUsage(FILE* out)
    {
        fputs("history - show and manage the command history\n", out);
        fputs("  history                   List the command history\n", out);
        fputs("  history clear [<n>]       Clear the history, or its <n> oldest entries\n", out);
        fputs("  history size [<n>]        Show or set the max entries kept\n", out);
        fputs("  !<n>                      Replay history entry <n>\n", out);
        fputs("  !!                        Replay the previous command\n", out);
        fputs("\n", out);
        fputs("  `history` may be abbreviated as `hi` or `hist`.\n", out);
        fprintf(out, "  Command history is kept (defaults up to %u entries) in ~/%s/%s.\n",
            defaultMaxHistoryEntries, historyDirName, historyFileName);
#if HAVE(READLINE)
        fputs("  It is navigable with the Up/Down arrows and Ctrl-R reverse search.\n", out);
#endif
        fputs("\n", out);
    }

    // Dispatches `help [<command>]`. `lex` is positioned after the "help" word.
    static bool handleHelp(Lexer lex)
    {
        if (lex.atEnd()) {
            printUsage(stdout);
            return true;
        }
        std::string_view topic = lex.nextToken();
        if (topic == "sn" || topic == "snap" || topic == "snapshot") {
            printSnapshotUsage(stdout);
            return true;
        }
        if (topic == "hi" || topic == "hist" || topic == "history") {
            printHistoryUsage(stdout);
            return true;
        }
        if (topic == "th" || topic == "thread") {
            printThreadUsage(stdout);
            return true;
        }
        fprintf(stderr, "mya: No help for '%.*s'\n", static_cast<int>(topic.length()), topic.data());
        return false;
    }

    // Writes a byte count in the largest unit that keeps it readable, e.g. "512 KB" or "1.50 MB".
    static void formatByteSize(size_t bytes, char* out, size_t outSize)
    {
        if (bytes >= 1024 * 1024)
            snprintf(out, outSize, "%.2f MB", bytes / (1024.0 * 1024.0));
        else if (bytes >= 1024)
            snprintf(out, outSize, "%zu KB", bytes / 1024);
        else
            snprintf(out, outSize, "%zu B", bytes);
    }

    enum class ContinuationAction { Continue, Exit, Error };
    ContinuationAction parseArguments(int argc, char** argv)
    {
        // Help is answered before anything else is acted on, so that asking for it
        // never attaches to a process or takes a snapshot along the way.
        for (int i = 1; i < argc; ++i) {
            std::string_view arg = argv[i];
            if (arg != "--help" && arg != "-h")
                continue;
            if (i + 1 >= argc) {
                printUsage(stdout);
                return ContinuationAction::Exit;
            }
            Lexer lex(argv[i + 1]);
            return handleHelp(lex) ? ContinuationAction::Exit : ContinuationAction::Error;
        }

        for (int i = 1; i < argc; ++i) {
            const char* argText = argv[i];
            std::string_view arg = argText;
            const char* pidText = nullptr;
            if (arg == "--pid" || arg == "-p") {
                if (i + 1 >= argc) {
                    fprintf(stderr, "mya: %s requires an argument\n", argText);
                    return ContinuationAction::Error;
                }
                pidText = argv[++i];
            } else if (arg.starts_with("--pid="))
                pidText = argText + 6;
            else if (arg.starts_with("-p") && arg.size() > 2)
                pidText = argText + 2; // "-p12345"
            else {
                fprintf(stderr, "mya: Unknown option '%s'\n", argText);
                return ContinuationAction::Error;
            }

            pid_t pid = -1;
            Lexer lex(pidText);
            if (!lex.consumePID(pid) || !lex.atEnd()) {
                fprintf(stderr, "mya: Invalid PID '%s'\n", pidText);
                return ContinuationAction::Error;
            }

            attachAndSnapshot(pid);
        }
        return ContinuationAction::Continue;
    }

    void attach(pid_t pid)
    {
        RefPtr<Process> process;
        auto existing = m_processes.find(pid);
        if (existing != m_processes.end())
            process = existing->value;
        else {
            process = Process::create(pid);
            m_processes.add(pid, process);
        }

        if (!process->attach())
            return;

        if (m_currentProcess && m_currentProcess != process)
            m_currentProcess->detach();

        m_currentProcess = WTF::move(process);
        printf("Attached to %d\n", static_cast<int>(m_currentProcess->pid()));
    }

    void detach()
    {
        if (!m_currentProcess) {
            fputs("Not attached to any process.\n", stdout);
            return;
        }
        pid_t pid = m_currentProcess->pid();
        m_currentProcess->detach();
        m_currentProcess = nullptr;
        printf("Detached from %d\n", static_cast<int>(pid));
    }

    // `mya --pid <pid>` and `snapshot --pid <pid>` both attach then snapshot.
    void attachAndSnapshot(pid_t pid)
    {
        attach(pid);
        if (m_currentProcess && m_currentProcess->pid() == pid)
            captureSnapshot();
    }

    void captureSnapshot()
    {
        if (!m_currentProcess) {
            fputs("Unable to capture snapshot. Not attached to any process. Use `attach` command or specify `--pid` argument for the snapshot command.\n", stderr);
            return;
        }
        auto snapshot = WTF::makeUnique<Snapshot>(m_currentProcess);
        if (!snapshot->isValid())
            return; // The Snapshot constructor already logged the failure.
        unsigned id = snapshot->id();
        // The map owns the Snapshot and the list only records capture order.
        Snapshot* node = snapshot.get();
        m_snapshotsById.add(id, WTF::move(snapshot));
        m_snapshots.append(node);
        printf("Captured Snapshot #%u of %d\n", id, static_cast<int>(m_currentProcess->pid()));
        useSnapshot(id); // Capturing switches to the new snapshot.
    }

    // Sets the current snapshot used by subsequent commands.
    void useSnapshot(unsigned id)
    {
        if (!snapshotById(id)) {
            fprintf(stderr, "mya: No snapshot #%u\n", id);
            return;
        }
        if (m_currentSnapshot) {
            if (m_currentSnapshot == id)
                printf("Already using snapshot %u\n", id);
            else
                printf("Switching to using snapshot %u\n", id);
        }
        m_currentSnapshot = id;
    }

    // Returns the snapshot with the given id, or nullptr if there is none.
    Snapshot* snapshotById(unsigned id) const
    {
        auto entry = m_snapshotsById.find(id);
        return entry != m_snapshotsById.end() ? entry->value.get() : nullptr;
    }

    void listSnapshots()
    {
        if (m_snapshots.isEmpty()) {
            fputs("No snapshots.\n", stdout);
            return;
        }
        for (Snapshot* snapshot = m_snapshots.head(); snapshot; snapshot = snapshot->next()) {
            // Mark the snapshot currently in use.
            const char* marker = snapshot->id() == m_currentSnapshot ? "*" : " ";
            printf("%s #%u: pid %d\n", marker, snapshot->id(), static_cast<int>(snapshot->process()->pid()));
        }
    }

    void snapshotInfo(unsigned id)
    {
        Snapshot* snapshot = snapshotById(id);
        if (!snapshot) {
            fprintf(stderr, "mya: No snapshot #%u\n", id);
            return;
        }
        printf("Snapshot #%u: pid %d, corpse %s\n", id,
            static_cast<int>(snapshot->process()->pid()), snapshot->isValid() ? "valid" : "invalid");
    }

    void snapshotDelete(unsigned id)
    {
        Snapshot* snapshot = snapshotById(id);
        if (!snapshot) {
            fprintf(stderr, "mya: No snapshot #%u\n", id);
            return;
        }
        // Unlink before dropping the owning entry: the list does not own its
        // nodes, so it must not be left pointing at a destroyed Snapshot.
        m_snapshots.remove(snapshot);
        m_snapshotsById.remove(id);
        if (id == m_currentSnapshot)
            m_currentSnapshot = 0;
        printf("Deleted Snapshot #%u.\n", id);
    }

    void snapshotDiff(unsigned a, unsigned b)
    {
        if (!snapshotById(a)) {
            fprintf(stderr, "mya: No snapshot #%u\n", a);
            return;
        }
        if (!snapshotById(b)) {
            fprintf(stderr, "mya: No snapshot #%u\n", b);
            return;
        }
        printf("Snapshot diff #%u vs #%u is not implemented yet.\n", a, b);
    }

    // `lex` is positioned after the "snapshot" command word.
    void handleSnapshot(Lexer lex)
    {
        if (lex.atEnd()) {
            captureSnapshot();
            return;
        }
        // A bare number switches to that snapshot e.g. "snapshot 3".
        if (isASCIIDigit(static_cast<unsigned char>(lex.peek()))) {
            unsigned number = 0;
            if (!lex.consumeUint32(number) || !lex.atEnd()) {
                fputs("Usage: snapshot <n>\n", stderr);
                return;
            }
            useSnapshot(number);
            return;
        }
        if (lex.consumeToken("--pid") || lex.consumeToken("-p")) {
            pid_t pid = -1;
            if (!lex.consumePID(pid) || !lex.atEnd()) {
                fputs("Usage: snapshot [--pid|-p] <pid>\n", stderr);
                return;
            }
            attachAndSnapshot(pid);
            return;
        }
        if (lex.consumeToken("li") || lex.consumeToken("list")) {
            listSnapshots();
            return;
        }
        if (lex.consumeToken("inf") || lex.consumeToken("info")) {
            unsigned number = 0;
            if (!lex.consumeUint32(number) || !lex.atEnd()) {
                fputs("Usage: snapshot info <n>\n", stderr);
                return;
            }
            snapshotInfo(number);
            return;
        }
        if (lex.consumeToken("del") || lex.consumeToken("delete")) {
            unsigned number = 0;
            if (!lex.consumeUint32(number) || !lex.atEnd()) {
                fputs("Usage: snapshot delete <n>\n", stderr);
                return;
            }
            snapshotDelete(number);
            return;
        }
        if (lex.consumeToken("diff")) {
            unsigned a = 0;
            unsigned b = 0;
            if (!lex.consumeUint32(a) || !lex.consumeUint32(b) || !lex.atEnd()) {
                fputs("Usage: snapshot diff <a> <b>\n", stderr);
                return;
            }
            snapshotDiff(a, b);
            return;
        }
        std::string_view token = lex.nextToken();
        fprintf(stderr, "mya: Unknown snapshot subcommand '%.*s'\n",
            static_cast<int>(token.length()), token.data());
    }

    // Lists the threads captured in the snapshot currently in use.
    void listThreads()
    {
        Snapshot* snapshot = snapshotById(m_currentSnapshot);
        if (!snapshot) {
            fputs("No snapshot in use. Capture one with `snapshot`, or select one with `snapshot <n>`.\n", stderr);
            return;
        }

        const Vector<Thread>& threads = snapshot->threads();
        if (threads.isEmpty()) {
            fputs("No threads.\n", stdout);
            return;
        }

        printf("Threads in snapshot #%u (pid %d):\n", snapshot->id(), static_cast<int>(snapshot->process()->pid()));

        // Build the rows as text first so each column can be sized to its widest entry.
        static constexpr size_t columnCount = 12;
        static const char* const headings[columnCount] = {
            "INDEX", "TID", "STATE", "USER(ms)", "SYS(ms)", "SP", "STACK", "SIZE",
            "PAGES", "RESIDENT", "DIRTY", "NAME"
        };
        static const bool rightAligned[columnCount] = {
            true, false, false, true, true, false, false, true, true, true, true, false
        };

        struct Row {
            std::string cells[columnCount];
        };
        Vector<Row> rows;
        rows.reserveCapacity(threads.size());

        char buffer[64];
        for (size_t i = 0; i < threads.size(); ++i) {
            const Thread& thread = threads[i];
            Row row;

            snprintf(buffer, sizeof(buffer), "%zu", i + 1);
            row.cells[0] = buffer;
            snprintf(buffer, sizeof(buffer), "0x%llx", static_cast<unsigned long long>(thread.id()));
            row.cells[1] = buffer;
            row.cells[2] = thread.runStateDescription();
            snprintf(buffer, sizeof(buffer), "%.3f", thread.userTimeUsec() / 1000.0);
            row.cells[3] = buffer;
            snprintf(buffer, sizeof(buffer), "%.3f", thread.systemTimeUsec() / 1000.0);
            row.cells[4] = buffer;
            if (thread.stackPointer()) {
                SAFE_SPRINTF(std::span { buffer }, "0x%llx",
                    static_cast<unsigned long long>(thread.stackPointer().toTargetVMAddress()));
                row.cells[5] = buffer;
            } else
                row.cells[5] = "-";
            if (thread.hasStack()) {
                const auto& stack = thread.stackRegion();
                SAFE_SPRINTF(std::span { buffer }, "0x%llx-0x%llx",
                    static_cast<unsigned long long>(stack.base().toTargetVMAddress()),
                    static_cast<unsigned long long>(stack.end().toTargetVMAddress()));
                row.cells[6] = buffer;
                formatByteSize(stack.size(), buffer, sizeof(buffer));
                row.cells[7] = buffer;
                snprintf(buffer, sizeof(buffer), "%llu",
                    static_cast<unsigned long long>(stack.pageCount()));
                row.cells[8] = buffer;
                snprintf(buffer, sizeof(buffer), "%llu",
                    static_cast<unsigned long long>(stack.residentPageCount()));
                row.cells[9] = buffer;
                snprintf(buffer, sizeof(buffer), "%llu",
                    static_cast<unsigned long long>(stack.dirtyPageCount()));
                row.cells[10] = buffer;
            } else {
                row.cells[6] = "-";
                row.cells[7] = "-";
                row.cells[8] = "-";
                row.cells[9] = "-";
                row.cells[10] = "-";
            }
            row.cells[11] = thread.name().empty() ? "-" : thread.name();

            rows.append(WTF::move(row));
        }

        size_t widths[columnCount];
        for (size_t column = 0; column < columnCount; ++column) {
            widths[column] = strlen(headings[column]);
            for (const Row& row : rows)
                widths[column] = std::max(widths[column], row.cells[column].length());
        }

        auto printRow = [&](auto&& cellAt) {
            fputs("  ", stdout);
            for (size_t column = 0; column < columnCount; ++column) {
                if (column)
                    fputs("  ", stdout);
                const char* text = cellAt(column);
                // The last column needs no padding, which also avoids trailing
                // whitespace on every line.
                if (column == columnCount - 1)
                    fputs(text, stdout);
                else if (rightAligned[column])
                    printf("%*s", static_cast<int>(widths[column]), text);
                else
                    printf("%-*s", static_cast<int>(widths[column]), text);
            }
            putchar('\n');
        };

        printRow([&](size_t column) { return headings[column]; });
        for (const Row& row : rows)
            printRow([&](size_t column) { return row.cells[column].c_str(); });
    }

    // Dispatches the `thread ...` subcommands. `lex` is positioned after the
    // "thread" command word.
    void handleThread(Lexer lex)
    {
        // Listing is the default, so a bare `thread` lists too.
        if (lex.atEnd() || lex.consumeToken("li") || lex.consumeToken("list")) {
            if (!lex.atEnd()) {
                fputs("Usage: thread list\n", stderr);
                return;
            }
            listThreads();
            return;
        }
        std::string_view token = lex.nextToken();
        fprintf(stderr, "mya: Unknown thread subcommand '%.*s'\n",
            static_cast<int>(token.length()), token.data());
    }

    // Dispatches `heap [<vm>]`: walks the JS heap of the snapshot in use from
    // the VM at the hex address <vm>, or from WebCore's g_commonVMOrNull, and
    // reports how much of libpas's heap the walk reaches and reads as a type,
    // and what holds what it misses.
    // The VM at the address `lex` names, or WebCore's main thread VM, or else
    // the VM JavaScriptCore last entered; and JavaScriptCore's image.
    bool findHeap(Snapshot& snapshotRef, Lexer& lex, Address& vm, Address& javaScriptCore)
    {
        Snapshot* snapshot = &snapshotRef;
        if (!lex.atEnd()) {
            std::string token { lex.nextToken() };
            char* end = nullptr;
            unsigned long long value = strtoull(token.c_str(), &end, 16);
            if (!value || *end || !lex.atEnd()) {
                fputs("Usage: heap [<vm address, in hex>]\n", stderr);
                return false;
            }
            vm = Address { static_cast<uint64_t>(value) };
        } else {
            // WebCore's main thread VM, or else the VM JavaScriptCore last entered.
            for (const char* name : { "_ZN7WebCore16g_commonVMOrNullE", "_ZN3JSC9VMManager10s_recentVME" }) {
                Address variable = snapshot->symbol(name);
                auto pointer = variable ? snapshot->memory().ptr<uint64_t>(variable) : JSC::Corpse::Memory::Ptr<uint64_t> { };
                if (pointer && *pointer) {
                    vm = Address { *pointer };
                    break;
                }
            }
            if (!vm) {
                fputs("mya: No VM in this snapshot; name a VM's address\n", stderr);
                return false;
            }
        }
        for (const JSC::Corpse::Image& image : snapshot->images()) {
            std::string_view path { image.path().legacyCStringPointer() };
            if (path.ends_with("/JavaScriptCore") || path.ends_with("/libJavaScriptCore.so") || path.find("/libjavascriptcore") != std::string_view::npos)
                javaScriptCore = image.loadAddress();
        }
        if (!javaScriptCore) {
            fputs("mya: JavaScriptCore is not among the snapshot's images\n", stderr);
            return false;
        }
        return true;
    }

#if OS(DARWIN)
    static String vmTagName(unsigned tag)
    {
        switch (tag) {
        case 0: return "untagged"_s;
        case VM_MEMORY_MALLOC: return "malloc"_s;
        case VM_MEMORY_MALLOC_SMALL: return "malloc small"_s;
        case VM_MEMORY_MALLOC_LARGE: return "malloc large"_s;
        case VM_MEMORY_MALLOC_HUGE: return "malloc huge"_s;
        case VM_MEMORY_MALLOC_TINY: return "malloc tiny"_s;
        case VM_MEMORY_MALLOC_NANO: return "malloc nano"_s;
        case VM_MEMORY_MALLOC_MEDIUM: return "malloc medium"_s;
        case VM_MEMORY_MALLOC_LARGE_REUSABLE: return "malloc large reusable"_s;
        case VM_MEMORY_MALLOC_LARGE_REUSED: return "malloc large reused"_s;
        case VM_MEMORY_STACK: return "stack"_s;
        case VM_MEMORY_GUARD: return "guard"_s;
        case VM_MEMORY_IOKIT: return "IOKit"_s;
        case VM_MEMORY_SHARED_PMAP: return "shared pmap"_s;
        case VM_MEMORY_DYLD: return "dyld"_s;
        case VM_MEMORY_DYLD_MALLOC: return "dyld malloc"_s;
        case VM_MEMORY_TCMALLOC: return "libpas (WebKit Malloc)"_s;
        case VM_MEMORY_JAVASCRIPT_CORE: return "JavaScriptCore"_s;
        case VM_MEMORY_JAVASCRIPT_JIT_EXECUTABLE_ALLOCATOR: return "JIT code"_s;
        case VM_MEMORY_JAVASCRIPT_JIT_REGISTER_FILE: return "JS register file"_s;
        case VM_MEMORY_OS_ALLOC_ONCE: return "OS alloc once"_s;
        case VM_MEMORY_LIBDISPATCH: return "libdispatch"_s;
        case VM_MEMORY_ACCELERATE: return "Accelerate"_s;
        case VM_MEMORY_COREGRAPHICS: return "CoreGraphics"_s;
        case VM_MEMORY_FOUNDATION: return "Foundation"_s;
        case VM_MEMORY_CORESERVICES: return "CoreServices"_s;
        case VM_MEMORY_IMAGEIO: return "ImageIO"_s;
        case VM_MEMORY_COLORSYNC: return "ColorSync"_s;
        case VM_MEMORY_APPLICATION_SPECIFIC_1: return "application specific 1"_s;
        default: return makeString("tag "_s, tag);
        }
    }

    static std::optional<uint64_t> physicalFootprint(mach_port_t task)
    {
        task_vm_info_data_t info { };
        mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
        if (task_info(task, TASK_VM_INFO, reinterpret_cast<task_info_t>(&info), &count) != KERN_SUCCESS)
            return std::nullopt;
        return info.phys_footprint;
    }

    // What the target's memory footprint is made of: each VM tag's private dirty
    // and compressed pages, in the snapshot and in the live process, and what
    // libpas's own dirty and compressed pages hold, by its enumerator's records.
    void handleMemory(Lexer lex)
    {
        using JSC::Corpse::HeapWalk;
        using JSC::Corpse::Region;
        Snapshot* snapshot = snapshotById(m_currentSnapshot);
        if (!snapshot) {
            SAFE_FPRINTF(stderr, "No snapshot in use. Capture one with `snapshot`, or select one with `snapshot <n>`.\n");
            return;
        }
        Address vm;
        Address javaScriptCore;
        if (!findHeap(*snapshot, lex, vm, javaScriptCore))
            return;
        HeapWalk heap(*snapshot, vm, javaScriptCore);
        if (!heap.isValid()) {
            SAFE_FPRINTF(stderr, "mya: No JS heap at 0x%llx\n", static_cast<unsigned long long>(vm.toTargetVMAddress()));
            return;
        }
        uint64_t pageSize = vm_kernel_page_size;
        struct Totals {
            size_t regions { 0 };
            uint64_t footprint { 0 }; // By the pages' dispositions, as the snapshot was taken.
            uint64_t counted { 0 }; // By the regions' own dirty and compressed page counts.
            uint64_t reusable { 0 };
        };
        auto byTag = [&](const Vector<Region>& regions, bool fromSnapshot) {
            HashMap<unsigned, Totals, IntHash<unsigned>, WTF::UnsignedWithZeroKeyHashTraits<unsigned>> result;
            for (const Region& region : regions) {
                if (!region.isPrivate())
                    continue;
                auto& totals = result.add(region.userTag(), Totals { }).iterator->value;
                ++totals.regions;
                totals.footprint += fromSnapshot ? snapshot->footprintBytes(region) : 0;
                totals.counted += (region.dirtyPageCount() + region.swappedPageCount()) * pageSize;
                totals.reusable += region.reusablePageCount() * pageSize;
            }
            return result;
        };
        const Vector<Region>& snapshotRegions = snapshot->regions();
        auto snapshotTotals = byTag(snapshotRegions, true);
        Process* process = snapshot->process();
        bool hasLive = process && process->isAttached();
        auto liveTotals = hasLive ? byTag(Region::all(process->taskPort()), false) : decltype(snapshotTotals) { };

        auto footprint = snapshot->physicalFootprint();
        auto liveFootprint = hasLive ? physicalFootprint(process->taskPort()) : std::nullopt;
        SAFE_PRINTF("Physical footprint: %llu KB in the snapshot, %s KB live\n", static_cast<unsigned long long>(footprint.value_or(0) / KB),
            (liveFootprint ? String::number(*liveFootprint / KB) : "unknown"_s).utf8());
        Vector<std::pair<unsigned, Totals>> tags;
        for (auto& [tag, totals] : snapshotTotals)
            tags.append({ tag, totals });
        std::ranges::sort(tags, std::ranges::greater { }, [](auto& entry) { return entry.second.footprint; });
        uint64_t sum = 0;
        SAFE_PRINTF("  %-28s %8s %12s %12s %12s %12s\n", "private memory by VM tag"_s, "regions"_s, "footprint KB"_s, "counted KB"_s, "live KB"_s, "reusable KB"_s);
        for (auto& [tag, totals] : tags) {
            uint64_t live = liveTotals.contains(tag) ? liveTotals.get(tag).counted : 0;
            sum += totals.footprint;
            if (!totals.footprint && !totals.counted && !live)
                continue;
            SAFE_PRINTF("  %-28s %8zu %12llu %12llu %12llu %12llu\n", vmTagName(tag).utf8(), totals.regions, static_cast<unsigned long long>(totals.footprint / KB),
                static_cast<unsigned long long>(totals.counted / KB), static_cast<unsigned long long>(live / KB), static_cast<unsigned long long>(totals.reusable / KB));
        }
        SAFE_PRINTF("  %-28s %8s %12llu\n", "sum"_s, ""_s, static_cast<unsigned long long>(sum / KB));

        auto records = heap.libpasRecords();
        auto pages = records ? heap.libpasPages(*records) : std::nullopt;
        if (!pages)
            return;
        SAFE_PRINTF("libpas, as the snapshot was taken: %llu KB of dirty or compressed pages (%llu KB compressed), holding\n",
            static_cast<unsigned long long>(pages->footprintBytes / KB), static_cast<unsigned long long>(pages->compressedBytes / KB));
        SAFE_PRINTF("  %llu KB of objects, %llu KB of payload no live object holds, %llu KB of metadata, %llu KB libpas records nothing in\n",
            static_cast<unsigned long long>(pages->objectBytes / KB), static_cast<unsigned long long>(pages->freePayloadBytes / KB),
            static_cast<unsigned long long>(pages->metaBytes / KB), static_cast<unsigned long long>(pages->unrecordedBytes / KB));
        SAFE_PRINTF("  and %llu KB of objects in pages outside the footprint; %llu KB of libpas's pages are reusable\n",
            static_cast<unsigned long long>(pages->objectBytesElsewhere / KB), static_cast<unsigned long long>(pages->reusableBytes / KB));
        uint64_t otherTags = sum - std::min(sum, snapshotTotals.contains(VM_MEMORY_TCMALLOC) ? snapshotTotals.get(VM_MEMORY_TCMALLOC).footprint : 0);
        uint64_t ledger = snapshot->taggedLedgerBytes();
        uint64_t accounted = pages->footprintBytes + otherTags + ledger;
        int64_t residual = static_cast<int64_t>(footprint.value_or(0)) - static_cast<int64_t>(accounted);
        SAFE_PRINTF("Footprint %llu KB = libpas's pages %llu KB + other private memory %llu KB + graphics, media, network and neural memory %llu KB + %lld KB that no region or ledger shows, such as page tables\n",
            static_cast<unsigned long long>(footprint.value_or(0) / KB), static_cast<unsigned long long>(pages->footprintBytes / KB),
            static_cast<unsigned long long>(otherTags / KB), static_cast<unsigned long long>(ledger / KB), static_cast<long long>(residual / static_cast<int64_t>(KB)));
    }
#endif

    // `heapdump <file> [<vm>]`: the walk's heap dump, as a GCDebugging heap snapshot Web Inspector reads.
    void handleHeapDump(Lexer lex)
    {
        using JSC::Corpse::HeapWalk;
        Snapshot* snapshot = snapshotById(m_currentSnapshot);
        if (!snapshot) {
            SAFE_FPRINTF(stderr, "No snapshot in use. Capture one with `snapshot`, or select one with `snapshot <n>`.\n");
            return;
        }
        if (lex.atEnd()) {
            fputs("Usage: heapdump <file> [<vm address, in hex>]\n", stderr);
            return;
        }
        std::string path { lex.nextToken() };
        Address vm;
        Address javaScriptCore;
        if (!findHeap(*snapshot, lex, vm, javaScriptCore))
            return;
        HeapWalk heap(*snapshot, vm, javaScriptCore);
        auto allocations = heap.isValid() ? heap.libpasAllocations() : std::nullopt;
        if (!allocations) {
            SAFE_FPRINTF(stderr, "mya: No JS heap at 0x%llx\n", static_cast<unsigned long long>(vm.toTargetVMAddress()));
            return;
        }
        Vector<HeapWalk::Reference> references;
        HeapWalk::Reach reach = heap.reach(*allocations, { }, { }, 0, &references);
        HeapWalk::HeapDump dump = heap.heapDump(*allocations, reach, references);
        CString json = dump.json().utf8();
        FILE* file = fopen(path.c_str(), "w");
        if (!file || fwrite(json.data(), 1, json.length(), file) != json.length()) {
            SAFE_FPRINTF(stderr, "mya: Could not write %s: %s\n", String::fromUTF8(path.c_str()).utf8(), safeStrerror(errno));
            if (file)
                fclose(file);
            return;
        }
        fclose(file);
        uint64_t bytes = 0;
        std::array<uint64_t, 4> bytesByKind { };
        for (auto& node : dump.nodes) {
            bytes += node.size;
            bytesByKind[static_cast<size_t>(node.kind)] += node.size;
        }
        SAFE_PRINTF("Wrote %zu nodes and %zu edges, %llu bytes in all, to %s\n", dump.nodes.size(), dump.edges.size(), static_cast<unsigned long long>(bytes), String::fromUTF8(path.c_str()).utf8());
        SAFE_PRINTF("  %llu bytes of live cells, %llu of other allocations the walk reached, %llu of allocations it missed; %zu references read\n",
            static_cast<unsigned long long>(bytesByKind[static_cast<size_t>(HeapWalk::HeapDump::Kind::Cell)]),
            static_cast<unsigned long long>(bytesByKind[static_cast<size_t>(HeapWalk::HeapDump::Kind::Allocation)]),
            static_cast<unsigned long long>(bytesByKind[static_cast<size_t>(HeapWalk::HeapDump::Kind::Missed)]), references.size());
#if OS(DARWIN)
        auto records = heap.libpasRecords();
        auto pages = records ? heap.libpasPages(*records) : std::nullopt;
        if (pages) {
            SAFE_PRINTF("  of which %llu bytes are in libpas's dirty or compressed pages, and %llu bytes in pages outside the footprint\n",
                static_cast<unsigned long long>(pages->objectBytes), static_cast<unsigned long long>(pages->objectBytesElsewhere));
            SAFE_PRINTF("  those pages also hold %llu bytes of free payload, %llu of metadata and %llu libpas records nothing in; `memory` accounts for the rest of the footprint\n",
                static_cast<unsigned long long>(pages->freePayloadBytes), static_cast<unsigned long long>(pages->metaBytes), static_cast<unsigned long long>(pages->unrecordedBytes));
        }
#endif
    }

    void handleHeap(Lexer lex)
    {
        using JSC::Corpse::HeapWalk;
        Snapshot* snapshot = snapshotById(m_currentSnapshot);
        if (!snapshot) {
            SAFE_FPRINTF(stderr, "No snapshot in use. Capture one with `snapshot`, or select one with `snapshot <n>`.\n");
            return;
        }
        Address vm;
        Address javaScriptCore;
        if (!findHeap(*snapshot, lex, vm, javaScriptCore))
            return;

        MonotonicTime start = MonotonicTime::now();
        HeapWalk heap(*snapshot, vm, javaScriptCore);
        if (!heap.isValid()) {
            SAFE_FPRINTF(stderr, "mya: No JS heap at 0x%llx\n", static_cast<unsigned long long>(vm.toTargetVMAddress()));
            return;
        }
        MonotonicTime opened = MonotonicTime::now();
        size_t cells = 0;
        uint64_t cellBytes = 0;
        heap.forEachLiveCell([&](const HeapWalk::Cell& cell) {
            ++cells;
            cellBytes += cell.size;
            return IterationStatus::Continue;
        });
        MonotonicTime walked = MonotonicTime::now();
        auto allocations = heap.libpasAllocations();
        if (!allocations)
            return;
        MonotonicTime enumerated = MonotonicTime::now();
        constexpr size_t listCount = 25;
        HeapWalk::Reach reach = heap.reach(*allocations, { }, { }, listCount);
        MonotonicTime reached = MonotonicTime::now();

        auto string = [](const String& value) {
            return value.utf8();
        };
        SAFE_PRINTF("The heap of the VM at 0x%llx, %s\n", static_cast<unsigned long long>(vm.toTargetVMAddress()),
            heap.isAtSafePoint() ? "at mya's safe point"_s : "unverified: the target was not at mya's safe point"_s);
        SAFE_PRINTF("  %zu live cells, %llu bytes, in %llu libpas allocations of %llu bytes\n", cells, static_cast<unsigned long long>(cellBytes),
            static_cast<unsigned long long>(allocations->size()), static_cast<unsigned long long>(reach.bytesAllocated));
        SAFE_PRINTF("  reached: %.2f%% (%llu bytes)\n", reach.percent(), static_cast<unsigned long long>(reach.bytesReached));
        SAFE_PRINTF("  typed:   %.2f%% (%llu bytes, with %llu free in MarkedBlocks)\n", reach.typedPercent(), static_cast<unsigned long long>(reach.bytesTyped), static_cast<unsigned long long>(reach.bytesFreeInBlocks));
        SAFE_PRINTF("  %llu JS cells read as their class, %llu bytes beyond their class; %llu global variables, %llu untyped data symbols\n",
            static_cast<unsigned long long>(reach.cellsWithClass), static_cast<unsigned long long>(reach.cellBytesBeyondClass),
            static_cast<unsigned long long>(reach.globalVariables), static_cast<unsigned long long>(reach.untypedDataSymbols));
        SAFE_PRINTF("  time: %.0f ms to open the debug info, %.0f ms to walk the cells, %.0f ms to enumerate libpas, %.0f ms to reach, type and explain; %.0f ms in all\n",
            (opened - start).milliseconds(), (walked - opened).milliseconds(), (enumerated - walked).milliseconds(), (reached - enumerated).milliseconds(), (reached - start).milliseconds());
        constexpr std::array<ASCIILiteral, HeapWalk::numberOfNotFollowedReasons> reasons {
            "pointers to declarations"_s, "pointers to types without a size"_s, "polymorphic pointees without a dynamic type"_s,
            "JS cells without a class"_s, "encoded pointers to types not reached"_s, "pointers to types that do not fit their allocation"_s,
        };
        for (size_t index = 0; index < reasons.size(); ++index)
            SAFE_PRINTF("  not followed: %llu %s\n", static_cast<unsigned long long>(reach.notFollowed[index]), reasons[index]);
        constexpr std::array<ASCIILiteral, HeapWalk::numberOfMissCauses> causes {
            "held by a reached allocation"_s, "held by static data"_s, "held by other memory"_s, "held by other misses"_s, "held by nothing"_s,
        };
        for (size_t index = 0; index < causes.size(); ++index)
            SAFE_PRINTF("  missed: %llu bytes %s\n", static_cast<unsigned long long>(reach.bytesMissedByCause[index]), causes[index]);
        size_t listed = 0;
        for (const HeapWalk::Miss& miss : reach.misses) {
            if (listed++ == listCount)
                break;
            SAFE_PRINTF("  miss: %llu bytes at 0x%llx, %llu with what only it holds, %s: %s%s%s\n", static_cast<unsigned long long>(miss.allocation.size),
                static_cast<unsigned long long>(miss.allocation.address.toTargetVMAddress()), static_cast<unsigned long long>(miss.bytesHeld),
                causes[static_cast<size_t>(miss.cause)], string(miss.holder), miss.content.isEmpty() ? ""_s : "; it holds "_s, string(miss.content));
        }
        for (auto& [kind, bytes] : reach.untypedByKind)
            SAFE_PRINTF("  untyped bytes: %llu in allocations with %s\n", static_cast<unsigned long long>(bytes), string(kind));
        for (auto& [name, bytes] : reach.cellBytesBeyondClassByClass)
            SAFE_PRINTF("  beyond class: %llu bytes of '%s' cells\n", static_cast<unsigned long long>(bytes), string(name));
        for (const HeapWalk::Untyped& untyped : reach.mostUntyped) {
            SAFE_PRINTF("  untyped: %llu of %llu bytes at 0x%llx, read as %s, reached by %s\n", static_cast<unsigned long long>(untyped.bytesUntyped), static_cast<unsigned long long>(untyped.allocation.size),
                static_cast<unsigned long long>(untyped.allocation.address.toTargetVMAddress()), string(untyped.typeAtStart.isNull() ? String("nothing at its start"_s) : untyped.typeAtStart),
                string(untyped.reachedBy));
        }
        SAFE_PRINTF("  %zu values read as a type bigger than their allocation\n", reach.overruns.size());
        // The same overrun at many addresses is one kind: its text without its numbers.
        HashMap<String, std::pair<uint64_t, String>> overrunKinds;
        for (const String& overrun : reach.overruns) {
            StringBuilder kind;
            for (unsigned index = 0; index < overrun.length();) {
                if (overrun.substring(index, 2) == "0x"_s) {
                    for (index += 2; index < overrun.length() && isASCIIHexDigit(overrun[index]); ++index) { }
                    kind.append('#');
                } else if (isASCIIDigit(overrun[index])) {
                    for (; index < overrun.length() && isASCIIDigit(overrun[index]); ++index) { }
                    kind.append('#');
                } else
                    kind.append(overrun[index++]);
            }
            auto& entry = overrunKinds.add(kind.toString(), std::pair<uint64_t, String> { 0, overrun }).iterator->value;
            ++entry.first;
        }
        Vector<std::pair<uint64_t, String>> kinds;
        for (auto& [kind, entry] : overrunKinds)
            kinds.append(entry);
        std::ranges::sort(kinds, std::ranges::greater { }, &std::pair<uint64_t, String>::first);
        for (size_t index = 0; index < kinds.size() && index < listCount; ++index)
            SAFE_PRINTF("  overrun, %llu like: %s\n", static_cast<unsigned long long>(kinds[index].first), string(kinds[index].second));
        SAFE_PRINTF("  %zu arrays declared past the readable memory they start in, left unread\n", reach.unreadableArrays.size());
        for (size_t index = 0; index < reach.unreadableArrays.size() && index < listCount; ++index)
            SAFE_PRINTF("  unreadable array: %s\n", string(reach.unreadableArrays[index]));
        SAFE_PRINTF("  %zu unions whose live member no reader picks, left unread\n", reach.unreadUnions.size());
        for (size_t index = 0; index < reach.unreadUnions.size() && index < listCount; ++index)
            SAFE_PRINTF("  unread union: %llu of %s\n", static_cast<unsigned long long>(reach.unreadUnions[index].second), string(reach.unreadUnions[index].first));
    }

    // Dispatches `p[/<format>] <expression>`. The only expression understood so
    // far is `&<symbol>`, which resolves the symbol in the snapshot in use.
    // `format` is the text after the '/', empty when none was given.
    void handlePrint(std::string_view format, Lexer lex)
    {
        bool hex = false;
        if (!format.empty()) {
            if (format == "x")
                hex = true;
            else if (format != "d") {
                fprintf(stderr, "mya: Unknown print format '%.*s'; use x or d\n",
                    static_cast<int>(format.length()), format.data());
                return;
            }
        }

        Snapshot* snapshot = snapshotById(m_currentSnapshot);
        if (!snapshot) {
            fputs("No snapshot in use. Capture one with `snapshot`, or select one with `snapshot <n>`.\n", stderr);
            return;
        }

        // Taking a symbol's address is all we can do without type information.
        if (!lex.consumeChar('&')) {
            fputs("Usage: p[/x] &<symbol>\n", stderr);
            return;
        }
        std::string_view token = lex.nextToken();
        if (token.empty() || !lex.atEnd()) {
            fputs("Usage: p[/x] &<symbol>\n", stderr);
            return;
        }

        CString name = UTF8CString { byteCast<char8_t>(std::span { token }) };
        Address address = snapshot->symbol(name.legacyCStringPointer());
        if (!address)
            return;
        if (hex)
            SAFE_PRINTF("&%s = 0x%llx\n", name, static_cast<unsigned long long>(address.toTargetVMAddress()));
        else
            SAFE_PRINTF("&%s = %llu\n", name, static_cast<unsigned long long>(address.toTargetVMAddress()));
    }

    // Releases resources without extra output. Dropping the current selection
    // and clearing the containers runs ~Process / ~Snapshot, which release the
    // task and corpse ports. Idempotent: safe from the quit path and destructor.
    void cleanup()
    {
        m_currentProcess = nullptr;
        m_processes.clear();
        // Unlink the non-owning list before destroying the Snapshots it points at.
        m_snapshots.clear();
        m_snapshotsById.clear();
        if (m_historyFile) {
            fclose(m_historyFile);
            m_historyFile = nullptr;
        }
        if (m_historyDirDescriptor >= 0) {
            close(m_historyDirDescriptor);
            m_historyDirDescriptor = -1;
        }
    }

    // Prompts for confirmation before quitting. Enter (empty) defaults to yes.
    bool confirmQuit()
    {
        for (;;) {
            std::string response;
#if HAVE(READLINE)
            char* input = readline("Really quit? [Y/n] ");
            if (!input) {
                putchar('\n');
                return true; // EOF: treat as yes.
            }
            response = input;
            free(input);
#else
            fputs("Really quit? [Y/n] ", stdout);
            fflush(stdout);
            char buffer[64];
            if (!fgets(buffer, sizeof(buffer), stdin)) {
                putchar('\n');
                return true; // EOF: treat as yes.
            }
            // Without a newline the answer was longer than the buffer, and the rest
            // would be read as the answer to the next prompt. Discard it.
            if (!std::string_view(buffer).contains('\n')) {
                int discarded = 0;
                while ((discarded = getchar()) != '\n' && discarded != EOF) { }
            }
            response = buffer;
#endif
            size_t start = 0;
            while (start < response.size() && isASCIIWhitespace(static_cast<unsigned char>(response[start])))
                ++start;
            size_t stop = response.size();
            while (stop > start && isASCIIWhitespace(static_cast<unsigned char>(response[stop - 1])))
                --stop;
            response = response.substr(start, stop - start);

            if (response.empty() || response[0] == 'y' || response[0] == 'Y')
                return true;
            if (response[0] == 'n' || response[0] == 'N')
                return false;
            fputs("Please answer 'y' or 'n'.\n", stdout);
        }
    }

    void printStatus()
    {
        if (m_currentProcess)
            printf("Attached to pid %d\n", static_cast<int>(m_currentProcess->pid()));
        else
            fputs("Not attached to any process.\n", stdout);

        if (Snapshot* snapshot = snapshotById(m_currentSnapshot))
            printf("Using snapshot %u of pid %d\n", snapshot->id(), static_cast<int>(snapshot->process()->pid()));
        else
            fputs("No snapshot in use.\n", stdout);
    }

    void printHistory()
    {
        if (!m_history.size()) {
            printf("History is empty.\n");
            return;
        }
        for (size_t i = 0; i < m_history.size(); ++i)
            printf("%5zu  %s\n", i + 1, m_history[i].c_str());
    }

    // Drops the `count` oldest entries.
    void clearHistory(unsigned count)
    {
        if (!count) {
            fputs("Nothing to do for clearing 0 history entries.\n", stdout);
            return;
        }
        if (m_history.empty()) {
            fputs("History is already empty.\n", stdout);
            return;
        }
        unsigned removeCount = count >= m_history.size() ? safeCast<unsigned>(m_history.size()) : count;
        m_history.erase(m_history.begin(), m_history.begin() + removeCount);
#if HAVE(READLINE)
        // readline has no way to drop individual entries, so rebuild its history
        // from the cache to keep the arrow keys in sync.
        clear_history();
        for (const std::string& command : m_history)
            add_history(command.c_str());
#endif
        if (m_historyFile && !rewriteHistoryFile()) {
            fputs("mya: Failed to clear history file.\n", stderr);
            return;
        }
        if (m_history.empty()) {
            if (removeCount == 1)
                printf("Cleared 1 history entry.\n");
            else
                printf("Cleared %u history entries.\n", removeCount);
        } else if (removeCount == 1)
            printf("Cleared the oldest history entry.\n");
        else
            printf("Cleared the %u oldest history entries.\n", removeCount);
    }

    void printHistorySize()
    {
        printf("History holds %zu of %u entries.\n", m_history.size(), m_maxHistoryEntries);
    }

    // Sets how many entries the history keeps, purging the oldest if the new
    // capacity is smaller than what is currently stored.
    void setMaxHistorySize(unsigned capacity)
    {
        if (capacity < minHistorySize) {
            capacity = minHistorySize;
            printf("Minimum history size is %u.\n", minHistorySize);
        }
        if (m_maxHistoryEntries == capacity) {
            printf("Maximum history size is already %u.\n", m_maxHistoryEntries);
            return;
        }
        m_maxHistoryEntries = capacity;
        boundReadlineHistory();
        if (m_history.size() > m_maxHistoryEntries) {
            m_history.erase(m_history.begin(), m_history.end() - m_maxHistoryEntries);
#if HAVE(READLINE)
            clear_history();
            for (const std::string& command : m_history)
                add_history(command.c_str());
#endif
        }
        if (!rewriteHistoryFile())
            fputs("mya: The new size applies to this session only.\n", stderr);
        printHistorySize();
    }

    // Dispatches the `history ...` subcommands. `lex` is positioned after the
    // "history" command word.
    void handleHistory(Lexer lex)
    {
        if (lex.atEnd()) {
            printHistory();
            return;
        }
        if (lex.consumeToken("clear")) {
            unsigned count = UINT_MAX; // Default to  "all".
            if (!lex.atEnd()) {
                unsigned parsed = 0;
                if (!lex.consumeUint32(parsed) || !lex.atEnd()) {
                    fputs("Usage: history clear [<n>]\n", stderr);
                    return;
                }
                count = parsed;
            }
            clearHistory(count);
            return;
        }
        if (lex.consumeToken("size")) {
            if (lex.atEnd()) {
                printHistorySize();
                return;
            }
            unsigned capacity = 0;
            if (!lex.consumeUint32(capacity) || !lex.atEnd()) {
                fputs("Usage: history size [<n>]\n", stderr);
                return;
            }
            setMaxHistorySize(capacity);
            return;
        }
        std::string_view token = lex.nextToken();
        fprintf(stderr, "mya: Unknown history subcommand '%.*s'\n",
            static_cast<int>(token.length()), token.data());
    }

    // Resolves a history reference ("!!" or "!<n>") to a stored command and
    // replays it. `lex` is positioned after the leading '!'; `line` is the whole
    // input, used for error reporting.
    void replayHistory(const char* line, Lexer lex)
    {
        std::string command;
        if (lex.consumeChar('!') && lex.atEnd()) {
            if (m_history.empty()) {
                fputs("mya: No commands in history\n", stderr);
                return;
            }
            command = m_history.back();
        } else {
            unsigned index = 0;
            if (!lex.consumeUint32(index) || !lex.atEnd() || index > m_history.size()) {
                fprintf(stderr, "mya: %s: event not found\n", line);
                return;
            }
            if (!index) {
                fprintf(stderr, "mya: %s: invalid history entry\n", line);
                return;
            }
            command = m_history[index - 1];
        }
        // Echo the resolved command, then run it as if it had been typed. The
        // replayed command records itself; the "!" reference is not recorded.
        printf("%s\n", command.c_str());
        handleLine(command.c_str());
    }

    static bool isRunningAsRoot() { return !geteuid(); }

    // The directory that holds the history file, empty if the user has no home
    // directory to put it in.
    //
    // We deliberately keep root (when run with sudo)'s history file distinct from
    // the non-root user's. This is better for security (root is not dependent on
    // non-root user data), and does not block the non-root user from accessing
    // their history if the last mya run was via sudo and the history file was
    // updated by root (and ownership changed).
    static std::string historyDirectory()
    {
        const char* home = nullptr;
        if (isRunningAsRoot()) {
            if (const struct passwd* entry = getpwuid(0))
                home = entry->pw_dir;
        } else {
            home = getenv("HOME");
            if (!home || !*home) {
                if (const struct passwd* entry = getpwuid(getuid()))
                    home = entry->pw_dir;
            }
        }
        if (!home || !*home)
            return { };

        std::string directory = home;
        if (directory.back() != '/')
            directory += '/';
        directory += historyDirName;
        return directory;
    }

    void boundReadlineHistory()
    {
#if HAVE(READLINE)
        // libedit only applies the bound when an entry is added, so lowering it does
        // not shorten the existing list: callers that shrink the cache must rebuild
        // readline's list as well for the change to take effect immediately.
        stifle_history(safeCast<int>(m_maxHistoryEntries));
#endif
    }

    // Opens the history file for read+write, creating it if absent, and loads any
    // stored commands into the cache. If it cannot be opened, the cache stays in
    // memory only for the session.
    //
    // The file lives under the user's home directory, not the working directory:
    // mya carries a debugger entitlement and its own usage suggests running it as
    // root, so it must not be steered into writing through a path controlled by
    // whoever owns the directory it happens to be started in. Both path
    // components are opened O_NOFOLLOW, so a symlink planted at either one is
    // refused rather than followed, and the file is never opened with O_TRUNC.
    void openHistory()
    {
        boundReadlineHistory();

        std::string directory = historyDirectory();
        if (directory.empty()) {
            fputs("mya: No home directory, so command history will not be saved.\n", stderr);
            return;
        }

        if (isRunningAsRoot()) {
            fprintf(stderr, "mya: Running as root: using history file %s/%s.\n",
                directory.c_str(), historyFileName);
        }

        if (mkdir(directory.c_str(), 0700) && errno != EEXIST) {
            fprintf(stderr, "mya: Could not create %s: %s\n", directory.c_str(), strerror(errno));
            return;
        }

        int directoryDescriptor = open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (directoryDescriptor < 0) {
            fprintf(stderr, "mya: Could not open %s: %s\n", directory.c_str(), strerror(errno));
            return;
        }

        int fileDescriptor = openat(directoryDescriptor, historyFileName, O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
        if (fileDescriptor < 0) {
            fprintf(stderr, "mya: Could not open %s/%s: %s\n", directory.c_str(), historyFileName, strerror(errno));
            close(directoryDescriptor);
            return;
        }

        // Anything other than a regular file is not something mya wrote: reading a
        // FIFO here would block the shell before it ever prompted. So, we decline to open
        // any non-regular files.
        struct stat status;
        if (fstat(fileDescriptor, &status) || !S_ISREG(status.st_mode)) {
            fprintf(stderr, "mya: %s/%s is not a regular file, so command history will not be saved.\n",
                directory.c_str(), historyFileName);
            close(fileDescriptor);
            close(directoryDescriptor);
            return;
        }

        m_historyFile = fdopen(fileDescriptor, "r+");
        if (!m_historyFile) {
            close(fileDescriptor);
            close(directoryDescriptor);
            return;
        }

        // Held for the session: rewriting the file creates and renames through this
        // descriptor, so the replacement lands in the directory that was checked
        // here rather than wherever the path may point by then.
        m_historyDirDescriptor = directoryDescriptor;

        loadHistory();
    }

    // Reads the cap out of a "max entries <n>" header line. Returns 0 if `line` does not
    // contain the header, which means the file is corrupted.
    static unsigned parseMaxEntriesHeader(const char* line)
    {
        size_t prefixLength = strlen(maxEntriesHeaderPrefix);
        if (!std::string_view(line).starts_with(maxEntriesHeaderPrefix))
            return 0;
        Lexer lex(line + prefixLength);
        unsigned entries = 0;
        if (!lex.consumeUint32<1>(entries) || !lex.atEnd())
            return 0;
        // We deliberately allow reading a capacity value below minHistorySize so that we can
        // print a meaningful error message about it in the caller.
        return entries;
    }

    void loadHistory()
    {
        if (!m_historyFile)
            return;
        rewind(m_historyFile);

        char buffer[4096];
        unsigned capacity = 0;
        if (fgets(buffer, sizeof(buffer), m_historyFile)) {
            buffer[strcspn(buffer, "\n")] = '\0';
            capacity = parseMaxEntriesHeader(buffer);
            if (!capacity) {
                fprintf(stderr, "mya: Corrupted file: ~/%s/%s does not start with a valid header (\"%s<n>\"); starting a new one.\n",
                    historyDirName, historyFileName, maxEntriesHeaderPrefix);
            } else if (capacity < minHistorySize) {
                fprintf(stderr, "mya: Corrupted file: ~/%s/%s header asks for fewer than the minimum %u entries;"
                    " starting a new one.\n", historyDirName, historyFileName, minHistorySize);
                capacity = 0; // Treat as error.
            }
        }
        if (!capacity) {
            rewriteHistoryFile(); // Invalid header. Reset the history file.
            return;
        }
        m_maxHistoryEntries = capacity;
        boundReadlineHistory();

        Vector<std::string> entries;
        while (fgets(buffer, sizeof(buffer), m_historyFile)) {
            buffer[strcspn(buffer, "\n")] = '\0';
            if (!buffer[0])
                continue;
            if (isReplayCommand(buffer))
                continue; // A ! replay command in history is invalid and not allowed. Skip.
            entries.append(buffer);
        }
        m_entriesInHistoryFile = safeCast<unsigned>(entries.size());

        // The cache never holds more than the capacity, however much the file holds.
        unsigned keep = std::min(m_entriesInHistoryFile, m_maxHistoryEntries);
        for (size_t i = entries.size() - keep; i < entries.size(); ++i) {
            m_history.push_back(entries[i]);
#if HAVE(READLINE)
            add_history(entries[i].c_str());
#endif
        }

        // Seek to the end so later commands append, and to satisfy the C rule
        // that a positioning call separates a read from a following write.
        if (fseek(m_historyFile, 0, SEEK_END))
            fallBackToMemoryOnly("Could not read the history file", errno);
    }

    // Report the failure condition and switch to in-memory cache only history.
    // The history file itself is left exactly as it was, and whatever was already
    // read from it stays in the cache.
    void fallBackToMemoryOnly(const char* what, int error)
    {
        fprintf(stderr, "mya: %s: %s\n", what, strerror(error));
        fputs("mya: Command history is kept in memory only from here, and the saved"
            " history is left as it is.\n", stderr);
        if (m_historyFile) {
            fclose(m_historyFile);
            m_historyFile = nullptr;
        }
        if (m_historyDirDescriptor >= 0) {
            close(m_historyDirDescriptor);
            m_historyDirDescriptor = -1;
        }
    }

    // Replaces the history file as a transaction i.e. the file either has the new
    // history or remains the old one if something went wrong. It is never left half
    // modified. This is done by writing the new file completely before replacing the
    // old history file with it.
    //
    // In the event something went wrong, the in-memory cache retains its state, and
    // may become out of sync with the history file.
    bool rewriteHistoryFile()
    {
        if (!m_historyFile || m_historyDirDescriptor < 0)
            return false;

        // Qualified by pid so two mya instances cannot land on the same temporary.
        char temporaryName[64];
        snprintf(temporaryName, sizeof(temporaryName), "%s.%d.tmp", historyFileName,
            static_cast<int>(getpid()));

        int descriptor = openat(m_historyDirDescriptor, temporaryName,
            O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
        if (descriptor < 0 && errno == EEXIST) {
            // Left behind by a run that died between creating and renaming.
            unlinkat(m_historyDirDescriptor, temporaryName, 0);
            descriptor = openat(m_historyDirDescriptor, temporaryName,
                O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
        }
        if (descriptor < 0) {
            fallBackToMemoryOnly("Could not create a temporary history file", errno);
            return false;
        }

        FILE* replacement = fdopen(descriptor, "w+");
        if (!replacement) {
            int error = errno;
            close(descriptor);
            unlinkat(m_historyDirDescriptor, temporaryName, 0);
            fallBackToMemoryOnly("Could not rewrite the history file", error);
            return false;
        }

        fprintf(replacement, "%s%u\n", maxEntriesHeaderPrefix, m_maxHistoryEntries);
        for (const std::string& command : m_history)
            fprintf(replacement, "%s\n", command.c_str());

        // Commit the contents before publishing them, so a crash cannot leave the
        // rename pointing at a file that was never written.
        int error = 0;
        if (fflush(replacement) || fsync(fileno(replacement)))
            error = errno;
        else if (ferror(replacement))
            error = EIO;
        if (!error && renameat(m_historyDirDescriptor, temporaryName, m_historyDirDescriptor, historyFileName))
            error = errno;
        if (error) {
            fclose(replacement);
            unlinkat(m_historyDirDescriptor, temporaryName, 0);
            fallBackToMemoryOnly("Could not rewrite the history file", error);
            return false;
        }

        // The rename published the temporary file, so it is the history file now.
        fclose(m_historyFile);
        m_historyFile = replacement;
        m_entriesInHistoryFile = safeCast<unsigned>(m_history.size());
        if (fseek(m_historyFile, 0, SEEK_END)) {
            fallBackToMemoryOnly("Could not rewrite the history file", errno);
            return false;
        }
        return true;
    }

    static bool isReplayCommand(const char* line)
    {
        Lexer lex(line);
        return lex.peek() == '!';
    }

    void recordCommand(const char* line)
    {
        // Do not allow replay commands in the history. They just pollute the history, and
        // add recursion complexities in the replay execution code, which we want to prevent.
        if (isReplayCommand(line))
            return;

        // Only filter an immediate repeat of the previous command.
        if (!m_history.empty() && m_history.back() == line)
            return;

        m_history.push_back(line);
#if HAVE(READLINE)
        add_history(line);
#endif
        if (m_history.size() > m_maxHistoryEntries)
            m_history.erase(m_history.begin());

        if (!m_historyFile)
            return;

        fprintf(m_historyFile, "%s\n", line);
        int error = 0;
        if (fflush(m_historyFile))
            error = errno;
        else if (ferror(m_historyFile))
            error = EIO;
        if (error) {
            fallBackToMemoryOnly("Could not append to the history file", error);
            return;
        }
        ++m_entriesInHistoryFile;
        // The sum cannot overflow: the capacity only ever comes from Lexer::consumeUint32,
        // which rejects anything above INT_MAX, leaving room for the overflow
        // allowance on top.
        if (m_entriesInHistoryFile >= m_maxHistoryEntries + maxOverflowEntries)
            rewriteHistoryFile();
    }

    void handleLine(const char* line)
    {
        Lexer lex(line);
        if (lex.atEnd())
            return; // Blank line: not a command, not an error.
        CORPSE_DIAGNOSTICS(diagnostics, "running the command '%s'", line);

        // History replay ("!!" / "!<n>") is expanded before command dispatch.
        if (lex.consumeChar('!'))
            return replayHistory(line, lex);

        std::string_view word = lex.nextToken();
        // `p` takes an lldb-style format suffix, as in "p/x", so the command and
        // its format arrive as one token.
        std::string_view command = word;
        std::string_view format;
        if (size_t slash = word.find('/'); slash != std::string_view::npos) {
            command = word.substr(0, slash);
            format = word.substr(slash + 1);
        }

        auto is = [&](const char* name) {
            return word == name;
        };

        if (is("help")) {
            handleHelp(lex);
            return;
        }
        if (is("q") || is("quit") || is("exit")) {
            m_isQuitting = confirmQuit();
            return;
        }
        if (is("st") || is("status")) {
            recordCommand(line);
            printStatus();
            return;
        }
        if (is("hi") || is("hist") || is("history")) {
            // A bare `history` only lists the history. Recording it would make
            // the last entry of every listing be the command that asked for it.
            if (!lex.atEnd())
                recordCommand(line);
            handleHistory(lex);
            return;
        }
        if (is("detach")) {
            recordCommand(line);
            detach();
            return;
        }
        if (is("attach")) {
            recordCommand(line);
            // Optional "--pid"/"-p" before the number: "attach --pid 42" == "attach 42".
            if (!lex.consumeToken("--pid"))
                lex.consumeToken("-p");
            pid_t pid = -1;
            if (!lex.consumePID(pid) || !lex.atEnd()) {
                fputs("Usage: attach [--pid|-p] <pid>\n", stderr);
                return;
            }
            attach(pid);
            return;
        }
        if (is("sn") || is("snap") || is("snapshot")) {
            recordCommand(line);
            handleSnapshot(lex);
            return;
        }
        if (is("th") || is("thread")) {
            recordCommand(line);
            handleThread(lex);
            return;
        }
        if (is("heap")) {
            recordCommand(line);
            handleHeap(lex);
            return;
        }
        if (is("heapdump")) {
            recordCommand(line);
            handleHeapDump(lex);
            return;
        }
#if OS(DARWIN)
        if (is("memory")) {
            recordCommand(line);
            handleMemory(lex);
            return;
        }
#endif
        if (command == "p" || command == "print") {
            recordCommand(line);
            handlePrint(format, lex);
            return;
        }

        recordCommand(line);
        fprintf(stderr, "mya: Unknown command '%.*s'\n", static_cast<int>(word.length()), word.data());
    }

    void runInteractive()
    {
#if HAVE(READLINE)
        for (;;) {
            char* line = readline(prompt);
            if (!line) {
                putchar('\n');
                break;
            }
            handleLine(line);
            free(line);
            if (m_isQuitting)
                break;
        }
#else
        char line[4096];
        fputs(prompt, stdout);
        fflush(stdout);
        while (fgets(line, sizeof(line), stdin)) {
            line[strcspn(line, "\n")] = '\0';
            handleLine(line);
            if (m_isQuitting)
                break;
            fputs(prompt, stdout);
            fflush(stdout);
        }
        putchar('\n');
#endif
        cleanup(); // About to quit.
    }

    std::vector<std::string> m_history;
    unsigned m_maxHistoryEntries { defaultMaxHistoryEntries };
    unsigned m_entriesInHistoryFile { 0 }; // Actual number of commands in the file (may exceed target capacity).
    FILE* m_historyFile { nullptr };
    int m_historyDirDescriptor { -1 };
    HashMap<pid_t, RefPtr<Process>> m_processes;
    // m_snapshotsById owns the Snapshots; m_snapshots only records capture order.
    HashMap<unsigned, std::unique_ptr<Snapshot>> m_snapshotsById;
    DoublyLinkedList<Snapshot> m_snapshots;
    RefPtr<Process> m_currentProcess;
    unsigned m_currentSnapshot { 0 }; // Snapshot id in use; 0 means none.
    bool m_isQuitting { false };
};

} // namespace Mya

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(MYA)

int main(int argc, char** argv)
{
#if ENABLE(MYA)
    return Mya::Shell().run(argc, argv);
#else
    UNUSED_PARAM(argc);
    UNUSED_PARAM(argv);
    SAFE_PRINTF("Not supported platform for mya\n");
    return 1;
#endif // ENABLE(MYA)
}
