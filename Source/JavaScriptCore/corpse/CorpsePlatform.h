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

// Whether a process can be read here, which is what mya is for. So far that
// means Darwin's Mach task and corpse APIs; MacCatalyst and the simulators are
// out because task_for_pid is not usable there. mya is built on Linux too, as
// the main that says so, so that the build is exercised.
//
// FIXME: Read a process on Linux by stopping every thread through WTF's
// signal-based Thread::suspend(), forking, and reading the child.
// https://bugs.webkit.org/show_bug.cgi?id=324772
#if !defined(HAVE_MYA)
#if (OS(MACOS) || USE(APPLE_INTERNAL_SDK)) && !PLATFORM(MACCATALYST) && !PLATFORM(IOS_FAMILY_SIMULATOR)
#define HAVE_MYA 1
#endif
#endif

// Whether mya can say what an address holds. That takes LLDB's SB API to read
// the target's debug info, and RTTI so that an object's own type_info is there
// to identify it by. CorpseLLDB.h is what includes the SB API.
//
// Debug builds only: a dependency on LLDB and the metadata RTTI emits are not
// things a shipping build carries. The build gives a debug build the SB API
// headers and -frtti, so anything missing here is a debug build that was set up
// wrong, and the typeinfo suite says so.
//
// Xcode ships LLDB.framework with its headers under the internal SDK only, so a
// public Xcode takes them from an LLVM install: "brew install lldb" on macOS,
// liblldb-dev or lldb-devel on Linux.
#if !defined(HAVE_MYA_TYPEINFO)
#if ASSERT_ENABLED \
    && (__has_include(<LLDB/LLDB.h>) || __has_include(<lldb/API/LLDB.h>)) \
    && (defined(__cpp_rtti) || defined(__GXX_RTTI))
#define HAVE_MYA_TYPEINFO 1
#endif
#endif
