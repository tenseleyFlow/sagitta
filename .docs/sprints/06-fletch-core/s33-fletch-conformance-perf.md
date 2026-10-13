# Sprint 33: Fletch Conformance Suite and Performance Baselines

## Prerequisites

- Sprint 28 — `.docs/fletch-spec.md`, frozen, sections §1–§16 with stable
  numbers. s28 pinned that *"the conformance suite (s33) is organized by
  section"* and that each § ends with a `Conformance:` line naming the
  `tests/fletch/` file(s) that will cover it. **Those paths were reserved
  then; this sprint creates the files and closes the loop.** s28's DoD 8
  also pins the counts the suite must assert: the error-kind set (11, now 12
  after Sprint 30's §16-A1 added `"limit"`) and the 22 reserved words.
- Sprint 29 — lexer/parser, `fl_parse_literal`, AST dumper, fixtures.
- Sprint 30 — VM, 60 opcodes, GC stress mode, both dispatch modes,
  `fl_disasm_chunk`, `tests/perf/baselines/fletch_smoke.txt`.
- Sprint 31 — the seven modules, `yew fl --list-natives`, capability model,
  module cache and cycle errors.
- Sprint 32 — `yew fl` with the exit-code contract, stack traces, `-e`,
  `-c`, `--dump-ast`, `--dump-bytecode`, the VM fuzzer, `yew_bug` reports.
- Sprint 1 — the unit harness, `make test`, the CI lane set, and the skip
  discipline (skips are never silent). Sprint 6 — the pty harness, for the
  REPL half of the milestone.
- Binding: `00-decisions.md` quality-bar table and its rule that *budgets
  are locked as gates from the sprint that lands each subsystem*;
  `02-fletch.md` requirement 7 (interpreter start ~0, **config load < 1 ms**,
  **motion-op dispatch ~1 µs**); invariants 3, 4, 5.

## Goals

Close Campaign 06: build `tests/fletch/` as a spec-section-organized
conformance suite with a `CHECK`-style directive language, a **coverage
ledger** that mechanically maps spec section → test file and fails CI when
the spec gains a section (or the stdlib gains a function, or the spec gains
an error kind) with no test, and `tests/perf/perf_fletch.c` with committed
baselines and a two-condition regression gate — including the two hard
numeric gates from `02-fletch.md` req 7. **MILESTONE: Fletch hello world** —
`yew fl` runs a script and a REPL session end to end, with the suite green.
Deferred: editor API bindings (Sprint 34) and the recorder round-trip
harness (Sprint 35) — both hard-error until then, and this sprint's fixtures
*assert* that they hard-error, so invariant 3 is tested rather than trusted.

## Deliverables

### 1. Suite layout — `tests/fletch/`

One file per spec section, named by section number so the mapping is
mechanical rather than remembered:

```
tests/fletch/
  run.c                     the runner (make test-fletch)
  01-lexical.fl             02-grammar.fl        03-motion.fl
  04-values.fl              05-expressions.fl    06-statements.fl
  07-scoping.fl             08-functions.fl      09-errors.fl
  10-transactions.fl        11-modules/          12-pure-literal.fl
  13-capability/            14-example.fl        15-style.fl
  errors/                   negative cases, one construct per file
  11-modules/{main.fl,lib/util.fl,cycle_a.fl,cycle_b.fl,cycle_c.fl,
              dup_a.fl,_private.fl}
  13-capability/{plugin_main.fl,helper.fl,config_main.fl}
  ledger.txt                GENERATED + COMMITTED (§3)
```

A section needing several programs (modules, capabilities, negative error
paths) gets a directory; a section covered by one program gets a file. Long
sections may split as `09-errors.fl` + `09-errors-kinds.fl` — the ledger
keys on the `# SPEC:` directive, not the filename, so splitting is free.

Runner contract: `make test-fletch` builds `tests/fletch/run.c`, walks the
tree in `LC_ALL=C` sorted order, executes each file through
`build/yew fl`, applies its directives, and prints one `PASS`/`FAIL`
line per file plus a summary. It is wired into `make test`; the whole suite
must run in **< 20 s** so nobody is tempted to skip it locally.

### 2. Directive language — `tests/fletch/run.c`

Directives are Fletch comments, so every conformance file is also a valid
program: `# NAME: value`, scanned from the whole file.

| Directive | Semantics |
|---|---|
| `# SPEC: §N[.M][, §N…]` | **Required, ≥ 1 per file.** The section(s) this file covers; feeds the ledger |
| `# COVERS: tok tok …` | Ledger tokens: grammar production names (`for_stmt`), stdlib names (`str.slice`), error kinds (`kind:limit`), opcodes (`op:CLOSURE`) |
| `# CHECK: <substr>` | Must match a line of stdout, in order; two CHECKs may not match the same line |
| `# CHECK_NEXT: <substr>` | Must match the line immediately after the previous CHECK's |
| `# CHECK_NOT: <substr>` | Must not appear between the previous CHECK and the next |
| `# OUT: <exact>` | Exact whole-line match, in order — for output that is fully deterministic |
| `# ERROR: <substr>` | Substring on stderr; implies a nonzero exit |
| `# ERROR_KIND: <kind>` | The uncaught error's `kind` field, exactly |
| `# ERROR_LINE: <n>` | Line of the innermost trace frame |
| `# EXIT: <n>` | Exact exit code (default 0) |
| `# CAPS: none \| fs.read,fs.write \| all` | Grant set for the run (default `all`, the CLI origin) |
| `# ORIGIN: cli \| plugin \| config` | Origin for the entry script (default `cli`) |
| `# ARGS: <flags>` | Extra `yew fl` flags (`-c`, `--dump-bytecode`) |
| `# TIMEOUT: <secs>` | Per-file override; default 10 |
| `# GC_STRESS: 0` | Opt out of the stress lane; **requires a trailing reason** |
| `# XFAIL: <XF-id> <reason>` | Expected failure; ID must exist in `.docs/audits/xfail-debt.md`. **XPASS is a hard failure** |
| `# SKIP: <reason>` | Skipped, and the runner prints a `HARNESS_SKIP` line (s01's discipline) |

Two rules carried over from the Cgfried runner because they are what keeps a
directive suite honest:

- **Any `# ALL_CAPS:` comment not in this table is a hard configuration
  error.** A typo'd directive must never degrade into an ordinary comment,
  which is how a test quietly stops asserting anything.
- **Unknown `XFAIL` IDs and unknown selector values are configuration
  errors**, not skips.

One rule that is ours and better than Cgfried's, because we own the lexer:
**directives are scanned from the lexer's comment tokens, not from raw
lines.** `let s = "# CHECK: nope"` is a string, not a directive; a
line-based scanner would fire on it, and the resulting test would pass for
the wrong reason forever. `run.c` calls `fl_lex_next` in a loop and collects
`FL_T_COMMENT` spans. (s29's lexer discards comments; this sprint adds an
opt-in `lex_opts.keep_comments` — a flag, not a new token in the parser's
path.)

Multi-file cases: a directory is one test, entered at `main.fl` (or the
single `*_main.fl`); directives are read from the entry file only.

### 3. The coverage ledger and its CI gate

`tests/fletch/ledger.txt` is **generated by the runner and committed**, the
same discipline `src/unicode/tables.c` uses: CI regenerates and diffs, so
the file in the tree can never drift from reality.

```
# GENERATED by tests/fletch/run.c --ledger — do not edit
§  | section title            | files                       | covers
01 | Lexical structure        | 01-lexical.fl               | 47
02 | Grammar                  | 02-grammar.fl errors/*.fl   | 38
...
16 | Amendment log            | 09-errors-kinds.fl          | 1
-- totals: sections 16/16  productions 38/38  natives 157/157
   kinds 12/12  opcodes 60/60
```

`scripts/check-fletch-coverage.sh` is the gate, and each check names what
to do about a failure:

| # | Check | Source of truth |
|---|---|---|
| 1 | every `## §N` heading in `.docs/fletch-spec.md` has ≥ 1 file whose `# SPEC:` names it | the spec file itself — **this is the "spec gains a section, CI fails" requirement** |
| 2 | every nonterminal defined in the §2 EBNF block appears in some `# COVERS:` | the spec's own fenced EBNF, parsed for `^name *=` |
| 3 | every native appears in some `# COVERS:` | `yew fl --list-natives` (s31), diffed |
| 4 | every error kind has ≥ 1 `# ERROR_KIND:` case | the spec's §9 kind list, parsed |
| 5 | every opcode appears in some `# COVERS: op:NAME` | `src/fl/opcodes.def`, parsed |
| 6 | every `Conformance:` line in the spec names a file that exists | the spec |
| 7 | regenerated `ledger.txt` is byte-identical to the committed one | `git diff --exit-code` |

Checks 1 and 6 are the ones that matter over the campaign's life: they make
the *spec* the driver. Amending the spec (§16) without adding a test breaks
the build in the same commit, which is the only moment anyone is willing to
write the test.

Failure output names the gap and nothing else: `check 3: native
'str.pad_end' has no COVERS token (add it to tests/fletch/04-values.fl or a
new file)`.

### 4. Coverage content requirements

Beyond the mechanical gate, these are the human requirements the suite must
satisfy — each is a DoD row:

- **Every grammar production** from §2 exercised positively at least once,
  and every production with a plausible mistake exercised negatively in
  `errors/` (s29's four special-cased diagnostics each keep a fixture here
  too, now asserted end to end through `yew fl` rather than through the unit
  harness).
- **Every stdlib function** called at least once with a checked result, and
  **every documented error case** from the s31 tables raised at least once
  with `# ERROR_KIND:`.
- **Every documented error path**: all 12 kinds; the `try`/`catch` law; the
  transaction law of §10 in its headless form (`edit` is a plain block
  without a host); the capability-denial path; the import-cycle message.
- **`14-example.fl` is byte-identical to the spec's §14 fenced block.** The
  runner extracts the block from `.docs/fletch-spec.md` and diffs it — the
  worked example cannot rot into something the spec no longer shows. Its
  normative results, pinned by s28's testing strategy, are asserted:
  `total == 17`, `next()` returns 9 then 10, `dup` is defined, and `shout`
  headless raises `"motion"` caught → returns `"MOTION"`.
- **Counts asserted, not printed** (s28 DoD 8): 22 reserved words, 12 error
  kinds, 16 sections, 60 opcodes.
- **Pure-literal safety** (§12): the negative list is re-asserted through
  `fl_parse_literal` — no identifier, call, `@[`, import, or `fn`
  production is reachable.

### 5. Bench suite — `tests/perf/perf_fletch.c`

```
make bench-fletch                     # run, print the table
make bench-fletch PERF_GATE=1         # also enforce the gates (perf lane)
make bench-fletch BASELINE=dev        # which baseline file to compare
```

| Bench | What it measures | Gate |
|---|---|---|
| `startup` | `main` → VM initialized → exit, on an empty script (`fl_vm_init` + `fl_std_register`) | **≤ 2 ms**, hard — the Fletch share of the 20 ms cold-start budget |
| `config_load` | parse + compile + run `tests/perf/fixtures/init_bench.fl` (≈ 400 lines: options map, 40 `bind` calls, 6 functions, 2 imports) | **< 1 ms**, hard — `02-fletch.md` req 7 |
| `motion_dispatch` | 1 000 000 `FL_OP_MOTION` executions of a 1-word program against `fl_host_null` | **≤ 1 µs per op**, hard — req 7 |
| `fib27` | `fn fib(n)` recursive to 27 — call/return, frames, int arithmetic | baseline |
| `strbuild_join` | build 20 000 fragments in a list, `str.join` once | baseline |
| `strbuild_concat` | the naive `s = s + x` loop, 5 000 iterations | baseline **plus** an asserted ratio: `concat/join > 20` at n=5 000, documenting the O(n²) shape rather than pretending it is fast |
| `map_heavy` | 100 000 inserts then 100 000 lookups on one insertion-ordered map | baseline |
| `list_sort` | `list.sort` over 100 000 random ints, default comparator | baseline |
| `gc_pause` | worst single collection pause during `map_heavy` | baseline (p99 recorded; becomes a gate in Sprint 56) |
| `parse_only` | parse + AST-build of a 5 000-line generated file | baseline |

Method, pinned so numbers mean the same thing every run:

- Each bench runs **11 times**; the reported figure is the **median**, with
  the min also recorded. Median, not mean: one scheduler hiccup should not
  move the number, and the mean is the metric that makes people add
  `sleep` to their CI.
- A warmup iteration is discarded. `math.random`'s seed is the fixed default
  (s31), so inputs are identical across runs.
- Benches run with **GC stress off** and with `FL_CGOTO` at its probed
  default; the perf lane additionally records a `cgoto=0` column so the
  extension's value is measurable rather than assumed.
- Timing is `clock_gettime(CLOCK_MONOTONIC)`; no `rdtsc`, no libc timers
  that vary by target.

Baselines live at `tests/perf/baselines/fletch_<runner>.txt`, default
`dev`, format one row per bench:

```
# bench            median_ns   min_ns    why
config_load          812340     790112    s33 initial; gate 1000000
motion_dispatch         740        712    s33 initial; gate 1000
```

**Regression-gate policy.** With `PERF_GATE=1` a bench fails when
**both** `median > baseline_median × 1.15` **and** `min > baseline_min ×
1.10`. Two conditions, because a single noisy sample must not redden the
build, but a genuine regression moves the whole distribution. Hard-gated
benches (`startup`, `config_load`, `motion_dispatch`) fail additionally and
unconditionally on their absolute budget, regardless of the baseline —
`00-decisions.md` says budgets are gates, so a "baseline" that drifted past
1 ms is not a defence.

Rebaselining requires a commit that edits the baseline file, and the `why`
column must be filled in with a reason (`s41: highlighter added 3 natives to
the config path`). The checker rejects a row whose `why` is empty or
unchanged from `s33 initial` when the numbers moved by more than 5 %.
Sprint 56 re-calibrates every row on the designated reference hardware; the
gate mechanism does not change, only the numbers.

**Pitfall — `motion_dispatch` against `fl_host_null` measures dispatch, not
editing.** It is the right number for req 7 (the VM's share of a motion op)
and the wrong number to quote as "yew executes a motion in 740 ns".
The bench file says so in a comment, and Sprint 34 adds a second row against
the real host.

### 6. MILESTONE: Fletch hello world

The milestone is proven by four artifacts, all in CI:

1. `printf 'io.print("hello, world")\n' > $t.fl && yew fl $t.fl` prints
   exactly `hello, world` and exits 0 (`scripts/smoke.sh`).
2. `yew fl -e 'io.print("hello, world")'` does the same.
3. A pty golden (s06) of an interactive session: `fl> io.print("hello,
   world")` → `hello, world` → `:quit`, grid byte-compared.
4. `make test-fletch` green with a nonzero, asserted file count, and
   `scripts/check-fletch-coverage.sh` exiting 0.

`--help` documents `yew fl`, and the README gains the four-line "your first
Fletch program" block. Campaign 06 is closed when these are green.

### 7. Defer

- **Editor API bindings** → Sprint 34. `tests/fletch/34-editor.fl` exists
  **now** and asserts the deferral: calling `bind("H","s",fn(){})` produces
  `# ERROR_KIND: name` and `# ERROR: lands in Sprint 34`. When Sprint 34
  lands, that file is rewritten into real coverage and the ledger picks it
  up. Testing the hard-error is how invariant 3 stops being a promise.
- **The recorder round-trip harness** → Sprint 35. `tests/fletch/roundtrip/`
  exists with a `README` and no cases; `make test-fletch-roundtrip`
  hard-errors `round-trip law harness lands in Sprint 35 (invariant 10)`.
- **Motion *execution*** → Sprint 34: `03-motion.fl` covers lexing, parsing,
  compilation to `FL_OP_MOTION`, and the headless `"motion"` raise. Its
  `# COVERS:` tokens for the motion vocabulary are complete now, so the
  ledger will not need re-auditing in Sprint 34.
- **`--batch` conformance** → Sprint 37; **config-load gate against the real
  default config** → Sprint 36 (the `init_bench.fl` fixture is a stand-in
  and says so at the top; s36 swaps it for the shipped default and
  re-baselines in the same commit).
- **Reference-hardware calibration** → Sprint 56; baselines here are
  dev-machine numbers, labelled as such.

## Testing Strategy

- **The suite is the test.** Beyond it, the runner needs its own meta-suite
  (`tests/fletch/meta/`), because a test runner that silently passes is
  worse than no runner. Meta-cases, each an expected runner outcome:
  a failing `CHECK`; a `CHECK_NEXT` that matches a non-adjacent line; a
  firing `CHECK_NOT`; a wrong `EXIT`; a wrong `ERROR_KIND`; a typo'd
  directive (`# CHEK:`) → configuration error; an unknown `XFAIL` ID →
  configuration error; an `XFAIL` that passes → XPASS hard failure; a
  `SKIP` printing exactly one `HARNESS_SKIP` line; a directive inside a
  string literal → **not** interpreted; a missing `# SPEC:` → configuration
  error. Expected totals are asserted as counts
  (`meta: total=11 pass=… fail=… config=…`), Cgfried-style.
- **Coverage-gate meta-tests**: run `check-fletch-coverage.sh` against a
  scratch tree with (a) a spec section that has no test, (b) a native with
  no `COVERS`, (c) an error kind with no case, (d) a stale `ledger.txt` —
  each must fail with the check number and the missing item named.
- **GC-stress lane**: the whole suite under `FL_GC_STRESS=1`, minus files
  carrying a justified `# GC_STRESS: 0`. The count of opt-outs is printed
  and asserted ≤ 2, so the escape hatch cannot quietly become the norm.
- **Dispatch differential**: the whole suite under `FL_CGOTO=0` and
  `FL_CGOTO=1`, outputs byte-compared (s30 built the harness; this sprint
  points it at the full corpus).
- **Determinism lane**: the suite twice, stdout+stderr byte-compared;
  `ledger.txt` regenerated twice, byte-compared.
- **Sanitize + valgrind lanes**: whole suite, zero findings, zero definite
  leaks.
- **Perf lane**: `make bench-fletch PERF_GATE=1` on the designated runner;
  other lanes run `make bench-fletch` without gating (correctness only —
  the benches must still *run* everywhere, or they rot).

## Definition of Done

1. `tests/fletch/` contains ≥ 20 conformance files/directories covering all
   16 spec sections; `make test-fletch` is green and part of `make test`,
   and the whole suite completes in < 20 s on the dev machine.
2. `scripts/check-fletch-coverage.sh` exits 0, and all seven checks are
   proven to fire by a seeded violation in the meta-suite (spec section
   without a test, native without `COVERS`, kind without a case, stale
   ledger, missing `Conformance:` target, undefined production, unknown
   opcode).
3. `ledger.txt` is committed and `run.c --ledger && git diff --exit-code
   tests/fletch/ledger.txt` is clean from a fresh clone.
4. Totals in `ledger.txt`: sections 16/16, EBNF productions 38/38 (or
   whatever §2 defines — the count comes from the spec, not from this file),
   natives N/N matching `yew fl --list-natives`, kinds 12/12, opcodes 60/60.
5. `14-example.fl` is byte-identical to the spec §14 block (diffed by the
   runner) and asserts all four normative results (`total == 17`; 9 then 10;
   `dup` defined; `shout` → `"MOTION"`).
6. Counts asserted by fixtures, not printed: 22 reserved words, 12 error
   kinds, 16 sections, 60 opcodes.
7. Meta-suite: 11 cases, expected totals asserted; a directive inside a
   string literal is proven not to fire; a typo'd directive is a
   configuration error.
8. Hard perf gates green on the dev machine and enforced in the perf lane:
   `config_load < 1 ms`, `motion_dispatch ≤ 1 µs/op`, `startup ≤ 2 ms`.
9. `tests/perf/baselines/fletch_dev.txt` committed with all 10 rows, each
   carrying a `why`; the two-condition regression gate proven by a seeded
   20 % slowdown failing and a seeded 5 % slowdown passing.
10. `strbuild_concat/strbuild_join > 20` asserted at n = 5 000, with the
    comment explaining that the quadratic path is documented, not fixed.
11. GC-stress and both `FL_CGOTO` settings green over the whole suite;
    `# GC_STRESS: 0` opt-outs ≤ 2 and each carries a reason.
12. The landing deferrals were tested rather than stubbed. Sprints 34, 35,
    and 37 subsequently replaced those guards with live editor bindings, the
    recorder round-trip harness, and `yew --batch`; their shipping suites now
    provide the release-closeout evidence.
13. **MILESTONE proven**: the four artifacts of §6 are green in CI —
    `yew fl hello.fl`, `yew fl -e`, the REPL pty golden, and the coverage
    gate. `--help` and the README document `yew fl`.

---

## Corrections after landing — 2026-08-09

Five clauses were wrong about the sprint they were describing and are
corrected here rather than silently worked around. None touches the
LANGUAGE, so none goes through s28 §16's amendment registry: that
registry is for `.docs/fletch-spec.md`.

**C1 — the XFAIL debt list is tracked, at `tests/fletch/xfail-debt.txt`.**
§2 names `.docs/audits/xfail-debt.md`. That file does not exist (it is
Sprint 58's), and more importantly `.docs/` is **gitignored by project
decision**, so a gate that reads its id list passes locally and fails
in CI on the first `# XFAIL:` — or has to fail open, which defeats the
rule that an unknown id is a configuration error. The list therefore
lives beside the suite it governs. Sprint 58 may consolidate.

**C2 — the hello world is two lines: `import io` then the print.**
§6's snippet is `printf 'io.print("hello, world")\n'`, which does not
run: spec §11's first sentence makes the builtins **imported, not
ambient**. The spec outranks the sprint file (`00-decisions.md` reading
order), and making `io` implicitly global would be a one-line change
that falsifies §11. The milestone is the whole two-line program.

**C3 — the ledger counts 42 productions, not 38, and the count comes
from every EBNF block.** §2 defines 37 nonterminals and §12 defines
five more (`pl_file` and friends) in a block of its own. DoD 4 already
says the count comes from the spec rather than from the sprint file;
this records what the spec actually says. Check 2 reads all `ebnf`
blocks, so §12's five are covered rather than exempt.

**C4 — `init_bench.fl` is 135 lines, not ≈400.** §5 estimates the
fixture at ~400 lines and then itemises it: an options map, 40 `bind`
calls, 6 functions, 2 imports. The itemisation is the contract and it
is met exactly; written at the style §15.2 requires it comes to 135
lines. `bind` is defined **locally** in the fixture rather than called
for real, because the editor API hard-errors until Sprint 34 and the
fixture would otherwise measure forty raises. Sprint 36 replaces the
whole file anyway.

**C5 — `gc_pause` is recorded, never regression-gated.** §5's table
already says "baseline (p99 recorded; becomes a gate in Sprint 56)",
but the two-condition gate applied to every row with a baseline. A
single collection's worst case swings by 2x run to run on an idle
machine, so gating it flaps the build weekly and teaches everyone to
ignore the perf lane. It is measured like every other row — median and
min of eleven runs — and excluded from the comparison.

### A sixth thing, superseded by Sprint 56

Sprint 33 originally enforced absolute wall-clock budgets outside the perf
runner. Sprint 56's later runner policy supersedes that deviation: hosted
and unknown runners use `--gate-budgets` with `YEW_PERF_ADVISORY=1`, so
ordinary timing overruns warn while zero, >100x-budget, functional, and
deterministic failures remain hard. `PERF_GATE=1` preserves strict absolute
and relative enforcement on designated hardware.

## Definition of Done — 12/13 walked, 13 met

| # | item | verdict |
|---|---|---|
| 1 | ≥ 20 files covering all 16 sections; `make test-fletch` green and part of `make test`; whole suite < 20 s | **PASS** — 37 units, 16/16 sections, ~0.2 s on the dev machine |
| 2 | `check-fletch-coverage.sh` exits 0; all seven checks proven by a seeded violation | **PASS** — `check-fletch-gate-selftest.sh`, eight seeds (check 6 twice: a missing target and §14 drift) |
| 3 | `ledger.txt` committed; regenerating leaves no diff from a fresh clone | **PASS** — check 7 regenerates to a temp file and `cmp`s, so a failing gate never rewrites the artifact it is checking |
| 4 | totals: 16/16 sections, N/N productions, N/N natives, 12/12 kinds, 60/60 opcodes | **PASS** — 16/16, **42/42** (C3), 117/117, 12/12, 60/60 |
| 5 | `14-example.fl` byte-identical to the §14 block and asserts all four normative results | **PASS** — the runner diffs the file's PREFIX against the spec's fenced block, because the same DoD requires assertions after it |
| 6 | counts asserted, not printed: 22 reserved words, 12 kinds, 16 sections, 60 opcodes | **PASS** — `15-style.fl`, `16-amendments.fl`, and checks 1/4/5 |
| 7 | meta-suite: 11 cases, totals asserted; a directive in a string proven not to fire; a typo is a config error | **PASS** — `total=11 pass=1 fail=6 skip=1 xfail=0 config=3`, asserted as one line |
| 8 | hard gates green and enforced: config_load < 1 ms, motion_dispatch ≤ 1 µs/op, startup ≤ 2 ms | **PASS** — 177 µs, 296 ns/op, 45 µs; 5.6x, 3.4x and 44x headroom |
| 9 | 10 rows committed with a `why`; the two-condition gate proven by a 20 % failure and a 5 % pass | **PASS** — `--selftest-gate`, 7 cases; **proven on the rule, not on a stopwatch**, because a live re-measurement tests the machine (a loaded box swings parse_only 80 %) |
| 10 | `concat/join > 20` at n = 5000, with the comment | **PASS** — 52x, asserted in the driver |
| 11 | GC-stress and both `FL_CGOTO` settings green; `# GC_STRESS: 0` opt-outs ≤ 2 | **PASS** — `test-fletch-gc-stress`, `test-fletch-dispatch` (byte-identical), **zero** opt-outs. No file needs one, so the directive has no user in the suite; it was validated by hand against a scratch tree — a missing reason is a configuration error, and an opt-out is announced with a `HARNESS_SKIP` line. Manufacturing an opt-out to exercise the directive would be gaming the number this row asks for |
| 12 | landing deferrals tested, then replaced by owning sprints | **PASS** — Sprints 34, 35, and 37 delivered editor bindings, recorder round-trip, and `yew --batch` with shipping coverage |
| 13 | MILESTONE: the four artifacts green; `--help` and the README document `yew fl` | **PASS** — smoke script (script + `-e`), pty golden `s33_hello_world_repl`, coverage gate; README gained "Your first Fletch program" |

### Defects this sprint found in landed code

Six, all in code Sprints 30–32 shipped green, and every one found by a
conformance file asserting something the spec says. (Three more were
in code this sprint wrote and were caught by its own gates: an ASan
global-buffer-overflow from a hand-counted length in a pty case, 121
LeakSanitizer allocations in the runner, and a free() of stack memory
the moment the first `# ARGS:` case exercised that path.)

1. **`map.clear` left `floor(n/2)` entries behind** for n ≥ 3.
   `fl_map_del` compacts once `ndead > n/2`, which moves live entries
   down past the caller's cursor, so the clear walk stopped early. The
   landed unit test used n=2 — the one size where the arithmetic comes
   out right. New `fl_map_clear` primitive.
2. **`for k, v in <list>` raised** `two-variable for needs a map`. §6
   requires a list's index/value pairs in as many words.
3. **`for x in <string>` raised** `cannot iterate str`. Third clause of
   the same §6 sentence.
4. **A two-deep closure segfaulted.** `comp_function` copied the
   compiled child's capture descriptors over the ENCLOSING compiler's
   `upvals` array. Invisible one level down; a NULL deref two levels
   down, because the middle function's real descriptor was replaced by
   the inner one's and the outer `CLOSURE` emitted a pair into an
   upvalue array that did not exist.
5. **§9's `error(v)` had no implementation at all.** The only
   documented way for a program to raise, and `kind: "user"` was
   therefore unreachable. Now a prelude name, consulted after the
   caller's own globals so a program may shadow it.
6. **`io.print` stair-stepped in the REPL.** Sprint 32 gave the prompt
   an LF→CRLF translator for its own output, but `io.print` writes to
   stdout from inside the VM and never passed through it. The prompt
   now keeps `OPOST|ONLCR` while raw and the kernel translates for
   every writer.

### Two divergences pinned rather than fixed

Neither is this sprint's to change; both are now asserted, so changing
either is a deliberate act with a failing test attached.

- **`io.glob` resolves relative paths against the importing module's
  directory; `io.read`/`io.write` resolve against the process CWD.**
  s31 specified glob's root explicitly ("never the process cwd — a
  config must behave the same whatever directory yew was launched
  from") and left the other half unstated. The same relative string
  therefore names two different files depending on which function
  receives it. `13-capability/config_main.fl` asserts both halves.
- **`str.split`'s `limit` counts PIECES; `re.split`'s counts SPLITS.**
  One apart for the same intent. s31 specified the first as "caps
  parts" and left the second unstated, and re.split's behaviour is
  pinned by its own landed unit test. `11-modules-re.fl` records it.
