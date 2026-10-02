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

#include <JavaScriptCore/CorpsePlatform.h>

#if ENABLE(MYA)

#include <JavaScriptCore/CorpseAddress.h>
#include <JavaScriptCore/CorpseRemote.h>
#include <JavaScriptCore/CorpseSnapshot.h>
#include <JavaScriptCore/CorpseSnapshotDebugInfo.h>
#include <JavaScriptCore/CorpseTargetValue.h>
#include <JavaScriptCore/HeapCell.h>
#include <JavaScriptCore/JSCJSValue.h>
#include <JavaScriptCore/MarkedBlock.h>
#include <array>
#include <optional>
#include <stdint.h>
#include <utility>
#include <wtf/BitSet.h>
#include <wtf/Function.h>
#include <wtf/HashMap.h>
#include <wtf/HashSet.h>
#include <wtf/IterationStatus.h>
#include <wtf/RefPtr.h>
#include <wtf/Vector.h>
#include <wtf/text/ASCIILiteral.h>
#include <wtf/text/WTFString.h>

namespace JSC {

class BlockDirectory;
struct ClassInfo;
class Heap;
class JSCell;
class MarkedSpace;
class Structure;
class VM;

namespace Corpse {

struct MarkedSpaceState;
class ReachWalk;

// The JS heap of a VM in a corpse, walked through the target's debug info.
// The walk starts from a roots object, whose type comes from its vtable. Every
// other type it reads is the type of a field reached from there, of a global
// variable, or of a class in mya's lists of JavaScriptCore's classes, found by
// name in JavaScriptCore's image, so the layouts are the target's whatever it
// was built for.
class HeapWalk {
public:
    struct Cell {
        Address address;
        uint64_t size;
        HeapCell::Kind kind;
    };

    // `roots` is an object of a polymorphic class with the fields `JSC::VM* vm`
    // and `bool atSafePoint`. Invalid, and reported, if the roots name no VM,
    // the snapshot has no debug info, or JavaScriptCore's image lacks a class
    // the walk needs by name.
    HeapWalk(Snapshot&, Address roots);

    bool isValid() const { return static_cast<bool>(m_vm); }
    Remote<VM> vm() const { return m_vm; }
    // The roots, as their dynamic type. Null if the walk is invalid.
    const TargetValue* roots() const { return isValid() ? &*m_roots : nullptr; }

    // Whether the roots say the target was at mya's safe point when it was
    // snapshotted, where no collection runs. Only then are the walk's results
    // exact; on any other snapshot they are unverified.
    bool isAtSafePoint() const { return m_isAtSafePoint; }

    // The cells MarkedSpace::forEachLiveCell would visit in the target inside
    // a HeapIterationScope, which first stops every allocator, with the
    // liveness rules of a heap between collections.
    void forEachLiveCell(const Function<IterationStatus(const Cell&)>&) const;

    // The cell at `address`, as a JSCell, and the Structure a cell's
    // StructureID bits name.
    Remote<JSCell> jsCell(Address) const;
    Remote<Structure> structure(uint32_t structureIDBits) const;

    // The C++ class of the JS cell at `address`: the class in mya's list of
    // cell classes (CorpseCellClasses.h) whose s_info is its Structure's
    // ClassInfo, checked against that ClassInfo's size and parent. Null,
    // having reported why once per ClassInfo, if there is none.
    const TargetType* cellClass(Address) const;

    // The s_info symbols of JavaScriptCore's image whose class is not in
    // mya's list of cell classes, by name: each needs a line in the list.
    Vector<String> unlistedCellClasses() const;
    // How many classes of the list JavaScriptCore's image has.
    size_t listedCellClassCount() const;

    using AtomBits = WTF::BitSet<MarkedBlock::atomsPerBlock>;

    struct Allocation {
        Address address;
        uint64_t size;
    };
    // Why the walk did not follow a value that may point somewhere.
    enum class NotFollowed : uint8_t {
        Declaration, // A pointer to a class that is only declared.
        VoidPointer, // A pointer to a type without a size, such as void.
        NoDynamicType, // A pointer to a polymorphic class whose object has no dynamic type; followed as its static type.
        CellWithoutClass, // A JS cell whose ClassInfo names no class.
        TypeNotReached, // An encoded pointer whose pointee type the walk does not have.
    };
    static constexpr size_t numberOfNotFollowedReasons = static_cast<size_t>(NotFollowed::TypeNotReached) + 1;

    // What holds an allocation the walk missed, as the walk finds it: the
    // first of these that holds a word whose value lies inside the allocation.
    enum class MissCause : uint8_t {
        Reached, // A word of an allocation the walk reached: a field it does not read as a pointer, or bytes no type it read covers.
        StaticData, // A word in an image's data, in a symbol the walk does not read.
        OtherMemory, // A word anywhere else, such as libpas's metadata.
        Missed, // Only words of other missed allocations: it is part of what they hold.
        NoReferrer, // No word anywhere: it is leaked, or held only in a form that is not a pointer.
    };
    static constexpr size_t numberOfMissCauses = static_cast<size_t>(MissCause::NoReferrer) + 1;

    struct Miss {
        Allocation allocation;
        MissCause cause;
        String holder; // What holds it: a field, a symbol, a thread, a region or a missed allocation.
        String content; // Its dynamic type, if it starts with a polymorphic object.
        // Where the words that hold it are, the first few found, whatever their cause.
        Vector<Address> referrers;
        bool isExcluded;
        uint64_t bytesHeld; // Its own and those of the misses that only it holds, through other misses.
    };

    // An allocation with bytes the walk read as no type.
    struct Untyped {
        Allocation allocation;
        uint64_t bytesUntyped;
        String typeAtStart; // The type the walk read at the allocation's start, if any.
    };

    struct Reach {
        uint64_t bytesAllocated { 0 };
        uint64_t bytesExcluded { 0 }; // Of the allocations the caller excludes.
        uint64_t bytesReached { 0 }; // Of the allocations the walk reaches any address in.
        uint64_t bytesTyped { 0 }; // Read as some type, each byte once.
        uint64_t bytesFreeInBlocks { 0 }; // In MarkedBlocks, outside their live cells and headers.

        // How much of the program's own memory the walk reaches, and reads as a type.
        double percent() const { return ratio(bytesReached, bytesAllocated - bytesExcluded); }
        double typedPercent() const { return ratio(bytesTyped, bytesAllocated - bytesExcluded - bytesFreeInBlocks); }

        // Every missed allocation, with what holds it, the most bytes held first.
        Vector<Miss> misses;
        // The bytes of the missed allocations that are not excluded, by cause.
        std::array<uint64_t, numberOfMissCauses> bytesMissedByCause { };
        uint64_t bytesMissed() const
        {
            uint64_t total = 0;
            for (uint64_t bytes : bytesMissedByCause)
                total += bytes;
            return total;
        }
        Vector<Untyped> mostUntyped; // Of the allocations reached, most untyped bytes first.
        Vector<bool> isReached; // For each allocation, in the order given.
        Vector<uint64_t> bytesTypedIn; // Likewise.
        // Each value whose type runs past the end of its allocation, with the field that led to it.
        Vector<String> overruns;
        std::array<uint64_t, numberOfNotFollowedReasons> notFollowed { };
        uint64_t cellsWithClass { 0 };
        uint64_t cellBytesBeyondClass { 0 }; // In cells bigger than their class, such as objects with inline storage.
        uint64_t globalVariables { 0 }; // Walked as roots.
        uint64_t untypedDataSymbols { 0 }; // Data symbols that are no variable the debug info describes, such as vtables.

    private:
        static double ratio(uint64_t part, uint64_t whole) { return whole ? 100.0 * part / whole : 0; }
    };

    // How much of `allocations`, sorted by address, the walk reaches and
    // types: every live cell, each JS cell as its C++ class, and every C++
    // object reached from the roots, a global variable, a block's header or a
    // cell through a pointer whose pointee type is known and that lands inside
    // an allocation. The allocations that overlap `excluded`, also sorted, are
    // left out of the percentages. Every miss is explained, except that no
    // word in `notReferrers`, also sorted, holds one: they are the caller's
    // own records of the heap. The `listCount` most untyped allocations are listed.
    Reach reach(const Vector<Allocation>&, const Vector<Allocation>& excluded, const Vector<Allocation>& notReferrers, size_t listCount) const;

private:
    friend class ReachWalk;
    using BlockSet = UncheckedKeyHashSet<MarkedBlock*>;

    Remote<ClassInfo> classInfoOf(Address cell) const;
    // The classes of mya's list of cell classes, by the address of their s_info.
    const HashMap<uint64_t, const TargetType*>& cellClasses() const;
    bool isClassInfoOf(const Remote<ClassInfo>&, const TargetType&, const HashMap<uint64_t, const TargetType*>&) const;

    Remote<MarkedBlock::Header> header(Address block) const;
    bool forEachBlock(const Function<IterationStatus(Address block)>&) const;
    std::optional<HashMap<uint64_t, AtomBits>> stopAllocating(const Remote<MarkedSpace>&) const;
    IterationStatus walkBlock(Address block, const HashMap<uint64_t, AtomBits>& newlyAllocatedAfterStop, const MarkedSpaceState&, const Function<IterationStatus(const Cell&)>&) const;
    IterationStatus walkPreciseAllocations(const Remote<MarkedSpace>&, const Function<IterationStatus(const Cell&)>&) const;

    RefPtr<SnapshotDebugInfo> m_debugInfo;
    std::optional<TargetValue> m_roots;
    Remote<VM> m_vm;
    bool m_isAtSafePoint { false };
    std::optional<TargetValue> m_structure; // Any Structure, to retype from.
    std::optional<TargetValue> m_jsCell; // Any JSCell, to retype from.
    Address m_javaScriptCoreImage; // An address in JavaScriptCore's image, where the classes it needs by name are looked up.
    // Classes the walk computes an address for, rather than reading one.
    const TargetType* m_blockHeaderClass { nullptr };
    const TargetType* m_localAllocatorClass { nullptr };
    const TargetType* m_stringImplClass { nullptr }; // For JSString::m_fiber, a uintptr_t.
    // For CodeBlock::m_jitData, a void*. Null in a build without that tier.
    const TargetType* m_baselineJITDataClass { nullptr };
    const TargetType* m_dfgJITDataClass { nullptr };
    const TargetType* m_fatEntryClass { nullptr }; // For SymbolTableEntry::m_bits, an intptr_t.
    // WebConfig::g_config, words that WTF and JavaScriptCore read as their Config classes.
    std::optional<Address> m_config;
    const TargetType* m_wtfConfigClass { nullptr };
    const TargetType* m_jscConfigClass { nullptr };
    uint64_t m_startOfStructureHeap { 0 };
    mutable std::optional<HashMap<uint64_t, const TargetType*>> m_cellClasses;
    mutable HashSet<uint64_t> m_reportedClassInfos;
};

// A JSValue holds its cell, if it is one, as SlotVisitor::appendUnbarriered(JSValue) visits it.
template<>
struct RemoteTraits<JSValue> {
    template<typename Visitor>
    static void visitChildren(const Remote<JSValue>& value, Visitor& visitor)
    {
        auto bits = value.as<EncodedJSValue>();
        if (!bits)
            return;
        JSValue decoded = JSValue::decode(*bits);
        if (!decoded || !decoded.isCell())
            return;
        Address cell { static_cast<uint64_t>(*bits) };
        visitor.visit(Remote<JSCell>(value.snapshot(), cell));
    }
};

} // namespace Corpse
} // namespace JSC

#endif // ENABLE(MYA)
