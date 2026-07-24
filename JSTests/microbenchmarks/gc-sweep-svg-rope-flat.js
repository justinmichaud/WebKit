// CEILING VARIANT of gc-sweep-svg-rope.js. Identical resolved-string workload (same counts,
// lengths, kinds: 77% plain / 23% atomized, same tokens) but the long strings are built with
// Array.prototype.join instead of a chain of `+`. join materializes ONE result string per build
// with NO intermediate JSRopeString cells, whereas the `+`-chain leaves ~N unresolved rope cells
// per build. So this isolates exactly the "reduce rope construction" lever the user's
// C++-access-optimized string impl would pull (build the attribute string directly, no rope
// chain). The delta in sweep time between this and gc-sweep-svg-rope.js bounds the achievable
// win. (This is a JS-level proxy for the ceiling, NOT the real impl -- the tokens are still
// separate cells here; a true C++ buffer would remove those too.)
//
// Sweep-bound GC microbenchmark modeled on a real WebKit workload (the "Synergy UI" SVG app
// at localhost:8080), whose GC time is dominated by *sweeping* short-lived cells.
//
// Instrumenting JSRopeString construction/resolution/destruction in that app (RTRACE build)
// showed the string layer behaves as follows, which this benchmark reproduces:
//
//   * ~98% of all JSRopeStrings are transient concatenation INTERMEDIATES that are never
//     resolved -- their destructor is a no-op, so sweep cost is the sheer VOLUME of rope
//     cells scanned/destroyed per block.
//   * ~2% get resolved (materialize a StringImpl). Of those:
//       - ~23% are resolved to an AtomString by the DOM binding for element.setAttribute()
//         (WebCore Converter<IDLAtomStringAdaptor<IDLDOMString>>) -- these are large SVG path
//         "d"/points/transform attribute VALUES built by JS string concatenation. avgLen ~3KB,
//         heavy tail to 16KB. This is the "consumed by C++" population.
//       - ~77% are resolved to a plain String by JS-internal string ops (toLowerCase, endsWith,
//         split, Map keys, comparisons, property keys). avgLen ~544, bimodal (peak ~32 chars,
//         tail to 16KB).
//   * Retains almost nothing round-to-round; a full GC each round sweeps the churn.
//
// The benchmark builds strings via chained concatenation (each build leaves a chain of
// unresolved intermediate ropes + one final), then resolves the final either by atomizing it
// (property key -> resolveRopeToAtomString, the setAttribute/AtomString path) or by a plain
// character read (charCodeAt -> resolveRope, the JS-op path), matching the measured mix, then
// drops all references and forces a full GC. Marking stays cheap (near-empty live set), so most
// GC time is the destructor sweep -- the path a rope-storage change (e.g. a C++-access-optimized
// string impl replacing JSRopeString at these sites) would move.
//
// Prints METRIC lines for the A/B harness: churn vs gc vs total wall time (ms).
//
// USAGE for the "sweeps faster" A/B: run with JSC_sweepSynchronously=1 so the sweep runs inside
// each GC call (otherwise it is lazy and folds into churn); then METRIC gcMs ~= mark+sweep and,
// because the live set is near-empty (marking is cheap), ~= sweep time. Whole-process wall time
// (totalMs) captures the sweep either way. For a phase-isolated sweep number, profile under
// samply with the GC text markers and split with the jsc-marker-trace tooling.

// -------- tunables (fit to app RTRACE target: 98.4% unresolved; resolved 77% plain / 23% atom;
//          avgLen plain ~544, atom ~3095) --------
var ROUNDS         = 120;
var ATOM_BUILDS    = 480;   // DOM/SVG attribute-value population (atomized, large)
var PLAIN_SMALL    = 1500;  // JS-op population, short (peak ~32 chars)
var PLAIN_LARGE    = 110;   // JS-op population, large tail (4-16KB)
var ATOM_SEGMENTS  = 150;   // -> ~3KB path-like string (jittered)
var SMALL_SEGMENTS = 2;     // -> ~30 chars (jittered up into 8..256 buckets)
var LARGE_SEGMENTS = 380;   // -> ~5KB (jittered)

// pseudo-varied numeric token (no Math.random: keep deterministic & JIT-stable)
function tok(x) {
    x = (x * 2654435761) >>> 0;
    return "" + (x % 100000);
}
noInline(tok);

// Deterministic jitter: spread a base segment count across neighbouring log2 length buckets,
// with an occasional heavy multiplier to reproduce the measured long tail.
function jitter(base, seed) {
    var h = (seed * 2246822519) >>> 0;
    var s = base + (h % (base + 1));          // [base, 2*base)
    if ((h & 127) === 0) s = s * 5;           // ~0.8% heavy tail
    else if ((h & 3) === 0) s = (s >> 2) + 1; // ~25% shorter
    return s < 1 ? 1 : s;
}
noInline(jitter);

// Build the SAME SVG-path-like string via Array.join: one materialized result, NO intermediate
// JSRopeString chain (contrast the `+`-chain in gc-sweep-svg-rope.js).
function buildPath(segments, seed) {
    var parts = new Array(segments + 1);
    parts[0] = "M" + tok(seed) + " " + tok(seed + 1);
    for (var k = 0; k < segments; k++)
        parts[k + 1] = " L" + tok(seed + k * 2) + " " + tok(seed * 3 + k);
    return parts.join("");
}
noInline(buildPath);

var sink = 0;
var bag = { };   // holds atomized keys briefly; cleared each round (like DOM attr churn)

function round(r) {
    // DOM/SVG attribute values: build long path, atomize via property key (setAttribute path).
    for (var i = 0; i < ATOM_BUILDS; i++) {
        var d = buildPath(jitter(ATOM_SEGMENTS, i + 1), (r << 16) ^ (i * 7 + 1));
        bag[d] = i;            // toPropertyKey -> resolveRopeToAtomString (AtomString)
    }
    // JS-op short strings: build, force plain resolution via charCodeAt.
    for (var j = 0; j < PLAIN_SMALL; j++) {
        var s = buildPath(jitter(SMALL_SEGMENTS, j + 3), (r << 16) ^ (j * 13 + 3));
        sink += s.charCodeAt(0); // resolveRope (plain)
    }
    // JS-op large strings (tail).
    for (var m = 0; m < PLAIN_LARGE; m++) {
        var t = buildPath(jitter(LARGE_SEGMENTS, m + 5), (r << 16) ^ (m * 131 + 5));
        sink += t.charCodeAt(1); // resolveRope (plain)
    }
    // drop everything: new bag each round so last round's atoms die too
    bag = { };
}
noInline(round);

var t0 = preciseTime();
var churn = 0, gct = 0;
for (var r = 0; r < ROUNDS; r++) {
    var a = preciseTime();
    round(r);
    var b = preciseTime();
    fullGC();
    var c = preciseTime();
    churn += (b - a);
    gct += (c - b);
}
var total = preciseTime() - t0;
print("sink=" + sink);
print("METRIC totalMs=" + (total * 1000).toFixed(1) +
      " churnMs=" + (churn * 1000).toFixed(1) +
      " gcMs=" + (gct * 1000).toFixed(1));
