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
#include "CorpseTypeName.h"

#if HAVE(CORPSE_SUPPORT)

#include <cxxabi.h>
#include <memory>
#include <stdlib.h>
#include <wtf/ASCIICType.h>
#include <wtf/SystemFree.h>
#include <wtf/Vector.h>
#include <wtf/text/StringBuilder.h>
#include <wtf/text/StringToIntegerConversion.h>

namespace JSC {
namespace Corpse {
namespace TypeName {

String normalizeTypeName(StringView name)
{
    StringBuilder builder;
    builder.reserveCapacity(name.length());

    bool previousWasIdentifierCharacter = false;
    for (unsigned i = 0; i < name.length();) {
        char16_t character = name[i];

        // A run of digits that does not continue an identifier is a literal,
        // and any u/l suffix on it is spelling rather than meaning.
        if (isASCIIDigit(character) && !previousWasIdentifierCharacter) {
            unsigned digitsEnd = i;
            while (digitsEnd < name.length() && isASCIIDigit(name[digitsEnd]))
                ++digitsEnd;
            unsigned suffixEnd = digitsEnd;
            while (suffixEnd < name.length() && (name[suffixEnd] == 'u' || name[suffixEnd] == 'U'
                || name[suffixEnd] == 'l' || name[suffixEnd] == 'L')) {
                ++suffixEnd;
            }
            // Unless what follows keeps going, in which case this was an
            // identifier all along and must be copied whole.
            bool isLiteral = suffixEnd >= name.length()
                || !(isASCIIAlphanumeric(name[suffixEnd]) || name[suffixEnd] == '_');
            unsigned end = isLiteral ? digitsEnd : suffixEnd;
            builder.append(name.substring(i, end - i));
            previousWasIdentifierCharacter = !isLiteral;
            i = isLiteral ? suffixEnd : end;
            continue;
        }

        // Spacing around punctuation is the demangler's choice, not the type's.
        if (isASCIIWhitespace(character)) {
            unsigned next = i;
            while (next < name.length() && isASCIIWhitespace(name[next]))
                ++next;
            bool droppable = next >= name.length() || name[next] == '>' || name[next] == ','
                || name[next] == '*' || name[next] == '&';
            if (!droppable && !builder.isEmpty()) {
                char16_t last = builder[builder.length() - 1];
                droppable = last == '<' || last == '(';
            }
            if (!droppable)
                builder.append(' ');
            previousWasIdentifierCharacter = false;
            i = next;
            continue;
        }

        builder.append(character);
        previousWasIdentifierCharacter = isASCIIAlphanumeric(character) || character == '_';
        ++i;
    }
    return builder.toString();
}

std::optional<String> demangleType(const char* mangledType)
{
    if (!mangledType || !*mangledType)
        return std::nullopt;

    // __cxa_demangle reads a bare type mangling as well as a whole symbol,
    // which is how a std::type_info name is read back: the two are the same
    // encoding. Anything it refuses is a mangling this build's demangler does
    // not know, and the class simply does not appear in the index.
    int status = 0;
    auto demangled = adoptSystemMalloc(abi::__cxa_demangle(mangledType, nullptr, nullptr, &status));
    if (status || !demangled)
        return std::nullopt;
    String result = String::fromUTF8(demangled.get());
    if (result.isEmpty())
        return std::nullopt;
    return result;
}

} // namespace TypeName
} // namespace Corpse
} // namespace JSC

#endif // HAVE(CORPSE_SUPPORT)
