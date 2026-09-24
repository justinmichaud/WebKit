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
#include <JavaScriptCore/CorpseProcess.h>
#include <JavaScriptCore/CorpseSnapshot.h>
#include <JavaScriptCore/CorpseTargetDebugInfo.h>
#include <JavaScriptCore/CorpseTargetType.h>
#include <JavaScriptCore/CorpseTargetValue.h>
#include <JavaScriptCore/SourceProvider.h>
#include <array>
#include <errno.h>
#include <optional>
#include <signal.h>
#include <spawn.h>
#include <stdint.h>
#include <string_view>
#include <sys/wait.h>
#include <typeinfo>
#include <unistd.h>
#include <wtf/Ref.h>
#include <wtf/RefPtr.h>
#include <wtf/SafeStrerror.h>
#include <wtf/Scope.h>
#include <wtf/StdLibExtras.h>
#include <wtf/text/CString.h>
#include <wtf/text/TextPosition.h>

extern char** environ;

#endif // ENABLE(MYA_HEAP)

namespace JSCToolsTest {

#if ENABLE(MYA_HEAP)

namespace {

using JSC::Corpse::Address;
using JSC::Corpse::EagerTargetValue;
using JSC::Corpse::LazyTargetValue;
using JSC::Corpse::Materialization;
using JSC::Corpse::Process;
using JSC::Corpse::Snapshot;
using JSC::Corpse::TargetDebugInfo;
using JSC::Corpse::TargetType;
using JSC::Corpse::TargetValue;

// We walk this hierarchy to test TargetValue
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

// Start from mya and confirm we can read JSC debug info.
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

// CLAUDE: these should be baked in to value
template<typename Value>
std::optional<int64_t> integerAt(const std::optional<Value>& value, const char* fieldName)
{
    if (!value)
        return std::nullopt;
    auto field = value->field(fieldName);
    if (!field)
        return std::nullopt;
    return field->integer();
}

template<typename T, typename Value>
std::optional<T> readAs(const std::optional<Value>& value)
{
    if (!value)
        return std::nullopt;
    return value->template as<T>();
}

void expectInteger(std::optional<int64_t> actual, int64_t expected, const char* message)
{
    TEST_ASSERT(actual, message);
    if (actual)
        TEST_ASSERT_EQ(*actual, expected, message);
}

// CLAUDE: we shouldn't support reading through base classes; value only does one step of dereferencing, we should iterate through each base class (including virtual ones) to see every field in the object.
// Avoid nesting: ASSERT, if(!) return always.
template<Materialization materialization>
void checkFixture(const Snapshot& snapshot, Address address, TargetType& fixtureType)
{
    using Value = TargetValue<materialization>;

    auto fixture = Value::at(snapshot, address, Ref<TargetType> { fixtureType });
    TEST_ASSERT(fixture, "the fixture is readable");
    if (!fixture)
        return;

    // Fields of fields, with no polymorphism involved.
    auto circle = fixture->field("circle");
    TEST_ASSERT(circle, "the fixture has a circle");
    if (!circle)
        return;
    expectInteger(integerAt(circle, "radius"), 6, "the circle's own field reads back");
    expectInteger(integerAt(circle, "flags"), 5, "a field declared in a base reads through the derived class");
    auto origin = circle->field("origin");
    TEST_ASSERT(origin, "a struct field declared in a base is found through the derived class");
    if (!origin)
        return;
    TEST_ASSERT(typeNameIs(origin->type(), "(anonymous namespace)::Point"), "a field carries its declared type");
    expectInteger(integerAt(origin, "x"), 3, "a field of a field reads back");
    expectInteger(integerAt(origin, "y"), 4, "a field of a field reads back");
    auto point = readAs<Point>(origin);
    TEST_ASSERT(point && *point == Point { }, "a struct reads back whole");

    // A pointer, the dynamic type behind it, and the downcast to it.
    auto shape = fixture->field("shapeThatIsACircle");
    auto pointee = shape ? shape->dereference() : std::nullopt;
    TEST_ASSERT(pointee, "a pointer field dereferences");
    if (pointee) {
        TEST_ASSERT(typeNameIs(pointee->type(), "(anonymous namespace)::Shape"), "a dereferenced pointer has the pointee's static type");
        TEST_ASSERT(pointee->address() == circle->address(), "the pointer points at the circle");
        auto info = pointee->typeInfo();
        TEST_ASSERT(info && !info->offsetToTop, "a complete object has no offset to top");
        TEST_ASSERT(info && std::string_view { info->mangledName.span() } == typeid(Circle).name(), "the type_info name is what typeid gives");
        RefPtr<TargetType> dynamicType = pointee->dynamicType();
        TEST_ASSERT(dynamicType && typeNameIs(*dynamicType, "(anonymous namespace)::Circle"), "RTTI names the class the object really is");
        auto realCircle = pointee->downcast();
        TEST_ASSERT(realCircle && typeNameIs(realCircle->type(), "(anonymous namespace)::Circle"), "downcast() gives the dynamic type");
        expectInteger(integerAt(realCircle, "radius"), 6, "a field of the dynamic type reads through the downcast");

        auto neighbor = realCircle ? realCircle->field("neighbor") : std::nullopt;
        auto square = neighbor ? neighbor->dereference() : std::nullopt;
        auto realSquare = square ? square->downcast() : std::nullopt;
        TEST_ASSERT(realSquare && typeNameIs(realSquare->type(), "(anonymous namespace)::Square"), "a pointer chain crosses objects");
        if (realSquare) {
            auto side = realSquare->field("side");
            TEST_ASSERT(side && side->floatingPoint() == 7.5, "a double reads back");
            expectInteger(integerAt(realSquare, "signedByte"), -9, "a signed byte sign-extends");
        }
    }

    // A base subobject that does not start its complete object.
    auto tagged = fixture->field("circleThatIsTagged");
    auto taggedCircle = tagged ? tagged->dereference() : std::nullopt;
    TEST_ASSERT(taggedCircle, "a pointer to a second base dereferences");
    if (taggedCircle) {
        auto info = taggedCircle->typeInfo();
        TEST_ASSERT(info && info->offsetToTop == -static_cast<int64_t>(sizeof(Tagged)), "offset to top leads from the second base to the complete object");
        auto complete = taggedCircle->downcast();
        TEST_ASSERT(complete && typeNameIs(complete->type(), "(anonymous namespace)::TaggedCircle"), "downcast() from a second base finds the complete object's type");
        if (complete) {
            TEST_ASSERT(complete->address() == taggedCircle->address() - sizeof(Tagged), "the complete object starts before its second base");
            expectInteger(integerAt(complete, "tag"), 0x7a67, "a field of the first base reads");
            expectInteger(integerAt(complete, "extra"), 8, "the complete object's own field reads");
            expectInteger(integerAt(complete, "radius"), 6, "a field of the second base reads through the complete object");
            auto shapeBase = complete->base("(anonymous namespace)::Shape");
            TEST_ASSERT(shapeBase && shapeBase->address() == taggedCircle->address(), "an indirect base is found at its offset");
            expectInteger(integerAt(shapeBase, "flags"), 5, "a field of an indirect base reads");
        }
    }

    // Virtual bases, reached from a subobject that is not the complete object.
    auto right = fixture->field("rightOfDiamond");
    auto rightSubobject = right ? right->dereference() : std::nullopt;
    TEST_ASSERT(rightSubobject, "a pointer to a base with a virtual base dereferences");
    if (rightSubobject) {
        expectInteger(integerAt(rightSubobject, "right"), 2, "the subobject's own field reads");
        expectInteger(integerAt(rightSubobject, "shared"), 0x5555, "a field of a virtual base is found through the complete object");
        auto virtualBase = rightSubobject->virtualBase("(anonymous namespace)::VBase");
        TEST_ASSERT(virtualBase, "a virtual base is found by name");
        expectInteger(integerAt(virtualBase, "shared"), 0x5555, "the virtual base's field reads");
        auto diamond = rightSubobject->downcast();
        TEST_ASSERT(diamond && typeNameIs(diamond->type(), "(anonymous namespace)::Diamond"), "downcast() from the second base of a diamond");
        if (diamond) {
            expectInteger(integerAt(diamond, "left"), 1, "a field of the first base of a diamond reads");
            expectInteger(integerAt(diamond, "bottom"), 3, "the diamond's own field reads");
            auto numbers = diamond->field("numbers");
            TEST_ASSERT(numbers && numbers->type().kind() == TargetType::Kind::Array && numbers->type().elementCount() == 4, "an array field knows its length");
            auto third = numbers ? numbers->element(2) : std::nullopt;
            expectInteger(third ? third->integer() : std::nullopt, 30, "an element reads at its index");
            {
                ExpectedErrors expectedErrors;
                TEST_ASSERT(numbers && !numbers->element(4), "an index past the end gives nothing");
            }
        }
    }

    // Null and unreadable pointers are states of the heap, not errors.
    auto nullShape = fixture->field("nullShape");
    TEST_ASSERT(nullShape && !nullShape->dereference(), "a null pointer dereferences to nothing");
    auto unmapped = fixture->field("unmappedShape");
    TEST_ASSERT(unmapped, "the fixture has an unmapped pointer");
    if (unmapped) {
        auto unmappedValue = unmapped->dereference();
        if constexpr (materialization == Materialization::Eager)
            TEST_ASSERT(!unmappedValue, "an eager value of unreadable memory is nothing");
        else {
            TEST_ASSERT(unmappedValue, "a lazy value of unreadable memory exists until it is read");
            TEST_ASSERT(!integerAt(unmappedValue, "flags"), "reading unreadable memory gives nothing");
        }
    }

    // Misuse is reported, and gives nothing.
    auto radius = circle->field("radius");
    if (radius) {
        ExpectedErrors expectedErrors(10);
        TEST_ASSERT(!circle->field("nonexistent"), "a member the layout lacks");
        TEST_ASSERT(!circle->base("(anonymous namespace)::Tagged"), "a base the class lacks");
        TEST_ASSERT(!radius->dereference(), "dereferencing a non-pointer");
        TEST_ASSERT(!readAs<uint8_t>(radius), "reading with the wrong size");
        TEST_ASSERT(!radius->element(0), "indexing a non-array");
        TEST_ASSERT(!radius->floatingPoint(), "an integer as a float");
        TEST_ASSERT(!origin->integer(), "a struct as an integer");
        TEST_ASSERT(!origin->typeInfo(), "the type_info of a non-polymorphic type");
        TEST_ASSERT(!origin->dynamicType(), "the dynamic type of a non-polymorphic type");
        TEST_ASSERT(!origin->virtualBase("(anonymous namespace)::VBase"), "a virtual base of a non-polymorphic type");
    }

    // The container: our class holding a JSC object through its base class.
    auto container = fixture->field("container");
    expectInteger(integerAt(container, "marker"), 0xc0ffee, "the container's own field reads");
    auto provider = container ? container->field("provider") : std::nullopt;
    auto pointer = provider ? provider->field("m_ptr") : std::nullopt;
    TEST_ASSERT(pointer && pointer->type().kind() == TargetType::Kind::Pointer, "Ref's storage is seen through its typedef as a pointer");
    auto sourceProvider = pointer ? pointer->dereference() : std::nullopt;
    TEST_ASSERT(sourceProvider && typeNameIs(sourceProvider->type(), "JSC::SourceProvider"), "the Ref points at a JSC::SourceProvider");
    if (!sourceProvider)
        return;
    auto realProvider = sourceProvider->downcast();
    TEST_ASSERT(realProvider && typeNameIs(realProvider->type(), "JSC::StringSourceProvider"), "JavaScriptCore's RTTI and debug info name the class the object really is");
    if (!realProvider)
        return;
    TEST_ASSERT_EQ(realProvider->type().byteSize(), sizeof(JSC::StringSourceProvider), "JavaScriptCore's debug info gives the class its size");
    auto startPosition = realProvider->field("m_startPosition");
    TEST_ASSERT(startPosition && typeNameIs(startPosition->type(), "WTF::TextPosition"), "a field of the JSC base class is found through the JSC derived class");
    auto position = readAs<TextPosition>(startPosition);
    TEST_ASSERT(position && *position == targetStartPosition(), "the corpse holds the target's start position");
    auto line = startPosition ? startPosition->field("m_line") : std::nullopt;
    expectInteger(integerAt(line, "m_zeroBasedValue"), 1234, "a WTF field two structs down reads");
}

void analyze(const Snapshot& snapshot, Address fixtureAddress)
{
    RefPtr<TargetDebugInfo> debugInfo = TargetDebugInfo::create(snapshot);
    TEST_ASSERT(debugInfo, "liblldb opens every image of the snapshot");
    if (!debugInfo)
        return;
    TEST_ASSERT_EQ(static_cast<size_t>(debugInfo->moduleCount()), snapshot.images().size(), "liblldb has one module per image");

    RefPtr<TargetType> fixtureType = debugInfo->findType("(anonymous namespace)::TargetValueFixture");
    TEST_ASSERT(fixtureType, "the fixture's type is in the debug info");
    if (!fixtureType)
        return;
    TEST_ASSERT_EQ(fixtureType->byteSize(), sizeof(TargetValueFixture), "the debug info gives the fixture its size");
    TEST_ASSERT(fixtureType->kind() == TargetType::Kind::Class && !fixtureType->isPolymorphic(), "the fixture is a plain struct");

    {
        // Every image that uses a header-only type carries a complete copy of it.
        ExpectedErrors expectedErrors(2);
        TEST_ASSERT(!debugInfo->findType("WTF::TextPosition"), "a type complete in more than one image is ambiguous");
        TEST_ASSERT(!debugInfo->findType("(anonymous namespace)::NoSuchType"), "a type no image defines is missing");
    }
    // dyld lists the executable first.
    TEST_ASSERT(debugInfo->findType("WTF::TextPosition", snapshot.images()[0].path().data()), "naming the image resolves the ambiguity");

    dataLogLn("    eager values");
    checkFixture<Materialization::Eager>(snapshot, fixtureAddress, *fixtureType);
    dataLogLn("    lazy values");
    checkFixture<Materialization::Lazy>(snapshot, fixtureAddress, *fixtureType);

    auto eager = EagerTargetValue::at(snapshot, fixtureAddress, Ref<TargetType> { *fixtureType });
    auto lazy = LazyTargetValue::at(snapshot, fixtureAddress, Ref<TargetType> { *fixtureType });
    TEST_ASSERT(eager && lazy && eager->bytes() == lazy->bytes(), "eager and lazy values read the same bytes");
}

void attachAndAnalyze(pid_t pid, Address fixture)
{
    RefPtr<Process> process = Process::create(pid);
    bool attached = process->attach();
    TEST_ASSERT(attached, "attaching to the target process succeeds");
    if (!attached)
        return;
    Snapshot snapshot(process);
    TEST_ASSERT(snapshot.isValid(), "a snapshot of the target process is valid");
    if (!snapshot.isValid())
        return;

    analyze(snapshot, fixture);
}

// CLAUDE: abstract away this mechanism of running a test in and out of process for both test files that do it.
void testInThisProcess()
{
    TargetValueFixture fixture;
    attachAndAnalyze(getpid(), Address { &fixture });
}

// The analysis and the target are separate processes: this process launches a
// copy of itself as the target and takes a corpse of it.
void testInSeparateProcess()
{
    // The target writes the address of its fixture to its stdout.
    std::array<int, 2> addressPipe { -1, -1 };
    TEST_ASSERT(!pipe(addressPipe.data()), "a pipe from the target opens");
    auto closePipe = makeScopeExit([&] {
        for (int fd : addressPipe) {
            if (fd >= 0)
                close(fd);
        }
    });
    if (addressPipe[0] < 0)
        return;

    CString executablePath = Process::create(getpid())->executablePath();
    TEST_ASSERT(!executablePath.isNull(), "this process's executable path is readable");
    if (executablePath.isNull())
        return;

    char* const arguments[] = {
        const_cast<char*>(executablePath.data()),
        const_cast<char*>("--target-value-target"),
        nullptr
    };
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, addressPipe[1], STDOUT_FILENO);
    posix_spawn_file_actions_addclose(&actions, addressPipe[0]);
    posix_spawn_file_actions_addclose(&actions, addressPipe[1]);
    pid_t child = 0;
    int error = posix_spawn(&child, executablePath.data(), &actions, nullptr, arguments, environ);
    posix_spawn_file_actions_destroy(&actions);
    TEST_ASSERT(!error, "the target process launches");
    if (error) {
        dataLogLn("    posix_spawn: ", safeStrerror(error));
        return;
    }
    auto killChild = makeScopeExit([&] {
        kill(child, SIGKILL);
        while (waitpid(child, nullptr, 0) < 0 && errno == EINTR) { }
    });

    uint64_t fixture = 0;
    bool reported = read(addressPipe[0], &fixture, sizeof(fixture)) == sizeof(fixture);
    TEST_ASSERT(reported, "the target reports the address of its fixture");
    if (!reported)
        return;

    attachAndAnalyze(child, Address { fixture });
}

} // anonymous namespace

void testTargetValue()
{
    SuiteTracer tracer("TargetValue");
    if (!tracer.shouldRun())
        return;

    testInThisProcess();
    testInSeparateProcess();
}

int runTargetValueTarget()
{
    TargetValueFixture fixture;
    auto address = reinterpret_cast<uint64_t>(&fixture);
    if (write(STDOUT_FILENO, &address, sizeof(address)) != sizeof(address))
        return 1;

    // The analysis kills this process when it is done with the fixture.
    while (true)
        pause();
}

#else

void testTargetValue()
{
    SuiteTracer tracer("TargetValue");
    if (!tracer.shouldRun())
        return;

    // CLAUDE: abstract away this check, we only need it once. Don't duplicate the test bodies.
#if ASSERT_ENABLED
    TEST_ASSERT(false, "we expected to test mya_heap in this configuration");
#else
    skipSuite("TargetValue", "mya_heap is not enabled");
#endif
}

int runTargetValueTarget()
{
    return 1;
}

#endif // ENABLE(MYA_HEAP)

} // namespace JSCToolsTest
