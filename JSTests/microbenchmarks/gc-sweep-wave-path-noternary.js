// Sweep-bound GC microbenchmark that mirrors the dominant rope-churn pattern found in the
// "Synergy UI" app at localhost:8080. Creation-site instrumentation showed ~17M short-lived
// JSRopeStrings come from ONE site: a string ACCUMULATOR loop in WaveRenderingService's
// createFilledPath:
//
//     waveBody += hasPoint ? ` ${x2},${y2}` : `${x2},${y2}`;   // millions of intermediates, ~0% resolved
//     ...
//     return `M${firstX},${baseline} L${waveBody} L${lastX},${baseline} Z`;  // final `d`, 100% atomized
//
// The returned `d` value is handed to element.setAttribute("d", ...), whose binding resolves it
// to an AtomString (the "consumed by C++" string). The per-iteration `+=` builds a left-leaning
// rope tree whose interior nodes are never resolved on their own -- they are only read (via
// resolveToBuffer) when the final template string is materialized. This benchmark reproduces
// that exactly, then drops all references and forces a full GC so the churn is swept.
//
// This is the target pattern for a StringBuilder optimization: recognize the loop-carried
// `V = V + x` accumulator and build it natively (no per-iteration JS rope cells).
//
// Prints METRIC lines. For the sweep metric run with JSC_sweepSynchronously=1 (gcMs ~= sweep).

var ROUNDS      = 160;   // "frames"
var WAVES       = 8;     // waves per frame
var POINTS      = 600;   // samples per wave  -> WAVES*POINTS `+=` intermediates per frame

// Pre-stringify coordinates (like WaveRenderingService.toPathNumber): the accumulator's operands
// are already strings, matching the app (operand classification showed num=0).
function toPathNumber(v) {
    // round to 2 decimals, return a string
    return ((v * 100 | 0) / 100) + "";
}
noInline(toPathNumber);

// Mirror of createFilledPath: the `waveBody +=` accumulator + final template assembly.
function createFilledPath(seed, baselineOffset) {
    var waveBody = "";
    var hasPoint = false;
    var firstX = null, lastX = null;
    for (var i = 0; i < POINTS; i++) {
        // deterministic pseudo-wave coordinates
        var t = (seed * 131 + i * 7) & 1023;
        var x2 = toPathNumber(i * 1.5 + (t & 15));
        var y2 = toPathNumber(64 + ((t * 9) & 127) * 0.5);
        if (firstX === null)
            firstX = x2;
        lastX = x2;
        waveBody += " " + x2 + "," + y2;
        hasPoint = true;
    }
    if (firstX === null || lastX === null || !hasPoint)
        return "";
    var baseline = toPathNumber(baselineOffset);
    return "M" + firstX + "," + baseline + " L" + waveBody + " L" + lastX + "," + baseline + " Z";
}
noInline(createFilledPath);

var sink = 0;
var attrs = { };  // stands in for the DOM holding attribute AtomStrings briefly

function frame(r) {
    for (var w = 0; w < WAVES; w++) {
        var d = createFilledPath((r << 8) ^ (w * 977 + 1), w * 40 + 20);
        attrs[d] = w;   // rope used as property key -> resolveRopeToAtomString, like setAttribute's value binding
        sink += d.length;
    }
    attrs = { };  // drop last frame's attribute strings
}
noInline(frame);

var t0 = preciseTime();
var churn = 0, gct = 0;
for (var r = 0; r < ROUNDS; r++) {
    var a = preciseTime();
    frame(r);
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
