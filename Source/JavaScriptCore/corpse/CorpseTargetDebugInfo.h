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

#include <JavaScriptCore/CorpseAddress.h>
#include <memory>
#include <wtf/Ref.h>
#include <wtf/RefCounted.h>
#include <wtf/RefPtr.h>
#include <wtf/TZoneMalloc.h>

namespace lldb {
class SBDebugger;
class SBTarget;
class SBTypeList;
}

namespace JSC {
namespace Corpse {

enum class Materialization : uint8_t;
class Snapshot;
class TargetType;
template<Materialization> class TargetValue;

// CLAUDE: since this is per snapshot, this should be SnapshotDebugInfo or similar.
class TargetDebugInfo : public RefCounted<TargetDebugInfo> {
    WTF_MAKE_TZONE_ALLOCATED(TargetDebugInfo);
public:

    static RefPtr<TargetDebugInfo> create(const Snapshot&);
    ~TargetDebugInfo();

    // CLAUDE: this should require an image, so we can completely rule out ambiguity.
    RefPtr<TargetType> findType(const char* qualifiedName);
    RefPtr<TargetType> findType(const char* qualifiedName, const char* imagePath);

    // claude: why needed?
    unsigned moduleCount() const;

private:
    TargetDebugInfo(std::unique_ptr<lldb::SBDebugger>&&, std::unique_ptr<lldb::SBTarget>&&);

    // The type as the image mapped at `address` defines it.
    // CLAUDE: confirm rtti does not require us to search by name, there must be something more robust?
    RefPtr<TargetType> findTypeInImageContaining(Address, const char* qualifiedName);

    // The one complete definition among `candidates`, or null after saying how many there were.
    // CLAUDE: again, confusing and unnecesary
    RefPtr<TargetType> onlyCompleteType(lldb::SBTypeList& candidates, const char* qualifiedName, const char* where);

    std::unique_ptr<lldb::SBDebugger> m_debugger;
    std::unique_ptr<lldb::SBTarget> m_target;

    friend class TargetType;
    template<Materialization> friend class TargetValue;
};

} // namespace Corpse
} // namespace JSC

#endif // ENABLE(MYA_HEAP)
