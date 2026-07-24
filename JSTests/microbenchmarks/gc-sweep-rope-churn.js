// Sweep-bound GC microbenchmark. Mirrors a real WebKit workload whose GC time was dominated
// by *sweeping* (destroying short-lived cells), not marking: each round allocates many
// short-lived JSRopeStrings (lazy string concatenations, the top sweep cost in that trace)
// plus small objects, retains almost nothing, then forces a full GC. Marking stays cheap
// (near-empty live set) so most GC time is the destructor sweep -- the path a sweep-side
// optimization (block prefetch, fewer stopped-world atomics) would move.
//
// Reading r.length touches the rope without resolving it, so it stays an unresolved
// JSRopeString and is destroyed during the sweep rather than collapsed to a flat string.

function churn(n, tag) {
    var acc = 0;
    for (var i = 0; i < n; i++) {
        var r = tag + i + "-" + (i ^ 0x5a5a);
        var o = { k: i, s: r, n: i * 3 };
        acc += o.k + r.length;
    }
    return acc;
}
noInline(churn);

var acc = 0;
for (var iter = 0; iter < 200; iter++) {
    acc += churn(60000, "s" + (iter & 7) + "-");
    fullGC();
}
print(acc);
