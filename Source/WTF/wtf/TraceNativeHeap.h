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

#include <wtf/Compiler.h>
#include <wtf/DataLog.h>
#include <wtf/HashSet.h>
#include <wtf/StdLibExtras.h>
#include <wtf/Vector.h>

// The walker depends on C++26 reflection (<meta>). Apple clang parses WTF
// headers during the framework module verifier phase and does not ship
// <meta>, so gate on __has_include. Under the p2996 clang the header is
// fully active.
#if ENABLE(REFTRACKER) && __has_include(<meta>)

#include <meta>
#include <ranges>
#include <string_view>
#include <type_traits>

namespace WTF {

// Reflection-based native heap walker. Walks the C++ object graph starting
// from a root, using C++26 reflection to enumerate each node's non-static
// data members. Iterable types use their iterators. Cycles are cut by a
// pointer-keyed seen-set. Type information is erased through function-pointer
// trampolines so the runtime worklist holds regular values — std::meta::info
// is consteval-only in P2996 and cannot round-trip through runtime storage.
//
// Entry points:
//
//   traceNativeHeap<TargetTemplate, verbose>(root, callback)
//   traceNativeHeap<TargetTemplate, verbose>(root, callback, externalSeen)
//
// `TargetTemplate` is an `std::meta::info` NTTP formed at the call site from
// a class template reflection (e.g. `^^JSC::Strong`). The callback is
// instantiated once per concrete specialization encountered; the argument is
// the specialization by reference, so callers access members by name without
// further reflection.
//
// Matching: `std::meta::template_of(^^T) == TargetTemplate` — valid for any
// specialization of the reflected template (see libcxx test
// related-reflections.pass.cpp). On match, the callback fires and the walker
// does NOT descend into the matched node.
//
// Verbose mode: when `verbose` is true, every enter/match/deref logs the
// current path stack via dataLog with an explicit flush, so the last line
// survives a crash on a dangling pointer. When false, the path stack, log
// calls, and string-capture machinery all collapse to nothing via
// `if constexpr`.

namespace TraceNativeHeapDetail {

template<typename T>
concept HasBeginEnd = requires(T& t) {
    std::ranges::begin(t);
    std::ranges::end(t);
};

struct PathFrame {
    std::string_view typeName;
    std::string_view memberName;  // empty for type frames
    const void* address;
};

template<bool verbose>
class PathStack {
public:
    ALWAYS_INLINE void push(PathFrame f)
    {
        if constexpr (verbose)
            m_frames.append(f);
    }

    ALWAYS_INLINE void pop()
    {
        if constexpr (verbose)
            m_frames.removeLast();
    }

    void log(std::string_view kind, const void* about) const
    {
        if constexpr (verbose) {
            dataLog("native-heap-walk: ", kind, " ", about, " path=[");
            bool first = true;
            for (auto& f : m_frames) {
                if (!first)
                    dataLog(" -> ");
                first = false;
                dataLog(f.typeName);
                if (!f.memberName.empty())
                    dataLog(".", f.memberName);
                dataLog("@", f.address);
            }
            dataLogLn("]");
            WTF::dataFile().flush();
        }
    }

private:
    Vector<PathFrame, 0> m_frames;
};

} // namespace TraceNativeHeapDetail

template<typename Visitor, typename T>
ALWAYS_INLINE void visitNativeChildren(T& node, Visitor& v);

template<auto TargetTemplate, bool verbose, typename Callback>
class NativeHeapFinder {
public:
    using Self = NativeHeapFinder;
    static constexpr bool verboseMode = verbose;

    explicit NativeHeapFinder(Callback&& cb)
        : m_callback(std::forward<Callback>(cb))
    {
    }

    NativeHeapFinder(Callback&& cb, UncheckedKeyHashSet<const void*>&& seen)
        : m_callback(std::forward<Callback>(cb))
        , m_seen(WTF::move(seen))
    {
    }

    template<typename T>
    ALWAYS_INLINE void enqueue(T& node)
    {
        // Strip cv/ref so every worklist entry, trampoline, and recursive
        // visit uses the canonical bare type. Keeps reflection operators
        // from hitting const/volatile/reference variations of the same type.
        using Bare = std::remove_cvref_t<T>;
        auto* addr = const_cast<void*>(static_cast<const volatile void*>(std::addressof(node)));
        if constexpr (verbose)
            m_path.log("enqueue", addr);
        if (!m_seen.add(addr).isNewEntry)
            return;
        m_worklist.append(WorkItem { addr, &trampoline<Bare> });
    }

    void drain()
    {
        while (!m_worklist.isEmpty()) {
            auto item = m_worklist.takeLast();
            item.fn(item.ptr, *this);
        }
    }

    UncheckedKeyHashSet<const void*>& seen() LIFETIME_BOUND { return m_seen; }

    // Path-stack accessors — no-op when verbose is false.
    ALWAYS_INLINE void pushPathFrame(TraceNativeHeapDetail::PathFrame f)
    {
        if constexpr (verbose)
            m_path.push(f);
    }

    ALWAYS_INLINE void popPathFrame()
    {
        if constexpr (verbose)
            m_path.pop();
    }

    ALWAYS_INLINE void logDeref(std::string_view kind, const void* p)
    {
        if constexpr (verbose)
            m_path.log(kind, p);
    }

    // `T` is the bare type stored in the worklist — enqueue strips cv/ref
    // before picking `trampoline<Bare>`, so we can reflect directly on T.
    template<typename T>
    ALWAYS_INLINE void doVisit(T& node)
    {
        if constexpr (verbose) {
            m_path.push({
                std::meta::display_string_of(^^T),
                { },
                std::addressof(node)
            });
            m_path.log("visit", std::addressof(node));
        }

        if constexpr (std::meta::has_template_arguments(^^T)
                      && std::meta::template_of(^^T) == TargetTemplate) {
            if constexpr (verbose)
                m_path.log("MATCH", std::addressof(node));
            m_callback(node);
        } else {
            visitNativeChildren(node, *this);
        }

        if constexpr (verbose)
            m_path.pop();
    }


private:
    template<typename T>
    static void trampoline(void* ptr, Self& v)
    {
        v.template doVisit<T>(*static_cast<T*>(ptr));
    }

    struct WorkItem {
        void* ptr;
        void (*fn)(void*, Self&);
    };

    Callback m_callback;
    UncheckedKeyHashSet<const void*> m_seen;
    Vector<WorkItem> m_worklist;
    [[no_unique_address]] TraceNativeHeapDetail::PathStack<verbose> m_path;
};

namespace TraceNativeHeapDetail {

// Handle one field value: leaf for fundamentals/enums/function pointers,
// enqueue for class types and dereferenced non-void/non-function raw pointers.
template<typename Visitor, typename FieldType>
ALWAYS_INLINE void visitFieldValue(FieldType& field, Visitor& v)
{
    using Bare = std::remove_cvref_t<FieldType>;
    if constexpr (std::is_pointer_v<Bare>) {
        using Pointee = std::remove_cvref_t<std::remove_pointer_t<Bare>>;
        if constexpr (!std::is_void_v<Pointee> && !std::is_function_v<Pointee>) {
            if (field) {
                v.logDeref("deref raw pointer to", field);
                v.enqueue(*field);
            }
        }
    } else if constexpr (std::is_class_v<Bare> || std::is_union_v<Bare>) {
        v.enqueue(field);
    }
    // else: leaf (fundamental / enum / array of fundamental / member ptr)
}

} // namespace TraceNativeHeapDetail

// Shallow: enumerate direct children of `node`. If `T` is iterable use
// iterators; else reflect over its non-static data members.
// `T` is the bare type — reflect on it directly.
//
// When `T` is a class reached via a raw-pointer field whose pointee type is
// only forward-declared in this TU, `std::meta::is_complete_type(^^T)` is
// false and we skip the member expansion: we cannot enumerate members of an
// incomplete type, so we treat the object as a leaf. The seen-set entry is
// still recorded by `enqueue`, which prevents re-entry.
template<typename Visitor, typename T>
ALWAYS_INLINE void visitNativeChildren(T& node, Visitor& v)
{
    if constexpr (TraceNativeHeapDetail::HasBeginEnd<T>) {
        for (auto&& elem : node)
            TraceNativeHeapDetail::visitFieldValue(elem, v);
    } else if constexpr ((std::is_class_v<T> || std::is_union_v<T>)
                         && std::meta::is_complete_type(^^T)) {
        template for (constexpr auto member :
                      std::define_static_array(
                          std::meta::nonstatic_data_members_of(^^T,
                              std::meta::access_context::unchecked()))) {
            // Skip bit-fields (can't bind to a reference) and unnamed
            // members (anonymous unions/structs: not directly spliceable
            // and `identifier_of` isn't a constant expression for them).
            if constexpr (!std::meta::is_bit_field(member)
                          && std::meta::has_identifier(member)) {
                v.pushPathFrame({
                    std::meta::display_string_of(^^T),
                    std::meta::identifier_of(member),
                    std::addressof(node.[:member:])
                });
                TraceNativeHeapDetail::visitFieldValue(node.[:member:], v);
                v.popPathFrame();
            }
        };
    }
    // else: fundamental / enum / incomplete class — no children
}

// --- Public entry points ---------------------------------------------------

template<auto TargetTemplate, bool verbose = false, typename Root, typename Callback>
void traceNativeHeap(Root& root, Callback&& callback)
{
    NativeHeapFinder<TargetTemplate, verbose, std::decay_t<Callback>> visitor {
        std::forward<Callback>(callback)
    };
    visitor.template enqueue<Root>(root);
    visitor.drain();
}

template<auto TargetTemplate, bool verbose = false, typename Root, typename Callback>
void traceNativeHeap(Root& root, Callback&& callback,
                     UncheckedKeyHashSet<const void*>& externalSeen)
{
    NativeHeapFinder<TargetTemplate, verbose, std::decay_t<Callback>> visitor {
        std::forward<Callback>(callback),
        WTF::move(externalSeen)
    };
    visitor.template enqueue<Root>(root);
    visitor.drain();
    externalSeen = WTF::move(visitor.seen());
}

} // namespace WTF

#endif // ENABLE(REFTRACKER) && __has_include(<meta>)
