// Gap #2 (NewObject variant): property/field accumulator whose base is a *fresh* object literal.
// `p.buf += piece;` -> PutByOffset(base, MakeRope(GetByOffset(base, buf), piece)) where base is a
// NewObject (object literal). Unlike gc-sweep-prop-accum.js (which uses noInline(Path) -> the
// constructed object arrives via a Construct call, NOT a NewObject, so the field guard correctly
// will NOT fire), here `var p = { buf: "" }` compiles to NewObject, so the field-accumulator guard
// is expected to FIRE. Prints METRIC.
var ROUNDS = 120, WAVES = 400, POINTS = 60;
function tok(x){ x = (x * 2654435761) >>> 0; return "" + (x % 100000); }
noInline(tok);
function buildPath(seed){
    var p = { buf: "" };          // NewObject -> field-accumulator guard should fire
    for (var i = 0; i < POINTS; i++) {
        var px = tok(seed + i * 2);
        p.buf += px;              // PutByOffset(base, MakeRope(GetByOffset(base, buf), px))
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
