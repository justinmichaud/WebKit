#!/usr/bin/env python3
"""Config-driven side-by-side: normal JSC heap dump vs mya, for a named example.
Only the interesting subtree of each heap is drawn; the rest is one honest box.
Retained sizes are exact dominator-tree sizes (root retained == total shallow);
cycle members are marked but keep their true retained size.

Usage: python3 sbs.py <example>   where <example> in CONFIG.
"""
import sys
import heapsvg as H

NODE_W, NODE_H, H_GAP, V_GAP = H.NODE_W, H.NODE_H, H.H_GAP, H.V_GAP

CONFIG = {
    "worker": {
        "file": "worker_sidebyside.svg",
        "header": "Where is the leak?  Worker postMessage leak, same process, two dumps",
        "sample": "Sample: every 100 ms the page posts a 1 KB ArrayBuffer to a worker that never drains its queue. The copies pile up in C++, not in either JS heap.",
        "left": {"json": "worker/normal.json", "pins": ["#432", "#437"],
                 "title": "Normal JSC heap dump", "sub": "main-VM GC snapshot · Web Inspector format",
                 "annot": {}, "verdict": "Leak NOT found: Worker retains 605 B; no message-queue node exists at all."},
        "right": {"json": "worker/mya.json", "pins": ["#16695", "#31692"],
                  "title": "mya heap walk", "sub": "whole WebContent process, incl. C++ off the heap",
                  "annot": {}, "verdict": "Leak found: WorkerDedicatedRunLoop holds a Deque of 376 undrained Tasks (39.8 KB)."},
    },
    "detached": {
        "file": "detached_sidebyside.svg",
        "header": "Where is the leak?  Detached DOM subtree + event-listener leak, same process, two dumps",
        "sample": "Sample: every 100 ms a div of 20 spans (each a click listener closing over Uint16Array(1000)) is added to the page, removed, and kept in a `detached` array.",
        "left": {"json": "detached/normal.json", "pins": ["#661", "#10323"],
                 "title": "Normal JSC heap dump", "sub": "main-VM GC snapshot · Web Inspector format",
                 "annot": {661: "105 div wrappers — dead end", 10301: "1 of 2122 scattered listener roots"},
                 "verdict": "Leak NOT attributable: 4.3 MB is split across 2122 listener roots; detached retains 4.9 KB, its div wrappers have no edge to the spans."},
        "right": {"json": "detached/mya.json", "pins": ["#14955", "#35822", "#35823"],
                  "title": "mya heap walk", "sub": "whole WebContent process, incl. C++ off the heap",
                  "annot": {14955: "119 div wrappers", 35822: "1 of 105 detached subtrees (6.6 MB total)", 35823: "1 of 20 spans"},
                  "verdict": "Leak found: the detached DOM subtrees are reconstructed — div → 20 spans → listener → Uint16Array, 6.6 MB in all."},
    },
}


def focused_tree(path, pins, annot):
    snap = H.load_snapshot(path)
    dom = H.compute_retained(snap)
    tree = H.build_tree(snap, dom, 0, 10**15, 6, 80, pins)

    def tag(node):
        if node["id"] in annot:
            node["annot"] = annot[node["id"]]
        for c in node["children"]:
            tag(c)
    tag(tree)
    return tree, dom["retained"].get(0, 0), len(snap["nodes"]) - 1


def shorten(nm):
    if "unique_ptr<" in nm and "::Task" in nm:
        return "Deque<unique_ptr<...Task>>"
    nm = nm.replace("WebCore::", "").replace("JSC::", "").replace("WTF::", "")
    return nm[:31] + "…" if len(nm) > 32 else nm


def collapse_head(node):
    if node["id"] == 0:
        return f'rest of heap: +{node["collapsed"]["count"]} subtrees'
    nm = node["name"]
    if "RunLoop" in nm or "Deque" in nm or "Task" in nm:
        return f'+{node["collapsed"]["count"]} more (the queued Tasks)'
    if "HTMLDivElement" in nm:
        return f'+{node["collapsed"]["count"]} more spans'
    if "HTMLSpanElement" in nm or "SpanElement" in nm:
        return f'+{node["collapsed"]["count"]} (listener + Uint16Array)'
    if node["name"] == "Array" or "JSArray" in nm:
        return f'+{node["collapsed"]["count"]} div wrappers (dead end)'
    return f'+{node["collapsed"]["count"]} smaller subtrees'


def emit_tree(tree, ox, oy, out):
    H.layout(tree)

    def edges(node):
        for c in node["children"]:
            x1, y1 = ox + node["_x"] + NODE_W / 2, oy + node["_y"] + NODE_H
            x2, y2 = ox + c["_x"] + NODE_W / 2, oy + c["_y"]
            m = (y1 + y2) / 2
            out.append(f'<path d="M{x1:.1f},{y1:.1f} C{x1:.1f},{m:.1f} {x2:.1f},{m:.1f} {x2:.1f},{y2:.1f}" fill="none" stroke="#cbd5e1" stroke-width="1.5"/>')
            edges(c)
        if node["collapsed"]:
            x1, y1 = ox + node["_x"] + NODE_W / 2, oy + node["_y"] + NODE_H
            x2, y2 = ox + node["_collapsed_x"] + NODE_W / 2, oy + node["_y"] + NODE_H + V_GAP
            m = (y1 + y2) / 2
            out.append(f'<path d="M{x1:.1f},{y1:.1f} C{x1:.1f},{m:.1f} {x2:.1f},{m:.1f} {x2:.1f},{y2:.1f}" fill="none" stroke="#e2e8f0" stroke-width="1.5" stroke-dasharray="4 3"/>')

    def nodes(node):
        x, y = ox + node["_x"], oy + node["_y"]
        border, fill = H.color_for(node["name"], node["cyclic"])
        sw = 3.5 if node.get("pinned") else 2
        out.append(f'<rect x="{x:.1f}" y="{y:.1f}" width="{NODE_W}" height="{NODE_H}" rx="7" fill="{fill}" stroke="{border}" stroke-width="{sw}"/>')
        nm = shorten(node["name"])
        rtxt = f'retained {H.human_bytes(node["retained"])}'
        if node["cyclic"]:
            rtxt = f'↺ cycle · retained {H.human_bytes(node["retained"])}'
        out.append(f'<text x="{x+10:.1f}" y="{y+21:.1f}" font-size="13" font-weight="600" fill="{border}">{H.html.escape(nm)}</text>')
        out.append(f'<text x="{x+10:.1f}" y="{y+38:.1f}" font-size="12" fill="#475569">{H.html.escape(rtxt)}</text>')
        out.append(f'<text x="{x+NODE_W-10:.1f}" y="{y+38:.1f}" font-size="10.5" fill="#94a3b8" text-anchor="end">self {H.human_bytes(node["shallow"])}</text>')
        if node.get("annot"):
            out.append(f'<text x="{x+NODE_W/2:.1f}" y="{y-6:.1f}" font-size="11" font-style="italic" fill="#64748b" text-anchor="middle">{H.html.escape(node["annot"])}</text>')
        for c in node["children"]:
            nodes(c)
        if node["collapsed"]:
            cx, cy = ox + node["_collapsed_x"], oy + node["_y"] + NODE_H + V_GAP
            out.append(f'<rect x="{cx:.1f}" y="{cy:.1f}" width="{NODE_W}" height="{NODE_H}" rx="7" fill="#f1f5f9" stroke="#cbd5e1" stroke-width="1.5" stroke-dasharray="4 3"/>')
            out.append(f'<text x="{cx+10:.1f}" y="{cy+21:.1f}" font-size="12.5" font-weight="600" fill="#64748b">{H.html.escape(collapse_head(node))}</text>')
            out.append(f'<text x="{cx+10:.1f}" y="{cy+38:.1f}" font-size="12" fill="#64748b">retained {H.human_bytes(node["collapsed"]["retained"])}</text>')

    edges(tree)
    nodes(tree)


def main():
    name = sys.argv[1]
    base = sys.argv[2] if len(sys.argv) > 2 else "."
    cfg = CONFIG[name]
    L, R = cfg["left"], cfg["right"]
    lt, ltot, lc = focused_tree(f"{base}/{L['json']}", L["pins"], L["annot"])
    rt, rtot, rc = focused_tree(f"{base}/{R['json']}", R["pins"], R["annot"])
    lw, lh = H.layout(lt)
    rw, rh = H.layout(rt)

    HEAD = 150
    PAD = 24
    TOP = HEAD + 78
    gap = 56
    lpw, rpw = lw + 2 * PAD, rw + 2 * PAD
    width = PAD + lpw + gap + rpw + PAD
    body_h = max(lh, rh)
    height = TOP + body_h + 86

    o = [f'<svg xmlns="http://www.w3.org/2000/svg" width="{width:.0f}" height="{height:.0f}" viewBox="0 0 {width:.0f} {height:.0f}" font-family="ui-sans-serif,-apple-system,Segoe UI,Roboto,sans-serif">']
    o.append(f'<rect width="{width:.0f}" height="{height:.0f}" fill="#ffffff"/>')
    o.append(f'<text x="{PAD}" y="42" font-size="23" font-weight="800" fill="#0f172a">{H.html.escape(cfg["header"])}</text>')
    o.append(f'<text x="{PAD}" y="70" font-size="14" fill="#475569">{H.html.escape(cfg["sample"])}</text>')
    o.append(f'<text x="{PAD}" y="92" font-size="14" fill="#475569">Each node shows its exact dominator-tree retained size and type; tree is topological top-to-bottom; cycle members marked (↺) with their true retained size.</text>')
    lx = PAD
    for label, bd, fl in [("worker / message queue / leak family", "#be123c", "#fff1f3"),
                          ("cycle member", "#b45309", "#fff7ed"),
                          ("other JS / C++", "#334155", "#f8fafc"),
                          ("rest of heap (collapsed)", "#cbd5e1", "#f1f5f9")]:
        o.append(f'<rect x="{lx}" y="112" width="16" height="16" rx="3" fill="{fl}" stroke="{bd}" stroke-width="2"/>')
        o.append(f'<text x="{lx+22}" y="125" font-size="12.5" fill="#334155">{label}</text>')
        lx += 36 + 7.1 * len(label) + 22

    lpx, rpx = PAD, PAD + lpw + gap
    ftop = HEAD + 6
    fh = body_h + TOP - ftop + 56
    for px, pw, panel, color in [(lpx, lpw, L, "#1d4ed8"), (rpx, rpw, R, "#be123c")]:
        o.append(f'<rect x="{px:.0f}" y="{ftop:.0f}" width="{pw:.0f}" height="{fh:.0f}" rx="12" fill="#ffffff" stroke="{color}" stroke-width="1.5"/>')
        o.append(f'<text x="{px+PAD:.0f}" y="{ftop+30:.0f}" font-size="18" font-weight="700" fill="{color}">{H.html.escape(panel["title"])}</text>')
        o.append(f'<text x="{px+PAD:.0f}" y="{ftop+50:.0f}" font-size="12.5" fill="#64748b">{H.html.escape(panel["sub"])}</text>')

    emit_tree(lt, lpx + PAD, TOP, o)
    emit_tree(rt, rpx + PAD, TOP, o)

    vy = ftop + fh - 44
    for px, pw, panel, fillc, txtc in [(lpx, lpw, L, "#eff6ff", "#1e3a8a"), (rpx, rpw, R, "#fff1f3", "#9f1239")]:
        o.append(f'<rect x="{px+PAD-8:.0f}" y="{vy:.0f}" width="{pw-2*PAD+16:.0f}" height="34" rx="8" fill="{fillc}"/>')
        o.append(f'<text x="{px+PAD:.0f}" y="{vy+22:.0f}" font-size="13" font-weight="600" fill="{txtc}">{H.html.escape(panel["verdict"])}</text>')

    o.append("</svg>")
    with open(f"{base}/{cfg['file']}", "w") as f:
        f.write("\n".join(o))
    print(f"wrote {base}/{cfg['file']} ({width:.0f}x{height:.0f})")
    print(f"left:  {lc} cells, total {H.human_bytes(ltot)}")
    print(f"right: {rc} cells, total {H.human_bytes(rtot)}")


if __name__ == "__main__":
    main()
