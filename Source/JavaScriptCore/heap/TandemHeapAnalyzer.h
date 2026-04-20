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

#if ENABLE(REFTRACKER) && __has_include(<meta>)

#include "Heap.h"
#include "HeapAnalyzer.h"
#include "HeapProfiler.h"
#include "JSCell.h"
#include "NamespaceCellDispatcher.h"
#include "VM.h"
#include <unordered_set>
#include <wtf/CellDispatcher.h>
#include <wtf/HashSet.h>
#include <wtf/Lock.h>
#include <wtf/TraceNativeHeap.h>
#include <wtf/Vector.h>

namespace JSC {

// Tandem GC + native-heap walker. Installed as a HeapAnalyzer before a
// synchronous full GC; receives analyzeNode/analyzeEdge callbacks from the
// GC worker threads during the mark phase and records every live JSCell
// into a shared seen-set. After the GC returns, the reflection walker runs
// on the same thread while a PreventCollectionScope is held, so no further
// collection can start until the walk finishes. The reflection walker
// short-circuits at JSCell* fields because their pointees are already in the
// seen-set, meaning the two walks compose into one: GC side covers
// cell -> cell edges, reflection side covers C++ -> C++, and JSCell* is the
// bridge.
//
// Callbacks are invoked from potentially multiple GC worker threads during
// phase 1 (under a Lock) and from the main thread during phase 2 (no lock).
// Callers must accept both contexts.

class TandemHeapAnalyzer final : public HeapAnalyzer {
public:
    using CellCallback = void(*)(void* userData, JSCell*);

    TandemHeapAnalyzer(std::unordered_set<const void*>& seen, Lock& lock,
                       CellCallback cellCallback, void* userData)
        : m_seen(seen)
        , m_lock(lock)
        , m_cellCallback(cellCallback)
        , m_userData(userData)
    {
    }

    void analyzeNode(JSCell*) final;
    void analyzeEdge(JSCell* from, JSCell* to, RootMarkReason) final;
    void analyzePropertyNameEdge(JSCell* from, JSCell* to, UniquedStringImpl*) final;
    void analyzeVariableNameEdge(JSCell* from, JSCell* to, UniquedStringImpl*) final;
    void analyzeIndexEdge(JSCell* from, JSCell* to, uint32_t) final;

    void setOpaqueRootReachabilityReasonForCell(JSCell*, ASCIILiteral) final { }
    void setWrappedObjectForCell(JSCell*, void*) final { }
    void setLabelForCell(JSCell*, const String&) final { }

private:
    void recordCell(JSCell*);

    std::unordered_set<const void*>& m_seen;
    Lock& m_lock;
    CellCallback m_cellCallback;
    void* m_userData;
};

// The user-facing entry point. Runs a tandem GC + native heap walk and
// invokes `callback` for every reachable specialization of `TargetTemplate`
// found on the C++ side.
//
// Preconditions (release-asserted):
//   - the current thread holds the VM's API lock (single-mutator exclusion)
//   - vm.heap.isSafeToCollect() is true
//   - no other HeapAnalyzer is currently installed
template<auto TargetTemplate, bool verbose = false, typename Root, typename Callback>
void findAllOnNativeHeap(VM& vm, Root& root, Callback&& callback)
{
    RELEASE_ASSERT(vm.currentThreadIsHoldingAPILock());
    RELEASE_ASSERT(vm.heap.isSafeToCollect());
    HeapProfiler& profiler = vm.ensureHeapProfiler();
    RELEASE_ASSERT(!profiler.activeHeapAnalyzer());

    std::unordered_set<const void*> seen;
    Lock seenLock;

    using DecayedCallback = std::decay_t<Callback>;
    DecayedCallback cb { std::forward<Callback>(callback) };

    // The analyzer records every live JSCell into `seen`; the user callback
    // is for C++ types only (matched by the reflection walker), so the
    // analyzer does not call into it. The JSCell-subclass side now has its
    // own classInfo-based match via the per-call NamespaceCellDispatcher.
    TandemHeapAnalyzer analyzer {
        seen,
        seenLock,
        /* cellCallback */ nullptr,
        /* userData */ nullptr
    };

    // Build the per-call dispatcher chain. JSC's own dispatcher is
    // constructed fresh here because it's templated on the concrete walker
    // type, so the trampoline it installs is statically typed. Future
    // upper-layer dispatchers (WebCore, WebKit) will be registered into
    // vm.cellDispatchers() and appended here.
    using WalkerT = WTF::NativeHeapFinder<TargetTemplate, verbose, DecayedCallback>;
    JSC::NamespaceCellDispatcher<^^JSC, WalkerT> jscDispatcher;
    Vector<WTF::CellDispatcher*, 4> dispatchers;
    dispatchers.append(&jscDispatcher);

    // Phase 1: run a synchronous full GC with the analyzer installed.
    // The caller already holds the API lock (JSLockHolder), which prevents
    // any other thread from starting a collection or mutating the heap.
    // We do NOT use PreventCollectionScope here: it calls preventCollection()
    // which internally calls waitForCollector() → stopIfNecessarySlow() →
    // collectInMutatorThread(), and on the second call that sequence can drop
    // hasAccessBit (via a WebCore finalizer callback releasing the VM lock),
    // causing the RELEASE_ASSERT in relinquishConn() to fire.
    profiler.setActiveHeapAnalyzer(&analyzer);
    vm.heap.collectNow(JSC::Sync, JSC::CollectionScope::Full);
    profiler.setActiveHeapAnalyzer(nullptr);

    // Phase 2: native reflection walk.
    //
    // Build the walker explicitly so we can prime it with all live JSCells
    // before we walk from the root. `seen` (moved into the walker) pre-
    // populates m_seen with every live JSCell address so that JSCell*
    // fields encountered during the C++ walk are recognised as already-seen
    // and are not re-added to the worklist as plain objects.
    WalkerT walker {
        WTF::move(cb),
        std::move(seen),
        std::span<WTF::CellDispatcher* const>(dispatchers.span())
    };

    // Proactively dispatch every live JSCell through the dispatcher chain.
    //
    // Many JSCells (e.g. JSDOMWindow, any JSGlobalObject) are only reachable
    // in the GC graph — they are not reachable via typed C++ member pointers
    // from `root`. Without this loop, the C++ walk from `root` would never
    // descend into those cells, and any Strong<> (or other tracked type)
    // embedded in their C++ layouts would be silently missed.
    //
    // dispatchCell checks m_cellDispatched, so each cell is dispatched at
    // most once. Cells whose concrete type is unknown to the dispatcher table
    // are silently skipped. We iterate a snapshot of the seen-set; dispatchCell
    // only modifies m_cellDispatched and m_worklist, never m_seen, so the
    // iteration is safe.
    for (const void* cellPtr : walker.seen())
        walker.dispatchCell(cellPtr);

    // Also walk from the root to pick up non-JSCell C++ objects (e.g.
    // BytecodeIntrinsicRegistry) that are reachable via typed C++ member
    // pointers but are not themselves JSCells and therefore not in the
    // GC-phase seen-set.
    walker.template enqueue<Root>(root);
    walker.drain();
}

} // namespace JSC

#endif // ENABLE(REFTRACKER) && __has_include(<meta>)
