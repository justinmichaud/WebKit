// StringBuilder throughput microbenchmark (win): >3-piece op_strcat accumulator (V += a + b + c + d + ...), the tagcloud shape.
// Builds and consumes the string many times with NO forced GC -- measures append/build speed.
// A/B by comparing SB_DISABLE=1 (phase off) vs default (phase on). Prints METRIC.

function build(n) {
    var out = "";
    for (var i = 0; i < n; i++) { out += "<a n=" + i + " k=" + (i*3) + " c=" + (i&15) + ">" + ("t" + (i&7)) + "</a> "; }
    return out;
}
noInline(build);
var sink = 0;
var t0 = preciseTime();
for (var r = 0; r < 1600; r++)
    sink += build(200).length;
print("sink=" + sink);
print("METRIC totalMs=" + ((preciseTime() - t0) * 1000).toFixed(1));
