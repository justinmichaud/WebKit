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
#include "CorpseSymbolResolver.h"

#if HAVE(CORPSE_SUPPORT) && HAVE(CORPSE_EXPORTS_TRIE)

#include "CorpseError.h"
#include "CorpseExportsTrie.h"
#include "CorpseSnapshot.h"

#include <wtf/TZoneMallocInlines.h>

#include <mach-o/loader.h>
#include <optional>
#include <span>
#include <string.h>
#include <string_view>
#include <type_traits>
#include <wtf/StdLibExtras.h>
#include <wtf/Vector.h>
#include <wtf/text/StringCommon.h>

// Enable for more detailed error messages on what may have caused a symbol
// lookup failure.
#define CORPSE_SYMBOL_LOOKUP_DIAGNOSTICS 0

#if CORPSE_SYMBOL_LOOKUP_DIAGNOSTICS
#define CORPSE_DIAGNOSTIC_DO(statement) statement
#else
#define CORPSE_DIAGNOSTIC_DO(statement) ((void)0)
#endif

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC {
namespace Corpse {

// Resolves a name through a Mach-O image's dyld exports trie.
//
// Only regular and absolute exports are read out of a trie. A re-export is
// skipped rather than followed, so a name that one image re-exports resolves in
// the image that defines it, as long as that image is loaded in the corpse. A
// thread-local is not found at all.
//
// A re-export may also rename, and then no image exports the name at all:
// memcpy exists only as libsystem_c's re-export of __platform_memmove from
// libsystem_platform, so a lookup of memcpy finds nothing while a lookup of
// __platform_memmove succeeds.
class MachOResolver final : public ImageSymbolResolver {
    WTF_MAKE_TZONE_ALLOCATED_INLINE(MachOResolver);
public:
    Address resolveIn(const Snapshot& snapshot, const ImageInfo& image, std::string_view name,
        ReadBudget& budget) final
    {
        m_budget = &budget;
        // A Mach-O symbol name carries a leading underscore.
        std::string decorated = "_" + std::string(name);
        return resolveInImage(snapshot, image.loadAddress.stripped(), decorated);
    }

    void reportFailure(const Snapshot&, std::string_view name) const final;

private:
    Address resolveInImage(const Snapshot&, Address imageAddress, std::string_view name);

#if CORPSE_SYMBOL_LOOKUP_DIAGNOSTICS
    // How far a search got, so a failure can name the stage that fell short.
    struct Diagnostics {
        unsigned images { 0 };                  // Images the corpse reported.
        unsigned examined { 0 };                // ...whose Mach header we read.
        unsigned inSharedCache { 0 };           // ...of those, in the shared cache.
        unsigned unreadableHeader { 0 };        // Header missing or not 64-bit.
        unsigned implausibleCommandsSize { 0 }; // sizeofcmds too large to believe.
        unsigned unreadableCommands { 0 };      // Load commands unreadable.
        unsigned withoutTrie { 0 };             // No trie, or no __TEXT/__LINKEDIT.
        unsigned implausibleTrieSize { 0 };     // Trie size too large to believe.
        unsigned trieOutsideLinkedit { 0 };     // Trie not within __LINKEDIT.
        unsigned unreadableTrie { 0 };          // Trie located but not readable.
        unsigned readBudgetExhausted { 0 };     // Gave up: the lookup hit its read budget.
        unsigned searched { 0 };                // Tries actually walked.
        unsigned reExports { 0 };               // Matched, but re-exported.
        unsigned unsupportedKind { 0 };         // Matched, but not an export kind with one address.
    };

    void reportFailure(const Snapshot&) const;
    mutable Diagnostics m_diagnostics;
#endif

    ReadBudget* m_budget { nullptr };
};

std::unique_ptr<ImageSymbolResolver> ImageSymbolResolver::create()
{
    return makeUnique<MachOResolver>();
}

// It is assumed that this corpse analysis library is built with the same SDK targeting
// the same OS that the corpse binary is built for. While the corpse gives us the data
// to inspect, it does not provide the format. Hence, we need to rely on the invariant
// that this analysis library is built with the same understanding of the same data
// format used in the corpse. This is how we can walk and interpret the corpse's
// dyld exports trie and get addresses of symbols.

namespace {

// Sizes and counts read out of a corpse are used to bound loops and to size
// allocations, so they are checked against these limits first. Each one is a
// sanity check on a single value: it says the struct we read was not what we
// thought it was, in which case the addresses in it are not worth chasing. They
// are not a bound on the work a lookup can do, because the per-image limits
// multiply by the image count. maxTotalBytesRead above is that bound.
//
// The values sit above what was empirically measured: across every Mach-O image
// installed on a sample system the largest load commands were 7.4 KB and the
// largest exports trie 2.1 MB, and a process that dlopens every framework on the
// system reaches about 2,800 images.
constexpr size_t maxLoadCommandsSize = 128 * KB; // About 17× the measured maximum.
constexpr size_t maxExportsTrieSize = 16 * MB; // About 8× the measured maximum.
constexpr uint32_t maxImageCount = 16 * 1024; // About 6× the measured maximum.

// Copies data out of a corpse. Every read goes through the Snapshot, so the
// platform primitive stays behind the backend.
// FIXME: This is a temporary "get things to work solution", and will be replaced with
// a more efficient memory access from a page manager later that eliminates copying.
class CorpseMemory {
public:
    explicit CorpseMemory(const Snapshot& snapshot)
        : m_snapshot(snapshot)
    {
    }

    template<typename T>
    std::optional<T> read(Address address) const
    {
        static_assert(std::is_trivially_copyable_v<T>);
        T out;
        if (!readRaw(address, &out, sizeof out))
            return std::nullopt;
        return out;
    }

    std::optional<Vector<uint8_t>> readBytes(Address address, size_t length) const
    {
        Vector<uint8_t> buffer;
        // Callers derive `length` from the corpse, so failing to allocate is a
        // potential outcome here due to potential corruption.
        if (!buffer.tryGrow(length))
            return std::nullopt;
        if (!readRaw(address, buffer.mutableSpan().data(), length))
            return std::nullopt;
        return buffer;
    }

private:
    bool readRaw(Address address, void* destination, size_t length) const
    {
        return m_snapshot.read(address,
            std::span { static_cast<uint8_t*>(destination), length });
    }

    const Snapshot& m_snapshot;
};

template<typename T>
std::optional<T> readCommand(std::span<const uint8_t> commands, size_t offset)
{
    static_assert(std::is_trivially_copyable_v<T>);
    // Compared against what is left of the buffer rather than by forming
    // offset + sizeof(T), which could wrap and pass a direct comparison. The
    // first clause is what makes the subtraction safe.
    if (offset > commands.size() || commands.size() - offset < sizeof(T))
        return std::nullopt;

    T value;
    memcpySpan(asMutableByteSpan(value), commands.subspan(offset, sizeof(T)));
    return value;
}

bool segmentNameIs(const char (&name)[16], std::string_view expected)
{
    // A segment name fills the whole array when it is exactly 16 characters, in
    // which case it has no terminator.
    std::span<const char> span { name };
    return std::string_view(span.first(strlenSpan(span))) == expected;
}

} // anonymous namespace

// Resolves `name` in the one image loaded at `imageAddress`, via its exports
// trie. Returns a null address if this image does not export it.
Address MachOResolver::resolveInImage(const Snapshot& snapshot, Address imageAddress,
    std::string_view name)
{
    CorpseMemory memory(snapshot);

    auto header = memory.read<mach_header_64>(imageAddress);
    if (!header || header->magic != MH_MAGIC_64) {
        CORPSE_DIAGNOSTIC_DO(++m_diagnostics.unreadableHeader);
        return { };
    }
    CORPSE_DIAGNOSTIC_DO(++m_diagnostics.examined);
    if (header->flags & MH_DYLIB_IN_CACHE)
        CORPSE_DIAGNOSTIC_DO(++m_diagnostics.inSharedCache);

    if (header->sizeofcmds > maxLoadCommandsSize) {
        CORPSE_DIAGNOSTIC_DO(++m_diagnostics.implausibleCommandsSize);
        return { };
    }
    if (!m_budget->take(header->sizeofcmds))
        return { };
    auto commandsBuffer = memory.readBytes(imageAddress + sizeof(mach_header_64), header->sizeofcmds);
    if (!commandsBuffer) {
        CORPSE_DIAGNOSTIC_DO(++m_diagnostics.unreadableCommands);
        return { };
    }
    std::span<const uint8_t> commands = commandsBuffer->span();

    std::optional<uint64_t> textVMAddress;
    std::optional<uint64_t> linkeditVMAddress;
    std::optional<uint64_t> linkeditFileOffset;
    std::optional<uint64_t> linkeditFileSize;
    uint32_t exportOffset = 0;
    uint32_t exportSize = 0;

    size_t offset = 0;
    for (uint32_t i = 0; i < header->ncmds; ++i) {
        auto command = readCommand<load_command>(commands, offset);
        // cmdsize is compared against what is left of the blob rather than by
        // forming offset + cmdsize, which could wrap and pass a direct
        // comparison. The subtraction is safe only because a successful
        // readCommand has already established that offset is within the blob,
        // so the clauses have to stay in this order.
        if (!command || command->cmdsize < sizeof(load_command) || command->cmdsize > commands.size() - offset)
            break;

        // Each case below re-reads `offset` as the larger struct the command
        // claims to be. cmdsize has to cover that struct too: a command that
        // declares itself smaller is malformed, and reading it anyway would take
        // the fields that follow it as its own.
        switch (command->cmd) {
        case LC_SEGMENT_64: {
            if (command->cmdsize < sizeof(segment_command_64))
                break;
            auto segment = readCommand<segment_command_64>(commands, offset);
            if (!segment)
                break;
            if (segmentNameIs(segment->segname, SEG_TEXT))
                textVMAddress = segment->vmaddr;
            else if (segmentNameIs(segment->segname, SEG_LINKEDIT)) {
                linkeditVMAddress = segment->vmaddr;
                linkeditFileOffset = segment->fileoff;
                linkeditFileSize = segment->filesize;
            }
            break;
        }
        case LC_DYLD_INFO:
        case LC_DYLD_INFO_ONLY: {
            if (command->cmdsize < sizeof(dyld_info_command))
                break;
            auto info = readCommand<dyld_info_command>(commands, offset);
            if (!info)
                break;
            exportOffset = info->export_off;
            exportSize = info->export_size;
            break;
        }
        case LC_DYLD_EXPORTS_TRIE: {
            if (command->cmdsize < sizeof(linkedit_data_command))
                break;
            auto data = readCommand<linkedit_data_command>(commands, offset);
            if (!data)
                break;
            exportOffset = data->dataoff;
            exportSize = data->datasize;
            break;
        }
        default:
            break;
        }
        offset += command->cmdsize;
    }

    if (!textVMAddress || !linkeditVMAddress || !linkeditFileOffset || !linkeditFileSize || !exportSize) {
        CORPSE_DIAGNOSTIC_DO(++m_diagnostics.withoutTrie);
        return { };
    }
    if (exportSize > maxExportsTrieSize) {
        CORPSE_DIAGNOSTIC_DO(++m_diagnostics.implausibleTrieSize);
        return { };
    }

    // The trie is file-backed data living inside __LINKEDIT. So, exportOffset cannot
    // be less than the start of __LINKEDIT, cannot exceed the end of __LINKEDIT, and
    // the whole trie must fit inside it.
    if (exportOffset < *linkeditFileOffset) {
        CORPSE_DIAGNOSTIC_DO(++m_diagnostics.trieOutsideLinkedit);
        return { };
    }
    uint64_t trieSegmentOffset = exportOffset - *linkeditFileOffset;
    if (trieSegmentOffset > *linkeditFileSize || exportSize > *linkeditFileSize - trieSegmentOffset) {
        CORPSE_DIAGNOSTIC_DO(++m_diagnostics.trieOutsideLinkedit);
        return { };
    }

    // __TEXT's link-time address against where the image actually landed. The
    // load commands give link-time addresses, so everything read out of them
    // needs this added to reach the corpse.
    uint64_t slide = imageAddress - Address(*textVMAddress);
    Address trieAddress = Address(*linkeditVMAddress) + slide + trieSegmentOffset;

    if (!m_budget->take(exportSize))
        return { };
    auto trieBuffer = memory.readBytes(trieAddress, exportSize);
    if (!trieBuffer) {
        CORPSE_DIAGNOSTIC_DO(++m_diagnostics.unreadableTrie);
        return { };
    }
    CORPSE_DIAGNOSTIC_DO(++m_diagnostics.searched);

    auto found = ExportsTrie::lookUp(trieBuffer->span(), name);

#if CORPSE_SYMBOL_LOOKUP_DIAGNOSTICS

// Says how a failed search went, so a caller can tell "the symbol is not
// exported" from "the corpse could not be read".
void MachOResolver::reportFailure(const Snapshot& snapshot, std::string_view name) const
{
    const Diagnostics& d = m_diagnostics;

    Error::report("No symbol '_%s' in pid %d", std::string(name).c_str(),
        static_cast<int>(snapshot.process()->pid()));

    if (!d.images) {
        Error::report("  the corpse reports no loaded images, so there was nothing to search");
        return;
    }

    Error::report("  read %u of %u image headers (%u in the shared cache), walked %u exports tries",
        d.examined, d.images, d.inSharedCache, d.searched);
    if (d.unreadableHeader)
        Error::report("  %u image headers were unreadable or not 64-bit Mach-O", d.unreadableHeader);
    if (d.implausibleCommandsSize)
        Error::report("  %u images had implausible load command sizes", d.implausibleCommandsSize);
    if (d.unreadableCommands)
        Error::report("  %u images had unreadable load commands", d.unreadableCommands);
    if (d.withoutTrie)
        Error::report("  %u images had no exports trie", d.withoutTrie);
    if (d.implausibleTrieSize)
        Error::report("  %u images had implausible exports trie sizes", d.implausibleTrieSize);
    if (d.trieOutsideLinkedit)
        Error::report("  %u images placed their exports trie outside __LINKEDIT", d.trieOutsideLinkedit);
    if (d.unreadableTrie)
        Error::report("  %u exports tries were not readable", d.unreadableTrie);
    if (d.readBudgetExhausted) {
        Error::report("  gave up after reading %u MB from the corpse; %u images were skipped",
            static_cast<unsigned>(maxTotalBytesRead / MB), d.readBudgetExhausted);
    }
    if (d.reExports)
        Error::report("  %u images re-export the name from elsewhere, which is not followed", d.reExports);
    if (d.unsupportedKind)
        Error::report("  %u images export the name in a form that has no single address, such as a thread-local", d.unsupportedKind);

    if (d.searched) {
        Error::report(
            "  only exported symbols appear in an exports trie: a symbol hidden"
            " by the linker is invisible here even though lldb can still find"
            " it in the symbol table");
    } else {
        Error::report(
            "  no trie was searched, so this is a memory-access problem rather"
            " than the symbol being absent");
    }
}

#endif // CORPSE_SYMBOL_LOOKUP_DIAGNOSTICS

} // namespace Corpse
} // namespace JSC

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#undef CORPSE_DIAGNOSTIC_DO

#endif // HAVE(CORPSE_SUPPORT) && HAVE(CORPSE_EXPORTS_TRIE)
