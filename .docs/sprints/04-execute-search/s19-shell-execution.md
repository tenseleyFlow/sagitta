# Sprint 19: Shell Execution — Jobs, Filters, and the Job Table

**MILESTONE: shell integration.** The tmux half of the editor begins here:
yew runs the user's shell, streams its output, and pipes regions through
it.

## Prerequisites

- Sprint 3 — `Tty` and the **signal self-pipe**:
  `yew_tty_drain_signals(t, &winch, &cont, &chld)` already delivers SIGCHLD
  as the `'H'` byte. s03 says it is "plumbed now, consumed from Sprint 19
  on" — this is that sprint.
- Sprint 5/15 — grid, damage, viewport, statusline, message line (the job
  badge and exit-status messages draw there).
- Sprint 7/8/9/10 — `TextBuf`, `TextIter` (region → pipe, zero copies),
  `FileMeta`, `Cursor`, `MarkSet` with bias, undo **transactions**.
- Sprint 12/13/17 — registers, the named-command registry, selections.
- Sprint 18 — E-mode prompt, `CmdArgv`, `CmdRange`, `YewCmdInvoke`, the
  expansion table, and the shell-history file kind (`history/shell`).
- Binding: `00-decisions.md` (optional runtime tools: `$SHELL`, degrade
  gracefully; single-threaded core — invariant 8; latency budgets are CI
  gates — invariant 4; no data loss — invariant 1), `01-architecture.md`
  (process model: per-job pipes in the poll set; "Shell (E mode): jobs run
  under `$SHELL -c`; output streams into a job buffer; foreground filters
  are synchronous with a timeout + cancel").

Assumptions beyond earlier index entries: (a) Sprint 10's transaction API is
`yew_undo_begin(Buffer*) / yew_undo_end / yew_undo_abort` with abort
restoring the pre-transaction state exactly; (b) the buffer list supports
scratch buffers with a `YEW_BUF_SCRATCH` flag (no file, refuses save) — if
Sprint 14 did not land the flag, it lands here.

## Goals

Land the generic child-process layer (`src/edit/job.c`) and E mode's shell
surface (`src/edit/shell.c`): fork/exec discipline, pipe wiring, nonblocking
reads registered with the `poll(2)` loop, SIGCHLD reaping through the
existing self-pipe, and the three consumption modes — async streaming into a
job buffer with follow-tail, the synchronous region filter (timeout, cancel,
rollback, one undo transaction), and read-output-at-cursor — plus the job
table UI, the exported environment contract, exit-status surfacing, and
`yew_shell_quote()`. A pty-backed interactive terminal buffer is **not** in
1.0 (§10, no sprint). LSP (Sprints 45–46) and AI-via-curl (Sprint 48) reuse
`yew_job_*` rather than forking their own process layer — a forward note,
not a dependency.

## Deliverables

### 1. Job model — `src/edit/job.h`, `src/edit/job.c`

```c
typedef enum { YEW_JOB_RUNNING, YEW_JOB_EXITED, YEW_JOB_SIGNALED,
               YEW_JOB_EXECFAIL, YEW_JOB_TIMEOUT, YEW_JOB_CANCELLED }
    YewJobState;

typedef enum { YEW_SINK_BUFFER,   /* append to a job buffer (mode a)     */
               YEW_SINK_COLLECT,  /* accumulate, deliver on exit (b/c)   */
               YEW_SINK_DISCARD } YewJobSink;

typedef struct YewJobSpec {
    const char *cmdline;       /* NULL when argv is given                */
    char      **argv;          /* argv form: no shell, no -c string      */
    const char *cwd;           /* NULL = workspace root                  */
    YewJobSink  sink;
    const TextBuf *in_buf; Span in_span;  /* stdin source; len 0 = none  */
    i64  timeout_ms;           /* 0 = none (async jobs)                  */
    u32  target_win;           /* for follow-tail / insert-at-mark       */
} YewJobSpec;

typedef struct YewJob {
    u32  id;  pid_t pid, pgid;
    int  in_fd, out_fd, err_fd, exec_fd;   /* parent ends; -1 when closed*/
    YewJobState state;  int exit_code, termsig, exec_errno;
    Bytebuf hold;             /* incomplete UTF-8 / cluster tail (§3)    */
    Bytebuf collect;          /* YEW_SINK_COLLECT                        */
    u64  in_off;              /* bytes of in_span already written        */
    u64  bytes_out, bytes_err;
    i64  start_ms, end_ms;
    u32  buf_id;  bool follow_tail;  MarkId at;   /* mode (c)            */
    char *cmd_display;        /* arena-owned, for the job table          */
} YewJob;

u32  yew_job_spawn(Ed *ed, const YewJobSpec *spec, char *err, size_t errsz);
void yew_job_collect_fds(Ed *ed, struct pollfd *pfd, u32 *n);  /* loop    */
void yew_job_pump(Ed *ed, const struct pollfd *pfd, u32 n);    /* loop    */
void yew_job_reap(Ed *ed);            /* on the self-pipe 'H' byte        */
bool yew_job_signal(Ed *ed, u32 id, int sig);   /* to the process GROUP   */
YewJob *yew_job_find(Ed *ed, u32 id);
```

Constants (pinned; each is referenced by a test):

| Constant | Value | Why |
|---|---|---|
| `YEW_JOB_MAX` | 32 | concurrent jobs; spawn past it errors, never queues silently |
| `YEW_JOB_READ_BUDGET` | 256 KiB | max bytes drained per job per loop iteration |
| `YEW_JOB_LINE_MAX` | 1 MiB | a single output "line" longer than this is split; prevents a `tr -d '\n'` firehose from making one unbounded line |
| `YEW_JOB_HOLD_MAX` | 64 B | max bytes held back for cluster completion (§3) |
| `YEW_FILTER_TIMEOUT_MS` | 5000 | option `filter_timeout` |
| `YEW_JOB_TERM_GRACE_MS` | 200 | SIGTERM → SIGKILL escalation |

### 2. fork/exec discipline

`$SHELL` resolution, in order: `$SHELL` → `getpwuid(getuid())->pw_shell` →
`/bin/sh`. If the resolved shell does not exist, the *exec-status* channel
(below) reports it precisely; a missing `$SHELL` is a graceful message, not
a crash (locked: optional runtime tools degrade gracefully).
**`system()` is banned repo-wide** — it re-quotes through `/bin/sh`,
ignores `$SHELL`, and blocks the loop. `popen()` likewise (grep gate).

Spawn sequence, exactly this order:

```c
/* BEFORE fork: everything that allocates. argv, envp, and cwd are fully
 * built here — between fork() and execve() only async-signal-safe calls
 * are legal, and malloc in a forked child can deadlock on the allocator
 * lock held by the parent at fork time.                                 */
build argv:  spec->argv ? spec->argv : (char*[]){ shell, "-c", cmdline, 0 }
build envp:  copy environ + the §7 table (never putenv() in the parent)
yew_pipe_cloexec(in_p);         /* parent writes in_p[1]                 */
yew_pipe_cloexec(out_p);
yew_pipe_cloexec(err_p);
yew_pipe_cloexec(exec_p);       /* child->parent errno on exec failure   */
pid = fork();
child:
    setpgid(0, 0);              /* also done in the parent — see pitfall */
    dup2(in_p[0], 0); dup2(out_p[1], 1); dup2(err_p[1], 2);  /* clears
                                   CLOEXEC on exactly 0/1/2              */
    close every other pipe fd (including exec_p[0])
    signal(SIGPIPE, SIG_DFL); signal(SIGINT, SIG_DFL); ...   /* reset all */
    sigprocmask(SIG_SETMASK, &empty, NULL);                  /* unblock   */
    chdir(cwd);
    execve(argv[0], argv, envp);
    { int e = errno; write(exec_p[1], &e, sizeof e); }        /* 4 bytes  */
    _exit(127);
parent:
    setpgid(pid, pid);          /* race-proof: both sides call it        */
    close(in_p[0]); close(out_p[1]); close(err_p[1]); close(exec_p[1]);
    fcntl(O_NONBLOCK) on in_p[1], out_p[0], err_p[0], exec_p[0];
    if (spec->in_span is empty) { close(in_p[1]); in_fd = -1; }
```

Pitfalls, each with a test:

- **Forgetting to close the parent's copy of the child's stdin write end**
  (or the read ends of out/err) is the signature hang of this subsystem.
  The child's `read()` never sees EOF, `sort` never sorts, and the symptom
  presents as "that command is slow", not as a deadlock — nobody suspects
  an fd. The parent closes `in_fd` the instant the region is fully written
  (§5), and immediately when there is no stdin at all.
- **`setpgid` race:** whichever side runs first must succeed, so both call
  it. Killing always targets `-pgid` — `kill(pid)` leaves the shell's
  *children* running and `:!sleep 100 | cat` becomes unkillable.
- **`SIGPIPE`:** the parent sets `SIG_IGN` at startup (writes to a dead
  child must return `EPIPE`, not kill the editor); the child must restore
  `SIG_DFL`, or `head`-style pipelines inside the command never terminate.
- **exec failure vs command failure:** without the `exec_p` channel a
  missing shell and a command exiting 127 are indistinguishable. Four bytes
  of `errno` down a CLOEXEC pipe (which closing empty *is* the success
  signal) makes it exact: `cannot run /bin/fish: No such file or directory`.
- **No `yew_log` between fork and exec** — it opens files and formats with
  stdio; the exec-status pipe is the only diagnostic channel in that window.
- All production pipe creation routes through `yew_pipe_cloexec`. It uses
  `pipe2(O_CLOEXEC)` on Linux/FreeBSD and a portable macOS fallback of
  `pipe()` followed immediately by `FD_CLOEXEC`. The fallback is race-free
  because the core is single-threaded and never forks from a signal handler;
  no direct caller may reproduce that sequence elsewhere (S59-A16).

### 3. Reading: nonblocking, budgeted, grapheme-safe

`yew_job_collect_fds` adds `out_fd`, `err_fd` (POLLIN), `exec_fd` (POLLIN),
and `in_fd` (POLLOUT, only while stdin remains to write) to the loop's
pollfd array. `yew_job_pump` drains each ready fd with `read()` until
`EAGAIN` **or** `YEW_JOB_READ_BUDGET` bytes, whichever comes first. Why the
budget: `yes` produces gigabytes per second; an unbudgeted drain starves
keystrokes and blows the 5 ms p99 gate (invariant 4). A job that hits its
budget stays in the poll set and continues next iteration.

Byte-boundary rules (invariant 2 — no byte confusion):

```c
/* Returns the number of bytes safe to append now; the tail stays in
 * job->hold until more bytes arrive or the pipe reaches EOF.            */
static u64 yew_job_safe_prefix(const u8 *b, u64 n, bool at_eof);
```

1. A read may split a UTF-8 sequence: hold back a trailing incomplete
   sequence (≤ 3 bytes) — s02's decoder identifies it.
2. A read may split a **grapheme cluster** (the next read could deliver a
   combining mark, a ZWJ continuation, or a regional indicator). Hold back
   the final cluster too, unless the buffer's last byte is `\n` (a newline
   is a guaranteed cluster terminator and the common case, so streaming
   line-oriented output holds back nothing).
3. Never hold more than `YEW_JOB_HOLD_MAX` bytes: a pathological stream of
   combining marks with no boundary must not grow `hold` without bound.
   At the cap, flush verbatim.
4. At EOF flush everything, including an invalid tail. Invalid bytes are
   preserved byte-exact (s02's lossless escape policy governs *rendering*,
   never storage — s08's rule, unchanged here).
5. `\r` and NUL are stored verbatim. A job buffer is text, not a terminal:
   nothing interprets CR, backspace, or ANSI sequences (a progress bar
   therefore appears as successive lines). Interpreting them requires a
   pty-backed terminal buffer — explicitly **not in 1.0**.

**Pitfall — `PIPE_BUF`:** `PIPE_BUF` (512 POSIX / 4096 Linux) guarantees
atomicity only for a *writer* writing ≤ `PIPE_BUF` bytes in one call. It
guarantees the reader nothing: a `read()` may return one byte, half a line,
or half a codepoint, regardless of how the child wrote it. No code in this
sprint may assume a read contains a whole line, a whole record, or a whole
character.

### 4. Mode (a) — async job streaming into a job buffer

`:!cmd` (and `ed.shell.run`) spawns with `sink = YEW_SINK_BUFFER` into a
scratch buffer named `*job:<id> <cmd>` (`YEW_BUF_SCRATCH`: no file, save
refuses with `this is a job buffer`).

- The buffer is **undo-exempt** (`YEW_BUF_NOUNDO`): recording a million
  appends as undo ops is nonsense and would journal them (s08). Undo in a
  job buffer is a no-op with the message `job buffers have no undo`.
- stdout and stderr both append to the same buffer, stderr lines carrying
  the `YEW_ATTR_JOB_STDERR` attribute span. Why one buffer: two buffers
  desync ordering and force the user to correlate by eye. Caveat to
  document, not to "fix" with a pty: interleaving is best-effort — the
  kernel does not order two pipes, and a child whose libc block-buffers a
  non-tty stdout emits it in one late burst.
- **Follow-tail**, pinned: an append moves the window's cursor to the new
  end **iff** the cursor was at buffer end immediately before it *and* the
  window has no selection. Any manual scroll or selection detaches follow;
  `ed.move.buf_end` re-attaches. Yanking the view to the bottom while
  someone is reading the middle is the classic complaint.
- Damage: an append damages only the lines it touched plus the statusline;
  streaming never triggers a full repaint, and appends coalesce per loop
  iteration (one render per input burst, the architecture rule).
- Completion footer, appended as the last line of the buffer and mirrored
  to the message line:

| Outcome | Footer |
|---|---|
| exit 0 | `[exit 0 in 1.24s]` |
| exit N | `[exit 3 in 0.11s]` |
| signal | `[killed by SIGTERM after 2.00s]` |
| exec failure | `[cannot run /bin/fish: No such file or directory]` |

- **"Command produced no output":** if a job ends with `bytes_out +
  bytes_err == 0`, the job buffer is **not** opened or focused; the message
  line shows `:!ls — no output (exit 0)` (or `… (exit 1)`). Pin it: a
  full-screen empty buffer after every `:!make` is the most-hated behavior
  of every editor that does not special-case this.
- `ed.shell.run_bg` is the same thing without stealing focus; both record
  the command in the `history/shell` file (s18 §3).

### 5. Mode (b) — synchronous region filter

`:'<,'>!fmt`, `:%!sort` (`ed.shell.filter`): pipe a region through a command
and replace it with the output.

```c
typedef enum { YEW_FILT_OK, YEW_FILT_NONZERO, YEW_FILT_TIMEOUT,
               YEW_FILT_CANCELLED, YEW_FILT_SPAWN } YewFilterResult;

YewFilterResult yew_shell_filter(Ed *ed, Win *w, Span region,
                                 const char *cmdline, Bytebuf *stderr_out);
```

This is the delicate one. Rules, all pinned:

- **Not a nested event loop.** The filter runs a *restricted* poll loop over
  {job fds, tty stdin, signal self-pipe} only. Terminal input read during
  the filter is **buffered as typeahead and replayed afterwards** — never
  dispatched (that re-enters the editor with a half-applied transaction)
  and never dropped. The only key inspected is the cancel key (`^C`/Esc),
  which is consumed rather than replayed. SIGWINCH is honored (resize +
  repaint); the filter keeps running.
- **Interleaved write/read — the deadlock.** Writing the whole region
  before reading stdout deadlocks as soon as the child's output exceeds the
  pipe capacity (64 KiB on Linux): the child blocks writing, we block
  writing, nobody moves. The loop must poll `POLLOUT` on stdin and `POLLIN`
  on stdout/stderr in the same `poll()`, writing chunks straight from
  `TextIter` and draining output every iteration.
- **Close stdin at end-of-region.** `sort`, `sha256sum`, and every
  accumulating filter emit nothing until EOF; not closing is §2's hang.
- `EPIPE` on the stdin write (`head -1` exiting early) is **not an error**:
  stop writing, close stdin, keep draining output.
- Timeout `filter_timeout` (default 5000 ms) and cancel (`^C`/Esc) both
  send SIGTERM to the process group, wait `YEW_JOB_TERM_GRACE_MS`, then
  SIGKILL, yielding `YEW_FILT_TIMEOUT` / `YEW_FILT_CANCELLED`.

Outcome table — what happens to the buffer:

| Result | Buffer | Message line |
|---|---|---|
| `YEW_FILT_OK` (exit 0) | region replaced by stdout, one undo step | `filter: 412 → 388 lines` |
| `YEW_FILT_NONZERO` | **untouched** | last stderr line + `(exit N)`; full stderr → `*job:` buffer |
| `YEW_FILT_TIMEOUT` | untouched | `filter timed out after 5.0s` |
| `YEW_FILT_CANCELLED` | untouched | `filter cancelled; buffer unchanged` |
| `YEW_FILT_SPAWN` | untouched | the exec-failure message |
| exit 0, empty output | region **deleted** | `filter produced no output; region deleted (undo restores)` |

Rollback on nonzero exit is deliberate: `sort` on malformed input exits 2
with a diagnostic, and replacing the region with that diagnostic destroys
the text *and* hides the error. The empty-output row is *not* rolled back
(`grep` legitimately matches nothing) but it is announced — a silently
vanished selection reads as data loss.

**One undo transaction, always:**

```c
yew_undo_begin(w->buf);                       /* s10                      */
yew_textbuf_delete(tb, region);              /* marks/cursors adjust via  */
yew_textbuf_insert(tb, region.lo, out, n);   /* the single edit chokepoint*/
if (ok) yew_undo_end(w->buf); else yew_undo_abort(w->buf);
```

The transaction wraps delete+insert so one undo restores the original text
exactly; `yew_undo_abort` guarantees the "untouched" rows even when a
failure is discovered after the delete (it is not, in this ordering — we
buffer output first — but the abort path is the safety net and is fault-
injection tested). The journal (s08) sees one commit, so a crash mid-filter
recovers to either the pre- or the post-filter state.

### 6. Mode (c) — read command output at the cursor

`:r !date` (`ed.shell.read`): spawn with `sink = YEW_SINK_COLLECT` and an
insertion `Mark` (bias RIGHT, s09) at the cursor. Output is **collected in
full and inserted once, on exit, as one transaction** — streaming a
command's output into the middle of the user's document while they type is
chaos, and the mark keeps the insertion point correct if they edit
elsewhere meanwhile. Nonzero exit inserts nothing and reports like §5.
Collection is capped at `YEW_JOB_COLLECT_MAX` (64 MiB); past the cap the
job is killed and reported (`output too large; use :! instead`).

A trailing newline is added iff the insertion point is at a line start and
the output does not end with one — pinned, so `:r !date` on an empty line
does not glue the next line onto it.

### 7. Environment contract (pinned)

Exported for every job, added to a copy of `environ` (the parent's
environment is never mutated — `putenv` in the parent leaks into every
later job and into the editor itself):

| Variable | Value | Absent when |
|---|---|---|
| `YEW_FILE` | current buffer's path as the user gave it | buffer unnamed → set to `""` |
| `YEW_LINE` | primary cursor line, 1-based decimal | never |
| `YEW_COL` | primary cursor **grapheme** column, 1-based (`GCol + 1`) | never |
| `YEW_WORKSPACE` | `yew_ws_root()` realpath | never |
| `YEW_JOB` | `1` — lets scripts detect they run under yew | never |
| `PAGER`, `GIT_PAGER` | forced to `cat` | never |
| `COLUMNS`, `LINES` | **unset** (removed if present) | always |

- `YEW_COL` is grapheme columns, not bytes and not cells — it is the number
  the statusline shows, so scripts and the UI agree.
- The **selection is not exported**. A 2 MiB selection is `E2BIG` at
  `execve`, whose failure mode is a command that mysteriously refuses to
  run above some selection size. Selections reach a command through stdin
  (§5) or as a quoted argv element via s18's `%s`.
- `PAGER=cat` kills the hang class where a job spawns `less`, which waits
  forever on a stdin it does not own, producing no output and no exit.
- `COLUMNS`/`LINES` are removed so output is deterministic across window
  sizes — the same command must produce the same buffer in CI and on a
  laptop (invariant 5's spirit).

### 8. Job table UI — `src/edit/shell.c` + a scratch buffer

`ed.job.list` opens `*jobs*` (scratch, re-rendered on any job state change):

```
 id  state     code  elapsed    bytes  command
  3  running      -    12.4s    1.2Mi  tail -f build.log
  2  exited       0     0.31s     840  git status --porcelain
  1  signaled  TERM     2.00s      0    sleep 100
```

Keys (registered commands, bound in the `*jobs*` buffer's keymap layer):
`Enter` → `ed.job.jump` (focus that job's output buffer), `d` →
`ed.job.kill` (SIGTERM to the group), `D` → SIGKILL, `x` →
`ed.job.clear_finished`, `r` → re-run the command line. Columns are drawn
through the s22 `Rect`/layout rule (no width math outside `src/unicode/`).
The statusline (s15) carries a job badge `⟨2⟩` when any job is running, with
the count in the theme's accent; it disappears at zero.

### 9. Security note and `yew_shell_quote()`

We run the user's shell on the user's behalf. **No sandboxing claim is
made**: a job has exactly the user's privileges and can do anything their
shell can. Capability gating exists only for *plugins* (Sprint 54) — a
plugin-initiated job passes the capability check first; the editor's own
`:!` does not, because the user typed it.

The rule that keeps this from becoming an injection surface:

> Programmatically constructed commands MUST use argv arrays
> (`YewJobSpec.argv`), never string interpolation into `cmdline`.

The `-c` string form exists for exactly one input: text the user typed into
the E-mode prompt (or a Fletch script holding the shell capability).
Wherever the editor assembles a command from data — a filename, a
selection, an LSP root, a git ref — it passes an argv array and no shell is
involved. `grep -rn 'bytebuf_printf.*cmdline\|sprintf.*shell' src/` is a CI
gate.

For the unavoidable case — s18's `%`/`%s` expansions landing inside a `:!`
line the *user* is composing — `yew_shell_quote()` emits a POSIX-sh-safe
single-quoted form:

```c
/* 'foo bar'  ->  ''\''foo bar'\''' style; empty string -> ''            */
void yew_shell_quote(Bytebuf *out, const u8 *s, size_t n);
```

Algorithm, pinned: wrap in `'…'`; every `'` becomes `'\''`; nothing else is
escaped (inside single quotes POSIX sh treats every other byte literally,
including newline, `$`, backtick, and backslash). Bytes pass through
unchanged, invalid UTF-8 included — the shell speaks bytes.

### 10. Defer

- **Interactive pty-backed terminal buffer** (`:term`): **not in 1.0**.
  `ed.shell.term` still rejects terminal emulation as a permanent non-goal.
  Sprint 57.18's `:!!` gives Yew's real terminal to one child and restores it;
  it does not create a pty-backed buffer (S59-A15).
- Job persistence across restart: never — jobs die with the process, and
  Sprint 25's schema notes say so.
- LSP/AI/git reuse of `yew_job_*`: Sprints 45/46, 48, 51 add framed sinks;
  the spawn/reap/poll core is final here. Fletch `sh.run` with capability
  checks: Sprints 34/54.
- Sprint 21 delivered `:s` and `/re/` through the parser and search seams
  established by Sprints 18–20 (S59-A15).

## Testing Strategy

- **Unit** (`tests/unit/test_job.c`): full lifecycle of `/bin/echo hi`;
  exec failure (`argv[0] = "/nonexistent"`) reports `ENOENT` through the
  exec pipe, never as exit 127; exit 3; killed-by-signal;
  `YEW_JOB_MAX + 1` refused with the exact message; fd accounting
  (`/proc/self/fd`, else an `fcntl(F_GETFD)` sweep) before and after 500
  spawn/reap cycles, delta 0.
- **Unit** (`tests/unit/test_job_stream.c`): the `yew_job_safe_prefix`
  table — ≥ 20 byte strings (ASCII, 2/3/4-byte sequences, ZWJ family,
  regional-indicator pair, combining marks, CRLF, invalid `0xFF`) fed split
  at **every** byte offset, asserting the appended bytes equal the
  whole-stream case (s03's chunking-independence discipline applied to
  pipes); `YEW_JOB_HOLD_MAX` overflow flushes.
- **Unit + fuzz** (`test_shell_quote.c`, `fuzz/fuzz_shell_quote.c`): random
  byte strings (quotes, newlines, `$`, backslash, invalid UTF-8, empty) →
  quote → `/bin/sh -c "printf %s <quoted>"` → byte-compare; 100 000 cases.
- **Script** (`tests/script/filter_*` — the headless C driver
  `tests/unit/test_filter.c` until Sprint 37): every §5 outcome row on a
  fixture buffer, asserting content, undo depth == 1 after success, and
  byte-identity after every failure row; the empty-output row; a 4 MiB
  output (64× pipe capacity — proves the interleaved loop); `head -1` on a
  10 MiB region (`EPIPE` tolerance); `cat` over invalid UTF-8 (byte-exact).
- **Deadlock regression**: a fixture command writing 1 MiB to stdout
  *before* reading stdin, filtered over a 1 MiB region, must complete —
  this single test is why §5's loop is shaped as it is; label it so.
- **Pty** (`tests/pty/job_*.golden`): streaming with follow-tail on and
  off, the no-output message, all four exit footers, the `*jobs*` table and
  kill-from-table, the job badge appearing and clearing, filter cancel
  mid-run (synthetic `^C`).
- **Torture** (`make torture`, extending s08's target): 200 spawn/kill
  cycles at random timings → zero zombies (`waitpid(-1, …, WNOHANG)` →
  `ECHILD` at quiesce), zero leaked fds, editor still responsive.
- **Perf**: 50 MiB streamed into a job buffer under a scripted keystroke
  load — keypress p99 ≤ 5 ms, enforced by the read budget; baseline
  committed. Lanes: gcc/clang, ASan/UBSan, valgrind
  (`--trace-children=no`).

## Definition of Done

1. `make test` green on both compilers, zero warnings; ASan/UBSan/valgrind
   lanes green.
2. `system()` and `popen()` remain absent. All production pipe creation
   routes through `yew_pipe_cloexec`: Linux/FreeBSD use
   `pipe2(O_CLOEXEC)`, the portable macOS fallback uses `pipe()` plus
   immediate `FD_CLOEXEC`, and no other direct pipe caller exists (S59-A16).
3. Exec failure is distinguishable from exit 127: `YEW_JOB_EXECFAIL` +
   `exec_errno == ENOENT` for a missing binary vs `YEW_JOB_EXITED` +
   `exit_code == 127` for `sh -c 'exit 127'`.
4. Grapheme-safety: the split-at-every-offset test passes for all ≥ 20
   fixtures; a debug-build assertion proves no partial UTF-8 sequence ever
   reaches a job buffer.
5. All six §5 outcome rows verified, including byte-identity on every
   failure row and **undo depth exactly 1** after a successful filter and
   after a 10 000-line filter.
6. The 1 MiB-output-before-reading-stdin fixture completes in < 2 s;
   removing the `POLLOUT`/`POLLIN` interleave hangs it (documented in the
   test's comment).
7. Timeout and cancel leave the buffer byte-identical and reap the whole
   group: `kill(-pgid, 0)` → `ESRCH` afterwards.
8. Zero zombies and zero leaked fds after the 200-cycle torture lane and
   the 500-spawn fd-accounting test.
9. The §7 environment table is exported exactly — a job running `env` shows
   all pinned rows and no `COLUMNS`/`LINES`; `YEW_COL` matches the
   statusline column on a line containing a ZWJ emoji family.
10. `yew_shell_quote` fuzz: 100 000 random byte strings round-trip through
    `/bin/sh` byte-exact, embedded newlines and quotes included.
11. `:!true` opens no buffer and messages exactly `:!true — no output
    (exit 0)` (pty golden).
12. `ed.shell.term` hard-errors with the terminal-emulator permanent-non-goal
    message while `:!!` uses the tested terminal-handover path; Sprint 21's
    live `:s` and `/re/` routes pass their successor coverage (S59-A15).
13. Perf gate: 50 MiB of streamed output keeps keypress p99 ≤ 5 ms;
    baseline committed under `tests/perf/`.
14. ≥ 110 assertions across the four new unit files.
