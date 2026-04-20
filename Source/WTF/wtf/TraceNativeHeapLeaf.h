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

namespace WTF {

#if ENABLE(REFTRACKER)

// Empty base class that marks a C++ type as a leaf in the native-heap walk.
//
// The walker (TraceNativeHeap.h) will NOT recurse into any instance of a type
// that inherits from TraceNativeHeapLeaf (directly or transitively).  Use it
// for types that hold raw GC-cell pointers (e.g. WriteBarrier<T>) where the
// native-heap walk must not follow those pointers — the GC phase of
// findAllOnNativeHeap handles GC objects separately.
//
// ── Why a base class rather than the refTrackerVisitTag sentinel? ──────────
//
// The refTrackerVisitTag + ADL-overload mechanism (see TraceNativeHeap.h) works
// reliably for concrete types but FAILS for class templates in Bloomberg's
// P2996 clang: function-template ADL overloads are not found when the call is
// reached transitively from inside a `template for` loop (compiler bug).  The
// fallback that scanned static_data_members_of(^^T) for "refTrackerVisitTag"
// was also unreliable: Bloomberg's static_data_members_of incorrectly returns
// static members from the types of T's instance fields, and parent_of() for
// those spurious entries returns the outer class rather than the declaring
// class (second compiler bug).
//
// std::is_base_of_v is pure type-trait machinery — no ADL, no reflection —
// and is immune to all of those bugs.
//
// ── Usage ──────────────────────────────────────────────────────────────────
//
// To mark a type (including class templates) as a native-heap-walk leaf:
//
//   #if ENABLE(REFTRACKER)
//   class MyBarrier : public WTF::TraceNativeHeapLeaf { ... };
//   #endif
//
// The inheritance must be UNCONDITIONAL within the REFTRACKER guard so that
// std::is_base_of_v<WTF::TraceNativeHeapLeaf, MyBarrier> evaluates to true
// at compile time.  No ADL overload or refTrackerVisitTag member is required
// (though they may be kept for documentation or for non-Bloomberg builds).
//
// Empty Base Optimization (EBO) ensures the base sub-object occupies no
// additional space, so sizeof(MyBarrier) is unchanged.  The type remains
// trivially constructible and trivially destructible, allowing it to be
// used as a union member.

struct TraceNativeHeapLeaf { };

#endif // ENABLE(REFTRACKER)

} // namespace WTF
