# Sprint 5: Cell Grid and Renderer — Damage Diff to a Minimal Escape Stream

## Prerequisites

- Sprint 0 — `Bytebuf` (named in s00 §7 as the render escape stream's
  substrate), `Interner` (named there as `Cell`'s grapheme ids), `Arena`,
  `yew_log`, `YEW_BUG`.
- Sprint 1 — unit harness, `make test`, the determinism lane, `bans.sh`.
- Sprint 2 — UTF-8 decode, grapheme segmentation, width tables. The grid
  has no `TextBuf` — only bytes — so it uses s02's flat-buffer entry
  points: `int yew_cluster_width(const u8 *s, size_t len)` → cells for one
  segmented cluster (0/1/2; 4 for a U+DC80–DCFF escape, −1 never reaches
  here because TAB is resolved by the caller before a cell is written),
  and `yew_gb_next_bytes` as the cluster scanner. Note the `_bytes` suffix
  convention: `yew_grapheme_next` is s09's `TextBuf`-flavored wrapper and
  is *not* what the grid calls.
- Sprint 3 — `Tty`, `TtyCaps` (`truecolor`, `sync_output`), raw mode with
  `OPOST` cleared so our bytes reach the wire verbatim, `rows`/`cols`, and
  the restore blob (it already emits `CSI ?2026l` and `CSI ?25h`, which is
  what makes mode 2026 safe to use at all). Cursor visibility is declared
  renderer-owned there.
- Sprint 4 — `yew_input_enable`; no rendering dependency, but the two share
  the "one blob, byte-compared" discipline.
- Binding: `00-decisions.md` (Terminal row: truecolor with 256/16 fallback,
  synchronized output, damage-tracked double buffer, *"O(damage) renders,
  never full repaints per keystroke"*; quality bars; invariants 4 and 5),
  `01-architecture.md` §Rendering.

## Goals

Land `src/term/grid.c|h` and `src/term/render.c|h`: the `Cell` layout with
a pinned size budget, the front/back double buffer with per-row damage
spans, wide-glyph pairs with a continuation sentinel and the three rules
that keep them consistent, and the diff that turns damage into the
*minimal* escape stream — a cursor-motion cost model, SGR run coalescing,
an erase-to-EOL heuristic, and the truecolor → 256 → 16 ladder with
explicit integer formulas. Frames are wrapped in synchronized output (2026)
when Sprint 3's probe said so. Plus the perf harness with committed
baselines derived from the `00-decisions.md` budgets. Nothing here knows
what a buffer, viewport, statusline or theme is: drawing a file's text is
Sprint 15, pane rects Sprint 22, themes Sprints 39/41. Sprint 6's pty
harness verifies the emitted stream from the other side and owns the
first-paint milestone.

## Deliverables

### 1. `Cell` — the size budget and why — `src/term/grid.h`

```c
typedef struct YewColor {      /* exactly 4 bytes                         */
    u8 tag;                    /* 0 default, 1 indexed, 2 rgb             */
    u8 r, g, b;                /* tag==1: r is the palette index          */
} YewColor;
_Static_assert(sizeof(YewColor) == 4, "colour must be word-sized");

typedef struct Cell {
    union {                    /* anonymous union — inside the C11 box    */
        u8  utf8[8];           /* NUL-padded cluster; utf8[0]==0 = blank  */
        u32 id;                /* intern id when flags & CELL_INTERNED    */
    };
    YewColor fg, bg;
    u16 attrs;                 /* §2 bitmask                              */
    u8  w;                     /* 1 normal, 2 wide head, 0 continuation   */
    u8  flags;                 /* CELL_INTERNED                           */
} Cell;
_Static_assert(sizeof(Cell) == 20, "Cell size budget — see below");
```

**The budget.** Eight inline bytes covers every cluster real text produces
at speed: any single scalar (≤ 4 B), CJK (3), an emoji (4), emoji + VS16
(7), a letter with one or two combining marks (≤ 7). ZWJ families, flag
pairs and long combining stacks spill to the interner — under 0.1 % of
cells in any corpus we care about, and Sprint 0's interner already assigns
ids in deterministic first-intern order, so spilling costs no determinism.

Size matters because **the diff is a linear scan over both buffers**, and
its working set decides whether a full repaint stays in cache:

| Grid | Cells | One buffer | Both |
|---|---|---|---|
| 80×24 | 1 920 | 37.5 KiB | 75 KiB (L1/L2) |
| 200×50 | 10 000 | 195 KiB | 391 KiB (L2) |
| 400×100 | 40 000 | 781 KiB | 1.5 MiB (L3) |

Why not 16 B (a `u32` codepoint-or-id): it forces every `é` written as
base+combining into the interner, so ordinary European text pays a hash
lookup per cell — the common case funding the rare one. Why not 32 (16 B
inline): the table doubles, a 200×50 repaint leaves L2, and the gain is
clusters appearing in under a thousandth of cells. 20 bytes is the knee;
changing it means updating the `_Static_assert` *and* this paragraph.

- Pitfall: `yew_cell_eq()` is a 20-byte `memcmp`, which is only correct
  because `yew_grid_put` zeroes the whole union before writing a shorter
  cluster over a longer one. That zeroing is mandatory; one unit test
  writes a long cluster then a short one into the same cell.

### 2. Attributes and the shared-reset trap

| Bit | Attr | Set | Reset | Bit | Attr | Set | Reset |
|---|---|---|---|---|---|---|---|
| 0 | bold | `1` | `22` | 5 | blink | `5` | `25` |
| 1 | dim | `2` | `22` | 6 | reverse | `7` | `27` |
| 2 | italic | `3` | `23` | 7 | conceal | `8` | `28` |
| 3 | underline | `4` | `24` | 8 | strike | `9` | `29` |
| 4 | undercurl | `4:3` | `24` | 9 | overline | `53` | `55` |

- **Pitfall — `22` clears bold *and* dim** (and `24` clears underline *and*
  undercurl). Turning bold off on a cell that is also dim must emit `22`
  then re-emit `2`. The SGR delta builder handles the shared-reset groups
  explicitly; a unit test walks all 16 bold/dim transition pairs.
- Undercurl (`4:3`) is emitted only when `caps.truecolor` — roughly the
  same terminal population understands the colon subparameter form.
  Elsewhere it degrades to plain underline. Underline *colours* (`58;…`)
  are Sprint 41's problem.

### 3. `Grid` — double buffer and damage — `src/term/grid.c`

```c
typedef struct Damage { u16 lo, hi; } Damage;  /* [lo,hi); lo>=hi = clean */

typedef struct Grid {
    u16       rows, cols;
    Cell     *front;            /* believed on screen                    */
    Cell     *back;             /* what we want                          */
    Damage   *dmg;              /* one per row                           */
    u16       dmg_lo, dmg_hi;   /* dirty row bound: never walk 2000 clean
                                   rows to find one dirty span           */
    Interner *gi;
    u16       cur_row, cur_col; bool cur_vis;
    Cell      blank;            /* space, default fg/bg, no attrs        */
} Grid;

bool yew_grid_init(Grid *g, Interner *gi, u16 rows, u16 cols);
bool yew_grid_resize(Grid *g, u16 rows, u16 cols);
void yew_grid_clear(Grid *g);                          /* back := blank  */
u16  yew_grid_put(Grid *g, u16 row, u16 col, const u8 *cluster, size_t n,
                  YewColor fg, YewColor bg, u16 attrs);   /* → next col  */
u16  yew_grid_puts(Grid *g, u16 row, u16 col, const u8 *s, size_t n,
                   YewColor fg, YewColor bg, u16 attrs);  /* segments s  */
void yew_grid_fill(Grid *g, u16 row, u16 c0, u16 c1, Cell c);
void yew_grid_cursor(Grid *g, u16 row, u16 col, bool visible);
void yew_grid_mark_all(Grid *g);
bool yew_cell_eq(const Cell *a, const Cell *b);
```

- Damage is a **span per row**, not a bitmap: 4 bytes per row, and the diff
  walks `[lo,hi)` directly. Scattered edits widen the span to the whole row
  — deliberately conservative, because the diff still compares cells and
  emits nothing for equal ones. Damage bounds the *compare* region; the
  compare decides the *bytes*. Dropping damage and diffing everything costs
  391 KiB of memory traffic per keystroke at 200×50 and eats the 5 ms
  keypress budget by itself.
- `yew_grid_resize` reallocates both buffers, fills `front` with a value no
  real cell can equal (`w = 0xff`), and marks every row dirty: after a
  SIGWINCH the terminal's contents are unknowable. Pitfall: preserving
  `front` across a resize "to save work" is the classic post-resize garbage
  screen.
- `yew_grid_cursor` sets state; the renderer emits the cursor once per
  frame, at the end, inside the synchronized pair.

### 4. Wide glyphs: the pair, the sentinel, three rules

A width-2 cluster owns `(row,col)` and `(row,col+1)`: the **head** carries
the bytes with `w = 2`; the **continuation** carries no bytes, `w = 0`, and
the *same* fg/bg/attrs as the head — style copying is what lets an SGR run
coalesce straight through a CJK character instead of breaking at every
glyph.

```c
/* Clear whichever pair overlaps col, so a half-glyph cannot survive, and
 * widen the damage range the caller is about to commit.                 */
static void grid_break_pair(Grid *g, u16 row, u16 col, u16 *lo, u16 *hi) {
    Cell *r = &g->back[(size_t)row * g->cols];
    if (r[col].w == 0 && col > 0) {        /* landed on a tail:          */
        r[col - 1] = g->blank;             /* its head is now a lie      */
        if (col - 1 < *lo) *lo = (u16)(col - 1);
    }
    if (r[col].w == 2 && col + 1 < g->cols) /* clobbered a head:         */
        r[col + 1] = g->blank;              /* its tail is now an orphan */
    if (col + 2 > *hi) *hi = (u16)(col + 2 < g->cols ? col + 2 : g->cols);
}
```

1. **Overwriting one half must destroy the other.** An orphan continuation
   renders as a stale half-glyph that survives every later frame, because
   the diff sees `front == back` there and emits nothing. `grid_break_pair`
   runs on *every* write, single-width included, and the damage span is
   widened to cover the partner — forgetting the widening is the same bug
   one step later.
2. **A wide glyph at the last column is never written.** `yew_grid_put` at
   `col == cols-1` with width 2 writes one styled space and returns `cols`.
   Why not write the head and let the terminal cope: emulators disagree
   about deferred-wrap timing at the last column, so the result would
   differ per terminal — a direct invariant-5 violation. The caller learns
   from the return value that the glyph did not fit and wraps or truncates
   by its own rules (Sprint 15).
3. **The cursor never sits on a continuation.** `yew_grid_cursor` snaps
   left when `back[row][col].w == 0`. `CUP` into a continuation column puts
   the hardware cursor inside a glyph; where it *appears* is
   emulator-defined and the next relative motion starts from a position we
   do not know. Snap-left is deterministic and matches what Sprint 9's
   `yew_ccol_to_off` already does for clicks.

Zero-width clusters are appended to the preceding cell's inline bytes when
they fit, else the whole base+marks sequence is interned. A width-0 cluster
at column 0 has nothing to combine with: dropped and logged.

### 5. The diff and the cursor-motion cost model — `src/term/render.c`

```c
typedef struct Render {
    YewColor fg, bg; u16 attrs; bool pen_known;  /* believed SGR state   */
    u16 row, col; bool pos_known;                /* believed cursor      */
    u8  tier;                                    /* §7                   */
    bool sync;                                   /* caps.sync_output     */
    u64 frames, bytes;
} Render;

void   yew_render_init(Render *r, const TtyCaps *caps,
                       const char *(*getv)(const char *));
size_t yew_render_frame(Render *r, Grid *g, Bytebuf *out);
void   yew_grid_flip(Grid *g);      /* back → front, damage cleared      */
```

`yew_render_frame` appends to `out` and never touches an fd; the caller
does one `write(2)`, then flips. Why the split: the pty harness and the
perf harness both want the byte stream without a terminal, and one `write`
per frame is what makes synchronized output mean anything.

Per dirty row, walk `[lo,hi)` comparing `back` to `front`; when a run of
changed cells ends, get the cursor to the next changed cell:

| Situation | Emit | Cost |
|---|---|---|
| Already at target | nothing | 0 |
| Same row, gap `d ≤ 3`, every skipped cell single-width with style equal to the pen, and their encoded bytes cost less than CUF | the skipped cells' bytes | actual grapheme bytes |
| Same row, gap `d ≥ 4`, a skipped cell forces an SGR change or is wide, or encoded grapheme bytes cost at least CUF | `CSI <d> C` (CUF) | 4–5 B |
| Different row, or `!pos_known` | `CSI <row+1>;<col+1> H` (CUP) | 6–10 B |
| Frame start at (0,0) | `CSI H` | 3 B |

- Re-emitting skipped cells is legal only because they are *unchanged* —
  writing them again is a no-op on screen. Compare the actual encoded
  grapheme-byte total with the encoded CUF length; a one-cell combining
  cluster may already exceed `CSI 1 C`. The style-equality gate matters: a
  gap needing an SGR pair costs ≥ 10 B plus the bytes, losing to CUF
  immediately.
- **Never rely on autowrap to reach the next row.** Always CUP across rows,
  even to `(row+1, 0)`. Terminals differ on when the pending-wrap flag
  latches, and `\n` with `OPOST` cleared does not return the carriage. Four
  saved bytes are not worth a nondeterministic frame.
- **Erase-to-EOL.** When the row's remainder is entirely blank cells with
  `bg.tag == 0` (terminal default) and `attrs == 0`, and the run is ≥ 6
  cells, emit `CSI K` (3 B) instead of spaces. Restricting it to the
  default background is the trick: `EL` erases with the *current*
  background on terminals with BCE and the *default* background on those
  without, and we cannot probe which — so we use it only where both agree.
  Pitfall: `CSI K` after setting a non-default background paints the tail
  wrong on non-BCE terminals, and only on someone else's machine.
- **SGR run coalescing.** Diff each cell's style against the pen and emit
  only the delta, all parameters in one `CSI … m`. If the delta needs ≥ 3
  reset codes, emit `CSI 0 m` and re-establish the full style instead —
  cheaper and immune to §2's shared-reset trap. `pen_known` starts false
  each frame, so the first styled cell emits a full style: frames are
  independent and a dropped frame cannot corrupt the next.
- Blank cells emit one space, never a previous occupant's bytes.

### 6. Frame envelope

```
[ CSI ?2026 h ]     iff sync && the frame is non-empty
[ CSI ?25 l ]       iff the frame emits >= 1 cell
  … row runs …
[ CSI <r>;<c> H ]   final cursor position
[ CSI ?25 h ]       iff cur_vis
[ CSI ?2026 l ]     iff sync && the frame is non-empty
```

- **A frame with no terminal-state change emits zero bytes** — not an empty
  BSU/ESU pair and not a redundant cursor reposition. Cursor position and
  visibility are explicit render damage: a cursor-only change emits the
  minimal CUP/visibility delta even when no cells changed. The loop calls
  `yew_render_frame` after every input burst and most change nothing; a
  12-byte no-op per burst is measurable at typematic repeat rates and shows
  as terminal-side flicker.
- Hide/show brackets only non-empty frames. Without 2026 this is what stops
  the cursor being drawn at every intermediate position as runs stream out.
- **Pitfall — never nest BSU.** Some terminals count 2026 nesting, some
  latch it. `render.c` is the tree's only 2026 emitter and emits exactly
  one balanced pair per frame. Sprint 3's restore blob emits `CSI ?2026 l`
  unconditionally, so a crash mid-frame cannot leave a frozen screen —
  that guarantee is the licence to use the mode at all.
- Gating is entirely `caps.sync_output` from s03's DECRQM probe. A dumb
  terminal renders bare frames: correctness unchanged, only tearing.

### 7. Colour tiers and the downconversion formulas

`u8 yew_render_tier(const TtyCaps *, const char *(*getv)(const char *));`
— first hit wins, env lookup injected exactly as in s03 §7:
`YEW_COLORS` = `truecolor|256|16` → forced; else `caps.truecolor` →
truecolor; else `TERM` containing `256color` or `direct` → 256; else 16.

| Tier | Foreground | Background | Default |
|---|---|---|---|
| truecolor | `CSI 38;2;R;G;B m` | `CSI 48;2;R;G;B m` | `39` / `49` |
| 256 | `CSI 38;5;N m` | `CSI 48;5;N m` | `39` / `49` |
| 16 | `30`–`37`, bright `90`–`97` | `40`–`47`, bright `100`–`107` | `39` / `49` |

Pitfall: use `;` separators, not the ITU `38:2::R:G:B` colon form — the
colon form reads ECMA-48 more correctly and is rejected by a large fraction
of terminals in the field.

**RGB → 256.** Regions: 0–15 system, 16–231 the 6×6×6 cube, 232–255 the
grayscale ramp.

```c
static const u8 CUBE_LVL[6] = { 0, 95, 135, 175, 215, 255 };
static u8 cube_idx(u8 v) { return v < 48 ? 0 : v < 115 ? 1 : (u8)((v-35)/40); }

/* Weighted squared distance; 2/4/3 approximates luminance sensitivity
 * without a colour-space conversion. Integer only — invariant 5.       */
static u32 dist(u8 r1,u8 g1,u8 b1, u8 r2,u8 g2,u8 b2) {
    int dr = r1-r2, dg = g1-g2, db = b1-b2;
    return (u32)(2*dr*dr + 4*dg*dg + 3*db*db);
}

u8 yew_rgb_to_256(u8 r, u8 g, u8 b) {
    u8  ri = cube_idx(r), gi = cube_idx(g), bi = cube_idx(b);
    u8  cube = (u8)(16 + 36*ri + 6*gi + bi);
    u32 dc = dist(r,g,b, CUBE_LVL[ri], CUBE_LVL[gi], CUBE_LVL[bi]);
    int avg = (r + g + b) / 3;              /* ramp: 232+i holds 8+10*i  */
    int gidx = (avg - 8 + 5) / 10;          /* round to the nearest step */
    if (gidx < 0) gidx = 0;
    if (gidx > 23) gidx = 23;
    u8  gv = (u8)(8 + 10*gidx);
    return dist(r,g,b, gv,gv,gv) < dc ? (u8)(232 + gidx) : cube; /* tie→cube */
}
```

Indices 0–15 are never a conversion *target*: those entries are user-themed
and unknowable, so mapping a computed colour into them would make our
output depend on the reader's terminal theme. The cube and ramp are fixed
by the xterm palette, so the same RGB always yields the same index — which
is what invariant 5 needs.

**→ 16.** Nearest of the canonical sixteen under the same metric (an index
in 16–255 is first expanded to RGB by the formulas above, so there is
exactly one conversion path):

| i | Name | RGB | i | Name | RGB |
|---|---|---|---|---|---|
| 0 | black | `00 00 00` | 8 | bright black | `80 80 80` |
| 1 | maroon | `80 00 00` | 9 | red | `ff 00 00` |
| 2 | green | `00 80 00` | 10 | lime | `00 ff 00` |
| 3 | olive | `80 80 00` | 11 | yellow | `ff ff 00` |
| 4 | navy | `00 00 80` | 12 | blue | `00 00 ff` |
| 5 | purple | `80 00 80` | 13 | fuchsia | `ff 00 ff` |
| 6 | teal | `00 80 80` | 14 | aqua | `00 ff ff` |
| 7 | silver | `c0 c0 c0` | 15 | white | `ff ff ff` |

- Ties resolve to the **lower** index — an explicit rule, because a
  tie-break by iteration order is exactly what differs between compilers
  and breaks golden files.
- Pitfall: these are canonical values, not what the user's terminal shows.
  We map by them and accept the mismatch; querying OSC 4 for all sixteen
  costs a probe round-trip per colour and still fails on terminals that lie.
- Indexed input (`tag == 1`, 0–15) passes through untouched at every tier —
  a theme asking for "ANSI 4" gets ANSI 4, which is the point.

### 8. Perf harness — `tests/perf/perf_render.c`

Budgets derived from `00-decisions.md` rather than invented: the scroll
gate is full-viewport redraw ≥ 120 fps = 8.3 ms per frame for the *whole*
pipeline; layout, text extraction and highlighting share that frame, so the
renderer's slice is capped at 25 %.

| Scenario | Grid | Budget |
|---|---|---|
| Full repaint (every cell changed) | 200×50 | **2.0 ms** diff + emit |
| Full repaint | 80×24 | 400 µs |
| Single-cell change (a typed character) | 200×50 | **50 µs** |
| Single-row change | 200×50 | 150 µs |
| Zero-state-change frame | any | 0 bytes, ≤ 2 µs |
| Bytes, single-cell change | 200×50 | ≤ 32 B |
| Bytes, full 80×24 ASCII repaint, one style | — | ≤ 2 200 B |

Baselines in `tests/perf/baselines/render.txt`; the harness prints
`scenario ns_median ns_p99 bytes` and fails when a median regresses past
1.2×. `make perf` builds and runs it. CI's perf lane is advisory until
Sprint 56 calibrates reference hardware (invariant 4 hardens there), but
the **byte-count** rows are hard gates today — they are deterministic and
hardware-independent.

### 9. Defer

- Drawing a buffer's text, viewport, scrolling, gutter, statusline →
  Sprint 15. `grid.c` has no notion of a `TextBuf`.
- `Rect`, pane trees, the clickable-region registry → Sprint 22; this
  sprint's fill/put take explicit row/col spans.
- Themes, attribute palettes, underline colours → Sprints 39/41.
- **Scroll-region optimization** (`CSI r` + `IL`/`DL` to move rows rather
  than rewrite them) → Sprint 15, where scrolling first exists. Until then
  a one-line scroll is a full-viewport diff — correct and within budget.
  `render.c` emits none of `CSI r`, `L`, `M`, `S`, `T`, and Sprint 6's VT
  treats any of them as a test failure.
- OSC 52 clipboard writes → Sprint 12 (`render.c` never emits OSC).
  Sixel/kitty graphics: never. At this sprint's landing `yew <file>` still
  named Sprint 14; Sprint 14 subsequently delivered file editing (S59-A10).

## Testing Strategy

- **Unit** (`tests/unit/test_grid.c`): the static asserts; put/eq with the
  long-then-short cluster case (union tail zeroing); intern spill for a
  25-byte ZWJ family and round-trip back to bytes; damage span union
  arithmetic; resize marks all dirty and preserves nothing; cursor
  snap-left on a continuation.
- **Unit — wide glyphs** (`test_grid_wide.c`): head/tail style equality;
  overwrite the head → tail blanked *and* damage covers it; overwrite the
  tail → head died; a wide cluster at `cols-1` writes a space and returns
  `cols`; a zero-width mark appended to the previous cell; a zero-width
  mark at column 0 dropped.
- **Unit — render** (`test_render.c`): golden byte strings per decision —
  CUP vs CUF vs re-emit at gaps 1…6, EL fired and suppressed (non-default
  bg), SGR delta vs full reset, all 16 bold/dim transition pairs, the frame
  envelope with and without `sync`, and the empty frame emitting exactly
  zero bytes.
- **Unit — colour** (`test_color.c`): `yew_rgb_to_256` against ≥ 40
  hand-computed vectors including cube corners (`000000`→16,
  `ffffff`→231), the `v<48` and `v<115` boundaries (47/48, 114/115), every
  grayscale step (8, 18, …, 238 → 232…255 exactly), and the cube-beats-ramp
  tie; the sixteen-colour table plus the tie-break rule; `yew_render_tier`
  over ≥ 10 env vectors.
- **Determinism** (invariant 5): render the same grid twice → byte
  identical; then reach the same *state* by a different put order → the
  same bytes. Runs in the CI determinism lane.
- **Fuzz** (`tests/fuzz/fuzz_grid.c`, joins `make fuzz`): random
  put/fill/cursor/resize streams; after every op assert no orphan
  continuation (`w==0` implies the cell left of it has `w==2`), no head
  without a tail, damage covers every cell differing from front, `cur_col`
  never on a continuation, `w ∈ {0,1,2}`. Then render and assert the stream
  contains nothing outside the closed set Sprint 6 accepts. ASan/UBSan.
- **Round-trip against a reference** (`test_render_roundtrip.c`): a compact
  in-test VT stub — the seed of Sprint 6's interpreter, same sequence set,
  moving to `tests/pty/vt.c` next sprint — replays the emitted stream onto
  a fresh grid; assert it equals `back` cell for cell. This is the only
  test that proves the *whole* diff rather than its pieces, and it runs on
  every fuzz iteration.
- **Perf**: §8, `make perf`, baselines committed.

## Definition of Done

1. `make && make test` green on gcc and clang, zero warnings under the
   locked flags; ASan/UBSan and valgrind lanes clean.
2. `sizeof(Cell) == 20` and `sizeof(YewColor) == 4` assert at compile time;
   the §1 budget table and its why live in `grid.h` as comments.
3. Round-trip property green for ≥ 5 000 fuzz iterations: replaying the
   emitted stream reconstructs `back` exactly.
4. Each wide-glyph rule has a firing test: orphan-tail, orphan-head,
   last-column refusal, cursor snap-left.
5. A frame with no cell or cursor-state change emits exactly 0 bytes; a
   cursor-only change emits its minimal state delta; a single-cell change
   emits ≤ 32 bytes (all asserted numerically).
6. Colour: ≥ 40 `rgb_to_256` vectors and all 16 nearest-ANSI vectors pass;
   every grayscale ramp value round-trips to its own index; ties resolve
   per the documented rule.
7. Determinism: two renders of one grid are byte-identical, and two put
   orders reaching the same state produce the same bytes; both in the
   determinism lane.
8. No I/O in the renderer: `grep -rn 'write(\|fwrite\|printf'
   src/term/render.c src/term/grid.c` → empty (s03's `printf` ban for
   `src/term/` still passes).
9. Terminal mode 2026 is emitted only by `render.c`, only as a balanced pair:
   `scripts/check-render.sh` scans the escape spellings without matching
   unrelated decimal data, and a 1 000-frame fuzz run counts BSU == ESU
   (S59-A10).
10. `make perf` runs the §8 scenarios against
    `tests/perf/baselines/render.txt`; byte-count rows are hard failures,
    timing rows advisory until Sprint 56.
11. Resize 80×24 → 40×12 → 80×24 forces a full repaint each time and the
    final grid is byte-identical to the original given the same puts.
12. `render.c` emits no sequence outside the set Sprint 6's VT accepts —
    pinned by the fuzz assertion here and re-verified from the other side
    next sprint.
