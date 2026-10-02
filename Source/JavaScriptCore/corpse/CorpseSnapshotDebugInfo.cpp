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
#include <optional>
#include <string>
#include <string_view>
#include <wtf/HexNumber.h>
#include <wtf/text/MakeString.h>
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
    m_compileUnitClasses.clear();
    m_classesOfVTables.clear();
    m_types.clear();
    m_debugger->DeleteTarget(*m_target);
    lldb::SBDebugger::Destroy(*m_debugger);
}

// The image's identity, as AddModule takes it: liblldb refuses a file whose
// UUID or build-id is not this one, so a rebuilt image is refused, not misread.
static UTF8CString identityString(const Image& image)
{
    StringBuilder builder;
#if OS(DARWIN)
    const Image::UUID& uuid = image.uuid();
    for (size_t index = 0; index < uuid.size(); ++index) {
        if (index == 4 || index == 6 || index == 8 || index == 10)
            builder.append('-');
        builder.append(hex(uuid[index], 2));
    }
#else
    for (uint8_t byte : image.buildID())
        builder.append(hex(byte, 2));
#endif
    return builder.toString().utf8();
}

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
#if !OS(DARWIN)
    // Each compile unit's .debug_names lists the type units it emitted, and a
    // linker that does not merge the indexes (LLD before 19) leaves an entry
    // for every type unit it discarded as a duplicate, with a tombstone for its
    // offset. liblldb then reads those entries' DIEs in the wrong unit. Its own
    // index of the DWARF is as fast here.
    lldb::SBDebugger::SetInternalVariable("plugin.symbol-file.dwarf.ignore-file-indexes", "true", debugger->GetInstanceName());
#endif
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
        lldb::SBModule module = lldbTarget.AddModule(image.path().legacyCStringPointer(), nullptr, identityString(image).legacyCStringPointer());
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
    return dynamicTypeAt(snapshot, address, completeObject, ReportFailures::Yes);
}

const TargetType* SnapshotDebugInfo::dynamicTypeIfAnyAt(Snapshot& snapshot, Address address, Address& completeObject)
{
    return dynamicTypeAt(snapshot, address, completeObject, ReportFailures::No);
}

#define CORPSE_REPORT_IF(reportFailures, format, ...) do { \
        if (reportFailures == ReportFailures::Yes) \
            CORPSE_REPORT(format __VA_OPT__(, __VA_ARGS__)); \
    } while (0)

const TargetType* SnapshotDebugInfo::dynamicTypeAt(Snapshot& snapshot, Address address, Address& completeObject, ReportFailures reportFailures)
{
    if (!address) {
        CORPSE_REPORT_IF(reportFailures, "There is no object at a null address");
        return nullptr;
    }
    std::optional<Diagnostics> diagnostics;
    if (reportFailures == ReportFailures::Yes)
        diagnostics.emplace(CORPSE_DESCRIBE("resolving the dynamic type of the object at 0x%llx", address.toTargetVMAddress()));
    Memory& memory = snapshot.memory();

    auto vptr = memory.ptr<uint64_t>(address);
    if (!vptr) {
        CORPSE_REPORT_IF(reportFailures, "Could not read the vtable pointer of the object at 0x%llx", address.toTargetVMAddress());
        return nullptr;
    }
    Address vtable = Address { *vptr }.stripped();
    // An object that is not polymorphic, read as if it were, holds anything in
    // its first word; only a pointer into an image's data can be a vtable.
    if (!isInImage(vtable)) {
        CORPSE_REPORT_IF(reportFailures, "The vtable at 0x%llx is in no image with debug info", vtable.toTargetVMAddress());
        return nullptr;
    }
    auto offsetToTop = memory.ptr<int64_t>(vtable - offsetToTopBeforeAddressPoint);
    if (!offsetToTop) {
        CORPSE_REPORT_IF(reportFailures, "Could not read the offset to top of the vtable at 0x%llx", vtable.toTargetVMAddress());
        return nullptr;
    }
    completeObject = address + static_cast<uint64_t>(*offsetToTop);

    // A base subobject's vtable holds thunks; the complete object's own vtable holds its destructor.
    if (*offsetToTop) {
        vptr = memory.ptr<uint64_t>(completeObject);
        if (!vptr) {
            CORPSE_REPORT_IF(reportFailures, "Could not read the vtable pointer of the complete object at 0x%llx", completeObject.toTargetVMAddress());
            return nullptr;
        }
        vtable = Address { *vptr }.stripped();
        if (!isInImage(vtable)) {
            CORPSE_REPORT_IF(reportFailures, "The vtable at 0x%llx is in no image with debug info", vtable.toTargetVMAddress());
            return nullptr;
        }
    }

    // A heap holds many objects of each class, so each vtable is looked up once.
    auto entry = m_classesOfVTables.ensure(vtable, [&] {
        return classOfVTable(snapshot, vtable, reportFailures);
    });
    if (!entry.isNewEntry && !entry.iterator->value)
        CORPSE_REPORT_IF(reportFailures, "The vtable at 0x%llx names no class, as an earlier lookup found", vtable.toTargetVMAddress());
    return entry.iterator->value;
}

const TargetType* SnapshotDebugInfo::classOfVTable(Snapshot& snapshot, Address vtable, ReportFailures reportFailures)
{
    Memory& memory = snapshot.memory();
    for (size_t slot = 0; slot < maxVTableSlots; ++slot) {
        auto entry = memory.ptr<uint64_t>(vtable + slot * sizeof(uint64_t));
        if (!entry) {
            CORPSE_REPORT_IF(reportFailures, "Could not read slot %zu of the vtable at 0x%llx", slot, vtable.toTargetVMAddress());
            return nullptr;
        }
        bool inImage = false;
        Address function = Address { *entry }.stripped();
        if (auto* type = classOfDestructor(function, inImage))
            return vtableSymbolNames(vtable, *type) ? type : nullptr;
        if (!inImage)
            break;
        Diagnostics::count(DiagnosticCounter::VTableSlotsNotDestructors);
    }
    CORPSE_REPORT_IF(reportFailures, "No destructor in the vtable at 0x%llx: its class needs a virtual destructor", vtable.toTargetVMAddress());
    return nullptr;
}

// Where an unnamed local type, such as a lambda's closure type, starts in a
// demangled name, and its length; or no length.
static size_t unnamedLocalTypeLength(std::string_view name)
{
    auto skipDigits = [&](size_t index) {
        while (index < name.size() && name[index] >= '0' && name[index] <= '9')
            ++index;
        return index;
    };
    auto skipParentheses = [&](size_t index) -> size_t {
        unsigned depth = 0;
        for (; index < name.size(); ++index) {
            if (name[index] == '(')
                ++depth;
            else if (name[index] == ')' && depth && !--depth)
                return index + 1;
        }
        return 0;
    };
    if (name.starts_with("$_"))
        return skipDigits(2);
    if (name.starts_with("'unnamed"))
        return name.find('\'', 1) == std::string_view::npos ? 0 : name.find('\'', 1) + 1;
    if (name.starts_with("'lambda")) {
        size_t index = skipDigits(7);
        if (index >= name.size() || name[index] != '\'')
            return 0;
        return skipParentheses(index + 1);
    }
    if (name.starts_with("{lambda(")) {
        size_t end = name.find('}');
        return end == std::string_view::npos ? 0 : end + 1;
    }
    return 0;
}

// A demangled name with each template or function argument that is an
// unnamed local type spelled '?'. The two spellings compared below disagree on
// such a type: the symbol table's names it as the compiler numbered it, in its
// enclosing function as declared, and liblldb's as it numbered it again in the
// class it rebuilt, in that function as instantiated. Only another unnamed
// local type in the same position matches one, and in a build that folds no
// functions, rule 1 has no other class to confuse it with.
static std::string withUnnamedLocalTypesErased(std::string_view name)
{
    std::string result;
    result.reserve(name.size());
    Vector<size_t, 8> argumentStarts;
    size_t index = 0;
    while (index < name.size()) {
        if (size_t length = unnamedLocalTypeLength(name.substr(index))) {
            result.resize(argumentStarts.isEmpty() ? 0 : argumentStarts.last());
            result += '?';
            index += length;
            continue;
        }
        char character = name[index++];
        result += character;
        if (character == '<' || character == '(')
            argumentStarts.append(result.size());
        else if ((character == '>' || character == ')') && !argumentStarts.isEmpty())
            argumentStarts.removeLast();
        else if (character == ',' && !argumentStarts.isEmpty()) {
            if (index < name.size() && name[index] == ' ')
                result += name[index++];
            argumentStarts.last() = result.size();
        }
    }
    return result;
}

// Whether `member`, a demangled member function, is a member of the class
// `className` itself: "className::name(...)", with no further scope in `name`.
static bool isMemberOf(std::string_view member, std::string_view className)
{
    if (!member.starts_with(className) || !member.substr(className.size()).starts_with("::"))
        return false;
    std::string_view name = member.substr(className.size() + 2);
    unsigned depth = 0;
    for (size_t index = 0; index < name.size(); ++index) {
        char character = name[index];
        if (character == '<')
            ++depth;
        else if (character == '>' && depth)
            --depth;
        else if (!depth && character == '(')
            return index > 0;
        else if (!depth && name.substr(index).starts_with("::"))
            return false;
    }
    return false;
}

// A member function the class declares, demangled: its destructor if liblldb
// lists one, and otherwise any other. liblldb leaves the implicit members out
// of a class, so a class whose destructor is implicit lists none. liblldb
// computes each one's linkage name with clang's mangler from the class itself,
// and demangles it with LLVM's demangler, as it does the symbol table's names.
static std::string declaredMember(lldb::SBType& type)
{
    std::string result;
    uint32_t count = type.GetNumberOfMemberFunctions();
    for (uint32_t index = 0; index < count; ++index) {
        lldb::SBTypeMemberFunction function = type.GetMemberFunctionAtIndex(index);
        const char* demangledName = function.GetDemangledName();
        std::string member = demangledName ? demangledName : std::string { };
        if (member.empty())
            continue;
        if (function.GetKind() == lldb::eMemberFunctionKindDestructor)
            return member;
        if (result.empty())
            result = WTF::move(member);
    }
    return result;
}

// One function can be the destructor of two classes: clang makes a derived
// class's destructor an alias of its base's when it does nothing more, and
// linkers fold identical functions. Neither folds a vtable, which is
// relocated data, so the vtable's own symbol checks the class.
bool SnapshotDebugInfo::vtableSymbolNames(Address vtable, const TargetType& type)
{
    lldb::SBSymbol symbol = m_target->ResolveLoadAddress(vtable.toTargetVMAddress()).GetSymbol();
    const char* vtableName = symbol.IsValid() ? symbol.GetMangledName() : nullptr;
    if (!vtableName) {
        // A stripped image: the object is read as its static type.
        Diagnostics::count(DiagnosticCounter::VTablesWithoutSymbol);
        return false;
    }
    // Anything but a complete object's vtable, such as a construction vtable (_ZTC), names no class.
    // The name is liblldb's, from the demangler that spelled the member's.
    // Debian 12's __cxa_demangle cannot demangle a C++20 constraint.
    const char* vtableDemangledName = std::string_view { vtableName }.starts_with("_ZTV") ? symbol.GetName() : nullptr;
    std::string vtableClass = vtableDemangledName ? vtableDemangledName : std::string { };
    constexpr std::string_view vtablePrefix = "vtable for ";
    if (!vtableClass.starts_with(vtablePrefix)) {
        CORPSE_REPORT("The vtable at 0x%llx is in '%s', which is not a class's vtable", vtable.toTargetVMAddress(), vtableName);
        return false;
    }
    vtableClass.erase(0, vtablePrefix.size());

    // The class rule 1 found must be the vtable's: its own members are named
    // as members of that class, and a base's, a derived class's or a nested
    // class's are not.
    std::string member = declaredMember(*type.m_type);
    if (member.empty()) {
        CORPSE_REPORT("'%s', the class of the destructor in the vtable at 0x%llx, declares no member function with a linkage name", type.name().legacyCStringPointer(), vtable.toTargetVMAddress());
        return false;
    }
    if (!isMemberOf(withUnnamedLocalTypesErased(member), withUnnamedLocalTypesErased(vtableClass))) {
        CORPSE_REPORT("The vtable at 0x%llx is the vtable of '%s', but the class its destructor gives declares '%s'", vtable.toTargetVMAddress(), vtableClass.c_str(), member.c_str());
        return false;
    }
    return true;
}

// The classes of a compile unit, listed once.
struct SnapshotDebugInfo::CompileUnitClasses {
    Vector<lldb::SBType> classes;
};

const TargetType* SnapshotDebugInfo::classInCompileUnitOf(Address variable, const char* memberName, const char* linkageName)
{
    lldb::SBAddress resolved = m_target->ResolveLoadAddress(variable.toTargetVMAddress());
    lldb::SBSymbol symbol = resolved.GetSymbol();
    if (!symbol.IsValid() || symbol.GetStartAddress() != resolved || !symbol.GetMangledName()) {
        CORPSE_REPORT("No variable starts at 0x%llx", variable.toTargetVMAddress());
        return nullptr;
    }
    if (!linkageName)
        linkageName = symbol.GetMangledName();

    // A data address is in no compile unit's code, so liblldb finds the global
    // variable that contains it, and the compile unit that defines that.
    lldb::SBSymbolContext context = m_target->ResolveSymbolContextForAddress(resolved, lldb::eSymbolContextCompUnit | lldb::eSymbolContextVariable);
    lldb::SBCompileUnit unit = context.GetCompileUnit();
    lldb::SBFileSpec unitFile = unit.GetFileSpec();
    lldb::SBFileSpec moduleFile = context.GetModule().GetFileSpec();
    if (!unit.IsValid() || !unitFile.GetFilename()) {
        CORPSE_REPORT("No compile unit defines '%s' at 0x%llx", symbol.GetName(), variable.toTargetVMAddress());
        return nullptr;
    }

    auto string = [](const char* characters) {
        return String::fromUTF8(characters ? characters : "");
    };
    String key = makeString(string(moduleFile.GetDirectory()), '/', string(moduleFile.GetFilename()), '\n', string(unitFile.GetDirectory()), '/', string(unitFile.GetFilename()));
    auto& unitClasses = m_compileUnitClasses.ensure(key, [&] {
        auto result = makeUniqueWithoutFastMallocCheck<CompileUnitClasses>();
        lldb::SBTypeList types = unit.GetTypes(lldb::eTypeClassClass | lldb::eTypeClassStruct);
        for (uint32_t index = 0; index < types.GetSize(); ++index)
            result->classes.append(types.GetTypeAtIndex(index));
        return result;
    }).iterator->value;

    // liblldb names a static member with clang's own mangler, from the class
    // it built, so a match means this class declares that member.
    std::optional<lldb::SBType> match;
    std::string_view expected { linkageName };
    for (lldb::SBType& candidate : unitClasses->classes) {
        lldb::SBTypeStaticField member = candidate.GetStaticFieldWithName(memberName);
        const char* candidateName = member.IsValid() ? member.GetMangledName() : nullptr;
        if (!candidateName || std::string_view { candidateName } != expected)
            continue;
        if (match && !(*match == candidate)) {
            CORPSE_REPORT("Two classes of the compile unit of 0x%llx declare '%s'", variable.toTargetVMAddress(), linkageName);
            return nullptr;
        }
        match = candidate;
    }
    if (!match) {
        CORPSE_REPORT("No class of the compile unit of 0x%llx declares '%s'", variable.toTargetVMAddress(), linkageName);
        return nullptr;
    }
    return &type(*match);
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

const TargetType* SnapshotDebugInfo::dynamicTypeIfAnyAt(Snapshot&, Address, Address&)
{
    RELEASE_ASSERT_NOT_REACHED();
}

const TargetType* SnapshotDebugInfo::classInCompileUnitOf(Address, const char*, const char*)
{
    RELEASE_ASSERT_NOT_REACHED();
}

} // namespace Corpse
} // namespace JSC

#endif // HAVE(LLDB)

#endif // ENABLE(MYA)
