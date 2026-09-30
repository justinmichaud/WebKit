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

#if ENABLE(MYA)

#include "CorpseError.h"

#if OS(DARWIN)
#include <mach-o/dyld_images.h>
#include <mach/mach.h>
#include <mach/mach_error.h>
#include <mach/task_info.h>
#endif
#include <wtf/TZoneMallocInlines.h>

#if HAVE(LLDB) && OS(DARWIN) && !defined(BUILDING_WITH_CMAKE)
// Xcode has no optional dependencies, so liblldb is linked only when its headers are found.
__asm__(".linker_option \"-llldb\"");
#endif

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC {
namespace Corpse {

WTF_MAKE_TZONE_ALLOCATED_IMPL(Snapshot);

unsigned Snapshot::s_nextId = 1;

const Vector<Thread>& Snapshot::threads()
{
    if (!m_threads)
        m_threads = Thread::collect(*this);
    return *m_threads;
}

const Vector<Image>& Snapshot::images()
{
    if (!m_images) {
        if (!isValid()) {
            CORPSE_REPORT("Cannot list the images of an invalid snapshot");
            m_images = Vector<Image> { };
        } else {
            CORPSE_DIAGNOSTICS(diagnostics, "listing the images of pid %d", static_cast<int>(process()->pid()));
            m_images = Image::collect(*this);
        }
    }
    return *m_images;
}

#if OS(DARWIN)

Memory::Ptr<dyld_all_image_infos> Snapshot::dyldAllImageInfos()
{
    task_dyld_info_data_t dyldInfo;
    mach_msg_type_number_t count = TASK_DYLD_INFO_COUNT;
    kern_return_t kr = task_info(corpsePort(), TASK_DYLD_INFO, reinterpret_cast<task_info_t>(&dyldInfo), &count);
    if (kr != KERN_SUCCESS) {
        CORPSE_REPORT("Could not read dyld information: %s (0x%x)", mach_error_string(kr), kr);
        return { };
    }

    Address address { dyldInfo.all_image_info_addr };
    if (!address) {
        CORPSE_REPORT("dyld reports no image list");
        return { };
    }
    auto allImages = memory().ptr<dyld_all_image_infos>(address);
    if (!allImages)
        CORPSE_REPORT("Could not read dyld_all_image_infos at 0x%llx", address.toTargetVMAddress());
    return allImages;
}

#endif // OS(DARWIN)

Address Snapshot::symbol(const char* name)
{
    if (!name || !*name) {
        CORPSE_REPORT("A symbol lookup needs a name");
        return { };
    }

    auto entry = m_symbols.ensure<StringViewHashTranslator>(StringView::fromLatin1(name), [&] {
        return WTF::makeUnique<Symbol>(*this, name);
    });

    Address address = entry.iterator->value->address();
    if (!address && !entry.isNewEntry) {
        if (!isValid())
            CORPSE_REPORT("Cannot look up '%s' in an invalid snapshot", name);
        else
            CORPSE_REPORT("No symbol '%s' in pid %d, as an earlier lookup found", name, static_cast<int>(process()->pid()));
    }
    return address;
}

#if OS(DARWIN)

// Returns a null handle if the snapshot could not be taken, having reported why.
static OwnedTaskHandle takeSnapshot(Process* process)
{
    if (!process || !process->isAttached()) {
        CORPSE_REPORT("Could not snapshot: No process attached");
        return { };
    }

    // Snapshot the target into a corpse; only a read port is required from here
    // on, and the corpse is independent of the live target.
    mach_port_t corpsePort = MACH_PORT_NULL;
    kern_return_t kr = task_generate_corpse(process->taskPort(), &corpsePort);
    if (kr == KERN_SUCCESS)
        return OwnedTaskHandle::adopt(corpsePort);

    if (!process->holdsLiveTask()) {
        Error::report("Could not snapshot PID %d: the process has terminated",
            static_cast<int>(process->pid()));
    } else {
        Error::report("Could not snapshot PID %d: %s (0x%x)",
            static_cast<int>(process->pid()), mach_error_string(kr), kr);
    }
    return { };
}

#else

// There is no corpse on Linux yet: the snapshot reads the live process.
static OwnedTaskHandle takeSnapshot(Process* process)
{
    if (!process || !process->isAttached()) {
        CORPSE_REPORT("Could not snapshot: No process attached");
        return { };
    }
    return OwnedTaskHandle::adopt(process->taskPort());
}

#endif // OS(DARWIN)

Snapshot::Snapshot(RefPtr<Process> process)
    : m_process(WTF::move(process))
    , m_corpsePort(takeSnapshot(m_process.get()))
    , m_id(s_nextId++)
    , m_memory(corpsePort())
{
}

Snapshot::~Snapshot() = default;

} // namespace Corpse
} // namespace JSC

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(MYA)
