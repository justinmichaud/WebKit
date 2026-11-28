//@ $skipModes << :lockdown
//@ requireOptions("--useJSPI=1")
// One JSPI suspend and resume per iteration, with nothing else in the loop.
// Source in wasm-jspi-suspend-resume.wat.

// How many promising calls are suspended at once.
// 1: one call suspends and resumes by itself. Nothing else needs the stack
//    memory between its suspend and its resume, so its frames could stay on
//    the stack (copy-on-write).
// 2: two calls take turns on the same machine stack. Each one overwrites the
//    other's frames, so every resume needs a real copy.
const STACKS = 1;

const TOTAL_SUSPENDS = 100000;

var wasm_code;
try {
    wasm_code = read('wasm-jspi-suspend-resume.wasm', 'binary')
} catch {
    try {
        wasm_code = read('../../JSTests/microbenchmarks/wasm-jspi-suspend-resume.wasm', 'binary')
    } catch {
        wasm_code = read('JSTests/microbenchmarks/wasm-jspi-suspend-resume.wasm', 'binary')
    }
}

// The wrapped function returns a plain value: JSPI suspends anyway, so the
// loop measures only the suspend/resume machinery.
const suspend = new WebAssembly.Suspending(x => x);
const instance = new WebAssembly.Instance(new WebAssembly.Module(wasm_code), { env: { suspend } });
const run = WebAssembly.promising(instance.exports.run);

const n = TOTAL_SUSPENDS / STACKS;
const results = [];
let error = null;
for (let i = 0; i < STACKS; ++i)
    run(n).then(result => results.push(result), e => error = e);
drainMicrotasks();

if (error)
    throw error;
if (results.length !== STACKS)
    throw new Error("expected " + STACKS + " results, got " + results.length);
const expected = (n * (n - 1) / 2) | 0;
for (const result of results) {
    if (result !== expected)
        throw new Error("bad result: " + result + ", expected " + expected);
}
