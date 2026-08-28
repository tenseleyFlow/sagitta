# Sprint 56.5: Workspace Invocation, the FUSS Drawer, and Honest Suggestions

## Prerequisites

- Sprint 18 — `YewArgs`, positional file parsing, the named-command registry,
  command chords, and the reusable command line.
- Sprints 22–25 — pane trees, stable tabs, tab groups, and the binding rule
  **dir = workspace**. `yew_ws_key`, `yew_state_open`, `yew_ws_restore`, and
  `yew_ws_save` remain the only persistence path; this sprint does not add a
  database or a second session format.
- Sprint 26 — the deterministic workspace walk and fuss scorer. Directory
  discovery uses `yew_walk_*`; it never shells out to `find`.
- Sprint 43 — `Shadow`, `ShadowLayout`, `yew_shadow_layout`, and
  `yew_shadow_draw`. Its historical overlay-never-push rule is superseded by
  this sprint's insertion-preview rule because the shipped overlay can erase
  real cells at the cursor.
- Sprints 44/47/49 — symbol, LSP, and AI providers all feed the same shadow
  surface. Provider arbitration, generation checks, cancellation, and the
  explicit accept path do not change.
- Sprints 51–53 — `GitSnapshot`, `FussTree`, `FussMode`, Git verbs, editor
  views, and `ed.group.from_dir`. Git decoration is optional; a workspace tree
  is useful even when the directory is not a Git repository.
- Sprint 56 — in-loop profiling, the 5 ms keypress-to-paint gate, advisory
  hosted measurements, and the rule that deterministic rows remain hard on
  every machine.
- Binding: `00-decisions.md` workspace, shadow-completion, FUSS, layout, and
  module rows; invariants 1, 3, 4, 5, 8, and 9.

## Goals

Repair the three interaction seams found during dogfooding before Sprint 57
freezes binary and allocation budgets. Positional directory and file launches
must resolve one deterministic workspace, restore its saved tabs/panes/cursors,
and apply explicit CLI targets without replacing unrelated state. F mode becomes
the compact workspace drawer: it opens from the left over the current tab,
works outside Git repositories, and opens files into tabs or deliberate splits.
Shadow text becomes an honest preview of insertion: it never clears existing
document cells, never appears merely because indentation is whitespace, and
remains inside the existing latency budget on large source files.

This sprint changes interaction policy, not storage schema. Workspace-state
merge between simultaneous yew processes remains post-1.0. Project-wide LSP
`workspace/symbol`, nonliteral incremental regex preview, and the Sprint 57
size/embedded work remain with their existing owners.

## Deliverables

### 1. One startup-target resolver

`src/edit/ed.c` owns one pre-driver resolution pass. Argument syntax remains in
`src/args.c`; filesystem identity does not move into the parser.

```c
typedef enum YewStartKind {
    YEW_START_RESUME = 0,
    YEW_START_FILES,
    YEW_START_DIRECTORY
} YewStartKind;

typedef struct YewStartPlan {
    YewStartKind kind;
    char workspace[PATH_MAX];
    const char *const *files;
    size_t nfiles;
    bool enter_fuss;
} YewStartPlan;
```

The resolver implements this table exactly:

| Invocation | Workspace | Restore | Explicit target |
|---|---|---|---|
| `yew` | startup cwd | yes | none |
| `yew .` | canonical `.` | yes | enter F drawer |
| `yew DIR` | canonical `DIR` | yes | enter F drawer |
| `yew FILE...` | startup cwd | yes | open/focus each file as a tab |
| `yew --workspace DIR FILE...` | canonical `DIR` | yes | open/focus each file as a tab |

- `--workspace` wins over cwd. A positional directory beside another
  positional target is rejected before terminal entry; it is never guessed to
  be a file.
- Restore happens before explicit files are applied. A restored tab with the
  same canonical path is focused, not duplicated. A new explicit file becomes
  a new tab and the final explicit file is active.
- A directory launch records `enter_fuss`; it does not fabricate a scratch
  filename or a tab group containing every file.
- `MODULES=""` still opens the workspace and restores normally. Its automatic
  drawer request reports the canonical FUSS-module diagnostic once and leaves a
  usable L-mode editor; an excluded module never becomes a silent success.

**Pitfall:** do not derive the workspace from the first file's parent. The
locked Sprint 25 identity is startup cwd unless `--workspace` or a positional
directory explicitly selects another directory. This also makes multiple files
from different directories deterministic.

### 2. Directory launch is an all-files workspace tree

`src/mod/git/fussmode.c` separates tree population from Git decoration.

- `yew DIR` and `yew .` start the existing budget-sliced `yew_walk_*` over the
  canonical workspace root and show all nonignored files. Git status merges by
  workspace-relative path when a snapshot exists.
- A non-repository is a normal workspace tree. `ed.git.init` remains available,
  but lack of `.git` no longer replaces the tree with an init-only screen.
- The drawer title is `basename(realpath(workspace))`; the full path stays in
  `:ws info` and accessibility/help text, not the compact heading.
- Directory rows begin expanded through the first useful level on a directory
  launch. Collapsed paths, selection, `all_files`, and hidden-file policy retain
  their existing workspace-state persistence.
- Ordering remains stable: directories before files, case-folded name, then
  bytewise tie-break. No filesystem enumeration order reaches the grid.

### 3. F mode is a left drawer, not a replacement layout

Entering F preserves the active tab's `Pane *root`, focused `Win`, viewports,
and cursor sets. The drawer is a render/input layer; it is not serialized as a
pane and leaving it performs no pane-tree restoration.

```c
u16 yew_fuss_drawer_width(u16 content_cols);
Rect yew_fuss_drawer_rect(const Ed *ed);
Rect yew_fuss_backdrop_rect(const Ed *ed);
```

Width is deterministic: `content_cols / 6`, clamped to a 20-cell minimum and a
48-cell maximum; when the content is narrower than 48 cells the drawer may use
half the width. The selected row clips from the left only after preserving the
status glyph column and final path component.

Draw order:

1. draw the ordinary tab and pane tree;
2. dim the content rectangle without changing its glyph bytes;
3. fill and draw the opaque left drawer;
4. draw F footer/help and register drawer hit regions.

Only cells intersecting the drawer or backdrop-dim damage are invalidated.
Moving the F selection must not mark every editor row dirty.

**Pitfall:** copying or replacing the live pane root makes tabs, panes, cursors,
and deferred buffers diverge from workspace state. The drawer holds no editor
layout ownership.

### 4. Opening from F is explicit about destination

The command surface is:

| Command | Default key | File result | Directory result |
|---|---|---|---|
| `ed.git.open` | `Enter` | open/focus in a new tab, leave F | toggle expansion |
| `ed.git.open_split_h` | `C-w s` | horizontal split in the existing tab, leave F | error |
| `ed.git.open_split_v` | `C-w v` | vertical split in the existing tab, leave F | error |
| `ed.git.view` | `Space` | existing read-only preview | toggle expansion |

- The split commands resolve and validate the selected workspace-relative path
  before mutating layout. A failed open leaves F active and the pane tree
  byte-identical.
- New-tab open never replaces the source tab. If the path already has a tab,
  focus it rather than create an accidental duplicate.
- Split open uses the existing `yew_pane_split` transaction and normal buffer
  sharing rules. It creates no FUSS-owned viewer pane.
- Mouse double-click invokes the same `ed.git.open` command; keyboard remains
  the authority.

### 5. Shadow text previews insertion without erasing text

`src/ui/shadowdraw.c` replaces the destructive row fill with composition over
the cells already produced by `src/ui/draw.c`.

- A single-line ghost at a mid-line cursor is drawn at the cursor and the real
  suffix is shifted right by the ghost's cell width, clipping only at the pane
  edge. Wide-cell continuation markers move with their owning glyph.
- A multi-line ghost is eligible mid-line only when the real suffix after the
  cursor is empty. At end-of-line, document rows below the cursor shift down by
  the ghost's virtual-row count for the preview; real rows are never painted
  over by ghost cells.
- The buffer, cursor, viewport, undo tree, marks, and syntax state remain
  untouched. Accept remains the only path to `yew_edit_insert`.
- Ghost cells keep provider styling. Shifted document cells retain their exact
  original attrs, regions, and glyph bytes.
- Composition uses bounded row scratch owned by `ShadowLayout`/draw state;
  there is no heap allocation per frame.

The eligibility rule in `src/edit/shadow.c` becomes:

1. line end is eligible;
2. an all-whitespace suffix is eligible;
3. a non-whitespace suffix is eligible only with `shadow.midline=true` and a
   single-line suggestion;
4. cursor-at-indentation is not by itself eligibility.

Delivery at an ineligible position is retained only long enough for provider
statistics, then dismissed without painting. Staleness, generation, and typed
prefix rules remain unchanged.

**Pitfall:** `yew_grid_fill` from cursor to row end is forbidden in the shadow
draw path. It destroys evidence: the suffix appears to vanish even though the
buffer is unchanged.

### 6. Workspace resume is the existing state file

No database lands. The XDG/Fletch state from Sprint 25 remains canonical.

- `yew .` restores tabs, pane trees, active tab, viewports, cursors, undo
  references, FUSS collapsed paths, and FUSS options before opening the drawer.
- Explicit file launches merge with restored state according to §1.
- F mode itself is transient and is not restored as the process's active mode;
  a directory CLI argument is what requests the startup drawer.
- `--clean` remains stateless and does not restore or save.

### 7. Performance and structural gates

- Extend `tests/perf/perf_shadow.c` with a 100 kLOC C fixture, a live mid-line
  single-line ghost, and a nonempty suffix. Composition p99 stays ≤ 250 us and
  performs zero steady-state allocations.
- Extend the FUSS performance harness with drawer entry, 1,000 selection moves,
  and open-resolution over a 20,000-entry tree. Each input-to-damage turn is
  ≤ 5 ms on the Sprint 56 reference lane; hosted runs are advisory.
- Add a deterministic row asserting selection movement damages the drawer and
  footer only, not every editor content row.
- `scripts/bans.sh` rejects `yew_grid_fill` in the shadow line-composition
  function and direct pane-root replacement from F-mode entry/leave.

### 8. Defer

- Simultaneous-process workspace-state merge and remote/shared state are
  post-1.0.
- Workspace-wide server `workspace/symbol` remains post-1.0; the local index is
  the no-LSP workspace surface.
- Cooperative nonliteral regex preview remains Sprint 59 after the Sprint 58
  search audit.
- Binary-size, allocation-debug, musl, QEMU, and required arm64/macOS gates
  remain Sprint 57.

## Testing Strategy

- **Unit:** startup-plan table including symlinks, relative directories,
  multiple files, mixed directory/file rejection, and explicit workspace;
  drawer width/damage; non-Git all-files merge; open destination commands;
  shadow cell shifting with ASCII, CJK, combining marks, ZWJ, tabs, and clipping.
- **Script/integration:** saved workspace + explicit file merge, duplicate-path
  focus, non-repository directory walk, horizontal/vertical split open, and
  failure atomicity.
- **PTY:** `yew .`, `yew DIR`, and `yew FILE`; compact title; dim backdrop;
  Enter opens a new tab; both split chords; Esc restores L visuals; mid-line
  symbol/LSP/AI ghosts preserve the real suffix. Unicode and ASCII glyph modes
  each receive deterministic goldens.
- **Performance:** §7 plus the existing Sprint 43/49 shadow and Sprint 52/53
  FUSS gates. Capture profiler phase rows when a large-file case breaches; do
  not relax the 5 ms editor budget.
- **Sanitizers/Valgrind:** focused startup/FUSS/shadow unit and PTY cases. No
  new ownership domain is expected; a leak or invalid cell move blocks closeout.
- **Determinism:** run every new PTY twice and byte-compare. Workspace walks are
  stable-sorted before the first visible tree publication.

## Definition of Done

1. `yew`, `yew .`, `yew DIR`, `yew FILE...`, and explicit `--workspace`
   follow §1 under unit and PTY tests; mixed positional directories fail before
   raw terminal entry.
2. `yew .` restores the cwd workspace and opens a drawer titled with cwd's
   basename; `yew DIR` does the same for the canonical argument.
3. Non-Git directories show the all-files tree. Git directories add status
   glyphs without changing path order or selection identity.
4. F entry/leave preserves the pane root, tab ids, window ids, viewports, and
   cursor bytes exactly; the drawer width follows §3 at 40, 80, 120, and 240
   columns.
5. Enter opens/focuses the selected file in a tab without replacing the source
   tab. Both split commands open the same path in the requested orientation and
   roll back cleanly on failure.
6. A ghost at column 0 before real text, at indentation before code, and in the
   middle of CJK/emoji text never erases or restyles a real cell. Acceptance is
   the only test that changes buffer bytes.
7. Multi-line ghosts never overwrite following document rows; ineligible
   mid-line multi-line delivery paints nothing and reports no error.
8. Existing workspace-state fixtures remain byte-identical; no new state schema
   version or database file appears.
9. Shadow composition p99 ≤ 250 us on the 100 kLOC case, FUSS input turns ≤ 5
   ms on the designated lane, and selection damage is drawer-local.
10. Full and `MODULES=""` GCC/Clang builds are warning-free; focused
    ASan/UBSan, Valgrind, PTY, determinism, performance, and bans gates are
    green.
11. The deferral ledger gains no unowned 1.0 surface, and `rg -n "lands in
    Sprint|not implemented yet|YEW_CMD_DEFERRED" src docs` contains only the
    reconciled rows.

## Closeout — COMPLETE 2026-08-27

The startup resolver, workspace drawer, explicit tab/split destinations, and
insertion-preview shadow composition are implemented. Directory launches
restore the canonical workspace before entering F; non-Git workspaces receive
the same stable tree; F remains a transient render/input layer over the live
pane tree; and shadow delivery obeys the suffix/mid-line policy without erasing
or restyling document cells.

Closeout evidence:

- the fast gate is green at 2,385 unit tests / 71,013,449 assertions, followed
  by the Fletch, script, syntax-asset, package, structural-ban, dispatch,
  render, and smoke gates;
- strict full and `MODULES=""` GCC/Clang builds are warning-free; focused
  ASan/UBSan is green for startup, FUSS, shadow, and the repaired LSP lifecycle
  fixture;
- FUSS drawer coverage is green at 14 tests / 186 assertions, its sanitized
  5,000-iteration lifecycle fuzz run is deterministic at seed 1 with hash
  `36e80942c96ccf86`, and an independent review reports no findings;
- every new drawer PTY (`yew .`, `yew DIR`, Enter, both split chords, and Esc)
  passes twice byte-identically and passes again under focused Valgrind;
- focused Valgrind is also clean for the startup resolver, FUSS drawer,
  shadow eligibility/draw/layout, and LSP-to-shadow delivery unit surfaces;
- the 100 kLOC mid-line shadow path performs zero steady-state allocations and
  measures below its 250 us gate; strict FUSS drawer entry is below 3.6 ms on
  both local compilers against the 5 ms gate; and the Sprint 56 performance
  policy/selftests remain green.

Sprint 56's designated x86_64/arm64 calibration references and repeated
self-hosted measurements remain an external release prerequisite. No hosted
measurement was promoted into a reference or baseline during this closeout.
