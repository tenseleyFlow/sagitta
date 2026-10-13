# Sprint 17: Mode H and Multi-Cursor — Modal Complete

## Prerequisites

- Sprint 9 — `Cursor` (`pos`, `anchor`, `goal_col`), `CursorSet` with the
  sorted/deduped invariant, `yew_cset_normalize`/`yew_cset_adjust`, the
  merge-rule table, `MarkSet` bias rules, typed coords (`GCol`, `CCol`),
  and the staging hard-error on multi-cursor editing that this sprint
  replaces.
- Sprint 10 — undo tree with **transactions** (one multi-cursor edit = one
  undo step) and the clean-state node.
- Sprint 12 — named registers, kill-ring semantics, OSC 52 clipboard —
  yank/paste route here.
- Sprint 13 — registry + layers; every selection action is a command.
- Sprint 14 — `Ed`/`Win`, the loop, `ed_apply` (the single edit choke
  point), mode transitions and the Esc law, `prev_unit` memory field.
- Sprint 15 — statusline (the chip renders `H·<unit>`), damage model,
  theme roles.
- Sprint 16 — `UnitOps` vtable and the line/word/block/char instances,
  `yew_unit_of_mode`, the containment chain and `SelStack`
  (expand/contract, single-cursor only until now).
- Binding: `00-decisions.md` invariants 1, 4, 5, 9; `01-architecture.md`
  — *"Multi-cursor lives on the window (view), not the buffer"* and
  *"H borrows the previous unit mode's engine for extension."*

## Goals

**MILESTONE: modal complete.** H mode over *all* unit engines with the
mode-memory rule (H extends by the unit of the mode you came from,
displayed as `H·W`), char / line / rectangular selections (rectangular is
`CCol`-based, with the wide-glyph and tab edge cases pinned), the selection
action set routed through the registry (yank, delete, change, case,
indent, shift, join), the three selection→multi-cursor lifts, simultaneous
multi-cursor editing as **one** undo transaction with explicit
overlap/merge rules and the cursor-ordering invariant, and the rendering
of selection attributes plus secondary cursors. Match lift is literal-only
for 1.0; ghost completion remains single-cursor; and LSP edits require one
primary cursor. Their boundary guards are deliberate rather than unimplemented
stubs. Macro replay over cursor sets arrives with its owning campaign.

## Deliverables

### 1. H mode and the mode-memory rule — `src/edit/select.c`

```c
typedef struct HState {
    Mode           from;       /* the mode H was entered from            */
    const UnitOps *unit;       /* its engine — H's extension granularity */
    SelKind        kind;       /* CHAR | LINE | RECT                     */
    bool           sticky;     /* explicit `ed.mode.enter H W` pins unit */
} HState;
```

- `ed.mode.enter H` (no arg) uses `ed->prev_unit` (Sprint 14 records it on
  every L/W/B entry): from L → `H·L`, from W → `H·W`, from B → `H·B`.
  Entered from I, H uses the char engine → `H·C`.
- `ed.mode.enter H <unit>` (`"L"|"W"|"B"|"C"`) pins the unit
  (`sticky = true`), which is what Fletch and macros emit so a replay is
  not at the mercy of the mode the user happened to be in — the round-trip
  law (invariant 10) demands the recorded form be self-contained.
- **Statusline**: the chip reads `` H·L ``, `` H·W ``, `` H·B ``, `` H·C ``
  in the `mode.select` role, which recolors the whole footer (s15 §4). The
  suffix is the answer to "what will an arrow do right now" and is
  therefore not optional decoration.
- Arrows in H use the **same commands** as the source mode's layer
  (`ed.move.unit.*`), with one difference: motion in H moves `pos` and
  leaves `anchor` fixed. That is the entire implementation of "extend by
  the unit of the mode you came from" — no parallel motion path.
- Esc from H: clear the selection, drop extra cursors, return to L (the
  Sprint 14 collapse order). `ed.sel.swap_ends` (`o`) exchanges `pos` and
  `anchor` so the other end can be adjusted.
- Selection kind keys inside H use the `v` prefix: `v c` → CHAR
  (default), `v l` → LINE, `v r` → RECT (`ed.sel.kind`,
  `YEW_ARITY_STR`).  The prefix keeps `c` available for change and `r`
  available for replace-character capture; the original unprefixed table
  could not represent both meanings in the exact-edge key trie.

### 2. Selection geometry — `src/edit/select.h`

```c
typedef enum { YEW_SEL_CHAR, YEW_SEL_LINE, YEW_SEL_RECT } SelKind;

/* A selection is derived, never stored twice: per cursor it is
 * [min(pos,anchor), max(pos,anchor)) reinterpreted by `kind`.          */
Span yew_sel_span (const Win *w, const Cursor *c);         /* CHAR/LINE */
bool yew_sel_rect_row(const Win *w, const Cursor *c, LineNo l,
                      Span *out, CCol *c0, CCol *c1);      /* RECT      */
u32  yew_sel_rows (const Win *w, const Cursor *c);
```

| Kind | Extent |
|---|---|
| CHAR | byte range between the two ends, grapheme-aligned by construction |
| LINE | whole lines from `line_of(min)` to `line_of(max)`, including the final EOL bytes |
| RECT | for each line in the row range: the cells `[c0, c1)` where `c0 = min(CCol(anchor), CCol(pos))`, `c1 = max(...)` |

**Rectangular selections are `CCol`-based** — cells, not clusters — because
the user is selecting a visual column of the screen. The conversions are
s09's (`yew_off_to_ccol`, `yew_ccol_to_off`), so all width math stays in
`src/unicode/`. The edge cases, pinned:

| Case | Rule | Why |
|---|---|---|
| either edge falls inside a wide glyph | the glyph **is included**; `c0` floors to the glyph's start cell and `c1` ceilings to its end cell | a terminal cannot color half a cell, and deleting half a CJK char is byte confusion (invariant 2) |
| either edge falls inside a tab's expansion | the whole tab is included; `c0` floors to the tab's start cell and `c1` ceilings to its end cell | tabs are one byte; splitting one means rewriting it as spaces, which is an edit the user did not ask for |
| `rect_split_tabs` option | **not in 1.0** — deferred to Sprint 36 | pinned so nobody "fixes" the row above |
| line shorter than `c0` | contributes an **empty** range | delete/yank skip it |
| line shorter than `c0`, insert operation | padded with spaces up to `c0` | `ed.edit.rect.insert` is how column-insert works at all |
| line shorter than `c1` | range clipped at the line end | |
| line contains an invalid byte in range | the escape's cells count as its width (s02 policy); the raw byte is what gets deleted | round-trip fidelity |
| `c0 == c1` (zero width) | a rectangular **caret** — legal; used by `ed.cursor.lift.ends` | |

The per-row effective range is computed **once** per operation into a
`Vec(Span)` and shared by the edit path and the render path — the same
single-source-of-truth law Sprint 22 states for layout vs hit-testing. Two
computations means the highlight and the deletion disagree on a CJK line,
which is the classic column-select bug.

### 3. Selection actions (all through the registry)

| Command | Key (H layer) | CHAR | LINE | RECT |
|---|---|---|---|---|
| `ed.sel.yank` | `y` | span → register | whole lines, linewise flag | per-row spans, blockwise flag |
| `ed.sel.delete` | `d` / `<del>` | delete span | delete lines incl. EOL | delete each row's range |
| `ed.sel.change` | `c` | delete then enter I | delete lines, open one, enter I | delete ranges, enter I at `c0` on **every** row (multi-cursor!) |
| `ed.sel.case_upper` / `_lower` / `_toggle` | `U` / `u` / `~` | per grapheme, full Unicode case mapping | same | per row range |
| `ed.sel.indent` / `ed.sel.dedent` | `>` / `<` | covered lines | covered lines | covered lines |
| `ed.sel.shift_left` / `_right` | `A-<` / `A->` | move the selected text one grapheme | move lines | move the block one cell |
| `ed.sel.join` | `J` | join covered lines, collapsing runs of whitespace to one space; no space before `)` `.` `,` `;` | same | same |
| `ed.sel.replace_char` | `r<key>` | replace every grapheme with the key's cluster | same | per row range |
| `ed.edit.rect.insert` | `I` in RECT | — | — | insert at `c0` on every row, padding short lines |
| `ed.edit.rect.append` | `A` in RECT | — | — | insert at `c1`, padding to `c1` |

- Every one carries `YEW_CMD_CHANGES_BUFFER` (except yank) and
  `YEW_CMD_RECORDABLE`; every one runs inside a single transaction (§5)
  even for a single cursor, so undo granularity does not depend on how
  many cursors happened to exist.
- Case mapping uses the s02 tables (full mappings, so `ß`→`SS` grows the
  buffer — pitfall: an in-place same-length case conversion is wrong for
  Unicode and must not be "optimized" in).
- Indent/dedent respect the buffer's `tabwidth` and existing leading
  whitespace style; dedent on a line with no indent is a no-op, not an
  error.
- After any action, the selection collapses to a caret at the operation's
  start (except yank, which keeps the selection), and the mode returns to
  L — the Kakoune-style "selection survives" behavior is not the yew
  model, and pinning it here stops it from being reinvented per-command.

### 4. Selection → multi-cursor lift

```c
u32 yew_mc_lift_lines  (Win *w);                       /* one per line   */
u32 yew_mc_lift_matches(Win *w, const u8 *pat, u32 n); /* one per match  */
u32 yew_mc_lift_ends   (Win *w);                       /* one per end    */
```

| Command | Key | Result |
|---|---|---|
| `ed.cursor.lift.lines` | `<cr>` in H | a cursor on every line of the selection, at the selection's start `GCol`, clamped to each line's end |
| `ed.cursor.lift.matches` | `m` | a cursor at the start of every occurrence of the pattern inside the selection; pattern defaults to the word under the primary cursor, or `sarg` |
| `ed.cursor.lift.ends` | `e` | a cursor at each selection's `pos` and `anchor` (for a multi-selection set, both ends of each) |
| `ed.cursor.add.below` / `.above` | `C-A-<down>` / `<up>` | add a cursor on the next/previous line at the same `GCol` |
| `ed.cursor.drop` | `C-A-<bs>` | remove the most recently added cursor |
| `ed.cursor.collapse` | `<esc>` | keep only the primary (part of the Esc collapse order) |

- Lift leaves H and enters L with `cs.curs.len > 1`; the statusline shows
  ` L ` plus a cursor-count badge `×7`.
- `lift.matches` uses a literal scan (`memmem`-style over the s07 chunk
  iterator). Regex-driven lift remains outside the 1.0 surface; the guard
  prevents a caller from silently treating a regex as a literal pattern.
- Every lift ends with `yew_cset_normalize` (s09): sorted, deduped,
  primary preserved. A lift that produces > `YEW_MC_MAX` (10 000) cursors
  refuses with a message rather than allocating: `4 000 000 matches` is a
  typo, not an intent.

### 5. Simultaneous editing as one transaction — `src/edit/multicursor.c`

```c
CmdStatus yew_mc_run(Win *w, CmdId cmd, CmdCtx *cx);
```

`CmdCtx` (Sprint 13 §3) gains one field here — `u32 cursor_index` — which
is the *only* way a command learns which cursor it is editing. Commands
therefore stay single-cursor code. `yew_ed_invoke` remains the sole undo
transaction owner: it chooses `YEW_TXN_MULTI`, calls the runner inside the
open transaction, and commits or aborts exactly once.

```c
/* One undo step for the whole set (s10), one journal transaction (s08),
 * one damage pass.  Cursors are visited in ASCENDING order and the edit
 * choke point (ed_apply → yew_marks_adjust + yew_cset_adjust) fixes up
 * every later cursor: ONE bookkeeping mechanism, the s09 doctrine.
 * (Descending application would let us skip adjustment — rejected: two
 * mechanisms means two truths, and the one used less often rots.)      */
CmdStatus yew_mc_run(Win *w, CmdId cmd, CmdCtx *cx) {
    yew_cset_check_text(w->buf->tb, &w->cs); /* invariant in */
    CmdStatus st = YEW_CMD_OK;
    for (u32 i = 0; i < w->cs.curs.len && st == YEW_CMD_OK; i++) {
        cx->cursor_index = i;               /* the command edits cs[i]   */
        st = yew_cmd_desc(cmd)->fn(cx);
    }
    if (st != YEW_CMD_OK)
        return st;                         /* yew_ed_invoke aborts */
    yew_cset_normalize(w->buf->tb, &w->cs);
    yew_cset_check_text(w->buf->tb, &w->cs); /* invariant out */
    return YEW_CMD_OK;
}
```

**Overlap / merge rules** (applied during the run and by the final
normalize; each row is a unit test):

| Case | Rule |
|---|---|
| two cursors' edit ranges overlap | the later cursor's edit is **skipped**; its cursor merges into the earlier one; a counter is reported (`3 cursors merged`) |
| a cursor sits inside text another cursor deleted | collapses to the deletion point (s09 mark semantics), then merges with its neighbor |
| two cursors coincide after the edit | merge; survivor keeps the lower index's `goal_col`; primary wins if involved |
| selections touch or overlap after the edit | merge to the union span (s09 table) |
| adjacent but not touching | never merged |
| all cursors merged into one | the set is valid at length 1; the badge disappears |

**Transaction law**: any per-cursor failure aborts and rolls back the
*whole* set — the buffer returns to its pre-command state. This is the
same law Fletch's `edit{}` blocks obey (`02-fletch.md` requirement 4), and
sharing it means a macro over cursors cannot leave a half-applied edit.

**Cursor-ordering invariant**: `yew_cset_check` asserts ascending `pos`,
no duplicates, `primary < len`, every `pos` and `anchor` on a grapheme
boundary — called before and after every transaction in debug builds and
by the fuzzer always.

Count propagation: a count applies **per cursor** (`3x` with 7 cursors
deletes 3 graphemes at each of the 7), still inside one transaction.

### 6. Rendering — selection attrs and secondary cursors

`src/ui/draw.c` additions:

- Selection cells take the theme's `sel` background; the foreground is
  left alone so syntax colors (Campaign 08) survive. Attribute merge
  order, pinned: syntax → selection bg → search-match (Sprint 21) →
  cursor.
- **The terminal has exactly one hardware cursor.** The primary cursor is
  it (shape per mode: block in L/W/B/H, bar in I). Every secondary cursor
  is *drawn*: the cell takes the `cursor.secondary` role (reverse video in
  the 16-color fallback). Pitfall: relying on the hardware cursor for
  "the" cursor and forgetting that secondaries need painting yields a
  multi-cursor mode where you cannot see your cursors — and painting the
  primary too makes it double-render as the terminal moves its own.
- A secondary cursor at end-of-line renders on the one-cell pad past the
  last glyph; on an empty line it renders at column 0.
- Cursor-vs-selection overlap: the cursor wins its cell.
- RECT rendering consumes the same per-row `Vec(Span)` the edit path
  computed (§2), so a wide glyph is highlighted exactly where it will be
  deleted.
- Damage: a selection change damages the rows between the old and new
  selection bounds (their union), never the whole screen — dragging a
  selection down a 100 kLOC file must stay O(changed rows).

### 7. Boundaries retained after later campaigns

- Regex-driven lift remains outside 1.0; `ed.cursor.lift.matches` is literal
  and rejects a regex request rather than changing its meaning silently.
- Sprint 43 deliberately disables ghost completion for a multi-cursor set;
  LSP edits require one primary cursor. Both boundary guards remain tested.
- Macro record/replay over a cursor set → Sprint 35 (the transaction
  boundary here is exactly what replay will reuse).
- `rect_split_tabs`, `sel` theme customization, Kakoune-style
  selection-persistence option → Sprint 36.
- Mouse drag-select → Sprint 27 (the model is ready; nothing routes drag
  events yet).

## Testing Strategy

- **Unit** (`tests/unit/test_select.c`): the §2 edge-case table row by row
  on a fixture line `a漢\tb👨‍👩‍👧‍👦c` at every `CCol` pair (exhaustive over a
  40-cell line); LINE selection includes EOL bytes and behaves on the last
  line without a trailing newline; CHAR selection across CRLF.
- **Unit** (`tests/unit/test_sel_actions.c`): every §3 row × every
  `SelKind` against goldens — including `ß`→`SS` growth, join's
  punctuation rules, indent/dedent with mixed tabs and spaces, rect insert
  padding a short line, rect delete on a line containing an invalid byte.
- **Unit** (`tests/unit/test_multicursor_edit.c`): all six overlap/merge
  rows; ascending-order application vs a naive oracle that applies edits
  to a flat byte array; one-transaction proof (one `u` restores the
  pre-edit buffer for a 200-cursor insert); rollback on a failing cursor
  (buffer byte-identical to pre-command); count-per-cursor; the
  `YEW_MC_MAX` refusal.
- **Unit** (`tests/unit/test_hmode.c`): mode-memory matrix (entered from
  L/W/B/I → the right engine and the right chip string); `sticky` form;
  Esc collapse from H with a selection and 5 cursors.
- **Fuzz** (`tests/fuzz/fuzz_multicursor.c`): random buffers, random
  cursor sets (1–500), random action sequences, 100k ops — after every op
  `yew_cset_check` holds, the buffer matches an oracle that applies the
  same ops sequentially with offset fixups, and undo returns to the exact
  prior bytes.
- **Pty goldens**: H·L / H·W / H·B chips and footer recolor; char, line,
  and rect selections over a CJK+emoji+tab line (the highlight must cover
  exactly the cells the delete removes — asserted by golden **and** by
  deleting and comparing); 7 secondary cursors rendered on 7 lines; a
  1 000-cursor set (badge `×1000`, only visible cursors painted);
  simultaneous typing into 7 cursors then one `u`.
- **Perf** (`tests/perf/multicursor.c`): a 1-grapheme insert applied at
  1 000 cursors in a 10 kLOC buffer, keypress→paint within the 5 ms p99
  gate; 10 000 cursors within 50 ms (the refusal threshold's neighborhood).

## Definition of Done

1. Mode-memory: entering H from L/W/B/I selects the matching engine and
   renders `H·L`/`H·W`/`H·B`/`H·C`; the pinned form
   `ed.mode.enter H W` overrides. All five cases asserted (unit + golden).
2. Every §2 rectangular edge-case row has an assertion, including both
   wide-glyph rows, both tab rows, and the short-line pad/skip pair.
3. The highlight and the edit agree: for every rect fixture, the set of
   cells rendered selected equals the set of cells removed by
   `ed.sel.delete` (mechanically compared, not eyeballed).
4. Every §3 action is registered, recordable, and covered for all three
   selection kinds; `ß`→`SS` and the join punctuation rules asserted.
5. One transaction: a 200-cursor edit is exactly **one** undo step and
   exactly **one** journal transaction (`yew_journal_sync` called once —
   asserted via the s08 fault shim's intercept log).
6. Rollback: a per-cursor failure leaves the buffer byte-identical to its
   pre-command state (fixture + assertion).
7. All six overlap/merge rows tested; `yew_cset_check` invariants hold
   before and after every transaction in the fuzz run (100k ops × 4 seeds,
   zero violations, ASan/UBSan clean).
8. Multi-cursor vs oracle: differential fuzz green — the multi-cursor
   application of an op set equals sequential application with fixups.
9. Rendering: secondary cursors visible in the 16-color fallback lane;
   primary is the hardware cursor and is never double-painted (grid and
   raw-frame assertions prove exactly one hardware-cursor target, the
   primary cell has no secondary style, and the mode shape is correct;
   paint-time CUP escapes are not hardware cursor instances).
10. Perf gate: 1 000-cursor insert inside the 5 ms p99 keypress→paint
    budget; keyboard selection extension damages only the union of old and
    new bounds (frame counter asserts row counts, not full redraws; mouse
    drag remains Sprint 27).
11. Intentional 1.0 boundaries fail loudly and are tested: match lift rejects
    regex mode, ghost completion requires a single cursor, and LSP edits
    require one primary cursor.
12. ≥ 180 assertions across the four new unit files; ≥ 12 new pty
    goldens, byte-stable across two runs (invariant 5).
13. **Milestone gate**: a single pty script drives L → W → B → H → I →
    Esc → save on one fixture, exercising every mode's arrows, and its
    golden chain is green — modal complete.
