// StringBuilder throughput microbenchmark (win): single-piece accumulator over a long run (V += token), base64-encode shape, one site.
// Builds and consumes the string many times with NO forced GC -- measures append/build speed.
// A/B by comparing SB_DISABLE=1 (phase off) vs default (phase on). Prints METRIC.
var tab=[]; for (var t=0;t<64;t++) tab.push(String.fromCharCode(65 + (t & 31)));
function build(n) {
    var out = "";
    for (var i = 0; i < n; i++) { out += tab[i & 63]; }
    return out;
}
noInline(build);
var sink = 0;
var t0 = preciseTime();
for (var r = 0; r < 400; r++)
    sink += build(6000).length;
print("sink=" + sink);
print("METRIC totalMs=" + ((preciseTime() - t0) * 1000).toFixed(1));
