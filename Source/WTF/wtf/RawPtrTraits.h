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

#pragma once

#include <cstdint>
#include <utility>
#include <wtf/StdLibExtras.h>

#if OS(LINUX)
extern "C" uintptr_t pas_page_malloc_constrained_base(void);
#endif

namespace WTF {

#if OS(LINUX)

template<typename T>
struct RawPtrTraits {
    template<typename U> using RebindTraits = RawPtrTraits<U>;

    using StorageType = uint32_t;

    static ALWAYS_INLINE uintptr_t base()
    {
        static uintptr_t cachedBase = 0;
        if (__builtin_expect(!cachedBase, 0))
            cachedBase = pas_page_malloc_constrained_base();
        return cachedBase;
    }

    static ALWAYS_INLINE StorageType compress(T* ptr)
    {
        if (!ptr)
            return 0;
        uintptr_t addr = reinterpret_cast<uintptr_t>(ptr);
        uintptr_t b = base();
        RELEASE_ASSERT(addr >= b && (addr - b) < static_cast<uintptr_t>(UINT32_MAX));
        return static_cast<uint32_t>(addr - b);
    }

    static ALWAYS_INLINE T* decompress(StorageType offset)
    {
        if (!offset)
            return nullptr;
        return reinterpret_cast<T*>(base() + offset);
    }

    template<typename U>
    static ALWAYS_INLINE T* exchange(StorageType& storage, U&& newValue)
    {
        T* old = decompress(storage);
        storage = compress(std::forward<U>(newValue));
        return old;
    }

    static ALWAYS_INLINE T* exchange(StorageType& storage, StorageType newValue)
    {
        T* old = decompress(storage);
        storage = newValue;
        return old;
    }

    static ALWAYS_INLINE T* exchange(StorageType& storage, std::nullptr_t)
    {
        T* old = decompress(storage);
        storage = 0;
        return old;
    }

    static ALWAYS_INLINE void swap(StorageType& a, StorageType& b) { std::swap(a, b); }
    static ALWAYS_INLINE T* unwrap(const StorageType& storage) { return decompress(storage); }

    static StorageType hashTableDeletedValue() { return UINT32_MAX; }
    static ALWAYS_INLINE bool isHashTableDeletedValue(const StorageType& storage) { return storage == UINT32_MAX; }
};

#else

template<typename T>
struct RawPtrTraits {
    template<typename U> using RebindTraits = RawPtrTraits<U>;

    using StorageType = T*;

    static ALWAYS_INLINE StorageType compress(T* ptr) { return ptr; }

    template<typename U>
    static ALWAYS_INLINE T* exchange(StorageType& ptr, U&& newValue) { return std::exchange(ptr, newValue); }

    static ALWAYS_INLINE void swap(StorageType& a, StorageType& b) { std::swap(a, b); }
    static ALWAYS_INLINE T* unwrap(const StorageType& ptr) { return ptr; }

    static StorageType hashTableDeletedValue() { return std::bit_cast<StorageType>(static_cast<uintptr_t>(-1)); }
    static ALWAYS_INLINE bool isHashTableDeletedValue(const StorageType& ptr) { return ptr == hashTableDeletedValue(); }
};

#endif

} // namespace WTF

using WTF::RawPtrTraits;
