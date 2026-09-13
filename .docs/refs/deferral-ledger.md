# Deferral ledger

Last reconciled: 2026-08-27, while activating Sprint 56.5 after Sprint 56's
repository closeout and the Campaign 12 historical sweep.

This ledger covers user-visible product promises that are not live yet. It
does not treat lazy tab hydration, deferred syntax work queues, or other
internal scheduling as feature deferrals.

## Ledger rules

- A scheduled 1.0 surface names the sprint that owns it.
- A post-1.0 surface says so in present-tense product diagnostics and has no
  fake implementation sprint.
- A permanent non-goal says why it is rejected.
- Once a surface ships, historical sprint wording leaves runtime help and
  diagnostics. Compatibility names call the landed implementation.
- A missing implementation never silently succeeds.

## Reconciled historical promises

| Original sprint | Surface | Current disposition |
|---|---|---|
| 22 | `ed.win.next`, `ed.win.prev` | Live aliases for pane-order focus. |
| 23 | `ed.file.open`, `ed.file.close` | Live aliases for buffer open/close. |
| 23 | `ed.buf.next`, `ed.buf.prev` | Live aliases for tab navigation. |
| 23 | Multiple positional startup files | Live; later files start as lazy tabs. |
| 24 | `ed.group.next`, `ed.group.prev` | Live aliases for continuous file/group navigation. |
| 25 | Directory startup and `--workspace DIR` | Live with canonical workspace roots and startup validation. |
| 25 | `ed.ws.migrate` | Reserved live command; schema v1 is current and no v2 exists. |
| 36 | `:source` | Live compatibility spelling for `:config.reload`; legacy file operands are rejected. |
| 38 | `ed.find.command` | Live command/help picker. |
| 38 | `--replay REG` | Live after-script batch macro replay. |
| 47 | `ed.find.symbol` | Live compatibility alias for the current-document `ed.lsp.symbols` picker. |

The command registry has no built-in `YEW_CMD_DEFERRED` rows after this
reconciliation. The flag and its hard-error machinery remain available for
a future sprint-owned surface, but runtime help must not use it for aliases,
reserved commands, post-1.0 scope, or permanent non-goals.

Sprint 55.5 added no product deferral: both shipped example plugins execute
against the public Sprint 54/55 API in every build, and their guide describes
only live surfaces or explicit post-1.0 scope already listed below.

Sprint 56.5 owns the current workspace/FUSS/shadow interaction corrections.
It adds no new deferred 1.0 surface and keeps the existing workspace state file
canonical; simultaneous-process state merge remains post-1.0.

## Scheduled 1.0 work

| Surface | Owner | Current behavior |
|---|---|---|
| Cooperative incremental preview for nonliteral regex searches | Sprint 59, after Sprint 58 F06 audit | Sprint 56 bounds literal prompt previews and counted literal repeats; nonliteral prompt edits still run the exact Sprint 20 engine synchronously. The remediation must preserve anchors, word boundaries, captures and cross-slice matches through resumable Pike/DFA state rather than search-window approximations. |
| `--batch-strict` | Sprint 59 | Argument parsing rejects it and names Sprint 59. |

## Explicit post-1.0 scope

| Surface | Current 1.0 behavior |
|---|---|
| Workspace-wide LSP `workspace/symbol` | Current-document symbols and the local index ship; workspace-wide server querying does not. |
| Simultaneous-process workspace-state merge | Each process saves through the existing atomic XDG state path; cross-process semantic merge is post-1.0. |
| LSP snippet tab stops/placeholders | Snippets are safely downgraded to insertion-ready plain text. |
| Interactive terminal emulator | Shell jobs, filters, and captured output ship; `ed.shell.term` explains the boundary. Sprint 57.18 amends what follows from that (below): commands that need a terminal are LENT yew's own through `:!!`, which is not emulation. |
| AI prompt/conversation UI | Ghost-text completions ship; prompt UI reports that it is outside 1.0. |
| Document context menu and tab-to-pane drag | Keyboard/pane/tab operations ship; these mouse extensions remain post-1.0. |
| Interleaved syntax embeds | Properly nested embedded languages ship; `embed.interleave` is rejected. |
| Unicode 17 data update | Unicode 16.0.0 remains pinned for deterministic 1.0 behavior. |
| Per-hunk unstage and conflict-resolution UI | Whole-file unstage and editor/F-mode diff workflows ship. |

## Amendment S57.18-A1 (2026-09-12) — lending a terminal is not emulating one

Sprint 19 registered `ed.shell.term` only to refuse, and its comment said an
interactive pty-backed buffer "is a different subsystem ... and 1.0 does not
ship one". That stands, unchanged and permanently: yew does not interpret
CR/backspace/ANSI on a child's behalf, does not propagate resizes into a
pseudo-terminal it owns, and ships no terminal buffer.

What Sprint 57.18 adds is the other half of the sentence, which Sprint 19
never actually said: refusing to EMULATE a terminal was never a reason to
refuse to LEND one. `:!!cmd` runs a single command with yew's OWN terminal,
through `yew_job_run_sync` with `inherit_tty` inside
`yew_tty_handover_begin` / `yew_tty_handover_end` — the handover Sprint 19
built and Sprint 52's interactive rebase has used since. No new terminal
state, no second restore path, no emulation.

- Opt-in by SPELLING, never by guessing a command's name. A script called
  `top` in someone's `~/bin` would be guessed wrong the first time it ran,
  and the failure mode is an editor that appears to have locked up.
- Output is not captured; the child owned the screen. On return yew reports
  what exited and with what status, then repaints fully.
- `:!` is unchanged: asynchronous, piped, streamed into a job buffer,
  dismissed with `:q`. `yew_cmd_parse` is byte-identical — `:!!top` still
  parses to `ed.shell.run` with the single verbatim argument `!top`, and
  `ed.shell.run` is what reads the second bang.
- `ed.shell.term` still refuses. Its wording now distinguishes the refusal
  (no emulator) from the route (`:!!`), so the two cannot be read as
  contradicting each other.
- `--batch` has no terminal to hand over, so `ed.shell.term_run` is
  `YEW_CMD_INTERACTIVE` and the batch refusal table names `ed.shell.run` as
  the alternative. It is refused by name rather than degraded into `:!`.

## Permanent non-goals

| Surface | Supported alternative |
|---|---|
| A bespoke `:g` mini-language | Fletch queries and `buf.find`. |
| Regex lookaround, backreferences, atomic groups | The deterministic no-backtracking regex subset. |
| Product analytics/telemetry | No analytics are collected. |

## Reconciliation check

Before closing a sprint, search production strings for historical promises:

```sh
rg -n "lands in Sprint|not implemented yet|YEW_CMD_DEFERRED" src docs
```

Every result must be either this ledger's scheduled row, generic enforcement
machinery, or an intentional test of that machinery. Newly deferred work must
be added here in the same commit as its hard-error surface and sprint owner.
