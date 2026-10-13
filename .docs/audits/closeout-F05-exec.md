# Closeout: F05 — Execute and shell

Date:            2026-10-12
Baseline commit: 4c99e13474c84d75eec3250d61412b53fa41ae00
Reviewer:        UNASSIGNED
Sprints covered: s18, s19

## DoD items (every numbered item from every sprint above)

- [ ] s18.1 `make test` green on gcc and clang, zero warnings; ASan/UBSan/ determinism lanes green.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s18.2 Unicode measurement and coordinate conversion remain centralized in the shared text/Unicode engines. `src/ui/cmdline.c` and `cmdcomp.c` may name grapheme commands, hold `tabwidth`, and lay out already-measured cells, but contain no local decoder, boundary algorithm, width table/range, or text-to-cell conversion (S59-A16).
      — VERDICT: blocking — independent closeout review pending.
- [ ] s18.3 No second editor: `grep -n '\[[a-z_]*caret\|caret++\|caret--' src/ui/cmdline.c` → empty, and a unit test walks the `YEW_MODE_E` keymap asserting every leaf is a registered `ed.*` name.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s18.4 Expansion table: all 9 rows plus the error row unit-tested; the four worked-table lines produce exactly the listed argv.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s18.5 Quoting: a path with a space, `"`, `'`, `$`, and `\n` round-trips completion → prompt → argv as **one** element in all three quoting forms; ≥ 12 assertions.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s18.6 Range: all 6 `range_policy` rows enforced; the `:1,2d` span includes the trailing EOL (explicit case); Sprint 21's `'a` and `/re/` successors resolve through the named-mark and shared search routes (S59-A15).
      — VERDICT: blocking — independent closeout review pending.
- [ ] s18.7 History: cap, dedupe-to-end, frozen-stem walk, and concurrent-append all green; no history I/O under `--batch`/`--clean` (grep + unit).
      — VERDICT: blocking — independent closeout review pending.
- [ ] s18.8 Completion menu: a byte-identical pty golden per cycling row across two runs; Enter with an open menu never executes.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s18.9 Errors: all 13 catalogue messages verbatim; the prompt survives each with text intact and the cursor at `tok_lo`.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s18.10 Fuzz: 1 000 000 parse inputs, zero crashes, `tok` invariant holds on 100% of failures.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s18.11 Later owning sprints delivered the execute surfaces `:!`, `:s`, `:fl`, `'a`, and `/re/` through the original parser/registry routes. `:g` alone remains a tested permanent non-goal with its Fletch-query guidance (S59-A15).
      — VERDICT: blocking — independent closeout review pending.
- [ ] s18.12 ≥ 120 assertions across the four unit files.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s19.1 `make test` green on both compilers, zero warnings; ASan/UBSan/valgrind lanes green.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s19.2 `system()` and `popen()` remain absent. All production pipe creation routes through `yew_pipe_cloexec`: Linux/FreeBSD use `pipe2(O_CLOEXEC)`, the portable macOS fallback uses `pipe()` plus immediate `FD_CLOEXEC`, and no other direct pipe caller exists (S59-A16).
      — VERDICT: blocking — independent closeout review pending.
- [ ] s19.3 Exec failure is distinguishable from exit 127: `YEW_JOB_EXECFAIL` + `exec_errno == ENOENT` for a missing binary vs `YEW_JOB_EXITED` + `exit_code == 127` for `sh -c 'exit 127'`.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s19.4 Grapheme-safety: the split-at-every-offset test passes for all ≥ 20 fixtures; a debug-build assertion proves no partial UTF-8 sequence ever reaches a job buffer.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s19.5 All six §5 outcome rows verified, including byte-identity on every failure row and **undo depth exactly 1** after a successful filter and after a 10 000-line filter.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s19.6 The 1 MiB-output-before-reading-stdin fixture completes in < 2 s; removing the `POLLOUT`/`POLLIN` interleave hangs it (documented in the test's comment).
      — VERDICT: blocking — independent closeout review pending.
- [ ] s19.7 Timeout and cancel leave the buffer byte-identical and reap the whole group: `kill(-pgid, 0)` → `ESRCH` afterwards.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s19.8 Zero zombies and zero leaked fds after the 200-cycle torture lane and the 500-spawn fd-accounting test.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s19.9 The §7 environment table is exported exactly — a job running `env` shows all pinned rows and no `COLUMNS`/`LINES`; `YEW_COL` matches the statusline column on a line containing a ZWJ emoji family.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s19.10 `yew_shell_quote` fuzz: 100 000 random byte strings round-trip through `/bin/sh` byte-exact, embedded newlines and quotes included.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s19.11 `:!true` opens no buffer and messages exactly `:!true — no output (exit 0)` (pty golden).
      — VERDICT: blocking — independent closeout review pending.
- [ ] s19.12 `ed.shell.term` hard-errors with the terminal-emulator permanent-non-goal message while `:!!` uses the tested terminal-handover path; Sprint 21's live `:s` and `/re/` routes pass their successor coverage (S59-A15).
      — VERDICT: blocking — independent closeout review pending.
- [ ] s19.13 Perf gate: 50 MiB of streamed output keeps keypress p99 ≤ 5 ms; baseline committed under `tests/perf/`.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s19.14 ≥ 110 assertions across the four new unit files.
      — VERDICT: blocking — independent closeout review pending.

## Open audit findings against this front

2 Medium: `YEW-F-104`, `YEW-F-105`.

## Deferrals recorded against this front

none

## Verdict

NOT READY — Two contract findings and independent evidence review remain.
