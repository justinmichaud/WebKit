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
#include <wtf/RefCounted.h>
#include <wtf/RefPtr.h>
#include <wtf/TZoneMalloc.h>

namespace lldb {
class SBDebugger;
class SBModule;
class SBTarget;
}

namespace JSC {
namespace Corpse {

class Image;
class Snapshot;
class TargetType;
class TargetValue;

// The debug info of every image in a snapshot, opened through liblldb. liblldb
// only ever reads files here: it is never given a process.
class SnapshotDebugInfo : public RefCounted<SnapshotDebugInfo> {
    WTF_MAKE_TZONE_ALLOCATED(SnapshotDebugInfo);
public:
    static RefPtr<SnapshotDebugInfo> create(const Snapshot&);
    ~SnapshotDebugInfo();

    // The type as `image` defines it. A header-only type is complete in every
    // image that uses it, so a type is always asked for by its image.
    RefPtr<TargetType> findType(const char* qualifiedName, const Image&);

private:
    SnapshotDebugInfo(std::unique_ptr<lldb::SBDebugger>&&, std::unique_ptr<lldb::SBTarget>&&);

    RefPtr<TargetType> findType(const char* qualifiedName, lldb::SBModule&);

    // The type an object's RTTI names, as the image holding its vtable defines
    // it. Debug info has no link from a vtable's address to its class, so this
    // is a lookup by name, which is also how lldb's Itanium ABI runtime resolves
    // a dynamic type.
    RefPtr<TargetType> findTypeForVTable(Address vtable, const char* demangledName);

    std::unique_ptr<lldb::SBDebugger> m_debugger;
    std::unique_ptr<lldb::SBTarget> m_target;

    friend class TargetValue;
};

} // namespace Corpse
} // namespace JSC

#endif // ENABLE(MYA_HEAP)
