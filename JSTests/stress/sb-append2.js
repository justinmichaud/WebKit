//@ runDefault
// StringBuilder correctness: two-piece accumulator (V += a + b), routed through the 3-input append.
// build() is the accumulator under test; ref() computes the same string via array join (no builder).
// They must be identical across every tier, and the fiber-walking string fast paths (indexOf/slice/
// charAt/startsWith/endsWith) must work on the built (possibly deferred-builder) rope.

function build(n) {
    var out = "";
    for (var i = 0; i < n; i++) { out += ("k" + i) + ("v" + (i*3)); }
    return out;
}
noInline(build);
function ref(n) {
    var a = [];
    for (var i = 0; i < n; i++) { a.push("k" + i, "v" + (i*3)); }
    return a.join("");
}
noInline(ref);
function check(msg, x, y) { if (x !== y) throw new Error("append2: " + msg + " got " + JSON.stringify(x) + " want " + JSON.stringify(y)); }
for (var iter = 0; iter < testLoopCount; iter++) {
    var n = 1 + (iter % 400);
    var b = build(n), r = ref(n);
    check("value", b, r);
    check("length", b.length, r.length);
    check("indexOf", b.indexOf("1"), r.indexOf("1"));
    check("lastIndexOf", b.lastIndexOf("2"), r.lastIndexOf("2"));
    if (r.length > 6) {
        check("slice", b.slice(3, r.length - 2), r.slice(3, r.length - 2));
        check("charAt", b.charAt(r.length >> 1), r.charAt(r.length >> 1));
    }
    check("startsWith", b.startsWith(r.slice(0, 3)), true);
    check("endsWith", b.endsWith(r.slice(-2)), true);
    check("replace", b.replace("x", "Q"), r.replace("x", "Q"));
}
