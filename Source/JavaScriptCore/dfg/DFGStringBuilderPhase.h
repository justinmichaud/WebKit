/*
 * Copyright (C) 2026 Apple Inc. All rights reserved.
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
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES ARE DISCLAIMED.
 */

#pragma once

#if ENABLE(DFG_JIT)

namespace JSC { namespace DFG {

class Graph;

// Prototype: detect the string-accumulator ("StringBuilder") pattern -- a loop-carried local V
// repeatedly reassigned via V = V + x (MakeRope), where V is not otherwise used inside the loop.
// This is the dominant rope-churn pattern in the Synergy UI app (createFilledPath's waveBody +=).
// When Options::useStringBuilderConcat() is set, it also lowers each accumulating MakeRope to a
// StringBuilderAppend that builds natively instead of allocating a JS rope per iteration.
bool performStringBuilderConcat(Graph&);

} } // namespace JSC::DFG

#endif // ENABLE(DFG_JIT)
