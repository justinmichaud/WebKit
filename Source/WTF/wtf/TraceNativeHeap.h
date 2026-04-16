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
#include <wtf/Deque.h>
#include <wtf/HashSet.h>
#include <wtf/StdLibExtras.h>
#include <wtf/Vector.h>

#if CPU(ARM64E)
#include <ptrauth.h>
#endif

// The walker depends on C++26 reflection (<meta>). Apple clang parses WTF
// headers during the framework module verifier phase and does not ship
// <meta>, so gate on __has_include. Under the p2996 clang the header is
// fully active.
#if ENABLE(REFTRACKER) && __has_include(<meta>)

#include <meta>
#include <ranges>
#include <span>
#include <string_view>
#include <deque>
#include <type_traits>
#include <unordered_set>
#include <wtf/CellDispatcher.h>

// Forward-declared so the walker header can gate JSCell-subclass dispatch
// without pulling in any JSC headers. WTF must remain JSC-free.
namespace JSC { class JSCell; }

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

// Intentionally uses t.begin()/t.end() rather than std::ranges::begin/end so
// that containers whose iterators lack postfix ++ (e.g. WTF::Deque) are still
// treated as iterable. The range-based for loop only needs prefix ++, which
// those iterators do provide.
//
// The `*t.begin()` requirement rejects types whose iterators have no
// operator* (e.g. JSScope, whose ScopeChainIterator only has operator->).
// `sizeof(*t.begin())` rejects containers whose element type is incomplete
// (e.g. FixedVector<IncompleteType>): sizeof on an incomplete type is
// ill-formed, so the requires expression evaluates to false, and the walker
// treats the container as an opaque leaf rather than trying to iterate it.
template<typename T>
concept HasBeginEnd = requires(T& t) {
    t.begin();
    t.end();
    *t.begin();
    sizeof(*t.begin());
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
class NativeHeapFinder : public NativeHeapFinderBase {
public:
    using Self = NativeHeapFinder;
    static constexpr bool verboseMode = verbose;

    explicit NativeHeapFinder(Callback&& cb)
        : m_callback(std::forward<Callback>(cb))
    {
    }

    NativeHeapFinder(Callback&& cb, std::span<CellDispatcher* const> dispatchers)
        : m_callback(std::forward<Callback>(cb))
        , m_cellDispatchers(dispatchers)
    {
    }

    NativeHeapFinder(Callback&& cb, std::unordered_set<const void*>&& seen)
        : m_callback(std::forward<Callback>(cb))
        , m_seen(std::move(seen))
    {
    }

    NativeHeapFinder(Callback&& cb, std::unordered_set<const void*>&& seen,
                     std::span<CellDispatcher* const> dispatchers)
        : m_callback(std::forward<Callback>(cb))
        , m_seen(std::move(seen))
        , m_cellDispatchers(dispatchers)
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

        // If `T` is a JSCell subclass, route through the dispatcher chain
        // first so the walker re-enters with the dynamic concrete subclass
        // rather than walking only `T`'s inherited fields. Gate on a concept
        // that SFINAEs cleanly when JSCell is only forward-declared — the
        // `static_cast<JSC::JSCell*>(static_cast<T*>(nullptr))` expression
        // is well-formed iff T (complete) publicly derives from JSCell.
        if constexpr (requires { static_cast<JSC::JSCell*>(static_cast<Bare*>(nullptr)); }) {
            // Dedup cell dispatch via a separate set: phase-1 of the tandem
            // walk pre-populates m_seen with every live cell, so routing
            // through m_seen here would always early-return and cells would
            // never be walked. m_cellDispatched gates the dispatcher path
            // independently, preventing re-dispatch of the same cell while
            // leaving m_seen free to guard all downstream C++ fields.
            if (!m_cellDispatched.insert(addr).second)
                return;
            for (auto* dispatcher : m_cellDispatchers) {
                if (dispatcher->tryDispatch(addr, *this))
                    return;
            }
            // No dispatcher matched — fall back to the static-type trampoline.
        } else {
            if (!m_seen.insert(addr).second)
                return;
        }
        m_worklist.push_back(WorkItem { addr, &trampoline<Bare> });
    }

    // For inline (non-pointer) class-type fields. Visits the subobject
    // directly (synchronous recursion) instead of deferring through the
    // worklist. This is safe because inline C++ members cannot form cycles
    // (a struct cannot contain itself by value), so recursion always
    // terminates. Depth is bounded by the struct nesting depth (~10-15
    // levels in WebKit).
    template<typename T>
    ALWAYS_INLINE void visitInline(T& node)
    {
        using Bare = std::remove_cvref_t<T>;
        auto* addr = const_cast<void*>(static_cast<const volatile void*>(std::addressof(node)));
        if constexpr (verbose)
            m_path.log("visitInline", addr);
        if constexpr (requires { static_cast<JSC::JSCell*>(static_cast<Bare*>(nullptr)); }) {
            if (!m_cellDispatched.insert(addr).second)
                return;
            for (auto* dispatcher : m_cellDispatchers) {
                if (dispatcher->tryDispatch(addr, *this))
                    return;
            }
        }
        // If this address was already visited via a pointer dereference,
        // skip it — the pointer-based visit already walked this object.
        if (m_seen.count(addr))
            return;
        this->template doVisit<Bare>(*static_cast<Bare*>(addr));
    }

    // Called from a CellDispatcher after it has determined the concrete
    // JSCell subclass `T`. Appends (ptr, trampoline<T>) to the worklist
    // directly; dedup was already handled by `enqueue`'s m_cellDispatched
    // gate before the dispatcher chain was consulted.
    // The m_seen check prevents worklist explosion: without it, walking
    // every live JSCell's members generates millions of worklist entries
    // and overflows the Vector backing store.
    template<typename T>
    ALWAYS_INLINE void enqueueCellDispatched(void* ptr)
    {
        if (!m_seen.insert(ptr).second)
            return;
        m_worklist.push_back(WorkItem { ptr, &trampoline<T> });
    }

    void drain()
    {
        drainFrom(0);
    }

    // Drain only items pushed after `mark`. This allows container iteration
    // to fully process one element's subtree before moving to the next,
    // keeping the worklist bounded by graph depth rather than container size.
    void drainFrom(size_t mark)
    {
        while (m_worklist.size() > mark) {
            auto item = m_worklist.back();
            m_worklist.pop_back();
            item.fn(item.ptr, *this);
        }
    }

    size_t worklistMark() const { return m_worklist.size(); }

    std::unordered_set<const void*>& seen() LIFETIME_BOUND { return m_seen; }

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
    std::unordered_set<const void*> m_seen;
    std::unordered_set<const void*> m_cellDispatched;
    std::deque<WorkItem> m_worklist;
    std::span<CellDispatcher* const> m_cellDispatchers;
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
        // Skip void*, function pointers, and pointers to incomplete types.
        // Incomplete types cannot be dereferenced (no known size/layout),
        // and instantiating trampoline<IncompleteType> would fail because the
        // trampoline body dereferences the pointer. is_complete_type is
        // evaluated at template-instantiation time, so forward-declared types
        // that are never defined in this TU correctly evaluate to false.
        if constexpr (!std::is_void_v<Pointee> && !std::is_function_v<Pointee>
                      && std::meta::is_complete_type(^^Pointee)) {
            if (field) {
                auto* raw = field;
#if CPU(ARM64E)
                raw = ptrauth_strip(raw, ptrauth_key_process_dependent_data);
#endif
                // Skip tagged/misaligned pointers. CompactPointerTuple and
                // similar classes pack metadata into the low bits of pointer
                // fields; dereferencing those would crash.
                if (reinterpret_cast<uintptr_t>(raw) % alignof(Pointee) == 0) {
                    v.logDeref("deref raw pointer to", raw);
                    v.enqueue(*raw);
                }
            }
        }
    } else if constexpr (std::is_class_v<Bare> || std::is_union_v<Bare>) {
        v.visitInline(field);
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
        for (auto&& elem : node) {
            auto mark = v.worklistMark();
            TraceNativeHeapDetail::visitFieldValue(elem, v);
            v.drainFrom(mark);
        }
    } else if constexpr ((std::is_class_v<T> || std::is_union_v<T>)
                         && std::meta::is_complete_type(^^T)) {
        // 1. Walk inherited subobjects in declaration order. Single inheritance
        //    means &base == &derived, so we recurse directly rather than going
        //    through enqueue (which would hit the seen-set and skip).
        //    Private bases are filtered out because `static_cast<BaseT&>` is
        //    rejected by the compiler for inaccessible bases. Representative
        //    casualties: WTF::StringImpl privately inherits StringImplShape;
        //    WTF::VectorBuffer privately inherits VectorBufferBase;
        //    std::optional privately inherits its move-assign helper. Public
        //    bases are the overwhelming majority and cover every base that
        //    transitively holds tracked state.
        template for (constexpr auto baseInfo :
                      std::define_static_array(
                          std::meta::bases_of(^^T,
                              std::meta::access_context::unchecked()))) {
            if constexpr (std::meta::is_public(baseInfo)) {
                using BaseT = typename [: std::meta::type_of(baseInfo) :];
                if constexpr (std::is_class_v<BaseT> && std::meta::is_complete_type(^^BaseT)) {
                    visitNativeChildren<Visitor, BaseT>(static_cast<BaseT&>(node), v);
                }
            }
        }
        // 2. Then walk this class's own non-static members.
        template for (constexpr auto member :
                      std::define_static_array(
                          std::meta::nonstatic_data_members_of(^^T,
                              std::meta::access_context::unchecked()))) {
            // Skip bit-fields (can't bind to a reference) and unnamed
            // members (anonymous unions/structs: not directly spliceable
            // and `identifier_of` isn't a constant expression for them).
            if constexpr (!std::meta::is_bit_field(member)
                          && std::meta::has_identifier(member)) {
                // identifier_of works for data-member reflections but not for
                // type reflections in this clang P2996 fork, so typeName is
                // left empty. The member name and address are sufficient for
                // crash-path debugging.
                v.pushPathFrame({
                    "",
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
                     std::unordered_set<const void*>& externalSeen)
{
    NativeHeapFinder<TargetTemplate, verbose, std::decay_t<Callback>> visitor {
        std::forward<Callback>(callback),
        std::move(externalSeen)
    };
    visitor.template enqueue<Root>(root);
    visitor.drain();
    externalSeen = std::move(visitor.seen());
}

template<auto TargetTemplate, bool verbose = false, typename Root, typename Callback>
void traceNativeHeap(Root& root, Callback&& callback,
                     std::unordered_set<const void*>& externalSeen,
                     std::span<CellDispatcher* const> dispatchers)
{
    NativeHeapFinder<TargetTemplate, verbose, std::decay_t<Callback>> visitor {
        std::forward<Callback>(callback),
        std::move(externalSeen),
        dispatchers
    };
    visitor.template enqueue<Root>(root);
    visitor.drain();
    externalSeen = std::move(visitor.seen());
}

} // namespace WTF

#endif // ENABLE(REFTRACKER) && __has_include(<meta>)
