//@ runDefault
// StringBuilder correctness: MANY short single-site accumulations (each well under the promote threshold).
// build() is the accumulator under test; ref() computes the same string via array join (no builder).
// They must be identical across every tier, and the fiber-walking string fast paths (indexOf/slice/
// charAt/startsWith/endsWith) must work on the built (possibly deferred-builder) rope.

function build(n) {
    var out = "";
    for (var i = 0; i < n; i++) { out += "x" + (i & 9); }
    return out;
}
noInline(build);
function ref(n) {
    var a = [];
    for (var i = 0; i < n; i++) { a.push("x" + (i & 9)); }
    return a.join("");
}
noInline(ref);
function check(msg, x, y) { if (x !== y) throw new Error("short-many: " + msg + " got " + JSON.stringify(x) + " want " + JSON.stringify(y)); }
for (var iter = 0; iter < testLoopCount; iter++) {
    var n = 1 + (iter % 6);
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
