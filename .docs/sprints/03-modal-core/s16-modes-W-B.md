# Sprint 16: Modes W and B — The Unit Engines

## Prerequisites

- Sprint 2 — UTF-8 decode, grapheme segmentation, width tables, and the
  table generator `scripts/gen-unicode-tables` with its checked-in output.
  **Assumption beyond its index entry**: it emits grapheme + width
  properties only. This sprint extends the generator to also emit the
  **Word_Break** property table (`src/unicode/tables_wb.c`, generated,
  checked in) following the identical pattern — same UCD version pin, same
  two-level trie, same "regenerate is reproducible" CI check.
- Sprint 7 — `TextBuf`, `TextIter` chunk iteration, `yew_textbuf_line_span`,
  O(log n) line↔offset.
- Sprint 9 — typed coords, grapheme boundaries (`yew_grapheme_next/prev`),
  `Cursor` with `goal_col` and `YEW_GCOL_EOL`, `CursorSet`.
- Sprint 13 — registry + keymap layers; the arrow map below is table data.
- Sprint 14 — `Ed`/`Win`, mode transitions, L mode's line behavior (the
  line engine's reference semantics), `ed_apply` edit choke point.
- Sprint 15 — viewport/statusline; the mode chip renders `W` and `B`.
- Binding: `00-decisions.md` Modes row (*"L (home), W, B unit modes"*),
  invariants 2, 3, 5; `01-architecture.md` §Modal engine — *"Unit engines
  implement one interface … L/W/B are instances; H borrows the previous
  unit mode's engine"* and *"B mode's structural units are
  provider-layered … Same interface, so B works in a plain .txt."*

## Goals

Pin the **unit-engine vtable** and land three instances (line, word,
block) plus the char engine I mode already uses, so that "arrows always
move; the mode chooses the unit" becomes one dispatch path instead of
three. W mode: UAX #29 word boundaries with WORD (whitespace-delimited)
and subword (camelCase / snake_case / kebab-case) variants. B mode:
provider-layered structural units — bracket/brace scope with
string/comment-aware heuristics, indentation block, paragraph — chained by
strict containment so alt+arrows grow and shrink a structural selection.
The syntax provider slot was inert at this sprint's landing.
Sprint 40 delivered the syntax provider through that slot; B now prefers its
exact spans when syntax is available and still falls through to heuristics,
because B must remain genuinely useful in a plain `.txt`. Selection
*rendering* and H mode are Sprint 17; this sprint's expand/contract initially
set the cursor's anchor and head (s09 `Cursor`) before that rendering arrived.

## Deliverables

### 1. The unit-engine vtable — `src/edit/motion.h`

```c
typedef struct UnitCtx {                 /* everything an engine may see */
    const TextBuf *tb;
    const Buffer  *buf;      /* meta: eol, tabwidth; lang slot (S40)     */
    Win           *win;      /* NULL for headless/script callers         */
} UnitCtx;

typedef struct UnitOps {
    const char *name;                    /* "line" "word" "block" "char" */
    ByteOff (*next)(UnitCtx *u, ByteOff p, bool alt);
    ByteOff (*prev)(UnitCtx *u, ByteOff p, bool alt);
    ByteOff (*home)(UnitCtx *u, ByteOff p, bool alt);
    ByteOff (*end )(UnitCtx *u, ByteOff p, bool alt);
    Span    (*span)(UnitCtx *u, ByteOff p, bool alt);  /* unit under p   */
} UnitOps;

extern const UnitOps yew_unit_line, yew_unit_word,
                     yew_unit_block, yew_unit_char;

const UnitOps *yew_unit_of_mode(Mode m);   /* L→line W→word B→block I→char */
```

Contract every implementation obeys (asserted by the shared conformance
suite, §7):

1. Results are always grapheme boundaries (s09 invariant).
2. `next` is strictly increasing until end-of-buffer, where it returns
   `len`; `prev` strictly decreasing until 0. **No engine may return `p`
   unchanged for an in-range `p`** — a fixed point makes `4→` hang.
3. `home(p) <= p <= end(p)` and `span(p) == {home(p), end(p)}` for every
   engine where the unit is contiguous (all four).
4. Engines are **pure**: no editing, no mode changes, no viewport writes.
   `win` is read-only context (wrap state for the line engine's display
   rows).
5. `alt` selects the engine's documented alternate granularity (§3, §5)
   and must satisfy 1–4 identically.

**The arrow map** — pure keymap data (Sprint 13 `BindRow` tables), one row
per mode. This table *is* the modal paradigm:

| Mode | `←` | `→` | `↑` | `↓` |
|---|---|---|---|---|
| L | `unit.home` (line start) | `unit.end` (line end) | `unit.prev` (line above) | `unit.next` (line below) |
| W | `unit.prev` (word before) | `unit.next` (word after) | line above ¹ | line below ¹ |
| B | `unit.home` (block start) | `unit.end` (block end) | `unit.prev` (prev sibling) | `unit.next` (next sibling) |
| I | `char.prev` (grapheme) | `char.next` | line above | line below |

¹ W borrows `yew_unit_line`'s vertical explicitly: **there is no "word
above"**. The borrow is a different command (`ed.move.line.up/down`) bound
in W's layer, not a vtable trick — keeping the vtable honest is worth one
extra table row.

Commands (registered here, dispatching through `yew_unit_of_mode`):
`ed.move.unit.next`, `.prev`, `.home`, `.end`, and the `_alt` variants —
all `YEW_CMD_REPEATABLE` so counts work (`4→` in W = four words).

### 2. Line and char engines — `src/edit/motion.c`

Both already exist as Sprint 14 behavior; this sprint moves them behind
the vtable with no semantic change, so the conformance suite covers them
too.

| Engine | `next`/`prev` | `home`/`end` | `alt` |
|---|---|---|---|
| line | line below / above with `goal_col` (display rows when `wrap`, s15) | line start / before EOL bytes | half page down / up; first / last non-blank |
| char | grapheme next / prev (s09) | same as line's home/end | codepoint-wise (debug aid; the escape-policy view of an invalid run) |

### 3. Word engine — UAX #29 and its variants

`src/edit/word.c` + `src/unicode/wordbreak.c` (property lookup only —
width and property math stay in `src/unicode/`, s09's rule).

Implemented rules (UCD version pinned by s02; each row is a test):

| Rule | Effect |
|---|---|
| WB3 | `CR × LF` — never break inside CRLF |
| WB3a/b | break before/after Newline, CR, LF |
| WB3c | `ZWJ × ExtPict` — emoji ZWJ sequences are one word |
| WB4 | `Extend`/`Format`/`ZWJ` are absorbed into the preceding unit |
| WB5 | `AHLetter × AHLetter` — letter runs join |
| WB6/7 | `AHLetter × (MidLetter\|MidNumLetQ) AHLetter` — `don't`, `a.b` |
| WB7a/b/c | Hebrew single-quote and double-quote rules |
| WB8–11 | `Numeric` runs, `Numeric × MidNum Numeric` — `1,000`, `3.14` |
| WB12/13 | Numeric↔Letter joins; `Katakana × Katakana` |
| WB13a/b | `ExtendNumLet` glue — `foo_bar` is ONE word |
| WB15/16 | Regional-Indicator pairs (flags) |
| WB999 | otherwise break — this is why each Han ideograph is its own word |

Motion semantics on top of the boundary oracle:

- `next(p)`: the **start of the next word**, skipping intervening
  whitespace and punctuation runs (vim's `w`, not `e`). Crosses newlines;
  a blank line is itself a stop (so `→` does not fly over paragraph gaps).
- `prev(p)`: the start of the word at or before `p` (vim's `b`).
- `end(p)`: the end of the word containing `p`, else of the next word.
- `home(p)` / `span(p)`: the word containing `p`; when `p` is on
  whitespace, the whitespace run is the unit (so `H` + `→` selects the gap
  honestly rather than silently jumping).

Variants:

| Granularity | Trigger | Definition |
|---|---|---|
| word (default) | `←` `→` | UAX #29 above |
| WORD | `A-←` `A-→` (the vtable `alt`) | maximal run of non-whitespace |
| subword | `C-←` `C-→` (`ed.move.word.sub_prev/next`) | splits inside a word, §below |

Subword splits (applied *within* a UAX #29 word):

| Input | Subwords |
|---|---|
| `fooBarBaz` | `foo` `Bar` `Baz` (lower→Upper) |
| `HTTPServer` | `HTTP` `Server` (Upper-run followed by Upper+lower: split before the last Upper) |
| `snake_case_x` | `snake` `case` `x` (`_` is a separator, though WB13a made it one word) |
| `kebab-case` | `kebab` `case` (the `-` is already a WB break; subword agrees) |
| `v2Model3` | `v2` `Model3` (digits attach to the preceding run) |
| `__init__` | `init` (leading/trailing separator runs are skipped, not emitted) |

Boundary-case reference table (each row is a golden fixture line):

| Text | Cursor at 0, `→` lands on | Why |
|---|---|---|
| `foo.bar()` | `.` → `bar` → `(` | punctuation runs are their own words (WB999) |
| `a  b` | `b` | whitespace run skipped by `next` |
| `\n\n` | the blank line | blank lines are stops |
| `漢字テスト` | `字` then `テスト` | Han per-char (WB999); Katakana runs join (WB13) |
| `안녕하세요` | per Hangul syllable | Hangul is `ALetter`; the run joins → one word |
| `👨‍👩‍👧‍👦x` | `x` | ZWJ sequence is one grapheme *and* one word (WB3c) |
| `🇦🇺🇦🇺` | second flag | RI pairs (WB15/16) |
| `don't` | end of `don't` | WB6/7 keep the apostrophe inside |
| `well-known` | `-` then `known` | hyphen is a break in UAX #29 |
| `1,000.50` | end | WB8–11 keep the number whole |
| `0xFF` byte + `A` | the invalid byte, then `A` | invalid bytes are their own cluster (s02) and their own word |

**Pitfall — CJK has no spaces.** A word engine written against ASCII
whitespace turns a 200-character Japanese line into one word and W mode
becomes L mode. WB999's break-by-default is what saves it, and the two CJK
rows above are non-negotiable test cases.

**Pitfall — properties, not codepoint ranges.** Hard-coded ranges
(`0x4E00..0x9FFF`) rot with every Unicode release and get emoji wrong.
Everything goes through the generated Word_Break table plus the s02
Extended_Pictographic set.

### 4. Block engine — providers and the containment chain

`src/edit/block.c`.

```c
typedef struct BlockProvider {
    const char *name;
    int         priority;        /* higher wins ties at equal span size  */
    /* Smallest span of this provider's kind that CONTAINS p and is
     * strictly larger than `inner` (pass an empty span for level 0).
     * Returns false when this provider has nothing to say — the chain
     * simply skips it (this is how the S40 syntax slot no-ops today).  */
    bool (*enclosing)(void *ctx, UnitCtx *u, ByteOff p, Span inner,
                      Span *out);
    void *ctx;
} BlockProvider;

void yew_block_register(BlockProvider p);      /* insertion-ordered      */
bool yew_block_level(UnitCtx *u, ByteOff p, u32 level, Span *out);
```

Registered order (all four always registered; the first falls through when
syntax is unavailable):

| # | Provider | Priority | Status |
|---|---|---|---|
| 1 | `syntax` | 40 | installed by Sprint 40; exact syntax spans when available, otherwise the chain falls through |
| 2 | `scope` (bracket/brace/paren) | 30 | this sprint |
| 3 | `indent` | 20 | this sprint |
| 4 | `paragraph` | 10 | this sprint |

**The containment chain** is the whole design: `yew_block_level(p, n)`
asks every provider for the smallest span containing `p` that strictly
contains level `n−1`'s span, and takes the smallest answer (ties → higher
priority). Level 0 uses an empty span at `p`. The chain terminates at the
whole buffer. Consequences worth stating:

- A `.txt` with no brackets gets `paragraph → (indent block if the text is
  indented) → buffer` — three useful levels with zero language support.
- A C file gets `inner braces → enclosing braces → function → file`
  naturally, because each brace pair strictly contains the previous.
- Sprint 40's syntax provider slotted in *between* without changing any
  caller: it returns spans that are smaller/better than the heuristics' and
  wins the "smallest containing" comparison when syntax is available.

Arrow behavior in B mode (`←` `→` `↑` `↓` per §1's map):

- `home`/`end` = the level-0 span's edges.
- `next`/`prev` = the **next / previous sibling** unit: the level-0 span
  of the position just after `end` / just before `home`, skipping
  whitespace-only gaps. At the last sibling, `next` moves to the parent's
  end (so repeated `↓` walks out of a scope rather than stalling —
  contract rule 2).
- `A-↑` `ed.sel.unit.expand` / `A-↓` `ed.sel.unit.contract` — §6.
- `A-←` `A-→` `ed.move.block.match_prev/next` — jump to the matching
  delimiter of the enclosing pair (the classic `%`, arrow-shaped).

### 5. The three heuristic providers

**Paragraph.** A paragraph is a maximal run of non-blank lines; a maximal
run of blank lines (whitespace-only, per s02 White_Space) is also a unit,
so `↑`/`↓` alternate text/gap honestly. Level 1 for prose: the run of
paragraphs between two *multi-blank* separators (a "section"), when one
exists.

**Indentation block.** Indent width = expanded columns with tabs at
`tabwidth` (computed in `src/unicode/`, never here). The block containing
line `l` is the maximal contiguous run of lines whose indent ≥ indent(`l`),
extended through **blank lines that are followed by a line still in the
block**.

> Pitfall (the Python bug every editor ships once): terminating an indent
> block at the first blank line splits every function with a blank line in
> its body. Absorb interior blanks; never absorb trailing blanks — a block
> must not end in whitespace or `H`+delete eats the separator.

Level up = the block at the enclosing (strictly smaller) indent, which is
the header line (`def f():`, `if x:`) plus its body — so expand from a
statement gives the whole suite.

**Bracket/brace scope.** A bespoke outward scan with an explicit stack
over `() [] {}`, using the s07 chunk iterator, with line-level string and
comment suppression:

```c
/* Line-local quote heuristic.  Scan the line counting UNESCAPED ' and ".
 * A quote character only opens a string if its kind's count on that line
 * is EVEN — so `don't` (one apostrophe) is an apostrophe, while
 * `x = 'a[b'` (two) suppresses the bracket inside.  This is wrong for
 * multi-line strings and right for the 99% of lines that are not, which
 * is the correct trade for a heuristic that must also work on prose.  */
static bool line_in_string(UnitCtx *u, LineNo l, ByteOff p);
```

- Comments: `//`, `#`, `--`, `;` to end of line, and `/* … */` spanning
  lines, suppress bracket counting. A `commentstring`/`quotes` option
  was refined per-buffer in Sprint 36; Sprint 40's syntax provider makes
  it exact when syntax is available.
- Scan budget: the outward scan stops after `YEW_BLOCK_SCAN_LINES = 2000`
  lines in each direction and reports "no enclosing scope", falling
  through to indent/paragraph. **Pitfall:** an unbalanced brace in a
  100 MB file must not turn one arrow press into a full-file scan — the
  budget is what keeps B mode inside the 5 ms gate (invariant 4).
- Delimiter pairs are configurable data (`static const struct {u8 open,
  close;}`), so adding `<>` for a future language is a table row.

### 6. Grow / shrink the structural selection

```c
typedef struct SelStack { Span s[YEW_SEL_DEPTH]; u8 n; } SelStack; /* 16 */
```

- `ed.sel.unit.expand`: if the stack is empty, push level 0 (the unit
  under the cursor) and set `anchor`/`pos` to its edges; otherwise push the
  next level from the chain. At the buffer level, further expands are a
  no-op with a message.
- `ed.sel.unit.contract`: pop. **Pitfall:** recomputing "the inner unit"
  from the current span is ambiguous (which child?), so contraction *must*
  replay the stack. The stack is invalidated (cleared) by any edit and by
  any cursor motion that is not expand/contract — a stale stack is how
  users get "shrink went somewhere I've never been".
- The stack initially lived on `Win`, with a single-cursor guard at this
  sprint's landing. Sprint 17 moved it into the cursor set so every cursor
  has a synchronized stack and expansion works across the normalized set.
- Both commands work in **any** mode (they are structural, not modal) but
  are bound only in B's layer by default; H mode binds them in Sprint 17.

### 7. Shared conformance suite — `tests/unit/test_units.c`

One table-driven harness run against **every** registered `UnitOps`, so a
new engine cannot skip the contract:

```c
static const UnitOps *const engines[] = { &yew_unit_line, &yew_unit_word,
                                          &yew_unit_block, &yew_unit_char };
```

Per engine, per fixture, per `alt ∈ {false,true}`: monotonicity of
`next`/`prev`, termination within `len` steps, grapheme-boundary results,
`home <= p <= end`, `span == {home,end}`, round-trip `prev(next(p)) <= p`,
and purity (buffer generation counter unchanged, s07 `tb->gen`).

### 8. Delivery history and remaining deferments

- Syntax-aware structural units were deferred to Sprint 40 and are now
  installed through this provider slot, preserving heuristic fallback.
- H mode, selection rendering, and multi-cursor structural selection were
  deferred to Sprint 17 and are now live through these unit-engine paths.
- `ed.move.word.same_prev/next` (jump to the next occurrence of the word
  under the cursor) → Sprint 21 with the search engine; W's `A-↑`/`A-↓`
  stay unbound rather than shipping a naive scan that a real engine will
  replace.
- Language-specific bracket sets, `commentstring`, `tabwidth`,
  `subword` on/off → Sprint 36 (options).
- Fold/outline units → post-1.0.

## Testing Strategy

- **Generator** (`scripts/gen-unicode-tables`): Word_Break emission,
  regenerated-output-is-identical CI check (same discipline as s02's
  tables).
- **UAX #29 conformance** (`tests/unit/test_wordbreak.c`): the UCD
  `WordBreakTest.txt` corpus checked in under
  `tests/unit/fixtures/unicode/`; every non-skipped line asserted.
  Documented skips (if any) live in the fixture with a reason comment.
- **Unit** (`tests/unit/test_word.c`): the §3 boundary-case table, the
  subword table, and WORD variant; motion round-trips across line ends and
  blank lines; behavior on a buffer of pure invalid bytes.
- **Unit** (`tests/unit/test_block.c`): each provider in isolation
  (paragraph on prose, indent on a Python-shaped fixture with interior
  blanks, scope on a C fixture); the containment chain on a C file
  (4 levels hand-computed) and on a plain `.txt` (3 levels); the quote
  heuristic on `don't`, `x = 'a[b'`, and a line with an unbalanced quote;
  the 2000-line scan budget on an unbalanced 100 kLOC fixture (asserts the
  fall-through, and that the call stays under the frame budget).
- **Unit** (`tests/unit/test_syn_block.c`): Sprint 40's installed provider
  makes strict containment progress and falls back safely when syntax is
  unavailable.
- **Unit** (`tests/unit/test_units.c`): the §7 conformance suite — 4
  engines × 6 fixtures × 2 alt values.
- **Fuzz** (`tests/fuzz/fuzz_units.c`): random buffers (including invalid
  UTF-8, lone CR, 1-byte lines, 1 MB single line) × random positions ×
  random engine/op sequences, 100k ops: never a non-boundary result, never
  a fixed point, never an out-of-range offset, always terminates.
- **Pty goldens**: W mode across a mixed ASCII/CJK/emoji line with the
  cursor position shown in the statusline at each stop; B mode expand ×4
  on a C file and ×3 on prose (cursor/anchor positions asserted via the
  statusline, since selections do not render until Sprint 17).
- **Perf**: 10⁵ `next` calls per engine on a 10 kLOC buffer within the
  keystroke budget; B-mode expand on a deeply nested (32-level) fixture.

## Definition of Done

1. `WordBreakTest.txt` conformance: 100 % of the checked-in corpus passes;
   any skip is listed in the fixture with a written reason.
2. Every row of the §3 boundary table and the subword table has an
   assertion, including both CJK rows, both emoji rows, and the
   invalid-byte row.
3. The §7 conformance suite passes for all four engines × both `alt`
   values; adding a stub engine that violates rule 2 fails the suite
   (proved once by a deliberately broken engine in the test file).
4. B mode on a plain `.txt` with zero brackets yields ≥ 3 distinct
   containment levels (paragraph → section-or-indent → buffer), asserted
   on a prose fixture — the "useful without a language" gate.
5. The containment chain on a C fixture returns 4 hand-computed levels;
   Sprint 40's installed syntax provider makes strict containment progress
   when active and falls through inside `yew_block_level` when unavailable.
6. Indent blocks absorb interior blank lines and never end on a blank
   line (Python-shaped fixture, both asserted).
7. Quote/comment heuristics: `don't` does not suppress brackets;
   `x = 'a[b'` does; a `//` comment does; each in a unit test.
8. Scan budget honored: unbalanced 100 kLOC fixture falls through to
   indent/paragraph in < 1 ms and never scans the whole file.
9. Fuzz: 100k ops × 4 seeds, zero contract violations, ASan/UBSan clean.
10. `grep -rn 'wcwidth\|0x4E00\|0x1F3' src/edit/` → empty: no hard-coded
    codepoint ranges and no width math outside `src/unicode/`.
11. The Sprint 40 syntax provider installs through the original ordered slot;
    Sprint 17 multi-cursor expand keeps one selection stack per cursor and
    preserves cursor-set normalization.
12. ≥ 160 assertions across the four new unit files; perf checks inside
    the keystroke budget; determinism lane green on the new goldens.
