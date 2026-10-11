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

#if ENABLE(LIB_JSC_TOOLS)

#include <stddef.h>
#include <stdint.h>
#include <wtf/StdLibExtras.h>

namespace JSC {
namespace Corpse {

// Sizes and counts read out of a corpse are used to bound loops and to size allocations, so they are
// checked against these limits first. See the limits in CorpseSymbol.cpp.
constexpr uint32_t maxImageCount = 16 * 1024; // About 6x the 2,800 images of a process that dlopens every framework.
constexpr size_t maxImagePathLength = 4 * KB; // Linux's PATH_MAX, 4x Darwin's.
constexpr size_t maxRegionCount = 1024 * 1024; // A browser process measured 12,375.
constexpr size_t maxVTableSize = 64 * KB; // A vtable group, _ZTV. JavaScriptCore's largest measured 1 KB.

} // namespace Corpse
} // namespace JSC

#endif // ENABLE(LIB_JSC_TOOLS)
