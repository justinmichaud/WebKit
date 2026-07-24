/*
 *  Copyright (C) 1999-2002 Harri Porten (porten@kde.org)
 *  Copyright (C) 2001 Peter Kelly (pmk@post.com)
 *  Copyright (C) 2004-2021 Apple Inc. All rights reserved.
 *  Copyright (C) 2026 Igalia S.L.
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Library General Public
 *  License as published by the Free Software Foundation; either
 *  version 2 of the License, or (at your option) any later version.
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Library General Public License for more details.
 *
 *  You should have received a copy of the GNU Library General Public License
 *  along with this library; see the file COPYING.LIB.  If not, write to
 *  the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 *  Boston, MA 02110-1301, USA.
 *
 */

#include "config.h"
#include "JSString.h"

#include "JSGlobalObjectFunctions.h"
#include "JSGlobalObjectInlines.h"
#include "JSObjectInlines.h"
#include "StringObject.h"
#include "StrongInlines.h"
#include "StructureCreateInlines.h"
#include "CodeBlock.h" // rope-site instrumentation: bytecode -> source location

// --- BEGIN rope-usage instrumentation (temporary, env-gated; JSC_ROPE_TRACE=1) ---
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <execinfo.h>
#include <mutex>
#include <unordered_map>
#include <string>
#include <vector>
#include <wtf/text/StringBuilder.h>
#include <wtf/text/StringToIntegerConversion.h>
// --- END ---

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC {

// --- BEGIN rope-usage instrumentation implementation ---
// Confirms, on a live workload, how JSRopeStrings are consumed: how many get
// *resolved* (and via which API: plain value() vs toAtomString), their length /
// 8-bit distribution, and the C++ call stack of the consumer (to see if it is SVG/DOM).
// Denominator (total ropes ever created) comes from the destroy hook in JSStringInlines.h.
// Gated entirely on getenv("JSC_ROPE_TRACE"); zero effect unless set.
bool g_ropeTraceEnabled = false;
// Separate flag (env RSITE=1, implies rope trace) enabling per-creation-SITE attribution:
// tag each created rope with the bytecode site of the concat that made it (run JIT-off for
// complete coverage), then link creation site -> how the rope is later resolved. Answers
// "which concatenation sites / functions produce the resolved (C++-consumed) strings".
bool g_ropeSiteEnabled = false;

namespace {

enum RopeResolveKind { KindPlain = 0, KindAtom = 1, KindExistingAtom = 2, KindCount = 3 };
static const char* const kindName[KindCount] = { "value(plain)", "toAtomString", "toExistingAtomString" };

// ---- per-creation-site attribution ----
struct SiteInfo {
    std::string loc;              // "func  url:line:col"  (or "strcat ...")
    uint64_t create = 0;          // ropes created at this site
    uint64_t resolve[KindCount] = { }; // of those, how many resolved via each path
    // operand ("immediate string") classification for op_add sites:
    uint64_t lhsNum = 0, lhsRope = 0, lhsFlat = 0, lhsOther = 0;
    uint64_t rhsNum = 0, rhsRope = 0, rhsFlat = 0, rhsOther = 0;
    uint64_t strcatCount = 0;     // op_strcat operand-count accumulator
    uint64_t strcatN = 0;
};

struct SiteState {
    std::mutex mutex;
    std::unordered_map<uint64_t, uint32_t> intern;  // (cb,bcOffset) key -> siteId
    std::vector<SiteInfo> sites;
    std::unordered_map<const JSString*, uint32_t> ropeSite; // live rope addr -> siteId
    uint64_t mapDropped = 0;      // creations not tagged due to cap
    static constexpr size_t kMapCap = 12'000'000;
};
static SiteState& siteState() { static SiteState* s = new SiteState(); return *s; }

struct RopeTraceState {
    std::atomic<uint64_t> resolveCount[KindCount] { };
    std::atomic<uint64_t> is8bit[KindCount] { };
    std::atomic<uint64_t> isSubstring[KindCount] { };
    // log2 length buckets [0..31]
    std::atomic<uint64_t> lenBucket[KindCount][32] { };
    std::atomic<uint64_t> lenSum[KindCount] { };

    // destroy-side denominator (see JSStringInlines.h)
    std::atomic<uint64_t> destroyTotal { 0 };
    std::atomic<uint64_t> destroyStillRope { 0 }; // never resolved
    std::atomic<uint64_t> destroyResolved { 0 };  // had a StringImpl to free

    // sampled backtraces (mutator thread, so the JS/DOM consumer is on the stack)
    std::mutex stackMutex;
    std::unordered_map<std::string, uint64_t> stacks; // signature -> count

    uint64_t sampleEvery = 256;
    uint64_t dumpEvery = 500000;
    std::atomic<uint64_t> nextDumpAt { 500000 };
    std::string outPath;
    std::mutex dumpMutex;
};

static RopeTraceState& traceState()
{
    static RopeTraceState* s = new RopeTraceState();
    return *s;
}

static unsigned log2Bucket(unsigned n)
{
    unsigned b = 0;
    while (n > 1 && b < 31) { n >>= 1; ++b; }
    return b;
}

static std::string captureStackSignature()
{
    void* frames[48];
    int n = backtrace(frames, 48);
    std::string sig;
    // Skip frames 0..2 (backtrace + recordResolve + resolve*), keep next ~18.
    int start = 3;
    int end = n < start + 18 ? n : start + 18;
    for (int i = start; i < end; ++i) {
        Dl_info info;
        char buf[256];
        if (dladdr(frames[i], &info) && info.dli_fbase) {
            const char* fname = info.dli_fname ? info.dli_fname : "?";
            const char* base = strrchr(fname, '/');
            base = base ? base + 1 : fname;
            uintptr_t off = reinterpret_cast<uintptr_t>(frames[i]) - reinterpret_cast<uintptr_t>(info.dli_fbase);
            snprintf(buf, sizeof(buf), "%s+0x%lx", base, static_cast<unsigned long>(off));
        } else
            snprintf(buf, sizeof(buf), "?+%p", frames[i]);
        if (!sig.empty())
            sig += ';';
        sig += buf;
    }
    return sig;
}

static void dumpSnapshot()
{
    RopeTraceState& s = traceState();
    std::unique_lock<std::mutex> dl(s.dumpMutex, std::try_to_lock);
    if (!dl.owns_lock())
        return;
    FILE* f = fopen(s.outPath.c_str(), "w");
    if (!f)
        return;

    uint64_t totalResolved = 0;
    for (int k = 0; k < KindCount; ++k)
        totalResolved += s.resolveCount[k].load(std::memory_order_relaxed);

    uint64_t dTotal = s.destroyTotal.load(std::memory_order_relaxed);
    uint64_t dRope = s.destroyStillRope.load(std::memory_order_relaxed);
    uint64_t dResolved = s.destroyResolved.load(std::memory_order_relaxed);

    fprintf(f, "=== ROPE TRACE SNAPSHOT ===\n");
    fprintf(f, "ropes destroyed (>= created that reached a sweep): %llu\n", (unsigned long long)dTotal);
    fprintf(f, "  still-rope at destroy (NEVER resolved): %llu (%.1f%%)\n",
        (unsigned long long)dRope, dTotal ? 100.0 * dRope / dTotal : 0.0);
    fprintf(f, "  resolved-with-impl at destroy:          %llu (%.1f%%)\n",
        (unsigned long long)dResolved, dTotal ? 100.0 * dResolved / dTotal : 0.0);
    fprintf(f, "total rope resolutions: %llu\n", (unsigned long long)totalResolved);
    for (int k = 0; k < KindCount; ++k) {
        uint64_t c = s.resolveCount[k].load(std::memory_order_relaxed);
        if (!c) continue;
        uint64_t b8 = s.is8bit[k].load(std::memory_order_relaxed);
        uint64_t sub = s.isSubstring[k].load(std::memory_order_relaxed);
        uint64_t sum = s.lenSum[k].load(std::memory_order_relaxed);
        fprintf(f, "  [%s] count=%llu  8bit=%.1f%%  substring=%.1f%%  avgLen=%.1f\n",
            kindName[k], (unsigned long long)c,
            100.0 * b8 / c, 100.0 * sub / c, (double)sum / c);
        fprintf(f, "      len log2 buckets: ");
        for (int b = 0; b < 24; ++b) {
            uint64_t v = s.lenBucket[k][b].load(std::memory_order_relaxed);
            if (v) fprintf(f, "[%d:%llu] ", 1 << b, (unsigned long long)v);
        }
        fprintf(f, "\n");
    }

    fprintf(f, "\n=== SAMPLED CONSUMER STACKS (1/%llu of resolutions) ===\n", (unsigned long long)s.sampleEvery);
    {
        std::lock_guard<std::mutex> lk(s.stackMutex);
        // sort by count desc
        std::vector<std::pair<std::string, uint64_t>> v(s.stacks.begin(), s.stacks.end());
        std::sort(v.begin(), v.end(), [](auto& a, auto& b){ return a.second > b.second; });
        int shown = 0;
        for (auto& [sig, cnt] : v) {
            if (shown++ >= 60) break;
            fprintf(f, "%llu  %s\n", (unsigned long long)cnt, sig.c_str());
        }
    }

    if (g_ropeSiteEnabled) {
        SiteState& ss = siteState();
        std::lock_guard<std::mutex> lk(ss.mutex);
        std::vector<uint32_t> idx(ss.sites.size());
        for (uint32_t i = 0; i < idx.size(); ++i) idx[i] = i;
        auto resolvedTotal = [&](uint32_t i){ uint64_t t = 0; for (int k = 0; k < KindCount; ++k) t += ss.sites[i].resolve[k]; return t; };

        fprintf(f, "\n=== ROPE CREATION SITES (JIT-off; distinct concat sites=%zu; liveMapDropped=%llu) ===\n",
            ss.sites.size(), (unsigned long long)ss.mapDropped);

        // (a) sites ranked by total ropes CREATED (where the intermediates come from)
        std::sort(idx.begin(), idx.end(), [&](uint32_t a, uint32_t b){ return ss.sites[a].create > ss.sites[b].create; });
        fprintf(f, "-- top sites by ropes CREATED --\n");
        for (size_t n = 0; n < idx.size() && n < 40; ++n) {
            SiteInfo& si = ss.sites[idx[n]];
            fprintf(f, "%12llu created | resolved p=%llu a=%llu ea=%llu | %s\n",
                (unsigned long long)si.create,
                (unsigned long long)si.resolve[KindPlain], (unsigned long long)si.resolve[KindAtom],
                (unsigned long long)si.resolve[KindExistingAtom], si.loc.c_str());
            if (si.strcatN)
                fprintf(f, "                 operands: strcat avg=%.1f over %llu\n",
                    (double)si.strcatCount / si.strcatN, (unsigned long long)si.strcatN);
            else
                fprintf(f, "                 operands: lhs[num=%llu rope=%llu flat=%llu oth=%llu] rhs[num=%llu rope=%llu flat=%llu oth=%llu]\n",
                    (unsigned long long)si.lhsNum,(unsigned long long)si.lhsRope,(unsigned long long)si.lhsFlat,(unsigned long long)si.lhsOther,
                    (unsigned long long)si.rhsNum,(unsigned long long)si.rhsRope,(unsigned long long)si.rhsFlat,(unsigned long long)si.rhsOther);
        }

        // (b) sites ranked by ropes RESOLVED (the C++-consumed / materialized strings)
        std::sort(idx.begin(), idx.end(), [&](uint32_t a, uint32_t b){ return resolvedTotal(a) > resolvedTotal(b); });
        fprintf(f, "-- top sites by ropes RESOLVED (materialized; a=atomized=setAttribute path) --\n");
        for (size_t n = 0; n < idx.size() && n < 25; ++n) {
            uint32_t i = idx[n];
            if (!resolvedTotal(i)) break;
            SiteInfo& si = ss.sites[i];
            fprintf(f, "  resolved=%llu (p=%llu a=%llu ea=%llu) of %llu created | %s\n",
                (unsigned long long)resolvedTotal(i),
                (unsigned long long)si.resolve[KindPlain], (unsigned long long)si.resolve[KindAtom],
                (unsigned long long)si.resolve[KindExistingAtom], (unsigned long long)si.create, si.loc.c_str());
        }
    }
    fclose(f);
}

static void recordResolve(const JSRopeString* rope, RopeResolveKind kind)
{
    RopeTraceState& s = traceState();
    unsigned len = rope->length();
    bool is8 = rope->is8Bit();
    bool sub = rope->isSubstring();

    uint64_t seq = s.resolveCount[kind].fetch_add(1, std::memory_order_relaxed) + 1;
    if (is8) s.is8bit[kind].fetch_add(1, std::memory_order_relaxed);
    if (sub) s.isSubstring[kind].fetch_add(1, std::memory_order_relaxed);
    s.lenBucket[kind][log2Bucket(len)].fetch_add(1, std::memory_order_relaxed);
    s.lenSum[kind].fetch_add(len, std::memory_order_relaxed);

    if ((seq % s.sampleEvery) == 0) {
        std::string sig = captureStackSignature();
        std::lock_guard<std::mutex> lk(s.stackMutex);
        s.stacks[sig]++;
    }

    // Link this resolution back to the concat site that created the rope.
    if (g_ropeSiteEnabled) {
        SiteState& ss = siteState();
        std::lock_guard<std::mutex> lk(ss.mutex);
        auto it = ss.ropeSite.find(static_cast<const JSString*>(rope));
        if (it != ss.ropeSite.end())
            ss.sites[it->second].resolve[kind]++;
    }

    uint64_t total = 0;
    for (int k = 0; k < KindCount; ++k)
        total += s.resolveCount[k].load(std::memory_order_relaxed);
    uint64_t target = s.nextDumpAt.load(std::memory_order_relaxed);
    if (total >= target) {
        if (s.nextDumpAt.compare_exchange_strong(target, target + s.dumpEvery, std::memory_order_relaxed))
            dumpSnapshot();
    }
}

struct RopeTraceInit {
    RopeTraceInit()
    {
        const char* e = getenv("RTRACE");
        if (e && e[0] && e[0] != '0') {
            g_ropeTraceEnabled = true;
            RopeTraceState& s = traceState();
            if (const char* o = getenv("RTRACE_OUT")) s.outPath = o;
            else s.outPath = "/tmp/rope-trace.txt";
            if (const char* sv = getenv("RTRACE_SAMPLE")) { uint64_t v = strtoull(sv, nullptr, 10); if (v) s.sampleEvery = v; }
            if (const char* dv = getenv("RTRACE_DUMP")) { uint64_t v = strtoull(dv, nullptr, 10); if (v) { s.dumpEvery = v; s.nextDumpAt = v; } }
            const char* rs = getenv("RSITE");
            if (rs && rs[0] && rs[0] != '0')
                g_ropeSiteEnabled = true;
            fprintf(stderr, "[rope-trace] ENABLED out=%s sampleEvery=%llu dumpEvery=%llu site=%d\n",
                s.outPath.c_str(), (unsigned long long)s.sampleEvery, (unsigned long long)s.dumpEvery, g_ropeSiteEnabled);
            atexit([]{ dumpSnapshot(); });
        }
    }
};
static RopeTraceInit s_ropeTraceInit;

} // anonymous namespace

// Called from JSRopeString::destroy (JSStringInlines.h) during sweep.
void ropeTraceRecordDestroy(const JSRopeString* rope)
{
    RopeTraceState& s = traceState();
    s.destroyTotal.fetch_add(1, std::memory_order_relaxed);
    if (rope->isRope())
        s.destroyStillRope.fetch_add(1, std::memory_order_relaxed);
    else
        s.destroyResolved.fetch_add(1, std::memory_order_relaxed);
    if (g_ropeSiteEnabled) {
        SiteState& ss = siteState();
        std::lock_guard<std::mutex> lk(ss.mutex);
        ss.ropeSite.erase(static_cast<const JSString*>(rope));
    }
}

// Called from the interpreter concat slow paths (CommonSlowPaths.cpp) with JIT off, so every
// string `+` / template concat is captured. Tags the resulting rope with its bytecode site.
// kindTag: 0 = op_add (lhs/rhs meaningful), 1 = op_strcat (operandCount meaningful).
void ropeSiteRecordCreate(JSString* result, CodeBlock* codeBlock, unsigned bytecodeOffset,
    EncodedJSValue lhs, EncodedJSValue rhs, unsigned kindTag, unsigned operandCount)
{
    if (!result || !result->isRope())
        return;
    SiteState& ss = siteState();
    std::lock_guard<std::mutex> lk(ss.mutex);

    uint64_t key = (static_cast<uint64_t>(reinterpret_cast<uintptr_t>(codeBlock)) << 20)
        ^ (bytecodeOffset & 0xFFFFF) ^ (static_cast<uint64_t>(kindTag) << 62);
    uint32_t siteId;
    auto it = ss.intern.find(key);
    if (it == ss.intern.end()) {
        siteId = static_cast<uint32_t>(ss.sites.size());
        SiteInfo info;
        // Compute human-readable location once.
        LineColumn lc = codeBlock->lineColumnForBytecodeIndex(BytecodeIndex(bytecodeOffset));
        String url = codeBlock->ownerExecutable()->sourceURL();
        CString name = codeBlock->inferredName();
        char buf[512];
        snprintf(buf, sizeof(buf), "%s  %s:%u:%u  [%s]",
            name.data() ? name.data() : "?",
            url.isEmpty() ? "?" : url.utf8().data(),
            lc.line, lc.column, kindTag ? "strcat" : "add");
        info.loc = buf;
        ss.sites.push_back(WTF::move(info));
        ss.intern.emplace(key, siteId);
    } else
        siteId = it->second;

    SiteInfo& si = ss.sites[siteId];
    si.create++;
    if (kindTag == 1) { si.strcatCount += operandCount; si.strcatN++; }
    else {
        JSValue l = JSValue::decode(lhs), r = JSValue::decode(rhs);
        if (l.isNumber()) si.lhsNum++; else if (l.isString()) { asString(l)->isRope() ? si.lhsRope++ : si.lhsFlat++; } else si.lhsOther++;
        if (r.isNumber()) si.rhsNum++; else if (r.isString()) { asString(r)->isRope() ? si.rhsRope++ : si.rhsFlat++; } else si.rhsOther++;
    }

    if (ss.ropeSite.size() < SiteState::kMapCap)
        ss.ropeSite[result] = siteId;
    else
        ss.mapDropped++;
}
// --- END rope-usage instrumentation implementation ---

const ClassInfo JSString::s_info = { "string"_s, nullptr, nullptr, nullptr, CREATE_METHOD_TABLE(JSString) };

Structure* JSString::createStructure(VM& vm, JSGlobalObject* globalObject, JSValue proto)
{
    return Structure::create(vm, globalObject, proto, defaultTypeInfo(), info());
}

JSString* JSString::createEmptyString(VM& vm)
{
    JSString* newString = new (NotNull, allocateCell<JSString>(vm)) JSString(vm, *StringImpl::empty());
    newString->finishCreation(vm);
    return newString;
}

template<>
void JSRopeString::RopeBuilder<RecordOverflow>::expand()
{
    RELEASE_ASSERT(!this->hasOverflowed());
    ASSERT(m_index == JSRopeString::s_maxInternalRopeLength);
    static_assert(3 == JSRopeString::s_maxInternalRopeLength);
    ASSERT(m_length);
    ASSERT(m_strings[0]->length());
    ASSERT(m_strings[1]->length());
    ASSERT(m_strings[2]->length());

    JSString* string = JSRopeString::create(m_vm, m_strings[0], m_strings[1], m_strings[2]);
    ASSERT(string->length() == m_length);
    m_strings[0] = string;
    m_index = 1;
}

void JSString::dumpToStream(const JSCell* cell, PrintStream& out)
{
    const JSString* thisObject = uncheckedDowncast<JSString>(cell);
    out.printf("<%p, %s, [%u], ", thisObject, thisObject->className().characters(), thisObject->length());
    uintptr_t pointer = thisObject->fiberConcurrently();
    if (pointer & isRopeInPointer) {
        if (pointer & JSRopeString::isSubstringInPointer)
            out.printf("[substring]");
        else
            out.printf("[rope]");
    } else {
        if (WTF::StringImpl* ourImpl = std::bit_cast<StringImpl*>(pointer)) {
            if (ourImpl->is8Bit())
                out.printf("[8 %p]", ourImpl->span8().data());
            else
                out.printf("[16 %p]", ourImpl->span16().data());
        }
    }
    out.printf(">");
}

bool JSString::equalSlowCase(JSGlobalObject* globalObject, JSString* other) const
{
    return equalInline(globalObject, other);
}

size_t JSString::estimatedSize(JSCell* cell, VM& vm)
{
    JSString* thisObject = asString(cell);
    uintptr_t pointer = thisObject->fiberConcurrently();
    if (pointer & isRopeInPointer)
        return Base::estimatedSize(cell, vm);
    return Base::estimatedSize(cell, vm) + std::bit_cast<StringImpl*>(pointer)->costDuringGC();
}

// --- Prototype StringBuilder: deferred piece accumulation (see JSString.h) ---
// A builder rope records piece POINTERS into a singly-linked list of fixed chunks held off-heap
// (pointed to by fiber1). Chunks are never moved or freed while the builder is live, so a
// concurrent collector can walk them while the mutator appends; a per-append write barrier keeps
// the pieces reachable. Characters are copied once, at materialization. The chunk/state layout is
// declared in JSString.h so the JIT append fast path can address it directly.
using SBChunk = JSStringBuilderChunk;
using SBState = JSStringBuilderState;
static constexpr unsigned sbChunkSize = jsStringBuilderChunkSize;

unsigned JSRopeString::stringBuilderPromoteLength()
{
    static unsigned value = [] -> unsigned {
        if (const char* e = getenv("SB_PROMOTE")) {
            if (auto parsed = parseInteger<unsigned>(StringView::fromLatin1(e)))
                return *parsed;
        }
        return 64;
    }();
    return value;
}

// Diagnostic counters, off by default. The enable flag is a plain global read once at startup so
// the append fast path pays only a predicted-not-taken branch, not a locked atomic, when disabled.
static const bool g_sbStatEnabled = getenv("SB_STAT");
static std::atomic<uint64_t> g_sbCreates { 0 };
static std::atomic<uint64_t> g_sbAppends { 0 };
static std::atomic<uint64_t> g_sbGrows { 0 };
static struct SbStatDump { ~SbStatDump() {
    if (g_sbStatEnabled) fprintf(stderr, "[SB-STAT] creates=%llu appends=%llu grows=%llu\n",
        (unsigned long long)g_sbCreates.load(), (unsigned long long)g_sbAppends.load(), (unsigned long long)g_sbGrows.load());
} } s_sbStatDump;

static ALWAYS_INLINE SBState* sbStateFromFiber1(JSString* fiber1)
{
    return reinterpret_cast<SBState*>(fiber1);
}

// Append one piece pointer. The store is published before the count is bumped so a concurrent
// collector that reads the new count always sees a valid pointer in the slot.
static ALWAYS_INLINE void sbPush(SBState* state, JSString* piece)
{
    SBChunk* tail = state->tail;
    if (tail->count >= sbChunkSize) [[unlikely]] {
        if (g_sbStatEnabled) [[unlikely]]
            g_sbGrows.fetch_add(1, std::memory_order_relaxed);
        SBChunk* chunk = new SBChunk();
        tail->next = chunk;
        state->tail = chunk;
        tail = chunk;
    }
    tail->slots[tail->count] = piece;
    WTF::storeStoreFence();
    tail->count = tail->count + 1;
}

static void sbFree(SBState* state)
{
    for (SBChunk* chunk = state->head; chunk; ) {
        SBChunk* next = chunk->next;
        delete chunk;
        chunk = next;
    }
    delete state;
}

template<typename Visitor>
void JSString::visitChildrenImpl(JSCell* cell, Visitor& visitor)
{
    JSString* thisObject = asString(cell);
    ASSERT_GC_OBJECT_INHERITS(thisObject, info());
    Base::visitChildren(thisObject, visitor);
    
    uintptr_t pointer = thisObject->fiberConcurrently();
    if (pointer & isRopeInPointer) {
        if (pointer & JSRopeString::isSubstringInPointer) {
            visitor.appendUnbarriered(static_cast<JSRopeString*>(thisObject)->fiber1());
            return;
        }
        // Prototype StringBuilder: the marker sentinel in fiber0 distinguishes a builder (whose
        // accumulated pieces live in off-heap chunks and must be traced) from both a normal rope
        // and a partially-constructed rope (null fiber0). Reading each chunk's count with acquire
        // pairs with the release store in sbPush, so a published slot is never read stale.
        if ((pointer & JSRopeString::stringMask) == JSRopeString::stringBuilderMarker) {
            SBState* state = sbStateFromFiber1(static_cast<JSRopeString*>(thisObject)->fiber1());
            for (SBChunk* chunk = state->head; chunk; chunk = chunk->next) {
                unsigned count = chunk->count;
                WTF::loadLoadFence();
                for (unsigned i = 0; i < count; ++i)
                    visitor.appendUnbarriered(chunk->slots[i]);
            }
            return;
        }
        for (unsigned index = 0; index < JSRopeString::s_maxInternalRopeLength; ++index) {
            JSString* fiber = nullptr;
            switch (index) {
            case 0:
                fiber = std::bit_cast<JSString*>(pointer & JSRopeString::stringMask);
                break;
            case 1:
                fiber = static_cast<JSRopeString*>(thisObject)->fiber1();
                break;
            case 2:
                fiber = static_cast<JSRopeString*>(thisObject)->fiber2();
                break;
            default:
                ASSERT_NOT_REACHED();
                return;
            }
            if (!fiber)
                break;
            visitor.appendUnbarriered(fiber);
        }
        return;
    }
    if (StringImpl* impl = std::bit_cast<StringImpl*>(pointer))
        visitor.reportExtraMemoryVisited(impl->costDuringGC());
}

DEFINE_VISIT_CHILDREN(JSString);

template<typename CharacterType>
void JSRopeString::resolveRopeInternalNoSubstring(std::span<CharacterType> buffer, uint8_t* stackLimit) const
{
    resolveToBuffer(fiber0(), fiber1(), fiber2(), buffer, stackLimit);
}

GCOwnedDataScope<AtomStringImpl*> JSRopeString::resolveRopeToAtomString(JSGlobalObject* globalObject) const
{
    if (g_ropeTraceEnabled) recordResolve(this, KindAtom);
    if (isStringBuilderRope()) {
        const_cast<JSRopeString*>(this)->materializeStringBuilder();
        if (!valueInternal().impl()->isAtom()) {
            AtomString atom(valueInternal());
            swapToAtomString(globalObject->vm(), atom.releaseImpl());
        }
        return { this, static_cast<AtomStringImpl*>(valueInternal().impl()) };
    }
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    auto convertToAtomString = [this](const String& string) -> GCOwnedDataScope<AtomStringImpl*> {
        ASSERT(!string.impl() || string.impl()->isAtom());
        return { this, static_cast<AtomStringImpl*>(string.impl()) };
    };

    if (length() > maxLengthForOnStackResolve) {
        scope.release();
        constexpr bool reportAllocation = true;
        return convertToAtomString(resolveRopeWithFunction<reportAllocation>(globalObject, [&] (Ref<StringImpl>&& newImpl) {
            return AtomStringImpl::add(newImpl.ptr());
        }));
    }

    AtomString atomString;
    uint8_t* stackLimit = std::bit_cast<uint8_t*>(vm.softStackLimit());
    if (!isSubstring()) {
        if (is8Bit()) {
            std::array<Latin1Character, maxLengthForOnStackResolve> buffer;
            resolveRopeInternalNoSubstring(std::span { buffer }.first(length()), stackLimit);
            atomString = std::span<const Latin1Character> { buffer }.first(length());
        } else {
            std::array<char16_t, maxLengthForOnStackResolve> buffer;
            resolveRopeInternalNoSubstring(std::span { buffer }.first(length()), stackLimit);
            atomString = std::span<const char16_t> { buffer }.first(length());
        }
    } else
        atomString = StringView { substringBase()->valueInternal() }.substring(substringOffset(), length()).toAtomString();

    size_t sizeToReport = atomString.impl()->hasOneRef() ? atomString.impl()->cost() : 0;
    convertToNonRope(String { atomString.releaseImpl() });
    // If we resolved a string that didn't previously exist, notify the heap that we've grown.
    vm.heap.reportExtraMemoryAllocated(this, sizeToReport);
    return { this, static_cast<AtomStringImpl*>(valueInternal().impl()) };
}

GCOwnedDataScope<AtomStringImpl*> JSRopeString::resolveRopeToExistingAtomString(JSGlobalObject* globalObject) const
{
    if (g_ropeTraceEnabled) recordResolve(this, KindExistingAtom);
    if (isStringBuilderRope()) {
        const_cast<JSRopeString*>(this)->materializeStringBuilder();
        if (RefPtr<AtomStringImpl> existing = AtomStringImpl::lookUp(valueInternal().impl())) {
            swapToAtomString(globalObject->vm(), WTF::move(existing));
            return { this, static_cast<AtomStringImpl*>(valueInternal().impl()) };
        }
        return { };
    }
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    if (length() > maxLengthForOnStackResolve) {
        RefPtr<AtomStringImpl> existingAtomString;
        constexpr bool reportAllocation = true;
        resolveRopeWithFunction<reportAllocation>(globalObject, [&] (Ref<StringImpl>&& newImpl) -> Ref<StringImpl> {
            existingAtomString = AtomStringImpl::lookUp(newImpl.ptr());
            if (existingAtomString)
                return Ref { *existingAtomString };
            return WTF::move(newImpl);
        });
        RETURN_IF_EXCEPTION(scope, { });
        return { this, existingAtomString.get() };
    }
    
    RefPtr<AtomStringImpl> existingAtomString;
    if (!isSubstring()) {
        uint8_t* stackLimit = std::bit_cast<uint8_t*>(vm.softStackLimit());
        if (is8Bit()) {
            std::array<Latin1Character, maxLengthForOnStackResolve> buffer;
            resolveRopeInternalNoSubstring(std::span { buffer }.first(length()), stackLimit);
            existingAtomString = AtomStringImpl::lookUp(std::span { buffer }.first(length()));
        } else {
            std::array<char16_t, maxLengthForOnStackResolve> buffer;
            resolveRopeInternalNoSubstring(std::span { buffer }.first(length()), stackLimit);
            existingAtomString = AtomStringImpl::lookUp(std::span { buffer }.first(length()));
        }
    } else
        existingAtomString = StringView { substringBase()->valueInternal() }.substring(substringOffset(), length()).toExistingAtomString().releaseImpl();

    if (existingAtomString)
        convertToNonRope(*existingAtomString);
    return { this, existingAtomString.get() };
}

template<bool reportAllocation, typename Function>
const String& JSRopeString::resolveRopeWithFunction(JSGlobalObject* nullOrGlobalObjectForOOM, Function&& function) const
{
    ASSERT(isRope());

    VM& vm = this->vm();
    if constexpr (validateDFGDoesGC)
        vm.verifyCanGC();

    if (isSubstring()) {
        ASSERT(!substringBase()->isRope());
        auto newImpl = substringBase()->valueInternal().substringSharingImpl(substringOffset(), length());
        convertToNonRope(function(newImpl.releaseImpl().releaseNonNull()));
        return valueInternal();
    }
    
    if (is8Bit()) {
        std::span<Latin1Character> buffer;
        auto newImpl = StringImpl::tryCreateUninitialized(length(), buffer);
        if (!newImpl) {
            outOfMemory(nullOrGlobalObjectForOOM);
            return nullString();
        }

        size_t sizeToReport = newImpl->cost();
        uint8_t* stackLimit = std::bit_cast<uint8_t*>(vm.softStackLimit());
        resolveRopeInternalNoSubstring(buffer, stackLimit);
        convertToNonRope(function(newImpl.releaseNonNull()));
        if constexpr (reportAllocation)
            vm.heap.reportExtraMemoryAllocated(this, sizeToReport);
        return valueInternal();
    }
    
    std::span<char16_t> buffer;
    auto newImpl = StringImpl::tryCreateUninitialized(length(), buffer);
    if (!newImpl) {
        outOfMemory(nullOrGlobalObjectForOOM);
        return nullString();
    }
    
    size_t sizeToReport = newImpl->cost();
    uint8_t* stackLimit = std::bit_cast<uint8_t*>(vm.softStackLimit());
    resolveRopeInternalNoSubstring(buffer, stackLimit);
    convertToNonRope(function(newImpl.releaseNonNull()));
    if constexpr (reportAllocation)
        vm.heap.reportExtraMemoryAllocated(this, sizeToReport);
    return valueInternal();
}

const String& JSRopeString::resolveRope(JSGlobalObject* nullOrGlobalObjectForOOM) const
{
    if (g_ropeTraceEnabled) recordResolve(this, KindPlain);
    if (isStringBuilderRope()) {
        const_cast<JSRopeString*>(this)->materializeStringBuilder();
        return valueInternal();
    }
    constexpr bool reportAllocation = true;
    return resolveRopeWithFunction<reportAllocation>(nullOrGlobalObjectForOOM, [] (Ref<StringImpl>&& newImpl) {
        return WTF::move(newImpl);
    });
}

// Start a builder seeded with two pieces. `a` is the accumulator's first value (often the empty
// string, or a rope carried over from a lower tier); `b` is the first appended piece. Neither is
// resolved here: the pieces are recorded by pointer and the running length/is8Bit are tracked on
// the cell so length() stays correct without materializing.
JSString* JSRopeString::stringBuilderCreate(JSGlobalObject* globalObject, JSString* a, JSString* b)
{
    if (g_sbStatEnabled) [[unlikely]]
        g_sbCreates.fetch_add(1, std::memory_order_relaxed);
    VM& vm = globalObject->vm();
    JSRopeString* builder = new (NotNull, allocateCell<JSRopeString>(vm)) JSRopeString(vm);
    builder->finishCreation(vm);
    SBState* state = new SBState();
    state->head = state->tail = new SBChunk();
    sbPush(state, a);
    sbPush(state, b);
    builder->initializeFiber1(reinterpret_cast<JSString*>(state));
    builder->initializeFiber0(reinterpret_cast<JSString*>(stringBuilderMarker));
    builder->initializeLength(a->length() + b->length());
    builder->initializeIs8Bit(a->is8Bit() && b->is8Bit());
    vm.writeBarrier(builder, a);
    vm.writeBarrier(builder, b);
    return builder;
}

void JSRopeString::stringBuilderAppend(JSGlobalObject* globalObject, JSString* piece)
{
    if (g_sbStatEnabled) [[unlikely]]
        g_sbAppends.fetch_add(1, std::memory_order_relaxed);
    VM& vm = globalObject->vm();
    sbPush(sbStateFromFiber1(fiber1()), piece);
    initializeLength(length() + piece->length());
    initializeIs8Bit(is8Bit() && piece->is8Bit());
    vm.writeBarrier(this, piece);
}

template<typename CharacterType>
void JSRopeString::copyStringBuilderPiecesInto(JSStringBuilderState* state, std::span<CharacterType> buffer) const
{
    size_t position = 0;
    for (SBChunk* chunk = state->head; chunk; chunk = chunk->next) {
        for (unsigned i = 0; i < chunk->count; ++i) {
            JSString* piece = chunk->slots[i];
            unsigned pieceLength = piece->length();
            auto destination = buffer.subspan(position, pieceLength);
            if (!piece->isRope()) {
                StringView view = *piece->valueInternal().impl();
                view.getCharacters(destination);
            } else {
                // Resolve the rope piece straight into its slice; this copies characters without
                // allocating a per-piece StringImpl, matching how a normal rope resolves its tree.
                JSRopeString::resolveToBufferSlow<CharacterType>(piece, nullptr, nullptr, destination, nullptr);
            }
            position += pieceLength;
        }
    }
}

void JSRopeString::materializeStringBuilder()
{
    ASSERT(isStringBuilderRope());
    SBState* state = sbStateFromFiber1(fiber1());
    unsigned length = this->length();
    String result;
    if (!length)
        result = emptyString();
    else if (is8Bit()) {
        std::span<Latin1Character> buffer;
        auto impl = StringImpl::createUninitialized(length, buffer);
        copyStringBuilderPiecesInto<Latin1Character>(state, buffer);
        result = WTF::move(impl);
    } else {
        std::span<char16_t> buffer;
        auto impl = StringImpl::createUninitialized(length, buffer);
        copyStringBuilderPiecesInto<char16_t>(state, buffer);
        result = WTF::move(impl);
    }
    sbFree(state);
    convertToNonRope(WTF::move(result)); // now a normal flat string; isRope and the marker cleared
}

void JSRopeString::freeStringBuilderBuffer()
{
    ASSERT(isStringBuilderRope());
    sbFree(sbStateFromFiber1(fiber1()));
}

const String& JSRopeString::resolveRopeWithoutGC() const
{
    if (g_ropeTraceEnabled) recordResolve(this, KindPlain);
    if (isStringBuilderRope()) {
        const_cast<JSRopeString*>(this)->materializeStringBuilder();
        return valueInternal();
    }
    constexpr bool reportAllocation = false;
    return resolveRopeWithFunction<reportAllocation>(nullptr, [] (Ref<StringImpl>&& newImpl) {
        return WTF::move(newImpl);
    });
}

void JSRopeString::outOfMemory(JSGlobalObject* nullOrGlobalObjectForOOM) const
{
    ASSERT(isRope());
    if (nullOrGlobalObjectForOOM) {
        VM& vm = nullOrGlobalObjectForOOM->vm();
        auto scope = DECLARE_THROW_SCOPE(vm);
        throwOutOfMemoryError(nullOrGlobalObjectForOOM, scope);
    }
}

JSValue JSString::toPrimitive(JSGlobalObject*, PreferredPrimitiveType) const
{
    return const_cast<JSString*>(this);
}

double JSString::toNumber(JSGlobalObject* globalObject) const
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto view = this->view(globalObject);
    RETURN_IF_EXCEPTION(scope, 0);
    return jsToNumber(view);
}

inline StringObject* StringObject::create(VM& vm, JSGlobalObject* globalObject, JSString* string)
{
    StringObject* object = new (NotNull, allocateCell<StringObject>(vm)) StringObject(vm, globalObject->stringObjectStructure());
    object->finishCreation(vm, string);
    return object;
}

JSObject* JSString::toObject(JSGlobalObject* globalObject) const
{
    return StringObject::create(globalObject->vm(), globalObject, const_cast<JSString*>(this));
}

bool JSString::getStringPropertyDescriptor(JSGlobalObject* globalObject, PropertyName propertyName, PropertyDescriptor& descriptor)
{
    VM& vm = globalObject->vm();
    if (propertyName == vm.propertyNames->length) {
        descriptor.setDescriptor(jsNumber(length()), PropertyAttribute::DontEnum | PropertyAttribute::DontDelete | PropertyAttribute::ReadOnly);
        return true;
    }
    
    std::optional<uint32_t> index = parseIndex(propertyName);
    if (index && index.value() < length()) {
        descriptor.setDescriptor(getIndex(globalObject, index.value()), PropertyAttribute::DontDelete | PropertyAttribute::ReadOnly);
        return true;
    }
    
    return false;
}

GCOwnedDataScope<const String&> JSString::tryGetValueWithoutGC() const
{
    if (isRope()) {
        // Pass nullptr for the JSGlobalObject so that resolveRope does not throw in the event of an OOM error.
        return { this, static_cast<const JSRopeString*>(this)->resolveRopeWithoutGC() };
    }
    return { this, valueInternal() };
}

JSString* jsStringWithCacheSlowCase(VM& vm, StringImpl& stringImpl)
{
    ASSERT(stringImpl.length() > 1 || (stringImpl.length() == 1 && stringImpl[0] > maxSingleCharacterString));
    JSString* string = JSString::create(vm, stringImpl);
    vm.lastCachedString.setWithoutWriteBarrier(string);
    return string;
}

} // namespace JSC

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
