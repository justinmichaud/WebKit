#!/usr/bin/env python3
"""Side-by-side presentation view: the worker-leak subtree of the same process,
as a normal JSC heap dump sees it vs as mya sees it. Only the interesting part
of each heap is shown (the Worker and what hangs off it); the rest of the heap
is rolled into one honest "rest of heap" box so the scale stays visible.

Retained sizes are exact dominator-tree retained sizes (root retained == total
shallow bytes); cycle members are marked but keep their true retained size.
"""
import heapsvg as H

NODE_W, NODE_H, H_GAP, V_GAP = H.NODE_W, H.NODE_H, H.H_GAP, H.V_GAP


def focused_tree(path, pins):
    snap = H.load_snapshot(path)
    dom = H.compute_retained(snap)
    # min_retained huge -> nothing qualifies on size; only pinned nodes and their
    # root-path are kept, everything else collapses into the "rest" box.
    tree = H.build_tree(snap, dom, 0, 10**15, 6, 60, pins)
    total = dom["retained"].get(0, 0)
    ncells = len(snap["nodes"]) - 1
    return tree, total, ncells


def emit_tree(tree, ox, oy, out):
    H.layout(tree)

    def edges(node):
        for c in node["children"]:
            x1 = ox + node["_x"] + NODE_W / 2
            y1 = oy + node["_y"] + NODE_H
            x2 = ox + c["_x"] + NODE_W / 2
            y2 = oy + c["_y"]
            m = (y1 + y2) / 2
            out.append(f'<path d="M{x1:.1f},{y1:.1f} C{x1:.1f},{m:.1f} {x2:.1f},{m:.1f} {x2:.1f},{y2:.1f}" fill="none" stroke="#cbd5e1" stroke-width="1.5"/>')
            edges(c)
        if node["collapsed"]:
            x1 = ox + node["_x"] + NODE_W / 2
            y1 = oy + node["_y"] + NODE_H
            x2 = ox + node["_collapsed_x"] + NODE_W / 2
            y2 = oy + node["_y"] + NODE_H + V_GAP
            m = (y1 + y2) / 2
            out.append(f'<path d="M{x1:.1f},{y1:.1f} C{x1:.1f},{m:.1f} {x2:.1f},{m:.1f} {x2:.1f},{y2:.1f}" fill="none" stroke="#e2e8f0" stroke-width="1.5" stroke-dasharray="4 3"/>')

    def nodes(node):
        x, y = ox + node["_x"], oy + node["_y"]
        border, fill = H.color_for(node["name"], node["cyclic"])
        sw = 3.5 if node.get("pinned") else 2
        out.append(f'<rect x="{x:.1f}" y="{y:.1f}" width="{NODE_W}" height="{NODE_H}" rx="7" fill="{fill}" stroke="{border}" stroke-width="{sw}"/>')
        nm = node["name"]
        if "unique_ptr<" in nm and "::Task" in nm:
            nm = "Deque<unique_ptr<...Task>>"
        nm = nm.replace("WebCore::", "").replace("JSC::", "").replace("WTF::", "")
        if len(nm) > 32:
            nm = nm[:31] + "…"
        rtxt = f'retained {H.human_bytes(node["retained"])}'
        if node["cyclic"]:
            rtxt = f'↺ cycle · retained {H.human_bytes(node["retained"])}'
        out.append(f'<text x="{x+10:.1f}" y="{y+21:.1f}" font-size="13" font-weight="600" fill="{border}">{H.html.escape(nm)}</text>')
        out.append(f'<text x="{x+10:.1f}" y="{y+38:.1f}" font-size="12" fill="#475569">{H.html.escape(rtxt)}</text>')
        out.append(f'<text x="{x+NODE_W-10:.1f}" y="{y+38:.1f}" font-size="10.5" fill="#94a3b8" text-anchor="end">self {H.human_bytes(node["shallow"])}</text>')
        for c in node["children"]:
            nodes(c)
        if node["collapsed"]:
            cx, cy = ox + node["_collapsed_x"], oy + node["_y"] + NODE_H + V_GAP
            is_root = node["id"] == 0
            head = (f'rest of heap: +{node["collapsed"]["count"]} subtrees' if is_root
                    else f'+{node["collapsed"]["count"]} more (the queued Tasks)' if "RunLoop" in node["name"] or "Deque" in node["name"] or "Task" in node["name"]
                    else f'+{node["collapsed"]["count"]} smaller subtrees')
            out.append(f'<rect x="{cx:.1f}" y="{cy:.1f}" width="{NODE_W}" height="{NODE_H}" rx="7" fill="#f1f5f9" stroke="#cbd5e1" stroke-width="1.5" stroke-dasharray="4 3"/>')
            out.append(f'<text x="{cx+10:.1f}" y="{cy+21:.1f}" font-size="12.5" font-weight="600" fill="#64748b">{head}</text>')
            out.append(f'<text x="{cx+10:.1f}" y="{cy+38:.1f}" font-size="12" fill="#64748b">retained {H.human_bytes(node["collapsed"]["retained"])}</text>')

    edges(tree)
    nodes(tree)
    w = max((ox + n for n in [tree["_x"]]), default=ox)
    return H.layout(tree)  # (w,h) in local coords


def panel_dims(tree):
    return H.layout(tree)


def main():
    # Pin exact nodes for a clean presentation chain (ids from these two dumps).
    WORKER_PINS = ["#16695", "#31692"]   # WorkerDedicatedRunLoop -> Deque(unique_ptr<...Task>)
    NORMAL_PINS = ["#432", "#437"]       # JSDOMWindow (top real retainer) + the Worker instance

    import sys
    base = sys.argv[1] if len(sys.argv) > 1 else "."
    ltree, ltot, lcells = focused_tree(f"{base}/normal.json", NORMAL_PINS)
    rtree, rtot, rcells = focused_tree(f"{base}/mya.json", WORKER_PINS)
    lw, lh = panel_dims(ltree)
    rw, rh = panel_dims(rtree)

    HEAD = 150
    PANEL_PAD = 24
    PANEL_TOP = HEAD + 70
    gap = 56
    lpw = lw + 2 * PANEL_PAD
    rpw = rw + 2 * PANEL_PAD
    width = PANEL_PAD + lpw + gap + rpw + PANEL_PAD
    body_h = max(lh, rh)
    height = PANEL_TOP + body_h + 90

    o = []
    o.append(f'<svg xmlns="http://www.w3.org/2000/svg" width="{width:.0f}" height="{height:.0f}" viewBox="0 0 {width:.0f} {height:.0f}" font-family="ui-sans-serif,-apple-system,Segoe UI,Roboto,sans-serif">')
    o.append(f'<rect width="{width:.0f}" height="{height:.0f}" fill="#ffffff"/>')
    # Header
    o.append(f'<text x="{PANEL_PAD}" y="42" font-size="24" font-weight="800" fill="#0f172a">Where is the leak? A Worker postMessage leak, same process, two dumps</text>')
    o.append(f'<text x="{PANEL_PAD}" y="70" font-size="14" fill="#475569">Sample: every 100 ms the page posts a 1 KB ArrayBuffer to a worker that never drains its queue. The copies pile up in C++, not in either JS heap.</text>')
    o.append(f'<text x="{PANEL_PAD}" y="92" font-size="14" fill="#475569">Each node shows its exact dominator-tree retained size and type; the tree is topological top-to-bottom; cycle members are marked (↺) and keep their true retained size.</text>')
    # Legend
    lx = PANEL_PAD
    for label, bd, fl in [("worker / message queue", "#be123c", "#fff1f3"),
                          ("cycle member", "#b45309", "#fff7ed"),
                          ("other JS / C++", "#334155", "#f8fafc"),
                          ("rest of heap (collapsed)", "#cbd5e1", "#f1f5f9")]:
        o.append(f'<rect x="{lx}" y="112" width="16" height="16" rx="3" fill="{fl}" stroke="{bd}" stroke-width="2"/>')
        o.append(f'<text x="{lx+22}" y="125" font-size="12.5" fill="#334155">{label}</text>')
        lx += 36 + 7.2 * len(label) + 24

    # Panel frames + titles
    lpx = PANEL_PAD
    rpx = PANEL_PAD + lpw + gap
    frame_top = HEAD + 6
    frame_h = body_h + PANEL_TOP - frame_top + 58
    for px, pw, title, sub, color in [
        (lpx, lpw, "Normal JSC heap dump", "main-VM GC snapshot · Web Inspector format", "#1d4ed8"),
        (rpx, rpw, "mya heap walk", "whole WebContent process, incl. C++ off the heap", "#be123c")]:
        o.append(f'<rect x="{px:.0f}" y="{frame_top:.0f}" width="{pw:.0f}" height="{frame_h:.0f}" rx="12" fill="#ffffff" stroke="{color}" stroke-width="1.5"/>')
        o.append(f'<text x="{px+PANEL_PAD:.0f}" y="{frame_top+30:.0f}" font-size="18" font-weight="700" fill="{color}">{title}</text>')
        o.append(f'<text x="{px+PANEL_PAD:.0f}" y="{frame_top+50:.0f}" font-size="12.5" fill="#64748b">{sub}</text>')

    emit_tree(ltree, lpx + PANEL_PAD, PANEL_TOP, o)
    emit_tree(rtree, rpx + PANEL_PAD, PANEL_TOP, o)

    # Verdicts
    vy = frame_top + frame_h - 44
    o.append(f'<rect x="{lpx+PANEL_PAD-8:.0f}" y="{vy:.0f}" width="{lpw-2*PANEL_PAD+16:.0f}" height="34" rx="8" fill="#eff6ff"/>')
    o.append(f'<text x="{lpx+PANEL_PAD:.0f}" y="{vy+22:.0f}" font-size="13.5" font-weight="600" fill="#1e3a8a">Leak NOT found: Worker retains 605 B; no message-queue node exists at all.</text>')
    o.append(f'<rect x="{rpx+PANEL_PAD-8:.0f}" y="{vy:.0f}" width="{rpw-2*PANEL_PAD+16:.0f}" height="34" rx="8" fill="#fff1f3"/>')
    o.append(f'<text x="{rpx+PANEL_PAD:.0f}" y="{vy+22:.0f}" font-size="13.5" font-weight="600" fill="#9f1239">Leak found: WorkerDedicatedRunLoop holds a Deque of 376 undrained Tasks (39.8 KB).</text>')

    o.append("</svg>")
    with open(f"{base}/sidebyside.svg", "w") as f:
        f.write("\n".join(o))
    print(f"wrote {base}/sidebyside.svg  ({width:.0f}x{height:.0f})")
    print(f"normal: {lcells} cells, total {H.human_bytes(ltot)}")
    print(f"mya:    {rcells} cells, total {H.human_bytes(rtot)}")


if __name__ == "__main__":
    main()
