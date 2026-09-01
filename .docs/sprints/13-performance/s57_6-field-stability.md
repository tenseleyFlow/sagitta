# Sprint 57.6: Field Stability — Binary Isolation and FUSS Interaction

## Prerequisites

- Sprint 8 — byte-exact file loading, `FileMeta.binary`, and binary rendering.
- Sprint 44 — the cooperative open-buffer/workspace symbol index and its
  per-turn latency budget.
- Sprint 46 — syntax-language-driven LSP startup and the rule that binary
  documents are never opened by a server.
- Sprints 52, 56.5, and 57.5 — FUSS tree construction, workspace-rooted
  all-files view, type-jump machinery, compact remembered trees, and drawer
  performance gates.
- Sprint 56 — `YEW_PROF`, worst-frame evidence, and the 5 ms interaction
  budget.
- Binding plans and invariants 1–5, 7, and 8. Binary editing and core editor
  behavior may not be removed to make a latency number pass.

## Goals

Close the field failures reproduced by opening an extensionless Mach-O file
from a directory workspace inside a larger Git repository. Binary bytes must
remain viewable, editable, and saveable byte-for-byte, but they must not be
syntax-scored as source, start a language server, or enter the source-symbol
index. No cooperative background task may hold an event-loop turn for seconds.

Make the FUSS tree strictly workspace-relative even when Git discovers an
ancestor repository. Status data outside the workspace must never fabricate a
row in the drawer. Finally, restore reference-facsimile type-to-jump behavior:
ordinary filename characters search the effective visible tree directly;
printable Git/file verbs move behind a prefix chord.

This is a pre-audit stabilization sprint. Sprint 58 has not begun and its
baseline must be refreshed after this sprint closes. No Sprint 58 audit result
may cite the pre-57.6 tree.

## Field reproducer and work order

The binding reproducer is:

```text
./build/yew ~/scratch/wolf/starting/
open ~/scratch/wolf/starting/encode from FUSS
```

The captured profiler showed dispatch-only frames of 0.8–4.3+ seconds while
rendering stayed below 2 ms. `encode` was labelled `fortran`, attempted to
spawn `fortls`, and caused the open-buffer symbol index to scan opaque bytes
one unsliced line at a time. Git porcelain, rooted at
`~/scratch`, also returned `wolf/`, which was merged into the tree rooted at
`~/scratch/wolf/starting` without coordinate translation.

Work and commits proceed in this order:

1. binary syntax/LSP/symbol-index isolation and latency regression;
2. ancestor-Git-root to workspace-root path normalization;
3. direct visible-tree type-to-jump and prefixed action bindings;
4. full proportional validation, field replay, docs, and Sprint 58 baseline
   handoff.

## Deliverables

### 1. Binary consumers fail closed without gutting binary editing

- `yew_ed_syn_bind` binds `YEW_LANG_NONE` for `meta.binary` before extension,
  shebang, or ambiguous-Fortran scoring. A stale previous language binding is
  detached and `Buffer.lang` becomes `NULL`.
- LSP automatic and direct startup both reject a binary buffer before client
  allocation, root resolution, document attachment, or process spawn. The
  existing sync-layer binary check remains as defense in depth.
- Open-buffer and workspace symbol scans emit no entries for binary buffers.
  Binary buffers do not seed pending work, and edit notifications do not create
  a binary buffer index. A stale index is dropped defensively.
- Binary display, motion, search, edits, undo, journal, and save behavior are
  unchanged. There is no new size cap, read-only mode, hex-view substitution,
  dependency, or lossy decoding.
- A profiler replay of the field fixture has no multi-second dispatch turn and
  starts no `fortls` process. Paint timing alone is not accepted as proof.

### 2. Git rows are normalized to the workspace coordinate system

- The Git repository root and editor workspace root may differ. Every porcelain
  path is translated through their canonical relative prefix before it reaches
  `FussTree`.
- Rows outside the workspace are filtered. A status row naming an ancestor or
  sibling can never become a top-level workspace node.
- A Git row that represents the workspace root itself decorates the real
  workspace descendants or is omitted; it never fabricates a selectable row
  named after an ancestor directory.
- All-files filesystem enumeration remains authoritative for tree membership.
  Git supplies status decoration and Git-only paths that genuinely resolve
  inside the workspace.
- Tests cover workspace == Git root, workspace below Git root, sibling dirty
  content, collapsed untracked directory output, spaces, and prefix-collision
  paths such as `wolf/start` versus `wolf/starting`.

### 3. Bare filename characters drive visible-tree type-to-jump

- In F mode, an unmodified printable filename character begins type-to-jump
  immediately; `/` arming is no longer required. The 500 ms reset/replace
  window, stay-on-current exact match, deterministic scoring, and self-clearing
  footer hint remain.
- Candidates are exactly `FussTree.items`, the effective flattened tree after
  collapse, manual expansion memory, and open-file ancestry. Hidden descendants
  of collapsed directories cannot match.
- Structural keys remain direct: arrows, page/home/end, Enter, Space, Escape,
  `C-w` split chords, and refresh. Backspace edits an active jump pattern.
- Printable Git/file/tree actions move behind a `C-g` prefix, matching the
  reference facsimile interaction. This includes status verbs, destructive
  verbs, group-from-directory, all-files and hidden toggles, and an explicit
  prefixed leave action. Escape always leaves F mode.
- Panic defaults, shipped `runtime/init.fl`, round-trip command metadata,
  footer hints, unit dispatch tables, PTY goldens, and user-facing key docs are
  updated together. There is no interval where the UI advertises the retired
  `/` arm behavior.

## Testing strategy

- Unit: extensionless NUL-bearing input remains language-less; direct and
  automatic LSP startup allocate/spawn nothing; symbol scan and pump stay empty
  for binary buffers, including edits and a pre-existing stale index.
- Perf/profile: a deterministic long-line binary fixture is opened and pumped
  under the normal event-loop budget. The regression fails if one pump consumes
  an unsliced seconds-long scan or if dispatch exceeds the existing interaction
  gate.
- Unit/FUSS: table-driven Git/workspace-root normalization and tree merge cases;
  direct type-jump for letters, digits, `.`, `_`, and `-`; collapsed descendants
  excluded; expanded descendants included; every `C-g` action chord resolves to
  the former command.
- PTY: the field workspace has no spurious `wolf` row, binary status has no
  `fortran`/`fortls`, arrows remain responsive, direct typing moves selection,
  the jump hint expires, and prefixed actions remain discoverable.
- Sanitizers and fuzz: focused syntax/symbol/LSP/FUSS tests under ASan/UBSan,
  then the existing FUSS fuzz and deterministic PTY lanes.

## Definition of Done

1. The exact field reproducer no longer starts `fortls`, labels `encode` as a
   source language, or produces a seconds-long dispatch frame.
2. Binary bytes remain byte-identical through open/edit-undo/save coverage; no
   core binary/editor feature was excised.
3. No FUSS item can resolve outside `yew_ws_root(ed)`, and the nested-workspace
   regression contains no fabricated `wolf` row.
4. Bare filename characters type-jump across visible rows only. Printable
   actions are reachable behind `C-g`; every moved binding has a firing test.
5. Focused unit/perf/PTY/sanitizer suites pass with both module-enabled and
   applicable minimal builds; existing size and latency gates do not regress.
6. Handoff and sprint index name the post-57.6 Sprint 58 baseline requirement.

## Closeout — REPOSITORY COMPLETE 2026-09-01

The field reproducer is repaired without narrowing binary editing. The
546,232-byte arm64 Mach-O `encode` fixture now opens as `utf-8 bin`, binds no
source language, starts no `fortls` process, and contributes no work or stale
entries to the source-symbol index. Its display, edit, undo, and byte-exact
save paths remain the existing core paths. The reproduced pre-fix stall was
dispatch work, not painting: frames spent 0.8–4.3+ seconds scanning one opaque
binary line. The post-fix replay measured 4.846 ms p99 / 11.667 ms maximum
dispatch during idle/background sampling and 1.049 ms maximum rendering, with
no multi-second frame.

FUSS now translates ancestor-repository porcelain paths into workspace
coordinates and filters siblings and ancestors before tree construction. A
live replay rooted at `~/scratch/wolf/starting` contained only the real
workspace entries and no fabricated `wolf` row. Bare printable filename
characters except Space now search the effective visible flattened tree with
the existing 500 ms accumulation window; collapsed descendants are excluded.
Printable file, Git, and tree actions remain keyboard reachable behind the
facsimile-style `C-g` prefix, while arrows, paging, Enter, Space, Escape,
refresh, and split chords remain direct.

Implementation landed in `ecab2265`, `31810f28`, and `d0f893a3`; the sprint
contract and downstream chord-test follow-ups landed in `a6fcf777` and
`3a6070c9`.

Local closeout evidence on Darwin arm64:

- the complete default-module `make test` product suite is green: all PTYs,
  Fletch 38/38, scripts 93 tests / 919 assertions, package integration 51/51,
  2,000 round-trip seeds, fuzz corpora, 432 syntax assets, policy/smoke/torture
  gates, and 2,414 unit tests / 71,033,068 assertions pass;
- all 29 FUSS PTY cases pass twice; focused binary syntax/LSP/symbol-index,
  nested-workspace, type-jump, drawer, navigation, expansion, chord, command,
  and group-from-directory tests pass;
- focused Apple-clang ASan/UBSan tests pass for binary symbol isolation,
  binary LSP isolation, nested workspaces, and all 13 FUSS jump cases; a fresh
  sanitized build is warning-clean;
- the deterministic FUSS fuzz campaign passes 20,000 iterations at seed
  `0x57f6`, corpus 5, hash `5fdbbd63231c605c`;
- the 20,000-node FUSS performance rows remain inside their 5/12 ms budgets:
  2.186 ms build+flatten, 0.001 ms navigation p99, 0.020 ms toggle p99,
  2.614 ms drawer entry, and 1.408 ms open resolution;
- native `make size` passes all six shipping profiles: 1,452,640 bytes
  minimal, 1,891,504 full, 1,555,088 LSP-only, 1,588,496 AI-only, 1,586,752
  FUSS-only, and 1,536,096 plugins-only. A separate FUSS-only shipping build
  and its product smoke suite are green.

This host has Apple clang only; `/usr/bin/gcc` is its clang compatibility
driver. True GNU GCC, Linux Valgrind/musl, x86_64, and hosted CI evidence are
therefore not inferred and remain pushed-SHA evidence tails. Sprint 58 must
establish its audit baseline from a post-57.6 commit with those required lanes
green; no audit result may cite the pre-57.6 tree.
