// StringBuilder throughput microbenchmark (noregress): tight loop of very-short accumulation, no GC (the inline-short-rope residual case).
// Builds and consumes the string many times with NO forced GC -- measures append/build speed.
// A/B by comparing SB_DISABLE=1 (phase off) vs default (phase on). Prints METRIC.

function build(n) {
    var out = "";
    for (var i = 0; i < n; i++) { out += "x"; }
    return out;
}
noInline(build);
var sink = 0;
var t0 = preciseTime();
for (var r = 0; r < 200000; r++)
    sink += build(8).length;
print("sink=" + sink);
print("METRIC totalMs=" + ((preciseTime() - t0) * 1000).toFixed(1));
