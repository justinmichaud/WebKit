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
#include "CorpseElfImage.h"

#if HAVE(CORPSE_SUPPORT) && OS(LINUX)

#include "CorpseSnapshot.h"

#include <algorithm>
#include <elf.h>
#include <string.h>
#include <wtf/StdLibExtras.h>
#include <wtf/Vector.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC {
namespace Corpse {

namespace {

// Bounds on values read out of a corpse. Each one says the structure we read
// was not what we thought it was, in which case nothing in it is worth
// following. They sit well above what a real image reaches.
constexpr uint16_t maxProgramHeaders = 256;
constexpr uint64_t maxDynamicEntries = 4096;
constexpr uint32_t maxSymbolCount = 1024 * 1024;
constexpr uint64_t maxStringTableSize = 64 * MB;
constexpr uint32_t maxHashBuckets = 1024 * 1024;
constexpr size_t maxSymbolNameLength = 4 * KB;

// Every read here is charged to the lookup's budget before it happens, so a
// corpse claiming a huge symbol table cannot spend more than the caller allowed.
template<typename T>
bool readStruct(const Snapshot& snapshot, Address address, T& out, ReadBudget& budget)
{
    if (!budget.take(sizeof(T)))
        return false;
    return snapshot.readInto(address, out);
}

// A d_ptr in a mapped .dynamic may hold either the address the image was linked
// at or the address it actually landed on: whether the loader rewrites these in
// place is the architecture's business. A value below the image's own base
// cannot be an address in it, so it is the link-time one and needs the slide.
Address resolveDynamicPointer(uint64_t value, Address loadAddress, uint64_t slide)
{
    if (value >= loadAddress.value())
        return Address { value };
    return Address { value + slide };
}

} // anonymous namespace

std::optional<ElfImage> ElfImage::parse(const Snapshot& snapshot, const ImageInfo& info,
    ReadBudget& budget)
{
    if (!info.loadAddress)
        return std::nullopt;

    Elf64_Ehdr header;
    if (!readStruct(snapshot, info.loadAddress, header, budget))
        return std::nullopt;
    if (memcmp(header.e_ident, ELFMAG, SELFMAG))
        return std::nullopt;
    if (header.e_ident[EI_CLASS] != ELFCLASS64)
        return std::nullopt;
    if (!header.e_phoff || !header.e_phnum || header.e_phnum > maxProgramHeaders)
        return std::nullopt;
    if (header.e_phentsize != sizeof(Elf64_Phdr))
        return std::nullopt;

    // PT_DYNAMIC is where the loader's own view of the image lives: the symbol
    // table, the string table, and the hash table that says how many symbols
    // there are.
    std::optional<uint64_t> dynamicVirtualAddress;
    uint64_t dynamicSize = 0;
    for (uint16_t i = 0; i < header.e_phnum; ++i) {
        Elf64_Phdr segment;
        if (!readStruct(snapshot, info.loadAddress + header.e_phoff + i * sizeof(Elf64_Phdr),
            segment, budget)) {
            return std::nullopt;
        }
        if (segment.p_type != PT_DYNAMIC)
            continue;
        dynamicVirtualAddress = segment.p_vaddr;
        dynamicSize = segment.p_memsz;
        break;
    }
    if (!dynamicVirtualAddress || !dynamicSize)
        return std::nullopt;

    uint64_t entryCount = dynamicSize / sizeof(Elf64_Dyn);
    if (!entryCount || entryCount > maxDynamicEntries)
        return std::nullopt;

    Address dynamic = resolveDynamicPointer(*dynamicVirtualAddress, info.loadAddress, info.slide);

    uint64_t symbolTable = 0;
    uint64_t stringTable = 0;
    uint64_t stringTableSize = 0;
    uint64_t symbolEntrySize = sizeof(Elf64_Sym);
    uint64_t hashTable = 0;
    uint64_t gnuHashTable = 0;

    for (uint64_t i = 0; i < entryCount; ++i) {
        Elf64_Dyn entry;
        if (!readStruct(snapshot, dynamic + i * sizeof(Elf64_Dyn), entry, budget))
            return std::nullopt;
        if (entry.d_tag == DT_NULL)
            break;
        switch (entry.d_tag) {
        case DT_SYMTAB: symbolTable = entry.d_un.d_ptr; break;
        case DT_STRTAB: stringTable = entry.d_un.d_ptr; break;
        case DT_STRSZ: stringTableSize = entry.d_un.d_val; break;
        case DT_SYMENT: symbolEntrySize = entry.d_un.d_val; break;
        case DT_HASH: hashTable = entry.d_un.d_ptr; break;
        case DT_GNU_HASH: gnuHashTable = entry.d_un.d_ptr; break;
        default: break;
        }
    }

    if (!symbolTable || !stringTable || !stringTableSize)
        return std::nullopt;
    // A symbol entry that is not the size the ABI gives would make every offset
    // below wrong, so it is not a table we should be indexing.
    if (symbolEntrySize != sizeof(Elf64_Sym))
        return std::nullopt;
    if (stringTableSize > maxStringTableSize)
        return std::nullopt;

    ElfImage image;
    image.m_loadAddress = info.loadAddress;
    image.m_slide = info.slide;
    image.m_symbolTable = resolveDynamicPointer(symbolTable, info.loadAddress, info.slide);
    image.m_stringTable = resolveDynamicPointer(stringTable, info.loadAddress, info.slide);
    image.m_stringTableSize = stringTableSize;

    // Nothing in an ELF image states how many dynamic symbols it has; both hash
    // tables imply it, so whichever the image carries is what says where the
    // table ends.
    if (hashTable) {
        // DT_HASH opens with nbucket and nchain, and nchain is by definition
        // one chain slot per symbol.
        struct { uint32_t bucketCount; uint32_t chainCount; } counts;
        Address table = resolveDynamicPointer(hashTable, info.loadAddress, info.slide);
        if (readStruct(snapshot, table, counts, budget) && counts.chainCount
            && counts.chainCount <= maxSymbolCount) {
            image.m_symbolCount = counts.chainCount;
        }
    }

    if (!image.m_symbolCount && gnuHashTable) {
        Address table = resolveDynamicPointer(gnuHashTable, info.loadAddress, info.slide);
        if (auto count = countSymbolsFromGnuHash(snapshot, table, budget))
            image.m_symbolCount = *count;
    }

    if (!image.m_symbolCount)
        return std::nullopt;
    return image;
}

// DT_GNU_HASH omits every symbol before `symbolOffset` from its buckets, so the
// count is one past the highest index its chains reach. Each chain runs until
// an entry with the low bit set.
std::optional<uint32_t> ElfImage::countSymbolsFromGnuHash(const Snapshot& snapshot,
    Address table, ReadBudget& budget)
{
    struct {
        uint32_t bucketCount;
        uint32_t symbolOffset;
        uint32_t bloomSize;
        uint32_t bloomShift;
    } header;
    if (!readStruct(snapshot, table, header, budget))
        return std::nullopt;
    if (!header.bucketCount || header.bucketCount > maxHashBuckets)
        return std::nullopt;
    if (header.symbolOffset > maxSymbolCount)
        return std::nullopt;
    // The bloom filter is an array of 64-bit words whose size the header gives.
    if (header.bloomSize > maxHashBuckets)
        return std::nullopt;

    Address buckets = table + sizeof(header) + static_cast<uint64_t>(header.bloomSize) * sizeof(uint64_t);

    uint32_t highest = 0;
    for (uint32_t i = 0; i < header.bucketCount; ++i) {
        uint32_t bucket = 0;
        if (!readStruct(snapshot, buckets + i * sizeof(uint32_t), bucket, budget))
            return std::nullopt;
        if (bucket > maxSymbolCount)
            return std::nullopt;
        highest = std::max(highest, bucket);
    }
    if (highest < header.symbolOffset)
        return header.symbolOffset;

    // Walk the chain of the bucket that reaches furthest to its terminator.
    Address chains = buckets + static_cast<uint64_t>(header.bucketCount) * sizeof(uint32_t);
    uint32_t index = highest;
    while (index < maxSymbolCount) {
        uint32_t entry = 0;
        if (!readStruct(snapshot, chains + (index - header.symbolOffset) * sizeof(uint32_t),
            entry, budget)) {
            return std::nullopt;
        }
        ++index;
        if (entry & 1)
            return index;
    }
    return std::nullopt;
}

Address ElfImage::lookUp(const Snapshot& snapshot, std::string_view name, ReadBudget& budget) const
{
    if (name.empty() || name.size() > maxSymbolNameLength)
        return { };

    for (uint32_t i = 0; i < m_symbolCount; ++i) {
        Elf64_Sym symbol;
        if (!readStruct(snapshot, m_symbolTable + i * sizeof(Elf64_Sym), symbol, budget))
            return { };

        // An undefined symbol names an import, not a definition here, and an
        // entry with no name or no value cannot be what a lookup wants.
        if (!symbol.st_shndx || symbol.st_shndx == SHN_UNDEF)
            continue;
        if (!symbol.st_name || !symbol.st_value)
            continue;
        if (symbol.st_name >= m_stringTableSize)
            continue;

        // The name is compared in place rather than copied out: only as many
        // bytes as the sought name, plus the terminator that proves the
        // candidate is not merely a prefix.
        size_t available = m_stringTableSize - symbol.st_name;
        if (available < name.size() + 1)
            continue;

        Vector<uint8_t, 256> candidate;
        if (!candidate.tryGrow(name.size() + 1))
            return { };
        if (!budget.take(candidate.size()))
            return { };
        if (!snapshot.read(m_stringTable + symbol.st_name, candidate.mutableSpan()))
            continue;
        if (candidate[name.size()])
            continue;
        if (memcmp(candidate.span().data(), name.data(), name.size()))
            continue;

        // An absolute symbol's value is already an address; every other kind
        // holds the address the image was linked to put it at.
        if (symbol.st_shndx == SHN_ABS)
            return Address { symbol.st_value };
        return Address { symbol.st_value + m_slide };
    }
    return { };
}

} // namespace Corpse
} // namespace JSC

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // HAVE(CORPSE_SUPPORT) && OS(LINUX)
