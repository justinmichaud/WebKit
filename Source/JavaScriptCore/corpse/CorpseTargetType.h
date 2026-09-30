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
#include <wtf/Ref.h>
#include <wtf/RefCounted.h>
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

// A canonical type from a snapshot's debug info, described by its layout.
class TargetType : public RefCounted<TargetType> {
    WTF_MAKE_TZONE_ALLOCATED(TargetType);
public:
    ~TargetType();

    UTF8CString name() const;
    size_t byteSize() const;

    SnapshotDebugInfo& debugInfo() const { return m_debugInfo; }

    struct Field {
        UTF8CString name;
        size_t offset;
        Ref<TargetType> type;
        // A bitfield starts bitOffset bits into the byte at `offset`; bitSize is 0 for any other field.
        unsigned bitOffset { 0 };
        unsigned bitSize { 0 };
    };
    struct Base {
        size_t offset;
        Ref<TargetType> type;
    };

    struct Class {
        bool isPolymorphic;
        Vector<Field> properFields; // Declared by this class itself, not by a base.
        Vector<Base> bases; // Direct and non-virtual.
        Vector<Base> virtualBases; // Direct and indirect, at their offsets in a complete object of this class.
    };
    struct Pointer {
        Ref<TargetType> pointee; // Pointers and references.
    };
    struct Integer {
        bool isSigned; // Integers, enumerations and bool.
    };
    struct Other { }; // Anything else, readable only as bytes.
    using Layout = Variant<Class, Pointer, Integer, Other>;

    Layout layout() const;

private:
    TargetType(SnapshotDebugInfo&, const lldb::SBType&);

    Ref<TargetType> wrap(const lldb::SBType&) const;

    const Ref<SnapshotDebugInfo> m_debugInfo;
    const std::unique_ptr<lldb::SBType> m_type;

    friend class SnapshotDebugInfo;
};

} // namespace Corpse
} // namespace JSC

#endif // ENABLE(MYA)
