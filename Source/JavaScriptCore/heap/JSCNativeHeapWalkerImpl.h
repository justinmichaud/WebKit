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

// Non-template entry points for the JSC native-heap walker.
//
// Implemented in JSCNativeHeapWalkerImpl.cpp, which includes the full
// definitions of every type that VM.h and JSGlobalObject.h only
// forward-declare.  All unique_ptr<X> / UniqueRef<X> members are
// therefore complete when the reflection walker's is_complete_type(^^X)
// check runs — nothing is silently skipped.
//
// Callers include this header and call the functions below.  They do
// NOT include TandemHeapAnalyzer.h directly; that way the walker
// template is only instantiated in the one TU that has everything.

#if ENABLE(REFTRACKER) && __has_include(<meta>)

namespace JSC {

class VM;
class JSGlobalObject;

// Run a tandem GC + reflection walk from `vm` and return the count of
// all reachable JSC::Strong<T> specialisations.
unsigned findLiveStrongCount(VM& vm);

// Like findLiveStrongCount, but logs each Strong<T>'s address and its
// allocation stack trace (if REFTRACKER was enabled at construction
// time) to dataLog.
void dumpAllStrongHandles(VM& vm);

// Diagnostic: walk from globalObject (no GC phase) and count Strong<>.
unsigned countStrongInGlobalObject(JSGlobalObject& globalObject);

// Diagnostic: walk from globalObject's inspector controller and count Strong<>.
unsigned countStrongInInspectorController(JSGlobalObject& globalObject);

} // namespace JSC

#endif // ENABLE(REFTRACKER) && __has_include(<meta>)
