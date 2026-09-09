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

#pragma once

#include <JavaScriptCore/CorpsePlatform.h>

#if HAVE(CORPSE_SUPPORT)

#include <JavaScriptCore/CorpseAddress.h>
#include <memory>
#include <optional>
#include <array>
#include <span>
#include <stdint.h>
#include <type_traits>
#include <wtf/StdLibExtras.h>
#include <wtf/Vector.h>
#include <wtf/text/CString.h>

namespace JSC {
namespace Corpse {

class Process;

// The register values a corpse carries for one thread. Only the registers a
// walk over the target needs are kept: the rest of a thread state is
// architecture-shaped, and nothing above this layer is.
//
// `isComplete` says the platform handed over a real register set. A thread
// whose registers could not be read is still reported, because knowing a thread
// exists and that its stack is unavailable is the diagnosis; silently dropping
// it is not.
struct ThreadRegisters {
    Address stackPointer;
    Address programCounter;
    Address framePointer;
    bool isComplete { false };

    explicit operator bool() const { return isComplete; }
};

// Why a thread's registers are missing, so a report can say which threads a
// walk cannot reach and what would be needed to reach them.
enum class RegisterFailure : uint8_t {
    None,
    Unsupported,      // This build cannot decode the target's architecture.
    PermissionDenied, // The OS refused: ptrace scope, entitlement, or capability.
    Translated,       // Rosetta: the state describes the translator, not the program.
    Unavailable,      // The OS reported no state for a thread that exists.
};

// One thread as the corpse describes it. Everything here is captured at the
// instant the corpse is taken, from the same stopped target the memory comes
// from, so a stack pointer always indexes memory of the same moment.
struct ThreadInfo {
    // A stable identifier for the thread. Darwin reports the kernel's 64-bit
    // system-unique thread id; Linux reports the TID, which is unique within
    // the target's PID namespace for the life of the thread.
    uint64_t id { 0 };

    CString name;
    ThreadRegisters registers;
    RegisterFailure registerFailure { RegisterFailure::None };

    // A small integer whose meaning is the platform's. Backend::describeRunState
    // turns one into text; nothing else should interpret it.
    int runState { 0 };
    int suspendCount { 0 };
    uint64_t userTimeUsec { 0 };
    uint64_t systemTimeUsec { 0 };
};

// One mapped range of the corpse's address space. Page accounting is optional
// because not every platform reports it for every mapping.
struct RegionInfo {
    Address base;
    uint64_t size { 0 };
    std::optional<uint64_t> residentPageCount;
    std::optional<uint64_t> dirtyPageCount;

    bool isReadable { false };
    bool isWritable { false };
    bool isExecutable { false };

    Address end() const { return base + size; }
    bool contains(Address address) const { return address >= base && address < end(); }
};

// One image mapped into the corpse.
struct ImageInfo {
    CString path;

    // How far the image's linked addresses were shifted when it loaded.
    uint64_t slide { 0 };

    // Where the image's header sits in the corpse. Null when the platform
    // reports a slide but not a header address.
    Address loadAddress;
};

// The platform half of a corpse: everything that has to know about Mach ports
// or /proc lives behind this interface, and nothing above it does.
//
// A Backend is created by capturing a target, which freezes the target's memory
// and reads its threads at one instant. Both happen in capture() so that they
// cannot describe different moments: a stack pointer read after the freeze
// would index memory that no longer matches it.
//
// Every accessor is const and side-effect free, and every read is bounded and
// reports failure rather than trapping, because a corpse's contents are not
// trusted: it is taken precisely when the target may be corrupt.
class Backend {
public:
    // Freezes `process` and reads its threads. Returns nullptr and reports why
    // if the platform cannot take a corpse of that target; callers degrade
    // rather than abort, since being unable to snapshot is a normal outcome.
    static std::unique_ptr<Backend> capture(Process&);

    virtual ~Backend() = default;

    Backend(const Backend&) = delete;
    Backend& operator=(const Backend&) = delete;

    // Copies `into.size()` bytes from `address`. Returns false on any short
    // copy, unmapped range, or refusal; `into` is then unspecified. This is the
    // one primitive every other read is built from.
    virtual bool read(Address, std::span<uint8_t> into) const = 0;

    // Reads one trivially-copyable value. False on any short read, leaving
    // `out` unspecified. Every structure read out of a corpse goes through
    // this, so no caller has to spell the cast or the size.
    template<typename T> bool readInto(Address address, T& out) const
    {
        static_assert(std::is_trivially_copyable_v<T>);
        return read(address, asMutableByteSpan(out));
    }

    // A NUL-terminated string out of the corpse, read in chunks so that a
    // pointer into nothing costs one failed read rather than a scan of the
    // address space. Null if the string does not end within `maxLength`, which
    // is what a pointer into arbitrary bytes looks like.
    CString readCString(Address address, size_t maxLength = 4 * KB) const
    {
        Vector<char> characters;
        std::array<uint8_t, 256> buffer;
        while (characters.size() < maxLength) {
            if (!read(address + characters.size(), std::span { buffer }))
                return { };
            for (uint8_t byte : buffer) {
                if (!byte)
                    return CString(characters.span());
                characters.append(static_cast<char>(byte));
            }
        }
        return { };
    }

    // The threads as they were when the corpse was taken. Captured once, so
    // repeated calls cost nothing and cannot disagree with each other.
    virtual const Vector<ThreadInfo>& threads() const = 0;

    // The corpse's mappings, in ascending address order.
    virtual Vector<RegionInfo> regions() const = 0;

    // The images mapped into the corpse.
    virtual Vector<ImageInfo> images() const = 0;

    // Text for a ThreadInfo::runState this backend produced.
    virtual const char* describeRunState(int) const = 0;

protected:
    Backend() = default;
};

} // namespace Corpse
} // namespace JSC

#endif // HAVE(CORPSE_SUPPORT)
