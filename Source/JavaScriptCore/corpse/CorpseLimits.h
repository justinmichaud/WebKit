/*
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

#if ENABLE(MYA)

#include <stddef.h>
#include <stdint.h>
#include <wtf/StdLibExtras.h>

namespace JSC {
namespace Corpse {

// Every size and count read out of a corpse bounds a loop or an allocation, so
// it is checked against one of these first. Each limit is a plausibility check
// on a single value: exceeding it says the bytes read were not the struct they
// were taken for, so the addresses in them are not worth chasing. The values sit
// well above what was measured across every Mach-O image installed on a sample
// system, and in a process that dlopens every framework on it.

constexpr uint32_t maxImageCount = 16 * 1024; // About 6x the 2,800 images measured.
constexpr size_t maxPathLength = 4 * KB; // PATH_MAX on Darwin and on Linux.
constexpr size_t maxTypeNameLength = 4 * KB; // A mangled name out of a type_info.
constexpr size_t maxLoadCommandsSize = 128 * KB; // About 17x the 7.4 KB measured.
constexpr size_t maxExportsTrieSize = 16 * MB; // About 8x the 2.1 MB measured.

// The per-image limits multiply by the image count, so they do not bound the
// work of one symbol lookup. This does: a lookup that finds nothing reads every
// image's load commands and exports trie, which measured 101 MB for the 2,800
// image process and 0.4 MB for a small one.
constexpr size_t maxTotalBytesRead = 256 * MB; // About 2.5x the measured maximum.

// A JS heap holds a few hundred BlockDirectories, and no Vector walked out of
// it comes near these element counts.
constexpr unsigned maxBlockDirectories = 64 * 1024;
constexpr unsigned maxVectorSize = 16 * 1024 * 1024;

} // namespace Corpse
} // namespace JSC

#endif // ENABLE(MYA)
