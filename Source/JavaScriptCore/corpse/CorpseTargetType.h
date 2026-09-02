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

#if (OS(MACOS) || USE(APPLE_INTERNAL_SDK)) && !PLATFORM(MACCATALYST) && !PLATFORM(IOS_FAMILY_SIMULATOR)

#include <JavaScriptCore/CorpseAddress.h>
#include <cstdint>
#include <memory>
#include <optional>
#include <wtf/Vector.h>
#include <wtf/text/StringView.h>
#include <wtf/text/WTFString.h>

namespace JSC {
namespace Corpse {

class Snapshot;
class TargetObject;

// One data member of a target type as its debug info describes it. Types are
// carried as strings because the target's C++ types have no compile-time
// counterpart in the process doing the introspection.
struct TargetField {
    String name;
    String typeName;
    uint64_t byteOffset { 0 };
    uint64_t byteSize { 0 };
};

// A class or struct in the target, with its byte size and every data member
// flattened past anonymous unions and structs so members show up at their
// enclosing offset. Once built, the value carries no reference to any debug
// info handle.
class TargetType {
public:
    TargetType(String name, uint64_t byteSize, Vector<TargetField>&&,
        bool isPolymorphic, Address expectedVTableAddress);

    const String& name() const { return m_name; }
    uint64_t byteSize() const { return m_byteSize; }
    const Vector<TargetField>& fields() const { return m_fields; }

    // True if the type has a vtable at offset 0. A caller confirms an object's
    // runtime type by reading the vptr and matching it against this type's
    // expected vtable address.
    bool isPolymorphic() const { return m_isPolymorphic; }

    // The address of `vtable for T` in the target. Set exactly when
    // isPolymorphic() is true; findType refuses to build a TargetType for a
    // polymorphic class whose vtable symbol does not resolve, so a valid
    // TargetType never carries isPolymorphic() && !expectedVTableAddress().
    Address expectedVTableAddress() const { return m_expectedVTableAddress; }

    // Returns the field named `name`, or nullptr if the type has no such member.
    const TargetField* field(StringView name) const;

private:
    String m_name;
    uint64_t m_byteSize { 0 };
    Vector<TargetField> m_fields;
    bool m_isPolymorphic { false };
    Address m_expectedVTableAddress;
};

// Looks up types in a target's debug info and binds addresses to those types.
// One instance holds the LLDB session for the whole corpse. Reads and process
// queries go through Snapshot and Process so the platform primitives stay
// hidden behind the Corpse API.
class TypeSystem {
public:
    // The Snapshot's target must be the current process (self-corpse). Cross-
    // process type introspection is not wired up yet and calling create()
    // against any other pid trips a RELEASE_ASSERT.
    static std::unique_ptr<TypeSystem> create(Snapshot&);
    ~TypeSystem();

    TypeSystem(const TypeSystem&) = delete;
    TypeSystem& operator=(const TypeSystem&) = delete;

    std::optional<TargetType> findType(StringView qualifiedName);
    std::optional<TargetObject> getTargetObject(Address, const TargetType&);

private:
    class Impl;
    explicit TypeSystem(std::unique_ptr<Impl>);
    std::unique_ptr<Impl> m_impl;
};

} // namespace Corpse
} // namespace JSC

#endif // (OS(MACOS) || USE(APPLE_INTERNAL_SDK)) && !PLATFORM(MACCATALYST) && !PLATFORM(IOS_FAMILY_SIMULATOR)
