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
#include "TypeinfoTest.h"

#if (OS(MACOS) || USE(APPLE_INTERNAL_SDK)) && !PLATFORM(MACCATALYST) && !PLATFORM(IOS_FAMILY_SIMULATOR)

#include "LibJSCToolsTestUtilities.h"

#include <JavaScriptCore/CorpseAddress.h>
#include <JavaScriptCore/CorpseSnapshot.h>
#include <JavaScriptCore/CorpseTargetObject.h>
#include <JavaScriptCore/CorpseTargetType.h>
#include <memory>
#include <string.h>
#include <wtf/MediaTime.h>
#include <wtf/MonotonicTime.h>
#include <wtf/Ref.h>
#include <wtf/RefPtr.h>
#include <wtf/Seconds.h>
#include <wtf/Variant.h>
#include <wtf/Vector.h>
#include <wtf/WallTime.h>
#include <wtf/text/AtomString.h>
#include <wtf/text/CString.h>
#include <wtf/text/StringImpl.h>
#include <wtf/text/WTFString.h>

namespace JSCToolsTest {

using JSC::Corpse::Address;
using JSC::Corpse::Snapshot;
using JSC::Corpse::TargetField;
using JSC::Corpse::TargetObject;
using JSC::Corpse::TargetType;

void testTypeinfo()
{
    if (!beginSuite("Typeinfo"))
        return;

    // Force the lazy hash so m_hashAndFlags is stable for the comparison below.
    Ref<WTF::StringImpl> impl = WTF::StringImpl::create("stringimpl-under-test"_span);
    impl->hash();

    TEST_ASSERT(impl->is8Bit(), "the test string is Latin1");
    TEST_ASSERT(impl->hasOneRef(), "a fresh Ref carries one reference");

    Address implAddress { impl.ptr() };

    SelfSnapshot self;
    if (!self.isValid())
        return;
    Snapshot& snapshot = self.snapshot();

    // The corpse's type system describes WTF::StringImplShape (the base that
    // owns StringImpl's data members). If findType returns nullopt here, the
    // target binary carries no debug info for that class and the rest of the
    // suite has nothing to compare against.
    auto type = snapshot.findType("WTF::StringImplShape"_s);
    TEST_ASSERT(type, "corpse describes WTF::StringImplShape");
    if (!type)
        return;

    TEST_ASSERT_EQ(static_cast<size_t>(type->byteSize()), sizeof(WTF::StringImpl),
        "TargetType byte size for the shape base matches sizeof(StringImpl)");
    TEST_ASSERT(!type->fields().isEmpty(), "TargetType has at least one field");
    TEST_ASSERT(type->field("m_length"_s), "TargetType exposes m_length by name");

    // StringImplShape has no vtable, so getTargetObject has no RTTI check to
    // run. A polymorphic type with a resolved vtable would return nullopt if
    // the vptr in the corpse disagreed with the vtable symbol.
    TEST_ASSERT(!type->isPolymorphic(), "the shape base is not polymorphic");

    auto obj = snapshot.getTargetObject(implAddress, *type);
    TEST_ASSERT(obj, "TargetObject bound to the StringImpl address");
    if (!obj)
        return;

    TEST_ASSERT_EQ(static_cast<size_t>(obj->size()), sizeof(WTF::StringImpl),
        "TargetObject size matches its TargetType");
    TEST_ASSERT(obj->base() == implAddress, "TargetObject base is the StringImpl address");
    TEST_ASSERT_EQ(obj->fields().size(), type->fields().size(),
        "TargetObject exposes the same fields as its TargetType");

    // Each mapped field is read as a span of raw bytes and interpreted by
    // memcpy into a local of matching width. The size gate rejects a field
    // whose debug-info width disagrees with the C++ side; that mismatch is
    // exactly the drift this suite exists to catch.
    size_t comparedFields = 0;
    if (auto span = obj->get("m_length"_s)) {
        ++comparedFields;
        TEST_ASSERT_EQ(span->size(), sizeof(uint32_t), "m_length is 4 bytes");
        if (span->size() == sizeof(uint32_t)) {
            uint32_t length = 0;
            memcpy(&length, span->data(), sizeof(uint32_t));
            TEST_ASSERT_EQ(length, impl->length(), "m_length in corpse matches impl->length()");
        }
    }
    if (auto span = obj->get("m_refCount"_s)) {
        ++comparedFields;
        TEST_ASSERT_EQ(span->size(), sizeof(uint32_t), "m_refCount is 4 bytes");
        if (span->size() == sizeof(uint32_t)) {
            uint32_t refCount = 0;
            memcpy(&refCount, span->data(), sizeof(uint32_t));
            // The raw counter stores refCount() * s_refCountIncrement (0x2)
            // so a single Ref shows up as 2.
            TEST_ASSERT_EQ(refCount, static_cast<uint32_t>(0x2),
                "m_refCount in corpse matches expected raw refcount");
        }
    }
    if (const auto* field = type->field("m_hashAndFlags"_s)) {
        ++comparedFields;
        TEST_ASSERT_EQ(static_cast<size_t>(field->byteOffset),
            static_cast<size_t>(WTF::StringImpl::flagsOffset()),
            "TargetType offset for m_hashAndFlags matches flagsOffset()");
        auto span = obj->get(*field);
        TEST_ASSERT_EQ(span.size(), sizeof(uint32_t), "m_hashAndFlags is 4 bytes");
        if (span.size() == sizeof(uint32_t)) {
            uint32_t flagsWord = 0;
            memcpy(&flagsWord, span.data(), sizeof(uint32_t));
            unsigned expectedFlagsWord = (impl->hash() << 8) | (flagsWord & 0xff);
            TEST_ASSERT_EQ(flagsWord, expectedFlagsWord,
                "m_hashAndFlags word combines hash and flag bits");
        }
    }

    // The character-pointer members live inside an anonymous union whose
    // arms carry different names. The API surfaces them at the union's own
    // offset, so a caller reaches one of them by name without knowing the
    // union exists.
    bool foundDataField = false;
    for (StringView name : { "m_data8"_s, "m_data16"_s, "m_data8Char"_s, "m_data16Char"_s }) {
        auto span = obj->get(name);
        if (!span)
            continue;
        foundDataField = true;
        ++comparedFields;
        TEST_ASSERT_EQ(span->size(), sizeof(uintptr_t), "data pointer is pointer-sized");
        if (span->size() == sizeof(uintptr_t)) {
            uintptr_t pointer = 0;
            memcpy(&pointer, span->data(), sizeof(uintptr_t));
            TEST_ASSERT_HEX_EQ(pointer,
                reinterpret_cast<uintptr_t>(impl->span8().data()),
                "data pointer field matches impl->span8().data()");
        }
        break;
    }
    TEST_ASSERT(foundDataField, "a data pointer member is present in TargetType::fields()");
    TEST_ASSERT(comparedFields > 0, "at least one StringImplShape member was cross-checked");
}

namespace {

// Locates the target's DWARF description of T against candidate qualified
// names (templates surface under whichever fully instantiated spelling clang
// emitted). Verifies byteSize matches sizeof and that getTargetObject can bind
// the instance. Returns true when both hold; a miss is reported as a skip
// because the type may simply be spelled a way we did not anticipate.
template<typename T>
bool checkTypeLayout(Snapshot& snapshot, const T& instance,
    std::initializer_list<StringView> nameCandidates)
{
    std::optional<TargetType> type;
    StringView foundName;
    for (StringView candidate : nameCandidates) {
        type = snapshot.findType(candidate);
        if (type) {
            foundName = candidate;
            break;
        }
    }
    if (!type) {
        dataLogLn("    skipped: no DWARF match for ", *nameCandidates.begin());
        return false;
    }

    TEST_ASSERT_EQ(static_cast<size_t>(type->byteSize()), sizeof(T), foundName);

    auto obj = snapshot.getTargetObject(Address { std::addressof(instance) }, *type);
    TEST_ASSERT(obj, foundName);
    return obj.has_value();
}

} // anonymous namespace

void testCommonTypeLayouts()
{
    if (!beginSuite("CommonTypeLayouts"))
        return;

    SelfSnapshot self;
    if (!self.isValid())
        return;
    Snapshot& snapshot = self.snapshot();

    unsigned typesChecked = 0;

    // Time and duration wrappers -- single-member structs.
    WTF::Seconds seconds { 3.5 };
    if (checkTypeLayout(snapshot, seconds, { "WTF::Seconds"_s }))
        ++typesChecked;

    WTF::MonotonicTime monotonic = WTF::MonotonicTime::now();
    if (checkTypeLayout(snapshot, monotonic, { "WTF::MonotonicTime"_s }))
        ++typesChecked;

    WTF::WallTime wall = WTF::WallTime::now();
    if (checkTypeLayout(snapshot, wall, { "WTF::WallTime"_s }))
        ++typesChecked;

    // MediaTime -- rational time (numerator + denominator + flags).
    WTF::MediaTime media { 100, 25 };
    if (checkTypeLayout(snapshot, media, { "WTF::MediaTime"_s }))
        ++typesChecked;

    // String family -- each wraps a refcounted impl.
    WTF::CString cstr { "test-cstring" };
    if (checkTypeLayout(snapshot, cstr, { "WTF::CString"_s }))
        ++typesChecked;

    WTF::String str { "test-string"_s };
    if (checkTypeLayout(snapshot, str, { "WTF::String"_s }))
        ++typesChecked;

    WTF::AtomString atomStr { "test-atom"_s };
    if (checkTypeLayout(snapshot, atomStr, { "WTF::AtomString"_s }))
        ++typesChecked;

    // Ref / RefPtr -- template instantiations with default trait parameters
    // that DWARF sometimes spells in full.
    Ref<WTF::StringImpl> ref = WTF::StringImpl::create("ref-test"_span);
    if (checkTypeLayout(snapshot, ref, {
        "WTF::Ref<WTF::StringImpl>"_s,
        "WTF::Ref<WTF::StringImpl, WTF::RawPtrTraits<WTF::StringImpl> >"_s,
    }))
        ++typesChecked;

    RefPtr<WTF::StringImpl> refPtr = WTF::StringImpl::create("refptr-test"_span);
    if (checkTypeLayout(snapshot, refPtr, {
        "WTF::RefPtr<WTF::StringImpl>"_s,
        "WTF::RefPtr<WTF::StringImpl, WTF::RawPtrTraits<WTF::StringImpl>, WTF::DefaultRefDerefTraits<WTF::StringImpl> >"_s,
    }))
        ++typesChecked;

    // Vector -- default inline capacity and overflow-handling parameters.
    WTF::Vector<int> vec;
    vec.append(42);
    if (checkTypeLayout(snapshot, vec, {
        "WTF::Vector<int>"_s,
        "WTF::Vector<int, 0>"_s,
        "WTF::Vector<int, 0, WTF::CrashOnOverflow, 16, WTF::FastMalloc>"_s,
    }))
        ++typesChecked;

    // Variant -- tests the anonymous-union-flattening path since a variant's
    // storage is a tagged union.
    WTF::Variant<uint32_t, uint64_t> variant { static_cast<uint64_t>(0xDEADBEEFCAFEBABEull) };
    if (checkTypeLayout(snapshot, variant, {
        "WTF::Variant<unsigned int, unsigned long>"_s,
        "mpark::variant<unsigned int, unsigned long>"_s,
        "WTF::Variant<unsigned int, unsigned long long>"_s,
        "mpark::variant<unsigned int, unsigned long long>"_s,
    }))
        ++typesChecked;

    // JSC::JSObject -- lives in JSC and cannot be default-constructed without
    // a VM, so check only that DWARF describes it and reports fields. A live
    // instance is not needed to validate that the type system reaches JSC
    // headers.
    if (auto jsObjectType = snapshot.findType("JSC::JSObject"_s)) {
        TEST_ASSERT(jsObjectType->byteSize() > 0, "JSC::JSObject byteSize is nonzero");
        TEST_ASSERT(!jsObjectType->fields().isEmpty(), "JSC::JSObject has fields in DWARF");
        ++typesChecked;
    } else
        dataLogLn("    skipped: no DWARF match for JSC::JSObject");

    TEST_ASSERT(typesChecked > 0, "at least one common type layout was cross-checked");
}

} // namespace JSCToolsTest

#endif // (OS(MACOS) || USE(APPLE_INTERNAL_SDK)) && !PLATFORM(MACCATALYST) && !PLATFORM(IOS_FAMILY_SIMULATOR)
