# Sprint 18: E Mode — The Command Line

## Prerequisites

- Sprint 7/9 — `TextBuf`, `yew_textbuf_insert/delete`, `TextIter`, `Span`;
  `Cursor` with grapheme motion, typed coordinates, `MarkSet`. The prompt
  is a real `TextBuf` and its caret is a real `Cursor` (Deliverable 1).
- Sprint 8 — `FileMeta.realpath`; `yew_xdg_state_dir()`.
- Sprint 13 — key-trie keymaps with mode layers and **the named-command
  registry** (`"ed.*"`, one dispatch surface for keys / E mode / Fletch /
  recorder). This sprint *exposes* that registry; it does not re-declare it.
- Sprint 15 — viewport, statusline, **message line** (errors draw there).
- Sprint 12/16/17 — registers (`^R`), the word engine (`^W`), selections
  (`'<,'>` ranges and the `%s` expansion).
- Binding: `00-decisions.md` (Modes row: E is an action mode, Esc always
  returns to L; invariant 3 no silent stubs; invariant 9 modal first),
  `01-architecture.md` (`src/ui/cmdline.c`, "every action is a named editor
  command … one dispatch surface, no parallel paths").

Assumptions beyond the earlier index entries, per cross-sprint discipline:
(1) s13's registry entry carries per-command metadata slots this sprint
fills in (`argspec`, `range_policy`, `abbrev`) and its invoke path takes a
per-invocation argument block (`YewCmdInvoke`, §6); if s13 landed a
narrower `yew_cmd_invoke(Ed*, const YewCmd*)`, widening it is this sprint's
work and the registry stays singular either way. (2) `yew_ws_root()`
returns the realpath of the startup directory (dir = workspace, locked);
Sprint 25 makes it persistent, the value is constant within a session.

## Goals

Land E mode: a command prompt that is a *view onto the same text engine the
editor uses*, persistent history with dedupe and prefix search, a
source-driven completion engine with a menu widget, and the command grammar
— tokenization, the expansion table, quoting rules, range prefixes, and how
a parsed range reaches a registry command. Shell execution (`:!`) lands in
Sprint 19 and hard-errors here; `/re/` addresses and `:s///` in Sprints
20–21; `:fl` in Sprint 32; the options model behind option-name completion
in Sprint 36 (the *source* ships now, over whatever the provider returns);
named marks in ranges in Sprint 21. Sprint 21's search prompt reuses this
widget verbatim — it does not fork it.

## Deliverables

### 1. The prompt is a `TextBuf` — `src/ui/cmdline.h`, `src/ui/cmdline.c`

```c
typedef enum { YEW_PROMPT_CMD,      /* ':'  — this sprint                */
               YEW_PROMPT_SEARCH_F, /* '/'  — Sprint 21 sets the kind    */
               YEW_PROMPT_SEARCH_B, /* '?'  — Sprint 21                  */
               YEW_PROMPT_INPUT     /* generic one-line ask (rename, …)  */
} YewPromptKind;

typedef struct CmdLine {
    YewPromptKind kind;
    bool      active;
    TextBuf  *buf;         /* one-line scratch TextBuf — the SAME engine  */
    Cursor    cur;         /* s09 Cursor — grapheme-correct by inheritance*/
    CompMenu  menu;        /* Deliverable 4                               */
    HistCur   hist;        /* Deliverable 3                               */
    CmdErr    err;         /* Deliverable 7; err.tok_hi > err.tok_lo = live*/
    u16       scroll;      /* first visible CCol when the line overflows   */
} CmdLine;

void yew_cmdline_open(Ed *ed, YewPromptKind k, const char *seed);
void yew_cmdline_close(Ed *ed, bool accepted);
bool yew_cmdline_key(Ed *ed, const Key *k);   /* returns false = not ours */
void yew_cmdline_draw(Ed *ed, Rect r);        /* bottom row; s15 owns row */
```

**Law — there is exactly one text editor in this program.** The prompt's
text is a `TextBuf`; its caret is a `Cursor`; every motion and edit inside
the prompt is a registry command dispatched through the Sprint 13 keymap's
`YEW_MODE_E` layer, bound to the *same* `ed.move.*` / `ed.ins.*` /
`ed.del.*` commands L and I modes use, with the prompt's `TextBuf` as the
active target. **Pitfall:** a second mini-editor here (`char buf[512]` +
`size_t caret` + hand-rolled left/right) is 40 lines, passes every ASCII
test, and is a second implementation of grapheme motion. They diverge: the
day someone types an emoji family into `:e ` the prompt caret lands
mid-cluster while the document caret does not, and invariant 2 has a hole
nobody looks in. Forbidden; DoD 3 greps for it. Falling out for free (do
not re-implement): `^W` uses s16's word engine, `^R` s12's registers, `^V`
the s13 keymap, prompt undo s10's transactions over the scratch buffer.

Prompt key layer (`YEW_MODE_E`; chords are keymap data, not code):

| Key | Command | Note |
|---|---|---|
| Left/Right | `ed.move.char_prev/next` | grapheme steps, s09 |
| Home/End, `^A`/`^E` | `ed.move.line_home/end` | |
| `^W` | `ed.del.word_prev` | s16 word engine |
| `^U` / `^K` | `ed.del.to_home` / `ed.del.to_end` | |
| Up/Down | `ed.cmdline.hist_prev/next` | prefix search, §3 |
| Tab / Shift-Tab | `ed.cmdline.complete_next/prev` | §4 |
| `^R` *x* | `ed.cmdline.insert_register` | s12 |
| `^V` | literal next key | s13 |
| Enter | `ed.cmdline.accept` | §5–7 |
| Esc | `ed.cmdline.cancel` | menu open → closes menu only |

Newline policy: the prompt is one line. Inserted text (paste, register,
completion) has `\n` and `\r` replaced by a single space at the insertion
boundary — pinned, because a raw newline in a one-line `TextBuf` makes
`yew_textbuf_line_count()` disagree with the widget and every column
computation drifts by a line.

### 2. Entering and leaving E mode

`:` in L/W/B mode opens `YEW_PROMPT_CMD` empty; with an active selection it
opens seeded `'<,'>` with the cursor at end (the selection is why you
reached for the prompt). Esc closes without executing and returns to the
mode that opened the prompt; a second Esc returns to L. Pin both steps —
the statusline chip must never show E after a cancel. The prompt never
blocks the loop: `yew_cmdline_key` returns in O(edit), and completion and
history compaction are budgeted (§3, §4).

### 3. Persistent history — `src/ui/cmdhist.h`, `src/ui/cmdhist.c`

One file per prompt kind under `$XDG_STATE_HOME/yew/history/`:
`cmd`, `search`, `shell` (the last written from Sprint 19).

```c
typedef struct CmdHist CmdHist;                  /* Vec(char*) newest last */
CmdHist *yew_hist_open(const char *kind);        /* reads; never fails hard*/
void     yew_hist_add(CmdHist *h, const char *line);
void     yew_hist_flush(CmdHist *h);             /* append + maybe compact */

typedef struct HistCur { i32 idx; char *stem; char *draft; } HistCur;
const char *yew_hist_prev(CmdHist *h, HistCur *c);  /* NULL at the end    */
const char *yew_hist_next(CmdHist *h, HistCur *c);
```

- Format: one entry per line, UTF-8, `\` and control bytes escaped
  (`\\`, `\n`, `\t`, `\xHH`); unescape on read. Un-unescapable entries are
  dropped with a `yew_log(WARN)` — a corrupt history is never a startup
  failure. Lines longer than 4 KiB are truncated (a pasted blob is not
  history); lines beginning with a space are not recorded (shell
  convention, documented in `--help`).
- **Dedupe:** adding a line equal to an existing entry *moves it to the
  end* (one copy, newest position); equal to the newest entry is a no-op.
  Cap `YEW_HIST_MAX = 1000` per kind, oldest evicted at compaction.
- **Prefix search:** `HistCur.stem` is captured when the *first* Up is
  pressed and frozen for the whole walk; Up steps to the newest older entry
  with that prefix, Down walks back toward `idx == -1`, which holds `draft`
  — the text the user had typed — restored byte-exact. Pin the freeze:
  recomputing the stem from the current line makes Up-Up match the
  *previous suggestion*, which is the "history is broken" bug report.
  Comparison is bytewise (no folding, no normalization: determinism, and
  `:E` and `:e` are different commands).
- **Concurrent yews:** `yew_hist_add` appends with a single
  `O_APPEND | O_CLOEXEC` `write` (atomic for regular files); compaction
  (dedupe + cap) happens at flush via read → dedupe → tmp+rename (the
  Sprint 8 pattern). **Pitfall:** read-modify-rewrite on *every* add loses
  the other process's entries; append-then-compact loses at most ordering.
- `--batch` (Sprint 37) and `--clean` (Sprint 36) get an in-memory history
  and never touch the files.

### 4. Completion engine — `src/ui/cmdcomp.h`, `src/ui/cmdcomp.c`

```c
typedef enum { YEW_COMP_CMD, YEW_COMP_PATH, YEW_COMP_BUFFER,
               YEW_COMP_OPTION, YEW_COMP_VALUE } YewCompKind;

typedef struct { const char *text;   /* replacement text, arena-owned    */
                 const char *detail; /* right-column hint, may be NULL   */
                 u8 kind; bool is_dir; /* is_dir suppresses the space    */
                 i32 score; } CompItem;

typedef struct CompSource { YewCompKind kind;
    u32 (*enumerate)(Ed *ed, const char *stem, Vec_CompItem *out); } CompSource;

typedef struct CompMenu {
    Vec_CompItem items; i32 sel;     /* -1 = closed                      */
    Span replace;                    /* byte range in the prompt buffer  */
    bool cycling; } CompMenu;
```

Which source applies is decided by the **parse state of the line at the
cursor**, never by a heuristic on the last character:

| Cursor is in… | Source | Notes |
|---|---|---|
| token 0 (the command name) | `YEW_COMP_CMD` | registry names minus `ed.`, plus declared abbreviations |
| an argument whose `argspec` char is `f` | `YEW_COMP_PATH` | |
| `argspec` `b` | `YEW_COMP_BUFFER` | buffer names + numbers |
| `argspec` `o` | `YEW_COMP_OPTION` | option names from the provider |
| `argspec` `v` | `YEW_COMP_VALUE` | the option's own value enumerator |
| `argspec` `s` (free string) or unknown command | none | Tab inserts a literal tab |

`argspec` is a per-command string in the registry entry: one char per
positional argument, last char repeats (`"f"` = one path; `"f*"` = many;
`"ov"` = option then value). Commands wanting no completion declare `"s"`.

Ranking: exact-prefix matches first (`yew_sort_stable` by name), then
subsequence matches by `score`. The scorer is one function,
`yew_comp_score(const char *stem, const char *cand)` — prefix length plus
camel/`_`/`/`-boundary bonuses. **Sprint 26 replaces that one function with
the fuss scoring engine**; the signature is the seam, no call site changes.

Path source specifics — where the perf and correctness traps are:

- Split the stem at the last `/`: the head is the directory to read, the
  tail is the filter. `~`/`~user` expand in the head only. Relative heads
  resolve against the workspace root (pinned default — completion stays
  stable while you switch tabs).
- Hidden entries appear only when the filter starts with `.`.
- Use `d_type` from `readdir`; `lstat` **only** when `DT_UNKNOWN`.
  Pitfall: `stat`ing every entry turns a 10 000-file NFS directory into a
  multi-second freeze — this loop runs inside a keystroke.
- Capped at `YEW_COMP_MAX = 500` after ranking (footer shows `500+ of N`),
  sorted with `yew_sort_stable` so the menu is identical on glibc and musl.
- Directories get a trailing `/` and **no** space; files get a space. An
  inserted path containing any of ` \t"'\\$&|;<>()*?[]` is quoted per §5's
  rules — completion output must survive re-tokenization by construction.

Menu and cycling rules (pinned; every row is a pty golden):

| Event | Behavior |
|---|---|
| Tab, 0 matches | menu stays closed; message line `no completions`; line unchanged |
| Tab, 1 match | insert it (+ space or `/`); menu stays closed |
| Tab, N matches, LCP longer than stem | insert the longest common prefix only; menu opens showing all N; nothing selected |
| Tab again (menu open) | select next item and insert it (replacing `menu.replace`) |
| Shift-Tab | select previous; wraps at both ends |
| any printable key | close menu, keep inserted text, re-enumerate on the next Tab |
| Esc | close menu, **restore the stem as it was before the first Tab** |
| Enter with menu open | accept the selected item and close the menu — **does not execute the line** |
| Enter with menu closed | execute |

The last row is not a nicety: executing on the Enter that dismisses a menu
is how a user runs `:w /etc/pas` instead of picking `passwd`.

### 5. Command grammar — `src/ui/cmdparse.c` (with `cmdline.h`)

```
line    := ws* range? ws* name ( ws+ arg )* ws*
name    := [A-Za-z_][A-Za-z0-9_.]*        /* '.' allowed: file.write     */
arg     := ( bare | squoted | dquoted )+  /* adjacent parts concatenate  */
bare    := ( ~[ \t"'] | '\' any )+
squoted := "'" ( ~"'" )* "'"
dquoted := '"' ( ~["\\] | '\' any )* '"'
```

```c
typedef struct CmdArgv { char **v; u32 n; } CmdArgv;   /* arena-owned     */

typedef struct CmdParse {
    CmdRange   range;       /* §6                                        */
    Span       name_tok;    /* byte range in the prompt buffer           */
    CmdArgv    argv;        /* argv[0] = resolved command name           */
    Span      *arg_tok;     /* per-argv source span, for error carets    */
    CmdErr     err;         /* §7                                        */
} CmdParse;

bool yew_cmd_parse(Ed *ed, const char *line, size_t len, Arena *a,
                   CmdParse *out);
```

**Expansion table** — applied during tokenization, per token, *into that
token*:

| Sequence | Expands to | Error when |
|---|---|---|
| `%` | current buffer's path **as the user gave it** | buffer unnamed |
| `%p` | `FileMeta.realpath` of the current buffer | buffer unnamed |
| `%d` | `dirname(%)`, no trailing `/` (except `/`) | buffer unnamed |
| `%h` | `yew_ws_root()` — the workspace root | never |
| `%s` | the primary selection's text (H mode) | no selection |
| `%b` | current buffer's display name (`*jobs*`, `[No Name]`, …) | never |
| `%l` | primary cursor line, 1-based | never |
| `%c` | primary cursor grapheme column, 1-based (`GCol + 1`) | never |
| `%%` | a literal `%` | never |
| `%` + anything else | **error** `unknown expansion '%X'` | always |

Quoting and escaping — pinned, because this is where injection bugs live:

1. **Expansion is post-tokenization and produces exactly one token part.**
   A path containing spaces, quotes, `$`, or a newline stays one `argv`
   element; expansion output is *never* re-scanned for quotes, whitespace,
   or further `%` sequences. One pass, one direction.
2. Single quotes are fully literal — no expansions, no escapes; the only
   way to end one is `'`, and there is no `\'` (`'it''s'` → `it` + `s`).
3. Double quotes expand `%…` and honor `\\`, `\"`, `\%`, `\n`, `\t`; any
   other `\x` is an error, never a silent literal.
4. Bare tokens split on unquoted whitespace; `\ ` escapes a space.
5. Adjacent parts concatenate: `%d"/my file.c"` is one token.
6. An unterminated quote errors at the opening quote's offset; the line is
   neither executed nor discarded.
7. `%` expands inside bare and double-quoted parts, never inside single
   quotes. There is no third rule and no context-sensitive exception.

Worked table (the exact argv a command receives):

| Line | argv |
|---|---|
| `:w %d/backup.txt` | `["file.write", "/home/u/proj/backup.txt"]` |
| `:e "%d/my file.c"` | `["file.open", "/home/u/proj/my file.c"]` |
| `:e '%d/lit.c'` | `["file.open", "%d/lit.c"]` |
| `:set tabwidth 4` | `["set", "tabwidth", "4"]` |
| `:w %` (unnamed buf) | parse error at the `%` token: `buffer has no file name` |

The parsed `CmdArgv` is what registry commands receive. **No command ever
receives the raw line**, and nothing in the editor builds a shell string by
interpolating an expansion — Sprint 19's argv-arrays contract becomes
enforceable here because the tokenizer never hands out an un-split blob.

Name resolution: the E-mode name is the registry name with a leading `ed.`
implied (`:file.write` → `ed.file.write`); entries may declare `abbrev`
strings (`w`, `wq`, `q`). Order: exact abbreviation → exact full name →
**unique** prefix. A non-unique prefix errors listing up to 6 candidates;
first-match-wins is forbidden (it makes `:s` change meaning after a plugin
loads).

### 6. Ranges

```c
typedef enum { YEW_RANGE_NONE, YEW_RANGE_LINES,
               YEW_RANGE_BUFFER, YEW_RANGE_SELECTION } YewRangeKind;

typedef struct CmdRange {
    YewRangeKind kind;
    LineNo lo, hi;      /* inclusive, 0-based internal (display is +1)    */
    bool   given;       /* the user typed one                             */
    Span   tok;         /* source span, for error carets                  */
} CmdRange;
```

Address forms:

| Form | Meaning |
|---|---|
| `N` | absolute display line N (internal `N-1`) |
| `.` | the line of the primary cursor |
| `$` | last line |
| `+N` / `-N` | relative to `.` (bare `+`/`-` = ±1) |
| `addr+N` / `addr-N` | offset from any address |
| `%` | whole buffer — shorthand for `1,$`, `kind = BUFFER` |
| `'<,'>` | the selection's first..last line, `kind = SELECTION` |
| `'a`–`'z` | named mark — **hard-errors naming Sprint 21** |
| `/re/`, `?re?` | pattern address — **hard-errors naming Sprint 21** |

`lo > hi` is an error (`backwards range`), never silently swapped — the
swap makes `:5,3d` delete lines the user did not name. Addresses outside
`[1, line_count]` error naming the offending address's span;
`YEW_RANGE_BUFFER` on an empty buffer is legal and empty.

How a range reaches a command — the registry entry declares a policy and
`yew_cmd_parse` enforces it before dispatch:

| `range_policy` | No range given | Range given |
|---|---|---|
| `YEW_RP_FORBID` | ok, `kind = NONE` | error `:X takes no range` at the range span |
| `YEW_RP_OPT` | `kind = NONE` | passed through |
| `YEW_RP_LINE` | defaults to `.` (one line) | passed through |
| `YEW_RP_BUFFER` | defaults to `1,$` | passed through |
| `YEW_RP_REQUIRED` | error `:X requires a range` | passed through |

```c
typedef struct YewCmdInvoke {
    CmdRange   range;
    CmdArgv    argv;
    i64        count;      /* prefix count from the keymap, 0 = none      */
    bool       bang;       /* trailing '!' on the command name            */
    Win       *win;        /* the window the command acts on              */
} YewCmdInvoke;
```

`YewCmdInvoke` is what s13's dispatch carries; keybindings and Fletch fill
the same struct (empty `argv`, `kind = NONE`) — one shape, one dispatch
surface, and the recorder can serialize it because every field is data.
A command converts `range` to bytes with `yew_range_span(const TextBuf*,
const CmdRange*)` → `Span`: line-start of `lo` .. line-end of `hi`
**including** its EOL (except when `hi` is the last line of a file with no
final newline). Pin the EOL inclusion — a span that excludes the trailing
newline makes `:1,2d` leave a blank line, the most-reported off-by-one in
line-range editors.

### 7. Errors and the offending token

```c
typedef struct CmdErr { u32 tok_lo, tok_hi;    /* prompt byte offsets    */
                        char msg[128]; } CmdErr;
```

On any parse or dispatch error the prompt **stays open** with its text
intact, the cursor moves to `tok_lo`, the token `[tok_lo, tok_hi)` is drawn
in the theme's error style, and the message line (Sprint 15) shows
`E: <msg>`. Pin the stays-open rule — discarding a 60-character command
over one typo is hostile, and retyping is where the *second* mistake comes
from.

Catalogue (exact texts; each is a unit case):

| Condition | Message |
|---|---|
| unknown command | `unknown command 'foo' (try Tab)` |
| ambiguous prefix | `ambiguous: file.write, file.write_all` |
| missing argument | `:file.write requires 1 argument` |
| too many arguments | `:quit takes no arguments` |
| range forbidden | `:quit takes no range` |
| range required | `:sort requires a range` |
| backwards range | `backwards range (5,3)` |
| line out of range | `line 900 past end of buffer (412 lines)` |
| unterminated quote | `unterminated "` |
| unknown escape | `unknown escape '\q'` |
| unknown expansion | `unknown expansion '%z'` |
| unnamed buffer | `buffer has no file name` |
| no selection | `%s needs a selection` |
| deferred surface | `:! runs shell commands: Sprint 19` |

`tok_lo/tok_hi` must always be a valid, non-empty range inside the prompt
buffer — the parse fuzzer (Testing) asserts exactly that on every input.

### 8. Defer

- `:!cmd`, `:r !cmd`, `:'<,'>!filter`, `:jobs` → hard-error naming
  **Sprint 19** (the parser recognizes `!` so the message is precise).
- `/re/` addresses, `:s///`, `:g//` → **Sprint 20** (engine) / **Sprint 21**
  (surface); `:g//` also notes that the global-command surface is Fletch's
  query API (Sprint 34), not a bespoke mini-language.
- `'a`–`'z` mark addresses → **Sprint 21**. `:fl` / `:source` →
  **Sprint 32** / **Sprint 36**.
- Full options model and value enumerators → **Sprint 36**; the
  `YEW_COMP_OPTION` source ships now over whatever the provider has (an
  empty list is data, not a stub).
- Fuzzy ranking → **Sprint 26** (swaps `yew_comp_score` only).
  Per-workspace history → **Sprint 25**; history here is global per user.

## Testing Strategy

- **Unit** (`tests/unit/test_cmdparse.c`): table-driven tokenizer, ≥ 40 rows
  of (line, expected argv, expected range, expected `CmdErr`) covering every
  expansion row, all seven quoting rules, the worked table verbatim,
  adjacency, `%%`, unknown escape/expansion, unterminated quotes at every
  position, and a filename containing a space, a quote, a `$`, and a
  newline (each must survive as one argv element).
- **Unit** (`tests/unit/test_cmdrange.c`): every address form; every
  `range_policy` × given/not-given cell; `lo > hi`; out-of-range; empty
  buffer; last-line-without-final-newline span inclusion.
- **Unit** (`tests/unit/test_cmdhist.c`): dedupe-to-end; leading-space
  suppression; cap eviction; frozen-stem walk (Up Up Down Down returns the
  draft byte-exact); escape round-trip incl. invalid UTF-8; two handles
  appending then compaction preserves both.
- **Unit** (`tests/unit/test_cmdcomp.c`): source selection per `argspec`;
  path completion over a fixture tree (hidden files, dirs, 1200-entry dir →
  cap + `500+ of 1200`); ordering identical across two runs; quoting of a
  completed path with a space; `DT_UNKNOWN` fallback.
- **Pty** (`tests/pty/cmdline_*.golden`): selection-seeded open; one golden
  per Tab-cycling row; Enter-closes-menu-without-executing; error rendering
  with the token highlighted and the cursor on it; a ZWJ emoji family
  navigated with Left/Right; horizontal scroll past the width.
- **Fuzz** (`tests/fuzz/fuzz_cmdparse.c`): random bytes and random
  plausible lines → never crashes, never loops, and on failure
  `0 <= tok_lo < tok_hi <= len`; ASan/UBSan lanes.
- **Perf**: completion in a 10 000-entry directory (enumerate+rank+draw)
  ≤ 5 ms p99 — the Sprint 56 gate inherits the measurement.

## Definition of Done

1. `make test` green on gcc and clang, zero warnings; ASan/UBSan/
   determinism lanes green.
2. Prompt reuse proven: `grep -rn 'utf8_\|grapheme\|width\|wcwidth'
   src/ui/cmdline.c src/ui/cmdcomp.c` returns only calls into
   `src/unicode/` — zero local width or cluster arithmetic.
3. No second editor: `grep -n '\[[a-z_]*caret\|caret++\|caret--'
   src/ui/cmdline.c` → empty, and a unit test walks the `YEW_MODE_E` keymap
   asserting every leaf is a registered `ed.*` name.
4. Expansion table: all 9 rows plus the error row unit-tested; the four
   worked-table lines produce exactly the listed argv.
5. Quoting: a path with a space, `"`, `'`, `$`, and `\n` round-trips
   completion → prompt → argv as **one** element in all three quoting
   forms; ≥ 12 assertions.
6. Range: all 6 `range_policy` rows enforced; the `:1,2d` span includes the
   trailing EOL (explicit case); `'a` and `/re/` name Sprint 21.
7. History: cap, dedupe-to-end, frozen-stem walk, and concurrent-append all
   green; no history I/O under `--batch`/`--clean` (grep + unit).
8. Completion menu: a byte-identical pty golden per cycling row across two
   runs; Enter with an open menu never executes.
9. Errors: all 13 catalogue messages verbatim; the prompt survives each
   with text intact and the cursor at `tok_lo`.
10. Fuzz: 1 000 000 parse inputs, zero crashes, `tok` invariant holds on
    100% of failures.
11. Deferred surfaces (`:!`, `:s`, `:g`, `:fl`, `'a`, `/re/`) each produce
    their exact sprint-naming message — one smoke assertion each.
12. ≥ 120 assertions across the four unit files.
