# Sprint 37: Headless Batch — `yew --batch` and the Script-Test Runner

## Prerequisites

- Sprint 0 — the driver shell and argument parser, the **exit-code ledger**
  (0 success · 1 error · 2 `--batch` script failure · 3 I/O error · 4
  internal bug via `yew_bug()`), `yew_log` to `$XDG_STATE_HOME/yew/log`
  with a pluggable sink, `yew_mod_require`. s00 already errors on `--batch`
  naming this sprint; s01's `scripts/smoke.sh` asserts that message and must
  be updated here.
- Sprint 1 — the unit harness, `make test`, `scripts/bans.sh`, CI lanes, and
  the explicit deferral this sprint discharges:

  ```make
  test-script:
      @echo 'error: script-test runner lands in Sprint 37 (yew --batch)'; exit 1
  ```

- Sprint 3 — `Tty`, `yew_tty_open/raw/restore`, the terminal-restore
  guarantee, and s03's existing refusal `stdin is not a terminal (--batch
  lands in Sprint 37)`. **Assumption beyond its index entry**: `Tty` is a
  struct owned by `Ed` with an `rfd`; this sprint adds one `bool poisoned`
  field and a guard at the top of each `yew_tty_*` entry point.
- Sprint 8 — `yew_file_load`, `yew_file_save`, `FileMeta`, the journal.
  Sprint 10 — `yew_undo_dump`. Sprint 12 — registers. Sprint 13 —
  `yew_cmd_invoke`, `CmdDesc.flags`, `YEW_CMD_PROMPTS`, and the
  `YEW_CMD_INTERACTIVE` flag **defined** by Sprint 34 and enforced here.
- Sprint 14 — `Ed`, `Buffer`, `Win`, `Workspace`; the loop is *not* entered
  in batch mode. Sprint 18 — E-mode prompts (all interactive).
- Sprint 34 — flapi, handles, origins, `edit{}`, hooks, `ed.run`.
  Sprint 35 — the recorder and `yew_macro_replay`. Sprint 36 — config load
  order, `--clean`, the trust prompt's no-tty branch, `--config PATH`.
- Sprints 30–32 — VM, stdlib (`io` module, capability-gated), `yew fl`,
  runtime stack traces (`fl_trace_write(FlVm*, Bytebuf*)` assumed:
  s32's entry says "runtime stack traces" without an API).
- Binding: `00-decisions.md` Headless-mode row, invariants 1, 3, 6, 8;
  `02-fletch.md`'s vim comparison — *"`yew --batch script.fl files…` is a
  supported, documented product surface"*.

## Goals

Land `src/edit/batch.c|h` and `tests/script/`: `yew --batch script.fl
[files…] [-- args…]` — a real editor with no tty, no grid, and **no
terminal state touched at all**, with a pinned stdio contract, exit codes
per the ledger, batch save semantics, and interactive-only operations that
hard-error naming themselves. On top of it, the script-test runner Sprint 1
deferred here: `tests/script/*.fl` with an assertion stdlib, per-test
process isolation, deterministic discovery and output, and the migration of
the feature coverage earlier sprints explicitly parked for this moment.
`make test-script` stops hard-erroring and becomes a `make test` dependency
and a CI lane. Macro UX is Sprint 38; plugin `--grant` is Sprint 54 (the
flag's grammar is reserved here so 54 wires rather than designs).

## Deliverables

### 1. The command line

```
yew --batch SCRIPT.fl [FILE…] [-- ARG…]

  --batch SCRIPT      run SCRIPT headless; implies no tty, no grid
  --test              add the assertion globals (Deliverable 6)
  --config PATH       user config override (s36)
  --clean             skip all config (s36)
  --trust-workspace   pre-grant the workspace .yew.fl (s36)
  --quiet             suppress editor diagnostics on stderr (not errors)
```

- `FILE…` are opened as buffers, in order, before the script runs;
  `buf.list()` returns them in that order and `buf.current()` is the first.
  A file that cannot be read is exit 3 with the path on stderr **before**
  the script runs — a script must never see a half-open workspace.
- Everything after `--` is the script's `args`, verbatim.
- `-` as a FILE opens standard input as a buffer named `[stdin]`; it is read
  to EOF before the script starts and is never a tty.

### 2. No tty, and it fails at startup

```c
typedef struct BatchOpts { const char *script; char **files; u32 nfiles;
                           char **args; u32 nargs; bool test, quiet; } BatchOpts;
int yew_batch_run(const BatchOpts *o);   /* returns the process exit code */
```

The bootstrap, in order: parse args → `ed.headless = true` → **poison the
terminal** → init `Workspace`, `Registers`, the VM, origins → load config
(s36 rows 1–3, no prompts) → open files → run the script → teardown.
`yew_loop_run` is never called; `Grid`, `Damage`, and `Input` are never
initialized.

```c
/* Poisoning is the whole "fail loudly at startup" contract.  Every
 * yew_tty_* entry point begins with this guard, so a code path that
 * assumes a terminal dies on its FIRST call with a message naming itself
 * — not at "first paint", which in --batch never happens, which is
 * exactly how such a bug survives to a user's CI pipeline.            */
void yew_tty_poison(Tty *t);            /* t->poisoned = true; rfd = -1  */
#define YEW_TTY_GUARD(t) \
    do { if ((t)->poisoned) YEW_BUG("terminal access in --batch: %s", \
                                    __func__); } while (0)
```

- Exit 4 (internal bug) on a violation, with the function name — a poisoned
  tty access is a yew bug, not a user error.
- **Startup sweep, not first-paint discovery**: after poisoning, the
  bootstrap calls `yew_batch_selfcheck(Ed*)`, which asserts `grid.cells ==
  NULL`, `in.buf == NULL`, `tty.rfd < 0`, `timers.n == 0`, and that no
  `YEW_CMD_INTERACTIVE` command is bound in any *installed* layer's terminal
  path. Cheap, runs always (not just in debug), and turns "someone wired a
  render into a command" into an immediate, named failure.
- Grep gate (DoD 3): `grep -rn 'tcsetattr\|tcgetattr\|ioctl\|isatty' src/`
  hits only `src/term/tty.c`, and every hit in `tty.c` is preceded by
  `YEW_TTY_GUARD`.
- Invariant 6 is satisfied vacuously: batch never enters raw mode, so there
  is nothing to restore — asserted by a pty test that runs `yew --batch`
  *under* a pty and byte-compares the terminal state before and after.

### 3. The stdio contract — pinned

| Stream | Carries | Never carries |
|---|---|---|
| stdout | **only** what the script prints (`print`, `io.write`, `fmt.print`) | editor diagnostics, log lines, progress, warnings, the trace |
| stderr | editor diagnostics, `yew_log` (mirrored at WARN+ while `--batch`), compile diagnostics with carets, runtime stack traces, batch warnings | script output |
| stdin | the script via `io.stdin`, or a `-` FILE buffer, never both | prompts (there are none) |

Consequences, stated because they are the product:

- `yew --batch fmt.fl file.c > out.c` is safe: nothing but the script's
  output lands in `out.c`.
- `yew_log` normally never writes to stdout/stderr while the TUI owns the
  terminal (s00). In `--batch` nothing owns the terminal, so WARN and ERROR
  are mirrored to stderr prefixed `yew: `; the log file still gets
  everything. `--quiet` suppresses the mirror but not errors.
- stdout is line-buffered when it is a tty and block-buffered otherwise, and
  is **flushed before exit on every path** including the error paths — a
  script whose output vanished because it errored is a bug report we do not
  want.

The script's globals:

| Global | Type | Contents |
|---|---|---|
| `args` | list of str | everything after `--`, verbatim |
| `script_path` | str | realpath of the script |
| `files` | list of str | the FILE arguments as given |
| `batch` | bool | `true` — the one flag a script needs to branch on |

### 4. Exit codes and the trace

| Code | Meaning | Emitted by |
|---|---|---|
| 0 | script ran to completion | normal return |
| 1 | usage error, unreadable script, bad flag | argument layer |
| 2 | **script failure**: uncaught Fletch error, or a compile error in the script | `yew_batch_run` |
| 3 | I/O error opening or saving a file outside the script's control | file layer |
| 4 | internal bug (`YEW_BUG`), incl. a poisoned-tty access | `yew_bug` |

Exit 2's stderr format (goldens pin it):

```
yew: script failed: user: cannot format a binary buffer
  at fmt_buffer (tools/fmt.fl:41)
  at <main> (tools/fmt.fl:12)
tools/fmt.fl:41:9: error: raised here
   41 |         error("cannot format a binary buffer")
      |         ^~~~~
```

The trace is `error.trace` (spec §9) rendered top-frame-first; the caret
block reuses the s00 diag renderer. A compile error exits 2 with the caret
diagnostic and no trace (there are no frames yet). `error(v)` with a map
prints `kind: msg`; a map without `msg` prints its literal form.

**Pitfall — an uncaught error is not a crash.** The VM unwinds, `edit{}`
transactions roll back (s34 §5), buffers are left consistent, journals are
synced, and *then* the process exits 2. A `--batch` run that dies mid-edit
must leave the same on-disk state as one that never started (invariant 1);
the torture test kills batch runs at random instants and asserts it.

### 5. Batch save semantics and interactive refusals

- **Explicit saves only.** Nothing is written unless the script calls
  `b.save()` / `ed.run("ed.file.save")`. There is no autosave, no
  save-on-exit, and no prompt.
- Exiting with modified buffers is **exit 0** plus one stderr line:
  `warning: 2 buffers modified and not saved: a.c, b.c`. Making it an error
  would break the legitimate "analyze without writing" script; making it
  silent would hide a missing `save()`. `--batch-strict` (reserved, errors
  naming Sprint 59's docs pass) is *not* added speculatively.
- The crash journal is still written (invariant 1 does not have a headless
  exemption) and discarded on clean exit; a killed batch run is recoverable
  by the next interactive open of the same file.
- Overwrite/mtime conflicts: `yew_file_save`'s `YEW_SAVE_CHANGED_ON_DISK`
  (s08) becomes a catchable Fletch error of kind `"io"` instead of a prompt.
  `b.save({force: true})` is the scripted answer.
- **Interactive-only operations hard-error naming themselves.** Every
  command with `YEW_CMD_PROMPTS` or `YEW_CMD_INTERACTIVE` refuses in batch:

  ```
  yew: "ed.file.open_prompt" requires a terminal and is not available
           under --batch; use buf.open(path)
  ```

  The message names the command **and the scripted alternative** where one
  exists (a table in `batch.c`, one row per interactive command). Refusal is
  a Fletch error of kind `"capability"`, so a script can `try` it.

| Interactive command | Batch alternative named in the message |
|---|---|
| `ed.file.open_prompt` | `buf.open(path)` |
| `ed.search.forward` (incremental UI) | `b.find(re)` |
| `ed.find.files` / `.buffers` (pickers) | `buf.list()`, `io.glob` |
| `ed.mode.enter "E"` | `ed.run(name, args)` |
| `ed.macro.record` | recording needs keys; none exist |
| `ed.plug.*` prompts (s54) | `--grant name:cap` |
| `ed.config.reload` | allowed — no prompt, no tty |

`--grant NAME:CAP` (repeatable) is parsed here and stored on the origin
table for Sprint 54 to consume; using it before 54 errors naming 54.

### 6. The script-test runner — `tests/script/`

```
tests/script/
  runner.c            discovery, isolation, reporting; build/script_runner
  fixtures/*          input files copied into each test's sandbox
  <area>_<case>.fl    the tests themselves
```

`make test-script` builds `build/script_runner` and runs it; `make test`
gains it as a dependency and CI gains a `script` lane.

**Discovery**: `tests/script/*.fl` sorted **bytewise** (`yew_sort_stable`) —
a test exists iff the file exists, mirroring s01's registry discipline in
the only way a directory can. `--filter <substr>` and `--list` behave
exactly as s01's unit runner, including "a filter matching zero tests exits
1".

**Isolation**, per test, no exceptions:

1. a fresh temp root `$T` (`mkdtemp`), with `XDG_CONFIG_HOME=$T/cfg`,
   `XDG_STATE_HOME=$T/state`, `XDG_DATA_HOME=$T/data`, `HOME=$T`, `TMPDIR=$T`;
2. `tests/script/fixtures/` copied into `$T/work`, which is the cwd and the
   workspace root;
3. a **fresh process**: `fork` + `execvp` of `build/yew --batch --test
   --clean <file>` (`--clean` unless the test's header requests config);
4. a wall budget (`YEW_SCRIPT_BUDGET_MS`, default 10 000) then `SIGKILL`;
5. `$T` removed on pass, **preserved and its path printed** on failure — the
   log file inside it is usually the answer.

One process per test file is the pinned choice: a crash, a `YEW_BUG`, or a
runaway loop in one test cannot take the suite, and there is no shared
editor state to leak between tests. The cost is ~2 ms of process spawn per
test, which is under the noise floor of the file I/O each test already does.

**Assertion stdlib.** `--test` installs host globals under `t.*` (no
`import`, so no module-path change and no spec amendment):

| Function | Asserts |
|---|---|
| `t.eq(a, b, note?)` | Fletch `==`; on failure prints both values `fmt`-rendered |
| `t.ne(a, b)` | inequality |
| `t.text(buf, str)` | whole buffer bytes equal `str` **byte-exactly** |
| `t.line(buf, n, str)` | line `n` (1-based, no EOL bytes) |
| `t.cursor(win, line, col)` | primary cursor's 1-based line and GCol |
| `t.cursors(win, list)` | the whole cursor set as `[[line,col],…]`, ascending |
| `t.sel(win, str)` | primary selection's text |
| `t.reg(name, str, type?)` | register bytes and, optionally, `RegType` |
| `t.undo(buf, n)` | undo-tree node count (`yew_undo_dump` parse) |
| `t.file(path, str)` | on-disk bytes — the save path, not the buffer |
| `t.raises(kind, fn)` | `fn()` raises a spec §9 error of that kind |
| `t.log(level, substr)` | a log line was emitted (structural, not scraped) |
| `t.fixture(name)` | copies `fixtures/<name>` into cwd, returns its path |
| `t.tmpdir()` | the sandbox root |
| `t.skip(reason)` | marks the test skipped, exit 0 |

Every assertion increments a counter, and the counter is printed — the DoD
counts assertions, as s01 established. Failures do **not** stop the test:
`t.*` records and continues (matching the unit harness's per-test
`longjmp` isolation), so one script reports every failure it finds.
Byte-exactness matters: `t.text` compares bytes, never strings normalized
through anything, so invalid UTF-8 and CRLF ride through (invariant 2).

**Output**, pinned and deterministic (no timings, no paths in the pass
lines) so the determinism lane can diff it:

```
PASS undo_branch_survives            (14 assertions)
FAIL search_replace_confirm          (9 assertions, 1 failure)
  tests/script/search_replace_confirm.fl:31: t.text
    want | "hello world\n"
    got  | "hello  world\n"
                  ^ first difference at byte 6
  sandbox preserved: /tmp/yew-script-a7f2
script: 43 tests, 512 assertions, 1 failure, 0 skipped
```

Exit 0 iff zero failures. A test file that produces no assertions **fails**
("no assertions") — the silent-pass bug that makes a suite lie.

### 7. Migration: which sprints' coverage moves here

Sprints that explicitly parked coverage for this runner, and what lands:

| Sprint | Moves | Was |
|---|---|---|
| 19 (shell execution) | every §5 filter-outcome row on real fixture files | `tests/unit/test_filter.c` — s19 says "until Sprint 37" verbatim |
| 21 (search/replace) | search, confirm-replace, `:s` semantics over fixture files | s21 says "script tests (Sprint 37)" |
| 14 (L/I modes) | open → edit → save flows, dirty-quit refusal, reopen | pty + unit fragments |
| 17 (multi-cursor) | simultaneous edit semantics, merge matrix on real text | unit tests over synthetic sets |
| 12 (registers) | linewise/blockwise paste on real files, `clipboard.sync` | unit tests (the pure-data rows **stay** in unit) |
| 34 (editor API) | `api_*.fl` move from `yew fl` invocation to `--batch --test` | ran under `yew fl` |
| 35 (recorder) | `macro_*.fl` likewise; the round-trip runner keeps its own driver | ran under `yew fl` |
| 36 (config) | `config_*.fl` likewise | ran under `yew fl` |

What deliberately does **not** move: unit tests of pure data structures
(piece tree, undo tree internals, base64, keyid, regex compilation) — they
have no editor to drive and would only get slower; pty goldens — they test
rendering, which batch has none of; fuzzers.

Migration rule: a test moves only when its assertions become *stronger or
equal* in script form; anything that would weaken stays, and `runner.c`'s
header records the sprint each area came from.

### 8. Documentation

- `--help` gains the Deliverable-1 block; `scripts/smoke.sh`'s `batch` row
  changes from "exit 1 naming Sprint 37" to a real run of a two-line script
  asserting stdout, empty stderr, and exit 0 (plus a failing script
  asserting exit 2 and a trace on stderr).
- `docs/batch.md`: the stdio contract table, the exit-code table, the
  interactive-refusal table, and three worked examples — a formatter
  (read/edit/save), a linter (read, print to stdout, exit non-zero via
  `error`), and a bulk rename across a file list. Man-page assembly is
  Sprint 59; this is the source it consumes.

### 9. Defer

- Macro record/replay *UX* under batch (`--replay REG`) → **Sprint 38**; a
  script calls `ed.run("ed.macro.replay", …)` today.
- `--grant NAME:CAP` **enforcement** → **Sprint 54**; parsed and stored here,
  using it errors naming 54.
- `--batch-strict` (nonzero exit on unsaved buffers) → not for 1.0; pinned
  so it is a decision, not an oversight.
- Parallel script-test execution → not for 1.0: the suite is I/O-bound and
  under 20 s, and parallelism would trade a determinism lane for seconds.
- Coverage instrumentation of `.fl` tests → **Sprint 58**'s audit pass.

## Testing Strategy

- **Unit** (`test_batch_args.c`): the full flag grid incl. `--` separation,
  `-` as stdin, missing script, unreadable script, `--test` without
  `--batch`; every exit code produced by a pure argument path.
- **Smoke** (`scripts/smoke.sh`): the updated `batch` rows above, plus
  "stdout carries only script output" (a script that prints one line while
  the editor logs warnings — stdout must be exactly that line).
- **Script** (the runner testing itself, `tests/script/meta_*.fl`): a test
  with zero assertions fails; `t.skip` reports skipped; a deliberately
  failing assertion produces the pinned format; the sandbox is preserved on
  failure and removed on pass; `--filter` with zero matches exits 1.
- **Pty** (`tests/pty/batch_no_tty.c`): run `yew --batch` under a pty;
  assert termios is byte-identical before and after, no alternate-screen
  sequence is ever emitted, and the grid is untouched.
- **Negative**: a seeded `yew_tty_write` call on the batch path exits 4 with
  the function name (proving the guard); a seeded interactive command
  invocation errors naming itself.
- **Torture**: `kill -9` a batch run at 200 random instants during a
  save-heavy script; assert every file on disk is either the old or the new
  content and the journal replays cleanly (invariant 1 in headless form).
- **Determinism**: two `make test-script` runs byte-identical; the script
  suite is in the determinism lane.
- **Perf**: batch startup (no config, one small file, empty script) ≤ 8 ms
  — it is on every script test's critical path and on users' CI; committed
  baseline. Whole script suite ≤ 20 s.

## Definition of Done

1. `make test` green on gcc and clang at `-Werror`; ASan/UBSan/valgrind/
   determinism lanes clean; `make test-script` no longer hard-errors, is a
   `make test` dependency, and is a required CI lane.
2. **No terminal, provably**: the pty test shows byte-identical termios
   before/after a batch run and zero escape bytes written; the grep gate for
   `tcsetattr|tcgetattr|ioctl|isatty` hits only `src/term/tty.c` and every
   hit is guarded; a seeded tty call exits 4 naming the function.
3. `yew_batch_selfcheck` runs unconditionally and fails at **startup** for
   an uninitialized-grid violation (proven by a seeded regression).
4. Stdio contract asserted: stdout carries only script output across a run
   that logs warnings, raises, and saves; stderr carries diagnostics and the
   trace; stdout is flushed on every exit path incl. exit 2 and exit 4.
5. Exit codes 0/1/2/3/4 each produced by a fixture; the exit-2 stderr format
   matches the Deliverable-4 golden byte-for-byte (trace + caret).
6. Batch saves: nothing written without an explicit save; the unsaved-buffer
   warning appears with exit 0; `YEW_SAVE_CHANGED_ON_DISK` surfaces as a
   catchable `"io"` error, never a prompt.
7. Every `YEW_CMD_PROMPTS` / `YEW_CMD_INTERACTIVE` command refuses under
   `--batch` with a message naming itself, and the refusal table has a row
   (with an alternative or an explicit "none") for each — asserted by a
   script that loops over the registry.
8. Script runner: bytewise discovery, per-test process + sandbox isolation,
   `--filter`/`--list` semantics matching s01, sandbox preserved on failure,
   zero-assertion tests failing, pinned output format, two runs
   byte-identical.
9. All 14 `t.*` assertions implemented and each self-tested positively and
   negatively; `t.text`/`t.file` compare bytes (CRLF and invalid-UTF-8
   fixtures prove it).
10. Migration complete for the Deliverable-7 table; s19's
    `tests/unit/test_filter.c` and s21's parked cases are gone from
    `tests/unit/` and present in `tests/script/` with equal-or-stronger
    assertions; `runner.c`'s header records the provenance.
11. Torture: 200 `kill -9` batch runs leave no corrupted file; journals
    replay clean.
12. Perf: batch startup ≤ 8 ms, full script suite ≤ 20 s; baselines
    committed.
13. `docs/batch.md` exists with all three tables and three worked examples;
    `--help` documents every Deliverable-1 flag.
14. ≥ 40 script tests, ≥ 400 script assertions, ≥ 40 unit assertions in
    `test_batch_args.c`; deferred surfaces hard-error naming Sprints 38, 54.
