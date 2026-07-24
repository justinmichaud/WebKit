// StringBuilder sweep microbenchmark (win): single-piece accumulator over a long run (V += token), base64-encode shape, one site.
// Each round builds the string, drops it, and forces a full GC. Run with JSC_sweepSynchronously=1 so
// gcMs is dominated by sweep -- measures the GC-sweep cost of the rope churn. Prints METRIC.
var tab=[]; for (var t=0;t<64;t++) tab.push(String.fromCharCode(65 + (t & 31)));
function build(n) {
    var out = "";
    for (var i = 0; i < n; i++) { out += tab[i & 63]; }
    return out;
}
noInline(build);
var sink = 0, churn = 0, gct = 0;
function frame() {
    for (var w = 0; w < 4; w++) sink += build(6000).length;
}
noInline(frame);
var t0 = preciseTime();
for (var r = 0; r < 160; r++) {
    var a = preciseTime(); frame(); var b = preciseTime(); fullGC(); var c = preciseTime();
    churn += (b - a); gct += (c - b);
}
print("sink=" + sink);
print("METRIC totalMs=" + ((preciseTime() - t0) * 1000).toFixed(1) +
      " churnMs=" + (churn * 1000).toFixed(1) + " gcMs=" + (gct * 1000).toFixed(1));
