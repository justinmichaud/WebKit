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
    Remote(const Snapshot& snapshot, Address address)
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
    const Snapshot& snapshot() const { return *m_snapshot; }
    const TargetType* type() const { return m_value ? &m_value->type() : nullptr; }
    const TargetValue* typed() const { return m_value ? &*m_value : nullptr; }

    // The target's pointer size, or 0 for a value without a type.
    unsigned addressByteSize() const { return m_value ? m_value->type().debugInfo().addressByteSize() : 0; }

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
        auto value = m_snapshot->read<V>(m_address);
        if (!value)
            CORPSE_REPORT("Could not read the %zu bytes at 0x%llx", sizeof(V), forReport());
        return value;
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
            CORPSE_REPORT("The value at 0x%llx has no type, so it has no %s", forReport(), reportableString(what));
        return false;
    }

    const Snapshot* m_snapshot { nullptr };
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

template<typename Functor>
struct FunctorRemoteVisitor {
    const Functor& functor;

    template<typename U>
    void visit(const Remote<U>& child)
    {
        functor(child);
    }
};

// visitChildren with a function in place of a visitor, for a walk that treats
// every child alike.
template<typename T, typename Functor>
void forEachChild(const Remote<T>& value, const Functor& functor)
{
    FunctorRemoteVisitor<Functor> visitor { functor };
    visitChildren(value, visitor);
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

} // namespace Corpse
} // namespace JSC

#endif // ENABLE(MYA)
