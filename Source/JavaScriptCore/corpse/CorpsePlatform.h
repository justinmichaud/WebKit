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

// Whether the corpse support can be built at all: the Mach task and corpse APIs
// on Darwin, /proc and a copy-on-write fork on Linux. Simulators and
// MacCatalyst are out because task_for_pid is not usable there.
#if !defined(HAVE_CORPSE_SUPPORT)
#if (OS(MACOS) || USE(APPLE_INTERNAL_SDK)) && !PLATFORM(MACCATALYST) && !PLATFORM(IOS_FAMILY_SIMULATOR)
#define HAVE_CORPSE_SUPPORT 1
#elif OS(LINUX) && (CPU(ARM64) || CPU(X86_64))
#define HAVE_CORPSE_SUPPORT 1
#endif
#endif

// Type introspection reads the target's debug info through LLDB's SB API, which
// is in the SDK on Darwin but has to be found by the build elsewhere.
//
// What an object actually is comes from its own type_info rather than from the
// debug info, so nothing beyond LLDB is needed here: the target is built with
// RTTI and says what it holds.
#if !defined(HAVE_CORPSE_TYPE_SYSTEM)
#if HAVE(CORPSE_SUPPORT) && (OS(MACOS) || defined(CORPSE_USE_LLDB))
#define HAVE_CORPSE_TYPE_SYSTEM 1
#endif
#endif

// The dyld exports trie is a Mach-O construct, so the code that walks one and
// the symbol lookup built on it exist only where there are Mach-O images.
#if !defined(HAVE_CORPSE_EXPORTS_TRIE)
#if HAVE(CORPSE_SUPPORT) && OS(DARWIN)
#define HAVE_CORPSE_EXPORTS_TRIE 1
#endif
#endif
