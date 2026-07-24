// StringBuilder sweep microbenchmark (win): >3-piece op_strcat accumulator (V += a + b + c + d + ...), the tagcloud shape.
// Each round builds the string, drops it, and forces a full GC. Run with JSC_sweepSynchronously=1 so
// gcMs is dominated by sweep -- measures the GC-sweep cost of the rope churn. Prints METRIC.

function build(n) {
    var out = "";
    for (var i = 0; i < n; i++) { out += "<a n=" + i + " k=" + (i*3) + " c=" + (i&15) + ">" + ("t" + (i&7)) + "</a> "; }
    return out;
}
noInline(build);
var sink = 0, churn = 0, gct = 0;
function frame() {
    for (var w = 0; w < 8; w++) sink += build(200).length;
}
noInline(frame);
var t0 = preciseTime();
for (var r = 0; r < 200; r++) {
    var a = preciseTime(); frame(); var b = preciseTime(); fullGC(); var c = preciseTime();
    churn += (b - a); gct += (c - b);
}
print("sink=" + sink);
print("METRIC totalMs=" + ((preciseTime() - t0) * 1000).toFixed(1) +
      " churnMs=" + (churn * 1000).toFixed(1) + " gcMs=" + (gct * 1000).toFixed(1));
