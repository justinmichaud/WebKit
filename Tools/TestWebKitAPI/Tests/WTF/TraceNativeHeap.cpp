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

#include "config.h"

#if ENABLE(REFTRACKER)

#include <wtf/Deque.h>
#include <wtf/DoublyLinkedList.h>
#include <wtf/HashMap.h>
#include <wtf/HashSet.h>
#include <wtf/ListHashSet.h>
#include <wtf/Ref.h>
#include <wtf/RefCounted.h>
#include <wtf/RefPtr.h>
#include <wtf/StackShot.h>
#include <wtf/TraceNativeHeap.h>
#include <wtf/Vector.h>

namespace TestWebKitAPI {

namespace {

// Tags — forward-declared so we can use them as template arguments freely.
struct DirectTag;
struct VectorTag;
struct HashMapTag;
struct DequeTag;
struct RefPtrTag;
struct UniqueRefTag;
struct UniquePtrTag;
struct RawTag;
struct LinkedTag;
struct UnreachableTag;

// Target template whose specializations the walker looks for.
template<typename Tag>
struct Target {
    int value;
};

// RefCounted wrapper so we can put Targets inside Ref/RefPtr.
template<typename Tag>
struct RefTarget : public RefCounted<RefTarget<Tag>> {
    static Ref<RefTarget> create(int v) { return adoptRef(*new RefTarget(v)); }
    Target<Tag> inner;
    RefTarget(int v) : inner { v } { }
};

// Plain wrapper (not RefCounted) for UniqueRef/unique_ptr.
template<typename Tag>
struct PlainTarget {
    Target<Tag> inner;
    PlainTarget(int v) : inner { v } { }
};

struct LinkedNode {
    LinkedNode* m_prev { nullptr };
    LinkedNode* m_next { nullptr };

    // DoublyLinkedList<LinkedNode> expects these two accessors.
    LinkedNode* prev() const { return m_prev; }
    LinkedNode* next() const { return m_next; }
    void setPrev(LinkedNode* p) { m_prev = p; }
    void setNext(LinkedNode* n) { m_next = n; }

    Target<LinkedTag> payload;
};

// A Strong-like tracked class that carries a StackTraceProvider so we can
// verify the walker finds it via template-of matching and that the trace
// wrapper captured a birth-site backtrace.
template<typename T>
struct TrackedBox {
    T value;
    static bool isRefTrackerMixinEnabled() { return true; }
    [[no_unique_address]] WTF::StackTraceProvider<TrackedBox> m_stackTraceProvider;
};

struct Graph {
    // Direct class-type member.
    Target<DirectTag> direct { 1 };

    // Iterable containers of Targets.
    Vector<Target<VectorTag>> vectorTargets;
    HashMap<int, Target<HashMapTag>> hashMapTargets;
    HashSet<int> unrelatedInts;  // ensures fundamental iterables are leaves
    Deque<Target<DequeTag>> dequeTargets;
    ListHashSet<int> unrelatedSet; // ditto

    // Smart pointers holding boxed Targets.
    RefPtr<RefTarget<RefPtrTag>> refPtrBox;              // RefCounted box
    std::unique_ptr<PlainTarget<UniqueRefTag>> stdUniquePtrA; // exercises std::unique_ptr #1
    std::unique_ptr<PlainTarget<UniquePtrTag>> uniquePtrBox;  // exercises std::unique_ptr #2

    // Raw pointer.
    Target<RawTag>* rawPointerTarget { nullptr };

    // Intrusive list, exercised via field-walking (no begin/end).
    DoublyLinkedList<LinkedNode> linkedList;

    // Tracked-box instance for StackTraceProvider verification.
    TrackedBox<int> tracked { 99, { } };

    // Cycle-detection stress: pointer back to self.
    Graph* selfCycle { nullptr };

    Graph() = default;
};

} // anonymous namespace

TEST(WTF_TraceNativeHeap, FindsEveryReachableTargetSpecialization)
{
    Graph g;

    g.vectorTargets.append(Target<VectorTag> { 100 });
    g.vectorTargets.append(Target<VectorTag> { 101 });
    g.vectorTargets.append(Target<VectorTag> { 102 });

    g.hashMapTargets.add(1, Target<HashMapTag> { 200 });
    g.hashMapTargets.add(2, Target<HashMapTag> { 201 });

    g.unrelatedInts.add(7);
    g.unrelatedInts.add(8);

    g.dequeTargets.append(Target<DequeTag> { 300 });
    g.dequeTargets.append(Target<DequeTag> { 301 });

    g.unrelatedSet.add(9);

    g.refPtrBox = RefTarget<RefPtrTag>::create(400);
    g.stdUniquePtrA = std::make_unique<PlainTarget<UniqueRefTag>>(10);
    g.uniquePtrBox = std::make_unique<PlainTarget<UniquePtrTag>>(600);

    auto rawStorage = std::make_unique<Target<RawTag>>(Target<RawTag> { 700 });
    g.rawPointerTarget = rawStorage.get();

    LinkedNode a { nullptr, nullptr, Target<LinkedTag> { 800 } };
    LinkedNode b { nullptr, nullptr, Target<LinkedTag> { 801 } };
    g.linkedList.append(&a);
    g.linkedList.append(&b);

    g.selfCycle = &g;

    // Separately, construct an unreachable Target that the walker must NOT
    // find. It lives on the stack but nothing points to it from `g`.
    Target<UnreachableTag> unreachable { 9999 };
    (void)unreachable;

    Vector<int> foundValues;
    WTF::traceNativeHeap<^^Target>(g, [&](auto& t) {
        foundValues.append(t.value);
    });

    std::sort(foundValues.begin(), foundValues.end());

    // Expected: every Target reachable from `g`, each exactly once.
    // Note: the Unreachable tag has value 9999 and must be absent.
    // Note: tracked.value is an `int`, not a Target — it won't appear.
    Vector<int> expected {
        1,             // g.direct
        100, 101, 102, // vectorTargets
        200, 201,      // hashMapTargets.value (Target<HashMapTag>)
        300, 301,      // dequeTargets
        400,           // refPtrBox.inner (through Ref<RefTarget>)
        10,            // stdUniquePtrA.inner
        600,           // uniquePtrBox.inner
        700,           // rawPointerTarget
        800, 801,      // linkedList nodes' payload
    };
    std::sort(expected.begin(), expected.end());

    EXPECT_EQ(foundValues.size(), expected.size());
    EXPECT_EQ(foundValues, expected);

    // Unreachable target must not be present.
    EXPECT_TRUE(std::ranges::find(foundValues, 9999) == foundValues.end());
}

TEST(WTF_TraceNativeHeap, FindsTrackedBoxAndReadsStackTrace)
{
    struct Outer {
        TrackedBox<int> a { 1, { } };
        TrackedBox<int> b { 2, { } };
    };
    Outer outer;

    Vector<int> found;
    Vector<size_t> traceSizes;
    WTF::traceNativeHeap<^^TrackedBox>(outer, [&](auto& box) {
        found.append(box.value);
        traceSizes.append(box.m_stackTraceProvider.frames().size());
    });

    std::sort(found.begin(), found.end());
    EXPECT_EQ(found.size(), 2u);
    EXPECT_EQ(found[0], 1);
    EXPECT_EQ(found[1], 2);

    // isRefTrackerMixinEnabled() returns true, so every TrackedBox must have
    // a non-empty captured trace.
    for (auto n : traceSizes)
        EXPECT_GT(n, 0u);
}

TEST(WTF_TraceNativeHeap, CycleDetection)
{
    struct Node {
        Target<struct CycleTag> payload;
        Node* self { nullptr };
    };
    Node n { { 42 }, nullptr };
    n.self = &n;   // immediate self-loop

    int count = 0;
    WTF::traceNativeHeap<^^Target>(n, [&](auto& t) {
        EXPECT_EQ(t.value, 42);
        ++count;
    });
    EXPECT_EQ(count, 1);
}

} // namespace TestWebKitAPI

#endif // ENABLE(REFTRACKER)
