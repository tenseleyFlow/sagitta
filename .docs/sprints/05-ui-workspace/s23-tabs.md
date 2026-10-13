# Sprint 23: Tabs

## Prerequisites

- Sprint 22 — pane tree with `Rect` cells layout, clickable-region registry
  (`YEW_REGION_TAB` / `YEW_REGION_TAB_SCROLL` kinds and the payload sign
  convention already declared), borders/dim rendering, click-to-focus.
- Sprint 15 — statusline/message line (the dirty-close prompt uses it).
- Sprint 13 — command registry; Sprint 10 — undo tree (clean-state node
  for the modified marker); Sprint 8 — atomic save (dirty-close "write").
- Binding: `.docs/plan/00-decisions.md` (Tab groups row — the stable-id
  discipline starts here), `01-architecture.md` data model (`Tabs[]` with
  stable `tab_id`; per-Tab `PaneTree`).

## Goals

Land the tab model and tab strip: a `Tab` owns a pane tree and carries a
**stable `tab_id`** that survives array compaction, plus the `group_id` /
`group_ordinal` fields Sprint 24 will animate (0 = ungrouped until then).
Rendering is a reusable strip-layout engine (entries → cell spans) with
overflow scrolling and clickable `<` / `>N` indicators; the modified `*`
marker derives from the undo tree, never a cached flag. Tab commands, digit
jump to tab N, and the close-with-dirty prompt flow complete the surface.
Groups, the row-2 member strip, and the 500 ms digit-extension window land
in Sprint 24; persistence in Sprint 25; drag-reorder and context menus in
Sprint 27.

## Deliverables

### 1. Tab model — `src/ui/tabs.h`

```c
typedef struct Tab {
    u32   tab_id;         /* monotonic, never reused; 0 invalid */
    u32   buffer_id;      /* BufferList handle (buffers shared, views not) */
    char *path;           /* canonical realpath, arena-owned; NULL=untitled */
    Pane *root;           /* this tab's pane tree */
    Pane *focus;          /* focused leaf within root */
    u32   group_id;       /* 0 = ungrouped (Sprint 24 animates) */
    u32   group_ordinal;  /* 1-based within group; 0 when ungrouped */
    bool  deferred;       /* placeholder; real lazy hydration is Sprint 24 —
                             until then always false and asserted so */
} Tab;

typedef struct Tabs {
    Vec(Tab) v;           /* compacted on close: indices shift, ids never */
    u32      next_tab_id; /* starts 1 */
    int      active;      /* INDEX into v; -1 when empty */
} Tabs;
```

**The pitfall this struct exists to kill (facsimile, verbatim lesson):
closing a tab compacts the array and renumbers every index above it, so a
saved index silently comes to mean a different tab** — in facsimile that let
a closed pane's text be written over its sibling's file. Anything that must
refer to "this tab" across a mutation holds the `tab_id`, not the position.
`next_tab_id` is monotonic and never reused, so a stale id resolves to
nothing rather than to whatever later took the slot.

- `path` is canonicalized (`realpath`) at the single site where a tab's
  name is established (`yew_tab_open`) — never in comparators, which sit on
  hot paths.

### 2. Tab operations — `src/ui/tabs.c`

```c
int  yew_tab_open(Ed *ed, const char *path);      /* -> index; -1 refused */
bool yew_tab_close(Ed *ed, int idx);              /* false = vetoed (dirty) */
int  yew_tab_index_of_id(const Ed *ed, u32 id);   /* -1 gone */
int  yew_tab_find_by_path(const Ed *ed, const char *path);
void yew_tab_switch(Ed *ed, int idx);
void yew_tab_reorder(Ed *ed, int from, int to);   /* insertion, not swap */
bool yew_tab_modified(const Ed *ed, int idx);     /* derived, see D4 */
```

- `yew_tab_open` on a path already open switches to it (one tab per file;
  the same buffer in two *panes* is fine, two *tabs* is not).
- Close decides the survivor **by id before compacting**: not closing the
  active tab → active stays itself; closing the active → the tab to its
  right, else left. Then compact, then `yew_tab_index_of_id(survivor)`.
  Sample (the order is the point):

```c
u32 survivor = pick_survivor_id(ed, idx);     /* BEFORE the memmove */
tab_destroy(&ed->tabs.v.data[idx]);
vec_tab_remove(&ed->tabs.v, idx);             /* compaction */
ed->tabs.active = yew_tab_index_of_id(ed, survivor);
```

- `yew_tab_reorder` is an **insertion**: dragging a tab three places right
  leaves the two it passed in their original relative order (what every
  editor does; a swap does not). The active tab is a position, so it is
  remapped through the shift:

```c
/* where an index lands after from -> to moves */
static int shifted_index(int i, int from, int to) {
    if (i == from) return to;
    if (to > from) return (i > from && i <= to) ? i - 1 : i;
    return (i >= to && i < from) ? i + 1 : i;
}
```

- Tab cap: 512 (a group opened from a directory routinely exceeds small
  caps). `yew_tab_open` at the cap **fails loudly** (message + -1); callers
  must check. Pitfall (facsimile): a silent cap failure made callers load
  the new file's text into the still-active tab — Ctrl-S then wrote the new
  content over the old file's path. The return value is not decoration.

### 3. Tab strip rendering — `src/ui/tabs.c` + the strip engine

One layout engine, reused verbatim by Sprint 24's row-2 member strip (that
reuse is why the two rows can never disagree about where a click landed):

```c
typedef struct { char label[64]; i32 payload; bool dim; } StripEntry;
typedef struct { int idx; u16 col0, col1; } StripSpan;    /* cells */

void yew_strip_layout(const StripEntry *e, int n, u16 width,
                      int active, int *scroll /*in-out*/,
                      StripSpan *spans, int *n_spans,
                      bool *more_left, bool *more_right);
```

- Row 1 (screen row 0) shows one entry per tab: `[N: name*]` where N is the
  1-based index, `name` the basename, `*` present iff modified. Orphan
  files (outside the workspace, Sprint 25 refines) render dim. Active entry
  in reverse video. Entry labels clip to 24 cells via the Sprint 2 clipper.
- Overflow: `*scroll` is the first visible entry; the engine keeps the
  active entry visible (adjusting scroll minimally), sets `more_left` /
  `more_right`. Renderer draws `<` at the left edge and `>N` at the right
  (N = entries past the last shown), both dim, both clickable:
  `YEW_REGION_TAB_SCROLL` payload −1 / +1 scrolls one entry per click.
- Every drawn span registers `YEW_REGION_TAB` with payload = tab index
  (positive; negative arrives with Sprint 24 groups) using the **same
  `StripSpan` cells it drew with**.
- Pitfall: the span is in cells from the same layout that drew it. Any
  "recompute position from strlen while hit-testing" reintroduces the
  multibyte click-shift bug the Sprint 22 law forbids.

### 4. Modified marker — derived, never cached

`yew_tab_modified` asks the buffer's undo tree whether its current node is
the clean node recorded at last load/save (Sprint 10 API). No `bool dirty`
field exists on `Tab`. Why: a cached flag is a label that can lie — undo
back to the clean state must clear the `*`, and every historical editor bug
in this area is a flag that drifted. (Facsimile carries the same rule for
group member counts; Sprint 24 inherits it.)

### 5. Tab commands

| Command | Behavior |
|---|---|
| `ed.tab.new` | open untitled scratch tab |
| `ed.tab.open` | open path (E-mode arg) |
| `ed.tab.close` | close active; dirty → prompt (D6) |
| `ed.tab.next` / `ed.tab.prev` | cyclic switch by index |
| `ed.tab.goto` | numeric arg: switch to tab N (1-based; 0 = tab 10) |
| `ed.tab.move` | numeric arg: reorder active tab to position N |

- All through the Sprint 13 registry. Default bindings ship in keymap data
  (L mode `t`-prefix family; `alt+1..9,0` → `ed.tab.goto 1..10` where the
  terminal delivers them).
- `ed.tab.goto` this sprint jumps immediately and completely. The 500 ms
  digit-extension window (and its group-member variant) is Sprint 24's
  deliverable — it wraps this command, so keep `goto` a pure function of
  its argument.
- Switching tabs swaps the visible pane tree; each `Win`'s cursors and
  viewport live on the Win and need no save/restore choreography (the
  facsimile global-editor-state sync dance is designed out by owning state
  where it lives).

### 6. Close-with-dirty prompt flow

`ed.tab.close` on a modified tab prompts on the message line:

```
save changes to src/main.c?  [w]rite  [d]iscard  [esc] cancel
```

- One keypress answers; the prompt is a mode-layer (Sprint 13 keymap layer)
  so every other key is swallowed with a hint, Esc cancels the close.
- `w` runs the Sprint 8 atomic save, then closes only on save success —
  an I/O error aborts the close with the error message (invariant 1: no
  path where "close" discards bytes the user asked to keep).
- `ed.quit` with dirty tabs iterates the same prompt per dirty tab, plus
  `[a]ll` (write all) and `[q]uit anyway`; each written file goes through
  the same single save path.
- Pitfall: the prompt must capture the **tab_id**, not the index — another
  event (e.g. an async job closing a tab, Sprint 19) can compact the array
  while the prompt is up. Resolve the id when the answer arrives; id gone
  → prompt dissolves silently.

### 7. Defer

- `group_id`/`group_ordinal` semantics, row-2 strip, group entries on row 1
  (negative payloads), digit-extension window: Sprint 24. Negative
  `YEW_REGION_TAB` payloads now identify groups through the sign convention
  reserved here; nonnegative payloads continue to identify tabs.
- `deferred` tabs / lazy hydration: Sprint 24 (`yew_tab_open` always loads;
  the field exists so Sprint 25's schema work can proceed in parallel).
- Persistence of the tab set: Sprint 25.
- Drag-reorder, wheel over the strip, tab context menu: Sprint 27 (clicks
  on `YEW_REGION_TAB` switch; everything else on the strip is ignored).

## Testing Strategy

- **Unit** (`tests/unit/test_tabs.c`, `test_strip.c`): id stability across
  open/close/reorder storms (close 3 of 7, assert every survivor's id and
  the active id); `shifted_index` exhaustive over n=6 (all from/to pairs vs
  a naive simulation); survivor selection for all three close cases; cap
  refusal returns -1 and mutates nothing; find_by_path with symlinked and
  relative inputs (canonicalization single-site); strip layout: scroll
  minimality (active just off right edge scrolls by exactly enough),
  more_left/more_right counts, span/label cell agreement with CJK labels;
  modified derives from undo (edit → true, undo → false, redo → true).
- **Pty** (`tests/pty/tabs_*.golden`): 3 tabs rendered, active reversed;
  overflow at 40 cols showing `<` and `>N`; click on `>N` scrolls; click
  on a tab span switches (assert active, not just pixels); `*` appears on
  edit and disappears on undo; dirty-close prompt full flow (w/d/esc).
- **Fuzz**: 10k random open/close/switch/reorder/goto ops against a naive
  oracle tracking (id, path) pairs; assert set equality + active id after
  every op; ASan/UBSan.
- **Regression fixture**: the survivor-by-id case — close the active tab
  while a lower-indexed tab exists, assert the neighbor (not the shifted
  index) becomes active.

## Definition of Done

1. ≥ 50 unit assertions across test_tabs/test_strip; gcc+clang clean.
2. Fuzz oracle run (10k ops, seed pinned + one random lane) green under
   ASan/UBSan.
3. Pty goldens byte-exact twice in a row; the click-to-switch test passes
   with a two-CJK-tab strip (multibyte spans).
4. `grep -rn "dirty\|modified" src/ui/tabs.c` shows no stored boolean —
   the marker is a call into undo state.
5. Close of active tab with 512-op history frees the pane tree and buffer
   view with zero leaks (valgrind lane).
6. Cap behavior: opening tab 513 fails with a message; `yew_tab_open`
   return checked at every call site (grep: no bare calls).
7. Dirty-close prompt: `w` on a read-only file aborts the close and shows
   the I/O error; the tab survives with its text intact.
8. Digit `ed.tab.goto 0` reaches tab 10; out-of-range N messages and
   stays put.
9. Negative `YEW_REGION_TAB` payloads now identify groups as delivered by
   Sprint 24; a hand-built-region unit test proves sign decoding cannot
   confuse a stable group identity with a tab index.
10. Two runs of the full pty suite produce byte-identical grids
    (invariant 5).
