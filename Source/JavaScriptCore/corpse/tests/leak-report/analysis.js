// Reads the dumps LeakTest.cpp writes (JSC_TOOLS_LEAK_REPORT) and recomputes, on its own,
// everything the report shows: dominators, retained sizes, growth, and the verdicts.
// Runs in a browser (index.html) and in node (check.js, against the C++ test's verdicts).

const Kind = ['Root', 'Cell', 'Allocation', 'Missed'];
const EdgeKind = ['untraced', 'weak', 'strong'];

class Dump {
    // `strings` is the table compact.js interned class names and labels into, if it did.
    constructor([flatNodes, flatEdges], strings) {
        const str = v => strings && typeof v === 'number' ? strings[v] : v;
        const n = flatNodes.length / 5;
        this.address = new Float64Array(n);
        this.size = new Float64Array(n);
        this.className = new Array(n);
        this.kind = new Uint8Array(n);
        this.isBlock = new Uint8Array(n);
        for (let i = 0; i < n; ++i) {
            this.address[i] = flatNodes[5 * i];
            this.size[i] = flatNodes[5 * i + 1];
            this.className[i] = str(flatNodes[5 * i + 2]);
            this.kind[i] = flatNodes[5 * i + 3];
            this.isBlock[i] = flatNodes[5 * i + 4] ? 1 : 0;
        }
        const m = flatEdges.length / 5;
        this.edges = new Array(m);
        this.out = Array.from({ length: n }, () => []);
        this.in = Array.from({ length: n }, () => []);
        for (let i = 0; i < m; ++i) {
            const e = { from: flatEdges[5 * i], to: flatEdges[5 * i + 1], kind: flatEdges[5 * i + 2], retains: !!flatEdges[5 * i + 3], label: str(flatEdges[5 * i + 4]) };
            this.edges[i] = e;
            this.out[e.from].push(e);
            this.in[e.to].push(e);
        }
        this.nodeCount = n;
        this.computeRetention();
        this.byAddress = new Map();
        for (let i = 1; i < n; ++i) {
            if (this.kind[i] !== 0 && this.address[i])
                this.byAddress.set(this.address[i], i);
        }
    }

    // Immediate dominators over the edges that retain, from <root> (node 0), by Cooper, Harvey
    // and Kennedy's iteration; a node no path reaches is <root>'s, as Web Inspector has it.
    computeRetention() {
        const n = this.nodeCount;
        const order = new Int32Array(n).fill(-1); // Reverse postorder number.
        const rpo = [];
        const visited = new Uint8Array(n);
        // Iterative DFS for postorder.
        const stack = [[0, 0]];
        visited[0] = 1;
        const post = [];
        while (stack.length) {
            const top = stack[stack.length - 1];
            const [node, i] = top;
            const out = this.out[node];
            let pushed = false;
            for (let j = i; j < out.length; ++j) {
                const e = out[j];
                if (!e.retains)
                    continue;
                if (!visited[e.to]) {
                    visited[e.to] = 1;
                    top[1] = j + 1;
                    stack.push([e.to, 0]);
                    pushed = true;
                    break;
                }
            }
            if (!pushed) {
                post.push(node);
                stack.pop();
            }
        }
        for (let i = post.length - 1; i >= 0; --i) {
            order[post[i]] = rpo.length;
            rpo.push(post[i]);
        }
        const idom = new Int32Array(n).fill(-1);
        idom[0] = 0;
        const intersect = (a, b) => {
            while (a !== b) {
                while (order[a] > order[b]) a = idom[a];
                while (order[b] > order[a]) b = idom[b];
            }
            return a;
        };
        let changed = true;
        while (changed) {
            changed = false;
            for (let k = 1; k < rpo.length; ++k) {
                const b = rpo[k];
                let newIdom = -1;
                for (const e of this.in[b]) {
                    if (!e.retains || order[e.from] < 0 || idom[e.from] < 0)
                        continue;
                    newIdom = newIdom < 0 ? e.from : intersect(e.from, newIdom);
                }
                if (newIdom >= 0 && idom[b] !== newIdom) {
                    idom[b] = newIdom;
                    changed = true;
                }
            }
        }
        this.reached = visited;
        for (let i = 0; i < n; ++i) {
            if (idom[i] < 0)
                idom[i] = 0;
        }
        this.idom = idom;
        this.children = Array.from({ length: n }, () => []);
        for (let i = 1; i < n; ++i)
            this.children[idom[i]].push(i);
        // Retained sizes, children first.
        const retained = new Float64Array(n);
        const preorder = [];
        const work = [0];
        while (work.length) {
            const node = work.pop();
            preorder.push(node);
            for (const c of this.children[node])
                work.push(c);
        }
        this.preorder = preorder;
        for (let i = preorder.length - 1; i >= 0; --i) {
            const node = preorder[i];
            retained[node] += this.size[node];
            if (node)
                retained[idom[node]] += retained[node];
        }
        this.retained = retained;
        // Depth in the dominator tree, for dominates().
        const depth = new Int32Array(n);
        for (const node of preorder)
            depth[node] = node ? depth[idom[node]] + 1 : 0;
        this.depth = depth;
    }

    dominates(dominator, node) {
        while (this.depth[node] > this.depth[dominator])
            node = this.idom[node];
        return node === dominator;
    }

    // The node whose bytes hold `address`: a node that starts there, or else the one it lies in.
    nodeStartingAt(address) {
        return this.byAddress.get(address);
    }

    dominatorChain(node) {
        const chain = [node];
        while (chain[chain.length - 1] !== 0)
            chain.push(this.idom[chain[chain.length - 1]]);
        return chain.reverse();
    }

    // Each node reached from `start` over every edge, and the node it was first reached from.
    parentsFrom(start) {
        const parents = new Int32Array(this.nodeCount).fill(-1);
        parents[start] = start;
        const queue = [start];
        for (let head = 0; head < queue.length; ++head) {
            const node = queue[head];
            for (const e of this.out[node]) {
                if (parents[e.to] >= 0)
                    continue;
                parents[e.to] = node;
                queue.push(e.to);
            }
        }
        return parents;
    }

    edgeBetween(from, to) {
        return this.out[from].find(e => e.to === to);
    }

    describe(node) {
        return `${this.className[node]}${this.address[node] ? ' at 0x' + this.address[node].toString(16).toUpperCase() : ''}`;
    }
}

// Growth of a node's retained size from the first dump to the second: a node matched by its
// address and class, a root's by its name; a node new in the second grew by all of it.
function growthFunction(before, after) {
    const rootsBefore = new Map();
    for (let i = 1; i < before.nodeCount; ++i) {
        if (before.kind[i] === 0 && !rootsBefore.has(before.className[i]))
            rootsBefore.set(before.className[i], i);
    }
    const growth = new Float64Array(after.nodeCount);
    const nodeBefore = new Int32Array(after.nodeCount).fill(-1);
    nodeBefore[0] = 0;
    for (let i = 1; i < after.nodeCount; ++i) {
        let b;
        if (after.kind[i] === 0)
            b = rootsBefore.get(after.className[i]);
        else if (after.address[i]) {
            const found = before.byAddress.get(after.address[i]);
            if (found !== undefined && before.className[found] === after.className[i])
                b = found;
        }
        growth[i] = b !== undefined ? after.retained[i] - before.retained[b] : after.retained[i];
        nodeBefore[i] = b !== undefined ? b : -1;
    }
    return { growth, nodeBefore };
}

function loadReport(json) {
    const walks = json.walks.map(w => {
        const before = new Dump(w.before, json.strings);
        const after = new Dump(w.after, json.strings);
        const { growth, nodeBefore } = growthFunction(before, after);
        return { name: w.name, hasStorageNodes: w.hasStorageNodes, before, after, growth, nodeBefore, isNew: n => nodeBefore[n] < 0 };
    });
    const demos = json.demos.map(d => {
        const leaked = [];
        for (let i = 0; i < d.leaked.length; i += 3)
            leaked.push({ object: d.leaked[i], storage: d.leaked[i + 1], isAfterFirstSnapshot: d.leaked[i + 2] });
        return { ...d, leaked };
    });
    for (const walk of walks) {
        walk.leaks = leaksIn(walk, demos);
        walk.keys = retainedPathKeys(walk);
        walk.entries = entriesOf(walk, demos, walk.keys);
    }
    return { ...json, walks, demos };
}

// The nodes of each demo's leak in the second dump, the bytes of those leaked between the
// dumps, and for each node the demos whose leaked nodes it dominates, as a mask.
function leaksIn(walk, demos) {
    const graph = walk.after;
    const nodes = demos.map(() => []);
    const between = demos.map(() => []);
    const bytesBetween = demos.map(() => 0);
    const missing = demos.map(() => null);
    const dominatorOf = new Uint32Array(graph.nodeCount);
    demos.forEach((demo, index) => {
        for (const leaked of demo.leaked) {
            for (const address of [leaked.object, walk.hasStorageNodes ? leaked.storage : 0]) {
                if (!address)
                    continue;
                const node = graph.nodeStartingAt(address);
                if (node === undefined || graph.kind[node] === 3) {
                    missing[index] ??= `the leaked 0x${address.toString(16).toUpperCase()} is no node of its own`;
                    continue;
                }
                nodes[index].push(node);
                if (leaked.isAfterFirstSnapshot) {
                    between[index].push(node);
                    bytesBetween[index] += graph.size[node];
                }
                for (let d = node; !(dominatorOf[d] & (1 << index)); d = graph.idom[d]) {
                    dominatorOf[d] |= 1 << index;
                    if (!d)
                        break;
                }
            }
        }
    });
    return { nodes, between, bytesBetween, missing, dominatorOf };
}

// The nodes of each demo's leak in one of a walk's dumps: in the first, what leaked before it.
function leakedNodesIn(graph, demos, hasStorageNodes, isFirst) {
    return demos.map(demo => {
        const nodes = [];
        for (const leaked of demo.leaked) {
            if (isFirst && leaked.isAfterFirstSnapshot)
                continue;
            for (const address of [leaked.object, hasStorageNodes ? leaked.storage : 0]) {
                const node = address ? graph.nodeStartingAt(address) : undefined;
                if (node !== undefined && graph.kind[node] !== 3)
                    nodes.push(node);
            }
        }
        return nodes;
    });
}

const hex = n => '0x' + n.toString(16).toUpperCase();

// LeakTest.cpp's whyNotSeen: the dumps charge the leak to each holder the test recorded.
function whyNotSeen(walk, index, demo) {
    const g = walk.after, leaks = walk.leaks;
    if (leaks.missing[index])
        return leaks.missing[index];
    const fromRoot = g.parentsFrom(0);
    for (const holder of demo.holders) {
        const node = g.nodeStartingAt(holder);
        if (node === undefined)
            return `its holder at ${hex(holder)} is no node`;
        const name = `the ${g.className[node]} at ${hex(holder)}`;
        if (fromRoot[node] < 0)
            return `no path from <root> reaches ${name}`;
        const fromHolder = g.parentsFrom(node);
        for (const leaked of leaks.nodes[index]) {
            if (fromHolder[leaked] < 0) {
                const path = [leaked];
                while (fromRoot[path[path.length - 1]] !== path[path.length - 1])
                    path.push(fromRoot[path[path.length - 1]]);
                const steps = path.reverse().map(n => `\n        ${g.className[n]} at 0x${g.address[n].toString(16).toUpperCase()}`).join('');
                return `${name} reaches no path to the leaked ${g.describe(leaked)}, which <root> reaches so:${steps}`;
            }
        }
        const holderGrowth = walk.growth[node];
        if (holderGrowth < Math.floor(leaks.bytesBetween[index] / 2))
            return `the retained size of ${name} grew by ${holderGrowth} bytes, of the ${leaks.bytesBetween[index]} that leaked between the dumps`;
    }
    return null;
}

// LeakTest.cpp's Graph::isInternal: the allocator's own memory, what no path reaches, and a
// block's handle with what it alone holds.
function blockRecords(g) {
    if (!g.isBlockRecord) {
        g.isBlockRecord = new Uint8Array(g.nodeCount);
        for (const n of g.preorder) {
            if (n)
                g.isBlockRecord[n] = g.isBlockRecord[g.idom[n]] || g.className[n] === 'JSC::MarkedBlock::Handle' ? 1 : 0;
        }
    }
    return g.isBlockRecord;
}
const isInternal = (g, n) => g.kind[n] === 3 || g.isBlock[n] === 1 || !g.reached[n] || blockRecords(g)[n] === 1;

// LeakTest.cpp's noiseRetainedIn: what each node retains that no demo's leak owns.
function noiseRetainedIn(g, leakedNodes) {
    const mask = new Uint32Array(g.nodeCount);
    leakedNodes.forEach((list, index) => {
        for (const node of list) {
            for (let d = node; !(mask[d] & (1 << index)); d = g.idom[d]) {
                mask[d] |= 1 << index;
                if (!d)
                    break;
            }
        }
    });
    const owned = new Uint8Array(g.nodeCount);
    for (const n of g.preorder) {
        if (n)
            owned[n] = owned[g.idom[n]] || (mask[n] && !(mask[n] & (mask[n] - 1))) ? 1 : 0;
    }
    const noise = new Float64Array(g.nodeCount);
    for (let i = g.preorder.length - 1; i >= 0; --i) {
        const n = g.preorder[i];
        if (!owned[n] && !isInternal(g, n))
            noise[n] += g.size[n];
        if (n)
            noise[g.idom[n]] += noise[n];
    }
    return { noise, owned, mask };
}

// LeakTest.cpp's retainedPathKeys: the classes on each node's path of dominators from <root>.
function retainedPathKeys(walk) {
    const paths = new Map();
    const descriptions = ['<root>'];
    const keysOf = g => {
        const result = new Int32Array(g.nodeCount);
        for (const n of g.preorder) {
            if (!n)
                continue;
            const parent = result[g.idom[n]];
            const id = parent + '\u0000' + g.className[n];
            let key = paths.get(id);
            if (key === undefined) {
                descriptions.push(descriptions[parent] + ' > ' + g.className[n]);
                key = descriptions.length - 1;
                paths.set(id, key);
            }
            result[n] = key;
        }
        return result;
    };
    return { before: keysOf(walk.before), after: keysOf(walk.after), descriptions };
}

// LeakTest.cpp's entriesOf: the entries of a list of what grew, the largest growth first.
function entriesOf(walk, demos, keys) {
    const ownBefore = noiseRetainedIn(walk.before, leakedNodesIn(walk.before, demos, walk.hasStorageNodes, true));
    const ownAfter = noiseRetainedIn(walk.after, leakedNodesIn(walk.after, demos, walk.hasStorageNodes, false));
    walk.ownership = ownAfter;
    const entryOfKey = new Map();
    const entries = [];
    const isCounted = (g, keyOf, node) => {
        for (let d = g.idom[node]; d; d = g.idom[d]) {
            if (keyOf[d] === keyOf[node])
                return false;
        }
        return true;
    };
    const g = walk.after;
    for (let node = 1; node < g.nodeCount; ++node) {
        if (isInternal(g, node))
            continue;
        const key = keys.after[node];
        let entry = entryOfKey.get(key);
        if (!entry) {
            entry = { key, members: [], growth: 0, noiseGrowth: 0, charge: demos.map(() => 0) };
            entryOfKey.set(key, entry);
            entries.push(entry);
        }
        entry.members.push(node);
        if (isCounted(g, keys.after, node)) {
            entry.growth += g.retained[node];
            entry.noiseGrowth += ownAfter.noise[node];
        }
    }
    const b = walk.before;
    for (let node = 1; node < b.nodeCount; ++node) {
        const entry = entryOfKey.get(keys.before[node]);
        if (isInternal(b, node) || !entry || !isCounted(b, keys.before, node))
            continue;
        entry.growth -= b.retained[node];
        entry.noiseGrowth -= ownBefore.noise[node];
    }
    demos.forEach((demo, index) => {
        for (const leaked of walk.leaks.between[index]) {
            const charged = new Set();
            for (let node = leaked; node; node = g.idom[node]) {
                const entry = entryOfKey.get(keys.after[node]);
                if (!entry || charged.has(entry))
                    continue;
                charged.add(entry);
                entry.charge[index] += g.size[leaked];
            }
        }
    });
    const cmp = (x, y) => x < y ? -1 : x > y ? 1 : 0;
    entries.sort((x, y) => x.growth !== y.growth ? y.growth - x.growth : cmp(keys.descriptions[x.key], keys.descriptions[y.key]));
    entries.forEach((entry, rank) => entry.rank = rank + 1);
    return entries;
}

function describeKey(keys, key) {
    const steps = keys.descriptions[key].split(' > ');
    if (steps.length <= 3)
        return keys.descriptions[key];
    return '... > ' + steps.slice(-3).join(' > ');
}

// LeakTest.cpp's whyNotFound: whether the entry of the demo's last holder is charged with half
// of what leaked, with nothing another demo leaked, and outgrows every unrelated entry's noise.
// Also returns that entry and the noisiest unrelated entry, for the report.
function findLeak(walk, demos, index) {
    const result = { verdict: null, stage: null, entry: null, noisiest: null };
    if (walk.leaks.missing[index]) {
        result.verdict = walk.leaks.missing[index];
        result.stage = 'missing';
        return result;
    }
    const g = walk.after, keys = walk.keys, entries = walk.entries;
    const leaked = walk.leaks.bytesBetween[index];
    const holderAddress = demos[index].holders[demos[index].holders.length - 1];
    const node = g.nodeStartingAt(holderAddress);
    if (node === undefined) {
        result.stage = 'holder';
        result.verdict = `its holder at ${hex(holderAddress)} is no node`;
        return result;
    }
    const entry = entries.find(e => e.key === keys.after[node]);
    if (!entry) {
        result.stage = 'holder';
        result.verdict = `its holder, the ${g.className[node]} at ${hex(holderAddress)}, is in no entry`;
        return result;
    }
    result.entry = entry;
    const isRelated = (a, b) => a.members.some(x => b.members.some(y => g.dominates(x, y) || g.dominates(y, x)));
    result.noisiest = entries.filter(o => !isRelated(o, entry)).reduce((best, o) => !best || o.noiseGrowth > best.noiseGrowth ? o : best, null);
    const name = `the entry of its holder, ${describeKey(keys, entry.key)}`;
    if (entry.charge[index] * 2 < leaked) {
        result.stage = 'concentration';
        result.verdict = `${name}, is charged with ${entry.charge[index]} of the ${leaked} bytes it leaked between the dumps`;
        return result;
    }
    const other = demos.findIndex((d, j) => j !== index && entry.charge[j]);
    if (other >= 0) {
        result.stage = 'exclusivity';
        result.verdict = `${name}, is charged with what ${demos[other].page} leaked too`;
        return result;
    }
    const noisier = entries.find(o => o.noiseGrowth > entry.growth && !isRelated(o, entry));
    if (noisier) {
        result.stage = 'noise';
        result.verdict = `${name}, grew by ${entry.growth} bytes, less than the unrelated ${describeKey(keys, noisier.key)} by ${noisier.noiseGrowth} bytes of noise`;
        return result;
    }
    result.stage = 'found';
    return result;
}

if (typeof module !== 'undefined')
    module.exports = { Dump, loadReport, growthFunction, leaksIn, whyNotSeen, findLeak, describeKey, isInternal, Kind, EdgeKind };
