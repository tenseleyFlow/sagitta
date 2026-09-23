# Sprint 57.26: Shell Completion IV — The Fish Oracle and History Suggestions

## Prerequisites

- **Sprint 57.25** — the precedence ladder for ARGUMENT position (57.25 §7:
  spec → fish → help → defaults), the help layer's prewarm on the idle
  tick, and its async/arrival discipline inherited from 57.24 §5 (argv
  form, `YEW_SINK_CALLBACK`, `internal`, ≤ 1 job per key, ≤ 4 completion
  jobs total, arrivals never edit the line, `…` marker as state).
- **Sprint 57.23** — `YewShCtx` (`argv`, `arg_index`, `stem`, `quote`),
  `yew_shq_quote`, `yew_secret_name` (`src/util/secret.h`).
- The command line's ghost: `cmdline_ghost` (`src/ui/cmdline.c:1428`),
  which today shows the rest of the menu's top row, never enters the
  TextBuf, and is accepted by `ed.cmdline.ghost.accept` (bound to `<right>`
  in E mode, `runtime/init.fl:410`). Per-kind scoped history:
  `yew_hist_open_scoped(history_kind(kind), ed->state.key.dir, …)`
  (`src/ui/cmdline.c:385–393`); `:!` commands are recorded there as E-mode
  lines.
- Binding: invariants 4, 5, 7 (fish is an OPTIONAL runtime tool — the
  amendment in §1), 9.

## Goals

Two things that make `:!` feel like fish.

**The oracle.** Fish carries hand-written completions for a very large
number of tools, and it answers over a plain pipe: `complete -C` prints
`candidate<TAB>description`, the exact shape of the pager's two columns.
When fish is installed, it answers for commands yew has no spec for,
before yew falls back to parsing `--help`. It also picks up any fish
completion the user has installed for their own tools — `wolf.fish` exists
in this user's `~/.config/fish/completions/` today.

**History suggestions.** Type the start of a command you have run before
and the rest appears as dim ghost text; `<right>` takes it, `A-f` takes one
word. This is fish's autosuggestion, built on the ghost the command line
already draws.

Deferred: a persistent shell session → Sprint 57.27 (reserved). Querying
zsh's completion system (needs a pseudo-terminal) and bash-completion (no
descriptions, fragile sourcing) are non-goals, named in §5.

## Deliverables

### 1. Record the decision — `.docs/plan/00-decisions.md`

Change the "Optional runtime tools" row to add fish, and append the
amendment below verbatim. The decision was made with the user on
2026-09-22; this sprint records it where the invariants live.

```markdown
**Amendment S57.26-A1 (2026-09-22) — fish is an optional completion oracle.**
When `fish` is on `PATH` and `shell.complete_fish` is `auto`, `:!`
completion may ask it for candidates for a command yew has no spec for.
It is queried asynchronously, never on the keystroke path, with user
configuration NOT loaded (`fish -N`) and the user's completion and function
directories added back explicitly. Absent fish degrades to the `--help`
layer exactly as absent `git` degrades FUSS mode. No test's pass/fail may
depend on fish being installed: the suite runs against a stub, and the one
real-fish test skips, loudly, when fish is absent.
```

### 2. The oracle — `src/ui/compfish.h`, `src/ui/compfish.c`

**Measured behaviour this design rests on** (fish 4.8.1, this machine,
2026-09-22). Re-verify each against the stub AND against real fish in the
conditional test; if a later fish changes one, the test must say which.

| Fact | Consequence |
|---|---|
| `fish -N` (no config) does NOT load `~/.config/fish/completions` — `wolf bu` returns nothing and `complete -c wolf` lists 0 rules | prepend `$__fish_config_dir/completions` to `fish_complete_path` and `$__fish_config_dir/functions` to `fish_function_path` inside the script |
| with that prepend, `wolf bu` → `build<TAB>compile an entry file to a native executable` | user completions work without running `config.fish` |
| without `-N`, a query costs 66 ms; with it, 44 ms (git, 5-run mean) | `-N`: faster, and no prompt/plugin side effects from `config.fish`. Either way it is ~9× the keystroke budget → async only |
| for a command fish knows nothing about, `complete -C "cmd "` lists the cwd's files (17 of them here for every unknown tool) | a result is used ONLY when `complete -c <cmd>` has rules after autoload — otherwise "fish has no idea" would masquerade as knowledge |
| candidates are printed RAW: `a b.txt`, `d$x`; directories end in `/` | re-quote every candidate with `yew_shq_quote`; never insert fish's text as-is |
| `complete -c git` after autoload lists 2101 rules; `wolf` (with prepend) > 0; `fackr` 0 | the gate distinguishes all three |

**The query.** One script, fixed text, the user's words passed as argv —
**never interpolated into the script**:

```c
static const char FISH_QUERY[] =
    "set -p fish_complete_path $__fish_config_dir/completions; "
    "set -p fish_function_path $__fish_config_dir/functions; "
    "set -l r (complete -C -- $argv[1]); "
    "if complete -c $argv[2] | string length -q; "
    "printf '%s\\n' $r; end";
/* argv: fish -N --private -c FISH_QUERY -- <line> <command> */
```

`--private` keeps the query out of the user's fish history.

**Pitfall — injection.** A line typed at `:!` is arbitrary text. Building
the `-c` string with it would let `x'; rm -rf ~; '` run. The line travels
only as `$argv[1]`. The unit test proves it: the stub records its argv and
the test asserts the hostile line arrived as ONE element, byte-identical.

**The line fish sees.** Not the raw `:!` text — fish's grammar differs
(`$(…)`, `${…}` and backticks are not fish). Rebuild it from `YewShCtx`:
`argv[0] … argv[arg_index-1]`, each backslash-escaped for fish (escape
space, tab, and `$ * ? ~ # ( ) { } [ ] < > & | ; " ' \`; newline as `\n`),
joined by single spaces, then a space, then the QUERY STEM:

- `""` normally — fish returns every candidate for the slot, and yew's own
  ranker filters by the real stem. One query then serves every keystroke
  in that slot.
- `"-"` when the real stem starts with `-` — fish lists flags only for a
  `-` stem.

**The gate and the parse.** Output lines split at the first TAB into
candidate and description; control bytes rendered as `·`; at most 5000
lines. Candidates ending `/` are directories (`is_dir`, and the `/` is
dropped from the text the ranker sees and re-added on insert).

**Caching and timing.** Key = (fish path, cwd, the rebuilt words before
the caret, query stem). Fresh for 5000 ms. Prewarm exactly as 57.25 §6 does
(idle tick, entering ARGUMENT position of a command with no spec, one in
flight, counts toward the four-job cap). Arrival rule unchanged: repaint if
the context still matches; never edit the line.

**Detection.** `shell.complete_fish` global enum, `auto` (default) or `off`.
Under `auto`, resolve `fish` on `PATH` once per session; the first query's
failure (spawn error, non-zero exit with empty output) disables the oracle
for the session with one `yew_log` line. Test seam: the environment
variable `YEW_TEST_FISH` names the executable to use instead, so the suite
runs against a stub script.

**Where the oracle is consulted.** ARGUMENT position only, only for a
command with no spec, and only after 57.23's shape rows (a path, `$VAR` or
`~user` stem never reaches fish). COMMAND position stays yew's EXEC and
BUILTIN sources.

### 3. History suggestions — `src/ui/cmdline.c`, `src/ui/cmdhist.c`

**Source.** The E-mode history entries that are bang commands; the
suggestion is matched against the BODY (text after `!`, `!!`, `r !` or a
range's `!`), so `:!git st` finds `:!git status` whichever prefix form it
was run with. Loaded once per prompt open into the completion arena,
deduplicated keeping the newest, newest first, capped at 20 000 entries.

Second source, global enum `shell.suggest_history`, `all` (default) or
`yew`. Under `all`, also read — read-only, once per prompt open — the
history files that exist. **Why `all` is the default** (the user's call,
2026-09-22): yew's own `:!` history starts empty, and this user's real
history lives in fish (3,588 lines against 38 in zsh). A suggestion
feature with nothing to suggest for its first month is not the fish
experience. `yew` remains for anyone who wants yew to read only its own
files.

| Shell | File | Format |
|---|---|---|
| fish | `$XDG_DATA_HOME/fish/fish_history` (default `~/.local/share/fish/fish_history`) | YAML-ish: `- cmd: <text>` lines; `\\` → `\`, `\n` → newline |
| zsh | `$HISTFILE`, else `~/.zsh_history` | optional `: <ts>:<dur>;` prefix; **metafied**: byte `0x83` followed by `b` means `b ^ 0x20`; a line ending `\` continues |
| bash | `$HISTFILE`, else `~/.bash_history` | plain; `#<digits>` timestamp lines skipped |

**Pitfall — zsh metafication.** zsh stores non-ASCII bytes escaped with
`0x83`. Reading the file as text turns every accented filename in the
user's history into mojibake — and a suggestion accepted from it names a
file that does not exist. Unmetafy before anything else touches the bytes
(invariant 2). The fixture test includes `café` written by zsh.

A multi-line history entry is never suggested (the prompt is one line).
Under either source, an entry containing a `NAME=value` word where
`yew_secret_name(NAME)` is true is skipped — a ghost is on screen, and
screens are shared.

**The rule.** When the caret is at the end of a bang body, the body is
non-empty, and the menu has NO explicit selection: the newest entry that
begins with the typed body and is longer than it supplies the ghost — its
remainder. Case-sensitive, byte-exact.

**Precedence with today's ghost.** `cmdline_ghost` becomes two providers
behind one function. With an explicit menu selection, today's token ghost
(the rest of the selected row) — the user is navigating the menu. Without
one, the history ghost if there is a match, else today's token ghost. One
function, so the drawing code at `cmdline.c:2077` and the accept path keep
their single call site.

**Accepting.** `ed.cmdline.ghost.accept` (`<right>`) takes the whole ghost
through the existing single accept path (a ghost accepted and a row
accepted must land byte-identical text — keep that comment true). New
`ed.cmdline.ghost.accept_word` takes the ghost up to and including the
next run of unquoted whitespace (or all of it if there is none), bound in
E mode to `A-f` and `A-<right>`. Both keys are free in E mode today;
re-verify before binding and report any collision rather than overwriting.
Register it for the invariant-9 audit, give it a motion word and a
`gen_cmds[]` entry or `D(...)` exclusion, add it to `command_name_valid`,
mirror both bindings in `tests/unit/test_runtime_defaults.c`, and RECOUNT
`yew_bind_active_count` by running the test.

**Invariant 5.** The ghost is a pure function of the loaded history
snapshot and the line. The snapshot is taken at prompt open, so history
written by another yew process mid-prompt cannot change a frame.

**Invariant 4.** The per-keystroke scan is a prefix `memcmp` over at most
20 000 entries; the loading happens at prompt open. Perf-gated below.

### 4. Documentation — `runtime/completions/README.md`

Add a section: how the oracle is found, what `shell.complete_fish`,
`shell.complete_help` and `shell.suggest_history` do, and that a fish
completion file in `~/.config/fish/completions/` is picked up.

### 5. Defer

- Persistent shell session (a `cd` or `export` in one `:!` visible to the
  next; aliases and functions) → **Sprint 57.27**, reserved. Not a
  pseudo-terminal: a long-lived shell coprocess over pipes.
- zsh's completion system as an oracle: non-goal. The known capture method
  runs zsh under a pseudo-terminal (`zpty`) and sources the user's whole
  `compinit`; too slow and too fragile for a keystroke-driven UI.
- bash-completion as an oracle: non-goal. Its functions return bare
  `COMPREPLY` words with no descriptions and depend on sourcing a large,
  distro-specific script tree.
- History suggestions for ordinary E-mode commands (not bang bodies): out
  of scope for this campaign.

## Testing Strategy

- **Unit, oracle with a stub** (`tests/unit/test_compfish.c`): `YEW_TEST_FISH`
  points at a fixture script that records its argv to a file and prints
  canned output keyed by `$argv` contents. Cases: gate open (rows used);
  gate closed (rows discarded, falls through to the help layer); a
  directory candidate; `a b.txt` and `d$x` inserted as `a\ b.txt` and
  `d\$x`; the hostile line `x'; rm -rf ~; '` arrives as one argv element,
  byte-identical; query stem `""` vs `"-"`; `$(…)` in the user's line is
  NOT passed through (the rebuilt words are); cache hit within 5000 ms;
  spawn failure disables the oracle once with one log line; precedence (a
  spec'd command never queries the stub — assert the argv file is absent).
- **Unit, real fish (conditional)**: when `fish` is on PATH, with a fixture
  `XDG_CONFIG_HOME` holding a `completions/demo.fish` whose rules call a
  helper in `functions/`: `demo <Tab>` returns the fixture's rows (proves
  both prepends); `nosuchtool ` is gated closed. When fish is absent, the
  test prints `SKIP real-fish: fish not on PATH` and the runner counts it
  as skipped, not passed.
- **Unit, history**: fixture fish, zsh (with a metafied `café` and a
  timestamp prefix and a continuation line) and bash (with `#ts` lines)
  histories under a fixture `HOME`; the default is `all` (assert the
  option's default value); `yew` reads no shell file (assert the fixture
  files are never opened); newest wins; secret
  assignments skipped; multi-line skipped; the ghost is the remainder;
  accept whole and accept word; precedence against the token ghost with
  and without an explicit selection; the ghost never enters the TextBuf
  (`yew_cmdline_text` unchanged).
- **PTY**: `s57_26_history_ghost` (fixture history, type a prefix, the dim
  remainder shows), `s57_26_history_accept_word`, `s57_26_fish_stub_rows`
  (stub oracle rows in the pager with descriptions).
- **Perf**: `perf_cmdcomp` gains the history scan over 20 000 entries,
  p99 under 300 µs.
- **Determinism**: the full suite passes with `PATH` stripped of fish.

## Definition of Done

1. `00-decisions.md` carries the amended row and Amendment S57.26-A1
   verbatim.
2. Every stub-oracle test passes, including the injection test.
3. The real-fish test passes on this machine, and reports SKIP (not pass)
   with fish removed from PATH.
4. In the built editor on this machine, a command known only to fish
   (e.g. `rsync --del<Tab>`) completes with fish's descriptions, and
   `wolf bu<Tab>` still comes from `wolf.fl` (spec precedence).
5. History tests pass for all three formats, including zsh metafication.
6. `<right>` accepts a whole history ghost; `A-f` / `A-<right>` accept one
   word; `yew_bind_active_count` recounted by running the test.
7. The three `s57_26_*` goldens pass; full `make test-pty` green, and green
   again with fish absent from PATH.
8. `perf-cmdcomp` green with the history case.
9. gcc and clang warning-free; ASan/UBSan filtered runs for compfish,
   cmdline, cmdhist, cmdcomp clean; `MODULES=""` builds and passes.
10. `test-fletch test-script test-roundtrip test-roundtrip-coverage
    test-audit` and the three `scripts/check-*.sh` gates green.

## Implementation divergences (recorded at landing)

Where this contract was written before 57.25 landed, or was wrong about
the code, the implementation did what the contract intends:

1. **Nothing spawns on the keystroke path, Tab included** (57.25's rule):
   a fish lookup that misses only QUEUES a request; the loop's input-free
   turn (`yew_cmdline_comp_idle`) spawns it, fish before help, one spawn
   per turn. At most one fish job is in flight and it counts toward the
   four completion jobs (compgen's and comphelp's caps count it too).
2. **The rung is 57.25's reserved slot** (`plan_fish` in `shell_plan`),
   consulted exactly where the help rung is -- rows 6 and 9, never a shell
   builtin (fish's `set`/`echo` rules describe fish's own). While fish's
   answer is pending the slot is held: 57.23's rows show with the `…` and
   the help rung is not asked; only a CLOSED answer (no rules) lets help
   answer. A flag stem has no 57.23 rows, so no pager shows the `…` while
   it waits; the arrival opens the menu (Tab asked).
3. **fish's rows are `YEW_COMP_GEN` rows** (an external answer with
   descriptions); no new kind was added. Candidates are re-quoted by
   `shell_push` (`yew_shq_insert`); a directory is ranked by its name and
   carries its `/` back.
4. **Query stems beyond `""` / `"-"`:** `--name=` for a flag's value (fish
   prints whole `--name=value` words) and `.` for a leading dot (fish
   lists dotfiles only for one).
5. **An empty word -- `''` or an expansion such as `$(pwd)`, whose value
   only the shell knows -- reaches fish as `''`**, so fish counts positions
   as the shell will; none of the expansion's text is passed.
6. **Measured fact 0 (fish 4.8.1):** fish autoloads a completion file only
   for a command that resolves on `PATH`. The real-fish test installs its
   `demo` executable before its rules can answer, and checks the fact.
7. **Failure:** a spawn error, an exec failure, or a non-zero exit with
   empty output -- on any query, not only the first -- disables the oracle
   for the session with one `yew_log` line (from the idle path or the
   arrival, never a lookup). A timeout caches the slot closed for 5 s.
   A stale answer is served and refreshed (compgen's pattern).
8. **A candidate line holding a control byte is dropped** rather than
   drawn with `·`: inserting it would name a different file. Descriptions
   still draw `·`.
9. **The history snapshot is taken on the first idle turn after the
   prompt opens** (and on demand if a bang body is typed before that
   turn), not synchronously at open: prompt open is itself a keystroke.
   It is fixed from then until the prompt closes. Order: yew's own bang
   bodies (newest first), then fish, zsh, bash (each newest first) --
   the files share no clock with yew's history.
10. **Snapshot cost:** bodies live in one pool, the dedupe hash reads
    eight bytes a step, and controls/secrets are found a word at a time;
    `perf-cmdcomp` gates the 20 000-entry scan (p99 300 us) AND a 20 000-
    entry load (5 ms).
11. **Refused entries:** any control byte (a tab included), and any
    `NAME=` whose name yew_secret_name() flags wherever it starts at a
    word boundary -- `--token=…` included, not only env assignments.
12. **`$HISTFILE`** is zsh's when its basename mentions zsh, else bash's;
    only the environment names a file (never the password database), so a
    process without HOME reads nothing. Leading blanks are ignored in the
    typed body and in entries.
13. **zsh 5.9 does not metafy `é` (C3 A9)**; the fixture (written by zsh
    itself) holds `café` AND `ăę` (C4 83 / C4 99, metafied to C4 83 A3 /
    C4 83 B9), which is what the unmetafy test is about.
14. **`A-f` takes any leading blanks, the next word and the unquoted
    whitespace after it** (a ghost `" status --short"` gives `" status "`,
    not a lone blank). On a token ghost whose word is the whole ghost it
    goes through the one accept path, so the byte-identical comment holds.
15. **`ed.cmdline.ghost.accept_word` is internal** like
    `ed.cmdline.ghost.accept`: not recordable, so no motion word and no
    `gen_cmds[]`/`D(...)` row; its verb `accept_word` was already in
    `command_name_valid` (Sprint 27). It is in the invariant-9 list and
    the internal-cmdline-command test. Bindings: 345 active.
16. **Isolation by default.** Unit: every test gets the run's own HOME and
    XDG_DATA_HOME (inside the canonicalized per-run root), no HISTFILE, and
    `YEW_TEST_FISH=""` (the seam's "no fish"); a test proves a child run
    pointed at a sentinel home never shows it. The harness gained SKIP
    (`SKIP <reason> (<test>)`, counted apart). PTY: the child environment
    is built from scratch, so HISTFILE and XDG_DATA_HOME are never
    exported and HOME only as a case's fixture; `YEW_TEST_FISH` is always
    exported, empty unless a case supplies a stub. `perf-cmdcomp` unsets
    HOME, XDG_DATA_HOME and HISTFILE.
17. **`yew_cmdline_ghost`** is public (tests and the perf gate read the
    ghost the prompt draws).
