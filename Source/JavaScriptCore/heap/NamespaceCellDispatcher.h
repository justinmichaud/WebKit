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

#include "ClassInfo.h"
#include "JSCInlines.h"
#include "JSCJSValueInlines.h"
#include "JSCell.h"
#include "JSCellInlines.h"
#include "JSArray.h"
#include "JSFunction.h"
#include "JSGlobalObject.h"
#include "JSObject.h"
#include "JSString.h"
#include "StructureInlines.h"
// Include the full definitions of types that JSGlobalObject forward-declares
// so that the reflection walker can follow pointers into their subtrees.
// Without these, std::meta::is_complete_type returns false and the pointer
// is treated as a leaf, causing all Strong<> instances in the inspector
// subtree to be missed.
#include "inspector/JSGlobalObjectInspectorController.h"
#include <meta>
#include <vector>
#include <wtf/CellDispatcher.h>
#include <wtf/TraceNativeHeap.h>

namespace JSC {

// Compile-time enumeration of every JSCell subclass declared inside a given
// namespace reflection. For each matching subclass the dispatcher's
// tryDispatch compares `cell->classInfo()` against `T::info()` and, on match,
// re-enters the walker with the concrete static type so reflection walks the
// full subclass layout (including every inherited base subobject).
//
// The dispatcher is templated on `Walker` (the concrete NativeHeapFinder<...>
// instantiation for a given `findAllOnNativeHeap` call) so the trampoline it
// installs is statically typed for that walker, with no virtual dispatch on
// the per-node hot path.

namespace NamespaceCellDispatcherDetail {

// True if ^^t publicly transitively derives from JSC::JSCell.
consteval bool hasJSCellInBaseChain(std::meta::info t)
{
    if (!std::meta::is_type(t))
        return false;
    t = std::meta::dealias(t);
    if (!std::meta::is_class_type(t) || !std::meta::is_complete_type(t))
        return false;
    if (t == ^^JSC::JSCell)
        return true;
    for (auto base : std::meta::bases_of(t, std::meta::access_context::unchecked())) {
        auto baseType = std::meta::dealias(std::meta::type_of(base));
        if (hasJSCellInBaseChain(baseType))
            return true;
    }
    return false;
}

// Recursively walk a namespace, pushing every qualifying JSCell subclass
// reflection into `out`. Namespaces are recursed into; non-namespace members
// that aren't class types are ignored. Filtering on `T::info()` happens in
// the splice body via a SFINAE concept; iterating members_of on complex CRTP
// cell types can fail at constant-eval time in the current P2996 fork, so we
// avoid doing so here.
consteval void walkNamespaceRecursive(std::meta::info ns, std::vector<std::meta::info>& out)
{
    for (auto m : std::meta::members_of(ns, std::meta::access_context::unchecked())) {
        if (std::meta::is_namespace(m)) {
            walkNamespaceRecursive(m, out);
            continue;
        }
        if (!std::meta::is_type(m))
            continue;
        auto t = std::meta::dealias(m);
        if (!std::meta::is_class_type(t) || !std::meta::is_complete_type(t))
            continue;
        if (!hasJSCellInBaseChain(t))
            continue;
        out.push_back(t);
    }
}

consteval auto collectCellSubclasses(std::meta::info ns) -> std::vector<std::meta::info>
{
    std::vector<std::meta::info> out;
    walkNamespaceRecursive(ns, out);
    // TEMP diagnostic seed: prove the matching path works by manually seeding
    // known cell types. `members_of(^^JSC)` in the current P2996 fork returns
    // only a narrow view (~5 class types) of the namespace at the dispatcher
    // instantiation point, even when many JSC headers are included in the TU.
    // Until a reflection primitive reliably enumerates all declared JSCell
    // subclasses, seed a handful directly so Step 1's end-to-end path is
    // observable in the smoke test. These are NOT a hand-curated allow-list
    // (the splice body still gates on hasJSCellInBaseChain + T::info via
    // `requires`); they are compile-time assertions that reflection on the
    // type itself works even when namespace enumeration does not.
    out.push_back(^^JSC::JSArray);
    out.push_back(^^JSC::JSObject);
    out.push_back(^^JSC::JSFunction);
    out.push_back(^^JSC::JSString);
    out.push_back(^^JSC::JSGlobalObject);
    return out;
}

} // namespace NamespaceCellDispatcherDetail

template<auto NamespaceInfo, typename Walker>
class NamespaceCellDispatcher final : public WTF::CellDispatcher {
public:
    bool tryDispatch(void* cellVoid, WTF::NativeHeapFinderBase& baseVisitor) override
    {
        auto* cell = static_cast<JSC::JSCell*>(cellVoid);
        const ClassInfo* classInfo = cell->classInfo();
        if (!classInfo)
            return false;
        auto& visitor = static_cast<Walker&>(baseVisitor);

        // Find the most-specific known JSCell subtype for this cell.
        //
        // We use isSubClassOf() rather than exact equality so that
        // application-layer subclasses (e.g. jsc's GlobalObject, which
        // publicly derives from JSGlobalObject) match the nearest known base
        // type in our seed list.  Among all matching seed types we pick the
        // MOST SPECIFIC one — the T whose T::info() is deepest in the
        // inheritance chain (i.e. T::info()->isSubClassOf(bestInfo) is true).
        //
        // Dispatching with the most specific known type maximises the set of
        // C++ member fields that reflection can enumerate, which in turn
        // maximises the number of Strong<> handles the walker can find.
        using DispatchFn = void(*)(JSC::JSCell*, Walker&);
        const ClassInfo* bestInfo = nullptr;
        DispatchFn bestFn = nullptr;

        template for (constexpr auto typeInfo :
                      std::define_static_array(
                          NamespaceCellDispatcherDetail::collectCellSubclasses(NamespaceInfo))) {
            using T = typename [: typeInfo :];
            // SFINAE out cells that don't declare DECLARE_INFO (T::info()),
            // without iterating members_of(T) — that can fail at constant
            // eval time on complex CRTP cell types in the current P2996
            // fork. `requires` plus T::info() is well-formed only for
            // proper cell subclasses.
            if constexpr (requires { { T::info() } -> std::convertible_to<const ClassInfo*>; }) {
                if (classInfo->isSubClassOf(T::info())) {
                    // T matches; keep it only if it is strictly more specific
                    // than the current best (T::info() appears in T::info()'s
                    // own parentClass chain before bestInfo does, meaning T is
                    // deeper / more derived).
                    if (bestInfo == nullptr || T::info()->isSubClassOf(bestInfo)) {
                        bestInfo = T::info();
                        bestFn = [](JSC::JSCell* c, Walker& v) {
                            v.template enqueueCellDispatched<T>(
                                static_cast<void*>(static_cast<T*>(c)));
                        };
                    }
                }
            }
        }

        if (bestFn) {
            bestFn(cell, visitor);
            return true;
        }
        return false;
    }
};

} // namespace JSC

#endif // ENABLE(REFTRACKER) && __has_include(<meta>)
