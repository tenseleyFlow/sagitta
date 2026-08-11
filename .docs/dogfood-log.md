# Daily-driver dogfood log

Sprint 42's daily-driver milestone is a field-evidence gate. Automated
tests and benchmark runs do not count as dogfood sessions, working days,
hours, real keystrokes, abnormal-exit trials, or workspace-resume cycles.
Only observations made while using yew as the primary editor belong in the
session table.

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
- [ ] At least 60% of Sprint 42's changed lines authored inside yew, linked
      by commit.
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
