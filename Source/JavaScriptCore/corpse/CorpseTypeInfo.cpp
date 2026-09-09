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
#include "CorpseTypeInfo.h"

#if HAVE(CORPSE_SUPPORT)

#include "CorpseSnapshot.h"

namespace JSC {
namespace Corpse {

namespace {

constexpr uint64_t pointerSize = sizeof(uint64_t);

// std::type_info opens with its own vptr and then the mangled name, whichever
// of the ABI's type_info classes it is. Bases, where a class has any, follow.
constexpr uint64_t typeInfoNameOffset = pointerSize;
constexpr uint64_t siBaseOffset = 2 * pointerSize;
constexpr uint64_t vmiFlagsOffset = 2 * pointerSize;
constexpr uint64_t vmiBaseCountOffset = vmiFlagsOffset + sizeof(uint32_t);
constexpr uint64_t vmiBaseArrayOffset = 3 * pointerSize;
constexpr uint64_t vmiBaseEntrySize = 2 * pointerSize; // type_info*, then offset/flags.

// A class with more direct bases than this is not one the target really has,
// and the count comes out of memory that may be corrupt.
constexpr uint32_t maxDirectBases = 1024;

// A hierarchy deeper than this is likewise not a real one. Cycles are caught by
// the visited set; this bounds the honest-but-absurd case.
constexpr unsigned maxHierarchyDepth = 128;

// A mangled name longer than this is not a name.
constexpr size_t maxMangledNameLength = 4 * KB;

} // anonymous namespace

TypeInfoReader::TypeInfoReader(const Snapshot& snapshot)
    : m_snapshot(snapshot)
{
}

// The ABI's three type_info classes are exported by the C++ runtime, so the
// corpse's own symbol lookup finds them. Their vtables' address points are what
// a type_info's vptr holds.
void TypeInfoReader::resolveAbiVPtrs()
{
    if (m_resolvedAbiVPtrs)
        return;
    m_resolvedAbiVPtrs = true;

    // Symbol lookup takes an undecorated name; the vtable symbols of the ABI
    // classes are spelled by the ABI itself.
    auto addressPointOf = [&](const char* vtableSymbol) -> Address {
        // The lookup is const, but the snapshot caches what it resolves.
        Address vtable = const_cast<Snapshot&>(m_snapshot).symbol(vtableSymbol);
        if (!vtable)
            return { };
        // Two slots in: offset-to-top, then the type_info pointer.
        return vtable + 2 * pointerSize;
    };

    m_classTypeInfoVPtr = addressPointOf("_ZTVN10__cxxabiv117__class_type_infoE");
    m_siClassTypeInfoVPtr = addressPointOf("_ZTVN10__cxxabiv120__si_class_type_infoE");
    m_vmiClassTypeInfoVPtr = addressPointOf("_ZTVN10__cxxabiv121__vmi_class_type_infoE");
}

std::optional<Address> TypeInfoReader::typeInfoForVPtr(Address vptr)
{
    if (!vptr)
        return std::nullopt;
    // A vptr that is not pointer-aligned is not a vptr.
    if (vptr.value() % pointerSize)
        return std::nullopt;
    // The slot below the address point holds the type_info, and reading it is
    // only safe where the corpse has something mapped.
    Address slot = vptr - pointerSize;
    if (!m_snapshot.regionContaining(slot))
        return std::nullopt;

    uint64_t typeInfo = 0;
    if (!m_snapshot.readInto(slot, typeInfo))
        return std::nullopt;
    Address address = Address { typeInfo }.stripped();
    if (!address || !m_snapshot.regionContaining(address))
        return std::nullopt;
    return address;
}

String TypeInfoReader::nameOfTypeInfo(Address typeInfo)
{
    uint64_t namePointer = 0;
    if (!m_snapshot.readInto(typeInfo + typeInfoNameOffset, namePointer))
        return { };

    // libc++abi flags a non-unique name in the top bit of the pointer on some
    // targets, and marks one with a leading '*' on others. Neither is part of
    // the name.
    static constexpr uint64_t nonUniqueBit = 1ull << 63;
    Address nameAddress = Address { namePointer & ~nonUniqueBit }.stripped();
    if (!nameAddress || !m_snapshot.regionContaining(nameAddress))
        return { };

    CString name = m_snapshot.readCString(nameAddress, maxMangledNameLength);
    if (name.isNull() || !name.length())
        return { };
    auto span = name.span();
    if (span.front() == '*')
        span = span.subspan(1);
    if (span.empty())
        return { };
    return String::fromUTF8(span);
}

Vector<Address> TypeInfoReader::basesOfTypeInfo(Address typeInfo)
{
    Vector<Address> bases;
    resolveAbiVPtrs();

    uint64_t kind = 0;
    if (!m_snapshot.readInto(typeInfo, kind))
        return bases;
    Address kindVPtr = Address { kind }.stripped();

    if (kindVPtr == m_classTypeInfoVPtr)
        return bases; // A class with no bases records none.

    if (kindVPtr == m_siClassTypeInfoVPtr) {
        uint64_t base = 0;
        if (!m_snapshot.readInto(typeInfo + siBaseOffset, base))
            return bases;
        if (Address address = Address { base }.stripped())
            bases.append(address);
        return bases;
    }

    if (kindVPtr != m_vmiClassTypeInfoVPtr)
        return bases; // Not a type_info we know how to read.

    uint32_t baseCount = 0;
    if (!m_snapshot.readInto(typeInfo + vmiBaseCountOffset, baseCount))
        return bases;
    if (!baseCount || baseCount > maxDirectBases)
        return bases;

    bases.reserveInitialCapacity(baseCount);
    for (uint32_t i = 0; i < baseCount; ++i) {
        uint64_t base = 0;
        if (!m_snapshot.readInto(typeInfo + vmiBaseArrayOffset + i * vmiBaseEntrySize, base))
            break;
        if (Address address = Address { base }.stripped())
            bases.append(address);
    }
    return bases;
}

String TypeInfoReader::mangledNameForVPtr(Address vptr)
{
    if (!vptr)
        return { };
    auto cached = m_nameByVPtr.find(vptr.value());
    if (cached != m_nameByVPtr.end())
        return cached->value;

    String name;
    if (auto typeInfo = typeInfoForVPtr(vptr))
        name = nameOfTypeInfo(*typeInfo);
    // A miss is cached too: a walk over a heap meets the same non-object bytes
    // again and again, and each retry would be three reads into nothing.
    m_nameByVPtr.add(vptr.value(), name);
    return name;
}

Vector<String> TypeInfoReader::mangledHierarchyForVPtr(Address vptr)
{
    Vector<String> names;
    auto typeInfo = typeInfoForVPtr(vptr);
    if (!typeInfo)
        return names;
    HashSet<uint64_t> visited;
    collectHierarchy(*typeInfo, names, visited, 0);
    return names;
}

void TypeInfoReader::collectHierarchy(Address typeInfo, Vector<String>& out,
    HashSet<uint64_t>& visited, unsigned depth)
{
    if (depth > maxHierarchyDepth)
        return;
    // A hierarchy read out of a corpse can be made to loop; visiting each
    // type_info once is what makes the walk terminate.
    if (!visited.add(typeInfo.value()).isNewEntry)
        return;

    String name = nameOfTypeInfo(typeInfo);
    if (!name.isEmpty())
        out.append(WTF::move(name));

    for (Address base : basesOfTypeInfo(typeInfo)) {
        if (!m_snapshot.regionContaining(base))
            continue;
        collectHierarchy(base, out, visited, depth + 1);
    }
}

} // namespace Corpse
} // namespace JSC

#endif // HAVE(CORPSE_SUPPORT)
