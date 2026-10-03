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

#pragma once

#include <JavaScriptCore/CorpsePlatform.h>

#if ENABLE(MYA)

#include <JavaScriptCore/CorpseAddress.h>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <wtf/Function.h>
#include <wtf/HashMap.h>
#include <wtf/RefCounted.h>
#include <wtf/RefPtr.h>
#include <wtf/TZoneMalloc.h>
#include <wtf/Vector.h>
#include <wtf/text/StringHash.h>
#include <wtf/text/WTFString.h>

namespace lldb {
class SBDebugger;
class SBCompileUnit;
class SBModule;
class SBTarget;
class SBType;
}

namespace JSC {
namespace Corpse {

class Snapshot;
class TargetType;

// The debug info of the images in a snapshot, opened through liblldb. liblldb
// only ever reads files here: it is never given a process. An image whose
// debug info cannot be opened is left out, and a lookup that lands in it fails.
class SnapshotDebugInfo : public RefCounted<SnapshotDebugInfo> {
    WTF_MAKE_TZONE_ALLOCATED(SnapshotDebugInfo);
public:
    static RefPtr<SnapshotDebugInfo> create(Snapshot&);
    ~SnapshotDebugInfo();

    // The class of the complete object containing the polymorphic object at
    // `address`, and where that complete object starts. The class must have a
    // virtual destructor: its vtable's destructor is what names the class.
    const TargetType* dynamicTypeAt(Snapshot&, Address, Address& completeObject);

    // dynamicTypeAt, for a walk that expects some objects to have none: a
    // failure is counted, not reported. A class without a virtual destructor
    // has no dynamic type, and nor does an object that is not polymorphic. A
    // vtable whose symbol refuses the class its destructor gives is reported
    // either way.
    const TargetType* dynamicTypeIfAnyAt(Snapshot&, Address, Address& completeObject);

    // The complete class liblldb names `name` in the image mapped at
    // `inImage`. `name` is spelled as liblldb spells it. Null, silently, if
    // that image has none.
    const TargetType* classNamed(Address inImage, const char* name);
    // The same, in the image that describes `neighbor`, such as WebCore for one
    // of its classes, or else in the first image that has one.
    const TargetType* classNamedBeside(const TargetType& neighbor, const char* name);
    // The class `declaration` declares, as the image that describes the
    // declaration defines it, or, if liblldb knows no image for it, as the one
    // image that defines a class of its name; or null. liblldb does not
    // complete every declaration a type of an image refers to from that
    // image's definition.
    const TargetType* definitionInItsImage(const TargetType& declaration);

    // The class whose static data member `member` is the variable starting at
    // `variable`, such as a JS cell class's s_info: the one class whose member
    // has the linkage name of the symbol there. `codeOfClass`, functions that
    // may be the class's, finds an instance of a class template whose name
    // liblldb spells differently. Null, having reported why, if there is none.
    const TargetType* classOfStaticMember(Address variable, const char* member, std::span<const Address> codeOfClass);

    struct Symbol {
        Address address;
        String name; // Demangled.
    };
    // Every data symbol of the image mapped at `inImage` whose mangled name
    // ends in `suffix`.
    Vector<Symbol> dataSymbolsEndingWith(Address inImage, const char* suffix);

    // Where the symbol `name`, a mangled name, is in the image mapped at `inImage`.
    std::optional<Address> symbolAddress(Address inImage, const char* name);

    // The symbol `address` lies in, as "name" or "name+0x10". Null if none does.
    String symbolAt(Address);

    struct GlobalVariable {
        Address address;
        const TargetType& type;
    };
    struct Range {
        Address address;
        uint64_t size;
    };
    // Every global variable of every image with debug info at an address
    // `isCandidate` accepts, as its declared type: each data symbol the debug
    // info describes as a variable at the symbol's address. A function-local
    // static is found in its function. The candidate data symbols that are no
    // variable the debug info describes, such as guard variables, are counted
    // in `untyped`, and the blocks of statics LLVM's GlobalMerge merged into
    // one symbol, whose variables have no symbol of their own, are listed in
    // `merged`.
    Vector<GlobalVariable> globalVariables(const Function<bool(Address)>& isCandidate, size_t& untyped, Vector<Range>& merged);

private:
    friend class TargetType;

    SnapshotDebugInfo(std::unique_ptr<lldb::SBDebugger>&&, std::unique_ptr<lldb::SBTarget>&&);

    // The one TargetType for `type`, made the first time it is asked for, so
    // that each type's layout is read from the debug info once.
    const TargetType& type(const lldb::SBType&);

    enum class ReportFailures : bool { No, Yes };
    const TargetType* dynamicTypeAt(Snapshot&, Address, Address& completeObject, ReportFailures);

    // The class the vtable at `vtable` belongs to, or null, having reported why.
    const TargetType* classOfVTable(Snapshot&, Address vtable, ReportFailures);

    // Whether the symbol the vtable at `vtable` lies in names the class whose
    // destructor gave `type`. It never supplies a type, only refuses one, and
    // reports why unless the image has no symbol table.
    bool vtableSymbolNames(Address vtable, const TargetType&);

    // The class whose destructor starts at `function`, or null. `inImage` is
    // false when no image with debug info is mapped there, which is how a walk
    // over vtable slots ends.
    const TargetType* classOfDestructor(Address function, bool& inImage);
    bool isInImage(Address) const;
    // The one class of `unit` whose name starts with `namePrefix` that `matches`.
    const TargetType* classInCompileUnit(lldb::SBCompileUnit&, const lldb::SBModule&, std::string_view namePrefix, const Function<bool(lldb::SBType&)>& matches);
    // The module of the image mapped at `address`, which may be invalid.
    lldb::SBModule moduleAt(Address) const;

    std::unique_ptr<lldb::SBDebugger> m_debugger;
    std::unique_ptr<lldb::SBTarget> m_target;
    // By name; types of one name from different images or anonymous namespaces share an entry.
    HashMap<String, Vector<std::unique_ptr<TargetType>>> m_types;
    HashMap<Address, const TargetType*> m_classesOfVTables;
    HashMap<const TargetType*, const TargetType*> m_definitions;
    struct DynamicTypeOfFirstWord {
        int64_t offsetToTop;
        const TargetType* type; // Null if an object with this first word has none.
    };
    HashMap<Address, DynamicTypeOfFirstWord> m_dynamicTypesOfFirstWords;
    struct CompileUnitClasses;
    // By the compile unit's image and source file.
    HashMap<String, std::unique_ptr<CompileUnitClasses>> m_compileUnitClasses;
};

} // namespace Corpse
} // namespace JSC

#endif // ENABLE(MYA)
