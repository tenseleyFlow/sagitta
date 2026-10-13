# F02 terminal/renderer pre-closeout evidence

- Date: 2026-10-13
- Baseline: `6842c24f47763e7e2d12a03cb8cadf43061316b9`
- Last F02-owned executable-surface change: `9bb1415f`
- Host: Darwin 25.4.0 arm64
- Compilers: Apple clang 21.0.0; Homebrew GCC 16.2.0

Disposition: **NOT READY — local remediation evidence is green; independent
current-baseline review and four-target release evidence remain.**

This packet prepares the F02 closeout without signing it. Its author also
authored the F02 contract remediation and therefore cannot be the independent
Sprint 59 §2 reviewer. A future reviewer must establish the baseline again,
re-run or independently inspect the cited witnesses, and decide each checkbox
in `.docs/audits/closeout-F02-terminal.md`.

## Scope and provenance

The original F02 audit is `.docs/audits/audit-02-terminal.md`. It reviewed
detached product baseline `4c99e134`; its source/behavior review found no
confirmed product defect. The current closeout-contract recheck began from
the manifest at
`/private/tmp/yew-s59-f02-recheck-20261013.nxpvtr/manifest.md`, filed
`YEW-F-099`, and retained that finding through its complete lifecycle.
Decision amendment S59-A10 and commit `29682c67` now distinguish historical
staging guards from current terminal requirements without changing product
behavior.

At this baseline all 99 audit registry entries pass, both remediation ledgers
report zero active XFAILs, and the F02 closeout reports zero open confirmed
findings. Long sanitized fuzzing was deliberately omitted while the Sprint 58
tier-1 soak owns four cores. The adjacent `lsp-wolf-default` worktree was not
merged again; its work is already present through `origin/trunk`.

## Current-baseline executable evidence

### Strict builds

Apple Clang built `build/yew`, `build/unit_tests`, `build/audit_tests`,
`build/pty_runner`, and `build/demo_paint` under the repository's locked C11
warning flags. Homebrew GCC 16 independently built the same five surfaces in
`build-f02-gcc` with:

```text
make -j2 BUILD=build-f02-gcc CC=gcc-16 \
  build-f02-gcc/yew build-f02-gcc/unit_tests \
  build-f02-gcc/audit_tests build-f02-gcc/pty_runner \
  build-f02-gcc/demo_paint
```

Both compilers exited 0 with zero warnings under `-std=c11 -pedantic -Wall
-Wextra -Werror -Wvla`. The GCC audit runner reported 99 tests and zero
failures.

### Unit and sanitizer surfaces

An exact-name extraction of the nine F02-owned unit sources passed under both
Apple Clang and GCC 16:

| Source | Tests | Assertions |
|---|---:|---:|
| `test_tty.c` | 24 | 808 |
| `test_input.c` | 16 | 90,779 |
| `test_grid.c` | 11 | 124 |
| `test_grid_wide.c` | 9 | 51 |
| `test_render.c` | 16 | 203 |
| `test_color.c` | 5 | 73 |
| `test_vt.c` | 9 | 8,016 |
| `test_snapshot.c` | 7 | 91 |
| `test_pty.c` | 7 | 212 |
| **Total** | **104** | **100,357** |

The current GCC build also passed the broader `tty_`, `input_`, `grid_`,
`render_`, `color_`, `vt_`, `snapshot_`, and `pty_` filters. Those same broad
filters passed with Clang ASan/UBSan; the filters selected 201 tests and
125,145 assertions in total, with overlap and deliberately broader
non-F02 names left visible rather than misreported as unique coverage. Darwin
ASan does not support LeakSanitizer, so Linux Valgrind/LSan evidence remains a
release blocker.

### Full PTY and environment isolation

`make test-pty` passed its expected-failure selftest and all 557 registered
PTY cases under Apple Clang. The run covered the current editor as well as the
Sprint 3–6 core cases, including probe profiles, first paint, truecolor/256/16
tiers, damage, resize, OSC 52, fatal-signal restore, suspend/resume, modern and
legacy input, hostile paste, and terminal restoration.

`COLORTERM=truecolor make test-pty` then repeated the entire 557-case run with
no golden difference. `test_pty_environment_exact` also passed, establishing
that the child environment is constructed rather than inherited. The registry
contains 557 entries and the tree contains 552 golden files; `scripts/bans.sh`
passed its registry/golden cross-check, including intentional golden sharing
and non-golden protocol cases.

### Deterministic fuzz and renderer performance

The three terminal fuzzers passed 5,000 seeded iterations each:

```text
fuzz_input: seed=5902 iters=5000 corpus=17 hash=5d408a8bf0ee2328 ok
fuzz_grid: seed=5902 iters=5000 corpus=5 hash=102da7a89786b2d7 ok
fuzz_grid: frames=9750
fuzz_vt: seed=5902 iters=5000 corpus=5 hash=102da7a89786b2d7 ok
```

Two further executions of each command produced byte-identical stdout. This
exceeds Sprint 5's 1,000-frame mode-2026 balance witness and supplies the
contracted 5,000-iteration grid/VT reconstruction sample, but it is not being
promoted into Sprint 4's required 60-second sanitized campaign.

`make perf-render` passed its policy selftest and all hard byte limits:

| Case | Median ns | p99 ns | Bytes |
|---|---:|---:|---:|
| `full_200x50` | 24,000 | 25,000 | 10,351 |
| `full_80x24` | 5,000 | 5,000 | 2,089 |
| `single_cell_200x50` | 0 | 1,000 | 23 |
| `single_row_200x50` | 0 | 1,000 | 220 |
| `zero_damage_200x50` | 0 | 1,000 | 0 |

The 80×24 frame remains below its 2,200-byte limit, the single-cell frame is
below 32 bytes, and zero damage emits exactly zero bytes.

### Static policy and diagnostics

- `scripts/check-sigsafe.sh`, `scripts/check-render.sh`, and
  `scripts/bans.sh` all exited 0.
- `build/yew </dev/null` exited 1 with `stdin is not a terminal; use --batch
  SCRIPT for headless execution`.
- Direct searches found no `tcsetattr` outside `src/term/tty.c`, no `read` or
  `poll` call in `src/term/input.c`, and no `write`, `fwrite`, or `printf` call
  in `src/term/grid.c` or `src/term/render.c`.
- The PTY implementation and link graph contain no `forkpty`, `openpty`, or
  `libutil` use.

## Prior-finding reconciliation

| Finding | Remediation witness at this baseline |
|---|---|
| `YEW-F-099` | decision S59-A10, Sprints 3–6, F02, and the audit observation agree on shipped staging transitions; the audit reproducer passes and no terminal product code changed |

## Candidate evidence map for the closeout reviewer

These are witnesses to inspect, not pre-approved checkbox verdicts.

| Row | Primary witnesses |
|---|---|
| s03.1 | strict Clang/GCC product and test builds; focused Clang ASan/UBSan filters; full hosted/Valgrind matrix still required |
| s03.2 | 24 exact `test_tty.c` tests / 808 assertions; raw-mode table tests |
| s03.3 | `tty_restore_blob`, `tty_mouse_motion_restore_blob`; restore PTYs |
| s03.4 | `tty_probe_{modern,kitty_only,sync_only,dumb_deadline}` and deadline assertions |
| s03.5 | `tty_probe_chunking` |
| s03.6 | `tty_probe_pending_interleaved`, `tty_probe_ambiguous_prefix` |
| s03.7 | green `scripts/check-sigsafe.sh` and Makefile wiring |
| s03.8 | exact non-TTY exit-1 diagnostic and S59-A10 |
| s03.9 | `tty_probe_config` |
| s03.10 | empty direct `tcsetattr` ownership search |
| s03.11 | restore-guarantee/fatal-path documentation in `src/term/tty.h`; fatal restore PTYs |
| s04.1 | strict Clang/GCC builds and Clang ASan/UBSan filters; Valgrind remains outstanding |
| s04.2 | compile-time `Key` size assertion and `input.h` code/text documentation |
| s04.3 | 16 exact `test_input.c` tests / 90,779 assertions and fixture tables |
| s04.4 | `input_chunking_independence` |
| s04.5 | `input_escape_deadlines` and kitty-key coverage |
| s04.6 | `input_enable_blobs` |
| s04.7 | deterministic 5,000-iteration input sample; full 60-second sanitized run still required |
| s04.8 | paste-state assertions in `fuzz_input`; hostile-paste units and PTY |
| s04.9 | `input_unknown_csi` |
| s04.10 | `input_random_progress`, `input_seed_and_cap` |
| s04.11 | empty direct `read`/`poll` search in `input.c` |
| s04.12 | `input_mouse_and_focus`, `input_mouse_motion_without_button` |
| s05.1 | strict Clang/GCC builds and Clang ASan/UBSan filters; Valgrind remains outstanding |
| s05.2 | compile-time `Cell`/`YewColor` assertions and `grid.h` budget comments |
| s05.3 | `fuzz_grid` 5,000 iterations / 9,750 reconstructed frames; `fuzz_vt` |
| s05.4 | nine exact `test_grid_wide.c` tests |
| s05.5 | render zero-damage, cursor-state, gap-motion, and single-cell byte-limit witnesses |
| s05.6 | five exact color tests / 73 assertions, including vector and tie rules |
| s05.7 | three named render determinism tests and duplicate seeded fuzzer stdout |
| s05.8 | empty renderer/grid I/O search |
| s05.9 | green `check-render.sh`; 9,750 balanced frames |
| s05.10 | green `make perf-render` policy and hard byte limits |
| s05.11 | `render_resize_repaint_is_deterministic` and paint-resize PTY |
| s05.12 | closed-stream assertions in `fuzz_grid` and differential `fuzz_vt` |
| s06.1 | full Clang 557-case PTY run; GCC 16 PTY graph builds warning-free; hosted GCC execution still required |
| s06.2 | empty `forkpty`/`openpty`/`libutil` search; hand-rolled harness units |
| s06.3 | `paint_basic` in both complete PTY runs |
| s06.4 | 557 registry entries, 552 goldens, green `bans.sh` cross-check |
| s06.5 | VT unknown-sequence rejection unit and documented seeded-fault drill |
| s06.6 | PTY expected-failure selftest, update policy in `tests/pty/README`, and `bans.sh` CI-variable guard |
| s06.7 | `pty_timeout_reaps_child` and runner cleanup paths |
| s06.8 | `restore_crash`, `restore_bus`, `restore_abrt`, and `restore_segv` PTYs |
| s06.9 | `input_keys_modern` and `input_keys_legacy` in both full runs |
| s06.10 | snapshot format/diff units and runner double-snapshot protocol |
| s06.11 | cleanup/reap sanitizer units; Linux Valgrind fd tracking remains outstanding |
| s06.12 | `pty_environment_exact` plus the complete `COLORTERM=truecolor` PTY rerun |

## Remaining blockers

1. No independent reviewer has signed the **current** baseline. The original
   independent audit predates S59-A10 and this evidence collection.
2. No hosted CI run exists for `6842c24f`; local `trunk` is ahead of
   `origin/trunk`. Sprint 59 DoD 15 requires the complete four-target matrix at
   the commit each closeout signs.
3. Valgrind is not available on this Darwin host. Linux leak and descriptor
   evidence must come from CI or a designated Linux runner.
4. The exact 60-second ASan/UBSan input campaign and full GCC PTY execution
   were not rerun here. The short deterministic sample and warning-free GCC
   PTY build are evidence, not substitutes for those gates.
5. The Sprint 58 soak is separate evidence against product commit `4c99e134`;
   it does not substitute for Sprint 59's exact closeout matrix.

## Coverage limits

This was remediation reconciliation and evidence mapping, not a fresh broad
novel-defect hunt. It inspected F02's source contracts, replayed every audit
reproducer, exercised all current PTY goldens twice under distinct parent
environments, and ran focused units on two compilers plus Clang sanitizers. It
found no new confirmed issue after `YEW-F-099`, but passing local samples do
not prove the terminal/renderer front correct.
