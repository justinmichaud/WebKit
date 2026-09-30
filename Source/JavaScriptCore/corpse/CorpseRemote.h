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
#include <JavaScriptCore/CorpseError.h>
#include <JavaScriptCore/CorpseLimits.h>
#include <JavaScriptCore/CorpseSnapshot.h>
#include <JavaScriptCore/CorpseSnapshotDebugInfo.h>
#include <JavaScriptCore/CorpseTargetType.h>
#include <JavaScriptCore/CorpseTargetValue.h>
#include <optional>
#include <stddef.h>
#include <stdint.h>
#include <type_traits>
#include <wtf/HashSet.h>
#include <wtf/HashTraits.h>
#include <wtf/IterationStatus.h>
#include <wtf/SentinelLinkedList.h>
#include <wtf/StdLibExtras.h>
#include <wtf/Vector.h>

namespace JSC {
namespace Corpse {

// A T in a corpse. Its layout is the target's, from the target's debug info;
// T only names what a walker knows the value holds, and picks the RemoteTraits
// that say what it holds. A value the debug info gives no type to, such as a
// JSValue in an object's inline storage, is held by its address alone, and
// can be read whole but not by field.
template<typename T>
class Remote {
public:
    Remote() = default;
    Remote(Snapshot& snapshot, Address address)
        : m_snapshot(&snapshot)
        , m_address(address)
    {
    }
    explicit Remote(TargetValue&& value)
        : m_snapshot(&value.snapshot())
        , m_address(value.address())
        , m_value(WTF::move(value))
    {
    }

    // False for a null address: a null pointer is a state of the target, not a failure.
    explicit operator bool() const { return m_address && (!m_value || m_value->isValid()); }
    Address address() const { return m_address; }
    Snapshot& snapshot() const { return *m_snapshot; }
    const TargetType* type() const { return m_value ? &m_value->type() : nullptr; }
    const TargetValue* typed() const { return m_value ? &*m_value : nullptr; }

    // A field this value's class declares itself, and the `index`th of its
    // direct bases.
    template<typename U>
    Remote<U> field(const char* name) const
    {
        if (!requireType("field"))
            return { };
        return Remote<U>(m_value->properField(name));
    }

    template<typename U>
    Remote<U> base(size_t index) const
    {
        if (!requireType("base"))
            return { };
        TargetType::Layout layout = m_value->type().layout();
        auto* klass = std::get_if<TargetType::Class>(&layout);
        if (!klass || index >= klass->bases.size()) {
            if (*this)
                CORPSE_REPORT("Type '%s' has no base %zu", m_value->type().name(), index);
            return { };
        }
        return Remote<U>(m_value->base(klass->bases[index]));
    }

    std::optional<int64_t> integer() const
    {
        if (!requireType("integer"))
            return std::nullopt;
        return m_value->integer();
    }

    // The value whole, as a V of the same size.
    template<typename V>
    std::optional<V> as() const
    {
        if (m_value)
            return m_value->template as<V>();
        if (!*this)
            return std::nullopt;
        auto value = m_snapshot->memory().ptr<V>(m_address);
        if (!value) {
            CORPSE_REPORT("Could not read the %zu bytes at 0x%llx", sizeof(V), forReport());
            return std::nullopt;
        }
        return *value;
    }

    std::optional<Address> pointerValue() const
    {
        if (!requireType("pointerValue"))
            return std::nullopt;
        return m_value->pointerValue();
    }

    Remote<std::remove_cv_t<std::remove_pointer_t<T>>> dereference() const
    {
        if (!requireType("dereference"))
            return { };
        return Remote<std::remove_cv_t<std::remove_pointer_t<T>>>(m_value->dereference());
    }

    // For a pointer: what it would point at if it pointed at `address`.
    Remote<std::remove_cv_t<std::remove_pointer_t<T>>> pointeeAt(Address address) const
    {
        if (!requireType("pointeeAt"))
            return { };
        return Remote<std::remove_cv_t<std::remove_pointer_t<T>>>(m_value->pointeeAt(address));
    }

    // The same value `count` values on, for a buffer of them.
    Remote<T> offsetBy(size_t count) const
    {
        if (!requireType("offsetBy"))
            return { };
        return Remote<T>(m_value->offsetBy(count));
    }

    // The same kind of value at another address.
    Remote<T> at(Address address) const
    {
        if (m_value)
            return Remote<T>(m_value->at(address));
        if (!m_snapshot)
            return { };
        return Remote<T>(*m_snapshot, address);
    }

private:
    unsigned long long forReport() const { return m_address.toTargetVMAddress(); }

    // False, and reported once, when there is no type to do `what` with.
    bool requireType(const char* what) const
    {
        if (m_value)
            return true;
        if (*this)
            CORPSE_REPORT("The value at 0x%llx has no type, so it has no %s", forReport(), what);
        return false;
    }

    Snapshot* m_snapshot { nullptr };
    Address m_address;
    std::optional<TargetValue> m_value;
};

// What a value holds, told to a visitor one child at a time, as a cell's
// visitChildren tells the SlotVisitor. A specialization for a type says what
// it holds; a type without one holds nothing a walk follows. A visitor is
// anything with visit(const Remote<U>&) for the children it will be given.
template<typename T>
struct RemoteTraits {
    template<typename Visitor>
    static void visitChildren(const Remote<T>&, Visitor&) { }
};

template<typename T, typename Visitor>
void visitChildren(const Remote<T>& value, Visitor& visitor)
{
    RemoteTraits<T>::visitChildren(value, visitor);
}

// A pointer holds what it points at.
template<typename T>
struct RemoteTraits<T*> {
    template<typename Visitor>
    static void visitChildren(const Remote<T*>& pointer, Visitor& visitor)
    {
        if (auto pointee = pointer.dereference())
            visitor.visit(pointee);
    }
};

// A Vector holds its elements, wherever its buffer is.
template<typename T, size_t inlineCapacity, typename OverflowHandler, size_t minCapacity, typename Malloc>
struct RemoteTraits<Vector<T, inlineCapacity, OverflowHandler, minCapacity, Malloc>> {
    using VectorType = Vector<T, inlineCapacity, OverflowHandler, minCapacity, Malloc>;

    // Nullopt, and reported, for a count no Vector reaches.
    static std::optional<size_t> size(const Remote<VectorType>& vector)
    {
        auto size = storage(vector).template field<unsigned>("m_size").integer();
        if (!size)
            return std::nullopt;
        if (*size < 0 || *size > maxVectorSize) {
            CORPSE_REPORT("The Vector at 0x%llx claims %lld elements", static_cast<unsigned long long>(vector.address().toTargetVMAddress()), static_cast<long long>(*size));
            return std::nullopt;
        }
        return static_cast<size_t>(*size);
    }

    static Remote<T> element(const Remote<VectorType>& vector, size_t index)
    {
        return storage(vector).template field<T*>("m_buffer").dereference().offsetBy(index);
    }

    template<typename Visitor>
    static void visitChildren(const Remote<VectorType>& vector, Visitor& visitor)
    {
        auto size = RemoteTraits::size(vector);
        if (!size || !*size)
            return;
        Remote<T> first = element(vector, 0);
        for (size_t index = 0; index < *size; ++index)
            visitor.visit(first.offsetBy(index));
    }

private:
    // The buffer and count live in VectorBufferBase, the base of the Vector's base.
    static Remote<void> storage(const Remote<VectorType>& vector)
    {
        return vector.template base<void>(0).template base<void>(0);
    }
};

// A HashSet holds the values in its table's buckets that are neither empty nor
// deleted, as HashTable's iterators skip them.
template<typename Value, typename HashArg, typename TraitsArg, typename TableTraitsArg, WTF::ShouldValidateKey shouldValidateKey>
struct RemoteTraits<HashSet<Value, HashArg, TraitsArg, TableTraitsArg, shouldValidateKey>> {
    using SetType = HashSet<Value, HashArg, TraitsArg, TableTraitsArg, shouldValidateKey>;
    using ValueType = typename SetType::ValueType;
    static_assert(std::is_trivially_copyable_v<ValueType>, "A bucket is copied out of the target to test it");

    static constexpr int tableSizeOffset = -1; // HashTable::tableSizeOffset, in unsigneds before the buckets.

    // False, and reported, if the table cannot be read.
    template<typename Functor>
    static bool forEach(const Remote<SetType>& set, const Functor& functor)
    {
        Remote<ValueType*> table = set.template field<void>("m_impl").template field<ValueType*>("m_table");
        auto buckets = table.pointerValue();
        if (!buckets)
            return false;
        if (!*buckets)
            return true;
        auto tableSize = Remote<unsigned>(set.snapshot(), *buckets - static_cast<uint64_t>(-tableSizeOffset) * sizeof(unsigned)).template as<unsigned>();
        if (!tableSize)
            return false;
        if (*tableSize > maxHashTableSize) {
            CORPSE_REPORT("The HashSet at 0x%llx claims %u buckets", static_cast<unsigned long long>(set.address().toTargetVMAddress()), *tableSize);
            return false;
        }
        Remote<ValueType> first = table.dereference();
        for (unsigned index = 0; index < *tableSize; ++index) {
            Remote<ValueType> bucket = first.offsetBy(index);
            auto value = bucket.template as<ValueType>();
            if (!value)
                return false;
            // HashTable::isEmptyOrDeletedBucket, with a HashSet's IdentityExtractor.
            if (WTF::isHashTraitsEmptyValue<TraitsArg>(*value) || TraitsArg::isDeletedValue(*value))
                continue;
            if (functor(bucket) == IterationStatus::Done)
                return true;
        }
        return true;
    }
};

// A SentinelLinkedList holds the nodes from its sentinel's next back around
// to the sentinel, as its iterators visit them.
template<typename T, typename RawNode>
struct RemoteTraits<SentinelLinkedList<T, RawNode>> {
    using ListType = SentinelLinkedList<T, RawNode>;

    // Calls `functor` with each node, as a RawNode. False, and reported, if the list cannot be read.
    template<typename Functor>
    static bool forEach(const Remote<ListType>& list, const Functor& functor)
    {
        Remote<RawNode> sentinel = list.template field<RawNode>("m_sentinel");
        auto node = sentinel.template field<RawNode*>("m_next").pointerValue();
        for (unsigned count = 0; node; ++count) {
            if (*node == sentinel.address())
                return true;
            if (count >= maxLinkedListLength) {
                CORPSE_REPORT("The SentinelLinkedList at 0x%llx has more than %u nodes", static_cast<unsigned long long>(list.address().toTargetVMAddress()), maxLinkedListLength);
                return false;
            }
            Remote<RawNode> current = sentinel.at(*node);
            if (functor(current) == IterationStatus::Done)
                return true;
            node = current.template field<RawNode*>("m_next").pointerValue();
        }
        return false;
    }
};

} // namespace Corpse
} // namespace JSC

#endif // ENABLE(MYA)
