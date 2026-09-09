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
#include "CorpseTypeNameTest.h"

#if HAVE(CORPSE_SUPPORT)

#include "LibJSCToolsTestUtilities.h"

#include <JavaScriptCore/CorpseTypeName.h>
#include <wtf/text/ASCIILiteral.h>
#include <wtf/text/WTFString.h>

namespace JSCToolsTest {

// The two operations that turn a `_ZTV` symbol into a name the debug info can
// be asked about. Neither needs a corpse, so they are checked outright rather
// than through whatever types this binary happens to contain.
void testTypeName()
{
    SuiteTracer tracer("TypeName");
    if (!tracer.shouldRun())
        return;

    using namespace JSC::Corpse::TypeName;

    auto demangles = [](const char* mangled, ASCIILiteral expected) {
        auto result = demangleType(mangled);
        TEST_ASSERT(result, "a mangled type demangles");
        if (result)
            TEST_ASSERT_EQ(*result, String { expected }, "the demangled type reads as expected");
    };

    demangles("N3WTF17StringPrintStreamE", "WTF::StringPrintStream"_s);
    demangles("3Foo", "Foo"_s);
    demangles("N3JSC6JSCellE", "JSC::JSCell"_s);
    // A template, its arguments, and the substitutions std:: is spelled with:
    // the whole reason the name comes from a demangler rather than from a
    // mangler written here.
    demangles("NSt3__16vectorIiNS_9allocatorIiEEEE",
        "std::__1::vector<int, std::__1::allocator<int>>"_s);

    TEST_ASSERT(!demangleType("not a mangled name at all"),
        "a string that is not a mangling demangles to nothing");
    TEST_ASSERT(!demangleType(""), "an empty mangling demangles to nothing");
    TEST_ASSERT(!demangleType(nullptr), "no mangling demangles to nothing");

    auto normalizes = [](ASCIILiteral input, ASCIILiteral expected) {
        TEST_ASSERT_EQ(normalizeTypeName(input), String { expected },
            "a type name normalises as expected");
    };

    // The difference that matters: a demangler writes a non-type argument with
    // its literal suffix and the debug info writes it without.
    normalizes("X<unsigned long, 4u>"_s, "X<unsigned long, 4>"_s);
    normalizes("X<unsigned long, 4>"_s, "X<unsigned long, 4>"_s);
    normalizes("A<0ul, 1ull, 2L>"_s, "A<0, 1, 2>"_s);
    // Spacing around punctuation is the demangler's choice, not the type's.
    normalizes("A< B , 4ul >"_s, "A<B, 4>"_s);
    normalizes("std::vector<int, std::allocator<int> >"_s,
        "std::vector<int, std::allocator<int>>"_s);
    // A digit inside an identifier is not a literal and keeps whatever follows.
    normalizes("Foo4u"_s, "Foo4u"_s);
    normalizes("B<Foo4u, 4u>"_s, "B<Foo4u, 4>"_s);
    normalizes("int"_s, "int"_s);
    normalizes(""_s, ""_s);
}

} // namespace JSCToolsTest

#endif // HAVE(CORPSE_SUPPORT)
