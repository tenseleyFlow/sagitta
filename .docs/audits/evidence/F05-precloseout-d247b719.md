# F05 execute/shell pre-closeout evidence

- Date: 2026-10-13
- Baseline: `d247b7195596d1aae8c3dbde49c780cf99faf7a7`
- Last F05-owned product-source change: `bebb3e7e`
- Last current/evolved F05-owned test-surface change: `9bb1415f`
- Host: Darwin 25.4.0 arm64
- Compilers: Apple clang 21.0.0; Homebrew GCC 16.2.0

Disposition: **NOT READY — local remediation evidence is green; independent
current-baseline review and required release/platform campaigns remain.**

This packet prepares F05 for closeout without signing it. Its author also
authored F05's contract reconciliation and therefore cannot be Sprint 59's
independent reviewer. A future reviewer must establish the baseline again,
independently inspect or rerun the cited witnesses, and decide every checkbox
in `.docs/audits/closeout-F05-exec.md`.

## Scope and provenance

The original F05 audit is `.docs/audits/audit-05-exec.md`. It reviewed
detached product baseline `41fef416`; hosted run `34283504177` at post-audit
control head `ef14ff7e` passed all 22 standard jobs. That audit established
the one-editor prompt model, descriptor/child accounting, structural argv
boundary, load-bearing filter interleave, real-shell quoting, failed-filter
rollback, foreground-filter typeahead, and exact child environment. Those are
useful historical results, not executions against this packet's baseline.

The current recheck began from
`/private/tmp/yew-s59-f05-recheck-20261013.Y5dmr7/manifest.md`. It filed and
closed two contract findings without changing product behavior:

- `YEW-F-104` / S59-A15 distinguishes later-delivered `:!`, `:s`, `:fl`,
  named-mark, and pattern-address routes from the intentional `:g` and
  terminal-emulator non-goals. The shipped `:!!` route remains terminal
  handover rather than a terminal emulator.
- `YEW-F-105` / S59-A16 expresses actual ownership boundaries instead of
  obsolete literal greps. Unicode measurement/conversion remains centralized;
  all production pipe creation routes through `yew_pipe_cloexec`, whose
  locked macOS implementation necessarily uses `pipe()` followed immediately
  by `FD_CLOEXEC` in the single-threaded, no-signal-handler-fork core.

At this baseline all 105 audit registry entries pass, both remediation ledgers
report zero active XFAILs, and F05 has zero open confirmed findings. The latest
scoped product change is `bebb3e7e`; the later scoped change `9bb1415f` affects
the evolved PTY test surface. Subsequent commits through this baseline are
documentation, audit-control, and contract reconciliation.

The adjacent `lsp-wolf-default` worktree was not merged again; its work is
already present through `origin/trunk`. Long general campaigns were omitted
while the Sprint 58 tier-1 soak owns four cores. That soak remains frozen
against product commit `4c99e134` plus runner-only child `4f483f31`; it is
separate evidence and cannot close this front.

## Current-baseline executable evidence

### Strict builds and owned units

Apple Clang and Homebrew GCC 16 each built `yew`, `unit_tests`,
`audit_tests`, `pty_runner`, `fuzz_cmdparse`, and `perf_jobstream` under the
repository's locked `-std=c11 -pedantic -Wall -Wextra -Werror -Wvla` policy
with zero warnings. A fresh Apple Clang ASan/UBSan tree built the same six
artifacts. Every registered test defined by the current/evolved F05-owned unit
sources was invoked by exact name under all three configurations:

| Sprint | Current/evolved owned sources | Tests | Assertions |
|---|---|---:|---:|
| s18 | `test_cmdline.c`, `test_cmdparse.c`, `test_cmdrange.c`, `test_cmdhist.c`, `test_cmdcomp.c` | 94 | 31,680 |
| s19 | `test_job.c`, `test_job_handover.c`, `test_job_stream.c`, `test_shell_quote.c`, `test_shell_self.c`, `test_shell_term.c` | 50 | 19,581 |
| **Total** | **11 source files** | **144** | **51,261** |

The source ownership has evolved since Sprints 18–19. The table names the
present files rather than hiding current coverage behind the obsolete
four-file counts in the original contracts. Each compiler and sanitizer run
produced the same counts. Darwin ASan supplies no LeakSanitizer, and Valgrind
is unavailable on this host; Linux leak and descriptor evidence remains a
release blocker.

The broader current `job_` filter also passed 42 tests / 6,622 assertions.
It includes the 500 complete spawn/reap/release cycles, 200 deterministic
spawn/kill timing cycles, process-group escalation, exact environment,
stream ownership, fd-delta, and child-reap controls. The stateless-history
control `ws_history_memory_history_writes_nothing` passed eight assertions
under Clang, GCC, and ASan/UBSan.

### Parser fuzz and shell quoting

The full contracted one-million-input command-parser campaign ran under all
three configurations. All three executions agreed byte-for-byte:

```text
fuzz_cmdparse: seed=1 iters=1000000 corpus=5 hash=573f773270d95ea2 ok
```

The owned-unit pass also ran `shell_quote_roundtrips_random_bytes` under all
three configurations. It sends 100,000 deterministic byte strings through
real `/bin/sh` in framed batches and compares every result byte-for-byte.
Together with the algorithm and hard-case controls, the shell-quote source
reported three tests / 1,262 assertions per configuration.

### Terminal and filter surfaces

The current Apple Clang product passed every registered `s18_` PTY case (22)
and every `s19_` PTY case (12). The harness runs each selected case twice and
requires its committed final-screen golden, so the completion-menu cycling,
error caret, grapheme motion, streamed output, no-output message, filter
rollback, foreground typeahead, and terminal non-goal surfaces were
byte-stable.

Six later-owner successor cases also passed:

```text
s57_11_shell_self_open
s57_12_job_output_quit_returns
s21_named_mark_round_trip
s21_global_is_a_non_goal
s21_replace_all
s21_search_preview
```

The three filter scripts passed all 32 assertions:

```text
44_filter_outcomes          13 assertions
45_filter_streaming         12 assertions
46_filter_timeout_and_scale  7 assertions
```

They cover nonzero and exec failure rollback, empty and partial output,
invalid UTF-8, timeout, a 1 MiB write-before-read deadlock fixture, EPIPE with
10 MiB still unwritten, 4 MiB output, and the 10,000-line single-undo case.

### Performance

The policy self-test and strict 50 MiB live-editor job-stream gate passed
while the independent soak remained active:

```text
perf-jobstream-policy: strict/advisory/sanity ok
jobstream keypress_to_paint_p99_streaming 188000 ns
  (limit 5000000 ns, 2000 keys under a 50 MiB stream) mode=GATING ok
```

This is a current local strict result, not a substitute for the designated
cross-platform performance matrix.

### Static ownership and policy

- `scripts/bans.sh` passed, including its planted-negative controls.
- `src/` contains no `system()` or `popen()` call.
- The only direct production `pipe()` call is the locked portable fallback
  in `src/util/base.c`. All pipe consumers call `yew_pipe_cloexec`; no caller
  duplicates the fallback sequence.
- Every production `.cmdline =` assignment remains in `src/edit/shell.c` and
  carries user- or capability-authorized shell text. The other current
  `YewJobSpec` sites construct structural argv.
- `src/ui/cmdline.c` has no caret array/index pattern matched by the Sprint
  18 second-editor guard. `cmdline_reuses_textbuf_and_grapheme_cursor` and
  `cmdline_e_keymap_leaves_are_registered_editor_commands` pass under all
  three configurations.
- `src/ui/cmdline.c` and `src/ui/cmdcomp.c` contain no local UTF-8 decoder,
  grapheme-boundary engine, `wcwidth` call, width table/range, or text-to-cell
  converter. They consume shared text/Unicode results.
- `make test-git-script` passed 15,372 assertions, including the exact Yew
  child-environment rows and hostile-parent preservation.
- `make test-audit` passed 105/105 controls. Findings, XFAIL debt, burndown,
  priority, deferral, and closeout governance all agree at zero open findings
  and zero active XFAILs.

## Prior-finding reconciliation

| Finding | Remediation witness at this baseline |
|---|---|
| `YEW-F-104` | S59-A15 and `84fd9ba3` align shipped execute/search/Fletch routes while retaining the tested `:g` and terminal-emulator non-goals |
| `YEW-F-105` | S59-A16 and `84fd9ba3` align Unicode/pipe ownership with centralized conversion and the safe locked-platform pipe helper |

## Candidate evidence map for the closeout reviewer

These are witnesses to inspect, not pre-approved checkbox verdicts.

| Row | Primary witnesses |
|---|---|
| s18.1 | strict Clang/GCC builds, fresh ASan/UBSan tree, exact owned-unit runs; full exact-baseline suite, Valgrind, and determinism lanes remain outstanding |
| s18.2 | empty local decoder/width/converter search in `cmdline.c`/`cmdcomp.c`, shared text/Unicode calls, S59-A16 |
| s18.3 | empty caret mini-editor grep; `cmdline_reuses_textbuf_and_grapheme_cursor`; E-layer registered-command walk |
| s18.4 | `cmdparse_tokenizer_expansion_matrix` (205 assertions) and current parser-owned controls |
| s18.5 | `cmdcomp_path_quoting_retokenizes_one_argv`, shell-quoted completion rows, bang-quoting PTYs |
| s18.6 | three current `cmdrange_` tests / 127 assertions; named-mark, search, and substitution successor PTYs; S59-A15 |
| s18.7 | seven `cmdhist_` tests / 113 assertions, memory-history no-write control under all configurations, `ed->clean` in-memory route |
| s18.8 | 22 current `s18_` PTYs, including every completion-cycle row and Enter-with-menu behavior |
| s18.9 | `cmdline_catalogue_errors_preserve_live_prompt`, parse-point controls, `s18_cmdline_error_caret` |
| s18.10 | identical one-million-input fuzz hash under Clang, GCC, and ASan/UBSan |
| s18.11 | current parser routes, six successor PTYs, tested `:g` non-goal, S59-A15 |
| s18.12 | 94 current/evolved owned tests / 31,680 assertions per configuration |
| s19.1 | strict Clang/GCC builds, fresh ASan/UBSan tree, exact owned-unit runs; full exact-baseline suite and Valgrind remain outstanding |
| s19.2 | empty `system`/`popen` scan; sole `pipe()` fallback in `base.c`; all consumers route through `yew_pipe_cloexec`; S59-A16 |
| s19.3 | `job_exec_failure_is_not_exit_127`, `job_exit_127_is_not_exec_failure` under all configurations |
| s19.4 | 24-fixture split-at-every-chunk control plus six hold/EOF/cap controls; seven tests / 1,856 assertions |
| s19.5 | scripts 44–46 (32 assertions), failed-filter unit rollback/journal proof, single-undo scale case |
| s19.6 | `45_filter_streaming` 1 MiB write-before-read fixture and its documented load-bearing interleave |
| s19.7 | timeout, process-group, signal, and cancellation controls plus byte-identical script/PTY failure outcomes |
| s19.8 | 500 complete lifecycle cycles and 200 spawn/kill timing cycles under Clang, GCC, and ASan/UBSan; Linux Valgrind remains outstanding |
| s19.9 | exact job environment units and current 15,372-assertion Git job script |
| s19.10 | 100,000 real-`/bin/sh` byte-string round trips under all three configurations |
| s19.11 | current `s19_no_output_message` byte-stable PTY |
| s19.12 | terminal refusal/handover owned units, `s19_term_is_not_a_feature`, search/substitution successor PTYs, S59-A15 |
| s19.13 | strict current 50 MiB / 2,000-key result: 0.188 ms p99 against 5 ms limit |
| s19.14 | 50 current/evolved owned tests / 19,581 assertions per configuration |

## Remaining blockers

1. No independent reviewer has signed the **current** baseline. The original
   audit predates S59-A15/A16 and the later product/test evolution.
2. No hosted exact-baseline full matrix exists for `d247b719`; local `trunk`
   is ahead of `origin/trunk`. Sprint 59 requires the complete cross-target
   matrix at the commit a closeout signs.
3. Valgrind and LeakSanitizer evidence is unavailable on this Darwin host.
   Linux leak and descriptor checks remain outstanding.
4. The complete exact-baseline `make test`, determinism, full torture, and
   designated performance campaigns were not rerun here. Focused current
   checks and the historical all-green F05 matrix are not substitutes.
5. The Sprint 58 soak is frozen against product commit `4c99e134`. At packet
   creation it has 4/56 durable streams complete and four `fuzz_grapheme`
   streams running. It neither blocks local Sprint 59 work nor substitutes
   for Sprint 59's exact closeout matrix.

## Coverage limits

This was remediation reconciliation and evidence mapping, not a fresh broad
novel-defect hunt. It inspected the changed F05 contracts and current execute
surface, replayed every audit reproducer, and exercised focused builds, units,
fuzz, terminal, filter, Git-environment, performance, and governance controls.
It does not claim that passing samples prove the entire execute/shell front
correct.
