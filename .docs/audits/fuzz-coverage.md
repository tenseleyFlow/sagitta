# Fuzz coverage ledger

Baseline: `b3f32645e0456dca1a90f73e4e4f2c2fc64003b3`

The first full `make fuzz-cov` run completed on arm64 macOS with Apple clang
21.0.0. Plain and instrumented output was identical for all 45 targets. The
deterministic generated snapshot had SHA-256
`333a9c93e00a4978bd07f5c0e1926725e16d6ce108ed7ebdd033ce65f9841d4e` on two
consecutive runs. This is an audit baseline, not a claim that the corpus has
plateaued.

Coverage commit: `1990241d32d720a8d4f97a5a5d4a983e512e34e6`.

| Date | Commit | Lane | Target | Iterations | Seed | New edges | Total edges | Corpus size | Findings |
|---|---|---|---|---:|---|---:|---:|---:|---|
| 2026-09-12 | `1990241d` | `baseline` | `fuzz_utf8` | 0 | 1 | 40 | 40 | 1235 | — |
| 2026-09-12 | `1990241d` | `baseline` | `fuzz_grapheme` | 0 | 1 | 191 | 191 | 1235 | — |
| 2026-09-12 | `1990241d` | `baseline` | `fuzz_input` | 0 | 1 | 175 | 175 | 17 | — |
| 2026-09-12 | `1990241d` | `baseline` | `fuzz_grid` | 0 | 1 | 373 | 373 | 5 | — |
| 2026-09-12 | `1990241d` | `baseline` | `fuzz_vt` | 0 | 1 | 528 | 528 | 5 | — |
| 2026-09-12 | `1990241d` | `baseline` | `fuzz_multicursor` | 0 | 1 | 1212 | 1212 | 5 | — |
| 2026-09-12 | `1990241d` | `baseline` | `fuzz_fl_lex` | 0 | 1 | 270 | 270 | 120 | — |
| 2026-09-12 | `1990241d` | `baseline` | `fuzz_fl_parse` | 0 | 1 | 597 | 597 | 120 | — |
| 2026-09-12 | `1990241d` | `baseline` | `fuzz_fl_vm` | 0 | 1 | 671 | 671 | 16 | — |
| 2026-09-12 | `1990241d` | `baseline` | `fuzz_flapi` | 0 | 1 | 1660 | 1660 | 10 | — |
| 2026-09-12 | `1990241d` | `baseline` | `fuzz_pkg_tree` | 0 | 1 | 102 | 102 | 5 | — |
| 2026-09-12 | `1990241d` | `baseline` | `fuzz_record` | 0 | 1 | 1570 | 1570 | 7 | — |
| 2026-09-12 | `1990241d` | `baseline` | `fuzz_syn` | 0 | 1 | 1005 | 1005 | 61 | — |
| 2026-09-12 | `1990241d` | `baseline` | `fuzz_syn_def` | 0 | 1 | 1741 | 1741 | 65 | — |
| 2026-09-12 | `1990241d` | `baseline` | `fuzz_fl_std` | 0 | 1 | 711 | 711 | 8 | — |
| 2026-09-12 | `1990241d` | `baseline` | `fuzz_tabs` | 0 | 1 | 1578 | 1578 | 5 | — |
| 2026-09-12 | `1990241d` | `baseline` | `fuzz_shadow` | 0 | 1 | 1823 | 1823 | 5 | — |
| 2026-09-12 | `1990241d` | `baseline` | `fuzz_groups` | 0 | 1 | 1956 | 1956 | 5 | — |
| 2026-09-12 | `1990241d` | `baseline` | `fuzz_panes` | 0 | 1 | 1491 | 1491 | 5 | — |
| 2026-09-12 | `1990241d` | `baseline` | `fuzz_search` | 0 | 1 | 2472 | 2472 | 5 | — |
| 2026-09-12 | `1990241d` | `baseline` | `fuzz_re_quote` | 0 | 1 | 179 | 179 | 5 | — |
| 2026-09-12 | `1990241d` | `baseline` | `fuzz_re_compile` | 0 | 1 | 269 | 269 | 5 | — |
| 2026-09-12 | `1990241d` | `baseline` | `fuzz_re_diff` | 0 | 1 | 831 | 831 | 5 | — |
| 2026-09-12 | `1990241d` | `baseline` | `fuzz_cmdparse` | 0 | 1 | 110 | 110 | 5 | — |
| 2026-09-12 | `1990241d` | `baseline` | `fuzz_fuzzy` | 0 | 1 | 34 | 34 | 5 | — |
| 2026-09-12 | `1990241d` | `baseline` | `fuzz_state` | 0 | 1 | 1987 | 1987 | 5 | — |
| 2026-09-12 | `1990241d` | `baseline` | `fuzz_gitignore` | 0 | 1 | 168 | 168 | 5 | — |
| 2026-09-12 | `1990241d` | `baseline` | `fuzz_porcelain` | 0 | 1 | 45 | 45 | 5 | — |
| 2026-09-12 | `1990241d` | `baseline` | `fuzz_fuss` | 0 | 1 | 2121 | 2121 | 5 | — |
| 2026-09-12 | `1990241d` | `baseline` | `fuzz_git_diff` | 0 | 1 | 147 | 147 | 5 | — |
| 2026-09-12 | `1990241d` | `baseline` | `fuzz_mouse` | 0 | 1 | 2224 | 2224 | 5 | — |
| 2026-09-12 | `1990241d` | `baseline` | `fuzz_symidx` | 0 | 1 | 2131 | 2131 | 5 | — |
| 2026-09-12 | `1990241d` | `baseline` | `fuzz_json` | 0 | 1 | 97 | 97 | 5 | — |
| 2026-09-12 | `1990241d` | `baseline` | `fuzz_jsonrpc` | 0 | 1 | 152 | 152 | 5 | — |
| 2026-09-12 | `1990241d` | `baseline` | `fuzz_lsp_msg` | 0 | 1 | 2537 | 2537 | 5 | — |
| 2026-09-12 | `1990241d` | `baseline` | `fuzz_lsp_resp` | 0 | 1 | 745 | 745 | 5 | — |
| 2026-09-12 | `1990241d` | `baseline` | `fuzz_http` | 0 | 1 | 131 | 131 | 15 | — |
| 2026-09-12 | `1990241d` | `baseline` | `fuzz_ai_stream` | 0 | 1 | 85 | 85 | 5 | — |
| 2026-09-12 | `1990241d` | `baseline` | `fuzz_ai_shadow` | 0 | 1 | 1833 | 1833 | 5 | — |
| 2026-09-12 | `1990241d` | `baseline` | `fuzz_ai_redact` | 0 | 1 | 159 | 159 | 5 | — |
| 2026-09-12 | `1990241d` | `baseline` | `fuzz_theme` | 0 | 1 | 316 | 316 | 290 | — |
| 2026-09-12 | `1990241d` | `baseline` | `fuzz_undo_serial` | 0 | 1 | 251 | 251 | 7 | `YEW-F-076` |
| 2026-09-12 | `1990241d` | `baseline` | `fuzz_undo` | 10000 | 1 | 594 | 594 | 0 | — |
| 2026-09-12 | `1990241d` | `baseline` | `fuzz_textbuf` | 6 | 1 | 570 | 570 | 0 | — |
| 2026-09-12 | `1990241d` | `baseline` | `fuzz_units` | 100000 | 1 | 558 | 558 | 0 | — |

## Pinned schedule

| Lane | Cadence | Budget | State |
|---|---|---|---|
| `fuzz` | every push | 200,000 iterations per target, seed 1 | existing |
| `fuzz-nightly` | nightly | 30 minutes per target, recorded date seed | scheduled across all 45 targets; admissions and ledger attached per target |
| `soak` | continuous | 72 aggregate hours per tier-1 target, four 18-hour seed streams | first local exact-profile cycle running at `ba72c3a0`; hosted runner automation not registered |
| `fuzz-cov-weekly` | weekly | four hours per target, monotonic edge counts | baseline and scheduler wired; fail-closed on Apple clang 21.0.0; no matching runner registered |
| `soak-rc` | each release candidate | 72 aggregate hours tier 1 + 12 aggregate hours tier 2 | exact-commit four-stream release blocker wired; designated runner not registered |

The campaign workflow keeps every worker below 24 hours: tier 1 is four
parallel 18-hour streams and tier 2 is four parallel 3-hour streams. This
preserves the pinned aggregate target budgets while keeping end-of-run corpus
artifacts inside the Actions token lifetime. The coverage baseline is compiler
sensitive, so the weekly and continuous jobs require the matching macOS arm64
Apple clang 21.0.0 runner instead of comparing unrelated hosted-compiler guard
maps.

The first local tier-1 cycle started on 2026-09-13 at `ba72c3a0` in an
isolated worktree on arm64 macOS with Apple clang 21.0.0. It runs in the
sanitizer week (`SAN=1`), with targets serial and each target's four streams
concurrent at low scheduling priority. `fuzz_utf8` started with seeds
`2218013357` through `2218013360`. Completed rows and minimized admissions
are committed only after each target finishes; this paragraph is launch
evidence, not a fabricated completion row.

An Actions API capacity check on 2026-09-13 found no repository variables and
no registered self-hosted runners. The workflow therefore leaves scheduled
coverage/soak jobs skipped and makes manual requests fail explicitly until
`YEW_FUZZ_COV_ENABLED=1` and the `yew-fuzz-cov` runner exist. The RC lane uses
the existing `YEW_PERF_X86_64_ENABLED` / `yew-perf-x86_64` identity and likewise
cannot be mistaken for a completed release gate while that runner is absent.
