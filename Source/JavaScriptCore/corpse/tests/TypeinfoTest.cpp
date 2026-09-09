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

#if HAVE(CORPSE_SUPPORT)

#include "LibJSCToolsTestUtilities.h"

#include <JavaScriptCore/CorpseAddress.h>
#include <JavaScriptCore/CorpseError.h>
#include <JavaScriptCore/CorpseSnapshot.h>
#include <JavaScriptCore/CorpseTargetObject.h>
#include <JavaScriptCore/CorpseTargetType.h>
#include <JavaScriptCore/CorpseTypeInfo.h>
#include <memory>
#include <string.h>
#include <wtf/MediaTime.h>
#include <wtf/MonotonicTime.h>
#include <wtf/Ref.h>
#include <wtf/RefPtr.h>
#include <wtf/Seconds.h>
#include <wtf/StringPrintStream.h>
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
    SuiteTracer tracer("Typeinfo");
    if (!tracer.shouldRun())
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

    // A member's own type, resolved through the debug info rather than by name.
    auto lengthType = snapshot.findTypeOfMember("WTF::StringImplShape"_s, "m_length"_s);
    TEST_ASSERT(lengthType, "corpse describes the type of StringImplShape::m_length");
    if (lengthType) {
        TEST_ASSERT_EQ(static_cast<size_t>(lengthType->byteSize()), sizeof(uint32_t),
            "the type of m_length is four bytes wide");
    }

    // Reachable for the same reason it is reachable in fields().
    auto dataType = snapshot.findTypeOfMember("WTF::StringImplShape"_s, "m_data8"_s);
    TEST_ASSERT(dataType, "corpse describes the type of StringImplShape::m_data8");
    if (dataType) {
        TEST_ASSERT_EQ(static_cast<size_t>(dataType->byteSize()), sizeof(uintptr_t),
            "m_data8 is pointer-sized");
    }

    // m_length is StringImpl's only by inheritance, so this covers the search
    // looking through base classes.
    auto inheritedType = snapshot.findTypeOfMember("WTF::StringImpl"_s, "m_length"_s);
    TEST_ASSERT(inheritedType, "a member inherited from a base is found by name");
    if (inheritedType) {
        TEST_ASSERT_EQ(static_cast<size_t>(inheritedType->byteSize()), sizeof(uint32_t),
            "the inherited m_length is four bytes wide");
    }

    // A member that does not exist is not answered for either.
    TEST_ASSERT(!snapshot.findTypeOfMember("WTF::StringImplShape"_s, "m_notAMember"_s),
        "a member the type does not have reports no type");
}

// A hierarchy of the suite's own, so that reading a derived object through its
// base is checked against types whose shape the suite controls rather than
// against whatever WTF happens to derive today. The virtual destructors are out
// of line so that each class has a key function, and so exactly one vtable is
// emitted for each.
class WalkBase {
public:
    WalkBase();
    virtual ~WalkBase();
    virtual int kind() const;

    uint32_t m_baseField { 0xba5e };
};

class WalkDerived : public WalkBase {
public:
    WalkDerived();
    ~WalkDerived() override;
    int kind() const override;

    uint32_t m_derivedField { 0xde21 };
};

// Templated classes, which is where a hand-written mangler would have given up:
// the ABI spells template arguments and non-type arguments with encodings and
// substitutions that only a real demangler covers. One with a type argument and
// one with a non-type argument as well.
template<typename T>
class WalkTemplate : public WalkBase {
public:
    ~WalkTemplate() override = default;
    int kind() const override { return 3; }

    T m_templateField { };
};

template<typename T, unsigned count>
class WalkFixedArray : public WalkBase {
public:
    ~WalkFixedArray() override = default;
    int kind() const override { return 4; }

    T m_values[count] { };
};

WalkBase::WalkBase() = default;
WalkBase::~WalkBase() = default;
int WalkBase::kind() const { return 1; }

WalkDerived::WalkDerived() = default;
WalkDerived::~WalkDerived() = default;
int WalkDerived::kind() const { return 2; }

namespace TypeAnchors {

// One anchor per type the suite checks, so that findTypeOfPointee gets the
// compiler's own spelling of it and a type whose template parameters change
// cannot quietly stop being checked. Nothing dereferences them; they are not
// static so that their pointees are certain to be described.
WTF::Seconds* seconds;
WTF::MonotonicTime* monotonicTime;
WTF::WallTime* wallTime;
WTF::MediaTime* mediaTime;
WTF::CString* cString;
WTF::String* string;
WTF::AtomString* atomString;
Ref<WTF::StringImpl>* ref;
RefPtr<WTF::StringImpl>* refPtr;
WTF::Vector<int>* vector;
WTF::Variant<uint32_t, uint64_t>* variant;
WTF::StringPrintStream* stringPrintStream;
WalkBase* walkBase;
WalkDerived* walkDerived;
WalkTemplate<int>* walkTemplate;
WalkFixedArray<uint64_t, 4>* walkFixedArray;

} // namespace TypeAnchors

namespace {

// A type that does not resolve is a failure rather than a skip: the anchor is
// in this translation unit, so not finding it means the type system is not
// reading the debug info at all.
template<typename T>
bool checkTypeLayout(Snapshot& snapshot, const T& instance, StringView anchorName)
{
    auto type = snapshot.findTypeOfPointee(anchorName);
    TEST_ASSERT(type, anchorName);
    if (!type)
        return false;

    TEST_ASSERT_EQ(static_cast<size_t>(type->byteSize()), sizeof(T), anchorName);

    auto object = snapshot.getTargetObject(Address { std::addressof(instance) }, *type);
    TEST_ASSERT(object, anchorName);
    return object.has_value();
}

} // anonymous namespace

void testCommonTypeLayouts()
{
    SuiteTracer tracer("CommonTypeLayouts");
    if (!tracer.shouldRun())
        return;

    // Built before the corpse is taken: a corpse is frozen at that moment, so
    // an object made after it reads back as whatever held that memory before.
    WTF::Seconds seconds { 3.5 };
    WTF::MonotonicTime monotonic = WTF::MonotonicTime::now();
    WTF::WallTime wall = WTF::WallTime::now();
    WTF::MediaTime media { 100, 25 };
    WTF::CString cstr { "test-cstring" };
    WTF::String str { "test-string"_s };
    WTF::AtomString atomStr { "test-atom"_s };
    Ref<WTF::StringImpl> ref = WTF::StringImpl::create("ref-test"_span);
    RefPtr<WTF::StringImpl> refPtr = WTF::StringImpl::create("refptr-test"_span);
    WTF::Vector<int> vec;
    vec.append(42);
    WTF::Variant<uint32_t, uint64_t> variant { static_cast<uint64_t>(0xDEADBEEFCAFEBABEull) };
    WTF::StringPrintStream stream;
    stream.print("polymorphic-under-test");

    // For the negative case below, built here for the same reason.
    alignas(WTF::StringPrintStream) uint8_t notAStream[sizeof(WTF::StringPrintStream)] = { };

    // For the derived-object case below. Like everything above, these have to
    // exist before the corpse is taken: the corpse freezes the memory, so an
    // object constructed afterwards is not in it.
    WalkBase walkBase;
    WalkDerived walkDerived;
    WalkTemplate<int> walkTemplate;
    WalkFixedArray<uint64_t, 4> walkFixedArray;

    SelfSnapshot self;
    if (!self.isValid())
        return;
    Snapshot& snapshot = self.snapshot();

    unsigned typesChecked = 0;

    // Time and duration wrappers, whose one member lives in a base class.
    if (checkTypeLayout(snapshot, seconds, "JSCToolsTest::TypeAnchors::seconds"_s))
        ++typesChecked;
    if (checkTypeLayout(snapshot, monotonic, "JSCToolsTest::TypeAnchors::monotonicTime"_s))
        ++typesChecked;
    if (checkTypeLayout(snapshot, wall, "JSCToolsTest::TypeAnchors::wallTime"_s))
        ++typesChecked;

    // MediaTime -- rational time (numerator + denominator + flags).
    if (checkTypeLayout(snapshot, media, "JSCToolsTest::TypeAnchors::mediaTime"_s))
        ++typesChecked;

    // String family -- each wraps a refcounted impl.
    if (checkTypeLayout(snapshot, cstr, "JSCToolsTest::TypeAnchors::cString"_s))
        ++typesChecked;
    if (checkTypeLayout(snapshot, str, "JSCToolsTest::TypeAnchors::string"_s))
        ++typesChecked;
    if (checkTypeLayout(snapshot, atomStr, "JSCToolsTest::TypeAnchors::atomString"_s))
        ++typesChecked;

    // Templates whose defaulted parameters are part of the name in the debug info.
    if (checkTypeLayout(snapshot, ref, "JSCToolsTest::TypeAnchors::ref"_s))
        ++typesChecked;
    if (checkTypeLayout(snapshot, refPtr, "JSCToolsTest::TypeAnchors::refPtr"_s))
        ++typesChecked;
    if (checkTypeLayout(snapshot, vec, "JSCToolsTest::TypeAnchors::vector"_s))
        ++typesChecked;

    // Variant -- covers the anonymous-union-flattening path, since a variant's
    // storage is a tagged union.
    if (checkTypeLayout(snapshot, variant, "JSCToolsTest::TypeAnchors::variant"_s))
        ++typesChecked;

    // A polymorphic type is where getTargetObject has something to check. The
    // vptr only matches if the image was placed at the address the corpse has
    // it at, so this covers the load addresses as well.
    auto streamType = snapshot.findTypeOfPointee("JSCToolsTest::TypeAnchors::stringPrintStream"_s);
    TEST_ASSERT(streamType, "corpse describes WTF::StringPrintStream");
    if (streamType) {
        TEST_ASSERT(streamType->isPolymorphic(), "WTF::StringPrintStream is polymorphic");
        TEST_ASSERT_EQ(static_cast<size_t>(streamType->byteSize()), sizeof(WTF::StringPrintStream),
            "TargetType byte size matches sizeof(StringPrintStream)");

        auto streamObject = snapshot.getTargetObject(Address { std::addressof(stream) }, *streamType);
        TEST_ASSERT(streamObject, "the vptr in the corpse matches the vtable symbol");

        // Shows the check does work rather than accepting what it is handed.
        // The refusal reports why, which is the point of it, but this one was
        // asked for and so is not news.
        JSC::Corpse::Error::Quiet quiet;
        auto refused = snapshot.getTargetObject(Address { notAStream }, *streamType);
        TEST_ASSERT(!refused, "a zeroed block is refused as a StringPrintStream");
        ++typesChecked;
    }

    // Reading a derived object through its base. This is what a walk over a
    // heap does constantly: a pointer to a base almost always addresses
    // something more derived, and refusing that would make the walk useless.
    {
        auto baseType = snapshot.findTypeOfPointee("JSCToolsTest::TypeAnchors::walkBase"_s);
        TEST_ASSERT(baseType, "corpse describes the base class");
        if (baseType) {
            TEST_ASSERT(baseType->isPolymorphic(), "the base class is polymorphic");

            // The exact class: no dynamic type to report, since it is the one
            // that was asked for.
            auto exact = snapshot.getTargetObject(Address { std::addressof(walkBase) }, *baseType);
            TEST_ASSERT(exact, "an object of the base class is read as its own class");
            if (exact) {
                TEST_ASSERT(!exact->isDerivedType(),
                    "an object of the exact class reports no derived type");
            }

            // The derived class, read through the base's layout.
            auto asBase = snapshot.getTargetObject(Address { std::addressof(walkDerived) }, *baseType);
            TEST_ASSERT(asBase, "an object of a derived class is read through its base");
            if (asBase) {
                TEST_ASSERT(asBase->isDerivedType(),
                    "reading a derived object through its base reports that it is derived");
                TEST_ASSERT_EQ(asBase->dynamicTypeName(), "JSCToolsTest::WalkDerived"_s,
                    "the object names the class it actually is");

                // The base's own fields still read correctly out of it, which
                // is what makes reading it through the base sound.
                if (auto span = asBase->get("m_baseField"_s)) {
                    TEST_ASSERT_EQ(span->size(), sizeof(uint32_t), "m_baseField is 4 bytes");
                    if (span->size() == sizeof(uint32_t)) {
                        uint32_t value = 0;
                        memcpy(&value, span->data(), sizeof(value));
                        TEST_ASSERT_HEX_EQ(value, walkDerived.m_baseField,
                            "the base field of a derived object reads back");
                    }
                }
            }

            // A templated class, read as itself and read through its base.
            // Naming one of these is exactly what a mangler built by hand could
            // not do, and what taking the name from the demangler settles.
            struct TemplateCase {
                ASCIILiteral anchor;
                ASCIILiteral name;
                const void* object;
            };
            TemplateCase templateCases[] = {
                { "JSCToolsTest::TypeAnchors::walkTemplate"_s,
                  "JSCToolsTest::WalkTemplate<int>"_s, std::addressof(walkTemplate) },
                { "JSCToolsTest::TypeAnchors::walkFixedArray"_s,
                  "JSCToolsTest::WalkFixedArray<unsigned long, 4>"_s,
                  std::addressof(walkFixedArray) },
            };
            for (const auto& testCase : templateCases) {
                auto templateType = snapshot.findTypeOfPointee(testCase.anchor);
                TEST_ASSERT(templateType, "corpse describes a templated polymorphic class");
                if (!templateType)
                    continue;
                TEST_ASSERT(templateType->isPolymorphic(), "the templated class is polymorphic");

                auto exactTemplate = snapshot.getTargetObject(Address { testCase.object },
                    *templateType);
                TEST_ASSERT(exactTemplate, "an object of the templated class is read as itself");

                // ...and through its base, which is what names it.
                auto throughBase = snapshot.getTargetObject(Address { testCase.object }, *baseType);
                TEST_ASSERT(throughBase, "a templated object is read through its base");
                if (throughBase) {
                    TEST_ASSERT_EQ(throughBase->dynamicTypeName(), testCase.name,
                        "the templated object names the class it actually is");
                }
                ++typesChecked;
            }

            // What the object itself says, read straight out of the corpse.
            // Against an RTTI build this is where identification comes from,
            // and nothing here consults an image file or the debug info.
#if defined(__cpp_rtti) || defined(__GXX_RTTI)
            {
                JSC::Corpse::TypeInfoReader reader(snapshot);

                uint64_t vptr = 0;
                bool readVPtr = snapshot.readInto(
                    Address { std::addressof(walkDerived) }, vptr);
                TEST_ASSERT(readVPtr, "the object's vptr reads out of the corpse");

                if (readVPtr) {
                    Address stripped = Address { vptr }.stripped();
                    TEST_ASSERT_EQ(reader.mangledNameForVPtr(stripped),
                        "N12JSCToolsTest11WalkDerivedE"_str,
                        "an object's type_info names the class it is");

                    // The bases come from the type_info too, so a hierarchy is
                    // answerable without debug info.
                    auto hierarchy = reader.mangledHierarchyForVPtr(stripped);
                    TEST_ASSERT(hierarchy.contains("N12JSCToolsTest11WalkDerivedE"_str),
                        "the hierarchy opens with the class itself");
                    TEST_ASSERT(hierarchy.contains("N12JSCToolsTest8WalkBaseE"_str),
                        "the hierarchy reaches the base class");

                    // A value that is not a vptr is reported as unidentified
                    // rather than guessed at.
                    TEST_ASSERT(reader.mangledNameForVPtr(Address { 0x1234 }).isEmpty(),
                        "an address that is not a vtable identifies nothing");
                    TEST_ASSERT(reader.mangledNameForVPtr(Address { }).isEmpty(),
                        "a null vptr identifies nothing");
                }
            }
#endif

            // An unrelated polymorphic object is still refused: accepting a
            // derived class must not mean accepting anything with a vptr.
            if (streamType) {
                JSC::Corpse::Error::Quiet quiet;
                auto unrelated = snapshot.getTargetObject(Address { std::addressof(walkDerived) },
                    *streamType);
                TEST_ASSERT(!unrelated,
                    "an object of an unrelated class is refused");
            }
            ++typesChecked;
        }
    }

    // One step of a walk over the heap. Neither WTF::StringImpl nor WTF::Ref's
    // instantiation is spelled here; both come out of the debug info.
    auto refType = snapshot.findTypeOfPointee("JSCToolsTest::TypeAnchors::ref"_s);
    TEST_ASSERT(refType, "corpse describes the Ref instantiation");
    if (refType) {
        auto refObject = snapshot.getTargetObject(Address { std::addressof(ref) }, *refType);
        TEST_ASSERT(refObject, "TargetObject bound to the Ref");

        const auto* pointerField = refType->field("m_ptr"_s);
        TEST_ASSERT(pointerField, "WTF::Ref exposes m_ptr");

        if (refObject && pointerField) {
            TEST_ASSERT(pointerField->isPointer, "Ref::m_ptr is reported as a pointer");

            auto implObject = snapshot.follow(*refObject, *pointerField);
            TEST_ASSERT(implObject, "following Ref::m_ptr binds an object");
            if (implObject) {
                TEST_ASSERT(implObject->base() == Address { ref.ptr() },
                    "the followed object is at the Ref's target");
                TEST_ASSERT_EQ(static_cast<size_t>(implObject->size()), sizeof(WTF::StringImpl),
                    "the followed object is the size of a StringImpl");

                const auto* lengthField = implObject->type().field("m_length"_s);
                TEST_ASSERT(lengthField, "the followed object exposes m_length");
                if (lengthField) {
                    auto lengthBytes = implObject->get(*lengthField);
                    if (lengthBytes.size() == sizeof(uint32_t)) {
                        uint32_t length = 0;
                        memcpy(&length, lengthBytes.data(), sizeof(uint32_t));
                        TEST_ASSERT_EQ(length, ref->length(),
                            "the followed object's m_length is the string's length");
                    }
                    TEST_ASSERT(!snapshot.follow(*implObject, *lengthField),
                        "a member that is not a pointer is not followed");
                }
            }
        }
        ++typesChecked;
    }

    // JSC::JSObject cannot be constructed without a VM, so there is no instance
    // to bind and no anchor for one. Its name needs no guessing, which leaves it
    // as the case that covers looking a type up by name in a JSC header.
    auto jsObjectType = snapshot.findType("JSC::JSObject"_s);
    TEST_ASSERT(jsObjectType, "corpse describes JSC::JSObject");
    if (jsObjectType) {
        TEST_ASSERT(jsObjectType->byteSize() > 0, "JSC::JSObject byteSize is nonzero");
        TEST_ASSERT(!jsObjectType->fields().isEmpty(), "JSC::JSObject has fields in DWARF");
        ++typesChecked;
    }

    TEST_ASSERT(typesChecked > 0, "at least one common type layout was cross-checked");
}

} // namespace JSCToolsTest

#endif // HAVE(CORPSE_SUPPORT)
