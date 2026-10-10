#!/usr/bin/env python3
"""Draw a GCDebugging heap snapshot (JSC's own, or `mya heapdump`'s) as its retention tree.

Every number and every node in the chart is computed from the snapshot; nothing is picked by
hand. The rules:

- Retained size is the dominator-tree retained size, as Web Inspector computes it: a node's
  bytes plus those of every node it dominates. A node's parent in the tree is its immediate
  dominator, the last node every path from <root> to it passes through: breaking that link,
  or anything above it, frees the node. A node no path reaches hangs off <root>.
- Siblings of one class are drawn as one group ("x2100"), whose retained size is the sum of its
  members'. A group's children are its members' dominator children, grouped again by class. A
  member dominated by a member of its own class, such as the next element of a linked list, is
  folded into that group.
- A cycle is a strongly connected set of nodes over strong edges that includes a C++ object
  (a mya node that is not a JS cell): memory that holds itself alive, which neither the collector
  nor reference counting can free. Its nodes are marked and retain 0 bytes, so nothing above them
  counts them or what they dominate. What they would otherwise retain is shown as what the cycle
  holds, and a level picks a cycle by that. A cycle of JS cells only is not one: the collector
  frees it.
- Each level shows the groups that retain at least --min-percent of the heap, the largest first,
  at most --max-children of them; the rest of the level is one "other" box with their total.
- A group whose parent in the tree does not itself point to it, because several holders share it,
  lists what does under its box: each holder's class and the edge's name.

Usage: heapchart.py <snapshot.json> [<snapshot.json> ...] -o out.svg [--titles A,B]
Several snapshots are drawn side by side.
"""
import argparse
import collections
import html
import json
import sys


class Snapshot:
    def __init__(self, path):
        with open(path) as f:
            data = json.load(f)
        if data.get("type") != "GCDebugging" or data.get("version") != 3:
            sys.exit(f"{path}: not a GCDebugging version 3 heap snapshot")
        self.path = path
        class_names = data["nodeClassNames"]
        labels = data["labels"]
        edge_names = data["edgeNames"]
        edge_types = data["edgeTypes"]
        raw = data["nodes"]
        self.size, self.cls, self.label = {}, {}, {}
        self.ids = []
        for i in range(0, len(raw), 7):
            node = raw[i]
            self.ids.append(node)
            self.size[node] = raw[i + 1]
            self.cls[node] = class_names[raw[i + 2]]
            self.label[node] = labels[raw[i + 4]] if raw[i + 4] < len(labels) else ""
        # mya labels its JS cells; JSC's own snapshot has a node per cell and nothing else.
        self.is_mya = "JS cell" in labels
        self.succ = collections.defaultdict(list)
        self.pred = collections.defaultdict(list)
        self.edge_name = {}
        self.strong = set()
        edges = data["edges"]
        for i in range(0, len(edges), 4):
            a, b, kind, extra = edges[i:i + 4]
            self.succ[a].append(b)
            self.pred[b].append(a)
            name = edge_names[extra] if edge_types[kind] in ("Property", "Variable") and extra < len(edge_names) else ""
            self.edge_name.setdefault((a, b), name)
            if not (name.endswith("(weak)") or name.endswith("(untraced)")):
                self.strong.add((a, b))
        self.roots = set()
        for i in range(0, len(data["roots"]), 3):
            self.roots.add(data["roots"][i])

    def is_cell(self, node):
        return not self.is_mya or self.label[node] == "JS cell"

    def total(self):
        return sum(self.size.values())


def dominators(snap):
    """Cooper, Harvey and Kennedy's iterative algorithm, over reverse postorder from <root> (node 0).
    A node no path reaches is given <root> as its dominator, as Web Inspector does."""
    succ = {n: list(snap.succ.get(n, ())) for n in snap.ids}
    have = set(succ[0])
    succ[0] += [n for n in sorted(snap.roots) if n and n not in have]
    order, seen, stack = [], {0}, [(0, iter(succ[0]))]
    while stack:
        node, it = stack[-1]
        for nxt in it:
            if nxt not in seen:
                seen.add(nxt)
                stack.append((nxt, iter(succ[nxt])))
                break
        else:
            order.append(node)
            stack.pop()
    unreached = [n for n in snap.ids if n not in seen]
    succ[0] += unreached
    for n in unreached:
        order.insert(0, n)  # Postorder: before <root>, which comes last.
    rpo = list(reversed(order))
    number = {n: i for i, n in enumerate(rpo)}
    preds = collections.defaultdict(list)
    for n in rpo:
        for m in succ[n]:
            if m in number:
                preds[m].append(n)
    idom = {0: 0}

    def intersect(a, b):
        while a != b:
            while number[a] > number[b]:
                a = idom[a]
            while number[b] > number[a]:
                b = idom[b]
        return a

    changed = True
    while changed:
        changed = False
        for n in rpo[1:]:
            new = None
            for p in preds[n]:
                if p in idom:
                    new = p if new is None else intersect(p, new)
            if idom.get(n) != new:
                idom[n] = new
                changed = True
    return idom, rpo, set(unreached)


def cycles(snap):
    """The nodes of each strongly connected set, over strong edges, that has a C++ object in it."""
    index, low, on, stack, marked, counter = {}, {}, set(), [], set(), [0]
    for start in snap.ids:
        if start in index:
            continue
        index[start] = low[start] = counter[0]
        counter[0] += 1
        stack.append(start)
        on.add(start)
        work = [(start, iter([m for m in snap.succ.get(start, ()) if (start, m) in snap.strong]))]
        while work:
            node, it = work[-1]
            pushed = False
            for m in it:
                if m not in index:
                    index[m] = low[m] = counter[0]
                    counter[0] += 1
                    stack.append(m)
                    on.add(m)
                    work.append((m, iter([k for k in snap.succ.get(m, ()) if (m, k) in snap.strong])))
                    pushed = True
                    break
                if m in on:
                    low[node] = min(low[node], index[m])
            if pushed:
                continue
            work.pop()
            if work:
                low[work[-1][0]] = min(low[work[-1][0]], low[node])
            if low[node] == index[node]:
                members = []
                while True:
                    m = stack.pop()
                    on.discard(m)
                    members.append(m)
                    if m == node:
                        break
                is_cycle = len(members) > 1 or (node, node) in snap.strong
                if is_cycle and any(not snap.is_cell(m) for m in members):
                    marked.update(members)
    return marked


class Analysis:
    def __init__(self, snap):
        self.snap = snap
        self.idom, rpo, self.unreached = dominators(snap)
        self.cyclic = cycles(snap)
        self.children = collections.defaultdict(list)
        for n, d in self.idom.items():
            if n != 0:
                self.children[d].append(n)
        # `held` is the dominator-tree retained size; `retained` is it with cycles at 0.
        self.retained, self.held = {}, {}
        for n in reversed(rpo):
            self.held[n] = snap.size[n] + sum(self.held[c] for c in self.children[n])
            if n in self.cyclic:
                self.retained[n] = 0
            else:
                self.retained[n] = snap.size[n] + sum(self.retained[c] for c in self.children[n])
        self.total = snap.total()
        self.unreached_bytes = sum(snap.size[n] for n in self.unreached)


def class_key(snap, node):
    return snap.cls[node]


def group_children(an, members):
    """The dominator children of `members`, grouped by class, with same-class descendants folded in."""
    snap = an.snap
    member_set = set(members)
    groups = collections.OrderedDict()
    for m in members:
        for c in an.children[m]:
            if c not in member_set:
                groups.setdefault(class_key(snap, c), []).append(c)
    result = []
    for key, nodes in groups.items():
        # Fold a chain of one class: a member dominated by a member of its own class joins the group.
        folded, frontier = list(nodes), list(nodes)
        while frontier:
            nxt = [c for n in frontier for c in an.children[n] if class_key(snap, c) == key]
            folded += nxt
            frontier = nxt
        result.append((key, folded))
    return result


def group_retained(an, nodes, sizes=None):
    node_set = set(nodes)
    sizes = sizes or an.retained
    # A folded member is inside another member's retained size; count each top member once.
    return sum(sizes[n] for n in nodes if n == 0 or an.idom[n] not in node_set)


def group_weight(an, nodes):
    """What a level ranks a group by: its retained size, or, for a group in a cycle, what the cycle holds."""
    if any(n in an.cyclic for n in nodes):
        return group_retained(an, nodes, an.held)
    return group_retained(an, nodes)


def link_name(an, parent_members, nodes):
    """The name of the edge from a group's dominator to its members, when it is a direct edge."""
    snap = an.snap
    names = collections.Counter()
    direct = 0
    parent_set = set(parent_members)
    top = [n for n in nodes if an.idom[n] in parent_set]
    for n in top:
        name = snap.edge_name.get((an.idom[n], n))
        if name is not None:
            direct += 1
            names[name] += 1
    if not top or not direct:
        return ""
    name, count = names.most_common(1)[0]
    if not name:
        return ""
    return name if count == len(top) else f"{name} ({count} of {len(top)})"


def holders(an, parent_members, nodes, limit=3):
    """What points to a group's members from outside it, when its dominator does not point to them all."""
    snap = an.snap
    node_set = set(nodes)
    top = [n for n in nodes if n != 0 and an.idom[n] not in node_set]
    if not top or all((an.idom[n], n) in snap.edge_name for n in top):
        return []
    counts = collections.Counter()
    for n in top:
        for p in snap.pred[n]:
            if p not in node_set:
                counts[(short(snap.cls[p], 40), short(snap.edge_name.get((p, n), ""), 90))] += 1
    lines = [f'{cls}{": " + name if name else ""}{f" (x{count})" if count > 1 else ""}' for (cls, name), count in counts.most_common(limit)]
    if len(counts) > limit:
        lines.append(f'{len(counts) - limit} more holders')
    return lines


def build_tree(an, min_bytes, max_children, max_depth):
    snap = an.snap

    def make(key, nodes, parent_members, depth):
        node_set = set(nodes)
        top = [n for n in nodes if n == 0 or an.idom[n] not in node_set]
        group = {
            "key": key,
            "count": len(top),
            "folded": len(nodes) - len(top),
            "retained": group_retained(an, nodes),
            "held": group_retained(an, nodes, an.held),
            "self": sum(snap.size[n] for n in nodes),
            "cyclic": sum(1 for n in nodes if n in an.cyclic),
            "unreached": sum(1 for n in top if n in an.unreached),
            "link": link_name(an, parent_members, nodes) if parent_members else "",
            "holders": holders(an, parent_members, nodes) if parent_members else [],
            "children": [],
            "other": None,
        }
        if depth >= max_depth:
            kids = group_children(an, nodes)
            if kids:
                group["other"] = (len(kids), sum(group_retained(an, k[1]) for k in kids), "deeper levels not drawn")
            return group
        kids = sorted(group_children(an, nodes), key=lambda k: group_weight(an, k[1]), reverse=True)
        shown = [k for k in kids if group_weight(an, k[1]) >= min_bytes][:max_children]
        for key2, nodes2 in shown:
            group["children"].append(make(key2, nodes2, nodes, depth + 1))
        rest = kids[len(shown):]
        if rest:
            group["other"] = (len(rest), sum(group_retained(an, k[1]) for k in rest), "")
        return group

    return make(snap.cls[0], [0], None, 0)


# ---------- drawing ----------

NODE_W, NODE_H, H_GAP, V_GAP = 250, 64, 18, 44
HOLDER_LINE = 13


def human(n):
    for unit, scale in (("MB", 1 << 20), ("KB", 1 << 10)):
        if abs(n) >= scale:
            return f"{n / scale:.1f} {unit}"
    return f"{n} B"


def short(name, limit=34):
    for prefix in ("WebCore::", "JSC::", "WTF::", "std::__1::", "WebKit::"):
        name = name.replace(prefix, "")
    return name if len(name) <= limit else name[:limit - 1] + "…"


def layout(tree):
    column = [0]
    holder_lines = collections.defaultdict(int)  # The most lines of holders under a box of each row.

    def measure(g, depth):
        holder_lines[depth] = max(holder_lines[depth], len(g["holders"]))
        holder_lines[depth + 1] += 0
        for c in g["children"]:
            measure(c, depth + 1)

    measure(tree, 0)
    rows = [0]
    for depth in range(max(holder_lines) + 1):
        rows.append(rows[-1] + NODE_H + V_GAP + HOLDER_LINE * holder_lines[depth])

    def place(g, depth):
        xs = []
        for c in g["children"]:
            place(c, depth + 1)
            xs.append(c["_x"])
        if g["other"]:
            g["_ox"] = column[0] * (NODE_W + H_GAP)
            g["_oy"] = rows[depth + 1]
            column[0] += 1
            xs.append(g["_ox"])
        if not xs:
            g["_x"] = column[0] * (NODE_W + H_GAP)
            column[0] += 1
        else:
            g["_x"] = (min(xs) + max(xs)) / 2
        g["_y"] = rows[depth]

    place(tree, 0)
    return column[0] * (NODE_W + H_GAP), rows[-1]


def draw_tree(tree, ox, oy, total, out):
    def edge(x1, y1, x2, y2, dashed):
        m = (y1 + y2) / 2
        dash = ' stroke-dasharray="4 3"' if dashed else ""
        out.append(f'<path d="M{x1:.1f},{y1:.1f} C{x1:.1f},{m:.1f} {x2:.1f},{m:.1f} {x2:.1f},{y2:.1f}" fill="none" stroke="var(--edge)" stroke-width="1.5"{dash}/>')

    def visit(g):
        x, y = ox + g["_x"], oy + g["_y"]
        for c in g["children"]:
            edge(x + NODE_W / 2, y + NODE_H, ox + c["_x"] + NODE_W / 2, oy + c["_y"], False)
            if c["link"]:
                out.append(f'<text x="{ox + c["_x"] + NODE_W / 2:.1f}" y="{oy + c["_y"] - 5:.1f}" class="link" text-anchor="middle">{html.escape(short(c["link"], 44))}</text>')
            visit(c)
        if g["other"]:
            cx, cy = ox + g["_ox"], oy + g["_oy"]
            edge(x + NODE_W / 2, y + NODE_H, cx + NODE_W / 2, cy, True)
            count, retained, note = g["other"]
            out.append(f'<rect x="{cx:.1f}" y="{cy:.1f}" width="{NODE_W}" height="{NODE_H}" rx="6" class="other"/>')
            out.append(f'<text x="{cx + 10:.1f}" y="{cy + 22:.1f}" class="name dim">{count} other group{"s" if count != 1 else ""}</text>')
            out.append(f'<text x="{cx + 10:.1f}" y="{cy + 42:.1f}" class="size dim">retained {human(retained)} · {100 * retained / total:.1f}%</text>')
            if note:
                out.append(f'<text x="{cx + 10:.1f}" y="{cy + 57:.1f}" class="self">{note}</text>')
        cls = "cycle" if g["cyclic"] else "node"
        out.append(f'<rect x="{x:.1f}" y="{y:.1f}" width="{NODE_W}" height="{NODE_H}" rx="6" class="{cls}"/>')
        count = f'×{g["count"]} ' if g["count"] > 1 else ""
        out.append(f'<text x="{x + 10:.1f}" y="{y + 22:.1f}" class="name">{html.escape(count + short(g["key"], 34 - len(count)))}</text>')
        size = f'retained {human(g["retained"])} · {100 * g["retained"] / total:.1f}%'
        if g["cyclic"]:
            size = f'cycle · retained 0 B · holds {human(g["held"])} · {100 * g["held"] / total:.1f}%'
        out.append(f'<text x="{x + 10:.1f}" y="{y + 42:.1f}" class="size">{size}</text>')
        detail = f'self {human(g["self"])}'
        if g["folded"]:
            detail += f' · {g["folded"]} more of its class folded in'
        if g["unreached"]:
            detail += f' · {g["unreached"]} reached by no path'
        out.append(f'<text x="{x + 10:.1f}" y="{y + 57:.1f}" class="self">{html.escape(detail)}</text>')
        for index, line in enumerate(g["holders"]):
            prefix = "held by " if index == 0 else "and "
            out.append(f'<text x="{x + 4:.1f}" y="{y + NODE_H + 12 + index * HOLDER_LINE:.1f}" class="held">{html.escape(prefix + line)}</text>')

    visit(tree)


STYLE = """
<style>
  svg { --bg:#ffffff; --fg:#0f172a; --dim:#64748b; --edge:#cbd5e1; --node:#f8fafc; --stroke:#334155;
        --other:#f1f5f9; --cycle:#fff7ed; --cyclestroke:#b45309; --panel:#e2e8f0; }
  @media (prefers-color-scheme: dark) {
    svg { --bg:#0b1220; --fg:#e2e8f0; --dim:#94a3b8; --edge:#334155; --node:#111a2e; --stroke:#94a3b8;
          --other:#0f172a; --cycle:#2a1a0a; --cyclestroke:#f59e0b; --panel:#1e293b; }
  }
  .bg { fill: var(--bg); }
  text { font-family: ui-sans-serif, -apple-system, "Segoe UI", Roboto, sans-serif; fill: var(--fg); }
  .title { font-size: 20px; font-weight: 700; }
  .sub { font-size: 13px; fill: var(--dim); }
  .ptitle { font-size: 17px; font-weight: 700; }
  .name { font-size: 13px; font-weight: 600; }
  .size { font-size: 12px; }
  .self, .link { font-size: 10.5px; fill: var(--dim); }
  .held { font-size: 10px; fill: var(--cyclestroke); }
  .dim { fill: var(--dim); }
  .node { fill: var(--node); stroke: var(--stroke); stroke-width: 1.5; }
  .other { fill: var(--other); stroke: var(--edge); stroke-width: 1.5; stroke-dasharray: 4 3; }
  .cycle { fill: var(--cycle); stroke: var(--cyclestroke); stroke-width: 2.5; }
  .panel { fill: none; stroke: var(--panel); stroke-width: 1.5; }
</style>
"""


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("snapshots", nargs="+")
    ap.add_argument("-o", "--out", required=True)
    ap.add_argument("--titles", default="", help="comma-separated panel titles, one per snapshot")
    ap.add_argument("--title", default="Retention tree")
    ap.add_argument("--min-percent", type=float, default=2.0)
    ap.add_argument("--max-children", type=int, default=5)
    ap.add_argument("--max-depth", type=int, default=16)
    ap.add_argument("--report", help="also write the drawn trees, and each snapshot's top retainers, as JSON")
    args = ap.parse_args()
    titles = args.titles.split(",") if args.titles else [s for s in args.snapshots]

    panels = []
    report = []
    for path, title in zip(args.snapshots, titles):
        snap = Snapshot(path)
        an = Analysis(snap)
        tree = build_tree(an, args.min_percent / 100 * an.total, args.max_children, args.max_depth)
        width, height = layout(tree)
        cells = sum(1 for n in snap.ids if n and snap.is_cell(n))
        sub = (f"{len(snap.ids) - 1} nodes ({cells} JS cells) · {human(an.total)} · "
               f"{len(an.cyclic)} nodes in cycles · {human(an.unreached_bytes)} reached by no path")
        panels.append((title, sub, tree, width, height, an.total))
        top = sorted((n for n in snap.ids if n), key=lambda n: an.retained[n], reverse=True)[:30]
        report.append({
            "snapshot": path, "nodes": len(snap.ids) - 1, "bytes": an.total, "cyclic": len(an.cyclic),
            "unreachedBytes": an.unreached_bytes, "tree": tree,
            "topRetainers": [{"id": n, "class": snap.cls[n], "retained": an.retained[n], "self": snap.size[n],
                              "dominator": an.idom[n], "dominatorClass": snap.cls[an.idom[n]]} for n in top],
        })

    pad, head, gap = 28, 120, 48
    width = pad + sum(p[3] + 2 * pad for p in panels) + gap * (len(panels) - 1) + pad
    height = head + max(p[4] for p in panels) + 3 * pad
    out = [f'<svg xmlns="http://www.w3.org/2000/svg" width="{width:.0f}" height="{height:.0f}" viewBox="0 0 {width:.0f} {height:.0f}">', STYLE,
           f'<rect class="bg" width="{width:.0f}" height="{height:.0f}"/>',
           f'<text x="{pad}" y="36" class="title">{html.escape(args.title)}</text>',
           f'<text x="{pad}" y="58" class="sub">Dominator tree: each box retains its subtree, and its parent is the link whose removal frees it. '
           f'Same-class siblings grouped; levels show groups over {args.min_percent:g}% of the heap, at most {args.max_children}. '
           f'A cycle (amber) holds itself alive through a C++ object: it retains 0 B, and shows what it holds.</text>']
    x = pad
    for title, sub, tree, w, h, total in panels:
        pw = w + 2 * pad
        out.append(f'<rect class="panel" x="{x:.0f}" y="72" width="{pw:.0f}" height="{height - 72 - pad:.0f}" rx="10"/>')
        out.append(f'<text x="{x + pad:.0f}" y="98" class="ptitle">{html.escape(title)}</text>')
        out.append(f'<text x="{x + pad:.0f}" y="116" class="sub">{html.escape(sub)}</text>')
        draw_tree(tree, x + pad, head + 24, total, out)
        x += pw + gap
    out.append("</svg>")
    with open(args.out, "w") as f:
        f.write("\n".join(out))
    if args.report:
        with open(args.report, "w") as f:
            json.dump(report, f, indent=1)
    print(f"wrote {args.out} ({width:.0f}x{height:.0f})")


if __name__ == "__main__":
    main()
