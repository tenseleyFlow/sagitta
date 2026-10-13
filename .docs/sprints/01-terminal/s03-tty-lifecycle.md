# Sprint 3: TTY Lifecycle — Raw Mode, Restore Guarantee, Capability Probes

## Prerequisites

- Sprint 0 — util containers (`Arena`, `Vec(T)`, `Bytebuf`), `yew_log()` to
  `$XDG_STATE_HOME/yew/log`, `yew_bug()`/`YEW_BUG` with the exit-code
  contract (0/1/2/3/4), the one Makefile, `--version/--help` driver shell.
- Sprint 1 — unit harness (registry table, no ctor attributes), `make test`,
  CI lanes: gcc, clang, ASan/UBSan, determinism.
- Sprint 2 — UTF-8 core (not consumed here; Sprint 5 needs the width tables).
- Binding: `00-decisions.md` (Terminal, Terminal restore, invariant 6),
  `01-architecture.md` (process model, `src/term/` layout).
- Assumption (not named by the Sprint 0 index entry): `yew_bug()` exposes a
  single pre-report hook slot `void yew_bug_set_prehook(void (*fn)(void))`.
  If Sprint 0 did not land it, add it in this sprint — the structured bug
  report is unreadable on a raw terminal (§4).

## Goals

Land `src/term/tty.c|h`: the `Tty` object, termios raw mode with an exact,
tested flag policy, alternate-screen enter/leave, window-size tracking via a
SIGWINCH self-pipe, suspend/resume, and the terminal-restore guarantee on
every exit path — normal quit, `atexit`, fatal signals, `yew_bug()`. Also
land terminfo-free capability probing (locked decision): DA1, DECRQM 2026,
kitty-protocol query, truecolor env heuristics, behind a deadline state
machine so a dumb terminal never hangs startup. Input decoding lands in
Sprint 4 (bytes not consumed by the probe recognizer are buffered raw);
grid/render in Sprint 5; pty-level automated verification in Sprint 6 —
this sprint's coverage is unit-level on the pure parts.

## Deliverables

### 1. The `Tty` object — `src/term/tty.h`, `src/term/tty.c`

```c
typedef struct TtyCaps {
    bool probed;        /* probe reached DONE (fence reply or deadline)   */
    bool truecolor;     /* §7 env heuristics — no query exists            */
    bool kitty_kbd;     /* CSI ? u answered                               */
    u32  kitty_flags;   /* flags value from the reply (current, not max)  */
    bool sync_output;   /* DECRQM 2026 answered set/reset/perm-set        */
    bool da1_seen;      /* fence arrived (false = deadline expiry)        */
} TtyCaps;

typedef struct Tty {
    int             rfd, wfd;     /* 0 and 1; both must satisfy isatty()  */
    struct termios  saved;        /* pre-raw state, restored verbatim     */
    bool            raw, alt;
    int             rows, cols;
    int             sigpipe[2];   /* self-pipe: [0] polled, [1] handlers  */
    TtyCaps         caps;
    Bytebuf         pending;      /* non-reply bytes seen during probing  */
    u8              pstate;       /* §6 probe state machine               */
    i64             pdeadline;    /* CLOCK_MONOTONIC ms; 0 = unarmed      */
} Tty;

bool   yew_tty_open(Tty *t);          /* isatty, tcgetattr, self-pipe     */
bool   yew_tty_raw(Tty *t);           /* §2; arms atexit + handlers       */
void   yew_tty_restore(void);         /* §4: idempotent, async-sig-safe   */
void   yew_tty_close(Tty *t);         /* restore + free; NOT for handlers */
bool   yew_tty_winsize(Tty *t);       /* TIOCGWINSZ → rows/cols           */
void   yew_tty_altscreen(Tty *t, bool on);
int    yew_tty_signal_fd(const Tty *t);
void   yew_tty_drain_signals(Tty *t, bool *winch, bool *cont, bool *chld);
void   yew_tty_suspend(Tty *t);       /* editor-command suspend, §5       */
void   yew_tty_probe_start(Tty *t, i64 now_ms);
size_t yew_tty_probe_feed(Tty *t, const u8 *b, size_t n);
void   yew_tty_probe_tick(Tty *t, i64 now_ms);   /* deadline check        */
bool   yew_tty_probe_done(const Tty *t);
i64    yew_tty_probe_deadline(const Tty *t, i64 now_ms); /* poll timeout  */
```

- Exactly one `Tty` per process. The restore path's state (saved termios,
  raw-mode flag, wfd) is duplicated into `static` storage inside `tty.c` —
  signal handlers take no arguments, so the handler-visible state must be
  global. Why not make `Tty` itself global: the testable surface (probe
  machine, flag transform) stays pure and instance-based; only the
  restore/handler sliver is static.
- All time is injected (`now_ms` parameters, `CLOCK_MONOTONIC` read by the
  caller). `tty.c` makes no clock syscalls — unit tests drive deadlines
  with a fake clock.
- `yew_tty_open` on a non-tty stdin/stdout exits 1. At this sprint's landing
  the diagnostic named the then-future Sprint 37; after Sprint 37 delivered
  batch mode, the current diagnostic is
  `yew: error: stdin is not a terminal; use --batch SCRIPT for headless
  execution` (S59-A10).
- No `yew_log` between raw-enter and restore *to stdout/stderr* — the
  Sprint 0 logger already writes to the XDG state file; this sprint adds
  the CI grep that keeps `printf`-family calls out of `src/term/`.

### 2. Raw mode — the exact flag table

`void yew_tty_rawios(struct termios *io);` is a **pure transform** applied
to a copy of `saved` (unit-testable without a tty):

| Field | Bits | Action | Why |
|---|---|---|---|
| `c_iflag` | `IXON` | clear | ^S/^Q become keys; no "frozen editor" support calls |
| `c_iflag` | `ICRNL` | clear | Enter arrives as CR 0x0D, distinct from ^J 0x0A |
| `c_iflag` | `INLCR` `IGNCR` | clear | no NL/CR rewriting on input |
| `c_iflag` | `BRKINT` `IGNBRK` `PARMRK` | clear | break is data, never SIGINT or 0xFF escapes |
| `c_iflag` | `ISTRIP` | clear | stripping bit 7 corrupts UTF-8 |
| `c_iflag` | `INPCK` | clear | no parity on ptys; off for determinism |
| `c_oflag` | `OPOST` | clear | renderer bytes reach the wire exactly (no \n→\r\n) |
| `c_lflag` | `ECHO` `ECHONL` | clear | we draw every glyph; the kernel draws none |
| `c_lflag` | `ICANON` | clear | byte-at-a-time reads, no line assembly |
| `c_lflag` | `ISIG` | clear | ^C/^Z/^\ are keys; suspend is an editor command (§5) |
| `c_lflag` | `IEXTEN` | clear | ^V/^O are keys (kills LNEXT/DISCARD) |
| `c_cflag` | `CSIZE` `PARENB` | clear | then… |
| `c_cflag` | `CS8` | **set** | 8-bit characters, no parity |
| `c_cc` | `VMIN=0` `VTIME=0` | set | reads never block; poll(2) is the only wait site |

- **Pitfall (`VMIN=1`)**: a `read` after `poll` can still block (signal
  wakeups, spurious readiness). `VMIN=0` makes every read a poll-guarded
  nonblocking drain.
- **Pitfall (tcsetattr flush mode)**: enter with `TCSANOW` — `TCSAFLUSH`
  on enter discards keys the user typed before first paint. Restore with
  `TCSAFLUSH` — dumping unread bytes stops the shell from executing the
  tail of a mouse report after exit.
- **Pitfall (partial application)**: `tcsetattr` reports success if *any*
  attribute was applied (POSIX). After setting, `tcgetattr` again and
  compare the six fields above; a mismatch is a hard error naming the
  terminal as broken, exit 3.

### 3. Alternate screen and cursor save/restore

- Enter: `ESC 7` (DECSC), then `CSI ? 1049 h`. Leave: `CSI ? 1049 l`,
  then `ESC 8` (DECRC).
- Why 1049 over 47 or the 1047+1048 pair: one atomic save-cursor +
  switch + clear; the split forms are legacy xterm and mis-order on some
  emulators. The explicit DECSC/DECRC pair is belt-and-suspenders for
  terminals whose 1049 forgets the cursor.
- **Pitfall**: DECSC has exactly one save slot per screen — never nest
  DECSC without an intervening DECRC; `tty.c` owns the only pair.
- Cursor visibility (`CSI ?25l/h`) is owned by the renderer (Sprint 5);
  the restore blob unconditionally re-shows it (§4).
- Exit ordering: SGR reset → leave alt → show cursor — the primary screen
  must never be stained with our attributes.

### 4. The restore guarantee (invariant 6)

Everything the restore path touches is static, precomputed, and
async-signal-safe:

```c
/* BEGIN ASYNC-SIGNAL-SAFE — allowlist: write tcsetattr sigaction signal
 * raise kill. CI enforces (scripts/check-sigsafe.sh). No fprintf, no
 * malloc, no yew_log, no snprintf, no exit: stdio locks and heap state
 * are undefined inside a SIGSEGV handler.                               */
static const char YEW_TTY_RESTORE_BLOB[] =
    "\x1b[<u"         /* pop kitty flags (no-op if never pushed)     */
    "\x1b[?2004l"     /* bracketed paste off                         */
    "\x1b[?1002l"     /* mouse tracking off                          */
    "\x1b[?1006l"     /* SGR mouse encoding off                      */
    "\x1b[?1004l"     /* focus events off                            */
    "\x1b[?2026l"     /* end synchronized update if crashed mid-frame*/
    "\x1b[0m"         /* SGR reset                                   */
    "\x1b[?1049l"     /* leave alternate screen                      */
    "\x1b[?25h";      /* show cursor                                 */

void yew_tty_restore(void) {          /* idempotent; re-armed per raw() */
    if (!g_raw) return;
    g_raw = 0;
    (void)!write(g_wfd, YEW_TTY_RESTORE_BLOB, sizeof YEW_TTY_RESTORE_BLOB - 1);
    (void)tcsetattr(g_wfd, TCSAFLUSH, &g_saved);
}

static void yew_tty_fatal(int sig) {
    yew_tty_restore();
    static const char note[] = "yew: fatal signal, terminal restored\r\n";
    (void)!write(2, note, sizeof note - 1);
    signal(sig, SIG_DFL);
    raise(sig);       /* pending while blocked; delivered on handler
                         return → correct killed-by-signal wait status */
}
/* END ASYNC-SIGNAL-SAFE */
```

- Disabling never-enabled modes is a no-op on every terminal — the blob is
  one static string, zero formatting in handlers, `write` in one syscall.
- Exit paths, all covered this sprint:
  1. normal quit — `yew_tty_close()`;
  2. `exit()` anywhere — `atexit(yew_tty_restore)`, registered once on
     first `yew_tty_raw()`;
  3. `SIGSEGV SIGBUS SIGABRT SIGTERM` — `yew_tty_fatal`: restore →
     re-raise. Why SIGTERM is in the fatal set for now: there is no
     buffer to journal yet; restore→re-raise is strictly safe and keeps
     the killed-by-TERM wait status. Sprint 8 upgrades SIGTERM to a
     graceful self-pipe shutdown once the crash journal exists.
  4. `yew_bug()` (exit 4) — `yew_bug_set_prehook(yew_tty_restore)` so the
     structured report prints on a cooked terminal. The prehook runs
     before the report; `yew_bug` is never called from signal handlers.
- **Pitfall**: `raise` inside a `sigaction`-installed handler (no
  `SA_NODEFER`) marks the signal pending; delivery happens at handler
  return with the default disposition — do not `_exit()` instead, that
  destroys the crash's wait status and core dump.

### 5. SIGWINCH self-pipe; suspend/resume

```c
static void yew_tty_signote(int sig) {   /* WINCH / CONT / CHLD → 1 byte */
    int e = errno;                        /* write clobbers errno         */
    u8 b = sig == SIGWINCH ? 'W' : sig == SIGCONT ? 'C' : 'H';
    (void)!write(g_sigpipe_w, &b, 1);     /* full pipe = event pending    */
    errno = e;
}
```

- `pipe2(O_NONBLOCK | O_CLOEXEC)`; `yew_tty_drain_signals` reads until
  `EAGAIN` and coalesces duplicates. The read end joins the poll set
  (architecture: process model).
- `'W'` → caller runs `yew_tty_winsize` and flags a full repaint. SIGCHLD
  (`'H'`) is plumbed now, consumed from Sprint 19 on.
- SIGTSTP (external `kill -TSTP`; ^Z is just a key — ISIG is off):

```c
static void yew_tty_tstp(int sig) {
    (void)sig;
    yew_tty_restore();
    signal(SIGTSTP, SIG_DFL);
    raise(SIGTSTP);
    /* ---- process stopped; SIGCONT resumes exactly here ---- */
    yew_tty_tstp_install();               /* re-arm ourselves            */
}
static void yew_tty_cont(int sig) {       /* also fires on bare kill -CONT */
    if (tcsetattr(g_wfd, TCSANOW, &g_rawios) == 0) g_raw = 1;
    yew_tty_signote(sig);                 /* 'C': loop redoes alt+modes+paint */
}
```

- The CONT handler re-enters raw immediately (`tcsetattr` is
  async-signal-safe; the shell handed the terminal back cooked); the `'C'`
  byte tells the main loop to re-enter the alt screen, re-push kitty
  flags/modes (Sprint 4's `yew_input_enable`), and force a full repaint.
  The dance is idempotent — a spurious `kill -CONT` while running is
  harmless.
- **Pitfall**: the raw termios must also live in static storage
  (`g_rawios`) — the CONT handler cannot recompute it.
- `yew_tty_suspend(t)` (the editor command path): restore, then
  `kill(0, SIGTSTP)` — the whole process group, shell-style.

### 6. Capability probing without terminfo (locked decision)

Why no terminfo: the DB describes what some distro thinks the terminal is;
we ask the live terminal directly, and the answer set is tiny. Locked in
`00-decisions.md` (bespoke terminal, invariant 7).

One write, three queries, in this order:

| Sent | Asks | Positive reply |
|---|---|---|
| `CSI ? u` | kitty keyboard protocol | `CSI ? <flags> u` |
| `CSI ? 2026 $ p` | DECRQM: synchronized output | `CSI ? 2026 ; Ps $ y` |
| `CSI c` | DA1 — the fence; every real terminal answers | `CSI ? … c` |

DECRPM `Ps`: 0 = not recognized, 1 = set, 2 = reset, 3 = permanently set,
4 = permanently reset. `sync_output = Ps ∈ {1,2,3}`.

State machine (`t->pstate`):

```
IDLE  --probe_start: write queries; pdeadline = now + timeout--> AWAIT
AWAIT --kitty reply-->  AWAIT   caps.kitty_kbd = true, kitty_flags = flags
AWAIT --DECRPM 2026-->  AWAIT   caps.sync_output per Ps
AWAIT --DA1 reply---->  DONE    da1_seen = true; unanswered = unsupported
AWAIT --tick: now >= pdeadline--> DONE   dumb terminal: everything off
```

- Timeout: default **50 ms**; `YEW_PROBE_TIMEOUT_MS` overrides (documented
  for >50 ms-RTT ssh links); `YEW_TTY_PROBE=0` skips probing entirely
  (all caps off except §7 truecolor).
- Budget: the probe **races** startup — first paint never waits on DONE.
  The editor (Sprint 14+) paints immediately with conservative caps and
  repaints once at DONE if caps changed (≤ 1 extra frame inside the
  window). Sprint 6's harness *does* wait for DONE before scripting keys,
  for determinism. Cold-start ≤ 20 ms gate is unaffected.
- Caps freeze at DONE. A reply arriving later (slow link under default
  timeout) reaches Sprint 4's parser, which recognizes-and-drops unknown
  CSI safely — never crashes, never becomes a Key.
- Recognizer contract: `probe_feed` consumes **only** the three exact
  reply shapes. Every other byte — the user typing at launch — is
  appended to `t->pending` in arrival order for Sprint 4. **Pitfall**:
  replies fragment across reads, and a partial `CSI ?` prefix may turn
  out to be user input (a real `ESC [` from a legacy key). Buffer the
  ambiguous prefix; on first mismatching byte, flush the whole prefix to
  `pending` in original order. Cap the prefix buffer at 64 bytes — an
  unterminated pseudo-reply flushes rather than growing.

### 7. Truecolor detection (no query exists)

First hit wins; implemented as
`bool yew_tty_detect_truecolor(const char *(*getv)(const char *))` — env
lookup injected so the unit table drives it:

1. `YEW_TRUECOLOR` = `0`/`1` → forced off/on.
2. `COLORTERM` = `truecolor` or `24bit` → on.
3. `TERM` contains `direct` or `truecolor` (terminfo `-direct` naming) → on.
4. Known-good: `TERM` prefix in {`xterm-kitty`, `kitty`, `foot`,
   `wezterm`, `alacritty`, `ghostty`}; or `TERM_PROGRAM` in {`iTerm.app`,
   `WezTerm`, `ghostty`, `vscode`}; or `VTE_VERSION` ≥ 3600; or
   `KONSOLE_VERSION` set → on.
5. Otherwise off — Sprint 5 downconverts to 256/16.

**Pitfall**: ssh does not forward `COLORTERM` by default; rule 4 rescues
most cases and `YEW_TRUECOLOR=1` is the documented escape hatch.

### 8. Defer

- Input decoding → Sprint 4. Bytes land in `t->pending` untouched;
  no mode-enables (2004/1002/1006/1004) and no kitty push are emitted this
  sprint — the restore blob's disables are still harmless.
- Cell grid, damage, render → Sprint 5. `yew_tty_*` never draws.
- Pty-harness verification of raw/alt/restore/suspend under a real child
  process → Sprint 6 (goldens there cover this sprint retroactively).
- At this sprint's landing, editing a file still hard-errored naming Sprint
  14. Sprint 14 later delivered that deferred surface; this line records the
  staging boundary rather than a current absence requirement (S59-A10).

## Testing Strategy

- **Unit** (`tests/unit/test_tty.c`): `yew_tty_rawios` asserts every row
  of the §2 table (each bit cleared/set individually, `VMIN`/`VTIME`);
  restore blob byte-compared against the exact §4 string (determinism
  pin); probe machine driven with a fake clock: full modern reply set,
  DA1-only, silence + deadline expiry, DECRPM `Ps` ∈ {0,1,2,3,4} mapping,
  kitty flags value captured.
- **Chunking independence**: every probe-reply stream re-fed split at
  every byte boundary — identical `TtyCaps` and identical `pending`
  contents.
- **Interleaving**: user bytes (`"ihello"`, a legacy `ESC [ A`, a lone
  `ESC`) mixed before/between/inside-gaps of replies — `pending` preserves
  them byte-exact, in order; the ambiguous-prefix flush case (`ESC [ Z`
  when a reply prefix was pending) covered explicitly.
- **Truecolor**: ≥ 12 env vectors through the injected-lookup table (each
  §7 rule positive + negative, override beats all).
- **Signal safety** (`scripts/check-sigsafe.sh`, run by `make test`):
  extract the `BEGIN/END ASYNC-SIGNAL-SAFE` region of `tty.c`; grep for
  `\b(printf|fprintf|snprintf|malloc|calloc|free|yew_log|exit|abort)\b`
  → must be empty. Second grep: `printf\|puts\|putchar` in `src/term/`
  outside test code → empty.
- Lanes: gcc + clang zero-warning, ASan/UBSan green. No pty tests yet.

## Definition of Done

1. `make && make test` green on gcc and clang, `-std=c11 -pedantic -Wall
   -Wextra -Werror`, ASan/UBSan lanes clean.
2. Every §2 table row has a dedicated unit assertion; ≥ 30 assertions in
   `test_tty.c` overall.
3. Restore blob byte-compared in a unit test; changing it breaks a test.
4. Probe machine: 4 scripted-terminal fixtures (modern, kitty-only,
   sync-only, dumb) produce the expected `TtyCaps`; the dumb fixture
   reaches DONE only via the fake-clock deadline, and `probe_deadline`
   returns a value ≤ 50 that a poll loop could use as its timeout.
5. Chunking-independence test passes for splits at every byte offset of
   every fixture stream.
6. `pending` preservation: interleaved user bytes recovered byte-exact in
   all fixtures, including the ambiguous-prefix flush case.
7. `scripts/check-sigsafe.sh` passes and is wired into `make test`.
8. `yew < /dev/null` (stdin not a tty) exits 1 and directs the user to the
   Sprint 37-delivered `--batch SCRIPT`; message text is pinned by a smoke
   check (S59-A10).
9. `YEW_TTY_PROBE=0` and `YEW_PROBE_TIMEOUT_MS` honored (unit-tested via
   injected env lookup on the probe-config reader).
10. `grep -rn 'tcsetattr' src/ | grep -v term/tty.c` → empty: tty.c is
    the only termios site.
11. Doc comment in `tty.h` states the restore guarantee and lists the
    four fatal signals + the atexit + prehook paths (the why carried in
    comments).
