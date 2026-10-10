# mya vs. JSC's own heap snapshot: two leaks

Two pages, each run in the Debug MiniBrowser on this Mac and dumped twice from the same
WebContent process: first by JSC (`notifyutil -p com.apple.WebKit.dumpGCHeap`, which collects),
then right after by `mya heapdump --collector`, which reads JSC's dump for which references the
collector marks through. `heapchart.py` draws both by the same rules, side by side.

| demo | chart | what the page does |
| --- | --- | --- |
| `detached/` | `detached/chart.svg` | every tick: a div of 20 spans, each with a click listener closing over a `Uint16Array(1000)`; the div is added, removed, and kept in `const detached = []` |
| `worker/` | `worker/chart.svg` | every tick: `postMessage({ id, data: new ArrayBuffer(1000) })` to a worker that spins forever and never takes a message |

The timers run at about 1 to 2 Hz, not every 100 ms: MiniBrowser throttles a page's timers when
its window is not frontmost. The counts below are what the dumps contain.

## Detached subtree (90 s)

- **JSC** (4.5 MB): 1982 `Function`s retain 93.5% of the heap, through their scopes, 1960
  `Uint16Array`s. Each is a child of `<root>`, held by `<root>` and by its span's wrapper. The
  leaked bytes are visible; what holds them is not: no node above the functions leads to `detached`.
- **mya** (10.3 MB, the whole process): `detached`, the `JSC::JSArray`, holds **6.4 MB, 65.5%**,
  the largest subtree: 101 div wrappers, 101 `HTMLDivElement`s, 2011 spans, their 1951 listener
  functions (held by the spans' wrappers, which the collector marks), their scopes, and the typed
  arrays' 5.0 MB of storage. Under the array, "held by" names its two holders:
  - `JSGlobalLexicalEnvironment`: **`detached`**, the global binding;
  - `FunctionCodeBlock`: the setInterval callback's DFG code, whose `m_weakReferences` the collector
    marks while the code block lives (`CodeBlock::stronglyVisitWeakReferences`).

  The array is drawn as a cycle, 0 B retained: the page's interval holds it. The document owns its
  timers (`Ref<DOMTimer>`), the timer's `ScheduledAction` holds the callback in a `JSC::Strong`,
  the callback's scope is the global scope, which holds `detached`, and the global object holds the
  document's wrapper, which owns the document. Neither the collector nor reference counting frees
  that loop; `clearInterval`, or clearing `detached`, does.

## Worker (300 s)

- **JSC** (178 KB, the main VM): no node for any queued message.
- **mya** (6.1 MB): the worker's `DedicatedWorkerThread` holds **1.4 MB, 23.4%**: its run loop's
  `Deque` of 546 `Task`s, their closures, 545 `SerializedScriptValue`s, and their copies of the
  `ArrayBuffer`s. It is a reference cycle, and drawn as one: each queued closure's `protectedThis`
  is a `Ref<WorkerMessagingProxy>`, which holds the thread (`RefPtr`), which owns the run loop
  (`unique_ptr`), whose queue owns the task. While the worker never drains, the queued messages
  keep alive what owns them.

## How the charts are drawn (`heapchart.py`)

Nothing is picked by hand: no node ids, no class names, no verdict text.

- A node's retained size is its dominator-tree retained size; its parent is its immediate
  dominator, the link whose removal frees it.
- Same-class siblings are one group; a group's children are its members' children, grouped
  again. A member dominated by a member of its own class (a sibling list) is folded into it.
- Each level shows the groups that retain at least 2% of the heap, at most 4; the rest is one
  "other" box with their total.
- A cycle is a strongly connected set over strong edges that includes a C++ object: memory that
  neither the collector nor reference counting can free. Its nodes retain 0 B and add nothing to
  their ancestors; the box shows what the cycle holds, which is what a level picks it by. A cycle
  of JS cells alone is not one, since the collector frees it.
- A group whose parent in the tree does not itself point to it, because several holders share it,
  lists what does ("held by"): each holder's class and the edge's name.

## What changed in mya

All in `Source/JavaScriptCore/corpse/` and `Source/JavaScriptCore/mya/mya.cpp`:

1. **Opaque roots are not references.** The collector's opaque-root set from its last marking gave
   every detached div a second retainer. No edge leaves it.
2. **A C++ pointer owns only through an owning type.** `Ref`, `RefPtr`, `UniqueRef`, `unique_ptr`,
   `RetainPtr` and container storage own; a raw pointer or a `CheckedPtr` (such as
   `Node::m_parentNode`) does not, and is a weak edge.
3. **A stack word, a stale `JSValue` or a free handle slot owns no C++ object.** They reach one
   only as the last resort.
4. **Generic lambdas.** A closure's type is found from `this` in one of its member functions; a
   generic lambda's `operator()` symbol does not demangle, so its destructor and move constructor
   are tried too. The worker's postMessage closure is one.
5. **The collector's snapshot decides what a cell's C++ storage holds.** Memory does not show which
   traced slots a cell visits. `heapdump --collector <JSC's snapshot>` reads them from JSC's own
   record: where a cell that owns a C++ object marked a cell through it, the dump has a strong edge
   from that cell, named as JSC names it (the variable `detached`). The slot itself stays weak, so
   another owner of shared storage, such as an unlinked code block, holds nothing through it.

`testLibJSCTools` covers each: a generic-lambda capture in `HeapWalk`; parent and child ownership
and opaque roots in `Leaks` (its mock node holds its parent in a `CheckedPtr`); and in `HeapDump`,
that given JSC's snapshot, every edge between cells in it is a strong path in mya's dump (31 are
not without it). Each new assertion fails against the unchanged sources. The suite passes in
`run-javascriptcore-tests --testlibjsctools` and in 10 of 10 repeated full runs; unchanged mya
failed its own detached-subtree dominance check in 3 of 3.

## Limits

- The JSC dump and mya's snapshot are taken one after the other; mya's has up to a few more ticks.
  Those cells are in no collector edge, and their references are read from memory alone.
- An edge from the collector's snapshot is named as JSC names it, which is nothing for its internal
  edges: then by the C++ slot, which names its type, not its member.
- The closure lookup needs an out-of-line destructor or move constructor, which an optimized build
  may inline.

## Reproduce

    ./capture.sh detached 90
    ./capture.sh worker 300
    python3 heapchart.py detached/capture/jsc.json detached/capture/mya.json -o detached/chart.svg --max-children 4
    python3 heapchart.py worker/capture/jsc.json worker/capture/mya.json -o worker/chart.svg --max-children 4

`superseded/` holds the previous charts and scripts, kept for comparison.
