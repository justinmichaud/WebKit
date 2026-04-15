/*
 * Copyright (C) 2026 Igalia S.L.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public License
 * along with this library; see the file COPYING.LIB.  If not, write to
 * the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

#pragma once

#include <wtf/Platform.h>

#if ENABLE(REFTRACKER) && __has_include(<meta>)

namespace WTF {

// Empty polymorphic base for NativeHeapFinder<...> so dispatchers can accept
// a non-templated reference and downcast back to the concrete walker type
// when they know the Walker template parameter at dispatch time.
//
// Option 1 from the plan: the dispatcher is itself a template on the walker
// type, so it knows the full concrete NativeHeapFinder<...> and can call the
// statically-typed trampoline directly via a downcast. No virtual indirection
// on the hot path; the only virtual call is CellDispatcher::tryDispatch.
class NativeHeapFinderBase {
public:
    virtual ~NativeHeapFinderBase() = default;
};

// Polymorphic dispatcher for type-erased JSCell*. The walker holds a span of
// these and, when it encounters a value whose static type derives from
// JSC::JSCell, queries each dispatcher in order. The first one that
// recognizes the cell's classInfo re-enters the walker with the correct
// concrete type so reflection walks the full subclass layout (including any
// base-class subobjects such as JSGlobalObject::m_inspectorController).
//
// The walker (WTF) stays ignorant of JSC::JSCell: `cellVoid` is a
// `JSC::JSCell*` cast to `void*` by the caller; the derived dispatcher casts
// it back. `visitor` is a `NativeHeapFinder<...>&` erased through
// `NativeHeapFinderBase&`; the derived dispatcher downcasts using its own
// Walker template parameter.
class CellDispatcher {
public:
    virtual ~CellDispatcher() = default;

    // Returns true if this dispatcher recognized the cell's dynamic type and
    // re-enqueued it under the correct static type. False if no match — the
    // walker then either falls back to the base-class trampoline or tries
    // the next dispatcher in the chain.
    virtual bool tryDispatch(void* cellVoid, NativeHeapFinderBase& visitor) = 0;
};

} // namespace WTF

#endif // ENABLE(REFTRACKER) && __has_include(<meta>)
