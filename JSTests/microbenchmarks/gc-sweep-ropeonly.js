// Sweep-isolation bench: the ONLY per-iteration allocation is the accumulator rope (pieces p,q are
// reused params -> no per-iteration flat cells). So the swept population is ~purely accumulator
// ropes -- the fusion (append3 into a native builder) should eliminate them and cut sweep.
var ROUNDS = 60, WAVES = 60, POINTS = 1500;
function build(seed, p, q) {
    var s = "";
    for (var i = 0; i < POINTS; i++)
        s += p + q;
    return "M" + s + "Z";
}
noInline(build);
var sink = 0, attrs = {};
function frame(r) {
    for (var w = 0; w < WAVES; w++) {
        var d = build((r<<8)^w, "ab", "cd");
        attrs[d] = w;
        sink += d.length;
    }
    attrs = {};
}
noInline(frame);
var t0=preciseTime(), churn=0, gct=0;
for (var r=0;r<ROUNDS;r++){ var a=preciseTime(); frame(r); var b=preciseTime(); fullGC(); var c=preciseTime(); churn+=(b-a); gct+=(c-b); }
print("sink="+sink);
print("METRIC totalMs="+((preciseTime()-t0)*1000).toFixed(1)+" churnMs="+(churn*1000).toFixed(1)+" gcMs="+(gct*1000).toFixed(1));
