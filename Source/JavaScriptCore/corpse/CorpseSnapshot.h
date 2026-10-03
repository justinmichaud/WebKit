/*
 * Copyright (C) 2026 Apple Inc. All rights reserved.
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

#if ENABLE(MYA)

#include <JavaScriptCore/CorpseAddress.h>
#include <JavaScriptCore/CorpseImage.h>
#include <JavaScriptCore/CorpseMemory.h>
#include <JavaScriptCore/CorpseProcess.h>
#include <JavaScriptCore/CorpseRegion.h>
#include <JavaScriptCore/CorpseSymbol.h>
#include <JavaScriptCore/CorpseThread.h>
#include <memory>
#include <optional>
#include <utility>
#include <wtf/DoublyLinkedList.h>
#include <wtf/HashMap.h>
#include <wtf/RefPtr.h>
#include <wtf/StdLibExtras.h>
#include <wtf/TZoneMalloc.h>
#include <wtf/Vector.h>
#include <wtf/text/StringHash.h>
#include <wtf/text/StringView.h>
#include <wtf/text/WTFString.h>

#if OS(DARWIN)
struct dyld_all_image_infos;
#endif

namespace JSC {
namespace Corpse {

// Owns a corpse: a read-only Mach snapshot of a process on Darwin, and on
// Linux a copy of the process that forkCopy made. A snapshot of another
// Linux process that made no copy reads that live process, as a best-effort
// view of a target that does not cooperate.
// Check isValid() to see whether acquisition succeeded.
//
// Snapshots are linked into a DoublyLinkedList by their owner. The list is
// intrusive and does not own its nodes: whoever appends a Snapshot must remove
// it from the list before destroying it.
class Snapshot : public DoublyLinkedListNode<Snapshot> {
    WTF_MAKE_TZONE_ALLOCATED(Snapshot);
public:
    // On Linux, a snapshot of this process copies it.
    explicit Snapshot(RefPtr<Process>);
#if !OS(DARWIN)
    // The copy `copy` that the target `process` made of itself with forkCopy,
    // which `lifetime`, the write end of its lifetime pipe, keeps alive. The
    // snapshot owns `lifetime`. The target's threads are read now, while it
    // waits for the snapshot to exist.
    Snapshot(RefPtr<Process>, pid_t copy, int lifetime);
#endif
    ~Snapshot();

    Snapshot(const Snapshot&) = delete;
    Snapshot& operator=(const Snapshot&) = delete;
    Snapshot(Snapshot&& other) = delete;

    bool isValid() const { return isValidTaskHandle(corpsePort()); }

    // A monotonically increasing identifier assigned at construction. IDs are
    // never reused, so they stay stable as snapshots are added and removed.
    unsigned id() const { return m_id; }

    Process* process() const { return m_process.get(); }
    // On Linux, the pid of the copy, or of the live process.
    TaskHandle corpsePort() const { return taskHandle(m_corpsePort); }

    Memory& memory() LIFETIME_BOUND
    {
        RELEASE_ASSERT(isValid());
        return m_memory;
    }

    // The threads and images captured in this corpse, read and cached on the first call.
    const Vector<Thread>& threads();
    const Vector<Image>& images();
    // On Darwin, with their page counts, as they were when the snapshot was
    // taken: reading a snapshot's untouched pages makes them resident in it.
    const Vector<Region>& regions();
#if OS(DARWIN)
    // The VM_PAGE_QUERY_PAGE_* disposition of each page of a private region
    // with dirty or compressed pages, as it was when the snapshot was taken.
    // Null for any other region.
    const Vector<uint16_t>* pageDispositions(const Region& region) const
    {
        auto found = m_pageDispositions.find(region.base());
        return found == m_pageDispositions.end() ? nullptr : &found->value;
    }
    // The task's physical footprint (task_vm_info's phys_footprint) when the
    // snapshot was taken: its dirty and compressed private pages, its page
    // tables, and the memory the kernel charges it for.
    std::optional<uint64_t> physicalFootprint() const { return m_physicalFootprint; }
    // The memory the kernel charges the task through its tagged ledgers, which
    // no private region shows, when the snapshot was taken: the graphics,
    // media, network and neural memory it owns, resident and compressed.
    uint64_t taggedLedgerBytes() const { return m_taggedLedgerBytes; }
    // What a region adds to the footprint: its pages that are dirty and not
    // reusable, and those compressed, by their dispositions. A page can be
    // dirty and reusable, which the region's own dirty count includes.
    uint64_t footprintBytes(const Region&) const;
#endif

#if OS(DARWIN)
    // dyld's record of the loaded images. Invalid, having reported why, if it cannot be read.
    Memory::Ptr<dyld_all_image_infos> dyldAllImageInfos();
#endif

    // The address of `name` in this corpse, null if it is not there.
    Address symbol(const char* name);

private:
    static unsigned s_nextId;

    RefPtr<Process> m_process;
#if !OS(DARWIN)
    int m_lifetime { -1 }; // The write end of the copy's lifetime pipe, if this snapshot owns a copy.
#endif
    OwnedTaskHandle m_corpsePort;
    unsigned m_id;

    std::optional<Vector<Thread>> m_threads;
    std::optional<Vector<Image>> m_images;
    std::optional<Vector<Region>> m_regions;
#if OS(DARWIN)
    HashMap<Address, Vector<uint16_t>> m_pageDispositions;
    std::optional<uint64_t> m_physicalFootprint;
    uint64_t m_taggedLedgerBytes { 0 };
#endif
    HashMap<String, std::unique_ptr<Symbol>> m_symbols;
    Memory m_memory;

    Snapshot* m_prev { nullptr }; // Required by DoublyLinkedListNode.
    Snapshot* m_next { nullptr }; // Required by DoublyLinkedListNode.

    friend class WTF::DoublyLinkedListNode<Snapshot>;
};

} // namespace Corpse
} // namespace JSC

#endif // ENABLE(MYA)
