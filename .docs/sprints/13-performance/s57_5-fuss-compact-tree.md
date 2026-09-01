# Sprint 57.5: The Compact, Remembering FUSS Drawer

## Prerequisites

- Sprint 52 — `FussNode`, `FussItem`, `FussTree`, path-held selection,
  depth-aware navigation, deterministic directories-first ordering, status
  marker aggregation, lazy untracked-directory expansion, FUSS commands, and
  the Git verb surface. Its branch-connector rows, expanded-by-default policy,
  `▼`/`▶` directory glyphs, and collapsed-path refresh algorithm are
  superseded here. Status markers and command behavior are not.
- Sprint 53 — editor-side Git views and file-backed buffers. Open-path
  expansion observes the editor's real tabs and panes; it never asks Git which
  files are open.
- Sprint 25 — workspace state and the frozen v1 schema. This sprint adds no
  state-file field and no schema version. Remembered expansion is session
  memory owned by `FussMode`.
- Sprint 26 — budget-sliced workspace walks and `yew_str_width` /
  `yew_str_clip`. Tree population and all cell-width measurements keep using
  those APIs.
- Sprint 56.5 — F mode as a transient left drawer over the live editor, the
  dim backdrop, drawer-local damage, all-files non-Git trees, and explicit
  file-open destinations. Its fixed `content_cols / 6`, 20..48-cell clamp and
  "first useful level" expansion are superseded here. F mode still owns no
  pane tree and leaving it still preserves editor state byte-for-byte.
- Sprint 57 — size, allocation, portability, musl, and arm64 gates. This
  pressure-valve sprint may not regress any of them or add a dependency.
- Binding: `00-decisions.md` FUSS, layout, workspace, width, determinism, and
  module rows; `01-architecture.md` render/layout separation and editor data
  model; invariants 1, 3, 4, 5, 7, 8, and 9.

## Goals

Make the FUSS drawer fit real workspaces instead of spending most of its width
on tree furniture and most of its height on directories the user did not ask
to inspect. Directory rows start collapsed. A directory is effectively open
only when the user opened it earlier in this yew session or when it is an
ancestor of a file currently open in the editor. Closing and reopening F mode
therefore reconstructs the same useful tree without preserving a stale fully
expanded snapshot.

Replace branch connectors and ambiguous triangle glyphs with a two-cell
plus/minus vocabulary and two-cell indentation. Compute the drawer's natural
width from the effective flat tree, begin at one quarter of the terminal, grow
only as far as needed to show every visible row, and use the full content width
when that row width cannot coexist with a meaningful editor backdrop. A bright
vertical edge makes the overlay boundary explicit. Navigation, Git markers,
Git verbs, selection-by-path, non-Git operation, and the two-row F footer remain
unchanged.

Persistence across a yew process restart, manual drawer resizing, horizontal
tree scrolling, compacting status markers, and a new Git/file browser are out
of scope. They do not receive silent stubs.

## Deliverables

### 1. Positive expansion memory replaces expanded-by-default state

`src/mod/git/fusstree.h` adds a small positive set of workspace-relative
directory paths. It records only explicit user expansion, never every default
or every currently collapsed path.

```c
typedef struct FussOpenPath {
    char *path;
    u32 path_len;
} FussOpenPath;

typedef struct FussOpenMemory {
    FussOpenPath *data;
    u32 len;
    u32 cap;
} FussOpenMemory;

void yew_fuss_open_memory_init(FussOpenMemory *m);
void yew_fuss_open_memory_drop(FussOpenMemory *m);
bool yew_fuss_open_memory_has(const FussOpenMemory *m,
                              const char *path, u32 path_len);
bool yew_fuss_open_memory_set(FussOpenMemory *m,
                              const char *path, u32 path_len,
                              bool expanded);
```

- Rows are byte-sorted by `(path, path_len)` and searched by binary search.
  Insert and erase preserve that order. Raw `qsort` and filesystem enumeration
  order remain forbidden.
- The set owns its path bytes. A tree rebuild frees the tree arena and must not
  invalidate expansion memory.
- `expanded=true` inserts once; `expanded=false` removes if present. The
  return value says whether effective memory changed, not whether allocation
  succeeded; allocation follows the repository's OOM-is-bug contract.
- `FussMode` owns one `FussOpenMemory manual_open` from
  `yew_fuss_state_init` through `yew_fuss_state_free`. Mode leave, Git refresh,
  all-files toggles, hidden-file toggles, and completed walks do not clear it.
- A directory first seen in a session is collapsed. Right on a collapsed
  directory expands and records it; Right on an expanded directory keeps
  Sprint 52's descend behavior. Space, directory Enter, and mouse indicator
  clicks toggle the same manual intent. Left remains parent navigation. Every
  expand/collapse route invokes the same model operation.
- Memory for a path temporarily absent from the current filtered tree remains.
  If `T`, `.`, a Git refresh, or a later walk makes it visible again, it opens.
  Workspace teardown frees it; changing workspaces cannot leak one workspace's
  paths into another.

This is positive memory deliberately. Sprint 52 harvested collapsed paths
because every new node began expanded. Keeping that inverse representation
after changing the default would retain nearly every path and make a new
directory unexpectedly open.

**Pitfall:** do not point `manual_open` into `FussTree.a`, `GitSnapshot.a`, or
`FileList`. Each of those has a shorter lifetime than F mode's session memory.

### 2. Effective expansion is manual memory union open-file ancestry

`src/mod/git/fusstree.h` exposes one application pass:

```c
typedef struct FussPathRef {
    const char *path;
    u32 path_len;
} FussPathRef;

void yew_fuss_apply_expansion(FussTree *t,
                              const FussOpenMemory *manual_open,
                              const FussPathRef *open_files,
                              u32 nopen);
```

The pass implements this truth table for every directory node:

| In `manual_open` | Ancestor of open file | Effective `expanded` |
|---:|---:|---:|
| no | no | no |
| yes | no | yes |
| no | yes | yes |
| yes | yes | yes |

`src/mod/git/fussmode.c` builds `open_files` from all file-backed editor
surfaces in the current workspace:

- hydrated and deferred tabs both count;
- every live pane buffer counts even if it has no tab entry;
- scratch, job, commit-message, diff, and other synthetic buffers do not;
- a path outside `yew_ws_root(ed)` does not;
- duplicate canonical paths are removed before the tree pass.

For `src/mod/git/a/b.c`, existing directory nodes `src`, `src/mod`, and
`src/mod/git` open. A byte-prefix alone is insufficient: `src/modem.c` is not
under `src/mod`. An ancestor match ends at `/` or at the file path's end.
Hidden/ignored/all-files policy still controls which nodes exist; open-path
expansion does not bypass filtering or fabricate clean rows in dirty-only mode.

The order of operations on entry and rebuild is fixed:

1. build/merge the tree with every directory collapsed;
2. collect and canonicalize the editor's open workspace-relative paths;
3. apply `manual_open ∪ ancestors(open_files)`;
4. flatten once;
5. resolve selection by path, falling back through ancestors as in Sprint 52;
6. compute and cache natural width (§4).

An editor-open descendant wins over a manual collapse while that descendant is
open. A toggle may remove the path from `manual_open`, but the directory stays
visibly open until the final open descendant closes; it then collapses on the
next editor-surface notification. If the directory had no manual entry to
remove, the toggle reports `kept open: contains an open file` instead of
appearing to act. This makes the "open file is discoverable" rule deterministic
rather than dependent on the order of two intents.

`yew_fuss_windows_changed` and tab open/close/restore notifications recompute
effective expansion only when the canonical open-path set changes. Selection
motion and painting never rescan tabs or buffers.

**Pitfall:** do not mutate `manual_open` while applying automatic ancestry.
Otherwise merely opening a file permanently changes the remembered tree after
that file closes.

### 3. Compact rows: no connectors, two-cell depth, plus/minus directories

`src/mod/git/fussmode.c:fuss_tree_row` uses this locked row grammar:

```text
row       = indent indicator name markers
indent    = "  " repeated depth times
indicator = "+ " collapsed directory
          | "- " expanded directory
          | "  " file
markers   = Sprint 52 status markers, unchanged
```

Example:

```text
- src ✗
  + edit ✗
  - mod
    - git ✗
        fussmode.c ✗
        fusstree.c ↑
+ tests ✗
  README.md
```

- `│`, `├──`, `└──`, ASCII `|   ` / `|-- ` / `` `-- ``, `▼`,
  `▶`, `v`, and `>` disappear from FUSS tree rows. The pane and panel glyph
  tables are unrelated and do not change.
- Plus and minus are ASCII in every terminal profile. `git.ascii_glyphs`
  continues to select ASCII **status** markers and the ASCII drawer edge; it
  no longer changes directory indicators.
- A file reserves the same two indicator cells as a directory. Names at the
  same depth therefore align and toggling a row does not shift its label.
- Depth costs two cells rather than four. All arithmetic saturates before
  conversion to `u16`; an adversarial path cannot wrap a width to zero.
- The selected reverse-video run fills only the tree-content cells. It stops
  before the edge column and still preserves the final component and status
  markers when even full-screen width must clip.
- Marker order, conflict replacement, colors, and aggregation to a collapsed
  parent remain Sprint 52's table verbatim.

`fuss_prefix_ancestor` and connector-specific prefix skipping are deleted.
Clipping may remove leading indentation before it removes the final component;
it may not reintroduce connector fragments or move status markers off-screen.

**Pitfall:** `depth * 2` is cell arithmetic, while `name_len` is bytes. Names
and markers still route through `yew_str_width`, `yew_gb_next_bytes`, and
`yew_str_clip`; neither `strlen` nor byte subtraction is display width.

### 4. Natural width and deterministic smart drawer layout

`src/mod/git/fussmode.h` replaces the fixed-width helper with model-aware
measurement and layout:

```c
enum {
    YEW_FUSS_INDENT_CELLS = 2,
    YEW_FUSS_DRAWER_MIN_CELLS = 24,
    YEW_FUSS_DRAWER_BASE_MAX_CELLS = 64,
    YEW_FUSS_EDITOR_RETAIN_CELLS = 40,
    YEW_FUSS_DRAWER_EDGE_CELLS = 1
};

typedef struct FussDrawerLayout {
    u16 width;       /* includes the edge column when !fullscreen */
    u16 tree_width;  /* cells available to header and rows */
    u16 edge_col;    /* UINT16_MAX when fullscreen */
    bool fullscreen;
} FussDrawerLayout;

u16 yew_fuss_tree_natural_width(const FussTree *tree);
FussDrawerLayout yew_fuss_drawer_layout(u16 content_cols,
                                        u16 natural_cols);
Rect yew_fuss_drawer_rect(const Ed *ed);
```

Natural width is measured over the current **effective flattened rows**, not
all nodes hidden beneath collapsed directories:

```text
row_cells = 1 left pad
          + 2 * depth
          + 2 indicator cells
          + display_width(name)
          + 2 * marker_count
          + 1 right pad
natural_cols = max(row_cells) + 1 edge cell
```

The header clips independently and never widens the drawer. An empty/loading
tree has `natural_cols = 0` and uses the base width. Measurement occurs after
flatten and is cached on `FussMode`; selection, scrolling, footer messages, and
ordinary repaint use the cached value with zero heap allocations.

The layout algorithm is exact:

```text
base = ceil(content_cols / 4), clamped to [24, 64]
base = min(base, content_cols)
want = max(base, natural_cols)
overlay_cap = max(content_cols - 40, 0)

if content_cols < 24 or want > overlay_cap:
    fullscreen = true
    width = tree_width = content_cols
    edge_col = UINT16_MAX
else:
    fullscreen = false
    width = want
    tree_width = width - 1
    edge_col = width - 1
```

Reference cases, asserted as one table-driven unit test:

| Content | Natural | Result | Width | Editor backdrop retained |
|---:|---:|---|---:|---:|
| 20 | 12 | full-screen | 20 | 0 |
| 40 | 18 | full-screen | 40 | 0 |
| 64 | 18 | overlay | 24 | 40 |
| 80 | 34 | overlay | 34 | 46 |
| 80 | 41 | full-screen | 80 | 0 |
| 120 | 20 | overlay | 30 | 90 |
| 120 | 70 | overlay | 70 | 50 |
| 120 | 81 | full-screen | 120 | 0 |
| 240 | 30 | overlay | 60 | 180 |
| 240 | 190 | overlay | 190 | 50 |
| 240 | 201 | full-screen | 240 | 0 |

There is no hysteresis and no remembered width: identical content, open paths,
terminal cells, and options always produce the same rectangle. A toggle may
change the widest visible row and therefore resize the drawer or cross the
full-screen boundary. Resize recomputes from the cached natural width in O(1).
Rows wider than a full terminal still follow Sprint 56.5's basename-and-marker
preserving clipping; full-screen means "all available width", not an impossible
promise to display more cells than the terminal owns.

The old `yew_fuss_drawer_width(u16)` and its 1/6, 20..48 clamp are removed,
along with tests that lock those values.

**Pitfall:** calculate `ceil(cols / 4)` in at least `u32`; `(u16)(cols + 3)`
can wrap at a legal maximum terminal width.

### 5. A bright edge separates drawer and editor

Add UI theme role `git.drawer.edge` to `runtime/themes/quiver-dark.fl` and
`runtime/themes/quiver-light.fl`, and cover it in
`tests/unit/test_fusstheme.c`. UI roles are open-ended in
`src/syn/theme.c:parse_ui`; this is not a syntax attribute and must not be
added to `src/syn/defs.c`. Its shipped foreground is `#ffffff`, with no
background override and bold enabled. Capability downgrade maps it to bright
white in 256/16-color modes and to the ordinary foreground in no-color mode.

- Overlay layout draws `│` in every `edge_col` cell from drawer top through
  the final tree row. `git.ascii_glyphs=true` draws `|` instead. The edge is
  painted after the opaque drawer and dim backdrop, so neither can erase it.
- Full-screen layout has no editor/drawer boundary and therefore no artificial
  right edge. The terminal boundary is not registered as a pane border.
- The edge is not draggable. It receives no FUSS-row or pane-border region;
  smart layout owns its column. Row click regions use `tree_width` and stop
  before it.
- Selected-row reverse video, header fill, and status colors stop before the
  edge. The edge remains visually stable while selection moves.
- Damage for selection movement remains row-local. A natural-width or
  full-screen transition invalidates the union of the old and new drawer plus
  the backdrop whose dim state changed; it must restore newly exposed editor
  cells from the live pane render rather than leave stale FUSS bytes.

**Pitfall:** overlaying only attributes cannot create a separator glyph. The
edge owns its cell after backdrop dimming and must write both glyph and style.

### 6. Full-screen consequences preserve existing F behavior

Full-screen is a FUSS rendering state, not a new mode and not a pane mutation.

- Esc and `q` return to L and reveal the byte-identical pre-F editor layout.
- Enter and split-open commands retain Sprint 56.5 behavior. A failed open
  leaves full-screen FUSS intact.
- The read-only `Space`/Git viewer remains reachable. In overlay layout it
  anchors after the drawer as today. In full-screen layout it opens through the
  existing generic panel surface, centered inside the content rectangle with
  a two-cell inset when available; Esc closes the viewer before leaving F.
- Tab bar and two-row F footer remain visible in both layouts. Full-screen
  consumes only the content rectangle between them.
- The ordinary pane tree stays live underneath an overlay and untouched behind
  a full-screen tree. Neither state serializes a pane, split ratio, or active F
  mode into workspace state.
- Mouse wheel, single-click selection, double-click open, and directory toggle
  use the new rectangles. The edge itself is inert. Keyboard commands remain
  the authority.

### 7. Refresh, invalidation, and allocation discipline

The old harvest/restore path is retired from production:

```c
u32 yew_fuss_harvest_collapsed(...);   /* remove after test migration */
void yew_fuss_restore_collapsed(...);  /* remove after test migration */
```

Every structural publication uses the positive-memory pipeline from §2.
`FussNode.expanded` remains the effective cached bit used by flatten/navigation;
it is not itself authoritative across a rebuild.

The following events invalidate effective expansion and natural width:

| Event | Rebuild tree | Reapply expansion | Remeasure width |
|---|---:|---:|---:|
| Git snapshot generation changes | yes | yes | yes |
| workspace walk publishes | yes/merge | yes | yes |
| `T` or `.` changes filtering | yes | yes | yes |
| manual directory toggle | no | yes | yes |
| tab/pane file set changes | no | yes | yes |
| selection or scroll changes | no | no | no |
| terminal resize only | no | no | no; relayout cached width |
| footer/message changes | no | no | no |

Applying expansion plus flatten is O(nodes + total open-path bytes). Natural
measurement is O(visible rows). No pass allocates during drawing, selection, or
resize. The open-path scratch vector is retained on `FussMode` and reused.

Sprint 52's lazy untracked scan still runs only on an expansion request. Once
loaded, the directory enters `manual_open`; refresh may rebuild it, and the
existing cache/walk policy decides whether its children need another scan.

### 8. Defer

- Persisting `manual_open` across process restart remains post-1.0 unless a
  later sprint versions the workspace schema. This sprint guarantees
  close/reopen of **F mode within one yew session**.
- A user-draggable or configured drawer width is not added. Smart width has
  one deterministic policy.
- Horizontal tree scrolling is not added. Full-screen fallback plus
  basename/status-preserving clipping is the 1.0 behavior.
- Status marker compaction, alternate path abbreviations, tree search changes,
  Git verb changes, and a second file-explorer mode are out of scope.
- The FUSS drawer remains part of the `fuss` compile-time module. A
  `MODULES=""` build continues to hard-error with the canonical module message.

## Testing Strategy

- **Unit — expansion** (`tests/unit/test_fussexpand.c`): default-collapse,
  positive-set insertion/removal/idempotence/sortedness, close/reopen of F,
  rebuild/filter disappearance and return, automatic open-file ancestors,
  segment-boundary matching, duplicate tabs, deferred tabs, scratch/outside-
  workspace exclusion, manual-plus-automatic precedence, final-descendant
  close, selection fallback, and lazy untracked expansion.
- **Unit — row model** (`tests/unit/test_fusstree.c` and
  `test_fussdrawer.c`): the exact `indent + indicator + name + markers`
  grammar at depths 0..32; absence of every retired connector/glyph; marker
  order and aggregation unchanged; selected-row clipping with ASCII, CJK,
  combining marks, and ZWJ names.
- **Unit — layout** (`tests/unit/test_fussdrawer.c`): every §4 reference
  row, exact edge ownership, saturating adversarial depths, cached measurement,
  resize transitions in both directions, full-screen viewer placement, region
  exclusion, and union damage when width changes.
- **Script/integration:** open nested files across tabs and splits, enter F,
  assert every existing ancestor opens; manually expand an unrelated directory,
  leave/reenter, close the nested file, refresh Git, and assert only the
  remembered unrelated directory remains open.
- **PTY:** compact Unicode and ASCII rows at 40×24, 64×24, 80×24,
  120×40, and 240×60; overlay and full-screen transitions; white/bright-
  white edge under truecolor/256/16/no-color; dim editor ending exactly at the
  edge; open-file ancestry; remembered reentry; mouse edge inertness; viewer;
  Esc restoration. Each new golden runs twice and byte-compares.
- **Fuzz:** random tree paths, open paths, filters, toggles, rebuilds, and
  terminal widths 0..65535 never overflow, expose a hidden descendant, leave an
  invalid selection, or produce a drawer rectangle outside the content rect.
- **Performance/allocation:** extend `tests/perf/fuss.c` with a 20,000-node
  mostly-collapsed tree, 512 open paths, 1,000 toggles, and natural-width
  transitions. Toggle/open-set p99 remains ≤ 5 ms on the designated lane;
  selection and resize perform zero allocations; build+apply+flatten remains
  within Sprint 52's 12 ms build budget.
- **Lanes:** GCC, Clang, ASan/UBSan, Valgrind, determinism, musl static-PIE,
  arm64 Linux/macOS, full modules, and `MODULES=""` all remain green.

## Definition of Done

1. A newly built tree shows all top-level entries and no descendants unless a
   directory is manually remembered or is an ancestor of an open editor file.
2. Expanding a directory, leaving F, and reentering F 100 times keeps that
   directory open; collapsing it removes the memory. Git refresh and `T`/`.`
   toggles do not drift the remembered set.
3. Every file-backed hydrated/deferred tab and live pane opens its existing
   ancestor chain. Scratch/outside-workspace paths do not. Closing the final
   descendant collapses ancestors that lack manual memory.
4. FUSS tree goldens contain `+ ` and `- ` and contain none of `│`, `├──`,
   `└──`, `|--`, `` `-- ``, `▼`, or `▶` in tree rows. Existing status
   marker combinations remain byte- and color-identical.
5. Every §4 layout-table row passes exactly. Natural width is derived from
   visible rows, drawer base width is one quarter clamped to 24..64, and at
   least 40 editor cells remain or FUSS becomes full-screen.
6. Overlay mode draws one bright-white `│`/`|` edge column; selection,
   header, dim backdrop, and row hit regions neither overwrite nor claim it.
   Full-screen mode draws no false internal edge.
7. Width changes and both full-screen transitions restore editor cells without
   stale tree bytes. Selection-only movement damages no editor content row.
8. Enter, split-open, preview, mouse, Git verbs, `q`, and Esc work in overlay
   and full-screen layouts; leaving restores pane/window/tab/cursor identity
   byte-for-byte.
9. Extracting `fuss_tree_row` and searching it for `│`, `├`, `└`, `▼`,
   `▶`, `|--`, or the backtick branch finds no match. The edge's legitimate
   `│` occurrence remains outside that function and is covered separately.
10. `grep -nE 'strlen|wcwidth|wcswidth' src/mod/git/fussmode.c
    src/mod/git/fusstree.c` is empty; Unicode width and clipping use the shared
    width layer.
11. The 20,000-node/512-open-path gate meets §Testing budgets; selection and
    resize allocate zero bytes in the Sprint 57 accounting build.
12. `make test`, PTY double-run determinism, sanitizers, Valgrind, musl,
    arm64, module parity, `make size`, and both warning-as-error compilers are
    green. No dependency or workspace-state schema field lands.

## Closeout — REPOSITORY COMPLETE 2026-09-01

The compact FUSS redesign is implemented. New trees start collapsed;
session-owned positive expansion memory survives leave/reentry and rebuilds;
open editor files force their existing ancestor chain open without polluting
manual memory; and rows use two-cell depth with `+`/`-` indicators and no
branch furniture. The drawer measures only visible rows, keeps at least 40
editor cells beside an adaptive quarter-width overlay, and falls back to the
full content rectangle when that is impossible. Overlay mode owns one bright
edge cell, while full-screen previews use the generic centered panel surface
without mutating the pane tree.

Implementation landed in `77d692bd`, `9481e025`, and `212e0930`. Golden and
module-shim follow-ups landed in `f423a498` and `f383278b`. Apple-silicon
validation also exposed two pre-existing host-tool assumptions: sanitized
child tests were injecting an instrumented durability interposer, and the
size target passed GNU flags to Apple `strip`. `dfaf0922` and `3ff018c7`
repair those test/build boundaries without changing editor behavior.

Local closeout evidence on Darwin arm64:

- full modules pass the complete native product suite, including all PTYs,
  with 2,409 unit tests / 71,032,846 assertions / 0 failures;
- an independent `MODULES=""` tree passes its complete applicable product
  suite with 1,946 unit tests / 70,056,319 assertions / 0 failures, proving
  that core editing, Fletch, syntax, search, persistence, recovery, and macros
  remain intact outside the four optional modules;
- the complete Clang ASan/UBSan unit run passes 2,391 tests /
  71,032,552 assertions / 0 failures; the 20,000-iteration sanitized FUSS
  campaign is deterministic at seed `91615317` with hash
  `8861851834338528`;
- all 29 FUSS PTY cases pass twice, covering 40/64/80/120/240-column layouts,
  Unicode/ASCII indicators and edges, remembered reentry, open-file ancestry,
  preview placement, edge inertness, and Esc restoration;
- the 20,000-node performance gate measures 2.231 ms build+flatten,
  0.023 ms toggle+measure p99, 2.538 ms drawer entry, and 3.819 ms open
  resolution, all within their 5/12 ms limits; the full u16 layout sweep is
  0.120 ms;
- native `make size` passes all six module profiles after Apple-native
  stripping: 1,452,640 bytes minimal, 1,891,472 full, and every single-module
  build below its existing cap. These Mach-O numbers corroborate rather than
  replace the binding x86_64 Linux measurements.

No core feature, safety invariant, or regression surface was removed or
weakened for footprint. Amendment S57-A1 remains the hard stop rule: if a
future measurement cannot meet its target without observable feature loss,
weaker durability/Unicode/terminal guarantees, or narrower tests, work pauses
and the measured floor is reviewed in `00-decisions.md` before code changes.
The 900 KiB planning value has already been moved to a post-1.0 optimization
ratchet for exactly this reason. Sprint 58 front F15 and Sprint 60 release
closeout are the next mandatory reassessment points.

The unavailable local lanes are not inferred: true GNU GCC, Linux Valgrind,
musl static PIE, arm64 Linux, and hosted CI remain external evidence tails on
the exact pushed SHA. A failure in any of them reopens Sprint 57.5 remediation
before release. The repository implementation frontier advances to Sprint 58.
