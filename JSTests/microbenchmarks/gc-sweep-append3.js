// StringBuilder-fusion microbenchmark: accumulator whose op is a single 3-input concat
// (waveBody += px + py), which the prototype fuses to ONE atomic operationStringBuilderAppend3
// per iteration -- the px,py leaves are appended into a native builder with NO piece rope and NO
// per-iteration accumulator rope. Final string atomized (setAttribute-like). Prints METRIC.
var ROUNDS = 40, WAVES = 40, POINTS = 2500;
function tok(x){ x=(x*2654435761)>>>0; return ""+(x%100000); }
noInline(tok);
function buildPath(seed){
    var waveBody = "";
    for (var i = 0; i < POINTS; i++) {
        var px = tok(seed + i * 2);
        var py = tok(seed * 3 + i);
        waveBody += px + py;          // MakeRope(waveBody, px, py) -> fused append3
    }
    return "M" + waveBody + "Z";
}
noInline(buildPath);
var sink = 0, attrs = {};
function frame(r){
    for (var w = 0; w < WAVES; w++) {
        var d = buildPath((r<<8) ^ (w*977+1));
        attrs[d] = w;                 // atomize the value like setAttribute
        sink += d.length;
    }
    attrs = {};
}
noInline(frame);
var t0=preciseTime(), churn=0, gct=0;
for (var r=0;r<ROUNDS;r++){ var a=preciseTime(); frame(r); var b=preciseTime(); fullGC(); var c=preciseTime(); churn+=(b-a); gct+=(c-b); }
print("sink="+sink);
print("METRIC totalMs="+((preciseTime()-t0)*1000).toFixed(1)+" churnMs="+(churn*1000).toFixed(1)+" gcMs="+(gct*1000).toFixed(1));
