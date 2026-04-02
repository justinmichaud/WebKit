/*
 * Copyright (C) 2017-2018 Apple Inc. All rights reserved.
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
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include <wtf/RawPtrTraits.h>

#if OS(LINUX)

#include <wtf/Assertions.h>
#include <stdio.h>

static uintptr_t sideTableEntries[rawPtrSideTableMaxIndex];
static uint32_t sideTableCount = 0;

uint32_t rawPtrSideTableStore(uintptr_t ptr)
{
    for (uint32_t i = 0; i < sideTableCount; ++i) {
        if (sideTableEntries[i] == ptr)
            return rawPtrSideTableFirstSentinel + i * 2;
    }
    RELEASE_ASSERT(sideTableCount < rawPtrSideTableMaxIndex);
    uint32_t idx = sideTableCount++;
    sideTableEntries[idx] = ptr;
    return rawPtrSideTableFirstSentinel + idx * 2;
}

uintptr_t rawPtrSideTableLoad(uint32_t compressed)
{
    uint32_t idx = (compressed - rawPtrSideTableFirstSentinel) / 2;
    RELEASE_ASSERT(idx < sideTableCount);
    return sideTableEntries[idx];
}

#endif
