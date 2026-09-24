# Sprint 57.27: The Persistent Shell Session

## Prerequisites

- **Sprint 19** — `:!` today: `yew_shell_run` (`src/edit/shell.c`) spawns
  `$SHELL -c CMD` per command through `yew_job_spawn` with
  `YEW_SINK_BUFFER`, into a `*job:N CMD*` scratch buffer, listed in the
  `*jobs*` table, dismissable with Esc / `:q`, signalled with
  `yew_job_signal`. `:r !cmd` (`yew_shell_read`), `:%!filter`
  (`yew_shell_filter`, stdin = the region), and `:!!cmd`
  (`yew_shell_term_run`, 57.18 §4, the real terminal) are separate modes.
  `yew_job_shell()` resolves `$SHELL` (fallback `/bin/sh`).
- **The framed job seam** — `YEW_SINK_FRAMED` + `YewJobFramedOps`
  (`src/edit/job.h:35`): a long-lived child whose stdin is a transmit
  queue (`tx_view` / `tx_consume`) and whose stdout is fed to an owner
  parser (`feed_stdout`, `finish_stdout`), with `tick` / `deadline` /
  `destroy`. The LSP client (`src/mod/lsp/client.c:1041`) and git's batch
  reader (`src/mod/git/git.c:1210`) are the working examples.
- **Sprint 57.32** — the effective directory at the caret (`YewShCtx.cwd`),
  computed relative to "the directory `:!` commands start in"; generators,
  fish and help jobs spawned with an explicit cwd; `yew_job_env()`.
- **Sprint 57.31** — `A-e`, `A-h` and 57.31 §3's rule that a terminal-owning
  child keeps the user's pagers.
- Binding: invariants 1, 4, 5, 6, 9; `00-decisions.md`'s "single-threaded
  core, concurrency = event loop + subprocesses".

## Goals

Every `:!` starts a fresh `$SHELL -c`, so `cd`, `export` and anything
defined in one command are gone by the next. This sprint gives `:!` a
**persistent shell session**: one long-lived shell per editor that every
plain `:!cmd` runs inside. Decisions made with the user (2026-09-24), not
to be relitigated:

1. **On by default.** Option `shell.session`, global enum, `persistent`
   (default) or `fresh` (today's behaviour, exactly).
2. **The other forms inherit the session's directory AND exported
   environment**: `:!!` (real terminal), `:r !cmd`, `:%!filter`, and
   completion's generator / help / fish jobs. They cannot run inside the
   session (they need a terminal or a piped stdin); they start from its
   state instead.
3. **A `:!` while the session is busy runs alongside** in a one-off shell
   started from the session's current directory and exports, with a note
   that it ran outside the session (so its own `cd` will not persist).
4. **The shell is `$SHELL`, non-interactive, no rc files** — matching
   today's `$SHELL -c` exactly (which also reads only what a non-interactive
   shell reads, e.g. zsh's `.zshenv`). The user's rc files hold one alias
   and interactive-only hooks; sourcing them would add risk for nothing.

Deferred: sourcing rc files (a later option if ever wanted); a fish-syntax
session (fish as `$SHELL` falls back to `fresh` with one message, §1);
job control inside the session (`bg`/`fg`/`jobs`).

## Deliverables

### 1. The session — `src/edit/shsession.h`, `src/edit/shsession.c`

One per `Ed`, created lazily by the first plain `:!` under
`shell.session = persistent`, spawned through `yew_job_spawn` with
`YEW_SINK_FRAMED`, `internal = true`, argv form `[$SHELL, "-s"]` (zsh,
bash, dash, ksh and sh all read commands from stdin with `-s`), cwd = the
workspace root (today's `:!` directory), and the standard job
environment. Its stderr is merged into stdout by the prologue, so the
command's output keeps its natural interleaving.

**Unsupported shells.** If `$SHELL`'s basename is `fish`, `nu`, `xonsh`,
`elvish` or `pwsh` (not sh-family), do not start a session: behave as
`fresh`, and say so once per editor (`shell session needs a POSIX-family
$SHELL; running each :! fresh`).

**The prologue** (written once, first thing on stdin):

- `exec 2>&1` — merge stderr.
- `trap : INT` — an interrupt kills the running command, not the session
  (a trapped signal is reset to default in children, an ignored one is
  not: use `:` , never `''`).
- History off, no prompts, `set +m` (no job control on pipes).
- Nothing that reads a file of the user's.

**The command frame.** Each command is sent as ONE line that the shell
cannot misparse whatever the command contains:

```sh
eval 'CMD-SINGLE-QUOTED' </dev/null; __yew_s=$?; printf '\036%s %d\036' "$NONCE" "$__yew_s"; "$__yew_self" --yew-env0
```

(`$__yew_self` is set once by the prologue to `yew_job_self_exe()`, shell-
quoted. Everything goes to the merged stdout; the parser treats the bytes
after a nonce-bearing marker as the env record set, not as output.)

**Pitfall — interrupt semantics differ per shell.** With `trap : INT`, a
SIGINT that kills the `eval`'d command must still let the rest of the
frame line run (so the marker is printed with status 130). Verify that on
zsh, bash and dash in a unit test rather than assuming it; if one shell
abandons the line, put the marker in an `EXIT`-safe position or re-send
it, and say which.

- The command is single-quoted (`'` → `'\''`) and `eval`'d, so an
  unbalanced quote or a syntax error in it is `eval`'s error with status
  2 — the frame still completes and the session keeps reading. Without
  this, `:!echo "unterminated` makes the shell swallow every later frame
  and the session hangs.
- `</dev/null` gives the command the stdin it has today.
- The **end marker** carries a per-session random NONCE (16 bytes from
  `arc4random_buf` or `/dev/urandom`, hex), the exit status, and then the
  session's `$PWD` and exported environment — emit those through a
  NUL-separated, length-safe form. Recommended: a hidden `yew --yew-env0`
  mode (via `yew_job_self_exe()`) that prints `getcwd()` then `environ` as
  NUL-terminated records; the parser reads exactly that record set after
  the marker. Decide the exact encoding; the requirements are (a) a
  command's own output can never forge an end (the nonce), (b) a `$PWD` or
  a value containing newlines or bytes ≥ 0x80 survives exactly, (c) the
  parser never blocks the event loop on a partial record.
- Everything before the marker is the command's output and goes to that
  command's `*job:N*` buffer through the same sanitising path
  `YEW_SINK_BUFFER` uses today (`yew_job_safe_prefix`, invariant 2).

**State the session exposes**: `const char *yew_shsession_cwd(Ed *)` (last
reported, absolute), `const char *const *yew_shsession_env(Ed *)` (last
reported exports, NULL-terminated), `bool yew_shsession_busy(Ed *)`.
Before the first command completes, cwd is the workspace root and env is
`yew_job_env()`.

**Session end.** EOF on stdout (a command ran `exit` or `exec`, or the
shell died): finish the running command's job with the status if known,
drop the session, and message `shell session ended; the next :! starts a
new one`. The next plain `:!` starts a fresh session from the LAST known
directory and exports (so an accidental `exit` does not lose your `cd`).

**Pitfall — the prologue is not a command.** Its output (there should be
none) and any `.zshenv` noise before the first marker must not land in the
first command's buffer: discard output until the prologue's own marker.

### 2. Per-command jobs — `src/edit/shell.c`, `src/edit/job.[ch]`

The user-facing contract of `:!` is unchanged: each plain `:!cmd` gets its
own `*job:N CMD*` buffer, appears in the `*jobs*` table with its state and
exit status, is dismissed with Esc / `:q`, and can be cancelled. Those
jobs are now NOT processes: add the smallest job-layer concept that lets a
table entry be backed by the session (e.g. a job kind whose output is fed
by the session owner and whose signal is routed to it). Say what you chose
and why; do not fork a second jobs table.

**Cancel** sends `SIGINT` to the session's process group (the session is a
group leader — verify the job layer makes it one); the prologue's trap
keeps the shell alive, the command dies, the frame completes with its
status. A second cancel within 2 s escalates to `SIGKILL` of the group and
ends the session (§1's end path), so a command that traps INT cannot wedge
it.

**Busy** (§ Goals 3): a plain `:!` while a session command is running
spawns today's one-shot `$SHELL -c` with cwd = `yew_shsession_cwd` and the
environment = `yew_shsession_env`, and messages `session busy: ran outside
it (a cd here will not persist)`.

### 3. The other forms inherit — `src/edit/shell.c`, `src/edit/job.c`

`:!!`, `:r !`, `:%!`: spawn exactly as today but with cwd =
`yew_shsession_cwd(ed)` and the environment = `yew_shsession_env(ed)`
(then the job layer's usual overrides, so e.g. 57.31 §3's pager rule and
the rebase handover's explicit `PAGER=cat` still apply). With no session
yet (or `fresh`), they behave exactly as today.

### 4. Completion follows the session — `src/ui/shctx.c`, `src/ui/cmdcomp.c`

57.32's "directory `:!` commands start in" becomes `yew_shsession_cwd(ed)`
(workspace root when there is no session). Generator, help and fish jobs
get the session's exports. So `:!cd ch7` followed later by
`:!wolf build ou<Tab>` completes in `ch7/`, and the pager footer's `in …`
note is relative to the WORKSPACE root when the effective directory is not
it (so `in ch7/` shows even with no `cd` on the current line).

### 5. Commands and option — `src/edit/option.c`, `src/edit/cmd.c`

- `shell.session`: `persistent` (default) / `fresh`. Switching to `fresh`
  ends a running session (§1's end path, no message).
- `ed.shell.reset`: end the session now; the next `:!` starts fresh from
  the WORKSPACE root and `yew_job_env()` (a real reset, unlike an
  accidental `exit`). E-mode spelling `:shreset`. Invariant 9: this is the
  keyboard path to a clean shell.
- The `:!` prompt's hint shows the session directory when it differs from
  the workspace root.

### 6. Size and CI

**Do not touch `tests/size/`**; the orchestrator runs the CI rebaseline.
Report an added-bytes estimate. LeakSanitizer does not run on macOS: audit
every allocation in the frame parser and session teardown by reading, and
say so.

### 7. Defer

- rc files; fish-syntax sessions; job control in the session.
- Background jobs started inside the session (`cmd &`) may print into a
  LATER command's buffer: named limit, documented in the header.

## Testing Strategy

Tests use a fixture `$SHELL=/bin/sh` (and zsh / bash where installed, like
57.23's round trip), never the developer's rc.

- **Unit, persistence**: `cd sub` then `pwd` prints `sub`; `export X=1`
  then `echo $X`; `f() { echo hi; }` then `f`; a variable set without
  export persists inside the session but is NOT in the inherited env.
- **Unit, framing robustness**: a command printing a fake marker WITHOUT
  the nonce does not end the frame; `echo "unterminated` completes with a
  non-zero status and the NEXT command still works; a command whose output
  has no trailing newline; a `$PWD` containing a newline and bytes ≥ 0x80
  round-trips exactly; output split across many reads; 1 MiB of output.
- **Unit, lifecycle**: `exit` ends the session with the right message and
  the next `:!` restarts it in the LAST directory; `ed.shell.reset`
  restarts it in the workspace root; `shell.session = fresh` restores
  today's behaviour byte-for-byte (every existing shell test passes
  unchanged under it); a non-sh `$SHELL` (fixture `fish` name) falls back
  to fresh with one message.
- **Unit, cancel**: `sleep 30` cancelled → frame ends with the signal
  status, the session survives and the next command runs; a command that
  traps INT → second cancel escalates and ends the session.
- **Unit, busy**: a running `sleep 5`, then `:!pwd` runs alongside in the
  session's directory with the message.
- **Unit, inheritance**: after `cd sub; export Y=2`, `:!!` (stubbed as
  57.18 does), `:r !pwd`, `:%!` and a completion generator all run in
  `sub` with `Y=2`.
- **Unit, completion**: `:!cd ch7` then `:!wolf build ou<Tab>` completes
  from `ch7/` with the `in ch7/` footer.
- **PTY**: `s57_27_session_cd_persists`, `s57_27_session_exit_recovers`,
  `s57_27_session_busy_alongside`, `s57_27_completion_follows_session`.

## Definition of Done

1. `:!cd sub` then `:!pwd` prints `sub`; exports and functions persist;
   every §Testing row passes.
2. `shell.session = fresh` is byte-identical to today (all pre-existing
   shell/job tests pass under it).
3. A syntax error or a forged marker in a command can never hang or end
   the session wrongly.
4. Cancel keeps the session; double cancel ends it; `exit` recovers into
   the last directory; `:shreset` returns to the workspace root.
5. `:!!`, `:r !`, `:%!` and completion jobs start in the session's
   directory with its exports.
6. `yew_bind_active_count` recounted by running the test if bindings
   change; new commands registered (invariant-9 list, `command_name_valid`,
   batch refusal table where interactive).
7. The four `s57_27_*` goldens pass; full `make test-pty` green.
8. gcc AND clang warning-free (whole tree with `make -k CC=gcc-16
   BUILD=build-gcc FAULTSHIM_ARCH_FLAGS=`); always check `snprintf`'s
   result; never pass a variable by value in the same call that writes it
   through a pointer; `MODULES=""` builds and passes; ASan/UBSan filtered
   runs clean; `scripts/check-sigsafe.sh` green.
9. `test-fletch test-script test-roundtrip test-roundtrip-coverage
   test-audit` and the `scripts/check-*.sh` gates green.
10. `tests/size/` untouched; added-bytes estimate in the report.

## Implementation divergences (recorded at landing)

Where this contract was wrong about the code, or silent, the
implementation does what it intends:

1. **The per-command job is a PROXY job** (`YewJobProxyOps`,
   `YewJobSpec.proxy_owner/proxy_ops`, `yew_job_proxy_output`,
   `yew_job_proxy_end`): a table entry with pid/pgid 0 and no fds.  Its
   output goes through the same UTF-8/cluster hold as YEW_SINK_BUFFER
   (the text path of `job_drain` is now `job_deliver_text`, shared), its
   end marks it reaped with the frame's verdict, and `yew_job_settle`
   finishes it like any BUFFER job -- footer, message, `*jobs*` row,
   dismissal and the git refresh all unchanged.  `yew_job_signal` routes a
   proxy's signal to its owner.  One table, no second jobs list.
2. **The frame runs the command in a function whose INT trap returns.**
   The contract's bare `eval ...; printf status` form was verified on
   zsh, bash 3.2/5.3, dash, ksh and /bin/sh: the marker does arrive with
   130, but every shell then resumes the REST of the command's list
   (`echo started; sleep 30; echo not-reached` printed not-reached), so
   a cancelled `sleep 9; make install` would install.  The line is now
   `__yew_run() { trap 'return 130' INT; W eval "$__yew_c"; };
   __yew_c='CMD'; __yew_run </dev/null 9>&-; W printf <status> >&9;
   W trap : INT; W unset __yew_c; yew --yew-env0 >&9; W printf <end> >&9`.
   Verified by `test_shsession_cancel_keeps_the_session_on_every_shell`
   on every shell installed (here: /bin/sh, /bin/bash, Homebrew bash,
   /bin/zsh, /bin/dash, /bin/ksh -- output is exactly `started`).  The
   function takes no arguments, so `$#` is 0 as under `$SHELL -c`.
3. **`eval` alone is not exit-safe.**  dash (and bash --posix) EXIT on
   a syntax error in the special built-in `eval`; zsh's `command` runs
   only external programs.  The first frame probes which prefix W works
   (`command`, else `builtin`, else none) and every later line uses it,
   which also makes `eval`/`printf` immune to a user function of that
   name.  The probe frame's output is discarded (§1's pitfall).
4. **Markers go to fd 9**, the prologue's dup of the output pipe, closed
   for the command: a command's `exec >/dev/null` cannot swallow them.
   The nonce is per session AND each marker carries the frame's sequence
   number, so replaying an earlier frame's real marker is output too.
5. **Env transport**: after the status marker, `yew --yew-env0` prints
   `YEW0 <len>:` and exactly <len> bytes (cwd -- the logical `$PWD` when
   it names the same inode -- then each exported row, NUL-terminated;
   `_` omitted), then the end marker `\036<nonce>-<seq>.\036`.  A missing
   or failed helper, or a record over 8 MiB, completes the frame without
   touching the state.  SHLVL is replaced by yew's own (a child started
   FROM the state is not inside the shell).
6. **`yew_shsession_env` returns NULL** when there is no session state
   (none yet, `fresh`, after reset): callers hand it to the new
   `YewJobSpec.env_base`, where NULL is environ -- i.e. exactly
   `yew_job_env()`'s standard environment.  `yew_job_env` itself now
   builds from the session's exports, so the `$VAR` completer and the
   bang lexer's variable lookups follow the session.
7. **Pagers.**  The session base carries the PAGER=cat the job layer
   gave the session; a terminal-owning `:!!` child gets the parent's
   pager instead (57.31 §3), unless the user exported a different one in
   the session.  Inside the session a user's `export PAGER=less` is
   honoured; one-offs started from the state still get PAGER=cat.
8. **YEW_FILE/YEW_LINE/YEW_COL are exported with every frame** -- the
   contract was silent, and the shell's copies would otherwise be frozen
   at the caret where the session started.
9. **stderr is merged** (`exec 2>&1`), so a session command counts its
   stderr in `bytes_out`; `fresh` keeps Sprint 19's split.
10. **Status mapping**: a status above 128 after a cancel is SIGNALED
    (footer `[killed by SIGINT …]`); ksh93's 256+N is normalised.
11. **Session end is detected on the shell's reap**, once `poll` says
    its pipe has nothing more (so the tail of its output is never lost),
    not on EOF -- a background child holding the pipe cannot delay the
    recovery; its pipe closes when the dead shell is released.  `exit N`
    finishes its command with N.  A last directory that no longer exists
    restarts at the workspace root.
12. **Messages**: double cancel / kill_force SIGKILL the group at once;
    `shell session ended; the next :! starts a new one` is said when that
    shell is reaped (after the command's footer and the cancel's own
    line).  `:shreset` says `shell session reset; the next :! starts in
    the workspace root`.
13. **The prompt hint**: a bare `:!` body names no command, so it had no
    hint at all; it now reads `in ch7/` (the pager's note, via the new
    `yew_comp_where_note`) when the session is not at the workspace root,
    and a spelled-out shell command appends ` · in ch7/`.
14. **Commands**: `ed.shell.reset` (`:shreset`) is not RECORDABLE (like
    complete_forget: no CMDWORD, no round-trip row), is in the
    invariant-9 list, needs FL_CAP_SHELL by its prefix, and is not
    interactive (no batch refusal).  `reset` was already a verb.  No
    binding changed (389).
15. **Tests**: the unit runner (`tests/unit/unit_main.c`) handles
    `--yew-env0` and publishes argv[0], since the session runs
    `yew_job_self_exe()` and under the runner that is the runner.
    `test_shell_term_run_reports_every_outcome` now counts PUBLIC jobs
    (the session adds a hidden one).  `-DYEW_SHELL_SESSION_DEFAULT='"fresh"'`
    (EXTRA_CFLAGS) builds a `fresh`-default tree for the byte-identity
    lane.
16. **Goldens that moved** (inspected): `s19_exit_footer_nonzero`
    (`exit 3`) and `s19_exit_footer_signal` (`kill -TERM $$`) now end
    the SESSION shell, so their message line reads `shell session ended;
    the next :! starts a new one` (the `[exit 3 …]` / `[killed by
    SIGTERM …]` footers in the buffer are unchanged); `s57_30_table_bottom_exit`
    lists the new `shell.session` option (4/4).  Every pre-existing unit
    test passes on a `fresh`-default build (2970/2970, the session tests
    excluded).
17. **Named limits** are in `src/edit/shsession.h`: background jobs may
    print into a later command's buffer; a command that closes fd 9 or
    redefines `command`/`builtin` can stall its frame (cancel twice);
    `set -e`/`exit` in a command end the session (it recovers in the last
    directory).
