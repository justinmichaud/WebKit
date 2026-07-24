// Sweep-bound GC microbenchmark mirroring the dominant rope-churn pattern in JetStream3
// SunSpider/tagcloud.js:250 -- a loop-carried string ACCUMULATOR whose RHS is a MANY-piece
// (>3) concatenation of string literals and variables:
//
//     output += ' <a href="' + url + '" style="font-size: ' + popularity
//             + 'px; color: ' + color + '">' + tag + '</a> \n';
//
// This is the ">3-piece RHS" shape. The accumulator MakeRope(output, pieceTree) is recognized
// by the StringBuilder phase, but the pieceTree itself is ~3-4 intermediate MakeRope cells that
// Fixup grouped from the RHS -- those are still allocated every iteration. The target of the
// extension is to flatten the whole flat-leaf pieceTree into ONE variadic append so ZERO piece
// ropes are allocated.
//
// Prints METRIC lines. For the sweep metric run with JSC_sweepSynchronously=1 (gcMs ~= sweep).

var ROUNDS = 200;
var TAGS   = 400;

// Pre-stringify like the app: operands reaching the accumulator are already strings.
function mkTag(i)  { return "tag" + (i & 63); }
function mkUrl(i)  { return "/t/" + (i * 7 & 255); }
function mkColor(i){ return "#" + ((i * 2654435761) & 0xffffff).toString(16); }
noInline(mkTag); noInline(mkUrl); noInline(mkColor);

// Mirror of the tagcloud output loop: a >3-piece RHS accumulator.
function buildCloud(seed) {
    var output = "";
    for (var i = 0; i < TAGS; i++) {
        var tag = mkTag(seed + i);
        var url = mkUrl(seed + i);
        var color = mkColor(seed + i);
        var popularity = ((seed + i) * 131 & 31) + 8 + "";
        output += ' <a href="' + url + '" style="font-size: ' + popularity + 'px; color: ' + color + '">' + tag + '</a> \n';
    }
    return output;
}
noInline(buildCloud);

var sink = 0;
var held = {};

function frame(r) {
    var d = buildCloud(r * 1009 + 1);
    held[d] = r;      // force resolve of the final accumulated string (like innerHTML assign)
    sink += d.length;
    held = {};        // drop it so the churn is swept
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
