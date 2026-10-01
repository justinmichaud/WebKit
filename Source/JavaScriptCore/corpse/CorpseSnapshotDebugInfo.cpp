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

#include "config.h"
#include "CorpseSnapshotDebugInfo.h"

#if ENABLE(MYA)

#include "CorpseError.h"
#include "CorpseTargetType.h"
#include <wtf/TZoneMallocInlines.h>

#if HAVE(LLDB)

#include "CorpseImage.h"
#include "CorpseLimits.h"
#include "CorpseProcess.h"
#include "CorpseSnapshot.h"
#include <lldb/API/LLDB.h>
#include <mutex>
#include <string_view>
#include <wtf/HexNumber.h>
#include <wtf/text/StringBuilder.h>

namespace JSC {
namespace Corpse {

WTF_MAKE_TZONE_ALLOCATED_IMPL(SnapshotDebugInfo);

// Itanium C++ ABI: the offset to top is the second word before a vtable's address point.
static constexpr size_t offsetToTopBeforeAddressPoint = 2 * sizeof(uint64_t);

SnapshotDebugInfo::SnapshotDebugInfo(std::unique_ptr<lldb::SBDebugger>&& debugger, std::unique_ptr<lldb::SBTarget>&& target)
    : m_debugger(WTF::move(debugger))
    , m_target(WTF::move(target))
{
}

SnapshotDebugInfo::~SnapshotDebugInfo()
{
    m_classesOfVTables.clear();
    m_types.clear();
    m_debugger->DeleteTarget(*m_target);
    lldb::SBDebugger::Destroy(*m_debugger);
}

#if OS(DARWIN)
static UTF8CString uuidString(const Image::UUID& uuid)
{
    StringBuilder builder;
    for (size_t index = 0; index < uuid.size(); ++index) {
        if (index == 4 || index == 6 || index == 8 || index == 10)
            builder.append('-');
        builder.append(hex(uuid[index], 2));
    }
    return builder.toString().utf8();
}
#endif

RefPtr<SnapshotDebugInfo> SnapshotDebugInfo::create(Snapshot& snapshot)
{
    const Vector<Image>& images = snapshot.images();
    if (images.isEmpty())
        return nullptr; // images() said why.
    int pid = static_cast<int>(snapshot.process()->pid());
    CORPSE_DIAGNOSTICS(diagnostics, "opening the debug info of pid %d", pid);

    static std::once_flag once;
    std::call_once(once, [] {
        lldb::SBDebugger::Initialize();
    });

    auto debugger = makeUniqueWithoutFastMallocCheck<lldb::SBDebugger>(lldb::SBDebugger::Create(false));
    if (!debugger->IsValid()) {
        CORPSE_REPORT("liblldb could not create a debugger");
        return nullptr;
    }
    lldb::SBError error;
    // The target starts empty, so that every module is one of the snapshot's images, where the snapshot has it.
    auto target = makeUniqueWithoutFastMallocCheck<lldb::SBTarget>(debugger->CreateTarget(nullptr, nullptr, nullptr, false, error));
    if (!target->IsValid()) {
        CORPSE_REPORT("liblldb could not create a target: %s", error.GetCString() ? error.GetCString() : "unknown error");
        lldb::SBDebugger::Destroy(*debugger);
        return nullptr;
    }
    Ref debugInfo = adoptRef(*new SnapshotDebugInfo(WTF::move(debugger), WTF::move(target)));
    lldb::SBTarget& lldbTarget = *debugInfo->m_target;
    size_t opened = 0;

    for (const Image& image : images) {
#if OS(DARWIN)
        lldb::SBModule module = lldbTarget.AddModule(image.path().legacyCStringPointer(), nullptr, uuidString(image.uuid()).legacyCStringPointer());
#else
        lldb::SBModule module = lldbTarget.AddModule(image.path().legacyCStringPointer(), nullptr, nullptr);
#endif
        if (!module.IsValid()) {
            Diagnostics::count(DiagnosticCounter::ImagesWithoutDebugInfo);
            continue;
        }
#if OS(DARWIN)
        // dyld gives where the header is, and liblldb wants the slide.
        lldb::addr_t headerFileAddress = module.GetObjectFileHeaderAddress().GetFileAddress();
        bool loaded = headerFileAddress != LLDB_INVALID_ADDRESS
            && lldbTarget.SetModuleLoadAddress(module, image.loadAddress().toTargetVMAddress() - headerFileAddress).Success();
#else
        bool loaded = lldbTarget.SetModuleLoadAddress(module, image.loadAddress().toTargetVMAddress()).Success();
#endif
        if (!loaded) {
            lldbTarget.RemoveModule(module);
            Diagnostics::count(DiagnosticCounter::ImagesWithoutDebugInfo);
            continue;
        }
        ++opened;
    }

    RELEASE_ASSERT(!lldbTarget.GetProcess().IsValid());
    if (!opened) {
        CORPSE_REPORT("liblldb could not open any of the %zu images of pid %d", images.size(), pid);
        return nullptr;
    }
    return debugInfo;
}

const TargetType& SnapshotDebugInfo::type(const lldb::SBType& type)
{
    lldb::SBType canonical = lldb::SBType(type).GetCanonicalType();
    const char* name = canonical.GetName();
    auto& types = m_types.ensure(String::fromUTF8(name ? name : ""), [] {
        return Vector<std::unique_ptr<TargetType>> { };
    }).iterator->value;
    for (auto& existing : types) {
        if (*existing->m_type == canonical)
            return *existing;
    }
    types.append(std::unique_ptr<TargetType>(new TargetType(*this, canonical)));
    return *types.last();
}

bool SnapshotDebugInfo::isInImage(Address address) const
{
    return m_target->ResolveLoadAddress(address.toTargetVMAddress()).GetModule().IsValid();
}

static bool isDestructor(lldb::SBFunction& function)
{
    // DWARF marks a destructor only by its name, "~Class". GetName() is the
    // qualified name in every build; GetBaseName() and GetMangledName() depend
    // on whether the debug info has linkage names (-dwarf-linkage-names=Abstract).
    const char* name = function.GetName();
    if (!name)
        return false;
    std::string_view qualifiedName { name };
    size_t lastScope = qualifiedName.rfind("::");
    return lastScope != std::string_view::npos && qualifiedName.substr(lastScope + 2).starts_with('~');
}

const TargetType* SnapshotDebugInfo::classOfDestructor(Address function, bool& inImage)
{
    lldb::SBAddress address = m_target->ResolveLoadAddress(function.toTargetVMAddress());
    inImage = address.GetModule().IsValid();
    if (!inImage)
        return nullptr;
    lldb::SBFunction destructor = address.GetFunction();
    if (!destructor.IsValid() || !isDestructor(destructor))
        return nullptr;

    // A destructor's only parameter is `this`.
    lldb::SBValueList parameters = destructor.GetBlock().GetVariables(*m_target, true, false, false);
    if (parameters.GetSize() != 1) {
        CORPSE_REPORT("The destructor at 0x%llx has %u parameters in its debug info", function.toTargetVMAddress(), parameters.GetSize());
        return nullptr;
    }
    lldb::SBType type = parameters.GetValueAtIndex(0).GetType().GetPointeeType();
    if (!type.IsValid() || !type.IsTypeComplete()) {
        CORPSE_REPORT("The class of the destructor at 0x%llx is incomplete in its debug info", function.toTargetVMAddress());
        return nullptr;
    }
    return &this->type(type);
}

const TargetType* SnapshotDebugInfo::dynamicTypeAt(Snapshot& snapshot, Address address, Address& completeObject)
{
    if (!address) {
        CORPSE_REPORT("There is no object at a null address");
        return nullptr;
    }
    CORPSE_DIAGNOSTICS(diagnostics, "resolving the dynamic type of the object at 0x%llx", address.toTargetVMAddress());
    Memory& memory = snapshot.memory();

    auto vptr = memory.ptr<uint64_t>(address);
    if (!vptr) {
        CORPSE_REPORT("Could not read the vtable pointer of the object at 0x%llx", address.toTargetVMAddress());
        return nullptr;
    }
    Address vtable = Address { *vptr }.stripped();
    auto offsetToTop = memory.ptr<int64_t>(vtable - offsetToTopBeforeAddressPoint);
    if (!offsetToTop) {
        CORPSE_REPORT("Could not read the offset to top of the vtable at 0x%llx", vtable.toTargetVMAddress());
        return nullptr;
    }
    completeObject = address + static_cast<uint64_t>(*offsetToTop);

    // A base subobject's vtable holds thunks; the complete object's own vtable holds its destructor.
    if (*offsetToTop) {
        vptr = memory.ptr<uint64_t>(completeObject);
        if (!vptr) {
            CORPSE_REPORT("Could not read the vtable pointer of the complete object at 0x%llx", completeObject.toTargetVMAddress());
            return nullptr;
        }
        vtable = Address { *vptr }.stripped();
    }
    if (!isInImage(vtable)) {
        CORPSE_REPORT("The vtable at 0x%llx is in no image with debug info", vtable.toTargetVMAddress());
        return nullptr;
    }

    // A heap holds many objects of each class, so each vtable is looked up once.
    auto entry = m_classesOfVTables.ensure(vtable, [&] {
        return classOfVTable(snapshot, vtable);
    });
    if (!entry.isNewEntry && !entry.iterator->value)
        CORPSE_REPORT("The vtable at 0x%llx names no class, as an earlier lookup found", vtable.toTargetVMAddress());
    return entry.iterator->value;
}

const TargetType* SnapshotDebugInfo::classOfVTable(Snapshot& snapshot, Address vtable)
{
    Memory& memory = snapshot.memory();
    for (size_t slot = 0; slot < maxVTableSlots; ++slot) {
        auto entry = memory.ptr<uint64_t>(vtable + slot * sizeof(uint64_t));
        if (!entry) {
            CORPSE_REPORT("Could not read slot %zu of the vtable at 0x%llx", slot, vtable.toTargetVMAddress());
            return nullptr;
        }
        bool inImage = false;
        if (auto* type = classOfDestructor(Address { *entry }.stripped(), inImage))
            return type;
        if (!inImage)
            break;
        Diagnostics::count(DiagnosticCounter::VTableSlotsNotDestructors);
    }
    CORPSE_REPORT("No destructor in the vtable at 0x%llx: its class needs a virtual destructor", vtable.toTargetVMAddress());
    return nullptr;
}

} // namespace Corpse
} // namespace JSC

#else // HAVE(LLDB)

// Complete stand-ins for the liblldb classes the members point to, so that
// the destructor compiles. No instance is ever made.
namespace lldb {
class SBDebugger { };
class SBTarget { };
}

namespace JSC {
namespace Corpse {

WTF_MAKE_TZONE_ALLOCATED_IMPL(SnapshotDebugInfo);

SnapshotDebugInfo::~SnapshotDebugInfo() = default;

RefPtr<SnapshotDebugInfo> SnapshotDebugInfo::create(Snapshot&)
{
    CORPSE_REPORT("This build has no liblldb, so it cannot read debug info");
    return nullptr;
}

const TargetType* SnapshotDebugInfo::dynamicTypeAt(Snapshot&, Address, Address&)
{
    RELEASE_ASSERT_NOT_REACHED();
}

} // namespace Corpse
} // namespace JSC

#endif // HAVE(LLDB)

#endif // ENABLE(MYA)
