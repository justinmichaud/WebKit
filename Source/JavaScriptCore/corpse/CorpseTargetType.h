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

#include <JavaScriptCore/CorpseSnapshotDebugInfo.h>
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

// A canonical type from a snapshot's debug info, described by its layout.
class TargetType : public RefCounted<TargetType> {
    WTF_MAKE_TZONE_ALLOCATED(TargetType);
public:
    ~TargetType();

    CString name() const;
    size_t byteSize() const;
    SnapshotDebugInfo& debugInfo() const { return m_debugInfo; }

    struct Field {
        CString name;
        size_t offset;
        Ref<TargetType> type;
    };
    struct Base {
        size_t offset;
        Ref<TargetType> type;
    };

    struct Class {
        bool isPolymorphic;
        Vector<Field> properFields; // Declared by this class itself, not by a base.
        Vector<Base> bases; // Direct and non-virtual.
        // Every virtual base, direct or indirect, at its offset in a complete
        // object of this class. Given
        //     struct Left : virtual VBase { }; struct Right : virtual VBase { };
        //     struct Diamond : Left, Right { }; struct Deeper : Diamond { };
        // Left lists VBase, Diamond lists it once although both of its bases
        // bring it in, and Deeper lists it although none of its direct bases
        // is virtual. Where VBase sits in the Left subobject of a Diamond only
        // Diamond's layout says, so a value reaches a virtual base through
        // its dynamic type.
        Vector<Base> virtualBases;
    };
    struct Pointer {
        Ref<TargetType> pointee; // Pointers and references.
    };
    struct Array {
        Ref<TargetType> element;
        size_t count;
    };
    struct Integer {
        bool isSigned; // Integers, enumerations and bool.
    };
    struct Other { }; // Anything else, readable only as bytes.
    using Layout = Variant<Class, Pointer, Array, Integer, Other>;

    Layout layout() const;

private:
    TargetType(SnapshotDebugInfo&, const lldb::SBType&);

    Ref<TargetType> wrap(const lldb::SBType&) const;

    const Ref<SnapshotDebugInfo> m_debugInfo;
    const std::unique_ptr<lldb::SBType> m_type;

    friend class SnapshotDebugInfo;
    friend class TargetValue;
};

} // namespace Corpse
} // namespace JSC

#endif // ENABLE(MYA)
