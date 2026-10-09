// Recomputes every verdict from the dumps with analysis.js, as index.html does, and
// compares each with the one the C++ test wrote. Exits non-zero if any differs.
// Usage: node check.js <leaks.json>
const fs = require('fs');
const path = require('path');
const A = require(path.join(__dirname, 'analysis.js'));
const report = A.loadReport(JSON.parse(fs.readFileSync(process.argv[2])));
let agree = 0;
let total = 0;
report.demos.forEach((demo, index) => report.walks.forEach((walk, walkIndex) => {
    const seen = A.whyNotSeen(walk, index, demo) ?? 'found';
    const find = A.findLeak(walk, report.demos, index);
    const found = find.verdict ?? 'found';
    for (const [js, cpp, what] of [[seen, demo.verdicts[2 * walkIndex], 'charges'], [found, demo.verdicts[2 * walkIndex + 1], 'finds']]) {
        ++total;
        if (js === cpp)
            ++agree;
        else
            console.log(`DIFFERS: ${walk.name}: ${demo.page}: ${what}\n  JS:  ${js}\n  C++: ${cpp}`);
    }
    console.log(`${walk.name.padEnd(20)} ${demo.page}  charges: ${seen === 'found' ? 'yes' : 'no '}  finds: ${find.stage}${find.stage === "found" ? " by " + A.describeKey(walk.keys, find.entry.key) + " #" + (walk.entries.indexOf(find.entry) + 1) + " of " + walk.entries.length : ""}`);
}));
console.log(`${agree} of ${total} verdicts agree with the C++ test's`);
process.exit(agree === total ? 0 : 1);
