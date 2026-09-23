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

#include <wtf/Platform.h>

// Whether mya can say what a native object on the heap it reads is. That takes
// LLDB's SB API to read the target's debug info, and RTTI in the target itself so
// that an object carries the type_info identifying it. ENABLE_MYA_HEAP is the one
// that decides, and CorpseLLDB.h is what includes the SB API.
//
// A development build's, both of them: neither a dependency on LLDB nor the
// metadata RTTI emits belongs in a shipping binary.
//
// Xcode ships LLDB.framework with its headers under the internal SDK only, so a
// public Xcode takes them from an LLVM install: "brew install lldb" on macOS,
// liblldb-dev or lldb-devel on Linux. An xcconfig cannot ask whether a header is
// installed, so the probe is what narrows an Xcode build that has no SB API.
#if !defined(HAVE_MYA_HEAP)
#if ENABLE(MYA_HEAP) && (__has_include(<LLDB/LLDB.h>) || __has_include(<lldb/API/LLDB.h>))
#define HAVE_MYA_HEAP 1
#endif
#endif

// Whether mya is built here. Reading a process is Darwin's Mach task and corpse
// APIs so far; MacCatalyst and the simulators are out because task_for_pid is not
// usable there. Linux builds the same classes against stubs that attach to
// nothing, so that the shape the Linux implementation has to fill is already
// here and already compiled.
//
// FIXME: Read a process on Linux by stopping every thread through WTF's
// signal-based Thread::suspend(), forking, and reading the child.
// https://bugs.webkit.org/show_bug.cgi?id=324772
#if !defined(HAVE_MYA)
#if (OS(MACOS) || USE(APPLE_INTERNAL_SDK)) && !PLATFORM(MACCATALYST) && !PLATFORM(IOS_FAMILY_SIMULATOR)
#define HAVE_MYA 1
#elif OS(LINUX)
#define HAVE_MYA 1
#endif
#endif

#if HAVE(MYA)

#if OS(DARWIN)
#include <mach/mach.h>
#endif

namespace JSC {
namespace Corpse {

// What a target process is held by. A Mach task or corpse port on Darwin; on a
// platform whose implementation is still a stub there is nothing to hold, and
// every handle is the invalid one.
#if OS(DARWIN)
using TaskHandle = mach_port_t;
constexpr TaskHandle invalidTaskHandle = MACH_PORT_NULL;
inline bool isValidTaskHandle(TaskHandle handle) { return MACH_PORT_VALID(handle); }
#else
using TaskHandle = int;
constexpr TaskHandle invalidTaskHandle = -1;
inline bool isValidTaskHandle(TaskHandle handle) { return handle >= 0; }
#endif

} // namespace Corpse
} // namespace JSC

#endif // HAVE(MYA)
