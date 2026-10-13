# Sprint 9: Cursors, Marks, and Typed Coordinates

## Prerequisites

- Sprint 7 — `TextBuf`, `TextIter`, `ByteOff`/`LineNo` in
  `src/text/coords.h`, line↔offset O(log n).
- Sprint 2 — UTF-8 decode, grapheme segmentation (`src/unicode/grapheme.c`),
  width tables (`src/unicode/width.c`), the lossless invalid-byte policy,
  and its golden corpus. Assumption beyond the index entry: s02's corpus
  lives under `tests/unit/fixtures/unicode/` and the segmenter exposes an
  incremental per-codepoint API (state in, boundary verdict out) — if it
  is whole-string only, this sprint adds the incremental wrapper in
  `src/unicode/` (segmentation over a piece-chunk stream needs it).
- Sprint 8 — `FileMeta.eol` (CRLF buffers arrive byte-verbatim; motion
  must treat `\r\n` as one step).
- Binding: `00-decisions.md` invariant 2; `01-architecture.md` data-model
  rule — *"Conversions live in src/unicode/ and are the only place allowed
  to do width math."*

## Goals

Land the typed coordinate system (`CharCol`, `GCol`, `CCol` joining s07's
`ByteOff`/`LineNo`) with every conversion in `src/unicode/`, the `Cursor`
type with grapheme-correct motion (never inside a cluster, never between
CR and LF), marks with explicit gravity/bias rules that survive arbitrary
edits, and the ordered multi-cursor set that Sprint 17 will drive.
Rendering consumption of `CCol` is Sprint 15; selections and simultaneous
multi-cursor *editing* are Sprint 17; word/block motion units are Sprint
16 — this sprint is char/grapheme/line motion only.

## Deliverables

### 1. Typed columns — `src/unicode/coords.h`, `src/unicode/coords.c`

```c
typedef struct { u64 v; } CharCol; /* codepoints from line start          */
typedef struct { u64 v; } GCol;    /* grapheme clusters from line start   */
typedef struct { u64 v; } CCol;    /* terminal cells from line start      */
```

Conversion API — the ONLY functions in the tree allowed to do width math
(review-rejection rule, same teeth as Cgfried's single host-sniff site):

```c
/* all take the line's Span (from yew_textbuf_line_span) as context     */
GCol    yew_off_to_gcol(const TextBuf *tb, Span line, ByteOff pos);
ByteOff yew_gcol_to_off(const TextBuf *tb, Span line, GCol g); /* clamps */
CharCol yew_off_to_charcol(const TextBuf *tb, Span line, ByteOff pos);
CCol    yew_off_to_ccol(const TextBuf *tb, Span line, ByteOff pos, u32 tabw);
ByteOff yew_ccol_to_off(const TextBuf *tb, Span line, CCol c, u32 tabw);

ByteOff yew_grapheme_next(const TextBuf *tb, ByteOff pos); /* clamp len  */
ByteOff yew_grapheme_prev(const TextBuf *tb, ByteOff pos); /* clamp 0    */
bool    yew_is_grapheme_boundary(const TextBuf *tb, ByteOff pos);
```

Implementation: decode codepoints from the s07 chunk iterator, feed the
s02 incremental segmenter, accumulate widths per s02 tables. Invalid bytes
follow the s02 escape policy: each invalid byte is its own 1-cluster,
width = the escape's display width (policy-defined), never merged into a
neighbor. `\t` maps to the next `tabw` stop in `CCol` space only.

**Pitfall — clusters straddle pieces.** A ZWJ emoji family inserted char
by char spans several pieces; conversions MUST run the decoder across
chunk boundaries via the iterator, never per-chunk. One golden case pins
exactly this (fixture built by inserting a 25-byte ZWJ sequence in 1-byte
inserts, defeating s07 coalescing with interleaved edits).

**Pitfall — `yew_ccol_to_off` on a wide glyph.** A click/`goal_col` landing
on the second cell of a CJK char resolves to the glyph's *start* offset;
never emit an offset mid-glyph. Ties round left.

### 2. Cursor — `src/text/cursor.h`, `src/text/cursor.c`

```c
typedef struct Cursor {
    ByteOff pos;       /* always a grapheme boundary — the invariant     */
    GCol    goal_col;  /* sticky column for vertical motion              */
    ByteOff anchor;    /* selection anchor; == pos when no selection     */
} Cursor;

void yew_cursor_left (const TextBuf *tb, Cursor *c);  /* grapheme        */
void yew_cursor_right(const TextBuf *tb, Cursor *c);
void yew_cursor_up   (const TextBuf *tb, Cursor *c);  /* uses goal_col   */
void yew_cursor_down (const TextBuf *tb, Cursor *c);
void yew_cursor_line_home(const TextBuf *tb, Cursor *c);
void yew_cursor_line_end (const TextBuf *tb, Cursor *c); /* before EOL   */
void yew_cursor_buf_home (const TextBuf *tb, Cursor *c);
void yew_cursor_buf_end  (const TextBuf *tb, Cursor *c);
void yew_cursor_clamp(const TextBuf *tb, Cursor *c);  /* post-edit snap  */
```

Rules, pinned:

- Horizontal motion updates `goal_col` (recomputed via `yew_off_to_gcol`);
  vertical motion does NOT — that is what makes the column sticky across
  short lines. Home/end update it (vim's behavior for `$` is a `goal_col`
  sentinel `YEW_GCOL_EOL = UINT64_MAX`; pin it now, s16/s17 rely on it).
- Vertical motion: `line' = line ± 1`, `pos' = yew_gcol_to_off(line',
  goal_col)` — conversion clamps to the line's last cluster start.
- `line_end` places the cursor *before* the EOL bytes (`\n` or `\r\n`);
  `\r\n` is one cluster (GB3) so left/right can never land between them.
- `clamp` snaps `pos` to `yew_textbuf_len` then to the previous grapheme
  boundary — called after any edit the cursor didn't make. **Pitfall:** an
  edit can turn a boundary into a non-boundary (typing a combining mark
  after the cursor's base char, or the second half of a surrogate-oid
  invalid pair); clamping only against length reintroduces mid-cluster
  cursors. Always re-verify with `yew_is_grapheme_boundary`.

### 3. Marks with gravity — `src/text/mark.h`, `src/text/mark.c`

```c
typedef struct { u32 id; u32 gen; } MarkId;      /* slab handle, ABA-safe */
typedef enum { YEW_BIAS_LEFT, YEW_BIAS_RIGHT } MarkBias;

typedef struct Mark { ByteOff pos; MarkBias bias; bool alive; } Mark;

typedef struct MarkSet MarkSet;   /* slab + pos-ordered index            */
MarkId  yew_mark_add(MarkSet *ms, ByteOff pos, MarkBias bias);
void    yew_mark_del(MarkSet *ms, MarkId id);
ByteOff yew_mark_pos(const MarkSet *ms, MarkId id);   /* YEW_BUG if dead */
void    yew_marks_adjust(MarkSet *ms, u8 op, ByteOff at, u64 len);
```

`yew_marks_adjust` is called from the single edit choke point — the same
op records Sprint 10's undo consumes (one op stream, three consumers:
journal, undo, marks; never three parallel bookkeeping paths). Adjustment
is binary search + range shift: O(log m + k) for k affected marks.

Bias table — normative; every row is a unit test:

| Event (op at `a`, length `n`) | `YEW_BIAS_LEFT` mark at `p` | `YEW_BIAS_RIGHT` mark at `p` |
|---|---|---|
| insert, `a <  p`               | `p += n` | `p += n` |
| insert, `a == p`               | stays `p` (text goes after mark) | `p += n` (mark rides right of text) |
| insert, `a >  p`               | unchanged | unchanged |
| delete `[a, a+n)`, `a+n <= p`  | `p -= n` | `p -= n` |
| delete `[a, a+n)`, `a <= p < a+n` | collapse to `a` | collapse to `a` |
| delete `[a, a+n)`, `p < a`     | unchanged | unchanged |

Why both biases: a selection end behaves LEFT (typing at the selection
edge shouldn't grow it) while an "insertion point after paste" behaves
RIGHT. Sprint 21's jumplist and Sprint 46's LSP positions are mark
consumers; getting bias wrong here surfaces as off-by-one rename edits
two campaigns later. Marks collapsed by deletion stay alive at `a`
(vim-style), they do not die — deletion of text is not deletion of
intent. **Pitfall:** adjust in *pre-op coordinates* exactly once; calling
adjust from both the edit site and a generic notifier double-shifts —
the choke point is singular by design.

### 4. Multi-cursor foundations — `src/edit/multicursor.h`, `src/edit/multicursor.c`

`Win` does not exist until Sprint 22; `CursorSet` is the freestanding type
`Win` will embed (stated per cross-sprint discipline).

```c
typedef struct CursorSet {
    Vec(Cursor) curs;      /* ALWAYS sorted by pos, ALWAYS deduped       */
    u32 primary;           /* index; survives merges                     */
} CursorSet;

void yew_cset_init(CursorSet *cs, Cursor primary);
bool yew_cset_add(CursorSet *cs, Cursor c);        /* false if merged    */
void yew_cset_remove_all_but_primary(CursorSet *cs);
void yew_cset_normalize(const TextBuf *tb, CursorSet *cs); /* sort+merge */
void yew_cset_adjust(CursorSet *cs, u8 op, ByteOff at, u64 len);
```

Overlap-merge rules (applied by `normalize`, which runs after every batch
motion or edit):

| Case | Rule |
|---|---|
| two cursors, equal `pos`, no selections | merge; survivor keeps the smaller index's `goal_col`; if either was primary, survivor is primary |
| selections overlap or touch (`[min(pos,anchor), max)` ranges intersect) | merge to the union span; direction (pos vs anchor order) follows the primary if involved, else the earlier cursor |
| adjacent but not touching | never merged — proximity is not overlap |

Ordering uses `yew_sort_stable` (determinism: merge survivors must not
depend on qsort tie order — same doctrine as Cgfried). `yew_cset_adjust`
maps each cursor's `pos`/`anchor` through the §3 table semantics
(pos behaves RIGHT-bias on insert-at-point — typing at a cursor moves the
cursor past the text; anchor behaves LEFT — the selection grows).
At this sprint's landing, simultaneous multi-cursor editing was deferred and
an editing path with `curs.len > 1` hard-errored naming Sprint 17.
Sprint 17 delivered multi-cursor editing through this sorted/deduplicated
`CursorSet` and one `YEW_TXN_MULTI`
transaction; the old hard error remains historical staging evidence
(S59-A11).

### 5. Grapheme-motion golden cases

Fixture `tests/unit/fixtures/unicode/motion_golden.txt` (built from the
s02 corpus) + table-driven test. Each row: byte sequence, expected cluster
boundaries, expected `CCol` widths. Minimum set:

| Case | Sequence | Clusters | Note |
|---|---|---|---|
| combining | `e` U+0301 | 1 | never land between base and accent |
| ZWJ family | 👨‍👩‍👧‍👦 (25 bytes) | 1 | one left-arrow skips all 25 bytes |
| flag pair | 🇦🇺 (RI RI) | 1 | two more RIs = second cluster, not third |
| skin tone | 👍🏽 | 1 | modifier glues |
| CRLF | `\r\n` | 1 | GB3; also `line_end` sits before `\r` |
| CJK | 漢 | 1, width 2 | `ccol_to_off` mid-glyph → glyph start |
| invalid | `0xFF 0x41` | 2 | invalid byte is its own cluster (s02 policy) |
| keycap | `1` U+FE0F U+20E3 | 1 | VS16 emoji-width interaction |
| Hangul | 각 as L+V+T jamo | 1 | GB6/7/8 |

### 6. Defer

- Rendering/`CCol` consumption, viewport, tabs-on-screen → Sprint 15.
- Word/block unit engines → Sprint 16 (`motion.c` does not exist yet).
- Selections UX, multi-cursor editing, column selections → Sprint 17.
- Named marks (vim `ma`) and jumplist → Sprint 21; `MarkSet` is the
  substrate, no user-facing surface now.
- Any keybinding — Campaign 03. This sprint is pure API + tests.

## Testing Strategy

- **Unit** (`tests/unit/test_coords.c`): conversion round-trips
  (`off→gcol→off` identity at every boundary of every golden row;
  `off→ccol→off` with tabw ∈ {2,4,8}); clamping past EOL; the
  straddled-cluster fixture (ZWJ across ≥ 3 pieces).
- **Unit** (`tests/unit/test_cursor.c`): motion over the golden fixture —
  right N times lands at each successive cluster start, then left N times
  returns exactly; up/down goal-col stickiness across a short line
  (long → short → long retains column); `YEW_GCOL_EOL` sentinel; clamp
  after edits including the combining-mark-appended-after-cursor trap.
- **Unit** (`tests/unit/test_mark.c`): all 12 bias-table cells (6 rows ×
  2 biases); slab handle reuse (`del` then `add` — stale `MarkId` with old
  `gen` is caught); 10k marks + 1k random edits vs a naive O(m) oracle.
- **Unit** (`tests/unit/test_multicursor.c`): every merge-rule row;
  primary survival through merges; determinism — two permutations of the
  same `add` sequence normalize to identical sets; adjust-through-edits
  vs naive oracle.
- **Fuzz smoke**: random edit + motion interleave (seeded, 10k ops):
  after every op, every cursor and mark satisfies
  `yew_is_grapheme_boundary || collapsed-at-delete-point` and set
  ordering invariants hold. Full campaign joins s11's fuzzer.

## Definition of Done

1. All conversion functions live in `src/unicode/`; grep gate:
   `grep -rn 'yew_off_to_ccol\|width' src/text/ src/edit/` shows call
   sites only, zero width arithmetic outside `src/unicode/`.
2. Golden-case table: all ≥ 9 rows pass for boundaries, widths, and
   left/right motion round-trip.
3. `yew_cursor_*` never produces a non-boundary `pos`: fuzz smoke (10k
   ops × 4 seeds) with the boundary assertion — zero violations.
4. CRLF: motion treats `\r\n` as one step in both directions;
   `line_end` sits before `\r`; verified on a CRLF fixture from s08.
5. All 12 mark bias cells unit-tested; 10k-mark oracle test green.
6. Merge rules: all table rows tested including primary survival and
   the two-permutation determinism case.
7. Sprint 17-delivered multi-cursor editing preserves the sorted/deduplicated
   `CursorSet`, primary-survival rules, and one `YEW_TXN_MULTI` transaction;
   the original Sprint 17 hard error was a staging guard (S59-A11).
8. `yew_gcol_to_off` on the second cell of a wide glyph returns the
   glyph start (explicit unit case).
9. ASan/UBSan/determinism lanes green; valgrind clean on all four new
   unit binaries.
10. ≥ 100 assertions across the four unit files.
