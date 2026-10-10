#!/usr/bin/env python3
"""Turn a JSC GCDebugging heap snapshot into an SVG of the retained-size tree.

Reads the GCDebugging JSON format emitted by HeapSnapshotBuilder.cpp (and by
`mya heapdump`, which writes the same format). Computes dominator-based retained
sizes (Lengauer-Tarjan), detects cycles (Tarjan SCC) and, per the chart spec,
marks cycle members and gives them 0 retained size. Lays the dominator tree out
top-to-bottom (it is already topologically ordered) and prunes to the subtrees
that carry real weight so the result is legible.

Node record (GCDebugging, 7 fields): id, size, classNameIndex, flags,
labelIndex, "0xCELL", "0xWRAPPED".  Node id 0 is the synthetic <root>.
Edge record (4 fields): fromId, toId, edgeTypeIndex, extra.
Root record (3 fields): nodeId, rootReasonLabelIndex, reachabilityReasonLabelIndex.
"""
import argparse
import html
import json
import sys


def load_snapshot(path):
    with open(path, "r") as f:
        data = json.load(f)
    class_names = data["nodeClassNames"]
    edge_names = data.get("edgeNames", [])
    labels = data.get("labels", [])
    edge_types = data.get("edgeTypes", ["Internal", "Property", "Index", "Variable"])

    raw = data["nodes"]
    nfields = 7 if data.get("type") == "GCDebugging" else 4
    nodes = {}
    order = []
    for i in range(0, len(raw), nfields):
        nid = raw[i]
        size = raw[i + 1]
        cls = class_names[raw[i + 2]] if raw[i + 2] < len(class_names) else "?"
        flags = raw[i + 3]
        label = ""
        cell = ""
        wrapped = ""
        if nfields == 7:
            li = raw[i + 4]
            label = labels[li] if isinstance(li, int) and li < len(labels) else ""
            cell = raw[i + 5]
            wrapped = raw[i + 6]
        nodes[nid] = {
            "id": nid, "size": size, "cls": cls, "flags": flags,
            "label": label, "cell": cell, "wrapped": wrapped,
        }
        order.append(nid)

    succ = {nid: [] for nid in nodes}
    redges = data["edges"]
    for i in range(0, len(redges), 4):
        frm = redges[i]
        to = redges[i + 1]
        etype = redges[i + 2]
        extra = redges[i + 3]
        if frm in nodes and to in nodes:
            succ[frm].append((to, etype, extra))

    gc_roots = {}  # nodeId -> list of reason strings
    for i in range(0, len(data.get("roots", [])), 3):
        nid = data["roots"][i]
        reason = labels[data["roots"][i + 1]] if data["roots"][i + 1] < len(labels) else ""
        gc_roots.setdefault(nid, []).append(reason)

    return {
        "nodes": nodes, "order": order, "succ": succ,
        "edge_names": edge_names, "edge_types": edge_types,
        "gc_roots": gc_roots, "root": 0,
    }


def tarjan_scc(nodes, succ):
    """Iterative Tarjan. Returns {nodeId: sccId} and set of cyclic nodeIds."""
    index = {}
    low = {}
    onstack = set()
    stack = []
    comp = {}
    counter = [0]
    scc_id = [0]
    cyclic = set()

    for start in nodes:
        if start in index:
            continue
        work = [(start, iter(succ[start]))]
        while work:
            v, it = work[-1]
            if v not in index:
                index[v] = low[v] = counter[0]
                counter[0] += 1
                stack.append(v)
                onstack.add(v)
            advanced = False
            for (w, _t, _e) in it:
                if w not in index:
                    work.append((w, iter(succ[w])))
                    advanced = True
                    break
                elif w in onstack:
                    low[v] = min(low[v], index[w])
            if advanced:
                continue
            if low[v] == index[v]:
                members = []
                while True:
                    w = stack.pop()
                    onstack.discard(w)
                    comp[w] = scc_id[0]
                    members.append(w)
                    if w == v:
                        break
                if len(members) > 1:
                    cyclic.update(members)
                else:
                    m = members[0]
                    if any(e[0] == m for e in succ[m]):  # self-loop
                        cyclic.add(m)
                scc_id[0] += 1
            work.pop()
            if work:
                p = work[-1][0]
                low[p] = min(low[p], low[v])
    return comp, cyclic


def dominators(nodes_order, succ, root):
    """Lengauer-Tarjan dominator tree. Returns idom dict over nodes reachable
    from root. root's idom is itself."""
    # DFS numbering (iterative).
    dfnum = {}
    vertex = []
    parent = {}
    semi = {}
    pred = {n: [] for n in succ}
    reachable = set()

    stack = [(root, None)]
    while stack:
        v, p = stack.pop()
        if v in dfnum:
            if p is not None:
                pred[v].append(p)
            continue
        dfnum[v] = len(vertex)
        vertex.append(v)
        parent[v] = p
        reachable.add(v)
        if p is not None:
            pred[v].append(p)
        for (w, _t, _e) in succ[v]:
            if w not in dfnum:
                stack.append((w, v))
            else:
                pred[w].append(v)

    n = len(vertex)
    ancestor = {}
    label = {}
    for v in vertex:
        semi[v] = dfnum[v]
        label[v] = v
        ancestor[v] = None

    def compress(v):
        path = []
        while ancestor[v] is not None and ancestor[ancestor[v]] is not None:
            path.append(v)
            v = ancestor[v]
        # v now is a node whose ancestor's ancestor is None
        for node in reversed(path):
            a = ancestor[node]
            if semi[label[a]] < semi[label[node]]:
                label[node] = label[a]
            ancestor[node] = ancestor[a]

    def eval_(v):
        if ancestor[v] is None:
            return v
        compress(v)
        return label[v]

    idom = {}
    bucket = {v: set() for v in vertex}
    idom[root] = root

    for i in range(n - 1, 0, -1):
        w = vertex[i]
        for v in pred[w]:
            if v not in dfnum:
                continue
            u = eval_(v)
            if semi[u] < semi[w]:
                semi[w] = semi[u]
        bucket[vertex[semi[w]]].add(w)
        ancestor[w] = parent[w]
        for v in list(bucket[parent[w]]):
            bucket[parent[w]].discard(v)
            u = eval_(v)
            idom[v] = u if semi[u] < semi[v] else parent[w]

    for i in range(1, n):
        w = vertex[i]
        if idom.get(w) != vertex[semi[w]]:
            idom[w] = idom[idom[w]]
    idom[root] = root
    return idom, reachable


def compute_retained(snap):
    nodes = snap["nodes"]
    succ = snap["succ"]
    root = snap["root"]

    # Root edges: node 0 already has edges to GC roots in `succ`. Also ensure
    # every gc_root is reachable from node 0.
    existing = {e[0] for e in succ[root]}
    for nid in snap["gc_roots"]:
        if nid != root and nid not in existing:
            succ[root].append((nid, 0, 0))
    # Any node with no path from root (floating) -- attach to root so retained
    # accounting still covers it; these are rare in a GC snapshot.
    comp, cyclic = tarjan_scc(nodes, succ)
    idom, reachable = dominators(snap["order"] if root in snap["order"] else list(nodes), succ, root)

    unreached = [n for n in nodes if n not in reachable]
    for nid in unreached:
        succ[root].append((nid, 0, 0))
    if unreached:
        idom, reachable = dominators(list(nodes), succ, root)

    # Dominator children.
    dom_children = {n: [] for n in nodes}
    for n in nodes:
        d = idom.get(n)
        if d is not None and d != n:
            dom_children[d].append(n)

    # Retained size = own shallow size + retained size of every node this node
    # dominates. This is the standard dominator-tree retained size and is exact
    # even in the presence of cycles (a strongly connected component's shared
    # mass is charged to the single node that dominates its entry). Cycle
    # membership is tracked only for marking, never subtracted from the size.
    retained = {}
    seen = set()
    st = [root]
    while st:
        v = st[-1]
        if v not in seen:
            seen.add(v)
            for c in dom_children[v]:
                st.append(c)
        else:
            st.pop()
            if v not in retained:
                retained[v] = nodes[v]["size"] + sum(retained[c] for c in dom_children[v])
    return {
        "idom": idom, "dom_children": dom_children, "retained": retained,
        "cyclic": cyclic, "scc": comp,
    }


# ---------- SVG rendering ----------

def human(n):
    for unit in ("B", "KB", "MB", "GB"):
        if abs(n) < 1024 or unit == "GB":
            return f"{n:.0f} {unit}" if unit == "B" else f"{n/1024**(['B','KB','MB','GB'].index(unit)):.1f} {unit}"
        n = n
    return str(n)


def human_bytes(n):
    units = ["B", "KB", "MB", "GB"]
    f = float(n)
    i = 0
    while f >= 1024 and i < 3:
        f /= 1024.0
        i += 1
    return (f"{int(f)} {units[i]}" if i == 0 else f"{f:.1f} {units[i]}")


def best_name(snap, nid):
    nd = snap["nodes"][nid]
    cls = nd["cls"]
    lbl = nd["label"]
    if lbl and lbl not in ("", cls):
        return f"{cls} [{lbl}]"
    return cls


def build_tree(snap, dom, root, min_retained, max_children, max_nodes, pins=None):
    """Prune the dominator tree to a legible shape: keep children whose retained
    size >= min_retained, cap fan-out at max_children (collapse the rest into a
    synthetic summary), and stop once max_nodes are emitted. Any node whose class
    matches a `pins` substring is forced in, along with its path to the root, so
    a small-but-interesting subtree (the leak) is never pruned away."""
    retained = dom["retained"]
    dom_children = dom["dom_children"]
    idom = dom["idom"]
    nodes = snap["nodes"]
    pins = [p.lower() for p in (pins or [])]

    id_pins = {int(p[1:]) for p in pins if p.startswith("#")}

    def matches(cls):
        cl = cls.lower()
        for p in pins:
            if p.startswith("#"):
                continue
            if p.startswith("="):
                if cl == p[1:]:
                    return True
            elif p in cl:
                return True
        return False

    forced = set()
    if pins:
        for n in nodes:
            if n == root:
                continue
            if n in id_pins or matches(nodes[n]["cls"]):
                cur = n
                while cur is not None and cur not in forced:
                    forced.add(cur)
                    nxt = idom.get(cur)
                    if nxt == cur:
                        break
                    cur = nxt
        forced.discard(root)
    emitted = {"n": 0}

    def rec(nid):
        if emitted["n"] >= max_nodes and nid not in forced:
            return None
        emitted["n"] += 1
        node = {
            "id": nid,
            "name": best_name(snap, nid),
            "retained": retained.get(nid, 0),
            "shallow": nodes[nid]["size"],
            "cyclic": nid in dom["cyclic"],
            "pinned": nid in forced and bool(pins),
            "children": [],
            "collapsed": None,
        }
        kids = sorted(dom_children[nid], key=lambda c: retained.get(c, 0), reverse=True)
        forced_kids = [k for k in kids if k in forced]
        over = [k for k in kids if k not in forced and retained.get(k, 0) >= min_retained]
        keep_over = over[:max(0, max_children - len(forced_kids))]
        kept = forced_kids + keep_over
        kept_set = set(kept)
        dropped = [k for k in kids if k not in kept_set]
        for k in sorted(kept, key=lambda c: retained.get(c, 0), reverse=True):
            child = rec(k)
            if child:
                node["children"].append(child)
        if dropped:
            rsum = sum(retained.get(k, 0) for k in dropped)
            node["collapsed"] = {"count": len(dropped), "retained": rsum}
        return node

    return rec(root)


# Layout: simple tidy top-down tree. Compute subtree widths then assign x.
NODE_W = 230
NODE_H = 52
H_GAP = 24
V_GAP = 46


def layout(tree):
    # returns (positions dict id->(x,y), width, height)
    leaf_x = [0]
    pos = {}
    depth_max = [0]

    def place(node, depth):
        depth_max[0] = max(depth_max[0], depth)
        nchildren = node["children"] + ([{"synthetic": True, "children": []}] if node["collapsed"] else [])
        if not nchildren:
            x = leaf_x[0] * (NODE_W + H_GAP)
            leaf_x[0] += 1
            node["_x"] = x
        else:
            xs = []
            for c in node["children"]:
                place(c, depth + 1)
                xs.append(c["_x"])
            if node["collapsed"]:
                x = leaf_x[0] * (NODE_W + H_GAP)
                node["_collapsed_x"] = x
                leaf_x[0] += 1
                xs.append(x)
            node["_x"] = sum(xs) / len(xs)
        node["_y"] = depth * (NODE_H + V_GAP)

    place(tree, 0)
    width = leaf_x[0] * (NODE_W + H_GAP) + 80
    height = (depth_max[0] + 1) * (NODE_H + V_GAP) + 80
    return width, height


PALETTE = {
    # class-name substring -> fill (light), used to tint by rough category
}


def color_for(name, cyclic):
    if cyclic:
        return "#b45309", "#fff7ed"  # amber border, pale fill
    key = name.lower()
    if any(k in key for k in ("arraybuffer", "serialized", "message", "queue", "deque",
                              "transfer", "wirebytes", "contents", "runloop", "task", "worker")):
        return "#be123c", "#fff1f3"  # rose: the leak family (worker message queue)
    if any(k in key for k in ("vm", "heap", "globalobject", "worker", "window")):
        return "#1d4ed8", "#eff6ff"  # blue: roots/vm
    if "structure" in key or "symbol" in key:
        return "#6d28d9", "#f5f3ff"
    return "#334155", "#f8fafc"  # slate: default


def render_svg(tree, title, subtitle):
    width, height = layout(tree)
    out = []
    out.append(
        f'<svg xmlns="http://www.w3.org/2000/svg" width="{width:.0f}" height="{height:.0f}" '
        f'viewBox="0 0 {width:.0f} {height:.0f}" font-family="ui-sans-serif,-apple-system,Segoe UI,Roboto,sans-serif">'
    )
    out.append(f'<rect width="{width:.0f}" height="{height:.0f}" fill="#ffffff"/>')
    out.append(f'<text x="24" y="30" font-size="20" font-weight="700" fill="#0f172a">{html.escape(title)}</text>')
    out.append(f'<text x="24" y="52" font-size="13" fill="#475569">{html.escape(subtitle)}</text>')

    ox, oy = 40, 80

    # edges first
    def walk_edges(node):
        for c in node["children"]:
            x1 = ox + node["_x"] + NODE_W / 2
            y1 = oy + node["_y"] + NODE_H
            x2 = ox + c["_x"] + NODE_W / 2
            y2 = oy + c["_y"]
            midy = (y1 + y2) / 2
            out.append(
                f'<path d="M{x1:.1f},{y1:.1f} C{x1:.1f},{midy:.1f} {x2:.1f},{midy:.1f} {x2:.1f},{y2:.1f}" '
                f'fill="none" stroke="#cbd5e1" stroke-width="1.5"/>'
            )
            walk_edges(c)
        if node["collapsed"]:
            x1 = ox + node["_x"] + NODE_W / 2
            y1 = oy + node["_y"] + NODE_H
            x2 = ox + node["_collapsed_x"] + NODE_W / 2
            y2 = oy + (node["_y"] + NODE_H + V_GAP)
            midy = (y1 + y2) / 2
            out.append(
                f'<path d="M{x1:.1f},{y1:.1f} C{x1:.1f},{midy:.1f} {x2:.1f},{midy:.1f} {x2:.1f},{y2:.1f}" '
                f'fill="none" stroke="#e2e8f0" stroke-width="1.5" stroke-dasharray="4 3"/>'
            )

    walk_edges(tree)

    def walk_nodes(node):
        x = ox + node["_x"]
        y = oy + node["_y"]
        border, fill = color_for(node["name"], node["cyclic"])
        sw = 3.5 if node.get("pinned") else 2
        out.append(f'<rect x="{x:.1f}" y="{y:.1f}" width="{NODE_W}" height="{NODE_H}" rx="7" '
                   f'fill="{fill}" stroke="{border}" stroke-width="{sw}"/>')
        nm = node["name"]
        # Shorten verbose C++ template/namespace names to the telling leaf.
        if "unique_ptr<" in nm and "::Task" in nm:
            nm = "Deque<unique_ptr<...Task>>"
        nm = nm.replace("WebCore::", "").replace("JSC::", "").replace("WTF::", "")
        if len(nm) > 32:
            nm = nm[:31] + "…"
        rtxt = f'retained {human_bytes(node["retained"])}'
        if node["cyclic"]:
            rtxt = f'cycle · retained {human_bytes(node["retained"])}'
        out.append(f'<text x="{x+10:.1f}" y="{y+21:.1f}" font-size="13" font-weight="600" fill="{border}">{html.escape(nm)}</text>')
        out.append(f'<text x="{x+10:.1f}" y="{y+38:.1f}" font-size="12" fill="#475569">{html.escape(rtxt)}</text>')
        out.append(f'<text x="{x+NODE_W-10:.1f}" y="{y+38:.1f}" font-size="10.5" fill="#94a3b8" text-anchor="end">self {human_bytes(node["shallow"])}</text>')
        for c in node["children"]:
            walk_nodes(c)
        if node["collapsed"]:
            cx = ox + node["_collapsed_x"]
            cy = oy + (node["_y"] + NODE_H + V_GAP)
            out.append(f'<rect x="{cx:.1f}" y="{cy:.1f}" width="{NODE_W}" height="{NODE_H}" rx="7" '
                       f'fill="#f1f5f9" stroke="#cbd5e1" stroke-width="1.5" stroke-dasharray="4 3"/>')
            out.append(f'<text x="{cx+10:.1f}" y="{cy+21:.1f}" font-size="13" font-weight="600" fill="#64748b">+{node["collapsed"]["count"]} smaller subtrees</text>')
            out.append(f'<text x="{cx+10:.1f}" y="{cy+38:.1f}" font-size="12" fill="#64748b">retained {human_bytes(node["collapsed"]["retained"])}</text>')

    walk_nodes(tree)
    out.append("</svg>")
    return "\n".join(out)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("snapshot")
    ap.add_argument("-o", "--out", required=True)
    ap.add_argument("--title", default="JSC heap")
    ap.add_argument("--subtitle", default="")
    ap.add_argument("--min-retained", type=int, default=2048,
                    help="prune dominator children retaining fewer than this many bytes")
    ap.add_argument("--max-children", type=int, default=8)
    ap.add_argument("--max-nodes", type=int, default=120)
    ap.add_argument("--pin", default="", help="comma-separated class-name substrings to force into the chart with their path to root")
    args = ap.parse_args()

    pins = [p for p in args.pin.split(",") if p]
    snap = load_snapshot(args.snapshot)
    dom = compute_retained(snap)
    tree = build_tree(snap, dom, snap["root"], args.min_retained, args.max_children, args.max_nodes, pins)

    total = dom["retained"].get(snap["root"], 0)
    ncyclic = len(dom["cyclic"])
    sub = args.subtitle or (f'{len(snap["nodes"])-1} cells · total {human_bytes(total)} · '
                            f'{ncyclic} cells in cycles (marked)')
    svg = render_svg(tree, args.title, sub)
    with open(args.out, "w") as f:
        f.write(svg)

    # Also print the top retainers for the report.
    ret = dom["retained"]
    tops = sorted((n for n in snap["nodes"] if n != snap["root"]),
                  key=lambda n: ret.get(n, 0), reverse=True)[:25]
    print(f"snapshot: {args.snapshot}")
    print(f"cells: {len(snap['nodes'])-1}  total-retained: {human_bytes(total)}  cyclic-cells: {ncyclic}")
    print("top retainers:")
    for n in tops:
        nd = snap["nodes"][n]
        print(f"  {human_bytes(ret.get(n,0)):>10}  self={human_bytes(nd['size']):>9}  {best_name(snap,n)}  (id {n})")


if __name__ == "__main__":
    main()
