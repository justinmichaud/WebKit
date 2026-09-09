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
#include "CorpseSnapshot.h"

#if HAVE(CORPSE_SUPPORT)

#include "CorpseError.h"

#include <algorithm>
#include <string.h>
#include <wtf/StdLibExtras.h>
#include <wtf/TZoneMallocInlines.h>
#include <wtf/text/StringHash.h>

namespace JSC {
namespace Corpse {

WTF_MAKE_TZONE_ALLOCATED_IMPL(Snapshot);

unsigned Snapshot::s_nextId = 1;

Snapshot::Snapshot(RefPtr<Process> process)
    : m_process(WTF::move(process))
    , m_id(s_nextId++)
{
    if (!m_process || !m_process->isAttached())
        return;

    m_backend = Backend::capture(*m_process);
    if (!m_backend)
        return;

    // The map and the image list are read once, with the corpse, so that every
    // later lookup sees one consistent address space rather than re-reading a
    // corpse that a caller might expect to have changed.
    m_regions = m_backend->regions();
    std::sort(m_regions.begin(), m_regions.end(), [](const auto& a, const auto& b) {
        return a.base < b.base;
    });
    m_images = m_backend->images();
}

Snapshot::~Snapshot() = default;

const Vector<Thread>& Snapshot::threads()
{
    if (!m_threads)
        m_threads = Thread::collect(*this);
    return *m_threads;
}

Address Snapshot::symbol(const char* name)
{
    if (!name || !*name)
        return { };

    auto entry = m_symbols.ensure<StringViewHashTranslator>(StringView::fromLatin1(name), [&] {
        return WTF::makeUnique<Symbol>(*this, name);
    });

    return entry.iterator->value->address();
}

bool Snapshot::read(Address address, std::span<uint8_t> into) const
{
    return m_backend && m_backend->read(address, into);
}

std::optional<Vector<uint8_t>> Snapshot::readBytes(Address address, size_t length) const
{
    if (!m_backend)
        return std::nullopt;

    Vector<uint8_t> buffer;
    // The length is caller-supplied and may come from a target type's byte
    // size; tryGrow lets a huge value from broken debug info fail as nullopt
    // without aborting.
    if (!buffer.tryGrow(length))
        return std::nullopt;
    if (!length)
        return buffer;

    if (!m_backend->read(address, buffer.mutableSpan()))
        return std::nullopt;
    return buffer;
}

// The regions are sorted and taken once, so this is a binary search rather than
// a re-parse of the corpse's map. A walk over a heap does one of these per
// pointer it considers, which is why it has to be cheap.
std::optional<Region> Snapshot::regionContaining(Address address) const
{
    // The first region whose base is past the address; the one before it is the
    // only one that can contain it.
    auto it = std::upper_bound(m_regions.begin(), m_regions.end(), address,
        [](Address value, const RegionInfo& region) {
            return value < region.base;
        });
    if (it == m_regions.begin())
        return std::nullopt;
    --it;
    if (!it->contains(address))
        return std::nullopt;
    return Region { *it };
}

TypeSystem* Snapshot::typeSystem()
{
    if (!isValid())
        return nullptr;
    if (!m_typeSystem)
        m_typeSystem = TypeSystem::create(*this);
    return m_typeSystem.get();
}

std::optional<TargetType> Snapshot::findType(StringView qualifiedName)
{
    auto* types = typeSystem();
    return types ? types->findType(qualifiedName) : std::nullopt;
}

std::optional<TargetType> Snapshot::findTypeOfPointee(StringView variableName)
{
    auto* types = typeSystem();
    return types ? types->findTypeOfPointee(variableName) : std::nullopt;
}

std::optional<TargetType> Snapshot::findTypeOfMember(StringView qualifiedTypeName,
    StringView memberName)
{
    auto* types = typeSystem();
    return types ? types->findTypeOfMember(qualifiedTypeName, memberName) : std::nullopt;
}

std::optional<TargetObject> Snapshot::getTargetObject(Address base, const TargetType& type)
{
    auto* types = typeSystem();
    return types ? types->getTargetObject(base, type) : std::nullopt;
}

std::optional<TargetObject> Snapshot::follow(const TargetObject& object, const TargetField& field)
{
    if (!field.isPointer)
        return std::nullopt;

    auto bytes = object.get(field);
    if (bytes.size() != sizeof(uint64_t))
        return std::nullopt;
    uint64_t pointer = 0;
    memcpy(&pointer, bytes.data(), sizeof(pointer));
    if (!pointer)
        return std::nullopt;

    auto* types = typeSystem();
    if (!types)
        return std::nullopt;
    auto type = types->findTypeOfMemberPointee(object.type().name(), field.name);
    if (!type)
        return std::nullopt;
    return types->getTargetObject(Address { pointer }, *type);
}

} // namespace Corpse
} // namespace JSC

#endif // HAVE(CORPSE_SUPPORT)
