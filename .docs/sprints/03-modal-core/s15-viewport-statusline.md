# Sprint 15: Viewport, Gutter, Statusline, Message Line

## Prerequisites

- Sprint 2 — grapheme segmentation, East-Asian width + emoji tables, the
  invalid-byte escape policy.
- Sprint 5 — `Grid`/`Cell`, damage diff, wide-glyph continuation cells,
  truecolor→256→16 downconversion, synchronized output.
- Sprint 6 — pty harness and golden byte-comparison.
- Sprint 8 — `FileMeta` carries `eol`, `had_bom`, `binary`,
  `had_invalid_utf8`, `missing_final_nl` (this sprint is where they
  finally reach the user, as s08's Defer promised).
- Sprint 9 — typed coordinates and the review-rejection rule: **width math
  only in `src/unicode/`**; `YEW_GCOL_EOL` sentinel; `goal_col`.
- Sprint 13 — command registry (`ed.view.*`, `ed.ui.*`), keymap layers.
- Sprint 14 — `Ed`/`Win`, the poll loop with its timer heap, `yew_draw_win`,
  the placeholder `Viewport` and one-row footer that this sprint replaces,
  `yew_ed_resize` hook, `yew_msg`.
- Binding: `00-decisions.md` (Terminal row, quality bars: scroll
  throughput ≥ 120 fps equivalent; invariants 2, 4, 5),
  `01-architecture.md` §Rendering.

## Goals

Turn the notepad into an editor you can read: the real viewport engine
(follow-cursor with scrolloff, horizontal scroll for the `nowrap` default,
and `wrap` as an option with grapheme-aware break points and honest
logical-line↔screen-row bookkeeping), the gutter (absolute / relative /
hybrid numbering plus a reserved sign column), the statusline spec (mode
chip whose color recolors the whole footer, fuss-style), the message line
(timed messages, severity attributes, truncation with an expand key), and
SIGWINCH → relayout → full damage. Panes still do not exist (Sprint 22
subdivides the area this sprint lays out); the sign column stays empty
until git hunks (Sprint 53) and diagnostics (Sprint 46); syntax colors are
Campaign 08 — cells carry attrs from the theme's default role only.

## Deliverables

### 1. Viewport model — `src/ui/viewport.h`, `src/ui/viewport.c`

```c
typedef struct Viewport {
    LineNo top;        /* first LOGICAL line rendered                    */
    u32    top_sub;    /* wrap: which screen row *within* that line      */
    CCol   left;       /* nowrap: horizontal scroll, in cells            */
    u16    rows, cols; /* CONTENT area: excludes gutter and footer       */
    u8     scrolloff, sidescrolloff;
    bool   wrap;
} Viewport;

void yew_vp_follow (Win *w);                 /* after any cursor move    */
void yew_vp_scroll (Win *w, i32 rows);       /* screen rows, signed      */
void yew_vp_page   (Win *w, i32 pages);
void yew_vp_center (Win *w);                 /* ed.view.center           */
void yew_vp_clamp  (Win *w);                 /* after edit/resize        */
```

**Typed-coordinate discipline (the pitfall this sprint exists to avoid).**
Three unit families meet here and must never mix:

| Quantity | Type | Legal arithmetic |
|---|---|---|
| document line | `LineNo` | ± integer lines; compare with `line_count` |
| column in text | `GCol` (clusters) / `CCol` (cells) | conversions only via `src/unicode/` |
| screen row / column | `u16` grid coordinates | ± within `Rect` bounds |

A screen row is **not** a `LineNo` the moment `wrap` is on, and a `CCol` is
not a grid column until the gutter width is added. Every function that
crosses families does the conversion at a named call
(`yew_vp_row_of_line`, `yew_vp_line_of_row`, `yew_vp_gridx_of_ccol`) and
those five functions are the only places the families touch. Cgfried's
single-host-sniff-site rule, applied to viewport math.

**Follow-cursor rules** (`yew_vp_follow`, evaluated in order):

| # | Condition | Action |
|---|---|---|
| 1 | cursor row < `top` row + `scrolloff` | scroll up to satisfy scrolloff |
| 2 | cursor row > bottom row − `scrolloff` | scroll down to satisfy it |
| 3 | viewport shorter than `2*scrolloff+1` | scrolloff degrades to `rows/2`, cursor centered |
| 4 | document shorter than `rows` | `top = 0`; scrolloff never scrolls past BOF/EOF |
| 5 | nowrap: cursor `CCol` < `left + sidescrolloff` | scroll left |
| 6 | nowrap: cursor `CCol` ≥ `left + cols − sidescrolloff` | scroll right so the cursor's **whole glyph** is visible |
| 7 | wrap | `left` is forced to 0 and never consulted |

- Rule 6's "whole glyph": a wide (2-cell) glyph at the right edge must not
  be scrolled to show only its continuation cell. `yew_vp_follow` uses the
  glyph's start and end `CCol`, and the half-glyph case scrolls one extra
  cell.
- Defaults: `scrolloff = 3`, `sidescrolloff = 5`, `wrap = false`
  (`00-decisions.md` says nothing about wrap; nowrap is pinned here as the
  default because horizontal scroll keeps the line↔row mapping trivial and
  code is the target corpus). Options become configurable in Sprint 36.
- **Pitfall — scrolloff at the document edges.** Naive scrolloff scrolls
  past the last line to keep 3 rows below the cursor, leaving a screen of
  tildes. Rules 3/4 are the fix, and the exhaustive unit test walks the
  cursor down a 5-line file in a 24-row viewport asserting `top == 0`
  throughout.

### 2. Wrap mode — break points and row bookkeeping

```c
u32  yew_wrap_rows (Win *w, LineNo l);          /* screen rows for line l */
Span yew_wrap_row  (Win *w, LineNo l, u32 sub); /* byte span of sub-row   */
```

Break rules, in order (each a unit-test row):

1. Never inside a grapheme cluster (s02 boundaries are the atoms).
2. Prefer the last **break opportunity** at or before the width limit: after
   a space or tab run; before/after a CJK ideograph (no spaces exist in
   CJK text — refusing to break there produces one-word 400-cell lines);
   after a hyphen `-`, an em/en dash, or `/` when followed by a non-space.
3. No opportunity within the row (one long token) → hard break at the last
   grapheme boundary that fits.
4. A **wide glyph that does not fit in the final cell** moves to the next
   row and the final cell is padded blank. It is never split, and the pad
   cell is not selectable, clickable, or cursor-addressable.
5. Trailing spaces at a break are absorbed by the ending row (they do not
   start the next row) — otherwise every wrapped paragraph gains a
   phantom leading space.
6. A tab that crosses the edge behaves like rule 4: it moves whole.

Bookkeeping: a logical line spans `N = yew_wrap_rows(w, l)` screen rows.

- Row cache: `Vec(u32)` of row counts keyed by `(first_line_of_cache,
  cols)`, invalidated by (a) any edit touching line `l` (damage range from
  the s07 op stream), (b) `cols` change, (c) `tabwidth` change. Cache
  covers only `[top, top+rows]` plus a slack of 64 lines — enough for
  scrolling, bounded for a 100 MB file.
- **Pitfall — never compute the total screen height of the document.**
  "Scrollbar position" and "percent through" must come from `LineNo`
  arithmetic, not from summing wrapped rows over a million lines. Percent
  is line-based (§5) and stays O(1) in every mode.
- Vertical arrows in wrap mode move by **display row** (option
  `wrap_display_arrows`, default true): `ed.move.line.up/down` consult
  `yew_wrap_row` and carry the goal column as a `CCol` **within the
  display row**. In nowrap the goal column stays the s09 `GCol` of the
  logical line. One command, two carriers, chosen by mode — pinned here so
  Sprint 16's engines do not each invent an answer.
- `ed.view.toggle_wrap` recomputes the cache, clamps the viewport onto the
  cursor's row, and takes full damage.

### 3. Gutter — `src/ui/gutter.c`

```c
typedef enum { YEW_NUM_NONE, YEW_NUM_ABS, YEW_NUM_REL,
               YEW_NUM_HYBRID } NumStyle;
u16 yew_gutter_width(const Win *w);      /* cells: sign + number + gap  */
```

| Style | Cursor line shows | Other lines show |
|---|---|---|
| `ABS` | absolute (1-based) | absolute |
| `REL` | `0` | distance from cursor |
| `HYBRID` (default) | absolute | distance |

- Width = `sign_cols (2, reserved)` + `max(3, digits(line_count))` + `1`
  gap. Numbers right-aligned, gap always blank.
- Wrapped continuation rows show blanks in the number field (never a
  repeated number — the number identifies a logical line).
- **Pitfall — width changes cause full damage.** Crossing 999→1000 lines
  widens the gutter by one cell and shifts every glyph on screen. Compute
  the width once per frame in layout; when it differs from last frame, set
  `full_damage`. A per-row width computation produces a screen where half
  the lines are shifted.
- Sign column: 2 cells, always drawn, always empty this sprint. Reserved
  for git hunk signs (Sprint 53) and LSP diagnostic signs (Sprint 46);
  documented in `gutter.h` so nobody reclaims the space.

### 4. Statusline — `src/ui/statusline.c`

The footer occupies the last row (message line borrows it, §6). Layout:

```
 L  src/main.c *                       utf-8  crlf  bom   42:17   38%
└──┘└──────────────────────────┘      └──────────────────┘└─────┘└───┘
chip  path + dirty marker              encoding segments   pos    pct
```

**Mode chip recolors the whole footer** (fuss's signature): the chip is
`" L "` in `bg = role`, `fg = role_fg`, and the *entire remaining row*
takes `bg = role_dim`. The mode is readable from peripheral vision without
reading a letter.

| Mode | Chip | Theme role | Note |
|---|---|---|---|
| L | ` L ` | `mode.line` (blue) | home |
| W | ` W ` | `mode.word` (cyan) | Sprint 16 |
| B | ` B ` | `mode.block` (violet) | Sprint 16 |
| H | ` H·L ` | `mode.select` (amber) | suffix = remembered unit, Sprint 17 |
| I | ` I ` | `mode.insert` (green) | Sprint 14 |
| E | ` E ` | `mode.exec` (orange) | Sprint 18 |
| F | ` F ` | `mode.git` (magenta) | Sprint 52 |

Segments, left to right, each a `{ text, width, priority }` row:

| Segment | Content | Drop priority |
|---|---|---|
| chip | mode (above) | never dropped |
| path | shortest unambiguous path; `[no name]` when unnamed | shortened, never dropped |
| dirty | ` *` when `yew_buf_dirty` | never dropped |
| readonly | ` [ro]` when the file is not writable | 4 |
| encoding | `utf-8` (always), `bin` when `meta.binary` | 3 |
| eol | `lf` / `crlf` / `mixed!` (warn attr on mixed) | 3 |
| bom | `bom` when `meta.had_bom` | 3 |
| invalid | `!utf8` when `meta.had_invalid_utf8` | 2 |
| position | `line:GCol` — both 1-based for display | 1 |
| percent | `top` / `bot` / `all` / `NN%` | 5 (first to go) |
| badges | git branch (53), LSP (46), AI (49) — width 0 now | 5 |

- Position is `line:GCol`, not a byte or cell column: the user's mental
  model is characters. When the cursor sits past a wide glyph the cell
  column differs, and showing cells would make CJK text look mis-indented.
  A `-v` variant showing `GCol-CCol` is Sprint 36 config, not a default.
- Percent (vim's rule, pinned): `all` when the whole document fits;
  `top` when `top == 0`; `bot` when the last line is visible; else
  `(top * 100) / (line_count - rows)`. O(1) in all modes.
- **Truncation.** When the assembled width exceeds `cols`, drop segments
  by descending priority number; if still too wide, shorten the path by
  eliding directory components from the left (`…/edit/keymap.c`), keeping
  the basename whole; if still too wide, truncate the basename with `…`.
  The chip and the dirty marker survive at any width ≥ 6.
- **Pitfall — width math.** Segment widths are `CCol` from
  `yew_str_cells()` in `src/unicode/`, never `strlen`. A CJK filename with
  `strlen`-based layout overflows the row and wraps the terminal, which
  scrolls the whole screen and corrupts every subsequent damage diff.

### 5. Message line — `src/ui/message.c`

```c
typedef enum { YEW_MSG_INFO, YEW_MSG_WARN, YEW_MSG_ERROR } MsgSev;
void yew_msg(Ed *ed, MsgSev sev, const char *fmt, ...);
void yew_msg_clear(Ed *ed);
```

- The message **replaces** the statusline row while live; the mode chip
  stays (it is the modal anchor and must never disappear).
- Attributes: INFO = normal fg; WARN = `warn` role, bold; ERROR = `error`
  role, bold, plus one bell if `errorbells` (default off).
- Expiry (heap timers from Sprint 14): INFO 4 s, WARN 8 s, ERROR until the
  next key press. Replacement rule: a new message replaces a live one
  **unless** the live one is ERROR and the new one is INFO — an error the
  user has not seen is never buried by "wrote 128 lines".
- Truncation: messages longer than the available width are cut with `…`;
  `ed.ui.message_expand` (bound `C-g`) opens a temporary overlay of up to
  5 rows showing the full text, dismissed by any key. The message history
  buffer (`ed.ui.messages`) is Sprint 18 (it wants a scratch buffer).
- Prompts (Sprint 14's dirty-quit, recovery, overwrite) render in this row
  with the `prompt` role and are never expired by a timer — a prompt owns
  the row until answered.

### 6. Damage, layout, and the frame

```c
void yew_layout(Ed *ed);     /* gutter width, content rect, footer rect */
void yew_render(Ed *ed);     /* draw dirty rows → grid → s05 diff       */
```

Order per frame: layout (cheap, pure arithmetic) → draw damaged content
rows → draw gutter for those rows → draw statusline/message row when its
state changed → `yew_grid_flush` (s05 diff + synchronized output).
Row-level damage granularity; a full-viewport redraw is the worst case and
is what the throughput gate measures.

### 7. Resize — SIGWINCH → relayout → full damage

Sequence (invoked from the Sprint 14 loop's coalesced signal step):

1. `yew_tty_winsize` → new rows/cols (ignore identical sizes: terminals
   send WINCH for non-size events).
2. `yew_grid_resize` (s05) — the grid must be resized **before** any draw;
   drawing into a stale grid writes past the row array.
3. Invalidate the wrap-row cache (its key includes `cols`, so this is a
   version bump, not a free).
4. `yew_layout` → new gutter width, content rect, footer rect.
5. `yew_vp_clamp` — `top` may now exceed the document, and the cursor may
   be off-screen; clamp then `yew_vp_follow`.
6. `full_damage = true`; one render.

- **Pitfall — WINCH storms.** Dragging a window edge delivers dozens of
  WINCH bytes; the self-pipe coalesces them and the loop relayouts once
  per iteration. Relayouting per byte makes a resize drag visibly stutter
  and re-wraps a large file dozens of times.
- **Pitfall — degenerate sizes.** `cols < 20` or `rows < 3`: draw the
  content area at whatever remains (minimum 1 content row, 0 gutter) and
  never index negative. A terminal squeezed to 4×1 must render *something*
  and must not crash — a pty golden pins it.
- SIGCONT (Sprint 3's `'C'`) takes the same path plus re-entering the alt
  screen and re-pushing input modes.

### 8. Commands registered here

| Command | Flags | Behavior |
|---|---|---|
| `ed.view.scroll.up` / `.down` | REPEATABLE | one screen row, cursor pushed by scrolloff |
| `ed.view.page_up` / `.page_down` | REPEATABLE | `rows − 2` (overlap keeps context) |
| `ed.view.half_page_up` / `.down` | REPEATABLE | L's alt-arrow target from Sprint 14 |
| `ed.view.center` / `.top` / `.bottom` | — | cursor line to middle/top/bottom |
| `ed.view.goto_line` | TAKES_COUNT | line N, then center |
| `ed.view.toggle_wrap` | — | §2 |
| `ed.view.number_style` | ARITY_STR | `none` / `abs` / `rel` / `hybrid` |
| `ed.ui.message_expand` | PROMPTS | §5 overlay |
| `ed.redraw` | — | Sprint 14's `C-l`, now also drops the wrap cache |

### 9. Defer

- Syntax attributes per cell → Campaign 08 (cells carry the default role).
- Sign column content → Sprint 46 (diagnostics) / 53 (git hunks).
- Statusline badges (branch, LSP, AI) → 53 / 46 / 49; segments exist with
  width 0.
- Splits sharing the area, per-pane statuslines → Sprint 22.
- Mouse wheel scrolling → Sprint 27 (`ed.view.scroll.*` is ready).
- User-configurable statusline format string, options for scrolloff/wrap →
  Sprint 36. Message history buffer → Sprint 18.

## Testing Strategy

- **Unit** (`tests/unit/test_viewport.c`): all seven follow-cursor rows,
  each with a hand-computed `top`; the short-document and short-viewport
  degradations; the half-wide-glyph horizontal case; clamp after a delete
  that removes lines below `top`; `yew_vp_scroll` past both ends.
- **Unit** (`tests/unit/test_wrap.c`): the six break rules over a fixture
  containing an 80-cell CJK run, a 200-char URL, a ZWJ family at the
  boundary, tabs at the boundary, trailing-space runs; `sum(wrap_rows)`
  agrees with a naive re-wrapper for 1 000 random lines × widths
  {1, 2, 3, 17, 80, 200} (width 1 with a wide glyph is the nastiest case
  — assert it renders a pad cell and never loops forever); cache
  invalidation after edits vs a cold recompute (differential).
- **Unit** (`tests/unit/test_statusline.c`): segment assembly at widths
  20…200 asserting the drop order; path elision with a CJK/emoji filename
  (cells, not bytes); percent for `all`/`top`/`bot`/mid; `mixed!` and
  `!utf8` flags from `FileMeta`.
- **Unit** (`tests/unit/test_message.c`): expiry timings via the fake
  clock; the ERROR-not-replaced-by-INFO rule; truncation + expand overlay;
  prompts never expire.
- **Pty goldens**: gutter styles (abs/rel/hybrid) with the cursor on line
  1, 9, 10, 100; nowrap horizontal scroll across a CJK line; wrap mode of
  the same line; resize 80×24 → 40×12 → 80×24 (final grid byte-identical
  to the first, invariant 5); 4×1 degenerate terminal; every mode chip's
  footer recolor (drive `ed.mode.enter` for the modes that exist; the
  deferred ones are covered when their sprints land).
- **Perf** (`tests/perf/scroll.c`): full-viewport redraw throughput on a
  10 kLOC fixture in nowrap and wrap, at 80×24 and 200×60 — must exceed
  the 120 fps equivalent from `00-decisions.md`; baselines committed.
- Lanes: gcc, clang, ASan/UBSan, valgrind, determinism (double-run
  goldens).

## Definition of Done

1. All seven follow-cursor rules and all six wrap rules have dedicated
   unit assertions with hand-computed expectations.
2. `sum(yew_wrap_rows)` matches the naive oracle for 1 000 random lines ×
   6 widths, including width 1 with wide glyphs; zero infinite loops
   (test has a step budget).
3. Resize round-trip 80×24 → 40×12 → 80×24 produces a byte-identical
   grid and identical `Viewport` fields.
4. Scroll throughput gate green (≥ 120 fps equivalent full-viewport
   redraw, both wrap modes, both sizes), baselines committed.
5. Statusline: the mode chip's role color is applied to the whole footer
   row in the golden (assert the SGR run covers `cols`), and truncation
   follows the pinned priority order at ≥ 6 widths.
6. Position reads `line:GCol` and is correct after a wide glyph and after
   a tab (pty golden with `漢字\tx`).
7. `FileMeta` indicators all reachable: `crlf`, `mixed!`, `bom`, `bin`,
   `!utf8` each appear in a golden.
8. Message severities: INFO/WARN expire on schedule under the fake clock;
   ERROR survives until a key; the not-buried rule tested.
9. Gutter width change across the 999→1000 boundary triggers full damage
   (frame counter asserts one full redraw, not a partial shift).
10. `grep -rn 'wcwidth\|cells\|width' src/ui/ | grep -v 'unicode/'`
    shows calls into `src/unicode/` only — zero width arithmetic in
    `src/ui/` (the s09 rule, re-gated).
11. Degenerate 4×1 terminal renders without a crash or an out-of-bounds
    write (ASan lane).
12. ≥ 130 assertions across the four new unit files; ≥ 14 pty goldens,
    all byte-stable across two runs.
