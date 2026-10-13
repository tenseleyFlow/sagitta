# Closeout: F15 — CI and release engineering

Date:            2026-10-12
Baseline commit: 4c99e13474c84d75eec3250d61412b53fa41ae00
Reviewer:        UNASSIGNED
Sprints covered: s00, s01, s37, s56, s57

## DoD items (every numbered item from every sprint above)

- [x] XFAIL.N Active expected-failure ceiling: 8.
      — EVIDENCE: `.docs/audits/xfail-debt.md` has zero non-fixed rows and `tests/fletch/xfail-debt.txt` has no IDs.
- [ ] s00.1 `make` produces `build/yew` and `build/yew` (symlink) with gcc and clang, zero warnings, `-std=c11 -pedantic -Wall -Wextra -Werror -Wvla` on every TU.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s00.2 `./build/yew --version | head -1` is exactly `yew 1.0.0-dev`; line 2 is `modules: lsp ai fuss plugins`. A `make MODULES=""` build prints `modules: none`. Exit 0 both.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s00.3 `./build/yew --help` exits 0 with sectioned stdout; unknown flag exits 1 naming the flag; `./build/yew foo.txt` and a bare `./build/yew` exit 1 with stderr naming Sprint 14; `--batch x.fl` names Sprint 37; `--clean` names Sprint 36.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s00.4 `./build/yew --selftest-bug` exits 4; stderr matches `yew: internal error at src/…:<line>: selftest` plus the report line; the same report appears in the log file.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s00.5 `grep -rn '\bexit(' src/ | grep -v util/log.c` returns nothing; `main()` is the only return-to-OS site.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s00.6 `grep -rn 'qsort\|__attribute__\|threads\.h' src/` returns nothing.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s00.7 `XDG_STATE_HOME=$(mktemp -d) ./build/yew --version >out 2>err` leaves `err` empty and `out` exactly the two version lines; the log dir, if created, is mode 0700. No yew invocation writes log text to stdout/stderr.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s00.8 Provisional unit binary runs green under `make test`; ≥ 12 container/log assertions including the insertion-order and stable-sort determinism cases.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s00.9 Rebuilding with a different `MODULES` value (no `make clean`) relinks correctly — `mods.stamp` forces it; `--version` reflects the new set.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s00.10 `make install DESTDIR=$(mktemp -d) PREFIX=/usr` installs `bin/ yew` and the `bin/yew` symlink; `make clean` removes `build/`.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s00.11 LICENSE (GPL-3.0 verbatim), README, `.gitignore` present; `git status` clean after `make && make test` (nothing generated outside `build/`).
      — VERDICT: blocking — independent closeout review pending.
- [ ] s01.1 `make test` builds and runs the harness + smoke.sh; exit 0 locally on gcc and clang; total wall time < 10 s.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s01.2 Runner output matches the pinned format exactly; two consecutive `make test` runs produce byte-identical stdout.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s01.3 All Sprint 0 provisional assertions live as registered tests: ≥ 18 tests, ≥ 80 assertions, zero remaining bare-`assert` tests.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s01.4 `--filter` and `--list` work; zero-match filter exits 1.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s01.5 Capturing sink: log tests assert structurally (level + substring); no test reads the log file; default sink restored after every test including the failure path (self-check test proves it).
      — VERDICT: blocking — independent closeout review pending.
- [ ] s01.6 `scripts/bans.sh` exits 0 on the tree; each seeded violation from Testing Strategy is detected (qsort, unregistered test).
      — VERDICT: blocking — independent closeout review pending.
- [ ] s01.7 `grep -rn 'constructor' tests/ src/` returns nothing — registration is the explicit table only.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s01.8 `make test-script`, `make test-pty`, `make fuzz` each fail with a message naming Sprints 37, 6, 2 respectively.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s01.9 CI: gcc, clang, sanitize, valgrind, modules, determinism, bans lanes all green on `trunk`; sanitize lane uses `build-san` (disjoint object tree); valgrind lane reports zero definite leaks.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s01.10 Determinism lane: double-run `make test` diff clean; clean-rebuild binary hash identical.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s01.11 The modules lane's `MODULES=""` build passes smoke with `modules: none` and the Sprint 0 `yew_mod_require` message test.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s37.1 `make test` green on gcc and clang at `-Werror`; ASan/UBSan/valgrind/ determinism lanes clean; `make test-script` no longer hard-errors, is a `make test` dependency, and is a required CI lane.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s37.2 **No terminal, provably**: the pty test shows byte-identical termios before/after a batch run and zero escape bytes written; the grep gate for `tcsetattr|tcgetattr|ioctl|isatty` hits only `src/term/tty.c` and every hit is guarded; a seeded tty call exits 4 naming the function.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s37.3 `yew_batch_selfcheck` runs unconditionally and fails at **startup** for an uninitialized-grid violation (proven by a seeded regression).
      — VERDICT: blocking — independent closeout review pending.
- [ ] s37.4 Stdio contract asserted: stdout carries only script output across a run that logs warnings, raises, and saves; stderr carries diagnostics and the trace; stdout is flushed on every exit path incl. exit 2 and exit 4.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s37.5 Exit codes 0/1/2/3/4 each produced by a fixture; the exit-2 stderr format matches the Deliverable-4 golden byte-for-byte (trace + caret).
      — VERDICT: blocking — independent closeout review pending.
- [ ] s37.6 Batch saves: nothing written without an explicit save; the unsaved-buffer warning appears with exit 0; `YEW_SAVE_CHANGED_ON_DISK` surfaces as a catchable `"io"` error, never a prompt.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s37.7 Every `YEW_CMD_PROMPTS` / `YEW_CMD_INTERACTIVE` command refuses under `--batch` with a message naming itself, and the refusal table has a row (with an alternative or an explicit "none") for each — asserted by a script that loops over the registry.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s37.8 Script runner: bytewise discovery, per-test process + sandbox isolation, `--filter`/`--list` semantics matching s01, sandbox preserved on failure, zero-assertion tests failing, pinned output format, two runs byte-identical.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s37.9 All 14 `t.*` assertions implemented and each self-tested positively and negatively; `t.text`/`t.file` compare bytes (CRLF and invalid-UTF-8 fixtures prove it).
      — VERDICT: blocking — independent closeout review pending.
- [ ] s37.10 Migration complete for the Deliverable-7 table; s19's `tests/unit/test_filter.c` and s21's parked cases are gone from `tests/unit/` and present in `tests/script/` with equal-or-stronger assertions; `runner.c`'s header records the provenance.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s37.11 Torture: 200 `kill -9` batch runs leave no corrupted file; journals replay clean.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s37.12 Perf: batch startup ≤ 8 ms, full script suite ≤ 20 s; baselines committed.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s37.13 `docs/batch.md` exists with all three tables and three worked examples; `--help` documents every Deliverable-1 flag.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s37.14 ≥ 40 script tests, ≥ 400 script assertions, ≥ 40 unit assertions in `test_batch_args.c`; deferred surfaces hard-error naming Sprints 38, 54.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s56.1 `make && make test && make perf` green on gcc and clang, zero warnings under the locked flags; ASan/UBSan, valgrind, determinism, bans green.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s56.2 `sizeof(ProfFrame) == 64` asserts at compile time; the complete measured profiler phase-clock/ring path costs ≤ **2 %** of external keypress p99 over the 10 000-key `typing` session; with `YEW_PROF` unset the ring is never allocated.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s56.3 `:prof`, `:prof reset`, `:prof dump`, `:prof mark`, `:prof frames` are `ed.prof.*` commands reachable from E mode, a key and Fletch; all five refuse with the pinned message when profiling is off. The report golden is byte-identical across two runs from a synthetic ring, and no float formatting exists in `src/util/prof.c`.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s56.4 `make calib` produces a scale on every lane; both reference vectors are committed **with their machines described in prose**; the refusal band and the 15 % drift abort fire under unit test and a seeded CI vector.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s56.5 Every §5 row is within budget on the designated runner: keypress p99 ≤ 5 ms on all seven session × class combinations; cold start ≤ 20 ms with and without `--clean` **and under the `dumb` profile**; 100 MB open ≤ 150 ms end to end; scroll ≥ 120 fps equivalent with highlighting on; incremental search ≤ 5 ms/keystroke on 1 GiB; every RSS row inside its ceiling.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s56.6 The external-vs-`YEW_PROF` p99 cross-check agrees within 25 % on all seven combinations after subtracting the contemporaneous median PTY echo floor from the external endpoint, and `perf_latency --floor` reports a plausible `pty_floor_p50`.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s56.7 Frames-per-10 000-keys ≤ 10 000 is a **hard** gate on every lane, proven to fail when a render is moved inside the key loop.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s56.8 Advisory mode proven: a seeded 2 ms slowdown fails the designated lane and passes (warning only) on the gcc lane, while the deterministic rows fail on both.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s56.9 Baselines committed for both designated runners in `tests/perf/baselines/` with the v2 header and a `why` per row; `tests/perf/baselines/` (singular) is gone and `bans.sh` rejects it; s05's and s41's rows live in the new home.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s56.10 Anti-flap proven exactly as s11 §8 specifies: median-of-3 with 2-of-3-to-fail, five consecutive unmodified runs with zero failures, a seeded 5 % slowdown passing, a seeded 20 % failing, and `perf-baseline-guard` rejecting a mixed `src/` + baseline commit.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s56.11 `tests/perf/sessions/` holds five committed 10 000-key `.keys` scripts; every §5.1 fixture class is exercised, and the `assist` class runs against the s46/47 and s49 mocks with ghost text asserted present in the VT screen, not assumed.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s56.12 `tests/perf/README` carries the §7 triage table and the "read the phase table before reaching for perf" rule; `make perf-symbols` builds a disjoint `build-prof/` tree.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s56.13 The `100m-allnl` exemption is discharged in writing per §5.3 — s11's "revisited in Sprint 56" resolves to a decision, not another deferral.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s56.14 `make size` hard-errors naming Sprint 57; ≥ 140 assertions across `test_prof.c`, `test_prof_format.c` and `test_calib.c`.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s57.1 `make` and `make test` green on gcc and clang; the pinned GCC `make size` lane green; zero warnings under the locked flags; ASan/UBSan, valgrind, determinism, bans, size, alloc, musl, embedded, arm64-linux and arm64-macos lanes all green on `trunk`.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s57.2 `make size` enforces all eight §1 rows; **full ≤ 2.0 MiB** and **minimal ≤ 1,576,960 bytes** stripped on x86_64-linux-gnu, with the measurement definition (stripped, on-disk, `-O2`) stated in `tests/size/README` and the four single-module budgets carrying their derivation in `why`.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s57.3 The additivity check passes: `full` ≤ Σ(single-module deltas) + 5 %.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s57.4 `scripts/size-ledger.sh` produces the §2.2 format; `ledger-full.txt` and `ledger-minimal.txt` are committed, regenerate byte-identically, and `git diff --exit-code tests/size/` is clean from a fresh clone. `unattributed` ≤ 2 %; `link_overhead` is reported as its own line and never distributed; `.text.*` folding proven by the musl ledger being non-empty.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s57.5 Per-bucket growth gate proven both ways: a seeded 40 KiB array fails naming its symbol; `make size-update` in its own commit clears it; and `perf-baseline-guard` rejects a commit touching both `src/` and `tests/size/`.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s57.6 `ALLOCDBG=1` builds in `build-adbg`; all named §3.2 rows green, including **zero allocations per frame during steady-state typing**, zero per `yew_render_frame`, zero per `yew_syn_line`, and zero per `yew_re_exec`; a seeded per-frame allocation fails and names its `file:line`.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s57.7 All §3.3 RSS rows green, including 1.6× on `100m-code` for the full, minimal **and** musl-static builds.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s57.8 The §4.2 audit table is fully enforced: every "banned" row has a `bans.sh` entry proven by a seeded violation; `nm -u build-musl/yew` is empty; `readelf -d` shows no `NEEDED`; `ldd` reports no library beyond musl's static-PIE self-relocating loader; `file` reports `static-pie linked`; s02's locale/iconv/ wide-char ban re-greps clean.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s57.9 The §4.3 rule is implemented and tested: literal addresses never reach the resolver, every `getaddrinfo` result is tried in order, and the s48 client suite passes with a full `/etc/hosts`, a minimal one, and none — with no hang in any case.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s57.10 The musl build is byte-identical across two runs (`--build-id=none`), within its §1 budgets, and `gc_saved_bytes` is recorded in the ledger header.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s57.11 **MILESTONE: embedded proof** — `make embedded-gate` green: image ≤ 12 MiB, all twelve checklist rows pass inside a **64 MiB** QEMU guest, peak `VmHWM` ≤ 24 MiB, and the 32 MiB run degrades with a clean named error and no OOM kill. The hardware corroboration run (Pi Zero 2 W class, 512 MB) is recorded in `.docs/embedded-runs.md` with its numbers and its date.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s57.12 Minimal-build matrix verified by all four §6 checks: zero excluded-module symbols in `nm`, exactly one `shim.o` per module on the link line, header-to-shim parity enforced by `bans.sh`, and every shim entry point returning a failure whose message names **its own** module (`lsp`, `ai`, `fuss`, `plugins` — asserted per module, not generically). `MODULES=""` passes the full `make test`, `test-pty`, `test-fletch` and `test-script` suites and edits, highlights, searches, saves, resumes a workspace, and records and replays a macro.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s57.13 `arm64-linux` and `arm64-macos` lanes are **required** and green, including the pty suite; the alignment sanitizer pass is clean on arm64; the `ru_maxrss` unit normalization is asserted on macOS; and the macOS-terminal limitation of §7.3 is written into `.docs/release-checklist.md` as a manual Sprint 60 item rather than claimed as covered.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s57.14 `make TARGET=<anything outside the closed four>` hard-errors listing the supported targets; `grep -rn 'dlopen\|strerror_r\|backtrace\|long double\|getopt_long' src/` is empty; `test_alloc.c`, `test_mod.c`, the shim parity/honesty positive controls, and the ledger meta-suite all pass. Structural header-to-shim enumeration replaces a brittle assertion-count proxy (S57-A2).
      — VERDICT: blocking — independent closeout review pending.

## Open audit findings against this front

1 open confirmed finding: `YEW-F-096`. Independent evidence review remains.

## Deferrals recorded against this front

none

## Verdict

NOT READY — `YEW-F-096` and independent evidence review remain pending.
