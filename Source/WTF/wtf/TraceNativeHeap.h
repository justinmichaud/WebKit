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
//
// ── Customization points ─────────────────────────────────────────────────────
//
// Any type (class, struct, or union) may opt into custom traversal by
// providing one or more of the hooks below.  All hooks are opt-in; types that
// do not declare them get the default reflection-based walk.
//
// 1. Full custom traversal — refTrackerVisit
// ─────────────────────────────────────────
//   static constexpr bool refTrackerVisitTag = true;   ← REQUIRED sentinel
//   template<typename Visitor>
//   static void refTrackerVisit(const T& self, Visitor& v);
//
//   IMPORTANT: Requires BOTH the `static constexpr bool refTrackerVisitTag = true;`
//   sentinel and a static method `refTrackerVisit(const T&, Visitor&)`.
//   Bloomberg's p2996 clang has three bugs: (1) ALL function-call requires-expressions
//   evaluate to FALSE; (2) `requires { typename T::tag; }` evaluates to FALSE in
//   reflection contexts; (3) `std::void_t<typename T::TypeAlias>` partial-specialisation
//   SFINAE also fails in those contexts.  The ADL `refTrackerHasVisitTag` function
//   (unqualified call via `using` in visitNativeChildren) is immune to all three bugs.
//   The actual call BareT::refTrackerVisit(node, v) is outside the detection path
//   and is unaffected by these bugs.
//
//   The walker calls this instead of its own reflection walk for any type that
//   declares it.  Use it when the default walk is unsafe or incomplete:
//
//   • JSValue — must consult its NaN-box tag before following the embedded
//     JSCell* so that float/int payloads are never treated as pointers:
//
//       template<typename Visitor>
//       static void refTrackerVisit(const JSValue& self, Visitor& v) {
//           if (self.isCell()) v.enqueue(*self.asCell());
//       }
//
//   • Tagged-pointer wrappers — strip the tag bits before dereferencing:
//
//       template<typename Visitor>
//       static void refTrackerVisit(const MyWrapper& self, Visitor& v) {
//           if (auto* p = self.untaggedPointer()) v.enqueue(*p);
//       }
//
//   • Variants / discriminated wrappers — dispatch to the active alternative.
//
//   The Visitor interface (for use inside refTrackerVisit):
//     v.enqueue(T& obj)      — schedule obj for walking (pointer targets)
//     v.visitInline(T& obj)  — walk obj immediately (inline subobjects)
//
// 2. Discriminated union active-member index — refTrackerActiveMemberIndex
// ────────────────────────────────────────────────────────────────────────
//   static constexpr bool refTrackerActiveMemberTag = true;   ← REQUIRED sentinel
//   static size_t refTrackerActiveMemberIndex(const T& u);
//
//   Declared as a static member of a union type T.  Returns the 0-based index
//   (in declaration order, matching nonstatic_data_members_of) of the union
//   member that is currently active.  The walker visits only that member.
//   Unions that declare neither this method nor refTrackerVisit are treated as
//   opaque leaves.
//
//   IMPORTANT: BOTH the `static constexpr bool refTrackerActiveMemberTag = true;`
//   sentinel AND the static method are required.  Bloomberg p2996 clang has bugs
//   that make function-call requires-expressions, typename requires-expressions,
//   and std::void_t<typename T::TypeAlias> SFINAE all fail in reflection contexts.
//   The ADL `refTrackerHasActiveMemberTag` function (unqualified call via `using`
//   in visitNativeChildren) is immune to all three bugs.
//
//   Example — a two-field tagged union:
//
//     union MyUnion {
//         int  asInt;
//         float* asPtr;
//         static constexpr bool refTrackerActiveMemberTag = true;
//         static size_t refTrackerActiveMemberIndex(const MyUnion& u) {
//             return u.tag ? 1 : 0; // tag stored elsewhere in the containing struct
//         }                         // … or encode it inside the union itself
//     };
//
//   Note: if the discriminant lives outside the union (e.g. JSValue's NaN-box
//   tag), use refTrackerVisit on the *containing* type instead, because the
//   union itself does not have access to the discriminant.

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

// Detection helpers for refTrackerVisit and refTrackerActiveMemberIndex hooks.
//
// Bloomberg p2996 clang has bugs that prevent ALL of the following from working
// inside template functions that use `template for` / P2996 reflection:
//   Bug 1: `requires { fn(args); }` always evaluates to false.
//   Bug 2: `requires { typename T::SomeTag; }` evaluates to false.
//   Bug 3: ALL partial-specialisation SFINAE (`std::void_t<...>`) fails — both
//           type-member and value-member forms — when the template is instantiated
//           transitively from a `template for` loop.
//
// The only approach immune to all three bugs:  call a constexpr function
// directly in `if constexpr (fn(...))` using ADL.  The function returns
// `false` by default (fallback template below); each tagged type adds an
// overload in its own namespace that returns `true`.  ADL finds the overload
// at instantiation time; no SFINAE is involved.
//
// CRITICAL: The call MUST be unqualified for ADL to apply. In visitNativeChildren
// (WTF namespace), we use `using TraceNativeHeapDetail::refTrackerHasVisitTag;`
// to bring the fallback into scope, then call `refTrackerHasVisitTag(p)` unqualified.
// DO NOT write `TraceNativeHeapDetail::refTrackerHasVisitTag(p)` — that is a
// qualified call and ADL does not apply, so overloads in other namespaces are missed.
//
// Usage pattern in each tagged type's namespace (OUTSIDE the class):
//   #if ENABLE(REFTRACKER)
//   inline constexpr bool refTrackerHasVisitTag(MyType*) noexcept { return true; }
//   #endif
//
// For class templates, use a function template overload:
//   template<typename... Ts>
//   constexpr bool refTrackerHasVisitTag(MyTemplate<Ts...>*) noexcept { return true; }
//
// Types also keep `static constexpr bool refTrackerVisitTag = true;` inside
// the class for documentation and for non-Bloomberg builds that do not need
// the ADL workaround.

// Default fallbacks (return false for any type not explicitly opted in):
template<typename T>
constexpr bool refTrackerHasVisitTag(T*) noexcept { return false; }

template<typename T>
constexpr bool refTrackerHasActiveMemberTag(T*) noexcept { return false; }

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
        // Guard against re-visiting the same (address, type) pair on the
        // current inline call stack. Pointer chains can lead back to an
        // object we're currently visiting (e.g. VM → ptr → ... → VM).
        // Store the pair directly to avoid XOR false collisions (two distinct
        // (addr, type) pairs can XOR to the same value). Erase on return so
        // sibling subtrees aren't blocked.
        auto key = std::make_pair(
            reinterpret_cast<uintptr_t>(addr),
            reinterpret_cast<uintptr_t>(&trampoline<Bare>));
        if (!m_inlineVisiting.insert(key).second)
            return;
        this->template doVisit<Bare>(*static_cast<Bare*>(addr));
        m_inlineVisiting.erase(key);
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

    struct PairHash {
        size_t operator()(std::pair<uintptr_t, uintptr_t> p) const
        {
            size_t h = std::hash<uintptr_t>{}(p.first);
            h ^= std::hash<uintptr_t>{}(p.second) + 0x9e3779b9u + (h << 6) + (h >> 2);
            return h;
        }
    };

    Callback m_callback;
    std::unordered_set<const void*> m_seen;
    std::unordered_set<const void*> m_cellDispatched;
    std::deque<WorkItem> m_worklist;
    std::unordered_set<std::pair<uintptr_t, uintptr_t>, PairHash> m_inlineVisiting;
    std::span<CellDispatcher* const> m_cellDispatchers;
    [[no_unique_address]] TraceNativeHeapDetail::PathStack<verbose> m_path;
};

namespace TraceNativeHeapDetail {

// Handle one field value: leaf for fundamentals/enums/function pointers,
// enqueue for class/union types and dereferenced non-void/non-function raw
// pointers.
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
        // Both class/struct and union fields route through visitInline.
        // visitNativeChildren handles each case:
        //   • class/struct: default reflection walk over members
        //   • union: refTrackerVisit hook or refTrackerActiveMemberIndex dispatch
        // Types that need custom traversal (e.g. JSValue, tagged pointer
        // wrappers) declare a refTrackerVisit member — see the file-level
        // comment for the full customization-point documentation.
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
//
// Customization points — see the file-level comment for full documentation:
//   refTrackerVisit(v)                  — full custom traversal for any type
//   refTrackerActiveMemberIndex(node)   — active-branch selector for unions
template<typename Visitor, typename T>
ALWAYS_INLINE void visitNativeChildren(T& node, Visitor& v)
{
    // ── Customization point 1: refTrackerVisit ────────────────────────────
    // If the type declares a static `refTrackerVisit(const T&, Visitor&)`,
    // delegate entirely to it and skip the default reflection walk.  Handles:
    //   • Types with externally-discriminated unions (e.g. JSValue → NaN-box)
    //   • Tagged-pointer wrappers (strip tag bits before enqueue)
    //   • Any type that needs non-trivial traversal logic
    //
    // NOTE: Detection uses ADL-based constexpr function calls, immune to all three
    // Bloomberg p2996 clang bugs. The `using` declarations below bring the fallback
    // functions from TraceNativeHeapDetail into the unqualified lookup set; each
    // tagged type adds a more-specific overload in its own namespace that ADL finds
    // at instantiation time. The call MUST be unqualified for ADL to apply — a
    // qualified call like TraceNativeHeapDetail::refTrackerHasVisitTag(...) would
    // bypass ADL entirely. See TraceNativeHeapDetail for the full story.
    using TraceNativeHeapDetail::refTrackerHasVisitTag;
    using TraceNativeHeapDetail::refTrackerHasActiveMemberTag;
    using BareT = std::remove_cv_t<T>;
    if constexpr (refTrackerHasVisitTag(static_cast<BareT*>(nullptr))) {
        BareT::refTrackerVisit(node, v);
        return;
    }

    if constexpr (TraceNativeHeapDetail::HasBeginEnd<T>) {
        for (auto&& elem : node) {
            auto mark = v.worklistMark();
            TraceNativeHeapDetail::visitFieldValue(elem, v);
            v.drainFrom(mark);
        }
    } else if constexpr (std::is_class_v<T>
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
    } else if constexpr (std::is_union_v<T>
                         && std::meta::is_complete_type(^^T)) {
        // ── Customization point 2: refTrackerActiveMemberIndex ────────────
        // For self-contained discriminated unions: the union type declares
        //   static size_t refTrackerActiveMemberIndex(const T&);
        // returning the 0-based index (declaration order, matching
        // nonstatic_data_members_of) of the currently-active member.  The
        // walker visits only that one member.
        //
        // If the discriminant lives outside the union (e.g. JSValue's NaN-box
        // tag), use refTrackerVisit on the *containing* type instead.
        //
        // Unions that declare neither hook are opaque leaves — safe because
        // the containing type's refTrackerVisit (if any) already handled the
        // semantically-meaningful pointer(s).
        // Detection via ADL sentinel (see refTrackerHasActiveMemberTag above).
        // Types with refTrackerActiveMemberIndex must also declare an ADL overload:
        //   inline constexpr bool refTrackerHasActiveMemberTag(MyUnion*) noexcept { return true; }
        if constexpr (refTrackerHasActiveMemberTag(static_cast<BareT*>(nullptr))) {
            size_t activeIdx = T::refTrackerActiveMemberIndex(node);
            size_t idx = 0;
            template for (constexpr auto member :
                          std::define_static_array(
                              std::meta::nonstatic_data_members_of(^^T,
                                  std::meta::access_context::unchecked()))) {
                if constexpr (!std::meta::is_bit_field(member)
                              && std::meta::has_identifier(member)) {
                    if (idx == activeIdx) {
                        v.pushPathFrame({
                            "",
                            std::meta::identifier_of(member),
                            std::addressof(node.[:member:])
                        });
                        TraceNativeHeapDetail::visitFieldValue(node.[:member:], v);
                        v.popPathFrame();
                    }
                    ++idx;
                }
            }
        } else {
            // No hook found — treat as an opaque leaf and skip.
            //
            // Ideally we would static_assert here to force every union to
            // declare either refTrackerActiveMemberIndex or a containing-class
            // refTrackerVisit.  However, Bloomberg p2996 clang has a known bug
            // where ADL-based sentinel detection (refTrackerHasVisitTag /
            // refTrackerHasActiveMemberTag) fails to resolve namespace-scope
            // overloads when the template is instantiated transitively from
            // inside a `template for` loop compiled with -freflection-latest.
            // In that build context ALL unqualified calls resolve only to the
            // TraceNativeHeapDetail fallback (returns false), so tagged types
            // appear un-tagged and the assert would fire spuriously.
            //
            // Silently skipping is safe: every union that actually holds a
            // tracked Strong<> should have a containing-class refTrackerVisit
            // that handles it before the walker ever descends into the union.
            (void)node;
        }
    }
    // else: fundamental / enum / incomplete type — no children
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
