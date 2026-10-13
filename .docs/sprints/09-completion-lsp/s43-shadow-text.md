# Sprint 43: Shadow Text — The Ghost-Text Engine

## Prerequisites

- Sprint 2 — `yew_utf8_decode`, `yew_gb_next_bytes`, `yew_cluster_width`,
  the U+DC80–DCFF escape policy. Ghost text is bytes; every width question
  goes through `src/unicode/`.
- Sprint 5 — `Cell` (20 B), `attrs` bitmask (bit 1 `dim`, bit 2 `italic`),
  `yew_grid_put/puts/fill`, `Damage` spans, `yew_cell_eq`. Ghost cells are
  ordinary cells; the renderer learns nothing new.
- Sprint 7 — `TextBuf`, `tb->gen` (bumped per edit), `yew_textbuf_from_bytes`,
  `yew_textiter_*`, `yew_textbuf_line_span`.
- Sprint 9 — `Cursor`, `MarkId`/`MarkBias`, `yew_off_to_gcol`,
  `yew_grapheme_next/prev`, `yew_is_grapheme_boundary`, `CursorSet`.
- Sprint 10 — the **edit choke point** `yew_edit_insert` / `yew_edit_delete`
  in `src/text/edit.c` with its pinned six-step body, `EditCtx`,
  `yew_undo_begin/end/abort`, `YewTxnReason`. This sprint inserts the two
  notify steps (§3) into that body; it does **not** add a callback registry
  (s10's doctrine: one op stream, N consumers, never a parallel
  notification path).
- Sprint 13 — `CmdDesc`/`yew_cmd_register`, `CmdCtx`, `YEW_CMD_*` flags,
  `BindRow`, `KeyStack` layers, the closed command-domain list.
- Sprint 15 — `Viewport` (`top`, `top_sub`, `rows`, `cols`, `wrap`),
  `yew_vp_row_of_line`, `yew_gutter_width`, `NumStyle`, `yew_msg`,
  `yew_win_damage_lines`. The gutter's **2-cell sign column is reserved and
  always empty** as of s15; this sprint lands the API that fills it.
- Sprint 16 — `UnitOps`, `yew_unit_word`, `yew_unit_block`,
  `yew_unit_of_mode(Mode)`. Accept-word is spelled through the vtable.
- Sprint 22 — `Rect`, `yew_region_add`, `YEW_REGION_BLOCK`.
- Sprint 39 — `SynBuf`, `yew_syn_attach`, and
  `yew_syn_edit(SynBuf *s, LineNo lo, u64 removed, u64 inserted)`, which
  s39 §5 calls "the only mutation site" and whose Assumption (b) is that
  *"the edit choke point publishes a line-damage record `{LineNo lo; u64
  removed, inserted;}` per edit"*. §3 below is that publication, so
  `yew_syn_edit` is a consumer of it rather than a second mechanism.
- Binding: `00-decisions.md` — *"Shadow completions: three providers, one
  ghost-text UX with distinct provenance styling … Accept by word / line /
  all"*; invariants 1 (no data loss), 3 (no silent stubs), 4 (latency is a
  gate), 5 (deterministic rendering), 8 (single-threaded core);
  `01-architecture.md` §Async subsystems.

Assumptions beyond earlier index entries, stated per cross-sprint
discipline:

- **(a)** Sprint 14's event loop exposes a timer heap as
  `u32 yew_timer_set(Ed *ed, i64 deadline_ms, void (*fn)(Ed*, void*), void *u)`
  and `void yew_timer_cancel(Ed *ed, u32 id)`. s15 refers to "s14 heap
  timers" without spelling them; if s14 spelled them differently this
  sprint adapts at the three call sites in `shadow.c`.
- **(b)** `Win` carries `buf` (a `Buffer*` with a stable `u32 buf_id`),
  `cset` (`CursorSet`), and `vp` (`Viewport`).

## Goals

Land the ghost-text engine as **core**, not a module: three providers feed
one surface, two of them (LSP, AI) live behind `MODULES=`, and a surface
that vanishes when a module is excluded is not a surface. This sprint
delivers `src/edit/shadow.c|h` (provider interface, generation counters,
debounce, acceptance, the I-mode interaction rules), `src/ui/shadowdraw.c`
(inline and multi-line rendering with the **overlay-never-push** layout
law), the gutter sign API `src/ui/gutter.c` grew a hole for in s15, and the
two edit-notify hooks in `src/text/edit.c` that Sprints 44 and 46 also
consume. The only provider registered here is a **test provider** behind
`YEW_SHADOW_TEST=1`; the symbol-index provider is Sprint 44, the LSP
provider Sprint 47, the AI provider Sprint 49. The explicit completion
*menu* is Sprint 44 — this sprint owns the passive surface only and pins
the arbitration rule (§9) that keeps the two from ever drawing in the same
frame.

## Deliverables

### 1. Provider interface — `src/edit/shadow.h`

```c
typedef enum { YEW_SHADOW_INDEX = 0,   /* buffer/workspace symbols (s44) */
               YEW_SHADOW_LSP,         /* textDocument/completion (s47)  */
               YEW_SHADOW_AI,          /* streamed completion    (s49)   */
               YEW_SHADOW_NPROV } ShadowProv;

typedef struct ShadowReq {
    u32     buf_id;
    u64     buf_gen;      /* TextBuf.gen at request time — the staleness key */
    ByteOff pos;          /* cursor byte offset, a grapheme boundary        */
    Span    line;         /* the cursor's line span, for cheap prefix work  */
    u32     seq;          /* monotonic per (buf, provider); §2             */
    u8      prov;
} ShadowReq;

typedef struct ShadowSug {
    u32      seq;         /* echoes the request                            */
    u8       prov;
    u32      buf_id;
    u64      buf_gen;     /* copied from the request, never re-read        */
    ByteOff  pos;         /* the offset the text was computed AT           */
    const u8 *text;  u32 len;   /* arena-owned by the shadow arena         */
    u32      consumed;    /* bytes of `text` the user has since typed (§8) */
    TextBuf *scratch;     /* lazily built for accept-word (§7); may be NULL*/
} ShadowSug;

typedef struct ShadowProvider {
    const char *name;                       /* "index" "lsp" "ai"         */
    u8          prov;
    u32         debounce_ms;                /* §4                         */
    /* Fire an async request. Returns false when the provider declines
     * (no server for this language, AI disabled, …) — declining is normal
     * and silent, never an error.                                        */
    bool (*request)(Ed *ed, const ShadowReq *r);
    /* Best-effort cancel of everything with seq < up_to. Providers that
     * cannot cancel (the index, which is synchronous) leave this NULL.   */
    void (*cancel)(Ed *ed, u32 buf_id, u32 up_to);
} ShadowProvider;

void yew_shadow_register(const ShadowProvider *p);   /* YEW_BUG on dup    */
/* THE delivery entry point. Called from the poll-loop pump phase only.   */
void yew_shadow_deliver(Ed *ed, const ShadowSug *s);
```

Providers are stored in a fixed `ShadowProvider *[YEW_SHADOW_NPROV]` indexed
by `prov`, not a growable list: the set is closed by `00-decisions.md` and a
NULL slot is exactly what an excluded module leaves behind. There is no
"provider not found" error path because there is no provider lookup by name.

### 2. The staleness law — display-only ghosts

```c
typedef struct Shadow {
    bool     live;
    ShadowSug sug;
    u32      seq_next[YEW_SHADOW_NPROV];   /* next seq to hand out       */
    u32      seq_min[YEW_SHADOW_NPROV];    /* drop anything below this   */
    u32      timer;                        /* debounce timer id, 0 = none*/
    u16      vrows;                        /* virtual rows currently drawn */
} Shadow;                                  /* one per Win                */
```

Three rules, each with a firing test:

1. **A ghost is never text.** `Shadow` holds bytes and a position; nothing
   in the buffer, no `Mark`, no piece, no undo op. Rendering reads it;
   acceptance is the *only* path that ever reaches `yew_edit_insert`.
2. **Generation counters drop stale results.** Every edit bumps
   `tb->gen` (s07) and, through §3, raises `seq_min[p]` for every provider
   to `seq_next[p]`. `yew_shadow_deliver` drops any suggestion with
   `s->seq < seq_min[s->prov]` or `s->buf_id != win->buf->buf_id`, silently,
   counting it in `shadow.dropped` for the perf harness.
3. **Acceptance re-validates against the current buffer.** Even a
   suggestion that survives rule 2 must not be trusted to describe the
   present. Before any insert:

```c
/* Returns the number of bytes of sug->text the buffer has already
 * absorbed since sug->pos, or -1 when the ghost no longer describes this
 * buffer at all. THE guard: a suggestion computed for one buffer state
 * must never be spliced into a different one.                           */
static i64 shadow_revalidate(const TextBuf *tb, const ShadowSug *sug,
                             ByteOff cur) {
    if (cur < sug->pos) return -1;              /* cursor moved backwards */
    u64 n = cur.v - sug->pos.v;
    if (n > sug->len) return -1;                /* typed past the ghost   */
    /* Compare the bytes the user typed against the ghost's head. One
     * chunk walk; no allocation; O(n) with n <= the ghost's length.     */
    TextIter it; yew_textiter_begin(&it, tb, sug->pos);
    u64 i = 0;
    const u8 *p; u64 k;
    while (i < n && yew_textiter_chunk(&it, tb, &p, &k)) {
        u64 take = (k < n - i) ? k : n - i;
        if (memcmp(p, sug->text + i, take) != 0) return -1;
        i += take;
        if (!yew_textiter_advance(&it, tb)) break;
    }
    return (i == n) ? (i64)n : -1;
}
```

`shadow_revalidate` returning −1 dismisses the ghost and inserts **nothing**,
with `yew_msg(ed, YEW_MSG_INFO, "suggestion is stale")`. This is the pitfall
the whole design exists to kill: an LSP or AI response that lands 80 ms
after a keystroke describes a buffer that no longer exists, and splicing its
text is silent corruption of the user's file (invariant 1).

### 3. Edit-notify hooks — `src/text/edit.c` (core plumbing)

s10 pins `yew_edit_insert`/`yew_edit_delete` as the only mutators, with a
six-step body. This sprint inserts two calls, and **only** two:

| # | Step (s10) | Added here |
|---|---|---|
| 1 | capture deleted bytes into the undo blob arena | |
| — | | **`yew_edit_notify_pre(ec, kind, at, len)`** |
| 2 | `yew_textbuf_insert` / `yew_textbuf_delete` | |
| 3 | `yew_marks_adjust` (pre-op coordinates) | |
| 4 | `yew_cset_adjust` | |
| — | | **`yew_edit_notify_post(ec, kind, at, len)`** |
| 5 | `yew_journal_record` | |
| 6 | `yew_undo_record` | |

```c
/* Fixed call lists, in this order, in src/text/edit.c. NOT a registry:
 * every consumer is a direct, greppable call, and the ORDER is load-
 * bearing — the LSP consumer must observe pre-op coordinates (s46 §5),
 * so it may only ever be called from _pre.                              */
void yew_edit_notify_pre (EditCtx *ec, u8 kind, ByteOff at, u64 len);
void yew_edit_notify_post(EditCtx *ec, u8 kind, ByteOff at, u64 len);
```

`_pre` calls, in order: `yew_symidx_note_pre` (s44, no-op stub here),
`yew_lsp_note_edit` (s46; compiled to the `src/mod/lsp/shim.c` no-op when
`MODULES` lacks `lsp`). `_post` calls, in order:
`yew_syn_edit(&buf->syn, lo, removed, inserted)` (s39 — the line-damage
record its Assumption (b) asked the choke point to publish; the triple is
computed here from `yew_textbuf_line_of` on the pre- and post-edit offsets,
exactly as s39 §5 says it needs), then `yew_shadow_on_edit` (this sprint),
then `yew_symidx_note_post` (s44 stub). Stubs are empty functions, not
`YEW_BUG` — invariant 3 is about *user-reachable* surfaces, and a hook with
no consumer yet is not one. Fletch's `buf.change` hook (s34) is a different
thing at a different layer: it is **coalesced per input burst** and carries
arbitrary user code, so it stays where s34 put it and never joins this list.

`yew_shadow_on_edit` raises `seq_min[]` for all providers, then applies §8's
interaction table. Pitfall: it must **not** dismiss unconditionally, or
typing the ghost's own prefix kills the ghost on the first keystroke —
which is the single most common way this feature ships broken.

### 4. Debounce policy

```c
void yew_shadow_arm(Ed *ed, Win *w);      /* schedule; idempotent        */
void yew_shadow_fire(Ed *ed, Win *w);     /* timer callback              */
```

One timer per window, not per provider: the timer fires at the **shortest**
armed debounce and each provider is asked only once its own delay has
elapsed since the last edit. Table (options in parentheses land in s36):

| Provider | Debounce | Cancel on new keystroke | In flight per buffer | Why |
|---|---|---|---|---|
| `index` (s44) | **0 ms**, synchronous | n/a | n/a | budget is 0.5 ms (s44 §3); a debounce would make a local operation feel remote |
| `lsp` (s47) | **120 ms** (`shadow.lsp_debounce_ms`) | yes — `$/cancelRequest` (s45) | 1 | below the 150 ms "instant" threshold, above a fast typist's 60–90 ms inter-key gap, so a burst produces one request |
| `ai` (s49) | **350 ms** (`shadow.ai_debounce_ms`) | yes — kill the stream | 1 **globally** | a token stream costs money and watts; 350 ms is the idle point where the user has stopped composing |

Additional gates, all pinned:

- Never armed when `w->cset.curs.len > 1`, when any cursor has a selection,
  when the completion menu is open (§9), during a macro replay
  (`YEW_SRC_REPLAY`), or in `--batch`.
- Never armed when the cursor is not at a line's logical end **and** the
  byte at the cursor is not whitespace — a ghost in the middle of a word the
  user is editing is noise. Option `shadow.midline` (default false).
- `yew_shadow_arm` is idempotent: re-arming cancels and re-sets the timer,
  so a 40-keystroke burst arms once and fires once.

### 5. Layout: virtual lines **overlay**, never push — `src/ui/shadowdraw.c`

```c
#define YEW_SHADOW_MAX_LINES 8       /* option shadow.max_lines; 1 = inline only */

typedef struct ShadowLayout {
    Rect  inline_run;        /* first ghost line: after the cursor cell   */
    Rect  vrows;             /* rows 2..N, or h == 0                      */
    u16   nlines;            /* ghost lines actually drawn                */
    bool  clipped;           /* the ghost did not fit; §5 rule 4          */
} ShadowLayout;

void yew_shadow_layout(const Win *w, const Shadow *sh, ShadowLayout *out);
void yew_shadow_draw  (Ed *ed, Win *w, const ShadowLayout *l, Grid *g);
```

**The law: ghost lines overlay the screen rows below the cursor's row; they
never displace real text.** Rules:

1. Ghost line 1 is drawn inline, starting at the cursor's cell, over
   whatever the rest of that row would have shown (real text after the
   cursor is hidden while the ghost is live, not shifted).
2. Ghost lines 2..N are drawn **over** screen rows `cursor_row+1 ..
   cursor_row+N-1`. The document lines that occupy those rows are simply
   not drawn this frame. `Viewport.top` / `top_sub` are untouched.
3. `N = min(ghost line count, YEW_SHADOW_MAX_LINES, rows_below_cursor)`.
   When the ghost has more lines than fit, `clipped = true` and the last
   drawn row ends with a themed `…` in the provider's role.
4. Damage is exactly `[cursor_row, cursor_row + N)` plus the gutter cells
   for those rows. Dismiss/accept damages the same range, restoring the real
   text on the next frame.

**Why overlay and not push (the pitfall).** A pushing layout re-flows every
row below the cursor each time the suggestion's line count changes. AI
providers *stream*: a suggestion grows from one line to six over ~400 ms, so
a pushing layout moves the text under the user's eyes four or five times per
suggestion, and every time they glance down to read a real line, it has
moved. Worse, pushing interacts with `yew_vp_follow`: a ghost that grows past
the viewport bottom either scrolls the document (the viewport *jumps*, and
the cursor's screen row changes while the user is typing) or is truncated by
a scroll the user did not ask for. Overlay makes the ghost's cost exactly
zero screen rows of movement: every real line stays at the screen row it had
before the ghost appeared, for the ghost's whole life. The price — some real
lines are temporarily hidden — is bounded by `YEW_SHADOW_MAX_LINES`,
reversible by one Esc, and visible (the overlaid rows are dimmed and carry
the provenance glyph in the gutter, §6).

Further pinned consequences:

- `yew_vp_follow` and `yew_vp_scroll` are **never** called by shadow code.
  Grep gate in DoD.
- Ghost cells register no clickable region. The overlaid rows are covered
  with `YEW_REGION_BLOCK` so a click lands on the ghost's rectangle rather
  than on a document line that is not visually there (s22's law: hit-testing
  consumes the same `Rect` drawing did).
- The ghost is drawn **last** in the frame, after document text, gutter and
  overlays, so `yew_region_hit`'s last-added-wins ordering is correct.
- A ghost line wider than the content area is clipped with
  `yew_str_clip` — never wrapped, even when `Viewport.wrap` is on. Wrapping
  a ghost would change the real text's row assignment, which is rule 2.

### 6. Provenance styling and the gutter sign API

The user must always know *who* is suggesting. Two independent channels, so
the answer survives a monochrome terminal:

| Provider | Theme role | `attrs` (s05 bits) | Gutter cell 1 |
|---|---|---|---|
| `index` | `shadow.index` | `dim` | `s` |
| `lsp` | `shadow.lsp` | `dim` \| `italic` | `l` |
| `ai` | `shadow.ai` | `dim` \| `italic` \| `underline` | `a` |

Rationale: `dim` is common to all three (it is what says *this is not your
text*), and the added bits are strictly nested so the three are ordered by
"how far from your buffer this came from". The gutter letter is ASCII,
lowercase, exactly one cell, drawn only on the cursor's row and on overlaid
virtual rows.

s15 reserved a 2-cell sign column and left it empty, naming Sprint 46
(diagnostics) and Sprint 53 (git hunks). Three consumers and two cells needs
arbitration, and this sprint is first to need one, so the API lands here:

```c
typedef enum { YEW_SIGN_DIAG = 0,   /* cell 0, s46 — highest priority    */
               YEW_SIGN_GIT,        /* cell 0, s53 — when no diagnostic  */
               YEW_SIGN_SHADOW,     /* cell 1, this sprint               */
               YEW_SIGN_NKIND } SignKind;

typedef struct GutterSign {
    const u8 *glyph; u8 nbytes;    /* exactly ONE cell wide — asserted   */
    const char *role;              /* theme role name                    */
    u8  attrs;
} GutterSign;

/* Per-line, per-kind. Cleared by yew_gutter_signs_clear at frame begin
 * for the rendered line range only.                                     */
void yew_gutter_sign_set(Win *w, LineNo l, SignKind k, const GutterSign *s);
void yew_gutter_signs_clear(Win *w, LineNo lo, LineNo hi);
```

Cell assignment is fixed by `SignKind`, not by arrival order — a git hunk
never displaces a diagnostic, a diagnostic never displaces the shadow
marker, and the column is therefore stable frame to frame (invariant 5).

**Pitfall — a sign glyph must be exactly one cell.** The default glyphs are
ASCII, but s41's themes may override them, and a theme picking an
EAW-Ambiguous glyph renders 1 cell under `ambiguous_wide = false` and 2
cells under `true`, silently smearing the gutter. `yew_gutter_sign_set`
asserts `yew_cluster_width(s->glyph, s->nbytes) == 1` and falls back to the
built-in ASCII glyph with one `yew_log(WARN)` per (theme, kind).

### 7. Acceptance — word, line, all

```c
bool yew_shadow_accept_word(Ed *ed, Win *w, bool alt);
bool yew_shadow_accept_line(Ed *ed, Win *w);
bool yew_shadow_accept_all (Ed *ed, Win *w);
void yew_shadow_dismiss    (Ed *ed, Win *w);
```

Every one of the three follows the same four steps, and there is exactly one
place in the tree that computes them:

```c
static bool shadow_accept_n(Ed *ed, Win *w, u64 nbytes) {
    Shadow *sh = &w->shadow;
    ByteOff cur = w->cset.curs.data[w->cset.primary].pos;
    i64 done = shadow_revalidate(w->buf->tb, &sh->sug, cur);   /* §2       */
    if (done < 0) { yew_shadow_dismiss(ed, w);
                    yew_msg(ed, YEW_MSG_INFO, "suggestion is stale");
                    return false; }
    u64 lo = (u64)done, hi = lo + nbytes;
    if (hi > sh->sug.len) hi = sh->sug.len;
    if (hi <= lo) return false;
    /* Insert through the choke point; one transaction; NOT mergeable with
     * the surrounding typing burst, so undo takes the accept back whole. */
    yew_undo_begin(w->buf->undo, YEW_TXN_PASTE);
    yew_edit_insert(&w->buf->ec, cur, sh->sug.text + lo, hi - lo);
    yew_undo_end(w->buf->undo);
    if (hi == sh->sug.len) yew_shadow_dismiss(ed, w);   /* fully consumed */
    return true;
}
```

- **accept-all**: `nbytes = sug.len - done`.
- **accept-line**: bytes up to and including the first `\n` at or after
  `done`; if none, the remainder (accept-line on a one-line ghost == all).
- **accept-word**: `nbytes` comes from the **s16 unit engines**, so the unit
  the user gets matches the unit their mode's arrows move by. The ghost text
  is not in a `TextBuf`, so the suggestion carries a scratch one, built
  once per suggestion and cached in `sug.scratch`:

```c
/* Granularity = the current mode's engine, with ONE exception: I mode's
 * engine is `char`, and accepting one grapheme is not a feature, so I mode
 * (and E mode) borrow yew_unit_word explicitly. `alt` selects the engine's
 * alternate granularity — in W mode that is WORD (whitespace-delimited). */
static u64 shadow_word_len(Ed *ed, ShadowSug *sug, u64 from, bool alt) {
    if (!sug->scratch) sug->scratch = yew_textbuf_from_bytes(sug->text,
                                                             sug->len);
    const UnitOps *u = (ed->mode == YEW_MODE_I || ed->mode == YEW_MODE_E)
                     ? &yew_unit_word : yew_unit_of_mode(ed->mode);
    UnitCtx ctx = { .tb = sug->scratch, .buf = NULL, .win = NULL };
    ByteOff n = u->next(&ctx, BYTEOFF(from), alt);   /* s16 rule 2: n > from */
    return n.v - from;
}
```

`yew_unit_*` engines are pure and take `win = NULL` (s16 §1 rule 4), so
running them over a scratch buffer is exactly the supported use.

### 8. I-mode interaction rules

Applied by `yew_shadow_on_edit` (§3) and by the dispatcher for non-editing
commands. Every row is a unit test.

| Event | Effect on the ghost |
|---|---|
| Insert of `n` bytes at the cursor, and `text[consumed .. consumed+n)` equals them | **kept and shortened**: `consumed += n`; the rendered ghost is `text[consumed..]`; the timer is *not* re-armed |
| …and `consumed == len` afterwards | dismissed (the ghost was fully typed out) — no message |
| Insert at the cursor that does not match | dismissed; debounce re-armed |
| Insert anywhere other than the cursor | dismissed; re-armed |
| Any delete, including backspace | **dismissed**, always. Pinned: never un-shorten. A ghost that re-grows as you delete reappears over text you are removing, and the next accept re-inserts what you just deleted |
| Cursor motion (any `ed.move.*`) | dismissed; re-armed if the new position qualifies (§4) |
| Mode change, undo, redo, paste, macro replay, save | dismissed; not re-armed until the next edit |
| Completion menu opens (s44) | dismissed; §9 |
| `<esc>` while a ghost is live | consumed: dismisses the ghost and **does not leave I mode**. A second `<esc>` leaves I mode. Pinned two-stage rule — a single Esc that both dismisses and exits makes the ghost feel like a trap |
| Window loses focus / buffer switch | dismissed |

### 9. Commands, bindings, and the menu-vs-ghost arbitration

s13's command-domain set is closed and adding one is a review event. This
sprint takes that event and adds the domain **`shadow`**, because these
commands belong to neither `edit` (they are not edits until accepted) nor
`ui` (they are model state), and routing them through `ed.ai.*` would be a
lie for two of the three providers.

| Command | Flags | Default binding (I/L/W/B layers) |
|---|---|---|
| `ed.shadow.accept_word` | `RECORDABLE \| NEEDS_WIN \| CHANGES_BUFFER` | `A-<right>` |
| `ed.shadow.accept_word_alt` | same | `S-A-<right>` |
| `ed.shadow.accept_line` | same | `A-<down>` |
| `ed.shadow.accept_all` | same | `A-<cr>` |
| `ed.shadow.dismiss` | `NEEDS_WIN` | `<esc>` (I layer, §8) |
| `ed.shadow.next` / `.prev` | `NEEDS_WIN` | `A-]` / `A-[` — cycle among providers that answered |
| `ed.shadow.toggle` | none | unbound; `:shadow.toggle` |

**The arbitration rule (pinned here so Sprint 44 cannot re-decide it):**

> The completion **menu** appears only on an explicit trigger. **Ghost text**
> appears only passively. The two never render in the same frame.

Mechanically: `yew_compl_open` (s44) calls `yew_shadow_dismiss` first and
sets `w->shadow.suppressed = true`; `yew_compl_close` clears it and calls
`yew_shadow_arm` — a *fresh* request with a new `seq`, never a restore of
the old ghost, because the buffer may have changed while the menu was up.
`yew_shadow_draw` asserts `!w->compl.open` in debug builds, and one unit
test drives both surfaces at once to prove the assert fires.

### 10. Options and instrumentation

| Option | Default | Effect |
|---|---|---|
| `shadow.enable` | `true` | master switch; false unregisters nothing, just never arms |
| `shadow.providers` | `"index lsp ai"` | ordered preference for `ed.shadow.next` |
| `shadow.max_lines` | `8` | §5; `1` = inline only |
| `shadow.midline` | `false` | §4 |
| `shadow.lsp_debounce_ms` | `120` | §4 |
| `shadow.ai_debounce_ms` | `350` | §4 |

Counters on `Ed` for the perf harness and `:shadow.stats`: `requests`,
`delivered`, `dropped_stale`, `dropped_gen`, `accepted_word/line/all`,
`revalidate_fail`. `dropped_stale` climbing while `delivered` stays flat is
the signature of a mis-set debounce, and it is the number Sprint 49 tunes
against.

### 11. Defer

- The symbol-index provider and the completion menu → **Sprint 44**.
  `yew_shadow_register` for `YEW_SHADOW_INDEX` is not called; the slot is
  NULL and the engine skips it without a branch outside `shadow_fire`.
- The LSP provider → **Sprint 47**; the AI provider → **Sprint 49**.
  `:shadow.stats` lists unregistered providers as `— (Sprint 47)`.
- Multi-cursor ghosts → **never for 1.0**: a suggestion per cursor is N
  independent staleness problems. `yew_shadow_arm` returns early when
  `curs.len > 1`, documented in `shadow.h`, not stubbed.
- Ghost text in the E-mode prompt → **never**: s18's law is that there is
  exactly one text editor in this program, and it is not this surface.
- Themes for `shadow.*` roles → Sprints 39/41; this sprint uses the theme's
  default fg with the §6 attrs when a role is missing.
- Persisting a ghost across a restart → never (it is display state).

## Testing Strategy

- **Unit** (`tests/unit/test_shadow.c`): the §8 interaction table, one
  assertion per row, driven through a fake provider; `shadow_revalidate`
  over ≥ 20 cases (exact prefix, one-byte divergence at every offset, cursor
  before `pos`, cursor past `len`, a ghost whose prefix straddles three
  pieces after interleaved inserts, a ghost containing an invalid byte, a
  ghost containing a ZWJ family typed one byte at a time); accept-word on
  `fooBarBaz`/`漢字テスト`/`👨‍👩‍👧‍👦x` in W, B, L and I modes with `alt` both ways.
- **Unit — staleness** (`test_shadow_gen.c`): deliver a suggestion whose
  `seq < seq_min` → dropped, buffer byte-identical; deliver a suggestion for
  another `buf_id` → dropped; edit-then-deliver-then-accept →
  `shadow_revalidate` fails, zero bytes inserted, `yew_undo_current`
  unchanged. **This test is the sprint's reason to exist**; label it so.
- **Unit — layout** (`test_shadow_layout.c`): `yew_shadow_layout` against a
  hand-computed table of (ghost lines, cursor row, viewport rows) →
  `nlines`, `clipped`, damage range; a 40-line ghost clamps to 8; a ghost at
  the last visible row draws 1 line; the damage range covers exactly the
  drawn rows and nothing else.
- **Unit — gutter** (`test_gutter_sign.c`): kind→cell assignment for all
  seven combinations of the three kinds; a 2-cell override glyph is rejected
  and logged once; clear/set across a scrolled line range.
- **Pty goldens** (`tests/pty/shadow_*.golden`): ghost appears after the
  debounce and is dim; the three provenance styles at all three colour tiers
  (truecolor / 256 / 16) — the 16-colour golden is the one that proves the
  attr channel carries provenance without colour; a 4-line ghost overlays
  and **the row of every real line is byte-identical before and after** (the
  no-jump proof); accept-word inserts one word and re-draws the shortened
  ghost; Esc dismisses without leaving I mode; a second Esc leaves it.
- **Determinism**: the same edit script with the same fake-provider replies
  produces byte-identical frames on two runs, and the *same* frames when the
  replies are delivered in a different pump order (the drop rules must not
  depend on arrival order).
- **Fuzz** (`tests/fuzz/fuzz_shadow.c`): random interleavings of edits,
  motions, deliveries with random `seq`/`buf_gen`, and accepts. Invariants
  per op: the buffer only ever changes on an accept; every accepted byte
  range is a prefix of `sug.text` after `consumed`; `yew_textbuf_check`
  passes; every cursor is a grapheme boundary; no accept ever runs with
  `revalidate < 0`.
- **Perf** (`tests/perf/perf_shadow.c`, baselines committed): a live 8-line
  ghost costs ≤ 250 µs of frame time at 200×50; `yew_shadow_on_edit` on a
  matching keystroke ≤ 5 µs; a 100-keystroke burst arms the timer exactly
  once and fires exactly one request per provider.

## Definition of Done

1. `make test` green under gcc and clang with `-std=c11 -pedantic -Wall
   -Wextra -Werror -Wvla`, zero warnings; ASan/UBSan/valgrind lanes clean.
2. The staleness suite: a suggestion delivered after an edit is **never**
   inserted — 0 bytes, in all ≥ 20 `shadow_revalidate` cases and in 4 × 50k
   fuzz iterations.
3. `grep -rn 'yew_textbuf_insert\|yew_textbuf_delete\|yew_edit_' src/edit/shadow.c
   src/ui/shadowdraw.c` shows `yew_edit_insert` in exactly one function
   (`shadow_accept_n`) and nothing in the draw file.
4. `grep -rn 'yew_vp_follow\|yew_vp_scroll\|yew_vp_center'
   src/edit/shadow.c src/ui/shadowdraw.c` → empty (the no-jump law).
5. Overlay proof: the pty golden with a 4-line ghost has every non-ghost
   row byte-identical to the same frame with `shadow.enable=false`.
6. All 12 rows of the §8 interaction table have a firing assertion,
   including both delete rows and both Esc stages.
7. Accept-word matches `yew_unit_of_mode` for L/W/B and `yew_unit_word` for
   I/E, on all three CJK/emoji/subword fixtures, `alt` both ways.
8. Provenance is legible without colour: the 16-colour pty goldens for the
   three providers differ, and the gutter letters `s`/`l`/`a` appear in cell
   1 of the sign column with the diagnostic/git cells untouched.
9. Debounce: a scripted 100-keystroke burst yields exactly 1 timer arm and
   ≤ 1 request per provider; `dropped_stale` is 0 in that run.
10. `yew_edit_notify_pre/post` exist with the §3 call lists in the pinned
    order; `grep -c 'yew_edit_notify' src/text/edit.c` == 2; no callback
    registry exists (`grep -rn 'notify_register\|on_edit_add' src/` empty).
11. Menu/ghost exclusivity: the debug assert fires under a test that opens
    both, and `shadow.suppressed` is proven set/cleared by `yew_compl_*`
    stubs registered for the test.
12. Unregistered providers are visibly deferred: `:shadow.stats` prints
    `lsp — (Sprint 47)` and `ai — (Sprint 49)`; no provider slot is a
    silent no-op.
13. Perf baselines committed under `tests/perf/baselines/shadow.txt`; the
    8-line-ghost frame cost is within budget on the dev machine.
14. ≥ 130 assertions across the four new unit files; determinism lane green
    on the new goldens.
