// Gap #2: property/field accumulator, modeled on JetStream3 Basic/lexer (`token.value += char`)
//   b.buf += piece;   ->  PutById(b, MakeRope(GetById(b, buf), piece))
// DFGStringBuilderPhase matches only LOCAL accumulators (SetLocal of MakeRope(GetLocal(V), ...)), so
// an accumulator stored in an object field is missed even though the intermediates are ~never
// resolved. Prints METRIC.
var ROUNDS = 120, WAVES = 400, POINTS = 60;
function tok(x){ x = (x * 2654435761) >>> 0; return "" + (x % 100000); }
noInline(tok);
function Path(){ this.buf = ""; }
noInline(Path);
function buildPath(seed){
    var p = new Path();
    for (var i = 0; i < POINTS; i++) {
        var px = tok(seed + i * 2);
        p.buf += px;              // PutById(MakeRope(GetById(p.buf), px)) -- property accumulator
    }
    return "M" + p.buf + "Z";
}
noInline(buildPath);
var sink = 0, attrs = {};
function frame(r){
    for (var w = 0; w < WAVES; w++) {
        var d = buildPath((r << 8) ^ (w * 977 + 1));
        attrs[d] = w;
        sink += d.length;
    }
    attrs = {};
}
noInline(frame);
var t0 = preciseTime(), churn = 0, gct = 0;
for (var r = 0; r < ROUNDS; r++) { var a = preciseTime(); frame(r); var b = preciseTime(); fullGC(); var c = preciseTime(); churn += (b-a); gct += (c-b); }
print("sink=" + sink);
print("METRIC totalMs=" + ((preciseTime()-t0)*1000).toFixed(1) + " churnMs=" + (churn*1000).toFixed(1) + " gcMs=" + (gct*1000).toFixed(1));
