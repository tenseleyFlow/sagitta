# Sprint 57.18: Shell Completion and Interactive Commands

## Prerequisites

- **Sprint 57.17** — the navigable, scrolling pager. This sprint renders its
  candidates in it and must not build a second list widget.
- Sprint 18 — `yew_cmd_parse` and `finish_bang` (`src/ui/cmdparse.c:821`),
  which makes everything after `!` ONE verbatim argument; `loose_name`
  (`cmdparse.c:1077`), which stops the name scan at `!`, so
  `yew_comp_query_at` returns false for any token inside `:!` — this is
  precisely why `:!` cannot complete today.
- Sprint 18.5 — `CompSource` by kind (`cmdcomp.h:143`), the existing PATH
  enumerator `enumerate_paths` (`cmdcomp.c:902`) and its sliced `DirListing`
  cache; `yew_comp_kind_for` (`cmdcomp.c:1328`), where argspec `'s'` maps to no
  completion kind; `yew_comp_quote` (`cmdcomp.c:312`).
- Sprint 19 — `yew_job_spawn` and the async `sh -c` path; **`yew_job_run_sync`
  with `YewJobSpec.inherit_tty` and `yew_tty_handover_begin`**, already used by
  the `:!yew` self-open handover; `yew_shell_dismiss_output` and the `:q`
  return.
- Binding: invariants 1, 4, 5, 6 (terminal always restored), 9.

## Goals

`:!` is a blind prompt. Nothing completes inside it, so every path is typed in
full, and any command that wants input gets a pipe rather than a terminal and
simply hangs or fails.

**This sprint amends a locked decision, deliberately and on the record.**
`ed.shell.term` exists only to refuse, and its comment states that an
interactive pty-backed buffer "is a different subsystem … and 1.0 does not
ship one". That stands: we are NOT building terminal emulation. We are routing
commands that need a terminal through the handover that already exists and
already works for `:!yew`. Update that comment to say so, so the refusal and
the new route cannot be read as contradicting each other.

Deferred, named: a terminal emulator or pty-backed buffer (still a non-goal);
completing shell builtins, aliases or functions; parsing pipelines and
redirections for per-stage completion; remembering per-directory history.

## Deliverables

### 1. Tokenising inside `:!` for completion only — `src/ui/cmdparse.c`

`yew_cmd_parse_point` must describe the token under the caret INSIDE a bang
command. Execution is untouched: `yew_cmd_parse` keeps handing the whole
string to `sh -c` verbatim, because that is what makes pipes, quotes and
redirection work.

- `loose_name` currently stops at `!`; teach the POINT parser (not the
  executing parser) to continue past it and split the remainder on unquoted
  whitespace, honouring single and double quotes and backslash escapes well
  enough to locate the caret's token and its stem.
- Report `token_index` relative to the bang body: index 0 is the command word,
  1+ are arguments.
- A caret inside quotes reports the stem without them, and the completion
  inserted must re-quote via `yew_comp_quote`.

**Pitfall the survey names:** `test_cmdparse_resolution_bang_errors_and_parse_point`
pins today's behaviour. Changing the executing parser breaks `:!` for every
existing user; changing only the point parser does not. Keep them separate and
say so in the header.

### 2. An executable completer — `src/ui/cmdcomp.c`

New `YEW_COMP_EXEC` source enumerating executables on `$PATH`.

- Walk each `$PATH` element once, `readdir`, keep entries that are regular or
  symlink and executable by the user. Reuse the sliced `DirListing` machinery
  so a slow or huge PATH element yields to the 1500 µs live budget and resumes
  on the idle tick, exactly as path completion does.
- Cache per PATH element keyed by directory and mtime; a changed PATH string
  invalidates wholesale. The survey's perf gate counts `opendir` calls, so
  scanning PATH on every keystroke will fail it.
- Deduplicate by basename, first PATH element winning, which is what the shell
  would actually run.
- `CACHEABLE | SLOW`, like the path source.

### 3. Routing completion inside `:!` — `src/ui/cmdcomp.c`, `src/edit/cmd.c`

`yew_comp_kind_for` currently maps argspec `'s'` to nothing. Add the bang case:
token 0 inside a bang body completes `YEW_COMP_EXEC`; tokens 1+ complete
`YEW_COMP_PATH`. This must key off the bang body, not off `ed.shell.run`'s
argspec generally, so `:r !cmd` and `:%!cmd` get the same treatment while
ordinary string arguments elsewhere stay uncompleted.

Candidates render in 57.17's pager, arrow into it the same way, and accept the
same way. No new widget, no second key path.

### 4. Interactive commands via the existing handover — `src/edit/shell.c`

A `:!` command that wants a terminal gets one.

- Add an explicit opt-in form rather than guessing: `:!!cmd` (or a documented
  alternative — pick one and justify it) runs through `yew_job_run_sync` with
  `inherit_tty`, wrapped in `yew_tty_handover_begin` / the matching restore.
  Guessing from the command name would be wrong the first time someone names a
  script `top`.
- The handover must restore the terminal on EVERY path including a signal
  death of the child, a failed exec, and a `SIGWINCH` during the run. Invariant
  6 is not negotiable, and `scripts/check-sigsafe.sh` gates the restore blob.
- Mode 1003, bracketed paste, the alternate screen and the kitty keyboard stack
  must all be off for the child and restored after. The existing handover
  already does this for `:!yew` — reuse it, do not reimplement.
- Output is NOT captured in this mode; the child owned the screen. On return,
  message what exited and with what status, then repaint fully.
- Ordinary `:!` is unchanged: async, piped, streamed into the job buffer,
  dismissed with `:q`.

### 5. Keyboard reach and docs

`:!!` is typed, so invariant 9 is satisfied by construction. Update
`ed.shell.term`'s help and comment to distinguish "no terminal emulator" from
"interactive commands are routed to a real terminal", and note the amendment
in Sprint 19's file so the original decision and its amendment sit together.

## Testing Strategy

- **Unit (`test_cmdparse.c`)**: the point parser locates token and stem inside
  `:!` across quoting, escapes and a caret at a boundary; the EXECUTING parser
  is byte-identical to today for every existing case, asserted explicitly.
- **Unit (`test_cmdcomp.c`)**: the exec source finds a fixture executable,
  skips non-executables and directories, deduplicates by first PATH element,
  respects the budget and resumes; `opendir` counts stay bounded across
  repeated keystrokes.
- **Unit (`test_job.c` / `test_shell_*.c`)**: the handover route restores the
  terminal after normal exit, non-zero exit, signal death and exec failure;
  ordinary `:!` is unchanged; every existing shell and job test stays green.
- **PTY**: `s57_18_bang_completes_exec`, `s57_18_bang_completes_path`,
  `s57_18_bang_quotes_a_spacey_path`. The handover cases need care in a PTY
  harness — if a faithful test is not achievable, say so explicitly and cover
  the restore path by unit test rather than pretending.
- **Perf**: `perf_cmdcomp` green including a PATH-scan case.
- **Script**: `scripts/check-sigsafe.sh`, `check-input.sh`, `bans.sh`.

## Definition of Done

1. Tab inside `:!` completes executables for the first word and paths after,
   rendered in 57.17's pager and selectable with the same keys.
2. Completions inside quotes round-trip through `yew_comp_quote`.
3. `:!` execution semantics are byte-identical to today — pipes, quotes and
   redirection unchanged — proven by the preserved parser tests.
4. An interactive command gets a real terminal and the editor's terminal state
   is fully restored afterwards on every exit path.
5. `ed.shell.term` still refuses, and its wording distinguishes the refusal
   from the new route.
6. gcc and clang warning-free; unit, ASan/UBSan, PTY, perf, script gates green;
   `MODULES=""` builds.
