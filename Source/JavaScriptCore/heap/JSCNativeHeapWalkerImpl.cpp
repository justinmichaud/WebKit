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

// ── Purpose ──────────────────────────────────────────────────────────────────
//
// This is the sole TU that instantiates the JSC native-heap walker templates.
// It deliberately includes the *full* definitions of every class that VM.h and
// JSGlobalObject.h only forward-declare so that the reflection walker's
//   is_complete_type(^^X)
// guard evaluates to TRUE for every unique_ptr<X>/UniqueRef<X> member it
// encounters. Without complete definitions the walker would silently skip
// those pointers; here nothing is skipped.
//
// Adding Strong<T> members to any of these classes in the future requires no
// changes here — the reflection walk picks them up automatically as long as
// the class header is already transitively included below.
//
// ── Include policy ───────────────────────────────────────────────────────────
//
// This file may include as many JSC/WTF headers as needed.  None of those
// includes should be added to non-walker headers (VM.h, JSGlobalObject.h,
// etc.) solely to satisfy the walker — that is exactly what this file avoids.

#include "config.h"

#if ENABLE(REFTRACKER) && __has_include(<meta>)

#include "JSCNativeHeapWalkerImpl.h"
#include "TandemHeapAnalyzer.h"

// ── Types forward-declared in VM.h ───────────────────────────────────────────
// Each header below provides the full definition of a class that VM.h stores
// behind a unique_ptr<> or UniqueRef<>.  Without these includes the walker's
// is_complete_type check would return false and the pointer would be skipped.

#include "BuiltinExecutables.h"
#include "BytecodeIntrinsicRegistry.h"
#include "CodeCache.h"
#include "ControlFlowProfiler.h"
#include "CrossTaskToken.h"
#include "DeferredWorkTimer.h"
#include "FuzzerAgent.h"
#include "HasOwnPropertyCache.h"
#include "IntlCache.h"
#include "JITSizeStatistics.h"
#include "JSPIContext.h"
#include "CodeBlockSet.h"
#include "FastMallocAlignedMemoryAllocator.h"
#include "GCActivityCallback.h"
#include "GigacageAlignedMemoryAllocator.h"
#include "JITStubRoutineSet.h"
#include "HeapVerifier.h"
#include "IncrementalSweeper.h"
#include "MarkingConstraint.h"
#include "MarkingConstraintSet.h"
#include "MarkingConstraintSolver.h"
#include "MutatorScheduler.h"
#include "StopIfNecessaryTimer.h"
#include "VerifierSlotVisitor.h"
#include "MegamorphicCache.h"
#include "ProfilerDatabase.h"
#include "RegExpCache.h"
#include "SamplingProfiler.h"
#include "ShadowChicken.h"
#include "SharedJITStubSet.h"
#include "TypeProfiler.h"
#include "TypeProfilerLog.h"
#include "Watchdog.h"
#include "WasmTag.h"
#include "wasm/debugger/WasmDebugServerUtilities.h"

// ── JSGlobalObject and its forward-declared members ──────────────────────────
// JSGlobalObject.h is needed so inspectorController() is callable and so the
// walker can see the full member list of JSGlobalObject.
// JSGlobalObjectInspectorController is already included transitively via
// NamespaceCellDispatcher.h → inspector/JSGlobalObjectInspectorController.h,
// but its own UniqueRef<>/unique_ptr<> members are only forward-declared in
// that header.  Include the full definitions here.

#include "JSGlobalObject.h"
#include "InjectedScriptManager.h"
#include "JSGlobalObjectConsoleClient.h"
#include "JSGlobalObjectDebugger.h"

// ── Extra JSC types forward-declared in VM.h / JSGlobalObject.h ──────────────

#include "BuiltinNames.h"
#include "CheckpointOSRExitSideState.h"
#include "ChainedWatchpoint.h"
#include "FTLThunks.h"
#include "GlobalObjectMethodTable.h"
#include "ImportMap.h"
#include "JITThunks.h"
#include "JSDateMath.h"
#include "JSGlobalObjectDebuggable.h"
#include "ObjectAdaptiveStructureWatchpoint.h"
#include "runtime/ObjectPropertyChangeAdaptiveWatchpoint.h"
#include "TypedArrayController.h"
#include "VMEntryScope.h"
#include "WaiterListManager.h"
#include "YarrInterpreter.h"

// ── Inspector types forward-declared in JSGlobalObjectInspectorController.h ──

#include "inspector/AsyncStackTrace.h"
#include "inspector/ConsoleMessage.h"
#include "inspector/InjectedScriptHost.h"
#include "inspector/ScriptArguments.h"
#include "inspector/ScriptCallStack.h"
#include "inspector/InspectorFrontendChannel.h"
#include "inspector/agents/InspectorAgent.h"
#include "inspector/agents/InspectorConsoleAgent.h"
#include "inspector/agents/InspectorDebuggerAgent.h"
#include "inspector/agents/InspectorScriptProfilerAgent.h"
#include "inspector/augmentable/AlternateDispatchableAgent.h"

// ── WTF types ─────────────────────────────────────────────────────────────────

#include <wtf/SimpleStats.h>

// ─────────────────────────────────────────────────────────────────────────────

#include <wtf/DataLog.h>
#include <wtf/StackTrace.h>

namespace JSC {

unsigned findLiveStrongCount(VM& vm)
{
    unsigned count = 0;
    findAllOnNativeHeap<^^JSC::Strong>(vm, vm, [&count](auto&) { ++count; });
    return count;
}

void dumpAllStrongHandles(VM& vm)
{
    findAllOnNativeHeap<^^JSC::Strong>(vm, vm,
        [](auto& strong) {
            dataLogLn("Live Strong at ", RawPointer(std::addressof(strong)), ":");
            auto frames = strong.refTrackerFrames();
            if (!frames.empty())
                dataLogLn(WTF::StackTracePrinter { frames });
            else
                dataLogLn("  (no stack trace - REFTRACKER flag was off at construction)");
            dataLogLn();
        });
}

unsigned countStrongInGlobalObject(JSGlobalObject& globalObject)
{
    unsigned count = 0;
    WTF::traceNativeHeap<^^JSC::Strong>(globalObject, [&count](auto&) { ++count; });
    return count;
}

unsigned countStrongInInspectorController(JSGlobalObject& globalObject)
{
    unsigned count = 0;
    auto& controller = globalObject.inspectorController();
    WTF::traceNativeHeap<^^JSC::Strong>(controller, [&count](auto&) { ++count; });
    return count;
}

} // namespace JSC

#endif // ENABLE(REFTRACKER) && __has_include(<meta>)
