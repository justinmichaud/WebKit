// StringBuilder throughput microbenchmark (noregress): multiple accumulation sites on the same local (V += a; V += b; V += c) -- gated off.
// Builds and consumes the string many times with NO forced GC -- measures append/build speed.
// A/B by comparing SB_DISABLE=1 (phase off) vs default (phase on). Prints METRIC.

function build(n) {
    var out = "";
    for (var i = 0; i < n; i++) { out += "a" + i; out += "b" + (i*2); out += "c" + (i & 3); }
    return out;
}
noInline(build);
var sink = 0;
var t0 = preciseTime();
for (var r = 0; r < 1600; r++)
    sink += build(200).length;
print("sink=" + sink);
print("METRIC totalMs=" + ((preciseTime() - t0) * 1000).toFixed(1));
