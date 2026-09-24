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
#include "LibJSCToolsTestUtilities.h"
#include "TargetValueTest.h"

#include <JavaScriptCore/CorpsePlatform.h>

#if ENABLE(MYA_HEAP)

#include <JavaScriptCore/CorpseAddress.h>
#include <JavaScriptCore/CorpseImage.h>
#include <JavaScriptCore/CorpseSnapshot.h>
#include <JavaScriptCore/CorpseSnapshotDebugInfo.h>
#include <JavaScriptCore/CorpseTargetType.h>
#include <JavaScriptCore/CorpseTargetValue.h>
#include <JavaScriptCore/SourceProvider.h>
#include <optional>
#include <stdint.h>
#include <string_view>
#include <typeinfo>
#include <wtf/Ref.h>
#include <wtf/RefPtr.h>
#include <wtf/StdLibExtras.h>
#include <wtf/text/CString.h>
#include <wtf/text/TextPosition.h>

namespace JSCToolsTest {

namespace {

using JSC::Corpse::Address;
using JSC::Corpse::Image;
using JSC::Corpse::Snapshot;
using JSC::Corpse::SnapshotDebugInfo;
using JSC::Corpse::TargetType;
using JSC::Corpse::TargetValue;

// The object graph a corpse of this process is read back through.
struct Point {
    int32_t x { 3 };
    int32_t y { 4 };
    friend bool operator==(const Point&, const Point&) = default;
};

class Shape {
public:
    virtual ~Shape() = default;
    Point origin;
    uint8_t flags { 5 };
    Shape* neighbor { nullptr };
};

class Circle : public Shape {
public:
    uint32_t radius { 6 };
};

class Square : public Shape {
public:
    double side { 7.5 };
    int8_t signedByte { -9 };
};

class Tagged {
public:
    virtual ~Tagged() = default;
    uint64_t tag { 0x7a67 };
};

// Held through Circle, its second base, so that its offset to top is not zero.
class TaggedCircle : public Tagged, public Circle {
public:
    uint32_t extra { 8 };
};

struct VBase {
    virtual ~VBase() = default;
    uint64_t shared { 0x5555 };
};

struct Left : virtual VBase {
    uint32_t left { 1 };
};

struct Right : virtual VBase {
    uint32_t right { 2 };
};

struct Diamond : Left, Right {
    uint32_t bottom { 3 };
    int32_t numbers[4] { 10, 20, 30, 40 };
};

// Every integer in a Diamond, its one VBase counted once.
constexpr int64_t diamondIntegerSum = 1 + 2 + 3 + 10 + 20 + 30 + 40 + 0x5555;

// Our class holding a JSC object, to read JSC's debug info from mya's side.
struct TestContainer {
    Ref<JSC::SourceProvider> provider;
    uint32_t marker { 0xc0ffee };
};

TextPosition targetStartPosition()
{
    return { OrdinalNumber::fromZeroBasedInt(1234), OrdinalNumber::fromZeroBasedInt(5678) };
}

struct TargetValueFixture {
    TargetValueFixture()
        : container { JSC::StringSourceProvider::create("1 + 1"_s, JSC::SourceOrigin { }, "target-value.js"_s,
            JSC::SourceTaintedOrigin::Untainted, targetStartPosition()) }
    {
        circle.neighbor = &square;
    }

    Circle circle;
    Square square;
    TaggedCircle taggedCircle;
    Diamond diamond;
    Shape* shapeThatIsACircle { &circle };
    Circle* circleThatIsTagged { &taggedCircle };
    Right* rightOfDiamond { &diamond };
    Shape* nullShape { nullptr };
    Shape* unmappedShape { reinterpret_cast<Shape*>(static_cast<uintptr_t>(0x10)) };
    TestContainer container;
};

bool typeNameIs(const TargetType& type, std::string_view expected)
{
    return std::string_view { type.name().span() } == expected;
}

void expectInteger(std::optional<int64_t> actual, int64_t expected, const char* message)
{
    TEST_ASSERT(actual, message);
    if (actual)
        TEST_ASSERT_EQ(*actual, expected, message);
}

// The class layout of a value's type, or nullopt after failing the test.
std::optional<TargetType::Class> classLayout(const TargetValue& value, const char* message)
{
    TargetType::Layout layout = value.type().layout();
    auto* klass = std::get_if<TargetType::Class>(&layout);
    TEST_ASSERT(value && klass, message);
    if (!value || !klass)
        return std::nullopt;
    return WTF::move(*klass);
}

// The sum of every integer in `value`, by its layout, without following
// pointers. A virtual base belongs to the complete object, so it is counted
// only from there.
int64_t integerSumOfSubobject(const TargetValue& value)
{
    return WTF::switchOn(value.type().layout(),
        [&](const TargetType::Class& klass) {
            int64_t sum = 0;
            for (const TargetType::Field& field : klass.properFields)
                sum += integerSumOfSubobject(value.field(field));
            for (const TargetType::Base& base : klass.bases)
                sum += integerSumOfSubobject(value.base(base));
            return sum;
        },
        [&](const TargetType::Array& array) {
            int64_t sum = 0;
            for (size_t index = 0; index < array.count; ++index)
                sum += integerSumOfSubobject(value.element(index));
            return sum;
        },
        [&](const TargetType::Integer&) {
            return value.integer().value_or(0);
        },
        [](const TargetType::Pointer&) {
            return int64_t { 0 };
        },
        [](const TargetType::Other&) {
            return int64_t { 0 };
        });
}

int64_t integerSum(const TargetValue& value)
{
    int64_t sum = integerSumOfSubobject(value);
    for (const TargetValue& virtualBase : value.virtualBases())
        sum += integerSumOfSubobject(virtualBase);
    return sum;
}

void checkPlainFields(const TargetValue& fixture)
{
    TargetValue circle = fixture.properField("circle");
    expectInteger(circle.properField("radius").integer(), 6, "a proper field reads back");
    {
        ExpectedErrors expectedErrors;
        TEST_ASSERT(!circle.properField("flags"), "a field of a base is not a proper field of the derived class");
    }

    auto circleLayout = classLayout(circle, "the circle is a class");
    if (!circleLayout)
        return;
    TEST_ASSERT(circleLayout->isPolymorphic && circleLayout->bases.size() == 1 && circleLayout->virtualBases.isEmpty(), "Circle has a vptr, one base and no virtual base");
    if (circleLayout->bases.size() != 1)
        return;
    TargetValue shape = circle.base(circleLayout->bases[0]);
    TEST_ASSERT(shape && typeNameIs(shape.type(), "JSCToolsTest::(anonymous namespace)::Shape") && shape.address() == circle.address(), "the primary base starts where the object does");
    expectInteger(shape.properField("flags").integer(), 5, "a field of a base reads through the base subobject");

    TargetValue origin = shape.properField("origin");
    TEST_ASSERT(origin && typeNameIs(origin.type(), "JSCToolsTest::(anonymous namespace)::Point"), "a field carries its declared type");
    expectInteger(origin.properField("x").integer(), 3, "a field of a field reads back");
    expectInteger(origin.properField("y").integer(), 4, "a field of a field reads back");
    auto point = origin.as<Point>();
    TEST_ASSERT(point && *point == Point { }, "a struct reads back whole");
}

void checkDynamicTypes(const TargetValue& fixture)
{
    TargetValue circle = fixture.properField("circle");
    TargetValue pointee = fixture.properField("shapeThatIsACircle").dereference();
    TEST_ASSERT(pointee && typeNameIs(pointee.type(), "JSCToolsTest::(anonymous namespace)::Shape"), "a dereferenced pointer has the pointee's static type");
    TEST_ASSERT(pointee.address() == circle.address(), "the pointer points at the circle");
    auto info = pointee.typeInfo();
    TEST_ASSERT(info && !info->offsetToTop, "a complete object has no offset to top");
    TEST_ASSERT(info && std::string_view { info->mangledName.span() } == typeid(Circle).name(), "the type_info name is what typeid gives");
    RefPtr<TargetType> dynamicType = pointee.dynamicType();
    TEST_ASSERT(dynamicType && typeNameIs(*dynamicType, "JSCToolsTest::(anonymous namespace)::Circle"), "RTTI names the class the object really is");
    TargetValue realCircle = pointee.downcast();
    TEST_ASSERT(realCircle && typeNameIs(realCircle.type(), "JSCToolsTest::(anonymous namespace)::Circle"), "downcast() gives the dynamic type");
    expectInteger(realCircle.properField("radius").integer(), 6, "a field of the dynamic type reads through the downcast");

    auto circleLayout = classLayout(realCircle, "the downcast circle is a class");
    if (!circleLayout || circleLayout->bases.size() != 1)
        return;
    TargetValue square = realCircle.base(circleLayout->bases[0]).properField("neighbor").dereference().downcast();
    TEST_ASSERT(square && typeNameIs(square.type(), "JSCToolsTest::(anonymous namespace)::Square"), "a pointer chain crosses objects");
    auto side = square.properField("side").as<double>();
    TEST_ASSERT(side && *side == 7.5, "a double reads back as its own type");
    expectInteger(square.properField("signedByte").integer(), -9, "a signed byte sign-extends");

    // A base subobject that does not start its complete object.
    TargetValue taggedCircle = fixture.properField("circleThatIsTagged").dereference();
    TEST_ASSERT(taggedCircle, "a pointer to a second base dereferences");
    info = taggedCircle.typeInfo();
    TEST_ASSERT(info && info->offsetToTop == -static_cast<int64_t>(sizeof(Tagged)), "offset to top leads from the second base to the complete object");
    TargetValue complete = taggedCircle.downcast();
    TEST_ASSERT(complete && typeNameIs(complete.type(), "JSCToolsTest::(anonymous namespace)::TaggedCircle"), "downcast() from a second base finds the complete object's type");
    TEST_ASSERT(complete.address() == taggedCircle.address() - sizeof(Tagged), "the complete object starts before its second base");
    expectInteger(complete.properField("extra").integer(), 8, "the complete object's own field reads");
    auto completeLayout = classLayout(complete, "the complete object is a class");
    if (!completeLayout)
        return;
    TEST_ASSERT_EQ(completeLayout->bases.size(), 2u, "TaggedCircle has two direct bases");
    if (completeLayout->bases.size() != 2)
        return;
    TargetValue tagged = complete.base(completeLayout->bases[0]);
    TargetValue secondBase = complete.base(completeLayout->bases[1]);
    TEST_ASSERT(typeNameIs(tagged.type(), "JSCToolsTest::(anonymous namespace)::Tagged") && tagged.address() == complete.address(), "the first base starts the complete object");
    TEST_ASSERT(typeNameIs(secondBase.type(), "JSCToolsTest::(anonymous namespace)::Circle") && secondBase.address() == taggedCircle.address(), "the second base is where the pointer pointed");
    expectInteger(tagged.properField("tag").integer(), 0x7a67, "a field of the first base reads");
}

void checkVirtualBases(const TargetValue& fixture)
{
    TargetValue right = fixture.properField("rightOfDiamond").dereference();
    TEST_ASSERT(right && typeNameIs(right.type(), "JSCToolsTest::(anonymous namespace)::Right"), "a pointer to a base with a virtual base dereferences");
    expectInteger(right.properField("right").integer(), 2, "the subobject's own field reads");

    Vector<TargetValue> virtualBases = right.virtualBases();
    TEST_ASSERT_EQ(virtualBases.size(), 1u, "the complete object has one virtual base");
    if (virtualBases.size() == 1) {
        TEST_ASSERT(typeNameIs(virtualBases[0].type(), "JSCToolsTest::(anonymous namespace)::VBase"), "the virtual base is VBase");
        expectInteger(virtualBases[0].properField("shared").integer(), 0x5555, "the virtual base's field reads");
    }

    TargetValue diamond = right.downcast();
    TEST_ASSERT(diamond && typeNameIs(diamond.type(), "JSCToolsTest::(anonymous namespace)::Diamond"), "downcast() from the second base of a diamond");
    expectInteger(diamond.properField("bottom").integer(), 3, "the diamond's own field reads");
    TEST_ASSERT_EQ(integerSum(diamond), diamondIntegerSum, "visiting the layout sees every integer, the shared virtual base once");

    auto diamondLayout = classLayout(diamond, "the diamond is a class");
    if (diamondLayout) {
        TEST_ASSERT_EQ(diamondLayout->bases.size(), 2u, "Diamond has two direct bases");
        TEST_ASSERT_EQ(diamondLayout->virtualBases.size(), 1u, "Diamond lists its virtual base once");
        for (const TargetType::Base& base : diamondLayout->bases)
            TEST_ASSERT_EQ(diamond.base(base).virtualBases().size(), 1u, "a base subobject reaches the virtual base through the complete object");
    }

    TargetValue numbers = diamond.properField("numbers");
    TargetType::Layout numbersLayout = numbers.type().layout();
    auto* array = std::get_if<TargetType::Array>(&numbersLayout);
    TEST_ASSERT(numbers && array && array->count == 4, "an array field knows its length");
    expectInteger(numbers.element(2).integer(), 30, "an element reads at its index");
    {
        ExpectedErrors expectedErrors;
        TEST_ASSERT(!numbers.element(4), "an index past the end gives nothing");
    }
}

void checkFailures(const TargetValue& fixture)
{
    TargetValue nullShape = fixture.properField("nullShape");
    TEST_ASSERT(nullShape && !nullShape.dereference(), "a null pointer dereferences to nothing, silently");

    TargetValue unmapped = fixture.properField("unmappedShape").dereference();
    TEST_ASSERT(unmapped, "a value in unreadable memory exists until it is read");
    {
        ExpectedErrors expectedErrors;
        TEST_ASSERT(!unmapped.properField("flags").integer(), "reading unreadable memory is reported and gives nothing");
    }

    TargetValue circle = fixture.properField("circle");
    TargetValue radius = circle.properField("radius");
    auto circleLayout = classLayout(circle, "the circle is a class");
    if (!circleLayout || circleLayout->bases.size() != 1)
        return;
    TargetValue origin = circle.base(circleLayout->bases[0]).properField("origin");
    TEST_ASSERT(radius && origin, "the values the misuse below starts from are valid");

    // Each misuse is reported once, and nothing after an invalid value reports again.
    {
        ExpectedErrors expectedErrors(11);
        TEST_ASSERT(!circle.properField("nonexistent"), "a field the class lacks");
        TEST_ASSERT(!radius.properField("x"), "a field of a non-class");
        TEST_ASSERT(!radius.dereference(), "dereferencing a non-pointer");
        TEST_ASSERT(!radius.as<uint8_t>(), "reading with the wrong size");
        TEST_ASSERT(!radius.element(0), "indexing a non-array");
        TEST_ASSERT(radius.virtualBases().isEmpty(), "the virtual bases of a non-class");
        TEST_ASSERT(!origin.integer(), "a struct as an integer");
        TEST_ASSERT(!origin.typeInfo(), "the type_info of a non-polymorphic type");
        TEST_ASSERT(!origin.dynamicType(), "the dynamic type of a non-polymorphic type");
        TEST_ASSERT(!origin.downcast(), "the downcast of a non-polymorphic type");
        TEST_ASSERT(!circle.properField("nonexistent").properField("x").dereference().integer(), "a chain reports its first failure only");
    }
    TEST_ASSERT(origin.virtualBases().isEmpty(), "a plain struct has no virtual bases, and that is not a failure");
}

void checkContainer(const TargetValue& fixture)
{
    TargetValue container = fixture.properField("container");
    expectInteger(container.properField("marker").integer(), 0xc0ffee, "the container's own field reads");
    TargetValue pointer = container.properField("provider").properField("m_ptr");
    TEST_ASSERT(pointer && std::holds_alternative<TargetType::Pointer>(pointer.type().layout()), "Ref's storage is seen through its typedef as a pointer");
    TargetValue sourceProvider = pointer.dereference();
    TEST_ASSERT(sourceProvider && typeNameIs(sourceProvider.type(), "JSC::SourceProvider"), "the Ref points at a JSC::SourceProvider");
    TargetValue realProvider = sourceProvider.downcast();
    TEST_ASSERT(realProvider && typeNameIs(realProvider.type(), "JSC::StringSourceProvider"), "JavaScriptCore's RTTI and debug info name the class the object really is");
    if (!realProvider)
        return;
    TEST_ASSERT_EQ(realProvider.type().byteSize(), sizeof(JSC::StringSourceProvider), "JavaScriptCore's debug info gives the class its size");

    auto providerLayout = classLayout(realProvider, "StringSourceProvider is a class");
    if (!providerLayout || providerLayout->bases.size() != 1)
        return;
    TargetValue startPosition = realProvider.base(providerLayout->bases[0]).properField("m_startPosition");
    TEST_ASSERT(startPosition && typeNameIs(startPosition.type(), "WTF::TextPosition"), "a field of the JSC base class is found through its subobject");
    auto position = startPosition.as<TextPosition>();
    TEST_ASSERT(position && *position == targetStartPosition(), "the corpse holds the target's start position");
    expectInteger(startPosition.properField("m_line").properField("m_zeroBasedValue").integer(), 1234, "a WTF field two structs down reads");
}

void analyze(Snapshot& snapshot, Address fixtureAddress)
{
    RefPtr<SnapshotDebugInfo> debugInfo = SnapshotDebugInfo::create(snapshot);
    TEST_ASSERT(debugInfo, "liblldb opens every image of the snapshot");
    if (!debugInfo)
        return;

    // The loader lists the executable first.
    const Image& executable = snapshot.images()[0];
    RefPtr<TargetType> fixtureType = debugInfo->findType("JSCToolsTest::(anonymous namespace)::TargetValueFixture", executable);
    TEST_ASSERT(fixtureType, "the fixture's type is in the executable's debug info");
    if (!fixtureType)
        return;
    TEST_ASSERT_EQ(fixtureType->byteSize(), sizeof(TargetValueFixture), "the debug info gives the fixture its size");
    {
        ExpectedErrors expectedErrors;
        TEST_ASSERT(!debugInfo->findType("JSCToolsTest::(anonymous namespace)::NoSuchType", executable), "a type the image does not define is missing");
    }
    TEST_ASSERT(debugInfo->findType("WTF::TextPosition", executable), "a header-only type is complete in the image that uses it");

    TargetValue fixture = TargetValue::at(snapshot, fixtureAddress, Ref { *fixtureType });
    auto fixtureLayout = classLayout(fixture, "the fixture is a class");
    TEST_ASSERT(fixtureLayout && !fixtureLayout->isPolymorphic && fixtureLayout->bases.isEmpty(), "the fixture is a plain struct");

    checkPlainFields(fixture);
    checkDynamicTypes(fixture);
    checkVirtualBases(fixture);
    checkFailures(fixture);
    checkContainer(fixture);
}

} // anonymous namespace

void testTargetValue()
{
    SuiteTracer tracer("TargetValue");
    if (!tracer.shouldRun())
        return;

    TargetValueFixture fixture;
    analyzeInAndOutOfProcess(Address { &fixture }, "--target-value-target", analyze);
}

void runTargetValueTarget()
{
    TargetValueFixture fixture;
    parkAsCorpseTarget(Address { &fixture });
}

#endif // ENABLE(MYA_HEAP)

} // namespace JSCToolsTest
