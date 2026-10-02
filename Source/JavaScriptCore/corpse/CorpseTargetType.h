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
#include <memory>
#include <optional>
#include <wtf/Noncopyable.h>
#include <wtf/TZoneMalloc.h>
#include <wtf/Variant.h>
#include <wtf/Vector.h>
#include <wtf/text/CString.h>

namespace lldb {
class SBType;
}

namespace JSC {
namespace Corpse {

class SnapshotDebugInfo;

// A canonical type from a snapshot's debug info, described by its layout. The
// SnapshotDebugInfo makes one TargetType per type, and owns it.
class TargetType {
    WTF_MAKE_TZONE_ALLOCATED(TargetType);
    WTF_MAKE_NONCOPYABLE(TargetType);
public:
    ~TargetType();

    UTF8CString name() const;
    // Read the first time it is asked for: liblldb completes a class to size
    // it, which a pointee the walk never follows does not need.
    size_t byteSize() const { return m_byteSize ? *m_byteSize : readByteSize(); }
    size_t alignment() const; // alignof the type.

    SnapshotDebugInfo& debugInfo() const { return m_debugInfo; }

    struct Field {
        UTF8CString name;
        size_t offset;
        const TargetType& type;
        // A bitfield starts bitOffset bits into the byte at `offset`; bitSize is 0 for any other field.
        unsigned bitOffset { 0 };
        unsigned bitSize { 0 };
    };
    struct Base {
        size_t offset;
        const TargetType& type;
    };

    struct Class {
        bool isPolymorphic;
        Vector<Field> properFields; // Declared by this class itself, not by a base.
        Vector<Base> bases; // Direct and non-virtual.
        Vector<Base> virtualBases; // Direct and indirect, at their offsets in a complete object of this class.
    };
    struct Pointer {
        const TargetType& pointee; // Pointers and references.
    };
    struct Array {
        const TargetType& element;
        size_t count; // Zero for an array of unknown bound.
    };
    struct Integer {
        bool isSigned; // Integers, enumerations and bool.
        bool isEnumeration;
    };
    struct Other { }; // Anything else, readable only as bytes.
    using Layout = Variant<Class, Pointer, Array, Integer, Other>;

    // Read from the debug info the first time it is asked for.
    const Layout& layout() const;

    // The type of the `index`th template argument of a class template
    // specialization, or null if it has no such type argument.
    const TargetType* templateArgument(unsigned index) const;
    // The `index`th template argument, if it is an integer, such as PackedAlignedPtr's alignment.
    std::optional<uint64_t> templateIntegerArgument(unsigned index) const;

private:
    TargetType(SnapshotDebugInfo&, const lldb::SBType&);

    Layout readLayout() const;
    size_t readByteSize() const;

    SnapshotDebugInfo& m_debugInfo;
    const std::unique_ptr<lldb::SBType> m_type;
    mutable std::optional<size_t> m_byteSize;
    mutable std::optional<Layout> m_layout;

    friend class SnapshotDebugInfo;
};

} // namespace Corpse
} // namespace JSC

#endif // ENABLE(MYA)
