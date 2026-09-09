/*
 * Copyright (C) 2026 Apple Inc. All rights reserved.
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

#if HAVE(CORPSE_SUPPORT)

#include <JavaScriptCore/CorpseAddress.h>
#include <JavaScriptCore/CorpseTargetType.h>
#include <optional>
#include <span>
#include <wtf/Vector.h>
#include <wtf/text/StringView.h>
#include <wtf/text/WTFString.h>

namespace JSC {
namespace Corpse {

// One object in the target, bound to a type. The bytes at `base` are read once
// at construction, then sliced by field name. A caller reads the value of a
// field as a byte span and interprets it by looking at the field's typeName
// and byteSize; there is no compile-time templating on the target's C++ types.
//
// A TargetObject that exists has already cleared the type system's vptr check:
// for a polymorphic type, getTargetObject returned nullopt unless the vptr in
// the corpse belongs to the type asked for or to something derived from it. A
// non-polymorphic type has no vptr to check, so its objects only need the bytes
// to read.
class TargetObject {
public:
    TargetObject(Address, TargetType, Vector<uint8_t>&&, String dynamicTypeName = { });

    Address base() const { return m_base; }

    // The type this object was read as, whose layout fields() describes.
    const TargetType& type() const { return m_type; }

    // What the object actually is, when the vptr named something more derived
    // than the type it was read as. Empty when the two are the same, or when
    // the type is not polymorphic.
    //
    // This is what makes a walk over a heap say something: reaching a
    // `JSC::JSCell*` and being told the object is a `JSC::JSString` is the
    // answer, and reading it as a JSCell is still correct because a derived
    // object opens with its base.
    const String& dynamicTypeName() const { return m_dynamicTypeName; }
    bool isDerivedType() const { return !m_dynamicTypeName.isEmpty(); }
    uint64_t size() const { return m_type.byteSize(); }
    const Vector<TargetField>& fields() const { return m_type.fields(); }

    // Returns the value of `fieldName` as a span into this object's bytes, or
    // nullopt when the type has no such field. The span is valid for the
    // lifetime of the TargetObject.
    std::optional<std::span<const uint8_t>> get(StringView fieldName) const;

    // Overload for a caller that already has the TargetField in hand.
    std::span<const uint8_t> get(const TargetField&) const;

private:
    Address m_base;
    TargetType m_type;
    Vector<uint8_t> m_bytes;
    String m_dynamicTypeName;
};

} // namespace Corpse
} // namespace JSC

#endif // HAVE(CORPSE_SUPPORT)
