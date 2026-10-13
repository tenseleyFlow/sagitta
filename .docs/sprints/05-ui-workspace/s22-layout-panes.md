# Sprint 22: Layout and Panes

## Prerequisites

- Sprint 5 — `Grid`/`Cell` double buffer, damage diff, truecolor/256/16
  fallback, synchronized output.
- Sprint 6 — pty harness: feed keys, snapshot grid, byte-compare goldens.
- Sprint 9 — typed coordinates (`ByteOff`, `GCol`, `CCol`); conversions only
  in `src/unicode/`.
- Sprint 13 — key-trie keymaps + the named-command registry (`"ed.*"`).
- Sprint 15 — viewport/scroll engine, statusline, message line; a single
  full-screen `Win` draws a buffer viewport into the grid.
- Assumed (per 01-architecture, landed with the Sprint 3–6 loop): the
  `poll(2)` event loop with the timer binary heap; SIGWINCH delivery via the
  signal self-pipe.
- Binding: `.docs/plan/00-decisions.md` (Layout + Mouse rows, invariants 5
  and 9), `.docs/plan/01-architecture.md` (Rendering section).

## Goals

Turn the single full-screen `Win` into a pane tree: binary h/v splits with
ratios, `Win` leaves, directional focus navigation, keyboard and drag resize,
minimum sizes with refusal rules, and borders that dim inactive panes. Two
architectural pieces land here and bind every later UI sprint: the
**cells-based layout/draw split** (layout computes `Rect`s in cells, drawing
consumes them — multibyte text can never desync clicks from glyphs) and the
**clickable-region registry** (render passes populate, the click router
queries — one source of truth for hit-testing). Tabs land in Sprint 23; the
tab strip's reserved rows are a parameter here, not a renderer. Full mouse
routing (drag-reorder, wheel, context menus) lands in Sprint 27 — this sprint
wires only click-to-focus and border-drag resize, both through the registry.
Workspace persistence of the pane tree lands in Sprint 25.

## Deliverables

### 1. Rect and the layout/draw split — `src/ui/layout.h`

```c
typedef struct { u16 x, y, w, h; } Rect;   /* screen cells, 0-based */
```

Law (invariant-level for the UI): **layout computes `Rect`s in cells;
drawing consumes them; hit-testing consumes the same values.** No renderer
recomputes a position from text content, and no width math happens outside
`src/unicode/`. Why: the facsimile lesson — when placement is derived twice
(once while drawing, once while hit-testing), a multibyte filename shifts
every click to its right. Placement is computed once and shared.

- Pitfall: `Rect` is in **cells**, never bytes, never graphemes. A function
  taking both a `Rect` and text must clip via the Sprint 2 width tables
  (`clip_to_cells`-style: returns clipped string + cells used), and the
  clipped text must be styling-free — an embedded SGR sequence would be
  counted as visible cells and silently shorten the run.

### 2. Pane tree — `src/ui/layout.c`

```c
typedef enum { YEW_SPLIT_H, YEW_SPLIT_V } SplitDir;  /* H: side-by-side */

typedef struct Pane {
    bool         is_leaf;
    Rect         rect;            /* filled by yew_layout_compute */
    struct Pane *parent;          /* NULL at root */
    /* split node */
    SplitDir     dir;
    float        ratio;           /* first child's share, (0,1) */
    struct Pane *a, *b;           /* a = left/top, b = right/bottom */
    /* leaf node */
    Win         *win;             /* view: buffer ref, cursors, viewport */
} Pane;

Pane *yew_pane_split(Ed *ed, Pane *leaf, SplitDir dir);  /* NULL = refused */
void  yew_pane_close(Ed *ed, Pane *leaf);
void  yew_layout_compute(Pane *root, Rect area);
Pane *yew_pane_leaf_at(Pane *root, u16 x, u16 y);        /* NULL off-tree */
```

- Split turns the leaf into a split node with two new leaves; the existing
  `Win` becomes child `a`, the new `Win` (same buffer, same cursor/viewport
  copied) becomes child `b` and takes focus. `ratio` starts at 0.5.
- Close replaces the parent split with the sibling subtree; the sibling
  inherits the parent's rect at next layout. Closing the last leaf of the
  last tab is Sprint 23's problem; this sprint the root leaf refuses to
  close with a message.
- Layout recursion (the trickiest arithmetic — integer cells, no float
  accumulation):

```c
static void layout_rec(Pane *p, Rect r) {
    p->rect = r;
    if (p->is_leaf) return;
    if (p->dir == YEW_SPLIT_H) {
        u16 aw = (u16)((r.w - 1) * p->ratio + 0.5f);   /* -1: border col */
        aw = clamp_u16(aw, YEW_PANE_MIN_W, (u16)(r.w - 1 - YEW_PANE_MIN_W));
        layout_rec(p->a, (Rect){ r.x, r.y, aw, r.h });
        /* border column at r.x+aw is owned by the split node */
        layout_rec(p->b, (Rect){ (u16)(r.x+aw+1), r.y,
                                 (u16)(r.w-aw-1), r.h });
    } else { /* YEW_SPLIT_V: border row between */ ... }
}
```

- Pitfall: the ratio is stored, the cell split is **recomputed from the
  ratio every layout** — never store cell widths. Storing cells makes a
  SIGWINCH resize drift the proportions; storing the ratio makes resize
  idempotent (invariant 5: same state → same grid).
- Pitfall: rounding must be deterministic and single-site (`*ratio + 0.5f`
  once). Two call sites rounding differently produce a one-cell disagreement
  between draw and hit-test — exactly what the layout/draw split forbids.

### 3. Minimum sizes and refusal rules

| Constant | Value | Meaning |
|---|---|---|
| `YEW_PANE_MIN_W` | 12 | leaf content columns (excl. border) |
| `YEW_PANE_MIN_H` | 3  | leaf content rows (excl. border) |
| max leaves per tab | 16 | hard cap; split refuses past it |

- `yew_pane_split` returns NULL (and the caller messages "no room to
  split") when either resulting leaf would be under minimum at the current
  screen size. Never split-then-clamp: a split that immediately renders a
  2-column sliver is a refusal, not a layout problem.
- On SIGWINCH shrink, ratios are preserved and the clamp in `layout_rec`
  floors each leaf at the minimum; when the area cannot fit both minima the
  **deeper** subtree collapses to zero-size and is skipped by draw (marked
  `rect.w==0`), never freed — growing the terminal back restores it. Why:
  destroying panes on shrink loses user state to a transient window drag.

### 4. Focus navigation — spatial, directional

```c
void yew_focus_dir(Ed *ed, int dir);   /* YEW_DIR_LEFT/RIGHT/UP/DOWN */
```

Algorithm (facsimile's, proven): take the focused leaf's center; among
leaves strictly beyond it in `dir` (candidate's far edge ≤ center for
left/up, near edge ≥ center for right/down), pick the one whose facing edge
is nearest. No candidate → no-op (never wraps; wrapping makes muscle-memory
directional keys ambiguous in 3-pane layouts).

- Commands: `ed.pane.split_h`, `ed.pane.split_v`, `ed.pane.close`,
  `ed.pane.focus_left/right/up/down`, `ed.pane.grow`, `ed.pane.shrink`,
  `ed.pane.focus_next` (tree-order cycle, for terminals that eat arrows).
  All registered in the Sprint 13 registry; default L-mode bindings ship in
  the keymap table (`s`/`v` prefixed per the mode design; exact chords are
  keymap data, not code).
- Only the focused leaf shows the hardware cursor; `Ed` holds
  `Pane *focus`, and focus is a **leaf pointer** validated after any tree
  mutation (close of the focused leaf moves focus to the sibling's first
  leaf in tree order).

### 5. Resize — keyboard and border drag

- Keyboard: `ed.pane.grow`/`ed.pane.shrink` adjust the nearest ancestor
  split's ratio by 2 cells' worth (`2.0f / span`), clamped so both sides
  stay ≥ minimum. "Nearest ancestor split" = walk up from the focused leaf
  to the first split whose `dir` matches the grow axis (grow defaults to
  the axis of the closest ancestor; `grow_h`/`grow_v` variants exist for
  scripts).
- Drag: border cells register `REGION_PANE_BORDER` regions (payload = index
  into a per-frame split-node table, deliverable 6). On SGR press over a
  border, the loop enters a drag: motion events move the split boundary in
  whole cells, converting back to ratio at drag end (single rounding site).
  Sprint 27 re-routes this through the full mouse router; this sprint the
  drag state machine lives in `layout.c` and is deliberately minimal (press
  → motion → release; Esc cancels, restoring the entry ratio).
- Pitfall: convert **cells → ratio once at release**, not per motion event.
  Per-event ratio round-trips accumulate float error and make the border
  stutter against the pointer.

### 6. Clickable-region registry — `src/ui/region.c|h`

```c
typedef enum {
    YEW_REGION_NONE = 0,
    YEW_REGION_PANE,          /* payload: leaf table index */
    YEW_REGION_PANE_BORDER,   /* payload: split table index */
    YEW_REGION_TAB,           /* payload: +tab index | -(group id)  (S23/24) */
    YEW_REGION_TAB_SCROLL,    /* payload: ±row (S23) */
    YEW_REGION_BLOCK,         /* inert: swallows clicks (dialogs) */
    /* S24+: YEW_REGION_GP_ROW, YEW_REGION_GP_NAME, YEW_REGION_CTX_ROW */
} RegionKind;

typedef struct { RegionKind kind; Rect rect; i32 payload; } Region;

void   yew_region_frame_begin(void);           /* clears the table */
void   yew_region_add(RegionKind k, Rect r, i32 payload);
Region yew_region_hit(u16 x, u16 y);           /* last-added wins */
```

- Populated **during render passes only** — a renderer that draws a thing
  registers its region in the same statement block, using the same `Rect`
  it drew with. Querying happens only in the input path. This is the one
  source of truth rule: nothing else may map screen cells to meaning.
- Last-added wins on overlap, so overlays (dialogs, menus) drawn after the
  document naturally shadow it; `YEW_REGION_BLOCK` exists so a dialog can
  own its rectangle without meaning anything (clicks on it never reach the
  pane below).
- The payload sign convention for `YEW_REGION_TAB` (positive = tab index,
  negative = −group id) is **documented in `region.h` now** even though
  groups land in Sprint 24 — the convention is load-bearing for the click
  router and must not be invented twice.
- Fixed-capacity array (256), `yew_region_add` past capacity logs once per
  session via `yew_log` and drops — a frame with 256 regions is a rendering
  bug, not a growth problem.
- Pitfall: the table must be cleared at frame start, not at frame end.
  Clearing at end leaves a crash/early-return frame querying stale regions
  from a layout that no longer exists.

### 7. Borders and dim-inactive rendering — `src/ui/draw.c`

- Border glyphs: `│` (vertical), `─` (horizontal), `┼` at crossings,
  `├ ┤ ┬ ┴` at T-joints — computed from the tree, drawn by the split nodes
  (each split owns its border row/column; leaves never draw borders, so
  adjacent leaves cannot double-draw a shared edge).
- Active pane: border in the theme's accent; inactive panes: border in the
  dim style **and** their content rendered with the inactive attribute
  (SGR 2 where supported, mapped per the Sprint 5 fallback chain; the exact
  palette is theme data). The focused leaf alone positions the terminal
  cursor.
- Click-to-focus: `YEW_REGION_PANE` hit → focus that leaf and (L mode)
  move the cursor to the clicked cell via `CCol → GCol` conversion in
  `src/unicode/` against that Win's viewport. This is the only mouse click
  behavior this sprint.

### 8. Defer

- Tab strip, tab model, multiple pane trees: Sprint 23 (`ed.tab.*` commands
  hard-error naming it).
- Group rendering conventions consuming the payload sign: Sprint 24.
- Persisting the pane tree: Sprint 25 (`layout.c` exposes
  `yew_pane_tree_walk(root, fn, ctx)` now so Sprint 25 serializes without
  reaching into node internals).
- Wheel scroll, drag-reorder, context menus, double/triple-click: Sprint 27
  — unrouted mouse events are ignored, never mis-dispatched.

## Testing Strategy

- **Unit** (`tests/unit/test_layout.c`, `test_region.c`): tree invariants
  after split/close sequences (parent pointers, leaf counts); ratio→cells
  determinism (layout twice, memcmp rects); min-size refusal at exactly
  min×2+1 and acceptance at min×2+2; SIGWINCH shrink-then-grow restores
  identical rects (byte-compare); spatial focus on a 4-pane grid and a
  3-pane T layout vs hand-computed answers; region last-added-wins,
  clear-at-frame-begin, capacity drop; `yew_pane_leaf_at` agrees with the
  rect of every leaf for all cells (exhaustive over an 80×24 layout).
- **Pty** (`tests/pty/panes_*.golden`): split h, split v, nested 3-pane,
  focus moves (border color change), keyboard resize, drag resize
  (synthetic SGR press/motion/release), dim-inactive rendering, borders
  with a CJK/emoji filename in the statusline (clicks land correctly —
  asserted by feeding a click and checking focus, not just pixels).
- **Fuzz**: random split/close/resize/focus sequences (10k ops) asserting
  tree invariants + layout determinism after every op; ASan/UBSan lanes.
- **Regression fixture**: the one-cell rounding disagreement — a 3-pane
  layout at odd widths (81, 121 cols), rects asserted exactly.

## Definition of Done

1. `make test` green: ≥ 40 layout/region unit assertions; both compilers,
   zero warnings.
2. All pty goldens byte-exact across two consecutive runs (invariant 5).
3. Split refusal: at 80×24, splitting past minimum messages and returns
   NULL; no 0-width leaf is ever drawn (assert in draw path).
4. SIGWINCH 80×24 → 40×12 → 80×24 restores byte-identical rects and grid.
5. Click-to-focus works through the registry: pty test feeds SGR click,
   focused pane changes, cursor lands on the clicked grapheme with a
   double-width char earlier on the line (the desync case).
6. Drag resize: press/motion/release sequence moves a border exactly the
   motion delta in cells; Esc mid-drag restores the entry ratio.
7. `grep -rn "wcwidth\|width" src/ui/ | grep -v unicode` shows no width
   math in `src/ui/` (conversions only via `src/unicode/` calls).
8. Every `ed.pane.*` command dispatches through the Sprint 13 registry
   (no direct key→function calls; grep for keymap table entries).
9. `ed.tab.*` invocations hard-error naming Sprint 23.
10. 10k-op fuzz run clean under ASan/UBSan; region table never queried
    between `frame_begin` and first `add` (assert counter in debug build).
