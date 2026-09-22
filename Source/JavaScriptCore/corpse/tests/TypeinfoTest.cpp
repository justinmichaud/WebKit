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
#include "LibJSCToolsTestUtilities.h"
#include "TypeinfoTest.h"

#include <JavaScriptCore/CorpsePlatform.h>

#if HAVE(MYA_TYPEINFO)

#include <JavaScriptCore/CorpseLLDB.h>
#include <array>
#include <limits.h>
#include <string_view>
#include <typeinfo>
#include <wtf/text/StringImpl.h>

#if OS(DARWIN)
#include <mach-o/dyld.h>
#else
#include <unistd.h>
#endif

#endif // HAVE(MYA_TYPEINFO)

namespace JSCToolsTest {

#if HAVE(MYA_TYPEINFO)

namespace {

// The virtual destructors are out of line so that each class has a key
// function, and so exactly one vtable and one type_info is emitted for each.
class Base {
public:
    virtual ~Base();
    virtual unsigned kind() const;
};

class Derived : public Base {
public:
    ~Derived() override;
    unsigned kind() const override;
};

Base::~Base() = default;
unsigned Base::kind() const { return 1; }

Derived::~Derived() = default;
unsigned Derived::kind() const { return 2; }

void testRuntimeTypeInformation()
{
    Derived derived;
    Base& base = derived;
    TEST_ASSERT(typeid(base) == typeid(Derived), "typeid names the class an object actually is");
    TEST_ASSERT(dynamic_cast<Derived*>(&base), "dynamic_cast reaches the derived class");

    // A class with no vtable has a type_info too; nothing about typeid needs
    // the object to be polymorphic.
    TEST_ASSERT(std::string_view(typeid(WTF::StringImpl).name()).contains("StringImpl"),
        "the type_info of WTF::StringImpl names it");
}

// The path of this executable, which is the module whose debug info describes
// the types this process is built from. Leaves `into` NUL-terminated.
bool currentExecutablePath(std::span<char> into)
{
#if OS(DARWIN)
    auto size = static_cast<uint32_t>(into.size());
    return !_NSGetExecutablePath(into.data(), &size);
#elif OS(LINUX)
    ssize_t length = readlink("/proc/self/exe", into.data(), into.size() - 1);
    if (length <= 0)
        return false;
    into[length] = '\0';
    return true;
#else
    UNUSED_PARAM(into);
    return false;
#endif
}

lldb::SBTypeMember memberNamed(lldb::SBType type, std::string_view name)
{
    for (uint32_t index = 0; index < type.GetNumberOfFields(); ++index) {
        auto member = type.GetFieldAtIndex(index);
        const char* memberName = member.GetName();
        if (memberName && name == memberName)
            return member;
    }
    return { };
}

void checkMemberOffset(lldb::SBType type, std::string_view name, size_t expectedOffset,
    size_t expectedSize, const char* message)
{
    auto member = memberNamed(type, name);
    TEST_ASSERT(member.IsValid(), message);
    if (!member.IsValid())
        return;
    TEST_ASSERT_EQ(static_cast<size_t>(member.GetOffsetInBytes()), expectedOffset, message);
    TEST_ASSERT_EQ(static_cast<size_t>(member.GetType().GetByteSize()), expectedSize, message);
}

// Reads WTF::StringImpl's layout back out of this binary's own debug info and
// compares it with what the compiler laid out. A disagreement means LLDB is
// reading something other than this build.
void testTypeSystem()
{
    std::array<char, PATH_MAX> path { };
    if (!currentExecutablePath(path)) {
        TEST_ASSERT(false, "this executable's own path is readable");
        return;
    }

    lldb::SBDebugger::Initialize();

    lldb::SBDebugger debugger = lldb::SBDebugger::Create();
    TEST_ASSERT(debugger.IsValid(), "liblldb creates a debugger");
    if (debugger.IsValid()) {
        lldb::SBError error;
        lldb::SBTarget target = debugger.CreateTarget(path.data(), nullptr, nullptr, true, error);
        TEST_ASSERT(target.IsValid(), "liblldb opens this executable as a target");

        // The target is built from files and is never attached to or launched:
        // a debugger that owned this process would stop it out from under us.
        RELEASE_ASSERT_WITH_MESSAGE(!target.GetProcess().IsValid(),
            "The corpse type system must never attach to a process.");

        // StringImplShape is the base that owns StringImpl's data members.
        lldb::SBType shape = target.FindFirstType("WTF::StringImplShape");
        if (!shape.IsValid())
            skipSuite("Typeinfo/StringImplShape", "this build carries no debug info for WTF::StringImplShape");
        else {
            TEST_ASSERT_EQ(static_cast<size_t>(shape.GetByteSize()), sizeof(WTF::StringImpl),
                "the debug info gives StringImplShape the size of a StringImpl");
            checkMemberOffset(shape, "m_length", static_cast<size_t>(WTF::StringImpl::lengthMemoryOffset()),
                sizeof(unsigned), "the debug info places m_length where lengthMemoryOffset() does");
            checkMemberOffset(shape, "m_hashAndFlags", static_cast<size_t>(WTF::StringImpl::flagsOffset()),
                sizeof(unsigned), "the debug info places m_hashAndFlags where flagsOffset() does");
        }

        debugger.DeleteTarget(target);
    }
    lldb::SBDebugger::Destroy(debugger);

    lldb::SBDebugger::Terminate();
}

} // anonymous namespace

void testTypeinfo()
{
    SuiteTracer tracer("Typeinfo");
    if (!tracer.shouldRun())
        return;

    testRuntimeTypeInformation();
    testTypeSystem();
}

#else // Neither LLDB's headers nor RTTI, so there is nothing to ask.

void testTypeinfo()
{
    SuiteTracer tracer("Typeinfo");
    if (!tracer.shouldRun())
        return;

    // Only a debug build gets here with something missing, and on a platform mya
    // is developed on that is a build set up wrong rather than a build that was
    // never meant to read types.
#if ASSERT_ENABLED && (OS(DARWIN) || OS(LINUX))
    TEST_ASSERT(false, "this build is missing LLDB's SB API headers or RTTI, which mya "
        "needs to say what an address holds: install lldb (\"brew install lldb\", "
        "liblldb-dev or lldb-devel) and build with RTTI");
#else
    skipSuite("Typeinfo", "reading types takes a debug build, with LLDB and RTTI");
#endif
}

#endif // HAVE(MYA_TYPEINFO)

} // namespace JSCToolsTest
