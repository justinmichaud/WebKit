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

#include "config.h"
#include "CorpseTargetObject.h"

#if HAVE(CORPSE_SUPPORT)

#include <wtf/Assertions.h>
#include <wtf/StdLibExtras.h>

namespace JSC {
namespace Corpse {

TargetObject::TargetObject(Address base, TargetType type, Vector<uint8_t>&& bytes,
    String dynamicTypeName)
    : m_base(base)
    , m_type(WTF::move(type))
    , m_bytes(WTF::move(bytes))
    , m_dynamicTypeName(WTF::move(dynamicTypeName))
{
}

std::span<const uint8_t> TargetObject::get(const TargetField& field) const
{
    RELEASE_ASSERT(field.byteOffset + field.byteSize <= m_bytes.size());
    return m_bytes.span().subspan(field.byteOffset, field.byteSize);
}

std::optional<std::span<const uint8_t>> TargetObject::get(StringView fieldName) const
{
    const auto* field = m_type.field(fieldName);
    if (!field)
        return std::nullopt;
    return get(*field);
}

} // namespace Corpse
} // namespace JSC

#endif // HAVE(CORPSE_SUPPORT)
