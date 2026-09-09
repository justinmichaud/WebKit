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
#include <optional>
#include <wtf/HashMap.h>
#include <wtf/HashSet.h>
#include <wtf/Vector.h>
#include <wtf/text/StringHash.h>
#include <wtf/text/WTFString.h>

namespace JSC {
namespace Corpse {

class Snapshot;

// Reads what an object says about itself.
//
// The Itanium C++ ABI puts a pointer to the class's std::type_info one slot
// before a vtable's address point, and a type_info holds the class's mangled
// name. So an object with a vptr already carries its own identity, and reading
// it needs nothing but the corpse: no image files, no symbol tables, no debug
// info. Both platforms follow the same ABI, so one reader serves them.
//
// This works only against a target built with RTTI. A class compiled without it
// has a vtable but no type_info, and is reported as unidentified rather than
// guessed at.
//
// Every value read is treated as untrusted. A corpse is read precisely when the
// target may be corrupt, and a vptr is the first thing a wild write destroys, so
// each step is bounded and checked and a chain that does not hold up yields
// nothing rather than a plausible wrong answer.
class TypeInfoReader {
public:
    explicit TypeInfoReader(const Snapshot&);

    // The mangled name of the class whose vtable `vptr` addresses, null when
    // the value is not a vtable address point of a class with RTTI. Cached, so
    // a walk that meets the same class repeatedly pays once.
    String mangledNameForVPtr(Address vptr);

    // The mangled names of the class at `vptr` and of every base it has,
    // however deep. Empty when the class has no type_info to read.
    //
    // The ABI records each class's bases in its type_info, so a hierarchy is in
    // the corpse and none of it has to come from debug info. Names come back
    // mangled, which is how they are stored; whoever asked knows how to read
    // them, and demangling stays in one place.
    Vector<String> mangledHierarchyForVPtr(Address vptr);

private:
    // The type_info a vtable address point points back to.
    std::optional<Address> typeInfoForVPtr(Address vptr);

    String nameOfTypeInfo(Address);

    // The direct bases of a class, as type_info addresses. Which of the ABI's
    // three type_info classes this is decides where they are recorded, and that
    // is what the object's own vptr says.
    Vector<Address> basesOfTypeInfo(Address);

    void collectHierarchy(Address typeInfo, Vector<String>& out, HashSet<uint64_t>& visited,
        unsigned depth);

    const Snapshot& m_snapshot;

    // The ABI's own type_info classes, resolved once. A type_info's vptr is one
    // of these three, and which one says how its bases are laid out.
    bool m_resolvedAbiVPtrs { false };
    Address m_classTypeInfoVPtr;      // No bases.
    Address m_siClassTypeInfoVPtr;    // One public, non-virtual base.
    Address m_vmiClassTypeInfoVPtr;   // Anything else.

    void resolveAbiVPtrs();

    HashMap<uint64_t, String> m_nameByVPtr;
};

} // namespace Corpse
} // namespace JSC

#endif // HAVE(CORPSE_SUPPORT)
