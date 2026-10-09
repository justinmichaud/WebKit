// Interns the class names and edge labels of the data LeakTest.cpp writes with
// JSC_TOOLS_LEAK_REPORT set, losslessly, so that it fits one artifact file.
// Usage: node compact.js <in.json> <out.json>
const fs = require('fs');
const report = JSON.parse(fs.readFileSync(process.argv[2]));
const strings = [];
const indices = new Map();
const intern = string => {
    let index = indices.get(string);
    if (index === undefined) {
        index = strings.length;
        strings.push(string);
        indices.set(string, index);
    }
    return index;
};
for (const walk of report.walks) {
    for (const [nodes, edges] of [walk.before, walk.after]) {
        for (let i = 0; i < nodes.length; i += 5) {
            nodes[i + 2] = intern(nodes[i + 2]);
            nodes[i + 4] = nodes[i + 4] ? 1 : 0;
        }
        for (let i = 0; i < edges.length; i += 5) {
            edges[i + 3] = edges[i + 3] ? 1 : 0;
            edges[i + 4] = intern(edges[i + 4]);
        }
    }
}
report.strings = strings;
fs.writeFileSync(process.argv[3], JSON.stringify(report));
