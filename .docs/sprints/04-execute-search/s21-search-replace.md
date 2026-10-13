# Sprint 21: Search, Replace, Jumplist

## Prerequisites

- Sprint 20 — `yew_re_compile`, `yew_re_search`/`_back`/`_match_at`,
  `YewReMatch`, `YewReErr` (pattern offset for caret rendering), the
  `YewReInput` view over `TextBuf`. All matching in this sprint goes
  through that API; no pattern logic is re-implemented here.
- Sprint 18 — the `CmdLine` widget (one-line `TextBuf` + `Cursor`), history
  files, `CmdErr` rendering with the offending token highlighted, the
  command grammar and `CmdRange`/`YewCmdInvoke`. The `/` and `?` prompts
  are `YEW_PROMPT_SEARCH_F/B` instances of that widget — **not a new
  widget** (the s18 law: there is exactly one text editor in this program).
- Sprint 9 — `Cursor`, `MarkSet` with the bias table, `yew_marks_adjust`
  from the single edit choke point. The jumplist stores marks.
- Sprint 10 — undo tree and **transactions** (`yew_undo_begin/commit/abort`).
- Sprint 12 — registers (the `/` register holds the last pattern).
- Sprint 13 — the named-command registry; Sprint 15 — viewport, scrolling,
  statusline, message line, and the damage plumbing this sprint's overlay
  feeds; Sprint 16/17 — word engine (`*` = search word under cursor) and
  selections.
- Sprint 19 — jobs (`:!rg …` is 1.0's multi-file search answer, §7).
- Binding: `00-decisions.md` — keypress→paint p99 ≤ 5 ms is a **CI gate**
  (invariant 4), deterministic rendering (invariant 5), modal first
  (invariant 9). `01-architecture.md` — `src/search/searchui.c`.

Assumption beyond earlier index entries: Sprint 15's window exposes
`yew_win_damage_lines(Win *w, LineNo lo, LineNo hi)` and an overlay slot the
draw pass consults. If Sprint 15 landed only whole-window damage, the
line-range damage API lands here — the latency gate in DoD 9 is unreachable
without it.

## Goals

Land the search and replace surface on top of Sprint 20's engine:
incremental search-as-you-type with live preview and exact restore on
cancel, direction and wrap semantics, pinned smartcase, viewport-scoped
match highlighting through a damage-friendly overlay that never scans a
whole file on a keystroke, replace with per-match confirmation where the
entire run is one undo transaction, replacement templates with capture
references and case handling, search history and register integration, and
the jumplist/changelist with their motions and coalescing rules.
**Persistence of the jumplist and changelist into workspace state is
deferred to Sprint 25** — this sprint lands the serializer against the
frozen schema shape and writes nothing to disk. An in-editor multi-file
project search is not a 1.0 feature (`:!rg` via Sprint 19 is); `:g/re/cmd`
is Fletch's query API in Sprint 34, never a bespoke mini-language.

## Deliverables

### 1. Search state and the incremental prompt — `src/search/searchui.h|c`

```c
typedef struct SearchState {
    YewRe   *re;            /* last successfully compiled program        */
    char    *pat;           /* its source text, arena-owned              */
    bool     reverse;       /* the direction the search was started with */
    u32      pat_gen;       /* bumped per recompile; overlay cache key   */
    /* live-preview restore point — captured at prompt open              */
    Cursor   save_cur;   LineNo save_top;   GCol save_goal;
    ByteOff  origin;        /* where the search started from             */
    bool     wrapped;       /* last step wrapped; drives the indicator   */
    YewReErr err;           /* live compile error, err.msg NULL = ok     */
} SearchState;

void yew_search_open(Ed *ed, Win *w, bool reverse);   /* '/' or '?'      */
void yew_search_input(Ed *ed, Win *w);                /* on prompt edit  */
void yew_search_accept(Ed *ed, Win *w);
void yew_search_cancel(Ed *ed, Win *w);
bool yew_search_step(Ed *ed, Win *w, bool forward, u32 count);  /* n / N */
```

Search-as-you-type, per prompt edit:

1. Recompile the prompt text (`yew_re_compile` into a per-keystroke arena;
   compilation of a typical pattern is microseconds). On failure keep the
   **last good** `re` for highlighting and show the error in the message
   line with the caret positioned at `err.off` inside the prompt (reusing
   s18's token-highlight renderer). A half-typed `[a-` must never blank the
   screen or beep.
2. Find the first match at/after `origin` (or before, for `?`), move the
   cursor and scroll the viewport to it — the **preview**.
3. Refresh the visible-match overlay (§2) for the new viewport.

**Restore on cancel is exact.** Esc restores `save_cur` (including
`goal_col`), the viewport's top line `save_top`, and the overlay state.
Pitfall: restoring only the cursor leaves the window scrolled somewhere
else — the user pressed `/`, looked, changed their mind, and lost their
place anyway. That is the bug this rule exists to prevent; the pty golden
compares the full grid before and after a cancel and requires byte
equality.

Accept commits: pattern → `history/search` (s18 §3) and → register `/`
(s12), cursor at the match start, a jumplist entry pushed for `origin`
(§5), and the overlay stays live until the pattern is cleared
(`ed.search.clear_highlight`, and the `hlsearch` option).

### 2. Direction, wrap, counts, smartcase

- `ed.search.next` (`n`) repeats in the search's original direction;
  `ed.search.prev` (`N`) repeats in the opposite direction **relative to
  the search**, not absolute. Pinned: after `?foo`, `n` goes backwards.
- A count repeats the step (`3n`); each intermediate step counts for wrap
  reporting but only the final position pushes nothing (n/N do **not**
  push jumplist entries — §5).
- Wrap: with `wrapscan` on (default) a search that reaches the end
  continues at the other end and the message line shows
  `search hit BOTTOM, continuing at TOP` (and the mirror for the other
  direction); a wrap indicator glyph appears in the statusline for 2 s via
  the Sprint 15 timer heap. With `wrapscan` off the message is
  `search hit BOTTOM without match` and the cursor does not move.
- Exactly one match, and the cursor is on it: `n` reports `1 match` rather
  than appearing to do nothing — silence here reads as a broken keybinding.
- No match at all: `pattern not found: foo`, cursor unmoved. During
  incremental typing this is a message-line state, not an error dialog, and
  the previously previewed position is retained until a match exists again.

Smartcase table (pinned; `ignorecase` and `smartcase` are options,
defaults `off`/`on` — i.e. exact-case by default, and `\c`/`\C` in the
pattern force ignore/match case):

| `ignorecase` | `smartcase` | pattern has an uppercase literal | effective |
|---|---|---|---|
| off | any | any | case-**sensitive** |
| on | off | any | case-insensitive |
| on | on | no | case-insensitive |
| on | on | yes | case-sensitive |
| any | any | pattern contains `\c` | case-insensitive (wins) |
| any | any | pattern contains `\C` | case-sensitive (wins) |

**Pitfall — what counts as "has an uppercase literal".** Only *literal*
codepoints outside escapes count. `\W`, `\S`, `\D`, `\x41`, `\A`, `\b`, and
POSIX names like `[[:upper:]]` must **not** flip the search to
case-sensitive; a user searching `\Wfoo` is not asking for case
sensitivity. The check runs over the parsed literal set, not over the
pattern string — a `strpbrk` for uppercase bytes is the wrong
implementation and its test case is `\Wfoo` matching `Foo`.

### 3. Match highlighting — a damage-friendly, viewport-scoped overlay

```c
typedef struct MatchOverlay {
    Vec_Span   spans;       /* buffer byte spans, ascending, disjoint    */
    Span       scanned;     /* the byte range these spans are valid for  */
    u32        pat_gen, buf_gen;   /* invalidation keys                  */
    i32        cur_index;   /* the match the cursor is on, or -1         */
    u32        count_total; /* see the counting rule below               */
    bool       count_capped;
} MatchOverlay;

void yew_overlay_refresh(Ed *ed, Win *w, i64 budget_us);
```

Rules, all pinned:

- **Scan scope is the viewport plus a bounded look-ahead**: from the start
  of the first visible line to the end of the last visible line, extended
  by `YEW_SEARCH_LOOKAHEAD` = min(2 viewport heights, 64 KiB) in each
  direction, so a one-screen scroll usually reuses `scanned` instead of
  rescanning. A whole-file scan on a keystroke is forbidden — on a 1 GB
  file it is seconds, and the keypress→paint budget is **5 ms p99**
  (invariant 4). This is the sprint's load-bearing performance decision.
- Per-keystroke scan budget: `budget_us` = 1000 µs (or 256 KiB of input,
  whichever comes first). If the budget is exhausted, the overlay keeps the
  spans found so far, marks itself incomplete, and resumes on the idle
  timer (Sprint 15's timer heap, 16 ms tick) — partial highlighting for one
  frame is invisible; a stalled keystroke is not.
- Invalidation: any change to `pat_gen` (pattern edited) or `buf_gen`
  (s07's `TextBuf.gen`) drops `scanned`. A scroll that leaves `scanned`
  keeps the spans and scans only the newly exposed strip.
- Damage: `yew_overlay_refresh` calls `yew_win_damage_lines` for exactly
  the lines whose highlight set changed — computed by diffing the old and
  new span lists, not by damaging the viewport. A keystroke in the search
  prompt that adds one character to the pattern typically re-damages a
  handful of lines; the pty test asserts the damaged-cell count.
- Two styles: `search_match` for all matches, `search_current` for the one
  under the cursor (`cur_index`). Wide glyphs and clusters are handled by
  the draw pass (s05/s22 rules); the overlay speaks byte spans only, and
  the renderer converts — no width math in `src/search/`.
- **Match counting (`[3/17]` in the statusline).** Counting *all* matches
  is a whole-file scan by definition, so it is bounded: counting runs on
  the idle timer with a cap of `YEW_SEARCH_COUNT_MAX` = 10 000 matches or
  50 ms of accumulated work, whichever first; over the cap the statusline
  shows `[3/10000+]` and stops. Pinned, because an unbounded counter is the
  exact feature that makes big-file editors feel broken.

### 4. Replace

Surface: `:[range]s/pat/rep/flags` (parsed by Sprint 18's grammar, with the
substitution body taken as one opaque argument and split here on the
delimiter — any non-alphanumeric, non-backslash delimiter is allowed:
`:s#a#b#`), plus `ed.search.replace` for keybindings and Fletch.

| Flag | Meaning |
|---|---|
| `g` | all matches per line, not just the first |
| `c` | confirm each match |
| `n` | count only — report `N matches on M lines`, change nothing |
| `i` / `I` | force ignore-case / match-case for this call |
| `p` | preserve the matched text's case (see the template rules) |
| `e` | no error when the pattern does not match (for scripts) |

Empty pattern (`:s//rep/`) reuses register `/` — the last search pattern.

Confirm keys (`c` flag), pinned, prompt in the message line
`replace with 'bar'? (y/n/a/q/l/^E/^Y)`:

| Key | Action |
|---|---|
| `y` | replace this match, advance |
| `n` | skip this match, advance |
| `a` | replace this and **all remaining** without asking |
| `q` / Esc | stop; keep what has already been replaced |
| `l` | replace this one ("last") and stop |
| `^E` / `^Y` | scroll the view down/up while deciding (does not answer) |

**One undo transaction, always.** A replace-all is one `yew_undo_begin` →
N edits → one `yew_undo_end`, so a single undo restores the entire
operation. A confirm run is *also* one transaction covering the whole run,
including a run ended with `q`: aborting the interaction commits what the
user already approved as one step. Pinned because the alternative — one
undo step per match — means undoing a 400-match replace takes 400
keystrokes, which is how people lose faith in undo.

Application order: **apply matches back-to-front** (last match first).
Offsets of earlier matches then remain valid without adjustment. Marks and
cursors still adjust through the s09 choke point either way; back-to-front
simply removes an entire class of off-by-N bugs from the replace loop
itself. (Confirm mode still *presents* matches front-to-back; it records
decisions and applies them in reverse at the end of the run — or applies
immediately and re-finds from the current offset. Pinned choice: record and
apply in reverse, so a decision can never invalidate a later match's
offsets mid-run.)

**Zero-width matches**: after a zero-width match, advance by exactly one
grapheme (`yew_grapheme_next`) before searching again, else `s/x*/-/g`
loops forever. But zero-width matches must still *fire*: `:%s/^/> /`
prefixing every line is the canonical case, and it is a golden test.

Replacement template grammar:

| Sequence | Expands to |
|---|---|
| `\0` | the whole match |
| `\1` … `\9` | capture group 1–9 (`\{10}`–`\{31}` for higher groups) |
| `\n` `\t` `\r` | newline, tab, carriage return |
| `\\` | literal backslash |
| `\u` / `\l` | uppercase / lowercase the **next** character |
| `\U` / `\L` … `\E` | uppercase / lowercase until `\E` |
| `&` | **literal `&`** — not the whole match |
| any other `\x` | error `unknown replacement escape '\x'` |

`&` being literal is a deliberate divergence from vim/sed: `&` appears in
real text far more often than it is wanted as a backreference, and `\0`
already covers the need. Documented in the manual as a known difference.

Case operations use the s02 tables (simple mappings, one codepoint in →
one out; the s20 §3 rationale applies identically). The `p` flag infers
case from the matched text instead:

| Matched text shape | Replacement rendered as |
|---|---|
| all uppercase (≥ 2 cased chars) | `\U`-style all uppercase |
| Titlecase (first cased char upper, rest lower) | first cased char uppercased |
| anything else | verbatim |

### 5. Jumplist and changelist — `src/edit/jumplist.h`, `src/edit/jumplist.c`

```c
typedef struct JumpEntry {
    u32     buf_id;
    MarkId  mark;        /* s09 mark, YEW_BIAS_LEFT — survives edits     */
    LineNo  line_hint;   /* display/recovery only, if the buffer closed  */
    u64     stamp_ms;
} JumpEntry;

#define YEW_JUMPLIST_MAX 100
typedef struct JumpList {          /* ring buffer, browser-history model  */
    JumpEntry e[YEW_JUMPLIST_MAX];
    u32 head, len;                 /* head = one past the newest entry    */
    u32 cur;                       /* walk position; == len when "at now" */
} JumpList;                        /* per WINDOW                          */

void yew_jump_push(Win *w, ByteOff from);         /* before a jump        */
bool yew_jump_back(Ed *ed, Win *w, u32 count);
bool yew_jump_fwd (Ed *ed, Win *w, u32 count);

#define YEW_CHANGELIST_MAX 100
typedef struct ChangeList { JumpEntry e[YEW_CHANGELIST_MAX];
                            u32 head, len, cur; } ChangeList; /* per BUFFER */
void yew_change_record(Buffer *b, ByteOff at, i64 now_ms);   /* edit hook  */
bool yew_change_older(Ed *ed, Win *w, u32 count);   /* g; / g,             */
bool yew_change_newer(Ed *ed, Win *w, u32 count);
```

- **Positions are marks, not offsets.** An offset stored before an edit
  above it points at the wrong line afterwards; a mark (s09, LEFT bias)
  is adjusted by the single edit choke point for free. `line_hint` exists
  only for display in a future jump picker and for recovery when the buffer
  has been closed (buf_id + line reopens the file at that line).
- **Jumplist is per window, changelist is per buffer.** Rationale: a
  jumplist is navigation history of a *view* (two panes on the same file
  have independent histories — that is the point of two panes), whereas a
  change is a property of the text and should be reachable from any view of
  it. Pinned; Sprint 25 serializes them at their respective owners.
- Push rules — what counts as a jump:

| Command class | Pushes? |
|---|---|
| search accept (`/`, `?`, `*`, `#`) | yes |
| `n` / `N` repeats | **no** — a repeat run would flood the list |
| goto-line (`:N`, `G`, `gg`) | yes |
| mark jump (`'a`), jumplist/changelist motions themselves | no (the list is the history, not an entry in it) |
| block/paragraph jumps, bracket match `%` (s16) | yes |
| buffer/tab/file switch | yes |
| LSP goto-def / references (Sprint 47) | yes — through this same API |
| any single-line or adjacent-line motion, scrolling | no |

- Dedupe/coalesce, pinned:
  1. Pushing a position on the **same line** as the newest entry replaces
     that entry (keeps the newer column) rather than appending.
  2. Pushing when the newest entry is already this exact position is a
     no-op.
  3. On the **first** `back` of a walk, the current position is pushed so
     that `fwd` can return to it. Missing this is the classic "Ctrl-I
     doesn't come back" bug.
  4. A new push while `cur < len` (i.e. after walking back) **truncates the
     forward tail** — browser-history semantics. Vim rotates instead; we
     choose truncate because rotation makes the list order depend on where
     you were standing, and users cannot predict it.
  5. Entries whose mark has died (buffer closed) are skipped during a walk
     and dropped; a walk that finds nothing left is a no-op with the
     message `no older jump position`.
- Changelist coalescing: consecutive edits on the same line **or** within
  `YEW_CHANGE_COALESCE_MS` = 500 ms of the previous record collapse into
  one entry (position updated to the newest edit). Fed from the same op
  stream that drives marks and the journal (s09's rule: one op stream,
  N consumers — never a parallel notification path).
- Commands: `ed.jump.back`, `ed.jump.fwd`, `ed.jump.list`,
  `ed.change.older`, `ed.change.newer`. Bound in the keymap data, not code.

### 6. History, registers, and word search

- The `/` and `?` prompts use s18's `history/search` file with the same
  dedupe and frozen-stem prefix walk; the shell history stays separate.
- Register `/` holds the last accepted pattern; `ed.search.from_register`
  seeds the prompt from any register (so a yanked identifier can be
  searched without retyping), and `^R /` inside the prompt inserts it.
- `ed.search.word_under_cursor` (`*` / `#`): takes the word under the
  cursor via the Sprint 16 word engine, escapes every regex metacharacter
  in it, wraps it in `\b…\b`, and searches forward/backward. Pinned: the
  escaping is `yew_re_quote()` in `src/search/regex.c` (an engine-side
  helper so exactly one implementation of "escape this literal" exists),
  and it is exercised by a fuzz round-trip: for any literal string s,
  `yew_re_compile(yew_re_quote(s))` matches s and nothing shorter.

### 7. Defer

- **Persistence of jumplist and changelist → Sprint 25.** This sprint
  lands `yew_jumplist_serialize(const JumpList*, Bytebuf*)` /
  `yew_jumplist_deserialize(...)` against the schema shape Sprint 25
  freezes (entries as `{buf_path, line, col, stamp}` — marks are resolved
  to line/col at serialize time, since a mark handle is meaningless across
  processes), and calls neither. Nothing is written to disk in Sprint 21.
- `:g/re/cmd` → **Sprint 34**'s Fletch query API (`buf.find(re)` mapped over
  an action). `:g` hard-errors naming it, with the note that a bespoke
  global-command mini-language is a permanent non-goal (we have a language).
- Multi-file / project search → 1.0's answer is `:!rg …` through Sprint 19's
  job buffer (documented in `--help` and the manual); an in-editor indexed
  project search is post-1.0.
- LSP document-highlight / references highlighting → Sprint 47 (it reuses
  `MatchOverlay`'s span/damage machinery — the overlay type is deliberately
  pattern-agnostic).
- Fuzzy matching in the search prompt → never; that is Sprint 26's finder,
  a different surface with different semantics.
- Named marks (`'a`) as *range addresses* in E mode — the deferral Sprint 18
  named — land here: `ed.mark.set`/`ed.mark.jump` and `'a` addresses resolve
  through `MarkSet` with a per-buffer 26-slot name table.

## Testing Strategy

- **Unit** (`tests/unit/test_search_smartcase.c`): all 6 smartcase rows plus
  the `\W`/`\x41`/`[[:upper:]]`/`\A` non-triggering cases and `\c`/`\C`
  precedence.
- **Unit** (`tests/unit/test_overlay.c`): scan scope equals viewport +
  look-ahead (assert the scanned byte range on a fixture where a full scan
  would find extra matches); scroll reuse; invalidation on
  `pat_gen`/`buf_gen`; budget exhaustion yields partial spans and resumes;
  the damaged-line diff for a one-character pattern change on a 200-line
  viewport; count cap at 10 000.
- **Unit** (`tests/unit/test_replace.c`): ≥ 40 rows (buffer, range, pattern,
  template, flags, expected buffer, expected count) covering
  `g`/`n`/`i`/`I`/`p`/`e`, every template escape, `\U…\E` spanning a
  capture, literal `&`, zero-width `^` prefixing, `s/$/;/`, a CRLF line
  end, back-to-front application with length-changing replacements, and
  undo depth **exactly 1** after each.
- **Unit** (`tests/unit/test_jumplist.c`): every push-rule row; all five
  dedupe/coalesce rules incl. first-back push and forward-tail truncation;
  ring wraparound at 100; dead-mark skipping after a buffer close;
  changelist coalescing on an injected clock; per-window independence (two
  windows on one buffer: separate jumplists, shared changelist).
- **Script** (headless via the Sprint 1 runner, moving to `--batch` in
  Sprint 37): search/replace semantics over fixtures, including a
  10 000-match replace-all asserting one transaction and byte-exact output.
- **Pty** (`tests/pty/search_*.golden`): incremental preview per typed
  character; **cancel restores the grid byte-identically** (full grids
  compared, not just the cursor); a compile error mid-typing (`[a-`) shows
  the caret at `err.off` and keeps the previous highlight; wrap message and
  indicator; `n`/`N` direction after `?`; confirm prompt for each of
  `y n a q l ^E`; highlighting on a line with CJK and a ZWJ emoji family;
  `[3/17]` and the capped `[3/10000+]`.
- **Fuzz** (`tests/fuzz/fuzz_search.c`): random interleavings of edits,
  searches, replaces, and jumps (10 000 ops × 4 seeds) asserting after every
  op that overlay spans are ascending, disjoint, in-buffer, and on grapheme
  boundaries; every live jumplist mark is alive and in range; undo replays
  byte-identically; ASan-clean. Plus §6's `yew_re_quote` round-trip fuzz.
- **Perf** (`tests/perf/search_latency.c`): on the 1 GB s11 fixture every
  search-prompt keystroke (recompile + preview + overlay) stays inside the
  **5 ms p99** keypress→paint gate; `n` through 1 000 matches averages
  ≤ 1 ms; the 10 000-match replace-all on 100 MiB meets a committed
  baseline under `tests/perf/baselines/`.

## Definition of Done

1. `make test` green on gcc and clang, zero warnings; ASan/UBSan/valgrind/
   determinism lanes green.
2. The search prompt is the Sprint 18 widget: s18's grep gate extended to
   `src/search/searchui.c` shows shared `Cursor`/`TextBuf` use and no local
   caret/index arithmetic.
3. Cancel restores exactly: the pty golden's pre-`/` and post-Esc grids are
   byte-identical, including viewport top line and `goal_col` (also
   asserted structurally).
4. All 6 smartcase rows plus the 4 non-triggering escape cases pass;
   `\Wfoo` matches `Foo` under `ignorecase+smartcase`.
5. **No whole-file scan on a keystroke**: the overlay test proves the
   scanned range is bounded by viewport + look-ahead on a fixture whose
   out-of-scope matches a full scan would find; the 1 GB perf test holds
   keypress p99 ≤ 5 ms.
6. Every replace-all and every confirm run (including one ended with `q`)
   is exactly **one** undo transaction — undo-tree depth asserted in ≥ 6
   unit cases including the 10 000-match one.
7. Zero-width handling: `:%s/^/> /` prefixes every line (with and without a
   final newline) and `:%s/x*/-/g` terminates; both are golden tests.
8. All template rows implemented; `&` is literal (explicit test); `p`-flag
   case inference covers all three shapes.
9. Overlay damage: the pty test asserts a one-character pattern change
   damages only the lines whose highlight set changed, against a computed
   expectation rather than a magic number.
10. Jumplist: all push-rule rows and all five dedupe/coalesce rules tested,
    incl. first-back push, forward-tail truncation, wraparound at exactly
    100, and dead-mark skipping; changelist coalescing verified at the
    499/501 ms boundaries and same-line vs different-line.
11. Persistence deferral is real: `grep -rn 'open(\|fopen\|write('
    src/edit/jumplist.c` → empty; the serializer exists, round-trips in
    memory under test, and is called by nobody until Sprint 25.
12. `:g` hard-errors as a permanent non-goal; multi-file search documents
    `:!rg`; `'a` addresses now resolve (the Sprint 18 deferral is closed).
13. Fuzz: 10 000 ops × 4 seeds with all overlay/mark/undo invariants green;
    `yew_re_quote` round-trip over 100 000 random literals.
14. ≥ 160 assertions across the four unit files.
