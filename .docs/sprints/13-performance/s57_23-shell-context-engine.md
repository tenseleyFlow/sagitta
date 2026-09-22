# Sprint 57.23: Shell Completion I — The Context Engine

## Prerequisites

- **Sprint 57.18** — `:!` completion as it stands: `bang_body_start`,
  `bang_word` and `bang_point` (`src/ui/cmdparse.c:1131–1280`) split a bang
  body on unquoted whitespace and report a word index; `yew_comp_kind_for`
  (`src/ui/cmdcomp.c:1908`) maps that index to a source in ONE line:
  `*kind = token_index == 0U ? YEW_COMP_EXEC : YEW_COMP_PATH;`. The
  `YEW_COMP_EXEC` source (`enumerate_exec`, PATH scan, deduplicated by
  basename). `bang_point`'s own comment names pipelines and redirections as
  57.18's deferral; this sprint lifts it.
- **Sprint 57.17** — the pager (`src/ui/complmenu.c`) this sprint fills and
  never replaces; `yew_comp_sole` (`cmdcomp.h`), the one definition of a
  sole survivor.
- **Sprint 18.5** — `CompReq`, `CompSource`, `CompFilter`, the sliced
  `DirListing`, `yew_comp_filter_run`, `yew_comp_lcp`, and the first-Tab
  flow in `complete()` (`src/ui/cmdline.c:1034`): sole survivor inserts,
  otherwise the longest common prefix of the TIERED rows inserts and the
  menu opens, and Tab again enters the list. That flow is already the
  agreed Tab feel; this sprint keeps it and makes its insertion shell-aware.
- **Sprint 19** — `$SHELL -c` execution via `yew_job_shell()`
  (`src/edit/job.c:416`, falls back to `/bin/sh`). Execution is NOT touched
  by this campaign.
- Binding: invariants 2 (bytes), 4 (5 ms keypress), 5 (deterministic
  render), 9, and `00-decisions.md`'s "stable sort only".

## Goals

`:!` completion is one ternary. Word 0 completes from `$PATH` and every
other word completes as a file, so `./scr<Tab>` searches `$PATH` (which
never contains `./`), `$HO<Tab>` completes a filename, `ls | gr<Tab>` treats
`gr` as a file because it is "word 2", and `sudo gi<Tab>`, `FOO=1 cm<Tab>`,
`make && ./bu<Tab>` and `cd <Tab>` all fail the same way. None of that is a
shortage of candidates; the engine does not know where the caret is.

This sprint builds that knowledge: a completion-only lexer for the
sh/bash/zsh family that reports the caret's CONTEXT, a routing rule —
**shape beats position, position refines** — and the generic sources that
rule needs. It fixes every complaint above with zero per-command knowledge.

Campaign (each sprint its own file; later ones are prerequisites-chained):

| Sprint | Lands |
|---|---|
| **57.23** | context engine, routing, generic sources, shell quoting |
| 57.24 | per-command specs as Fletch data, subcommands/flags, async generators, first spec batch |
| 57.25 | `--help` parsing for commands with no spec, on-disk cache, prewarm |
| 57.26 | fish as an optional completion oracle; history autosuggestion ghost |
| 57.27 | *reserved:* persistent shell session (cd/export/aliases survive) |

Deferred and hard-erroring nowhere (they are simply absent): subcommand and
flag knowledge → 57.24; any subprocess spawned for completion → 57.24;
fish-dialect syntax (`(cmd)`, `and`/`or`, `$argv[1]`) → non-goal, `:!`
targets the sh family; case-statement bodies, here-doc bodies, arithmetic
→ named limits of the lexer, see §1.

## Deliverables

### 1. The context lexer — `src/ui/shctx.h`, `src/ui/shctx.c`

A completion-grade lexer. It READS a line and describes the caret; it never
produces anything that is executed. `yew_cmd_parse` keeps handing the body
to `$SHELL -c` verbatim (`finish_bang`), which is what makes pipes, quoting
and redirection work. Say so in the header, in the same words as
`CmdParsePoint.bang_body`'s comment.

```c
typedef enum {
    YEW_SH_POS_COMMAND,   /* the word the shell would execute            */
    YEW_SH_POS_ARGUMENT,  /* an operand of the simple command            */
    YEW_SH_POS_REDIRECT,  /* the target of > >> < <> >| &> &>> n> n<     */
    YEW_SH_POS_ASSIGN,    /* the value side of NAME=<caret>              */
    YEW_SH_POS_VARIABLE,  /* $NA<caret> or ${NA<caret>                   */
    YEW_SH_POS_NONE       /* comment, heredoc delimiter, arithmetic, case */
} YewShPos;

typedef enum {
    YEW_SH_Q_NONE,
    YEW_SH_Q_SINGLE,      /* '...'                                        */
    YEW_SH_Q_DOUBLE,      /* "..."                                        */
    YEW_SH_Q_DOLLAR       /* $'...' (bash, zsh)                           */
} YewShQuote;

typedef struct YewShCtx {
    YewShPos pos;
    YewShQuote quote;     /* quoting state AT the caret                    */
    Span replace;         /* [word start, caret): bytes a completion replaces */
    char *stem;           /* decoded text of `replace`, quotes removed     */
    /*
     * The simple command the caret belongs to, AFTER assignment prefixes
     * and precommand wrappers (§2) are stripped.  argv[0] is the command
     * word; argv[arg_index] is the caret's word (its decoded stem), so
     * argv[1 .. arg_index-1] are the operands already typed.  argc == 0
     * and arg_index == 0 in command position.
     */
    char **argv;
    u32 argc;
    u32 arg_index;
    bool dashdash;        /* an unquoted `--` precedes the caret's word    */
    bool brace_var;       /* VARIABLE opened with `${`                      */
    u32 depth;            /* open $( ` ( <( >( { at the caret, for tests   */
} YewShCtx;

/* All strings live in `a`.  Never fails on any byte sequence: an input it
 * cannot classify yields YEW_SH_POS_NONE, not false.  Returns false only
 * for cursor > len. */
bool yew_shctx_at(const char *line, size_t len, size_t cursor, Arena *a,
                  YewShCtx *out);
```

**The lexer runs from the body start to the CARET, not to the end of the
line.** Text after the caret cannot change what the caret's context is,
and scanning it would let an unterminated quote later in the line
reclassify a word the user is completing. The one exception is the
caret's own word, whose extent is not needed: `replace` is `[start,
caret)`, the same prefix-up-to-caret rule 57.18 established and bash uses.

**Reference: tokens that put the NEXT word in command position.**

| Token | Notes |
|---|---|
| `\|` `\|&` `\|\|` `&&` `;` `&` `;;` `;&` `;;&` | control operators; `&` only when not part of `&>`, `>&`, `&&` |
| `(` `$(` `` ` `` `<(` `>(` | open a nested context; push, command position inside |
| `)` `` ` `` (closing) | pop to the enclosing context, which resumes AFTER the construct (an argument of the outer command) |
| `{` `!` `if` `then` `else` `elif` `do` `while` `until` `time` | reserved words, recognised ONLY in command position |
| `}` `fi` `done` `esac` | reserved words that end a construct; the next word is in command position again only after an operator |

**Reference: constructs the lexer recognises but does not complete in.**

| Construct | Context reported |
|---|---|
| `#` starting an unquoted word | NONE to end of line (a `-c` string is non-interactive, so both bash and zsh honour comments) |
| `<<` `<<-` then a word | the delimiter word: NONE |
| `<<<` then a word | ARGUMENT (a here-string can name `$VAR`s; paths are harmless) |
| `$((` … `))`, `((` … `))` | NONE until closed |
| `case WORD in` … `esac` | NONE from `in` to `esac` — a named limit, one-liners rarely use it |
| `for NAME in` | NAME is NONE; words after `in` are ARGUMENTs of no command (path completion) |
| `[[` … `]]` | `[[` is the command word; its operands are ARGUMENTs |
| `NAME() {` | not recognised; `NAME` and `()` are ordinary words — a named limit |

**Reference: quoting and escapes while scanning.**

| State | Ends at | Escapes honoured | `$` expands |
|---|---|---|---|
| none | whitespace, operator | `\` + any byte | yes |
| `'…'` | next `'` | none | no |
| `"…"` | next unescaped `"` | `\"` `\\` `` \` `` `\$` | yes, `$(` nests |
| `$'…'` | next unescaped `'` | `\'` `\\` and C escapes (decoded only for the stem) | no |

An unterminated quote at the caret is NORMAL — the user is typing inside it
— and sets `quote`. `bang_word`'s decoding rules are correct and are MOVED
here (and extended with `$'…'`), not duplicated; `bang_word` is deleted.

**Redirections.** The operator may be glued to its target (`>out.txt`,
`2>err.log`, `&>all`). A digit run is an fd prefix only when immediately
followed by `<` or `>`. `>&` and `<&` followed by a digit or `-` are fd
duplications and consume nothing further. The word after any other
redirection operator is `YEW_SH_POS_REDIRECT`; redirection words are
REMOVED from `argv` so `cat > out <Tab>` still sees `cat` with no operands.

**Pitfall — the nesting stack is bounded.** `depth` is capped at 64. On
overflow the lexer reports NONE rather than growing: `$(((((…` is a fuzz
input, not a command line, and an unbounded stack is an allocation the 5 ms
budget did not plan for.

**Pitfall — invalid UTF-8.** Scan BYTES. Every operator and quote is ASCII,
so a multibyte sequence can never be mistaken for one, and invalid bytes
pass through into `stem` untouched (invariant 2).

### 2. Simple-command resolution — `src/ui/shctx.c`

After the lexer has the words of the caret's simple command, strip:

1. **Assignment prefixes.** Words matching `^[A-Za-z_][A-Za-z0-9_]*=` before
   the command word. A caret inside one, after the `=`, is
   `YEW_SH_POS_ASSIGN` with `replace` starting after the `=`.
2. **Precommand wrappers.** A built-in table, replaceable by 57.24's specs:

| Wrapper | Flags that consume the next word |
|---|---|
| `sudo`, `doas` | `-u -g -C -h -p -U -r -t -D -R` (sudo); `-u -C` (doas) |
| `env` | `-u -C -S`; also skips `NAME=value` words and `-i`/`-0` |
| `nice` | `-n` |
| `timeout` | `-s -k`; then ONE duration word |
| `xargs` | `-a -d -E -e -I -i -L -l -n -P -s` |
| `nohup` `time` `command` `builtin` `exec` `caffeinate` `unbuffer` | none |

Skipping means: drop the wrapper and its flags (with their arguments), and
the next word is in COMMAND position. `sudo -u root gi<Tab>` completes
executables. **Pitfall:** `command -v NAME` and `builtin NAME` are queries,
not execution, but completing an executable there is still exactly right,
so no special case is needed — do not add one.

### 3. Routing — shape beats position — `src/ui/cmdcomp.c`

One function decides the sources from a `YewShCtx`. The table IS the spec;
the function is its transcription, top row first, first match wins.

| # | Caret's word (decoded stem) | Position | Sources |
|---|---|---|---|
| 1 | any | NONE | nothing (`yew_comp_query` returns false → Tab inserts a literal tab, as today) |
| 2 | any | VARIABLE (not inside `'…'`) | VAR |
| 3 | starts `~`, no `/` yet | any but NONE | USER, inserted as `~name/` |
| 4 | contains `/`, or is `.` / `..` | COMMAND | PATH filtered to executables and directories |
| 5 | contains `/`, or starts `./` `../` `~/` | ARGUMENT, REDIRECT, ASSIGN | PATH |
| 6 | starts `-`, not `dashdash` | ARGUMENT | nothing this sprint (57.24 adds flags) |
| 7 | other | COMMAND | EXEC ∪ BUILTIN |
| 8 | other | ARGUMENT, command in the §4 defaults table | that table's kind |
| 9 | other | ARGUMENT, REDIRECT, ASSIGN | PATH |

Row 4 is the `./scr<Tab>` fix and row 7 plus §1 is the `ls | gr<Tab>` fix.
Row 6 deliberately offers nothing rather than files: a flag stem matching a
file named `-rf` is the one completion that can hurt.

### 4. Generic sources — `src/ui/cmdcomp.c`

- **VAR** (`YEW_COMP_VAR`, appended to `YewCompKind`). Names from the
  environment `:!` commands will run with — `environ` merged with the job
  layer's standard overrides, so a name the child will not see is not
  offered. `detail` is a value preview: first 40 bytes, control bytes
  rendered as `·`, and **redacted** (`•••`) when the NAME contains, case-
  insensitively, one of the fragments the AI redactor's `env-assignment`
  rule already uses (`src/mod/ai/redact.c:42`): `SECRET TOKEN PASSWORD
  PASSWD PRIVATE_KEY API_KEY APIKEY CREDENTIAL`. That list exists today only
  INSIDE a regex string in an excisable module, so: add core
  `bool yew_secret_name(const char *name)` in `src/util/secret.[ch]` (so
  `MODULES=""` still builds), and a unit test that asserts every fragment
  in it appears in the AI rule's pattern, so the two cannot drift. Why: a
  completion pager is screen-shared far more often than it is secret.
  Inside `${`, insertion appends `}`.
- **USER** (`YEW_COMP_USER`): `getpwent`, names only, `detail` = home
  directory. Names starting `_` (macOS daemon accounts) are kept but sort
  after the rest.
- **BUILTIN**: the sh-family builtins and reserved words as static items
  with `detail` `"builtin"` or `"keyword"`, merged into command position
  (row 7). List: `alias bg break builtin cd command continue declare dirs
  echo eval exec exit export false fc fg getopts hash history jobs kill let
  local popd printf pushd pwd read readonly return set shift source test
  times trap true type typeset ulimit umask unalias unset wait . [` and
  keywords `if then else elif fi case esac for select while until do done
  function time { } ! [[ ]]`.
- **PATH filter mask.** New `CompReq.path_filter`: `YEW_PATH_ANY`,
  `YEW_PATH_DIRS`, `YEW_PATH_EXEC_OR_DIR`. Applied after `readdir`, so the
  sliced `DirListing` and its cache are shared across masks; the cache key
  gains the mask (see §5's pitfall).
- **Tilde.** `~/x` and `~user/x` enumerate the expanded directory and
  insert the UNexpanded prefix. Only a LEADING, unquoted `~` expands — a
  quoted one is a literal directory name.
- **Argument-kind defaults** (row 8), a small C table 57.24's specs
  override: `cd pushd rmdir` → DIRS; `mkdir` → DIRS (a parent to type
  into); `which type whence where command` → EXEC ∪ BUILTIN.

### 5. The dispatcher kind — `src/ui/cmdcomp.h`, `src/ui/cmdcomp.c`

The source registry maps ONE kind to ONE source (`yew_comp_source_register`
is idempotent by kind). The shell engine needs several sources merged, so it
is itself one source: `YEW_COMP_SHELL`, whose enumerator reads the context,
applies §3, calls the sub-enumerators, and returns one merged vector. Each
`CompItem.kind` keeps its SUB-source's kind (EXEC, PATH, VAR, …) so the
pager's provenance styling and `yew_comp_sole(items, kind)` still work.

```c
/* CompReq additions (both zero-initialised for every existing caller). */
const YewShCtx *shell;  /* non-NULL only for YEW_COMP_SHELL             */
u32 path_filter;        /* YEW_PATH_* mask; 0 == YEW_PATH_ANY           */
```

`yew_comp_kind_for`'s bang branch becomes `*kind = YEW_COMP_SHELL;` and
`CmdParsePoint` gains `const YewShCtx *shell` (arena-owned, set by the POINT
parser when `bang_body`). `bang_point` is rewritten as a call to
`yew_shctx_at` that fills `token`, `stem` and `token_index` from it, so every
existing reader of `CmdParsePoint` keeps working.

**Pitfall — the filter cache keys on the directory head alone.**
`CompFilter` re-ranks instead of re-enumerating while `head` and `pattern`
are unchanged. For `YEW_COMP_SHELL` that is wrong: `ls | gr` and `ls gr`
share head `""` and a pattern, but one is command position and the other is
not. Add `char *ctx_key` to `CompFilter`, set to a string of the routing
row, the position, `argv[0]` and the path mask; any change re-enumerates.
Write the regression first: type `ls gr`, then insert `| ` before `gr`
without closing the menu, and assert the rows changed.

### 6. Shell quoting — `src/ui/shctx.c`

`yew_comp_quote` quotes for yew's OWN argument tokenizer. Inside a bang body
that is wrong: it does not escape `$`, `` ` ``, `*`, `?`, `[`, so a file named
`$HOME` or `a*b` is inserted in a form the shell expands — and `rm` then
removes something else. That is invariant 1 territory, not polish.

```c
/* Quote `text` for insertion at a caret whose state is `q`.  Returns the
 * bytes to insert; `closing` receives the quote to append when the word
 * is complete (sole match, non-directory), or "" for none. */
char *yew_shq_quote(Arena *a, const char *text, size_t len, YewShQuote q,
                    const char **closing);
```

| Caret state | Rule |
|---|---|
| NONE, text has a byte < 0x20 or 0x7F | emit `'…'` for the whole word, `'` written as `'\''` — a backslash-newline is a line CONTINUATION and would silently delete the byte |
| NONE, otherwise | backslash-escape each of: space, tab, `\ ' " $ `` ` `` ! * ? [ ] ( ) { } < > \| & ;`; plus `#` and `~` when they would be the word's first byte |
| `'…'` | `'` becomes `'\''`; nothing else is special; `closing` = `'` |
| `"…"` | escape `" $ `` ` `` \`; `closing` = `"` |
| `$'…'` | escape `\` and `'`; control bytes as `\xHH`; `closing` = `'` |

**Completion suffix.** On a sole survivor: a directory gets `/` and NO
closing quote (the user will keep typing into it); anything else gets
`closing` then one space. On a menu row: the row's text only, no suffix,
until the user commits. This makes `wolf bu<Tab>` land as `wolf build `.

### 7. Ranking for shell completion — `src/ui/cmdcomp.c`

Shell reflexes are prefix reflexes. `git st` must not offer `reset` because
`s…t` is a subsequence of it. For `YEW_COMP_SHELL` only:

1. If any row is a case-sensitive PREFIX match, keep only those.
2. Else if any row is a case-insensitive prefix match, keep only those.
3. Else keep the fuzzy set as ranked today.

Every other kind keeps today's tiered-plus-fuzzy ranking untouched. Ties
sort by the stable comparator already used for the pager (raw `qsort` is
banned). `yew_comp_lcp` over the tiered rows keeps working unchanged
because after step 1 every row is tiered.

### 8. Defer

- Subcommands, flags, spec files and precommand specs → 57.24. Row 6 of §3
  returns nothing until then; §2's wrapper table becomes 57.24's fallback.
- Any subprocess spawned to answer a completion → 57.24 (generators),
  57.25 (`--help`), 57.26 (fish). This sprint spawns NOTHING; assert it.
- Fish-dialect lexing: non-goal. A `$SHELL` of fish still RUNS `:!`
  commands correctly (execution is untouched); only completion context may
  misread fish-only syntax. Say so in `shctx.h`.

## Testing Strategy

- **Unit, `tests/unit/test_shctx.c` — the corpus.** Table-driven, one row
  per case, the caret written as `‸` in the literal, 150 rows minimum.
  Required families, each with the sample below and at least eight more:

| Input (`‸` = caret) | pos | argv[0] | arg_index | stem |
|---|---|---|---|---|
| `./scr‸` | COMMAND | — | 0 | `./scr` |
| `ls \| gr‸` | COMMAND | — | 0 | `gr` |
| `make && ./bu‸` | COMMAND | — | 0 | `./bu` |
| `FOO=1 BAR=2 cm‸` | COMMAND | — | 0 | `cm` |
| `sudo -u root gi‸` | COMMAND | — | 0 | `gi` |
| `echo $HO‸` | VARIABLE | echo | 1 | `HO` |
| `echo "${PA‸` | VARIABLE | echo | 1 | `PA` |
| `echo '$HO‸` | ARGUMENT | echo | 1 | `$HO` |
| `cat > ou‸` | REDIRECT | cat | 1 | `ou` |
| `cat 2>er‸` | REDIRECT | cat | 1 | `er` |
| `grep x $(ls sr‸` | ARGUMENT | ls | 1 | `sr` |
| `echo $(wh‸` | COMMAND | — | 0 | `wh` |
| `cd "my di‸` | ARGUMENT | cd | 1 | `my di` |
| `git commit -m "fix # not‸` | ARGUMENT | git | 3 | `fix # not` |
| `ls # comm‸` | NONE | | | |
| `cat <<EO‸` | NONE | | | |
| `rm -- -r‸` | ARGUMENT, dashdash | rm | 2 | `-r` |
| `PATH=/usr/b‸` | ASSIGN | — | | `/usr/b` |

  Every row also asserts `replace` is `[start, caret)` and `stem` is its
  decoded text.
- **Unit — execution is untouched.** `test_cmdparse_resolution_bang_errors_and_parse_point`
  and every `yew_cmd_parse` bang case stay byte-identical; add an explicit
  assertion that `CmdParse`'s argument for `:!ls | gr` is the verbatim body.
- **Unit — quoting round trip against real shells.** For 500 seeded random
  byte strings (including `$`, `` ` ``, `*`, `'`, `"`, `\`, newline, tab,
  invalid UTF-8) and each quote state: build `printf %s <quoted><closing>`,
  run it through `/bin/sh -c` and, when present, `zsh -c` and `bash -c`
  via the job layer's sync path, and assert stdout equals the original
  bytes exactly. This is the proof §6 is right; a hand-picked list is not.
- **Unit — routing.** One test per §3 row asserting the source set, plus the
  §5 cache regression (`ls gr` → insert `| `), plus §7's `git st` case (a
  fixture PATH with `stash`, `status`, `reset` executables: `st` offers only
  the first two).
- **Unit — VAR redaction.** `GITHUB_TOKEN=abc` shows `•••`; `EDITOR=vi`
  shows `vi`; a value holding `\x1b[2J` renders no ESC byte.
- **Fuzz, `tests/fuzz/fuzz_shctx.c`** + `make fuzz-shctx`: arbitrary bytes
  and caret; asserts termination, `replace.lo <= replace.hi == cursor`,
  `arg_index <= argc`, `depth <= 64`, no ASan/UBSan report. Seed corpus = the
  unit corpus's inputs.
- **PTY**: `s57_23_bang_dot_slash_exec`, `s57_23_bang_pipe_command_position`,
  `s57_23_bang_variable`, `s57_23_bang_sudo_wrapper`, `s57_23_bang_cd_dirs_only`,
  `s57_23_bang_quote_dollar_file` (a fixture file named `a$b c` completes to
  `a\$b\ c`).
- **Perf**: `perf_cmdcomp` gains a case lexing a 4 KiB body with the caret at
  the end, 1000 iterations; p99 under 200 µs on the reference machine. The
  lexer runs on every keystroke of an open menu.
- **Subprocess count**: a unit test asserts `yew_job_running_count(ed)` is 0
  after 100 completions across every §3 row.

## Definition of Done

1. `make -j8 build/unit_tests build/yew` warning-free under gcc and clang;
   `MODULES=""` builds and passes (the redaction predicate lives in core).
2. `./build/unit_tests --filter shctx` passes with ≥ 150 corpus rows.
3. The quoting round-trip test passes against `/bin/sh`, and against `zsh`
   and `bash` where installed, for all 500 strings in every quote state.
4. Every §3 row has a routing test; the §5 cache regression and §7's `git st`
   test pass.
5. Every existing `test_cmdparse` and `test_cmdcomp` case passes unchanged
   (count before and after, stated in the merge report).
6. `make fuzz-shctx` runs 60 s clean under ASan/UBSan.
7. The six `s57_23_*` PTY goldens pass; the full `make test-pty` is green.
8. `make perf-cmdcomp` passes including the new lexer case.
9. No subprocess is spawned by completion (the §Testing count assertion).
10. `bang_word` no longer exists; `grep -n bang_word src/` is empty.
11. `make test-fletch test-script test-roundtrip test-roundtrip-coverage
    test-audit` green; `scripts/check-cmd-dispatch.sh`, `bans.sh`,
    `check-input.sh` green.
