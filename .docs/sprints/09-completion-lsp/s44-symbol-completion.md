# Sprint 44: Symbol Completion — The No-LSP Tier

## Prerequisites

- Sprint 0 — `Arena`, `Vec(T)`, `Strmap` (**insertion-ordered**),
  `Interner` (`yew_intern` / `yew_intern_str`, stable ids in first-intern
  order, id 0 = invalid), `Bytebuf`, `yew_sort_stable`, `yew_log`,
  `YEW_BUG`.
- Sprint 2 — `yew_utf8_decode`, `yew_gb_next_bytes`, `yew_cluster_width`,
  `yew_str_clip`. All width math stays in `src/unicode/`.
- Sprint 7/9/10 — `TextBuf`, `TextIter`, `yew_textbuf_line_span`,
  `yew_textbuf_line_of`, typed coords, `EditCtx`, the edit choke point.
- Sprint 13 — the command registry, `CmdDesc`, `YEW_CMD_*` flags,
  `BindRow`, `KeyStack` layers (the menu is a transient modal layer, pushed
  by a `YEW_CMD_PROMPTS` command).
- Sprint 15 — `Viewport`, `yew_gutter_width`, `yew_msg`,
  `yew_win_damage_lines`, the footer/message row rules.
- Sprint 16 — `UnitOps`, `yew_unit_word`, the UAX #29 Word_Break table in
  `src/unicode/tables_wb.c`, and the subword split table. Identifier
  extraction is a *word-unit scan*, not a bespoke `isalnum` loop.
- Sprint 18 — `CompItem`/`CompSource`/`CompMenu` in `src/ui/cmdcomp.c`.
  Those types serve the **E-mode prompt** and this sprint does not reuse
  them (§5 says why); its `yew_comp_score` seam was retired into s26's
  `yew_fz_score`, which is what this sprint ranks with.
- Sprint 19 — `YewJobSpec`/`yew_job_spawn`/`yew_job_collect_fds`/
  `yew_job_pump`/`yew_job_reap`, `YEW_SINK_COLLECT`, the read budget
  doctrine, and the argv-array rule (never string interpolation).
- Sprint 22 — `Rect`, `yew_layout_compute`, `yew_region_add`,
  `YEW_REGION_BLOCK`, the layout/draw/hit-test single-source law.
- Sprint 43 — `yew_edit_notify_pre/post`, `ShadowProvider`,
  `yew_shadow_register`, `yew_shadow_deliver`, `yew_gutter_sign_set`, and
  the **menu-vs-ghost arbitration rule** (this sprint implements the menu
  half of it; it may not re-decide it).
- Sprint 26 — the fuss scoring engine and **the one list picker**:
  ```c
  #define YEW_FZ_NO_MATCH  INT32_MIN
  #define YEW_FZ_MAX_POS   64
  typedef struct { u16 n_pos; u16 pos[YEW_FZ_MAX_POS]; } FzMatch;
  i32 yew_fz_score(const char *pat, u32 plen,
                   const char *text, u32 tlen, FzMatch *m);
  ```
  plus `PickItem`, `PickerSpec`, `yew_picker_open/key/draw/close`. s18's
  `yew_comp_score` seam is retired into `yew_fz_score` there. **This sprint
  does not use the picker** — the completion menu is a different widget
  with a different input model, and §5 says why; Sprint 47's location and
  symbol pickers are `PickerSpec` instances, which s26's Goals already
  anticipate ("LSP symbol picker (47) … `PickerSpec` instances requiring no
  new machinery").
- Sprint 39 — the syntax engine: `SynAttr` (`YEW_ATTR_TEXT = 0` …
  `YEW_ATTR__COUNT`, with the pinned *falls back to* chain — `comment` is
  id 16, `string` id 12), `SynSpan {u32 start; u16 len; u8 attr; u8 flags;}`
  with offsets **relative to line start**, `SynLineOut`, `SynBuf`, and the
  draw path's only entry point
  `void yew_syn_spans(SynBuf *s, const TextBuf *tb, LineNo l, SynLineOut *out)`
  with its 256-entry span cache keyed on `(line, buf_gen, entry_state)`.
  This is the whole reason Campaign 09 sits after Campaign 08.
- Binding: `00-decisions.md` — *"Shadow completions: three providers …
  (1) buffer/workspace symbol index (no LSP needed)"*; invariants 3, 4, 5,
  7, 8; `01-architecture.md` (`src/ws/` holds workspace-scoped state).

Assumption beyond earlier index entries, stated per cross-sprint
discipline: `Buffer` carries `buf_id` (u32, stable), `path` (canonical,
NULL for scratch), `lang` (interner id, 0 when unknown), `tb`, and the
`SynBuf syn` slot s39 Assumption (a) places there.

## Goals

Make completion work **everywhere, with no language server**: a bespoke
symbol index built from the identifiers already in the user's buffers and
workspace, ranked by fuzzy score × recency × proximity, feeding both the
passive ghost-text surface (as `YEW_SHADOW_INDEX`) and a new explicit
completion menu. This sprint delivers `src/ws/symidx.c|h` (per-buffer
extraction with syntax filtering, workspace index, incremental update),
`src/ws/symrank.c` (the integer ranking function and its tables),
`src/ws/symwalk.c` (the background workspace walk off the s19 job
machinery), and `src/ui/complmenu.c|h` — **the completion menu widget**,
which Sprint 47 reuses verbatim for LSP completion with a different item
source. Documentation panels are a *slot* here, always empty (the index has
no documentation to show); Sprint 47 fills it via `completionItem/resolve`.
Nothing here speaks JSON, spawns a language server, or knows what LSP is.

## Deliverables

### 1. Symbol records and the two indices — `src/ws/symidx.h`

```c
typedef enum { YEW_SYMK_WORD = 0,   /* an identifier we saw; no structure */
               YEW_SYMK_FUNC, YEW_SYMK_TYPE, YEW_SYMK_MACRO,
               YEW_SYMK_KEYWORD,    /* from the language's keyword set    */
               YEW_SYMK_NKIND } SymKind;

typedef struct SymEntry {
    u32     name;        /* Interner id — the ONLY copy of the bytes      */
    u32     buf_id;      /* 0 = a file not currently open (workspace tier)*/
    u32     file;        /* Interner id of the canonical path, 0 = none   */
    u64     off;         /* byte offset of the first occurrence in-file   */
    u32     line;        /* 0-based; display adds 1                       */
    u16     hits;        /* occurrences in that file, saturating          */
    u8      kind;        /* SymKind                                       */
    u8      flags;       /* YEW_SYMF_DECL when a §2 shape matched         */
} SymEntry;
_Static_assert(sizeof(SymEntry) <= 32, "sym entry bloat");

typedef struct SymIndex {
    Vec(SymEntry) e;         /* insertion-ordered; determinism            */
    Strmap        by_name;   /* interned name -> Vec(u32) posting list    */
    Arena         arena;
    u32           tick;      /* recency clock, §4                         */
    u64           bytes;     /* accounted against YEW_SYMIDX_BYTES_MAX    */
} SymIndex;
```

Two instances, both owned by `Workspace`:

| Index | Scope | Updated by | Lifetime |
|---|---|---|---|
| `buf[buf_id]` | one open buffer | §3, synchronously in the edit path | dies with the buffer |
| `ws` | every file the walk reached | §6, incrementally from the background walk | the process |

A name present in both tiers appears once in the merged query result, with
the buffer tier's record winning (it is fresher and has a live offset).
**Why the interner holds the bytes:** identifiers repeat enormously (a
100 kLOC C tree has ~15k distinct identifiers and millions of occurrences),
`yew_intern` is first-intern-ordered so the index is deterministic, and a
`u32` in `SymEntry` keeps the record at 32 bytes. **Pitfall:** the s00
interner has no delete, so nothing may intern *untrusted unbounded* strings
into it — the walk's per-file identifier cap (§6) is what bounds this, and
the DoD counts interner growth over a 10× repeat walk.

### 2. Extraction: a word-unit scan filtered by syntax attrs

```c
/* Scan [range] of buf, emitting identifiers into idx. Returns the count.
 * O(bytes) with one Word_Break lookup per codepoint and ONE yew_syn_spans
 * call per line (not per identifier, and never per byte).                */
u32 yew_symidx_scan(SymIndex *idx, Buffer *b, Span range);
```

Algorithm, pinned:

1. Walk `range` with `yew_unit_word.span()` (s16), not a bespoke character
   loop. This gets CJK, Hangul, emoji, `don't`, `1,000` and invalid bytes
   right for free, and it is the same notion of "word" the user's `→` uses.
2. Reject a span that is not an **identifier shape**: it must start with a
   Word_Break `ALetter` or `_`, and contain only `ALetter`, `Numeric`,
   `ExtendNumLet` and `Extend`. That drops punctuation runs, whitespace
   runs, pure-numeric spans (`1,000` is a word but not a symbol) and any
   span containing a U+DC80–DCFF escape.
3. Reject on length: `< YEW_SYM_MIN_LEN (3)` or `> YEW_SYM_MAX_LEN (128)`.
   Three, not two, because `if`, `to`, `id` are noise in a completion list
   and typing three characters is cheaper than dismissing them.
4. **Reject by syntax attr** — the reason this sprint lands after
   Campaign 08. The scan is line-oriented, so it calls s39's
   `yew_syn_spans(&b->syn, tb, l, &out)` **once per line** and walks the
   returned `SynSpan[]` (offsets relative to the line start) alongside the
   word spans, rather than asking per identifier. One call per line is what
   makes the 0.5 ms budget of §3 reachable and is exactly the access
   pattern s39's span cache is keyed for.

```c
/* An attr is excluded when its s39 fallback chain reaches `comment` or
 * `string`. Using the CHAIN, not the leaf, is the point: comment.doc,
 * comment.todo, string.escape, string.interp and string.special are all
 * new attrs that would otherwise leak through a leaf-id comparison the
 * day s42 adds a language that emits them.                              */
static bool syn_indexable(u8 attr) {
    for (u8 a = attr, guard = 0; guard < YEW_ATTR__COUNT; guard++) {
        if (a == YEW_ATTR_COMMENT || a == YEW_ATTR_STRING) return false;
        u8 up = yew_syn_attr_parent(a);        /* s39's table, id -> id  */
        if (up == a) break;                    /* reached `text`         */
        a = up;
    }
    return true;
}
```

`yew_syn_attr_parent` is the accessor over s39 §1's *falls back to* column.
That column is already a static `u8 parent[YEW_ATTR__COUNT]` table — s41's
theme compiler resolves the chain transitively over it at load time — so
this sprint exposes the accessor rather than inventing the data.

| Span attr (by chain) | Indexed? | Why |
|---|---|---|
| `text`, `function*`, `type*`, `namespace`, `preproc`, `variable*`, `constant*`, `attribute`, `label` | yes | real program identifiers |
| `keyword*` | yes, as `YEW_SYMK_KEYWORD` | the user wants `return` completed too, but ranked down (§4) |
| `number`, `boolean`, `operator` | no | never identifiers, and the §2 shape filter drops them anyway |
| anything under `comment` | **no** | prose pollutes the index with English |
| anything under `string` | **no** | string bodies; see the exception below |

  Exception, pinned: a string that is the whole of a `#include "…"` /
  `import "…"` argument is still skipped — we do not special-case it. Paths
  belong to s26's file finder, not here.
  **Pitfall:** classify by the span covering `word.lo − line.lo`, *not* by
  sampling the middle of the token. A word can straddle a span boundary
  (`string.escape` sits inside a `string`), and sampling the middle admits
  half the comments in the file. When no span covers the offset — s39 emits
  `stop = SPANS`/`BYTES` on pathological lines and leaves the remainder as
  `text` — the identifier is indexed; a degraded highlight must not silently
  empty the index.
  In a buffer with no language (`b->lang == 0`), `yew_syn_spans` yields one
  `text` span per line and the index degrades to "every word in the file" —
  exactly right for a `.txt`, and the behaviour the no-LSP tier promises.

5. Kind inference is one cheap lookahead, not a parser: an identifier
   immediately followed (skipping spaces) by `(` on the same line is
   `YEW_SYMK_FUNC`; one preceded on the line by `struct`/`enum`/`union`/
   `class`/`type` is `YEW_SYMK_TYPE`; one at column 0 of a line whose first
   token is `#define` is `YEW_SYMK_MACRO`; otherwise `YEW_SYMK_WORD`.
   `YEW_SYMF_DECL` is set for the `FUNC`/`TYPE`/`MACRO` cases. This is a
   heuristic and `symidx.h` says so; Sprint 47's `documentSymbol` is the
   accurate answer when a server exists.

### 3. Incremental buffer update — the 0.5 ms budget

The s43 hooks carry the work:

```c
void yew_symidx_note_pre (EditCtx *ec, u8 kind, ByteOff at, u64 len);
void yew_symidx_note_post(EditCtx *ec, u8 kind, ByteOff at, u64 len);
```

`_pre` records the **pre-op line range** the edit touches
(`yew_textbuf_line_of(at)` … `yew_textbuf_line_of(at+len)` for a delete,
one line for an insert) into a small per-buffer dirty accumulator.
`_post` widens it to post-op line numbers and marks the buffer dirty. The
rescan itself runs **once per event-loop iteration**, in the pump phase,
not per keystroke:

```c
/* Drain the dirty accumulator: drop every SymEntry whose `line` falls in
 * the dirty range, then yew_symidx_scan that range. Budgeted.           */
void yew_symidx_pump(Ed *ed, i64 budget_us);
```

| Constant | Value | Why |
|---|---|---|
| `YEW_SYMIDX_BURST_US` | **500** | the budget the goal states: ≤ 0.5 ms per keystroke burst, inside the 5 ms keypress gate (invariant 4) |
| `YEW_SYMIDX_DIRTY_MAX_LINES` | 512 | a wider dirty range is cheaper to handle as a full rescan |
| `YEW_SYMIDX_FULL_US` | 4000 | budget for a full-buffer rescan, chunked across loop iterations |
| `YEW_SYMIDX_BYTES_MAX` | 32 MiB | both tiers together; past it the walk stops and `:compl.stats` says so |

Behaviour when a rescan cannot finish inside `budget_us`: the dirty range
stays dirty and the rest lands next iteration. The index is therefore
allowed to be *stale*, never *wrong*: a stale index costs a missing
candidate, and a completion menu with a missing candidate is a mild
disappointment, while a keystroke over budget is a CI failure.

**Pitfall — deleting entries by line number.** Entries carry a line, not a
mark, so an edit that shifts 10 000 lines by one would invalidate the whole
buffer index if lines were the identity. They are not: the dirty range is
computed in *both* coordinate spaces by `_pre`/`_post`, entries **below**
the edit keep their lines, and entries **above** it have their `line`
shifted by the edit's net newline delta in one pass over `idx->e` (a linear
`u32` add over a contiguous vector — ~1 µs per 10 k entries, measured in the
perf harness). Using `MarkId` per entry instead was considered and rejected:
10 000 marks per buffer makes `yew_marks_adjust` the hottest function in the
editor for zero correctness gain.

### 4. Ranking — `src/ws/symrank.c`

```c
typedef enum { YEW_PROX_CURSOR = 0,  /* same buffer, within 200 lines     */
               YEW_PROX_BUFFER,      /* same buffer                       */
               YEW_PROX_DIR,         /* same directory as the buffer      */
               YEW_PROX_WS,          /* anywhere in the workspace         */
               YEW_PROX_N } SymProx;

/* Integer only — no float anywhere in the ranking path (invariant 5:
 * the same index and stem must produce the same order on every host and
 * every compiler).                                                      */
/* `fuzzy` is yew_fz_score's return, already filtered for
 * YEW_FZ_NO_MATCH by the caller.                                       */
i32 yew_sym_rank(i32 fuzzy, u32 age, SymProx prox, u8 kind, u16 hits);
```

```c
static const i32 PROX_MUL[YEW_PROX_N] = { 150, 130, 115, 100 };  /* percent */
/* Recency: a saturating 8-step decay over the tick clock. age is
 * idx->tick - entry_tick, in "index updates", not wall time — wall time
 * makes ranking depend on how long the user stared at the screen.       */
static const i32 REC_MUL[8] = { 160, 140, 125, 115, 108, 104, 102, 100 };
static i32 rec_mul(u32 age) { u32 b = 0; while (age >> b && b < 7) b++;
                              return REC_MUL[b]; }
static const i32 KIND_MUL[YEW_SYMK_NKIND] = { 100, 118, 112, 105, 90 };
/*                                          word  func  type macro keyword */

i32 yew_sym_rank(i32 fuzzy, u32 age, SymProx prox, u8 kind, u16 hits) {
    i64 r = (i64)fuzzy * PROX_MUL[prox] / 100;
    r = r * rec_mul(age) / 100;
    r = r * KIND_MUL[kind] / 100;
    r += (hits > 16 ? 16 : hits);        /* a tiny, bounded frequency nudge */
    return (i32)(r > INT32_MAX ? INT32_MAX : r);
}
```

- Keywords are ranked **down** (90): the user already knows them and can
  type them, whereas a 30-character function name is the reason completion
  exists.
- `hits` is additive and capped so a token that appears 4 000 times cannot
  outrank a good fuzzy match — frequency is a tiebreaker, not a signal.
- **Determinism:** ties break by (name length ascending, then interned name
  bytes with `memcmp`, then `file`, then `off`), sorted with
  `yew_sort_stable`. Never by posting-list order, never by `qsort` — s00
  bans raw `qsort` repo-wide for exactly this reason.

Query API, shaped like s10's `yew_undo_list` (caller-supplied buffer, so a
renderer never allocates):

```c
typedef struct SymQuery {
    const char *stem; u32 slen;
    u32   buf_id;  ByteOff pos;      /* proximity context                 */
    u32   max;                       /* cap; YEW_SYM_QUERY_MAX = 200      */
    bool  keywords;                  /* include YEW_SYMK_KEYWORD          */
} SymQuery;

typedef struct SymHit { u32 name; i32 rank; u8 kind; u8 prox;
                        FzMatch m;             /* s26 match positions    */
                        u32 file; u32 line; } SymHit;

u32 yew_symidx_query(Workspace *ws, const SymQuery *q, SymHit *out, u32 max);
```

The stem itself is excluded from its own results (completing `foo` must not
offer `foo` when the only occurrence is the one being typed — check
`file == current && line == cursor line && hits == 1`).

### 5. The completion menu widget — `src/ui/complmenu.c|h`

This is the reusable surface. Sprint 47 supplies LSP items through the same
`ComplSource` and changes nothing else.

```c
typedef struct ComplItem {
    const u8 *label;  u32 label_len;    /* what is listed                 */
    const u8 *insert; u32 insert_len;   /* what is typed (may differ)     */
    const u8 *detail; u32 detail_len;   /* right column: type sig, path   */
    const u8 *doc;    u32 doc_len;      /* panel body; NULL = unresolved  */
    u8   kind;                          /* SymKind or the s47 LSP mapping */
    i32  score;
    FzMatch m;                          /* s26 label offsets to highlight */
    void *user;                         /* source-owned; s47 stores the
                                           LSP item for lazy resolve      */
} ComplItem;

typedef struct ComplSource {
    const char *name;                   /* "index" | "lsp"                */
    /* Fill items for `stem`. Synchronous sources return the count; async
     * ones return 0 now and call yew_compl_push later (s47).             */
    u32  (*enumerate)(Ed *ed, Win *w, const u8 *stem, u32 slen,
                      Vec_ComplItem *out, void *ctx);
    /* Fill item->doc/detail on demand. NULL = nothing to resolve.        */
    void (*resolve)(Ed *ed, ComplItem *item, void *ctx);
    void *ctx;
} ComplSource;

typedef struct ComplMenu {
    bool  open;
    Vec_ComplItem items;
    i32   sel;              /* index; -1 only transiently while filling   */
    u32   top;              /* first visible row                          */
    Span  replace;          /* buffer bytes the accept replaces           */
    u64   buf_gen;          /* staleness key, s43 §2 rule 2               */
    Rect  box, panel;       /* computed once, shared by draw + hit-test   */
    bool  panel_open;
    const ComplSource *src;
} ComplMenu;                /* one per Win                                */

bool yew_compl_open (Ed *ed, Win *w, const ComplSource *src);  /* explicit */
void yew_compl_close(Ed *ed, Win *w, bool accepted);
void yew_compl_push (Ed *ed, Win *w, const ComplItem *it, u32 n); /* async  */
bool yew_compl_key  (Ed *ed, Win *w, const Key *k);   /* false = not ours  */
void yew_compl_draw (Ed *ed, Win *w, Grid *g);        /* last in the frame */
```

**Why this is not s18's `CompMenu` and not s26's picker.** s18's menu edits a
one-line prompt `TextBuf` and its `replace` span is in that prompt; this one
replaces a span in the user's document and must survive edits to it. s26's
picker is a *modal, full-screen-ish, fuzzy-filtered chooser over a static
list*; this is a *non-modal, anchored, incrementally re-filtered* surface
that must let ordinary typing through to the buffer. Merging them would mean
one widget with two input models — the version of that mistake every editor
ships is a completion popup that swallows your keystrokes.

Presentation, pinned (all cell arithmetic from `src/unicode/`):

```
        foo_bar(                          ← the buffer line, cursor here
        ┌──────────────────────┬──────────────────────────┐
        │f yew_foo_bar     fn  │ yew_foo_bar(TextBuf *tb, │  ← doc panel
        │v yew_foo_baz     var │ ByteOff at)              │     (slot;
        │t YewFooBarOpts   type│                          │      empty
        │w foo_barrier         │ (no documentation)       │      in s44)
        └──────────────────────┴──────────────────────────┘
          12 of 87   index
```

- Anchored under the `replace` span's start cell; flipped above when there
  are fewer than 4 rows below (`YEW_COMPL_ROWS = 10` visible max, width
  clamped to `YEW_COMPL_W = 44` label + 12 detail).
- Kind glyphs are **one cell, ASCII**, so they are correct at every width
  setting and in every terminal:

| Kind | Glyph | Theme role |
|---|---|---|
| `YEW_SYMK_FUNC` | `f` | `compl.func` |
| `YEW_SYMK_TYPE` | `t` | `compl.type` |
| `YEW_SYMK_MACRO` | `m` | `compl.macro` |
| `YEW_SYMK_KEYWORD` | `k` | `compl.keyword` |
| `YEW_SYMK_WORD` | `w` | `compl.word` |
| (s47 adds `v` var, `c` const, `F` field, `M` module, `p` property, `e` enum, `s` snippet) | | |

- Matched offsets (`ComplItem.m.pos`, straight from `yew_fz_score`)
  render `bold` over the label's base
  role — the score's own evidence, so the user can see why an item ranked.
- The documentation panel is a **slot**: reserved, laid out, and drawn with
  `(no documentation)` whenever `doc == NULL`. It is opened by
  `ed.compl.doc_toggle` (default `C-<space>` inside the menu) and its
  contents are plain text, hard-wrapped at the panel width by
  `yew_str_clip`. Never markdown — there is no markdown renderer in 1.0 and
  s47 asks servers for `plaintext`.
- Footer: `<sel+1> of <n>   <src->name>`. The source name is how the user
  knows whether they are looking at the index tier or a server.
- `box` and `panel` are computed **once**, in `yew_compl_open` and on
  resize, and consumed by both the drawing pass and `yew_region_add`
  (s22's law). Rows register `YEW_REGION_COMPL_ROW` with payload = item
  index; the whole box registers `YEW_REGION_BLOCK` underneath so clicks
  never fall through to the document.

Keys (a transient keymap layer pushed by the `YEW_CMD_PROMPTS` opener, s13):

| Key | Command | Behaviour |
|---|---|---|
| `<up>`/`<down>`, `C-p`/`C-n` | `ed.compl.prev/next` | wraps at both ends |
| `<pgup>`/`<pgdn>` | `ed.compl.page_prev/next` | |
| `<tab>` | `ed.compl.accept` | inserts `insert`, replacing `replace` |
| `<cr>` | `ed.compl.accept` | **only when an item is selected**; otherwise falls through and inserts a newline |
| `<esc>` | `ed.compl.cancel` | closes; the buffer is left exactly as typed |
| `C-<space>` | `ed.compl.doc_toggle` | opens/closes the panel |
| any printable | falls through to the buffer | the stem grows, the list re-filters, `replace` extends |
| `<bs>` | falls through | stem shrinks; closes when the stem is empty |
| anything else | closes the menu, then re-dispatches the key | never swallowed |

Accept applies through the choke point as one transaction
(`yew_undo_begin(YEW_TXN_PASTE)` → `yew_edit_delete(replace)` →
`yew_edit_insert` → `yew_undo_end`), so one undo takes the completion back
whole. Staleness: `yew_compl_key`'s fall-through path re-checks `buf_gen`
against `tb->gen` and closes the menu if anything other than the expected
stem edit happened.

s43 command domain rules apply: this sprint adds the domain **`compl`**
(a review event, taken deliberately — these commands are neither `edit` nor
`ui`), registering `ed.compl.{open,next,prev,page_next,page_prev,accept,
cancel,doc_toggle}` plus `ed.compl.stats`.

**Trigger policy, implementing s43 §9 and not re-deciding it:**

| Surface | Trigger | Suppresses |
|---|---|---|
| ghost text | passive, on the s43 debounce | suppressed while `menu.open` |
| menu | **explicit only**: `ed.compl.open` (default `C-<space>` in I mode), or a language trigger character from `compl.trigger_chars` (default `"." "->" "::"`) when `compl.auto_trigger` is true (default **false** in s44; s47 turns it on for languages whose server advertises `triggerCharacters`) | dismisses the ghost on open; re-arms it on close |

### 6. The workspace tier and the background walk — `src/ws/symwalk.c`

```c
typedef struct SymWalk {
    u32     job;             /* s19 job id for file discovery; 0 = none   */
    Vec(u32) queue;          /* interned paths still to read              */
    u32     next;            /* cursor into queue                         */
    u64     files_done, files_total, bytes_read;
    bool    running, capped;
} SymWalk;

void yew_symwalk_start(Ed *ed);                  /* :compl.reindex, open  */
void yew_symwalk_pump (Ed *ed, i64 budget_us);   /* poll-loop pump phase  */
void yew_symwalk_stop (Ed *ed);
```

Two phases, and only the first is a subprocess:

1. **Discovery** runs off the s19 job machinery — `yew_job_spawn` with
   `sink = YEW_SINK_COLLECT` and an **argv array** (s19 §9's rule: never
   string interpolation), `git ls-files -z --cached --others
   --exclude-standard` when `yew_ws_root()` is a repo, else a bespoke
   `opendir`/`readdir` walk pumped in the same budget as phase 2. Using git
   for discovery is free correctness: it gets `.gitignore`, submodules and
   `node_modules` right without us shipping an ignore-file parser.
   Absent `git` degrades to the bespoke walk with a fixed skip list
   (`.git`, `build`, `node_modules`, `target`, `.venv`, `dist`) — an
   optional runtime tool degrading gracefully, per `00-decisions.md`.
2. **Reading and scanning** is a cooperative task pumped from the same loop
   phase as `yew_job_pump`, **not** a subprocess: it is pure CPU over local
   files, and invariant 8 says concurrency is the event loop plus
   subprocesses, not a thread doing our own work. Each pump reads at most a
   bounded chunk into a throwaway `TextBuf`, then scans at most four ordinary
   lines (default 2000 µs per iteration, halved to 1000 while any key is
   pending). A file containing a line longer than 64 KiB is skipped by the
   background tier because TextBuf coordinate maintenance and the word-unit
   scan are indivisible at line scope; this does not affect the authoritative
   open-buffer tier.

Caps, each with a `:compl.stats` row:

| Constant | Value | Behaviour past it |
|---|---|---|
| `YEW_SYMWALK_MAX_FILES` | 20 000 | stop, `capped = true`, message once |
| `YEW_SYMWALK_MAX_FILE_BYTES` | 4 MiB | skip that file |
| `YEW_SYMWALK_MAX_LINE_BYTES` | 64 KiB | skip that background file |
| `YEW_SYMWALK_MAX_SYMS_PER_FILE` | 4 000 | stop scanning that file (the interner-growth bound, §1) |
| `YEW_SYMIDX_BYTES_MAX` | 32 MiB | stop the walk entirely |

Binary detection reuses s08's rule (NUL in the first 8 KiB → skip). Files
whose language is unknown are still scanned — the no-language degradation of
§2 is the point. A file that is also an open buffer is skipped by the walk;
the buffer tier owns it.

Re-walk policy: on `:compl.reindex`, on workspace open, and never
automatically otherwise. A file saved from inside yew updates the
workspace tier from the buffer tier at save time (one `yew_symidx_scan` of
the whole buffer into `ws`, replacing that file's entries) — no filesystem
watching, no inotify, no polling. Pinned in `symwalk.h` as a non-goal.

### 7. The ghost-text provider

```c
static bool index_request(Ed *ed, const ShadowReq *r);
static const ShadowProvider yew_shadow_index = {
    .name = "index", .prov = YEW_SHADOW_INDEX, .debounce_ms = 0,
    .request = index_request, .cancel = NULL,
};
```

Synchronous by design (s43 §4's table pins the 0 ms row): `index_request`
extracts the stem behind `r->pos` with `yew_unit_word.home`, runs
`yew_symidx_query` with `max = 1`, and calls `yew_shadow_deliver` **inline**
with `seq = r->seq`. It offers the top hit's *remaining* bytes (label minus
stem) as a single-line suggestion — the index has no multi-line content to
offer, and pretending otherwise would put fabricated call syntax into the
user's file.

Two guards: no suggestion when the top hit's rank is below
`YEW_SYM_GHOST_MIN` (default 400 — a weak fuzzy match shown as a ghost reads
as the editor guessing), and no suggestion when the stem is shorter than
`YEW_SYM_GHOST_MIN_STEM` (3).

### 8. Defer

- **Documentation content** for index items: there is none, and there will
  be none — the panel slot renders `(no documentation)`. `ComplItem.doc`
  is filled only by Sprint 47's `completionItem/resolve`.
- **Snippet items** (`YEW_SYMK_SNIPPET`, glyph `s`): the kind exists in the
  enum and the glyph table for Sprint 47's item mapping; nothing in this
  sprint produces one, and s47 §3 pins the downgrade policy.
- LSP completion, hover, signature help, and the location/symbol pickers →
  Sprints 46/47. `ed.compl.open` with `src = lsp` is unreachable here (no
  such source is registered).
- AI provider → Sprint 49.
- Filesystem watching / auto-reindex → **never for 1.0** (§6).
- Cross-workspace or global index → never; the workspace is the unit
  (`00-decisions.md`: dir = workspace).
- Options (`compl.*`, `shadow.*`) become real config keys in Sprint 36's
  model; this sprint reads them through the same accessor with compiled-in
  defaults.

## Testing Strategy

- **Unit** (`tests/unit/test_symidx.c`): extraction over a C fixture with a
  stub `yew_syn_spans` returning scripted `SynSpan` runs — assert every
  comment word and every string-body word is absent and every code
  identifier present; every attr under the `comment` and `string` fallback
  chains (`comment.doc`, `comment.todo`, `string.escape`, `string.interp`,
  `string.special`) excluded, proving `syn_indexable` walks the chain and
  not the leaf; a word straddling a `string`/`string.escape` boundary; a
  line whose spans stop early (`stop = SPANS`) still indexing its
  remainder; the same fixture with one whole-line `text` span per line
  yields the "plain .txt" superset; the identifier
  shape table (leading digit, leading `_`, `1,000`, `don't`, `漢字`,
  `👨‍👩‍👧‍👦`, an invalid-byte run, a 200-char token) with the expected
  accept/reject per row; kind inference for `foo(`, `struct Bar`,
  `#define BAZ`.
- **Unit — incremental** (`test_symidx_inc.c`): after N random edits, the
  incrementally maintained buffer index is **byte-identical** to a full
  rescan (same entries, same order, same lines) — the differential oracle
  is the whole point; 4 seeds × 5 000 edits; includes multi-line paste,
  whole-buffer delete, and an edit at offset 0 of a 10 000-line fixture.
- **Unit — ranking** (`test_symrank.c`): `yew_sym_rank` against ≥ 30
  hand-computed vectors covering every `PROX_MUL`, every `REC_MUL` step, all
  five `KIND_MUL` rows, and `hits` saturation; determinism — the same hit
  set shuffled into 8 different insertion orders sorts identically; no
  floating-point token appears in `symrank.c`
  (`grep -n 'float\|double' src/ws/symrank.c` → empty).
- **Unit — menu** (`test_complmenu.c`): the key table, one row per line,
  including `<cr>` falling through with no selection and an unknown key
  closing-then-re-dispatching; accept produces exactly one undo node and one
  undo restores the pre-menu text byte-exactly; `replace` extends correctly
  as the stem grows; a buffer edit from elsewhere (simulated `tb->gen` bump)
  closes the menu; box/panel `Rect`s from `yew_compl_open` equal the ones the
  hit-tester uses (same function, asserted by calling it twice).
- **Unit — walk** (`test_symwalk.c`): a fixture tree with 200 files
  including a 5 MiB file (skipped), a binary file (skipped), a file with
  10 000 identifiers (capped at 4 000), and a `.gitignore`d directory
  (absent when git discovery is used, present with the bespoke walk's skip
  list absent) — assert file counts and interner growth; 10 consecutive
  walks over the same tree grow the interner by 0 after the first.
- **Script** (`tests/script/compl_*`, via the s19-era headless driver until
  Sprint 37): complete in a `.txt` with no language (proves the no-language
  tier), in a C file with the real s39 highlighter (proves the comment/string
  filter end to end), and across two open buffers (proves the proximity
  tiers order results as the table says).
- **Pty goldens** (`tests/pty/compl_*.golden`): the menu anchored below and
  flipped above; the doc panel open and closed; the footer counts; the
  matched-offset bold highlighting; the menu at the right screen edge
  (clamped, not truncated mid-cluster); a menu whose labels contain CJK and
  emoji (column alignment proves the width math is s02's).
- **Perf** (`tests/perf/perf_symidx.c`, baselines committed):
  **the 0.5 ms gate** — 10 000 single-character inserts into a 10 kLOC C
  buffer, asserting p99 of `yew_symidx_pump` ≤ 500 µs; a full 100 kLOC
  workspace walk completes without any single pump exceeding its budget and
  without keypress p99 exceeding 5 ms under a scripted typing load;
  `yew_symidx_query` over a 15 000-symbol index ≤ 400 µs.
- **Fuzz** (`tests/fuzz/fuzz_symidx.c`): random buffers (invalid UTF-8, 1 MB
  single line, lone CR, pure emoji) × random edit streams × random queries.
  Invariants: no crash, `yew_symidx_query` never returns an entry whose
  `line` exceeds the buffer's line count, every returned `name` resolves
  through `yew_intern_str`, index bytes never exceed the cap, and two runs
  with the same seed produce identical result orders.

## Definition of Done

1. `make test` green on gcc and clang, zero warnings under the locked
   flags; ASan/UBSan/valgrind lanes clean.
2. **The 0.5 ms budget is a gate:** `yew_symidx_pump` p99 ≤ 500 µs over the
   10 000-keystroke perf case, baseline committed under
   `tests/perf/baselines/symidx.txt`.
3. Incremental correctness: the differential oracle (incremental index ==
   full rescan) is green for 4 seeds × 5 000 edits, including the
   offset-0-of-10 000-lines case.
4. Syntax filtering proven both ways: with the real s39 highlighter, a C
   fixture's comment and string words are absent from the index and its code
   identifiers are present; with `lang == 0` the same fixture indexes every
   word. Both asserted by count *and* by named example.
5. `sizeof(SymEntry) <= 32` (`_Static_assert`); index memory for a 100 kLOC
   workspace ≤ 32 MiB, reported by `:compl.stats`.
6. Determinism: the token-aware `float|double|qsort` scan covers the
   symbol-index sources rather than all workspace owners —
   `grep -En '(^|[^[:alnum:]_])(float|double|qsort)([^[:alnum:]_]|$)'
   src/ws/sym*.c src/ws/sym*.h` → empty. The same query on the same index
   returns byte-identical `SymHit` arrays across two runs and across 8
   insertion-order permutations.
7. Every row of the §5 key table has a firing assertion, including
   `<cr>`-falls-through and unknown-key-re-dispatch.
8. One completion accept == exactly one undo node; one undo restores the
   pre-menu buffer byte-exactly (asserted on a 3-buffer script test).
9. Menu geometry is computed once: `yew_compl_draw` and the region
   registration both read `menu.box`/`menu.panel`, and a test asserts the
   hit-tested row index equals the drawn row index for all 10 rows at three
   terminal widths, including a width where the box is clamped.
10. The menu and the ghost never coexist: a script that triggers both
    asserts `!(w->shadow.live && w->compl.open)` after every dispatch, and
    the s43 debug assert is proven to fire when the rule is violated.
11. The walk degrades: with `git` present it honors `.gitignore`; with `git`
    absent (PATH stripped in the test) it completes via the bespoke walk and
    messages once. Neither path exceeds its per-pump budget.
12. Interner discipline: 10 repeat walks over a 200-file tree grow the
    interner by 0 entries after the first; the per-file symbol cap fires on
    the 10 000-identifier fixture.
13. `ComplItem.doc` renders `(no documentation)` for every index item and
    `symidx.h` states the no-filesystem-watching non-goal. Sprint 47 delivered the LSP documentation handover through `completionItem/resolve`; current
    closeout tests both the permanent index behavior and that live LSP path.
14. ≥ 170 assertions across the five new unit files; determinism lane green
    on the new pty goldens.
