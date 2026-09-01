# Sprint 57 Allocation-Ownership Audit

**Reviewed:** 2026-08-31 on `arm64-macos`, against Sprint 57 amendment S57-A3.

This audit answers the checklist with the repository's actual ownership model.
It does not treat a different internal representation as a product goal. The
non-negotiable result is that hot editor paths stay allocation-free, bounded
setup paths grow geometrically, ownership has a teardown, and the sanitizer,
GC-stress and RSS gates remain intact.

| Subsystem | Result | Evidence and ownership conclusion |
|---|---|---|
| Fletch parse/compile | **Yes, with S57-A3 deferral** | `parse.c` allocates AST nodes/chunks from its caller arena; interned names and final bytecode live in the runtime arena. `compile.c` deliberately builds temporary bytecode/constants/line runs in geometric `Bytebuf`/`Vec` storage, copies the completed chunk into the arena, and frees every temporary on success and failure. Replacing this proven build-then-publish path is deferred absent a measured regression. |
| Fletch VM | **Yes** | `FlVm` uses fixed stack/call-frame arrays and the Sprint 30 GC heap. `fl_vm_free` releases transaction, root, trace, regex-cache and GC ownership. The whole-suite GC-stress and sanitizer lanes remain the lifetime proof; no per-call arena is invented in the audit. |
| terminal renderer | **Yes — mechanical** | `perf_alloc` warms the grid/output buffer, then runs 1,000 `yew_grid_fill` + `yew_render_frame` + flip cycles at **0 calls**. `perf_alloc_seed` links a test-only `yew_xmalloc(16)` into the renderer and proves that this row fails with its exact source site. |
| terminal grid | **Yes — mechanical** | Grid storage is acquired only by init/resize and released by `yew_grid_free`. The same 1,000-cycle renderer row exercises fill/flip after reset, so a per-cell or per-put allocation fails the zero-call gate. |
| piece tree | **Yes — mechanical** | Nodes come from `PieceNodeSlab`; add-buffer storage grows geometrically. The 1,000 sequential one-byte insertion row uses 2 calls against a limit of 40 on the reviewed run. |
| edit dispatch | **Yes — mechanical** | The editor owns a lifetime arena plus explicit subsystem storage. The 10,000-frame typing and navigation sessions reset after 100 warmups and both report **0 calls** across key dispatch and render. |
| syntax | **Yes — mechanical** | Definitions/cache storage is setup or buffer lifetime state. The warmed line matcher runs 1,000 times with **0 calls**. |
| UI layout | **Yes — mechanical** | Pane nodes are persistent topology allocated on split, not transient rectangles. The dedicated warmed `yew_ed_layout` row recomputes layout 1,000 times with **0 calls**. |
| workspace state | **Yes** | Save emission owns and frees a per-save arena. Parsed state owns `ed->state.doc`, frees it before replacement, and teardown disposes it. Repeated save/restore/schema tests run under sanitizer coverage; the closed-editor live-byte row remains the aggregate backstop. |
| search | **Yes — mechanical** | Compiled regex data uses the caller arena and `YewReWorkspace` owns reusable matcher state. The warmed matcher runs 1,000 times with **0 calls**. |
| LSP JSON/messages | **Yes** | `yew_json_parse` allocates nodes/strings only from the supplied arena. JSON-RPC notification handling and configuration parsing initialize and free one message-local arena on all exits. Persistent document change text belongs to the document arena and is reset after synchronization. |
| AI, Git/FUSS, plugins/package | **Yes** | Short-lived parse/request data uses local or pending-request arenas where useful; callback-surviving strings/buffers have explicit owners and teardown. Focused lifecycle tests plus full/minimal sanitizer and RSS gates cover those mixed lifetimes. A blanket conversion to arenas would be a redesign, not an audit correction. |

## Mechanical evidence recorded on the review commit

- allocation unit contract: 5 tests, 45 assertions, 0 failures;
- renderer positive control: seeded row fails and names `src/term/render.c`;
- real allocation rows: renderer 0, syntax 0, regex 0, warmed layout 0,
  typing 0, navigation 0, inserts 2/40, 100 MiB open 7/2,000, and both
  closed-live rows 0/65,536;
- shipping full/minimal RSS gates, sanitizer/alignment lanes, module-boundary
  checks, and policy bans remain independently required by the sprint.

## Deferral boundary

There is no finding here that justifies removing a Yew feature or rewriting a
stable core subsystem during footprint closeout. Fletch scratch migration,
arena reuse/reset, or alternative pane storage may be proposed only with a
measured size/allocation win, equivalent failure-path tests, and a separately
reviewed sprint. Sprint 58 may reopen a row if adversarial testing produces a
concrete leak, unbounded-growth trace, or invariant violation.
