// StringBuilder throughput microbenchmark (noregress): MANY short single-site accumulations (each well under the promote threshold).
// Builds and consumes the string many times with NO forced GC -- measures append/build speed.
// A/B by comparing SB_DISABLE=1 (phase off) vs default (phase on). Prints METRIC.

function build(n) {
    var out = "";
    for (var i = 0; i < n; i++) { out += "x" + (i & 9); }
    return out;
}
noInline(build);
var sink = 0;
var t0 = preciseTime();
for (var r = 0; r < 120000; r++)
    sink += build(6).length;
print("sink=" + sink);
print("METRIC totalMs=" + ((preciseTime() - t0) * 1000).toFixed(1));
