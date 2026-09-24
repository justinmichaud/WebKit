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

#if ENABLE(MYA_HEAP)

#include <JavaScriptCore/CorpseTargetDebugInfo.h>
#include <memory>
#include <optional>
#include <stdint.h>
#include <wtf/Ref.h>
#include <wtf/RefCounted.h>
#include <wtf/RefPtr.h>
#include <wtf/TZoneMalloc.h>
#include <wtf/Vector.h>
#include <wtf/text/CString.h>

namespace lldb {
class SBType;
}

namespace JSC {
namespace Corpse {

// A canonical type from the target's debug info, focused on layout info.
class TargetType : public RefCounted<TargetType> {
    WTF_MAKE_TZONE_ALLOCATED(TargetType);
public:
    ~TargetType();

    enum class Kind : uint8_t {
        Class,
        Pointer,
        Reference,
        Array,
        Integer,
        Float,
        Bool,
        Enum,
        Other,
    };
    Kind kind() const;

    CString name() const;
    size_t byteSize() const;

    bool isSigned() const;
    bool isPolymorphic() const;

    struct Field {
        CString name;
        size_t offset;
        Ref<TargetType> type;
    };
    struct Base {
        size_t offset;
        Ref<TargetType> type;
    };

    // Proper fields
    Vector<Field> fields() const;

    // Direct, non-virtual bases
    Vector<Base> bases() const;

    // Every virtual base, direct or indirect. This does not include any base subobject of a further-derived class.
    // CLAUDE: clean up this explanation, give two examples
    Vector<Base> virtualBases() const;

    // CLAUDE: this should only search proper fields, since full enumeration needs to search per parent class anyway. Call it properFields().
    std::optional<Field> field(const char* name) const;

    // CLAUDE: these are confusing. This should let us std::visit each case instead. Types and values should be walked up the inheritance chain, to avoid this "sometimes yes sometimes no" field list situation, and to avoid confusion with multiple copies of the same base field from diamond inheritance.
    RefPtr<TargetType> pointee() const; // Pointer and Reference.
    RefPtr<TargetType> element() const; // Array.
    size_t elementCount() const;

    // CLAUDE: is this needed?
    TargetDebugInfo& debugInfo() const { return m_debugInfo.get(); }

private:
    TargetType(TargetDebugInfo&, const lldb::SBType&);

    Ref<TargetType> wrap(const lldb::SBType&) const;

    Ref<TargetDebugInfo> m_debugInfo;
    std::unique_ptr<lldb::SBType> m_type;

    friend class TargetDebugInfo;
};

} // namespace Corpse
} // namespace JSC

#endif // ENABLE(MYA_HEAP)
