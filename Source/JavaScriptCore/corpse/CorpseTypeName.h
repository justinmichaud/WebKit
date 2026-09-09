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

#pragma once

#include <JavaScriptCore/CorpsePlatform.h>

#if HAVE(CORPSE_SUPPORT)

#include <optional>
#include <wtf/text/StringView.h>
#include <wtf/text/WTFString.h>

namespace JSC {
namespace Corpse {

// Turning the name a target stores into the name the debug info spells.
//
// A class records its own name mangled -- that is what a std::type_info holds
// and what the ABI puts in a `_ZTV` symbol -- while debug info spells it out.
// Comparing the two means demangling one and reconciling the punctuation.
namespace TypeName {

// "N3WTF17StringPrintStreamE" -> "WTF::StringPrintStream". The argument is a
// mangled type, which is what a std::type_info carries, so the platform
// demangler reads it directly. Nullopt for a mangling it cannot parse.
std::optional<String> demangleType(const char* mangledType);

// A type name in one spelling, so that a name from either side lands on one
// value.
//
// The demangler and the debug info agree on the shape of a name but not always
// on its punctuation: a non-type template argument comes out of the demangler
// as `4u` and out of the debug info as `4`, and the two describe the same type.
// Normalising strips the integer-literal suffixes and the optional spacing. Two
// instantiations cannot differ only by a literal's suffix, since the
// parameter's type is what fixes it, so nothing distinct is merged.
String normalizeTypeName(StringView);

} // namespace TypeName

} // namespace Corpse
} // namespace JSC

#endif // HAVE(CORPSE_SUPPORT)
