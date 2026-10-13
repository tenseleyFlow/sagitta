# Sprint 1: Test Infrastructure

## Prerequisites

- Sprint 0 — containers (arena/vec/strmap/intern/bytebuf/sort), `yew_log`
  with the pluggable-sink indirection, `YEW_BUG`/exit-code contract, the
  Makefile with `MODULES` plumbing, driver shell (`--version`, `--help`,
  `--selftest-bug`), provisional `tests/unit/unit_main.c`.
- Binding docs: `.docs/plan/00-decisions.md` (invariants 3, 5; quality
  bars are CI-enforced), `01-architecture.md` §Testing architecture.

## Goals

Make tests and CI first-class before any editor code exists: the bespoke
unit harness (explicit registry table — no `__attribute__((constructor))`,
which the C11 box forbids anyway), assertion macros, a capturing log sink
so tests assert log output structurally, `make test` as the one local
gate, `scripts/smoke.sh` for the driver contract, and the GitHub Actions
skeleton with gcc, clang, ASan/UBSan, valgrind, and determinism
(double-run diff) lanes plus grep-enforced repo-wide bans (qsort,
`__attribute__`, `threads.h`, `exit()` outside `yew_bug`; VLAs are banned
by `-Wvla -Werror` since greps can't see them). Sprint 0's provisional
asserts migrate onto the harness wholesale. The script-test runner is
deferred to Sprint 37 (it needs headless Fletch) and the pty-test runner
to Sprint 6 — but their Makefile targets exist now and hard-error, and
`make fuzz` hard-errors naming Sprint 2.

## Deliverables

### 1. Harness core (`tests/unit/harness.h`, `harness.c`, `registry.c`, `unit_main.c`)

```c
typedef struct { const char *name; void (*fn)(void); } YewTest;
extern const YewTest yew_tests[];      /* tests/unit/registry.c — the   */
extern const size_t  yew_tests_len;    /* ONLY registration site        */
```

- `registry.c` is a single table: `#define T(n) { #n, test_##n }` then
  `const YewTest yew_tests[] = { T(arena_align), T(vec_growth), … };`.
  Test functions are `void test_<name>(void)` in `tests/unit/test_*.c`,
  declared in `tests/unit/tests.h`. Explicit registration is the locked
  design: no ctor attributes, no linker-section tricks — a test exists
  iff it is in the table, and Deliverable 6's cross-check proves the
  table is complete.
- Runner flags: `--filter <substr>` (run matching tests), `--list`
  (names only, exit 0). A filter matching zero tests **exits 1** —
  catches filter typos in CI scripts.
- Per-test isolation: the runner `setjmp`s before each test; assertion
  failure records the failure and `longjmp`s to the next test, so one
  failure cannot cascade into a segfault that hides the rest. Teardown
  after every test: restore the default log sink, reset capture state.
- Output is pinned and deterministic (no timings, no dates — the
  determinism lane diffs it):

```
PASS arena_align
FAIL strmap_order at tests/unit/test_strmap.c:44: YEW_ASSERT_EQ_U64 left=3 right=4
unit: 21 tests, 187 assertions, 1 failure
```

  Exit 0 iff zero failures. Pitfall: print `PASS`/`FAIL` lines in
  registry order, not completion order — the table *is* the order.

### 2. Assertion macros (`tests/unit/harness.h`)

| Macro | Failure detail printed |
|---|---|
| `YEW_ASSERT(cond)` | the literal condition text |
| `YEW_ASSERT_EQ_I64(a, b)` / `_EQ_U64` | both values, decimal |
| `YEW_ASSERT_EQ_STR(a, b)` | both strings, quoted |
| `YEW_ASSERT_EQ_MEM(a, b, n)` | first differing byte offset + hex |
| `YEW_ASSERT_NULL(p)` / `_NOT_NULL(p)` | pointer state |

Each increments the global assertion counter (the DoD counts them), and
on failure reports `file:line`, marks the test failed, and `longjmp`s
out. Pitfall: evaluate arguments exactly once into locals inside the
macro (`do { i64 _a = (a), _b = (b); … } while (0)`) — a double-evaluated
`pop()` in an assertion is a heisenbug factory.

### 3. Capturing log sink (`tests/unit/harness.c`)

```c
void   yew_test_capture_log(void);   /* install capture sink, clear buf */
size_t yew_test_log_count(void);
/* level exact-match + substring search over captured messages */
bool   yew_test_log_contains(YewLogLevel lvl, const char *substr);
```

Built on Sprint 0's `yew_log_set_sink`; captures `(lvl, msg-copy)` pairs
into a vec. Tests assert log behavior structurally — never by reading
the log file (which has timestamps and would poison the determinism
lane). The runner reinstalls the default sink after every test even on
failure (the `longjmp` path runs teardown).

### 4. Makefile: test targets and knobs

- `test`: build `build/unit_tests` (all `src` objects **minus
  `src/main.o`** + `tests/unit/*.c`), run it, then run
  `scripts/smoke.sh ./build/yew`. One target, one local gate.
- `SAN=1`: appends `-fsanitize=address,undefined
  -fno-omit-frame-pointer -O1` to CFLAGS/LDFLAGS. Pitfall: sanitized
  and plain objects must never mix — CI uses `BUILD=build-san` so the
  object trees are disjoint; document this next to the knob.
- `VALGRIND=1`: runs the unit binary under `valgrind --quiet
  --error-exitcode=99 --leak-check=full --errors-for-leak-kinds=definite`.
  Pitfall: ASan and valgrind are mutually exclusive — never both knobs.
- Deferred runners (invariant 3 — present, loud, named):

```make
test-script:
	@echo 'error: script-test runner lands in Sprint 37 (yew --batch)'; exit 1
test-pty:
	@echo 'error: pty-test harness lands in Sprint 6'; exit 1
fuzz:
	@echo 'error: fuzz harness lands in Sprint 2'; exit 1
```

### 5. `scripts/smoke.sh`

Driver-contract checks against `$1` (the built binary), `set -eu`, each
check printing `smoke: <name> ok`. The table:

| Check | Expectation |
|---|---|
| version | line 1 exactly `yew 1.0.0-dev`; exit 0 |
| modules | line 2 is `modules: …` matching `$SMOKE_MODULES` (default `lsp ai fuss plugins`; the CI modules lane passes `none`) |
| help | `--help` exit 0; stdout contains `Usage` |
| unknown flag | `--no-such-flag` exit 1; stderr names the flag |
| file arg | `foo.txt` exit 1; stderr contains `Sprint 14` |
| no args | exit 1; stderr contains `Sprint 14` |
| batch | `--batch x.fl` exit 1; stderr contains `Sprint 37` |
| bug contract | `--selftest-bug` exit 4; stderr contains `internal error at` |
| quiet terminal | `XDG_STATE_HOME=$tmp … --version` writes nothing to stderr; log text never on stdout |
| deferred targets | `make test-script`, `test-pty`, `fuzz` fail naming Sprints 37, 6, 2 |

Pitfall: `set -e` does not fire inside `if cmd; then` — test exit codes
with explicit captures (`rc=0; cmd || rc=$?`) so an unexpected success
fails the check instead of slipping through.

### 6. `scripts/bans.sh` — grep-enforced repo law

Runs over `src/` (and `tests/` where noted); any hit → print offending
lines, exit 1. Exceptions live *in this script* with a comment defending
each — never in the code.

| Ban | Pattern / mechanism | Why |
|---|---|---|
| `qsort` | `grep -rn '\bqsort *(' src tests` | unstable + cross-libc tie order → nondeterminism (use `yew_sort_stable`) |
| `__attribute__` | grep src tests | outside the locked C11 box |
| VLAs | `-Wvla` in CFLAGS (compiler, not grep) | locked decision; greps can't see VLAs |
| `threads.h` / `pthread` | grep src | single-threaded core (invariant 8) |
| `exit(` outside `yew_bug` | `grep -rn '\bexit *(' src \| grep -v util/log.c` | exit-code contract auditable at one site |
| `__DATE__` / `__TIME__` | grep src | breaks byte-identical rebuilds |
| `mmap(` | grep src | locked decision: no mmap (SIGBUS on truncate vs invariant 1) |
| unregistered tests | every `^void test_` def in `tests/unit/test_*.c` appears in `registry.c` | the registry must be complete |

### 7. CI skeleton (`.github/workflows/ci.yml`)

`on: [push, pull_request]`, `ubuntu-latest`, all lanes required:

| Lane | Steps |
|---|---|
| gcc | `make CC=gcc && make test CC=gcc` |
| clang | `make CC=clang && make test CC=clang` |
| sanitize | `make test CC=clang SAN=1 BUILD=build-san` |
| valgrind | `apt-get install -y valgrind; make test VALGRIND=1` |
| modules | `make MODULES="" && SMOKE_MODULES=none scripts/smoke.sh build/yew` |
| determinism | see below |
| bans | `scripts/bans.sh` |

Determinism lane (invariant 5 starts being enforced *now*, on the only
outputs that exist):

```sh
make test > r1.txt 2>&1 ; make test > r2.txt 2>&1 ; diff r1.txt r2.txt
make clean && make && sha256sum build/yew > s1
make clean && make && sha256sum -c s1        # byte-identical rebuild
```

Pitfall: the log file is *excluded* from determinism diffs (timestamps
are allowed there, and only there). Pitfall: the rebuild-hash check is
why Sprint 0 banned `__DATE__` and absolute-path `-D` flags — if this
lane fails, look there first.

### 8. Migration of Sprint 0's provisional tests

`tests/unit/unit_main.c` becomes the runner entry; the plain-asserts
body splits into registered tests: `test_arena_align`,
`test_arena_strdup`, `test_vec_growth`, `test_strmap_order`,
`test_strmap_replace_keeps_order`, `test_intern_roundtrip`,
`test_intern_id_stability`, `test_sort_stable_ties`, `test_bytebuf`,
`test_log_capture`, `test_log_levels`, `test_mod_require_message`,
`test_args_parse_*` (version/help/unknown/file/batch — pure, via the
`Bytebuf` error channel, no subprocess). Nothing from Sprint 0 is
deleted without its assertions landing here first.

### 9. Defer

- Script-test runner (headless Fletch driving fixture edits) → Sprint 37;
  `make test-script` hard-errors until then.
- Pty-test harness (fork under pty, grid snapshots, goldens) → Sprint 6;
  `make test-pty` hard-errors until then.
- Fuzz harness and first fuzzers → Sprint 2; `make fuzz` hard-errors.
- Perf lane and latency gates → budgets become gates as each subsystem
  lands (invariant 4); calibration in Sprint 56. No perf CI yet.

## Testing Strategy

- **Harness self-checks (registered tests):** filter selects the right
  subset; `--list` output matches registry order; zero-match filter
  exits 1 (asserted from smoke.sh via the unit binary, not from inside
  the harness); assertion-failure path runs teardown (test installs the
  capture sink, fails on purpose under an env guard
  `YEW_TEST_SELFCHECK=1` in a forked child, parent asserts exit 1 and
  the pinned FAIL line format).
- **Capture sink:** `test_log_capture` logs at every level, asserts
  count and `yew_test_log_contains`; asserts the default sink is back
  after a teardown cycle.
- **Smoke:** the Deliverable 5 table, wired into `make test`.
- **Bans:** seed a violation locally (`echo 'qsort(x);' > src/tmp.c`),
  confirm `bans.sh` exits 1 naming the line, remove it. Same drill for
  an unregistered `void test_orphan(void)`.
- **CI:** push a branch; all seven lanes green; then verify each lane
  can fail by temporary sabotage (one commit each, reverted): a leak for
  valgrind, a heap overflow for ASan, a `printf("%d", time(NULL))` in
  the runner for determinism.

## Definition of Done

1. `make test` is green locally on gcc and clang. The sub-10-second figure is
   the landing-day unit+smoke budget; the current expanded aggregate follows
   its CI job budget and the dedicated Sprint 37/56 performance contracts.
2. Runner output matches the pinned format exactly; two consecutive
   `make test` runs produce byte-identical stdout.
3. All Sprint 0 provisional assertions live as registered tests: ≥ 18
   tests, ≥ 80 assertions, zero remaining bare-`assert` tests.
4. `--filter` and `--list` work; zero-match filter exits 1.
5. Capturing sink: log tests assert structurally (level + substring);
   no test reads the log file; default sink restored after every test
   including the failure path (self-check test proves it).
6. `scripts/bans.sh` exits 0 on the tree; each seeded violation from
   Testing Strategy is detected (qsort, unregistered test).
7. The semantic registration control proves every unit test is in the
   explicit table and no constructor attribute registers one, while allowing
   comments and syntax-corpus data.
8. `make test-script`, `make test-pty`, and `make fuzz` are live: current
   targets run their shipped owners from Sprints 37, 6, and 2 and their CI
   lanes remain required.
9. CI: gcc, clang, sanitize, valgrind, modules, determinism, bans lanes
   all green on `trunk`; sanitize lane uses `build-san` (disjoint
   object tree); valgrind lane reports zero definite leaks.
10. Determinism lane: double-run `make test` diff clean; clean-rebuild
    binary hash identical.
11. The modules lane's `MODULES=""` build passes smoke with
    `modules: none` and the Sprint 0 `yew_mod_require` message test.
