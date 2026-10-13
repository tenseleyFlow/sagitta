# Sprint 39: The Syntax Engine — Incremental Line State Machine

## Prerequisites

- Sprint 5 — `Cell` (20 bytes, `_Static_assert`), `YewColor`, the `attrs`
  bitmask (bits 0–9 used, 10–15 free), `yew_grid_put`, damage spans, the
  truecolor→256→16 ladder. This sprint produces *attr ids*; s05's cell
  fields are what a theme eventually fills (Sprint 41).
- Sprint 7 — `TextBuf`, `tb->gen`, `yew_textbuf_line_count`,
  `yew_textbuf_line_span`, `yew_lineiter_*`, and snapshots (s07 §8 names
  this sprint as a future consumer of stable views).
- Sprint 9 — grapheme boundaries; **width math stays in `src/unicode/`**.
- Sprint 15 — `Viewport`, `yew_layout`/`yew_render`, the frame order, the
  16 ms idle timer on Sprint 14's timer heap, the statusline segment table
  (this sprint adds one segment), `full_damage`.
- Sprint 17 — the pinned attribute merge order: **syntax → selection bg →
  search-match → cursor**. Syntax owns foreground and style bits only.
- Sprint 20 — `yew_re_compile`, `yew_re_match_at`, `YewReInput`, `ReLit`
  (the literal prefilter, reused here as a first-byte oracle), the
  no-backtracking law. s20 §9 names Sprints 39/40 as the consumers of
  definition patterns.
- Sprint 21 — the budgeted-overlay precedent: 1000 µs of scan per
  keystroke, resume on the idle timer, damage only the lines whose
  decoration changed. This sprint copies that shape deliberately.
- Assumptions beyond earlier index entries (state them if they land
  differently): (a) `Buffer` (s14) has a per-buffer extension slot —
  `01-architecture.md` already lists *"Buffer: piece tree + line index +
  undo tree + marks + syn state"*, so `SynBuf syn;` goes there; (b) the
  s14 `ed_apply` edit choke point publishes a line-damage record
  `{LineNo lo; u64 removed, inserted;}` per edit. If it does not, derive it
  here from `yew_textbuf_line_of` on the pre-edit and post-edit offsets —
  the splice in §5 needs the triple, not a diff; (c) `yew_win_damage_lines`
  exists (s21 made the same assumption).
- Binding: `00-decisions.md` Highlighting row (*"bespoke incremental
  line-state-machine engine (built-in, not a module)"*), quality bars,
  invariants 3, 4, 5, 7, 8.

## Goals

Land `src/syn/engine.c|h` and `src/syn/attr.h`: the per-line state machine
that every language definition drives, the semantic attribute vocabulary
themes map onto, the per-buffer line-state array and its splice against the
piece tree's line index, and the budgeted damage-propagation loop that
reaches a fixpoint without ever stealing a frame. **The locked decision is
a line state machine, not tree-sitter, and the trade is real: we give up
perfect nesting fidelity — no parse tree, no error recovery over
ill-formed input, no "this identifier is a type because of its
declaration" — and we buy a highlighter that adds tens of kilobytes rather
than a megabyte of parser tables and a C++ runtime to a ≤ 1.5 MiB binary,
that restyles an edited line in microseconds instead of re-parsing a
subtree, and that adds zero dependencies (invariant 7).** For an editor
whose keypress→paint p99 is a CI gate, a regular-language approximation
that is always fast beats a context-free parse that is usually fast. The
definition *format* and its compiler are Sprint 40 — this sprint consumes
packed tables and ships one hand-written toy definition in C so the engine
is testable on its own. Themes are Sprint 41 (this sprint resolves attrs
through a two-entry debug palette). The B-mode syntax provider is Sprint
40. Embedded languages (markdown fences) are out of 1.0's core: the state
carries a reserved `def` byte that must be 0, and any cross-definition
extension lands in its own sprint.

## Deliverables

### 1. Semantic attributes — `src/syn/attr.h`

**The law: definitions emit meanings, never colors.** A rule says `string`;
a theme decides `#98c379`. Nothing in `src/syn/engine.c` or in any `.fl`
definition may name a color, and DoD 3 greps for it.

```c
typedef enum SynAttr {           /* u8; YEW_ATTR__COUNT fits in 6 bits   */
    YEW_ATTR_TEXT = 0, …
} SynAttr;
_Static_assert(YEW_ATTR__COUNT <= 64, "attr id must fit 6 bits");
```

| id | Name | Falls back to | Meaning |
|---|---|---|---|
| 0 | `text` | — | ordinary code; the theme's default foreground |
| 1 | `keyword` | `text` | language keyword |
| 2 | `keyword.control` | `keyword` | `if while return goto match` |
| 3 | `keyword.op` | `keyword` | word-shaped operators: `and`, `.eq.`, `in` |
| 4 | `keyword.storage` | `keyword` | `static const pub mut extern` |
| 5 | `type` | `text` | type name |
| 6 | `type.builtin` | `type` | `int`, `u32`, `str`, `REAL` |
| 7 | `constant` | `text` | named constant / enum member |
| 8 | `constant.builtin` | `constant` | `NULL nil None true` (non-boolean) |
| 9 | `number` | `constant` | numeric literal |
| 10 | `boolean` | `constant` | `true`/`false`/`.TRUE.` |
| 11 | `character` | `string` | char/rune literal |
| 12 | `string` | `text` | string literal body and quotes |
| 13 | `string.escape` | `string` | `\n`, `\x41`, `\u{…}` |
| 14 | `string.interp` | `string` | the `${…}` / `{…}` delimiters |
| 15 | `string.special` | `string` | regex literals, raw strings, heredoc bodies |
| 16 | `comment` | `text` | comment |
| 17 | `comment.doc` | `comment` | `///`, `/**`, `#|` doc comment |
| 18 | `comment.todo` | `comment` | `TODO FIXME XXX HACK` inside a comment |
| 19 | `function` | `text` | function name at definition or call |
| 20 | `function.builtin` | `function` | `printf`, `len`, `SIZE` |
| 21 | `function.macro` | `function` | macro invocation (`println!`) |
| 22 | `method` | `function` | `.name(` |
| 23 | `variable` | `text` | ordinary identifier (usually unstyled) |
| 24 | `variable.builtin` | `variable` | `self this $@ __FILE__` |
| 25 | `variable.param` | `variable` | parameter / lifetime |
| 26 | `variable.member` | `variable` | field, mapping key |
| 27 | `namespace` | `text` | module/package/section name |
| 28 | `label` | `text` | goto label, make target, statement label |
| 29 | `attribute` | `text` | `#[derive]`, `@decorator`, `.PHONY` |
| 30 | `preproc` | `text` | `#include`, `#ifdef`, `%YAML` |
| 31 | `operator` | `text` | symbolic operators |
| 32 | `punct` | `text` | punctuation not otherwise classed |
| 33 | `punct.bracket` | `punct` | `() [] {}` |
| 34 | `punct.delim` | `punct` | `, ; :` |
| 35 | `tag` | `text` | markup element name |
| 36 | `tag.attr` | `tag` | markup attribute name |
| 37 | `heading` | `text` | markdown heading |
| 38 | `link` | `text` | URL / link target |
| 39 | `emphasis` | `text` | italic run |
| 40 | `strong` | `text` | bold run |
| 41 | `code` | `text` | inline code / fenced block body |
| 42 | `list` | `text` | list bullet or number |
| 43 | `quote` | `text` | block quote |
| 44 | `diff.add` | `text` | added line |
| 45 | `diff.del` | `text` | removed line |
| 46 | `error` | `text` | syntactically impossible text (see below) |
| 47 | `warning` | `text` | legal but suspicious (tabs in a Python indent) |
| 48 | `whitespace.special` | `text` | semantic whitespace (make's recipe TAB) |
| 49 | `motion.unit` | `keyword` | Fletch terse layer: `l w b c` |
| 50 | `motion.arrow` | `operator` | Fletch terse layer: `< > ^ v`, alt forms |
| 51 | `motion.count` | `number` | Fletch terse layer: count prefix |
| 52 | `motion.cmd` | `function` | Fletch terse layer: CMDWORD |
| 53 | `ui.invisible` | `text` | rendered whitespace glyphs (reserved, s41) |

`YEW_ATTR__COUNT == 54`. The set is **closed for 1.0**: adding a row means
touching this table, both shipped themes, and the theme-coverage test, and
that friction is the point — an open-ended attr namespace produces
definitions that style things no theme colors.

Rows 49–52 are the one deliberate language-specific concession: Fletch's
terse motion layer is a sublanguage with no analogue in the generic set,
and it is the language every yew user reads their own config in
(invariant 10's round-trip output is *source*). Justified, not an
accident.

- **Fallback resolution happens once, at theme load** (Sprint 41): the
  theme table is a flat 64-entry array, and an attr the theme does not
  mention inherits its parent's resolved entry, transitively, terminating
  at `text`. The draw path does exactly one array index — never a chain
  walk per cell.
- `error` is for text the definition can prove is impossible (a `\q`
  escape, a trailing comma in strict JSON), never for "the highlighter got
  confused". A definition that paints half a file `error` is a definition
  bug; Sprint 40's validator warns on `error` used as a context default.

### 2. State representation — `src/syn/engine.h`

Per line the engine is a pure function:

```
(line bytes, entry state)  →  (attr spans, exit state)
```

and a line must be rehighlighted **iff its text changed or its entry state
changed**. That biconditional is the whole design; everything below exists
to make both sides cheap to test.

```c
#define YEW_SYN_DEPTH_MAX   16       /* context stack depth cap          */
#define YEW_SYN_LOST_MAX    255      /* saturating over-depth counter    */

typedef struct SynState {
    u16 ctx[YEW_SYN_DEPTH_MAX];  /* context ids; ctx[0] is the def root  */
    u32 aux;                     /* per-frame scalar: interned string id
                                    (heredoc delimiter, raw-string tail,
                                    fence text) or a small integer       */
    u8  depth;                   /* 1 … YEW_SYN_DEPTH_MAX                */
    u8  lost;                    /* pushes refused at the cap (§4)       */
    u8  def;                     /* RESERVED, must be 0 — cross-definition
                                    states are not in 1.0's core         */
    u8  flags;                   /* YEW_SYN_F_VALUE | _STRIP | _DEGRADED */
} SynState;
_Static_assert(sizeof(SynState) == 40, "state tuple size");
```

**Storage is a u32, not this struct.** Every distinct `SynState` is
interned into a per-engine table and referred to by its id:

```c
typedef struct SynStateTab SynStateTab;      /* open-addressed, arena     */
u32              yew_syn_state_intern(SynStateTab *t, const SynState *s);
const SynState  *yew_syn_state_get(const SynStateTab *t, u32 id);
#define YEW_SYN_STATE_UNKNOWN 0u             /* reserved: "no retained value" */
#define YEW_SYN_STATE_ROOT    1u             /* reserved: depth 1, ctx[0]=root */
```

Why intern:

1. Per-line storage is **4 bytes** — a 1 M-line file costs 4 MiB of state,
   inside the 1.6× memory gate; a 40-byte struct per line would cost 40.
2. "Did the entry state change?" is one `u32` compare. The fixpoint test in
   §6 runs once per line and must not be a 40-byte `memcmp`.
3. Interning makes state identity canonical, which is what makes the
   differential oracle in Testing meaningful: two ways of reaching the same
   context stack produce the same id or the engine is wrong.

Table size: ids are u32; a table over `YEW_SYN_MAX_STATES = 65536` entries
is enforced, and exhaustion (only reachable from a pathological definition)
logs once and reuses `YEW_SYN_STATE_ROOT`, degrading the buffer rather than
failing. Real definitions produce tens to low hundreds of states.

**Depth cap = 16, and here is the justification.** The deepest *acyclic*
nesting any shipped definition needs is 6 (markdown → fenced block →
template literal → interpolation → nested string → escape). Sprint 40's
validator computes each definition's acyclic push depth statically and
**rejects any definition exceeding 12**, so the remaining 4 levels exist
purely for genuinely self-recursive contexts — Rust's nested block
comments and brace-balanced interpolation, which are the only constructs
that can recurse at runtime. 16 is therefore 2.5× the deepest real
construct with a compile-time proof that the static part cannot eat the
budget, and it keeps `SynState` at 40 bytes, which keeps the interner's
hash a single 40-byte pass.

**What happens at the cap — degrade honestly, never corrupt.**

| Event | Behavior |
|---|---|
| push at `depth == YEW_SYN_DEPTH_MAX` | the push is **refused**; `lost++` (saturating at 255); the rule's attr is still emitted; the line continues in the current context |
| pop with `lost > 0` | `lost--`; the stack is **not** popped |
| pop with `lost == 0 && depth == 1` | ignored; the root context is never popped |
| `lost` saturates at 255 | `flags |= YEW_SYN_F_DEGRADED`, one `yew_log(WARN)` per buffer, statusline shows `syn!` |

> **Pitfall — the asymmetric cap is the corruption.** If pushes are refused
> but pops are honored, the first over-deep construct desynchronizes the
> stack and every line after it is styled in the wrong context *forever*,
> with no edit able to fix it. The `lost` counter is what keeps push/pop
> balanced: it makes over-deep nesting style as its enclosing context —
> visually wrong in a way that is local, bounded, and self-healing when the
> nesting shrinks. `lost` is part of the interned tuple, so two states that
> differ only in `lost` are different states and the fixpoint test cannot
> conflate them. A unit test drives 40 nested pushes and 40 pops and
> asserts the exit state equals the entry state exactly.

### 3. Span output and the per-line entry point

```c
typedef struct SynSpan {         /* offsets are RELATIVE to line start   */
    u32 start;
    u16 len;                     /* runs > 65535 B split into several    */
    u8  attr;                    /* SynAttr                              */
    u8  flags;                   /* YEW_SPAN_TRUNCATED                   */
} SynSpan;
_Static_assert(sizeof(SynSpan) == 8, "span size");

typedef struct SynLineOut {
    SynSpan *spans; u32 n, cap;  /* caller-owned; engine never allocates */
    u32      exit_state;
    u8       stop;               /* YEW_SYN_STOP_{OK,SPANS,BYTES,STEPS}  */
} SynLineOut;

/* Pure: depends only on (def, entry_state, line, len). No buffer access,
 * no globals, no clock. This purity is what makes the incremental proof
 * checkable and the s41/s42 goldens plain (state, line) → dump pairs.  */
void yew_syn_line(SynEngine *e, u32 entry_state,
                  const u8 *line, u32 len, SynLineOut *out);
```

Bounds, all pinned, all producing an honest degradation rather than a
stall:

| Bound | Value | On exceed |
|---|---|---|
| `YEW_SYN_MAX_SPANS` | 4096 per line | stop emitting; remainder is `text`; `stop = SPANS` |
| `YEW_SYN_LINE_BYTE_CAP` | 64 KiB | stop matching; remainder is `text`; **exit state = entry state**; `stop = BYTES`; buffer flagged degraded |
| `YEW_SYN_LINE_STEPS` | `4096 + 8·len` rule attempts | as `BYTES` |

> **Pitfall — a single line cannot be split across frames.** The frame
> budget in §6 is line-granular, so one 512 KiB minified line would blow it
> outright. `YEW_SYN_LINE_BYTE_CAP` is the answer: past 64 KiB the line is
> inert. The state carried to the next line is deliberately the *entry*
> state (not a half-computed one) so the machine stays balanced; the cost
> is that a string opened past column 65536 is missed. The viewport can
> display ~20 KiB of one line at any terminal size, so nothing on screen is
> ever affected.

> **Pitfall — zero-width matches.** A rule whose pattern can match empty
> (`a*`, `^`, a lookaround-free assertion-only pattern) matches at every
> position and never advances. The engine **forces progress**: after
> applying a rule, if the new scan position equals the old one, it advances
> to the next UTF-8 boundary and emits the context default attr for the
> skipped bytes. One exception, tightly bounded: a rule flagged
> `zero-width pop` (Sprint 40's `indent_lt` matcher, used by YAML) may pop
> without consuming, at scan position 0 only, at most `depth` times per
> line — tracked by a counter the engine resets per line. Without both
> guards the editor hangs on a definition the user installed.

### 4. Compiled tables — the interface Sprint 40 targets

The engine consumes flat, pointer-free tables. Sprint 40 owns building
them; this sprint pins their shape and hand-builds one in C for tests.

```c
typedef enum { SYN_OP_STAY = 0, SYN_OP_PUSH, SYN_OP_POP, SYN_OP_SET } SynOp;
typedef enum { SYN_AUXM_NONE = 0, SYN_AUXM_LINE_EQ, SYN_AUXM_LITERAL,
               SYN_AUXM_FENCE_CLOSE, SYN_AUXM_INDENT_LT } SynAuxMatch;

typedef struct SynRule {
    const YewRe *re;        /* NULL when aux_match != NONE               */
    u8   attr;              /* attr for bytes not covered by `caps`      */
    u8   op;                /* SynOp                                     */
    u8   nop;               /* SYN_OP_POP: how many levels (1..4)        */
    u8   aux_match;         /* SynAuxMatch                               */
    u16  target;            /* PUSH/SET: context id                      */
    u8   consume;           /* advance to end of group N (0 = whole)     */
    u8   flags;             /* SET_AUX | SET_VALUE | CLR_VALUE | STRIP   */
    u8   caps[8];           /* caps[g] = attr for capture group g, 0xff = none */
    u8   aux_group;         /* group whose text becomes state.aux        */
    u8   first[32];         /* 256-bit set: bytes this rule can start on */
} SynRule;

typedef struct SynCtx {
    u32 first_rule, nrules;
    u8  dflt_attr;          /* bytes no rule matched                     */
    u8  at_eol;             /* SynOp applied at end of line              */
    u8  eol_nop;
    u8  flags;              /* CTX_UNIT_SPAN | CTX_UNIT_ATOM (s40 §5)    */
    u8  first[32];          /* union of the context's rules' first sets  */
} SynCtx;

typedef struct SynDef {
    const char *name; u16 root, nctxs; u32 nrules;
    SynCtx *ctxs; SynRule *rules; Interner *aux;   /* aux string ids     */
} SynDef;
```

`first[32]` is the load-bearing performance structure: a 256-bit set of
bytes on which any rule in the context can begin, built by Sprint 40 from
each pattern's s20 `ReLit`/first-byte analysis. Bytes outside the set are
consumed by a tight scan loop that never enters the regex engine.

```c
/* The matching loop.  FIRST match in definition order wins — not longest.
 * Why: authors reason top-to-bottom, and longest-match makes rule order
 * silently irrelevant, so "keyword before identifier" becomes
 * inexpressible.  Same reasoning as s20's leftmost-FIRST alternation.  */
static u32 scan_line(SynEngine *e, SynState *st, const u8 *s, u32 len,
                     SynLineOut *out)
{
    u32 p = 0;
    while (p < len) {
        const SynCtx *c = &e->def->ctxs[st->ctx[st->depth - 1]];
        if (!bitset_has(c->first, s[p])) {          /* fast path: no rule
                                                     * can start here    */
            u32 q = p;
            do { q++; } while (q < len && !bitset_has(c->first, s[q]));
            emit(out, p, q - p, c->dflt_attr);       /* one coalesced span */
            p = q;  continue;
        }
        const SynRule *r = rule_match_at(e, c, s, len, p, st);
        if (!r) { emit_default_advance(out, s, len, &p, c->dflt_attr);
                  continue; }
        u32 end = apply(e, st, r, s, len, p, out);   /* attrs + state op  */
        p = (end > p) ? end : next_boundary(s, len, p);  /* progress law  */
    }
    return p;
}
```

### 5. Per-buffer state array and the splice — `src/syn/engine.c`

```c
typedef struct SynBuf {
    u32     lang;          /* YEW_LANG_NONE = highlighting off           */
    Vec_u32 entry;         /* entry[i] = state ENTERING line i           */
    LineNo  settled_to;    /* entry[0..settled_to) is PROVEN correct     */
    LineNo  wave;          /* next line the propagation wave will run    */
    u64     buf_gen;       /* s07 tb->gen the array was spliced against  */
    bool    settling, degraded;
    u32     spec_from;     /* speculative viewport styling origin (§6)   */
} SynBuf;

void yew_syn_attach(SynBuf *s, u32 lang, const TextBuf *tb);
void yew_syn_edit  (SynBuf *s, LineNo lo, u64 removed, u64 inserted);
```

`entry[0]` is always `YEW_SYN_STATE_ROOT` and is never spliced.
`entry.len == line_count`. An edit that replaces `removed` lines starting
at `lo` with `inserted` lines splices **at index `lo + 1`**:

| Quantity | Rule |
|---|---|
| splice point | `lo + 1` — the state *entering* line `lo` cannot change |
| removed slots | `removed` |
| inserted slots | `inserted` copies of `YEW_SYN_STATE_UNKNOWN` |
| `settled_to` | `min(settled_to, lo + 1)` |
| `wave` | `min(wave, lo)` — line `lo` itself must be re-run (its text changed) |
| tail entries | **retained, not poisoned** — see §6 |

> **Pitfall — the off-by-one.** Splicing at `lo` instead of `lo + 1` throws
> away the one entry that is guaranteed still valid and forces a re-run
> from the previous line on every keystroke; splicing at `lo + 2` keeps a
> stale entry that the fixpoint test will then wrongly accept. A unit test
> edits line 0, the last line, and an empty buffer, and asserts
> `entry.len == yew_textbuf_line_count(tb)` after each.

> **Pitfall — never rebuild the array.** Pasting 10 000 lines must be one
> `memmove` plus one fill, not 10 000 element inserts. `Vec` grows
> geometrically; the splice is `vec_splice(&s->entry, lo+1, removed,
> inserted, YEW_SYN_STATE_UNKNOWN)` and it is the only mutation site.

`YEW_SYN_STATE_UNKNOWN` means exactly "no retained value here, the fixpoint
test cannot be applied at this index" — it is not a poison that invalidates
the tail.

### 6. Damage propagation to a fixpoint, on a budget

The naive rule ("rehighlight until the exit state stops changing") is
correct and, on the pathological case, catastrophic: typing `/*` on line 1
of a 1 MB C file changes the entry state of **every** following line, so a
single keystroke would restyle a million lines. Bounding that work is this
sprint's load-bearing decision.

```c
typedef struct SynSettleReport { u64 lines, us; bool fixpoint, hit_view; }
    SynSettleReport;

void yew_syn_settle(SynBuf *s, const TextBuf *tb, LineNo view_lo,
                    LineNo view_hi, i64 budget_us, SynSettleReport *rep);
```

| Constant | Value | Why |
|---|---|---|
| `YEW_SYN_FRAME_BUDGET_US` | 1000 | matches s21's overlay budget; 20 % of the 5 ms keypress gate |
| `YEW_SYN_IDLE_BUDGET_US` | 4000 | on Sprint 15's 16 ms idle tick — 25 % duty cycle |
| `YEW_SYN_CLOCK_EVERY` | 256 lines | `clock_gettime` per line costs more than a line |
| `YEW_SYN_SETTLING_MS` | 250 | delay before the statusline says anything |

The algorithm, in order:

1. **Viewport first.** Lines in `[view_lo, view_hi)` whose *text* changed
   this edit are restyled immediately and unconditionally (bounded by the
   viewport height, ~200 lines) and damaged. What the user is looking at is
   never provisional because of a budget.
2. **The wave.** From `wave` forward, spend budget:

   ```c
   for (LineNo i = s->wave; i < nlines; i++) {
       Span ls = yew_textbuf_line_span(tb, LINENO(i));
       yew_syn_line(e, s->entry.data[i], line_bytes, line_len, &out);
       u32 next = out.exit_state;
       if (i + 1 >= nlines) { s->settled_to = nlines; FIXPOINT; }
       u32 held = s->entry.data[i + 1];
       if (held != YEW_SYN_STATE_UNKNOWN && held == next) {
           s->settled_to = nlines;      /* the entire tail is unchanged  */
           FIXPOINT;                    /* <- the common case: 1 line    */
       }
       s->entry.data[i + 1] = next;
       if (i + 1 >= view_lo && i + 1 < view_hi)
           yew_win_damage_lines(w, i + 1, i + 2);
       s->settled_to = i + 2;
       if ((i & (YEW_SYN_CLOCK_EVERY - 1)) == 0 && elapsed() > budget_us) {
           s->wave = i + 1; s->settling = true; ARM_IDLE_TIMER; return;
       }
   }
   ```

   **Retaining the tail is what makes the fixpoint cheap.** Because
   `entry[i+1]` still holds its previous value, the wave stops the instant
   the recomputed exit state matches it — which for an ordinary edit
   (typing inside a line, adding a statement) is after exactly one line.
   Poisoning the tail on every edit would turn every keystroke into a
   full-file walk and defeat the entire design.

3. **Speculative viewport styling.** If the wave has not yet reached
   `view_lo` when the budget runs out, the visible lines are styled from
   the *retained* `entry[view_lo]` (stale, but correct in the
   overwhelming majority of edits, because an edit above rarely changes
   context) or from `YEW_SYN_STATE_ROOT` if the entry is `UNKNOWN`. Those
   lines are marked provisional via `spec_from`. When the wave later
   reaches `view_lo` and computes a different state, the viewport is
   restyled and damaged once. The user sees correct colors immediately and,
   in the rare disagreement, a single-frame correction.
4. **Settling indicator.** While `settling` and the elapsed time since the
   edit exceeds `YEW_SYN_SETTLING_MS`, the statusline shows a `syn…`
   segment (Sprint 15's segment table, drop priority 4). Under 250 ms
   nothing is shown — the indicator must not flicker on every keystroke.
   `ed.syn.status` prints `settled 1234/98765 lines, wave at 1234,
   degraded=no` for scripts and bug reports. `syn!` (not `syn…`) marks a
   degraded buffer (§2/§3 caps hit) and does not clear on its own.

Worst case, stated honestly: on a 1 M-line file where line 1 opens a
comment, every frame stays under budget, the viewport is correct within one
frame, and the tail reaches its fixpoint after roughly
`1e6 lines × 3 µs / 4 ms per idle tick ≈ 750` idle ticks ≈ 12 s of idle
time. Nothing blocks, nothing is wrong on screen, and the indicator says so.

### 7. Render integration — `src/ui/draw.c`

```c
/* The draw path's ONLY entry point.  Never allocates; `out` is the window's
 * reusable buffer.                                                       */
void yew_syn_spans(SynBuf *s, const TextBuf *tb, LineNo l, SynLineOut *out);
```

- **Span cache**: a ring of `YEW_SYN_SPAN_CACHE = 256` entries keyed by
  `(line, buf_gen, entry_state)`. A redraw of an unchanged viewport is
  cache hits only; a scroll costs the newly exposed rows. Without it, a
  120 fps scroll gate re-runs the machine over 200 lines every frame.
- Attr → cell style: `theme->tab[attr]` (Sprint 41's flat 64-entry table),
  giving `fg` and the `attrs` bits. Background is left alone so the s17
  merge order holds: **syntax → selection bg → search-match → cursor**.
- **Cluster rule, pinned:** spans are byte ranges; the draw pass segments
  graphemes (s02) and gives each cluster the attr of its **first byte**. A
  span boundary landing inside a cluster is resolved that way and never
  splits it, which is what keeps s05's rule that a wide glyph's head and
  continuation carry identical style — otherwise SGR runs break at every
  CJK character and the byte budget in s05 §8 fails.
- No width math in `src/syn/` (the s09 rule, re-gated in DoD 10).

### 8. Toy definition and the debug palette — testability without Sprint 40

`tests/unit/syn_toy.c` hand-builds a `SynDef` in C: contexts `main`,
`string` (with `\` escapes and `at_eol: pop`), `comment_block` (multi-line,
non-nesting), `comment_line` (`at_eol: pop`), keywords, numbers. Roughly 12
rules. Every §2–§6 test drives this definition, so the engine's tests do
not depend on Sprint 40 landing, and Sprint 40's compiler has a known-good
target to reproduce (`yew syn dump` on its INI example is diffed against
this shape).

`src/syn/theme.h` lands here with the *interface only*:
`typedef struct { YewColor fg, bg; u16 attrs; } ThemeEnt;` plus
`const ThemeEnt *yew_theme_table(void);` returning a built-in two-entry
debug palette (comment = dim, everything else = default). Sprint 41 lands
`theme.c`, the format, and the real tables.

### 9. Defer

- Definition format, compiler, `yew syn compile`, XDG cache, validation
  → **Sprint 40**. `yew_syn_def_compile` does not exist; loading a `.fl`
  definition hard-errors `syntax definitions land in Sprint 40`.
- B-mode syntax provider → **Sprint 40**;
  `yew_block_provider_syntax_install` keeps hard-erroring per s16 DoD 11.
- Themes, the attr→color format, light/dark tables, `NO_COLOR`, runtime
  switching, underline colors → **Sprint 41**.
- Shipped language definitions → **Sprints 41/42**. `yew_syn_lang_for()`
  exists and returns `YEW_LANG_NONE` for everything; `SynBuf.lang` is
  settable only from tests.
- Cross-definition (embedded-language) states → out of 1.0's core;
  `SynState.def` is reserved and asserted 0, and any extension lands in
  its own sprint with its own goldens.
- LSP semantic tokens layered over syntax attrs → **post-1.0**; the later
  binding Sprint 46/47 contracts explicitly exclude them from 1.0.

## Testing Strategy

- **Unit** (`tests/unit/test_syn_state.c`): interner identity (same tuple →
  same id; 40 differing tuples → 40 ids); the depth-cap balance law (40
  pushes + 40 pops returns to the entry state; `lost` saturation sets
  `degraded`); pop at depth 1 is a no-op; `YEW_SYN_MAX_STATES` exhaustion
  degrades instead of failing.
- **Unit** (`tests/unit/test_syn_line.c`): the toy definition over a table
  of ≥ 40 `(entry state, line) → (spans, exit state)` rows covering string
  open/close/escape/unterminated-at-EOL, block comment open/carry/close,
  nested-comment recursion at the cap, the zero-width guard (a rule with
  `a*` terminates), the three §3 bounds each firing, and byte-cap behavior
  (exit state == entry state).
- **Unit** (`tests/unit/test_syn_splice.c`): the §5 table exhaustively —
  edit on line 0, last line, empty buffer, 1→0 lines, 0→10 000 lines,
  delete spanning `settled_to`; after each, `entry.len == line_count` and
  `entry[0] == ROOT`.
- **Unit** (`tests/unit/test_syn_settle.c`): the one-line fixpoint (typing
  inside a line settles after 1 line); the `/*`-at-top propagation on a
  50 000-line fixture completes with **no single call exceeding
  `budget_us`** (fake clock) and the viewport correct after frame 1;
  speculative styling corrected exactly once when the wave disagrees; the
  settling indicator appears only past 250 ms.
- **Differential** (`tests/unit/test_syn_diff.c`) — the correctness oracle:
  apply N random edits to a fixture, settle to completion, and compare the
  full `entry` array **and every line's spans** against a from-scratch
  highlight of the final text from `ROOT`. Any divergence is a hard
  failure. This is the only test that proves incrementality itself.
- **Fuzz** (`tests/fuzz/fuzz_syn.c`, joins `make fuzz`): random bytes as
  line content × random entry states × random toy-definition mutations
  (rule order, ops, targets); assert forward progress every iteration,
  spans non-overlapping and ascending and within `[0,len)`, `depth ∈
  [1,16]`, no state id out of range, and a 5 s alarm that never fires.
  Then the edit fuzzer: random splices + settles vs the from-scratch
  oracle, 100 k ops × 4 seeds. ASan/UBSan.
- **Pty goldens**: the toy definition rendered over a 40-line fixture at
  80×24 (byte-exact SGR runs, proving the cluster rule on a CJK+emoji
  line); an unterminated `/*` typed at the top of a 5 000-line fixture,
  asserting the viewport is correct in the frame after the keystroke and
  the settling indicator appears then clears.
- **Perf** (`tests/perf/perf_syn.c`, baselines in
  `tests/perf/baselines/syn.txt`): per-line median/p99; full-viewport cold
  restyle; single-char-edit settle; the 512 KiB single-line cap. Numbers
  become hard gates in Sprint 41 against named fixtures; here they are
  committed baselines with a 1.2× regression trip.
- **Determinism lane**: same buffer + same edits → identical `entry` array
  and identical span dumps across two runs and both compilers.

## Definition of Done

1. `make && make test` green on gcc and clang under `-std=c11 -pedantic
   -Wall -Wextra -Werror`, zero warnings; ASan/UBSan/valgrind/determinism
   lanes green.
2. `_Static_assert`s hold: `sizeof(SynState) == 40`, `sizeof(SynSpan) ==
   8`, `YEW_ATTR__COUNT <= 64`; `YEW_ATTR__COUNT == 54` asserted so a
   silent table edit fails the build.
3. **No colors in the engine**: `grep -rniE '#[0-9a-f]{6}|rgb|\b38;2\b|\b48;5\b'
   src/syn/` returns nothing, and `grep -rn 'yew_cp_width\|yew_str_width'
   src/syn/` returns nothing (width math stays in `src/unicode/`).
4. The depth-cap balance law: 40 nested pushes followed by 40 pops returns
   the exact entry state id; the test fails if `lost` handling is removed
   (proved once with a deliberately broken build in the test comment).
5. The splice table: all 8 rows unit-tested; `entry.len` equals
   `yew_textbuf_line_count` after every mutation in every test; a
   10 000-line paste performs exactly one `Vec` splice (counter asserted).
6. Fixpoint economy: typing one character inside a line of a 100 000-line
   fixture re-runs **≤ 2 lines** (counter asserted) and completes in
   ≤ 60 µs.
7. Bounded propagation: `/*` inserted at line 1 of a 1 000 000-line
   fixture never makes a single `yew_syn_settle` call exceed its
   `budget_us` (fake clock, 10 000 calls sampled), the viewport is correct
   one frame later, and settling completes to a verified fixpoint.
8. Differential oracle: 100 000 random edits × 4 seeds — incremental state
   array and span dumps identical to from-scratch highlighting, zero
   divergences.
9. Fuzz: 10 minutes each for the line fuzzer and the edit fuzzer, zero
   findings, alarm never fires, ASan/UBSan clean; corpus committed and
   replayed in `make test`.
10. Progress law: a definition containing a rule that matches empty is
    accepted and terminates on every line of the fuzz corpus; removing the
    forced-advance branch makes the test hang (documented in the test).
11. Cache effectiveness: redrawing an unchanged 200-row viewport runs
    `yew_syn_line` **zero** times (counter asserted); a one-row scroll runs
    it once.
12. Deferrals hard-error naming their sprint: loading a `.fl` definition
    (Sprint 40), theme loading (Sprint 41), `yew_syn_lang_for` returning
    anything but `YEW_LANG_NONE` (Sprints 41/42), any `SynState.def != 0`
    (`YEW_BUG`).
13. ≥ 180 assertions across the five new unit files; `make perf` runs the
    syn scenarios and writes `tests/perf/baselines/syn.txt`.
