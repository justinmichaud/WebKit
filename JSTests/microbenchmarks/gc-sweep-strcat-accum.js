// Gap #1: op_strcat / DFG StrCat local accumulator, modeled on JetStream3 Basic/simulate
//   result += next.value.string + "\n";
// `V += piece + const` is one op_strcat(V, piece, const). When `piece` is a property access (type
// not provable at compile time) the DFG keeps a StrCat node (operands need ToString) rather than
// converting to MakeRope, so DFGStringBuilderPhase (MakeRope-only) misses it -- yet it is a genuine
// loop-carried accumulator whose intermediates are ~never resolved (pure sweep churn). Prints METRIC.
var ROUNDS = 120, WAVES = 400, POINTS = 60;
function makeTok(x){ x = (x * 2654435761) >>> 0; return { s: "" + (x % 100000) }; }
noInline(makeTok);
function buildPath(seed){
    var result = "";
    for (var i = 0; i < POINTS; i++) {
        var o = makeTok(seed + i * 2);
        result += o.s + ",";      // op_strcat(result, o.s, ",") ; o.s is a property -> StrCat
    }
    return "M" + result + "Z";
}
noInline(buildPath);
var sink = 0, attrs = {};
function frame(r){
    for (var w = 0; w < WAVES; w++) {
        var d = buildPath((r << 8) ^ (w * 977 + 1));
        attrs[d] = w;             // atomize the value like setAttribute
        sink += d.length;
    }
    attrs = {};
}
noInline(frame);
var t0 = preciseTime(), churn = 0, gct = 0;
for (var r = 0; r < ROUNDS; r++) { var a = preciseTime(); frame(r); var b = preciseTime(); fullGC(); var c = preciseTime(); churn += (b-a); gct += (c-b); }
print("sink=" + sink);
print("METRIC totalMs=" + ((preciseTime()-t0)*1000).toFixed(1) + " churnMs=" + (churn*1000).toFixed(1) + " gcMs=" + (gct*1000).toFixed(1));
