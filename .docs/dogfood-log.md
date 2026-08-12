# Daily-driver dogfood log

Sprint 42's daily-driver milestone is a field-evidence gate. Automated
tests and benchmark runs do not count as dogfood sessions, working days,
hours, real keystrokes, abnormal-exit trials, or workspace-resume cycles.
Only observations made while using yew as the primary editor belong in the
session table.

Sprint 42's implementation landed before this evidence window opened.
Sprint 42.5 was designated as the first self-hosting evidence window, but no
qualifying yew session was recorded while its implementation was authored.
It therefore contributes zero working days, hours, keystrokes, or eligible
changed lines. Sprint 43 is not designated automatically by implementation
start; it contributes only if a qualifying yew session is explicitly opened
and logged before eligible edits. A later complete implementation sprint must
be explicitly designated before its work begins to satisfy the "one full
sprint" and ≥60% eligible-changed-lines criterion. For that qualifying sprint,
the denominator
is the sum of all eligible added and deleted human-authored lines under `src/`,
`runtime/syntax/*.fl`, `scripts/`, `tests/`, `.docs/`, `README.md`, and
`Makefile`; the numerator is the added-plus-deleted subset tied by commit to a
logged yew session. Both numerator and denominator exclude
`src/syn/langs_gen.c`,
`tests/**/*.spans`, fuzz corpora, generated or mechanically expanded perf
fixtures, build/cache artifacts, and vendored files. Record the base commit,
tip commit, path-filtered `git diff --numstat` totals, and attributed commits
with the final evidence. Automated edits, CI runs, benchmarks, and work
performed in another editor never enter the numerator. This changes where
evidence is collected, not the milestone's safety, duration, latency, or
revocation thresholds.

## Reference machine

Recorded 2026-08-11.

| Field | Value |
|---|---|
| CPU | Intel Core i7-10870H, 8 cores / 16 threads |
| RAM | 32 GiB installed (31 GiB usable) |
| Kernel | Linux 7.1.5-arch1-2, x86_64 |
| Terminal emulator | Not recorded yet |
| `TERM` | `xterm-256color` |

## Current totals

| Measure | Required | Recorded |
|---|---:|---:|
| Working days | 10 | 0 |
| Dogfood hours | 40 | 0 |
| Real keystrokes | 200,000 | 0 |
| Abnormal-exit restore trials | 3 | 0 |
| Quit/reopen resume cycles | 20 | 0 |

The milestone is **pending**. Add a row only after a real yew session and
link committed evidence where the gate requires it.

## Sessions

| Date | Hours | Files / work | Commits | Keystrokes | Latency summary | Resume cycles | Fallbacks and incidents | Evidence |
|---|---:|---|---|---:|---|---:|---|---|

## Gate checklist

- [ ] At least 10 working days and 40 logged hours, with every fallback
      recorded.
- [ ] At least 60% of one complete post-Sprint-42 implementation sprint's
      eligible changed lines, using the path and artifact rules above,
      authored inside yew and linked by commit. Sprint 42.5 recorded no
      qualifying session; designate a later full sprint before work begins.
- [ ] Zero data-loss incidents.
- [ ] Zero crashes or `BUG` log entries across recorded sessions.
- [ ] Three abnormal-exit terminal-restore trials: `SIGKILL`, `SIGSEGV`,
      and terminal closure.
- [ ] At least 200,000 real keystrokes with live keypress-to-paint p99 at
      or below 5 ms and p99.9 at or below 12 ms.
- [ ] Same-day field budgets recorded for cold start, 100 MB open, scroll
      throughput, RSS, and stripped full/minimal binary sizes.
- [ ] Every repository file type and both Fortran forms checked with
      `ed.syn.status`, with no degraded or unknown known type.
- [ ] At least 20 exact workspace quit/reopen cycles.
- [ ] Every logged editing task was keyboard-reachable; mouse-required
      tasks are linked as defects.

## Revocation categories

Any one category changes the milestone to `REVOKED` until a fix, a
regression test, and three consecutive clean dogfood days are recorded.

| ID | Category |
|---|---|
| B1 | Data loss: lost buffer, corrupted save, failed journal recovery, or an unrequested byte change including EOL, BOM, or final-newline changes |
| B2 | Terminal left in raw mode, the alternate screen, or with input modes pushed on any exit path |
| B3 | Event-loop stall above 200 ms, or session keypress-to-paint p99 sustained above 5 ms |
| B4 | Undo/redo fails to restore exact bytes, or a transaction partially applies |
| B5 | Multi-cursor edit differs from the equivalent single-cursor path |
| B6 | Crash: `SIGSEGV`, `SIGBUS`, `SIGABRT`, `yew_bug()`, or exit code 4 |
| B7 | Highlight state desynchronization that survives `ed.redraw` |
| B8 | Workspace resume loses tabs, groups, panes, cursors, or undo history |
| B9 | Rendering nondeterminism, including stale half-glyphs or orphan continuation cells |
| B10 | Silent stub: a path no-ops instead of hard-erroring with its sprint number |
