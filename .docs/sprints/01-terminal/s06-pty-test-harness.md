# Sprint 6: PTY Test Harness — Fork Under a PTY, Snapshot, Golden

## Prerequisites

- Sprint 0 — `Bytebuf`, `Vec(T)`, `Arena`, `yew_log`, exit codes, the one
  Makefile.
- Sprint 1 — unit harness (explicit registry table, no ctor attributes),
  `make test`, `smoke.sh`, `bans.sh`, CI lanes. **`make test-pty` currently
  hard-errors naming this sprint** (s01 §4); this sprint replaces the stub.
- Sprint 3 — `Tty`: raw mode, alternate screen, the restore blob, the
  SIGWINCH/SIGCONT self-pipe, the probe and its three queries (`CSI ? u`,
  `CSI ? 2026 $ p`, `CSI c`) with `YEW_TTY_PROBE` / `YEW_PROBE_TIMEOUT_MS`.
  Sprint 3 §8 explicitly deferred pty-level verification of
  raw/alt/restore/suspend under a real child process to here.
- Sprint 4 — `In`/`Key`, the exact enable blob, the kitty push
  `CSI > 21 u`, bracketed paste, SGR mouse, focus events.
- Sprint 5 — `Grid`/`Cell`, `yew_render_frame`, the frame envelope, colour
  tiers, and the closed set of sequences the renderer emits (s05 §9 pins
  that no scroll-region / insert-line / delete-line sequence exists yet).
- Binding: `00-decisions.md` invariants 5 (*"the pty test harness asserts
  grids exactly"*) and 6, `01-architecture.md` §Testing architecture —
  *"The TUI is tested like a compiler tests codegen."*

## Goals

Land the harness every later UI sprint depends on: fork a yew binary
under a pseudo-terminal we own, feed it scripted key bytes, read what it
writes, interpret that stream with a **minimal bespoke VT** implementing
*only* the sequences our renderer emits, snapshot the resulting cell grid,
and byte-compare against a golden with a readable diff on failure. The VT's
incompleteness is the feature: the sequence set is closed by construction
(Sprints 3–5 emit a finite list), so anything outside it is a renderer bug
that fails a test instead of quietly entering the goldens. Also
`tests/pty/demo_paint.c`, painting a static screen through the full `Tty` →
`Grid` → `Render` stack — the **MILESTONE: first paint** proof. The real
editor UI lands in Sprint 14 and after; at this sprint's landing `yew <file>`
still hard-errored, and Sprint 14 subsequently delivered it (S59-A10).
`libutil`/`forkpty` is not used and never will be (invariant 7) — the
replacement is pinned in full below.

## Deliverables

### 1. Hand-rolled forkpty — `tests/pty/harness.c`

Written out entire: it is the harness's foundation, every line is
load-bearing, and `forkpty(3)` is a libutil function we may not link.

```c
/* MUST be the first lines of the file: posix_openpt/grantpt/unlockpt/
 * ptsname need XSI, and -std=c11 defines no feature-test macro for us.  */
#define _XOPEN_SOURCE 700
#include <fcntl.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <unistd.h>

typedef struct PtySpec {
    const char  *path;       /* binary to exec                           */
    char *const *argv;       /* argv[0] = path                           */
    char *const *envp;       /* PINNED, never `environ` — §2             */
    u16 rows, cols;
    i64 budget_ms;           /* per-case wall clock, default 5000        */
} PtySpec;

typedef struct Pty {
    int master; pid_t pid; u16 rows, cols;
    bool reaped; int status; i64 started_ms;
} Pty;

bool yew_pty_spawn(Pty *p, const PtySpec *sp) {
    char sname[128];
    int  m = posix_openpt(O_RDWR | O_NOCTTY);
    if (m < 0) return false;
    /* CLOEXEC before the fork, so no window exists in which any exec in
     * this process could inherit the master and hold the pty open.      */
    if (fcntl(m, F_SETFD, FD_CLOEXEC) < 0) goto fail;
    if (grantpt(m) < 0 || unlockpt(m) < 0) goto fail;
    {   /* ptsname() returns STATIC storage and ptsname_r is a GNU
         * extension invisible under -std=c11 -pedantic. Copy the name
         * before any other libc call can clobber it; the harness is
         * single-threaded (invariant 8), so this is safe and auditable. */
        const char *s = ptsname(m);
        if (!s) goto fail;
        size_t n = strlen(s);
        if (n + 1 > sizeof sname) goto fail;
        memcpy(sname, s, n + 1);
    }
    pid_t pid = fork();
    if (pid < 0) goto fail;
    if (pid == 0) {
        /* ---- child ---- every failure is _exit(127), never exit():
         * exit() would run the PARENT's atexit handlers (yew_tty_restore
         * among them) and flush the parent's stdio into the pty,
         * poisoning the golden. _exit is also the only async-safe choice */
        if (setsid() < 0) _exit(127);        /* MUST precede the open()  */
        int s = open(sname, O_RDWR);         /* no O_NOCTTY: take ctty   */
        if (s < 0) _exit(127);
#ifdef TIOCSCTTY
        (void)ioctl(s, TIOCSCTTY, 0);        /* belt-and-suspenders      */
#endif
        struct winsize ws;
        memset(&ws, 0, sizeof ws);
        ws.ws_row = sp->rows; ws.ws_col = sp->cols;
        (void)ioctl(s, TIOCSWINSZ, &ws);
        if (dup2(s, 0) < 0 || dup2(s, 1) < 0 || dup2(s, 2) < 0) _exit(127);
        if (s > 2) (void)close(s);
        (void)close(m);                      /* child must NOT hold it   */
        execve(sp->path, sp->argv, sp->envp);
        _exit(127);
    }
    p->master = m; p->pid = pid; p->rows = sp->rows; p->cols = sp->cols;
    p->reaped = false; p->status = -1;
    (void)fcntl(m, F_SETFL, O_NONBLOCK);     /* the parent never blocks  */
    return true;
fail:
    (void)close(m);
    return false;
}
```

Pitfalls, each a real way this fails:

- **`setsid()` before `open(slave)`.** Reversed, the slave never becomes
  the controlling terminal: `isatty()` still passes so Sprint 3 starts
  happily, but `TIOCSWINSZ` delivers no `SIGWINCH` and the resize cases
  pass by doing nothing.
- **`close(m)` in the child.** If the child inherits the master, the pty
  never reaches EOF after the child exits and the parent burns its full
  budget on *every* case — a green suite that takes an hour.
- **`_exit`, never `exit`, after `fork`** (see the comment above).
- **`read()` on the master after the child exits returns `EIO` on Linux,
  not 0.** Treat `errno == EIO` as EOF; treating it as an error is the
  single most common source of spurious failures in a hand-rolled harness.
- **`char *const argv[]`**: build the arrays with a
  `pty_strv_build(Arena *, const char *const *)` helper rather than casting
  away `const` at the `execve` call — one documented site under `-Werror`.

### 2. Environment pinning — determinism starts at the fork

The child's environment is **constructed**, never inherited: a developer's
`COLORTERM`, `TERM` or locale must not be able to change a golden.

| Var | Value | Why |
|---|---|---|
| `TERM` | `xterm-256color` | the fixture's terminal identity |
| `COLORTERM` | unset by default | keeps s03 §7 rule 2 out of the way |
| `YEW_COLORS` | `truecolor`\|`256`\|`16` | pins the s05 tier per case |
| `YEW_TTY_PROBE` | `1` | the harness answers probes (§3) |
| `YEW_PROBE_TIMEOUT_MS` | `500` | CI machines are slow; the harness always replies |
| `YEW_ESC_TIMEOUT_MS` | `25` | makes the s04 lone-ESC path reproducible |
| `XDG_STATE_HOME` | a per-run temp dir | the log must not land in `$HOME` |
| `LANG` / `LC_ALL` | `C.UTF-8` | our code is locale-free; a child's libc is not |
| `YEW_LOG_LEVEL` | `debug` | the log is the post-mortem artifact on failure |

The nine-key table was the runner's baseline; later owning sprints extend this set
only when their deterministic fixtures need a new input. Each addition updates
the constructed allowlist's enumerated key table and
`test_pty_environment_exact` in the same change. The current exact allowlist
remains constructed rather than inherited; a stray host variable is a
test-integrity bug, not a convenience (S59-A10).

### 3. The fake terminal: answering probes

The harness *is* the terminal, so it must answer Sprint 3's probe or every
case pays the timeout and lands on the dumb capability set. Per-case
profiles select the replies (the recognizer shares the §4 parser — there is
not a second one):

| Profile | `CSI ? u` | `CSI ? 2026 $ p` | `CSI c` | Exercises |
|---|---|---|---|---|
| `modern` | `CSI ? 0 u` | `CSI ? 2026 ; 2 $ y` | `CSI ? 62;22 c` | kitty + sync path |
| `nokitty` | *(silence)* | `CSI ? 2026 ; 2 $ y` | `CSI ? 62;22 c` | s04 legacy decoding |
| `nosync` | `CSI ? 0 u` | `CSI ? 2026 ; 0 $ y` | `CSI ? 62;22 c` | s05 unsynchronized frames |
| `dumb` | *(silence)* | *(silence)* | *(silence)* | s03 deadline expiry, s05 16-colour tier |

Cases wait for the probe to reach DONE before scripting keys — Sprint 3 §6
promised the harness would, and it is what makes key timing irrelevant to a
golden.

### 4. The minimal VT interpreter — `tests/pty/vt.c|h`

**The closed set.** The table below is the Sprints 3–5 baseline. The
interpreter rejects anything outside its compiled whitelist, and later owning
sprints extend this set only by updating the VT plus rejection, unit,
round-trip, and PTY coverage in the same change. The harness remains a
conformance check on every current emitter; it never accepts a new sequence
merely to make a golden pass (S59-A10).

| Sequence | Emitter | VT action |
|---|---|---|
| `ESC 7` / `ESC 8` | s03 §3 | save / restore cursor (primary screen) |
| `CSI ? 1049 h` / `l` | s03 §3 | enter / leave alt screen (the grid *is* the alt screen) |
| `CSI ? 25 h` / `l` | s05 §6 | cursor-visible flag |
| `CSI ? u`, `CSI ? 2026 $ p`, `CSI c` | s03 §6 | probe queries → reply per profile |
| `CSI ? 2004 h/l`, `? 1002 h/l`, `? 1006 h/l`, `? 1004 h/l` | s04 §8, s03 blob | mode flags |
| `CSI > <n> u` / `CSI < u` | s04 §7 / s03 blob | push / pop an 8-deep kitty flag stack |
| `CSI ? 2026 h` / `l` | s05 §6 | BSU / ESU; **nesting is an error** |
| `CSI <r>;<c> H`, `CSI H` | s05 §5 | CUP, 1-based → 0-based, clamped |
| `CSI <n> C` | s05 §5 | CUF, clamped at `cols-1` |
| `CSI K` | s05 §5 | EL 0: erase to EOL with the **default** bg |
| `CSI <params> m` | s05 §2/§7 | SGR — sub-table below |
| UTF-8 text | s05 §5 | place a cluster at the cursor, advance by its width |

SGR parameters accepted — anything else is an error: `0` (or empty) reset;
`1 2 3 4 4:3 5 7 8 9 53` the ten attributes of s05 §2; `22 23 24 25 27 28
29 55` their resets (`22` clears bold **and** dim); `30`–`37` / `90`–`97`
fg indexed 0–15; `40`–`47` / `100`–`107` bg indexed 0–15; `39` / `49`
default; `38;5;N` / `48;5;N` indexed; `38;2;R;G;B` / `48;2;R;G;B` rgb.

```c
typedef struct VtCell { u8 g[8]; u8 nb; YewColor fg, bg; u16 attrs; u8 w; } VtCell;

typedef struct VtScreen {
    int      rows, cols, cur_r, cur_c;
    bool     alt, cur_vis, in_sync, primary_written;
    u32      modes;              /* bitset: 2004 1002 1006 1004          */
    u32      kitty[8]; int ksp;
    VtCell  *cells;              /* rows*cols; the alt screen only       */
    Bytebuf  errors; u32 nerrors, nsync_pairs;
    /* parser state, mirroring src/term/input.c but write-side           */
} VtScreen;

void vt_init(VtScreen *v, int rows, int cols);
void vt_feed(VtScreen *v, const u8 *b, size_t n);   /* chunk-safe        */
void vt_resize(VtScreen *v, int rows, int cols);
```

- The primary screen is not modeled beyond enter/leave plus
  `primary_written` — writing there is itself an error, which makes
  invariant 6's "never stain the primary screen" mechanical.
- **Autowrap is not implemented, deliberately.** s05 §5 forbids relying on
  it, so a text write that would pass `cols` records an error instead of
  wrapping: a renderer that starts depending on wrap fails immediately
  rather than producing plausible-looking goldens.
- Wide glyphs mirror s05 §4: head `w = 2`, tail `w = 0` carrying the head's
  style. A write landing on a tail records an orphan error.
- `vt_feed` is chunk-safe — the master is read in whatever sizes the kernel
  hands us.
- **Pitfall — the VT must not become a real terminal.** When a case fails
  with "unknown sequence", the tempting fix is to implement the sequence.
  The correct fix is almost always in `render.c`; the set changes only when
  a sprint file says it does (Sprint 15 adds the scroll region, and says so).

### 5. Snapshot format and diff renderer — `tests/pty/snapshot.c|h`

Goldens are text so `git diff` is useful and review is possible.
`tests/pty/goldens/<case>.golden`:

```
# yew pty golden v1
size 80x24 alt=1 cursor=5,12 vis=1
modes 2004,1002,1006,1004 kitty=21 sync_pairs=3
--- text
┌──────────────────────────────┐
│ yew                      │
└──────────────────────────────┘
--- style
AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA
ABBBBBBBCCCCCCCCCCCCCCCCCCCCCCCA
AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA
--- legend
A fg=default bg=default attrs=----------
B fg=#c0caf5 bg=#1a1b26 attrs=b---------
C fg=idx:7   bg=default attrs=----------
```

- The `text` block prints each row's clusters; a continuation prints
  nothing, so a CJK glyph occupies two display columns exactly as on screen
  and the block reads naturally. An **orphan** continuation prints `¿` —
  orphans are bugs and must be loud.
- The `style` block assigns one letter per distinct `{fg,bg,attrs}` triple
  in **row-major first-appearance order** (`A`…`Z`, `a`…`z`, then `#0`…).
  First-appearance order is deterministic and stays stable under content
  changes in a way a hash or a value-sort does not.
- `attrs` is a fixed ten-character flag string in s05 §2 bit order, so a
  new attribute shows as a one-character diff.
- Trailing default-styled blanks are trimmed from both blocks and
  reconstructed on read; otherwise every golden is 80 columns of whitespace
  and every review is unreadable.

```c
void snapshot_write(const VtScreen *v, Bytebuf *out);
/* Byte-compare; on mismatch render a human diff into `msg` and return
 * false: first differing line, both versions, a caret row under the first
 * differing column, and the legend entries that changed.
 * YEW_PTY_DIFF=full dumps both snapshots entire.                       */
bool snapshot_compare(const Bytebuf *got, const Bytebuf *want, Bytebuf *msg);
```

Comparison is a byte compare of the *rendered snapshot*, not a
field-by-field structural compare. Why: one comparison path means the
golden file **is** the assertion, and no field can be asserted in the
comparator yet missing from the file, or vice versa.

### 6. Case registry, runner, `make test-pty`

Cases are C functions in a table, mirroring Sprint 1's discipline — a case
exists iff it is in the table, no ctor attributes, no linker sections:

```c
typedef struct PtyCase {
    const char *name;
    const char *profile;    /* modern | nokitty | nosync | dumb          */
    u16 rows, cols;
    void (*fn)(PtyCtx *c);
} PtyCase;
extern const PtyCase yew_pty_cases[];      /* tests/pty/registry.c       */

void ptc_spawn(PtyCtx *, const char *bin, ...);  /* NULL-terminated argv */
void ptc_settle(PtyCtx *, i64 quiet_ms);   /* pump until quiet, bounded  */
void ptc_keys(PtyCtx *, const char *spec); /* "ctrl+q", "esc", "a", "F5" */
void ptc_bytes(PtyCtx *, const char *lit); /* raw: paste, mouse reports  */
void ptc_resize(PtyCtx *, u16 rows, u16 cols);   /* TIOCSWINSZ + SIGWINCH */
void ptc_snapshot(PtyCtx *, const char *golden_name);
void ptc_expect_exit(PtyCtx *, int code);
```

- `ptc_keys` parses the same key spelling the Sprint 13 keymap will use and
  emits the bytes the *profile's* terminal would send — kitty `CSI u` under
  `modern`, legacy under `nokitty`. One spelling, two encodings: that is
  how one case proves both s04 paths.
- `ptc_settle` pumps until `quiet_ms` (default 50) pass with no output,
  bounded by the case budget. Never a fixed `sleep` — a fixed sleep is both
  slower and flakier than a quiet-period detector.
- `make test-pty` builds `build/pty_runner` and the demo binaries and runs
  the registry; `make test` gains it as a dependency, CI gains a `pty` lane
  and keeps the suite in the determinism lane (run twice, outputs diffed).

### 7. Timeout and cleanup discipline

| Hazard | Rule |
|---|---|
| Hung child | Every pump is bounded by the case's `budget_ms` (default 5 s) from `CLOCK_MONOTONIC` at spawn; the runner has a global `YEW_PTY_BUDGET_MS` (default 60 s) after which it kills everything and exits 1 |
| `SIGCHLD` races | **No** `SIGCHLD` handler is installed and blocking `wait()` is never called. Exit is detected by EOF/`EIO` on the master, then `waitpid(pid,&st,WNOHANG)` polled to the deadline, then `SIGKILL` + a 1 s `waitpid` |
| Double reap | `p->reaped` guards `waitpid`; a second call returns `ECHILD` and reads a garbage status |
| Orphaned fds | The master is `FD_CLOEXEC` from birth, closed on every error path; the runner keeps a `Vec(Pty*)` of live children, swept after every case and at exit |
| Zombies | After each case: kill if unreaped, `waitpid` to completion, assert the live list is empty |
| `SIGPIPE` | Set `SIG_IGN` once — writing keys to an already-exited child must return `EPIPE`, not kill the runner |
| Signal deaths | `ptc_expect_exit` separates `WIFEXITED`/`WEXITSTATUS` from `WIFSIGNALED`: a child killed by `SIGSEGV` reports "died on signal 11", never "exit 139" |

- Kill with `SIGKILL`, not `SIGTERM`: Sprint 3 installs a `SIGTERM` handler
  that restores and re-raises, so a `TERM` to a child wedged *inside* that
  handler achieves nothing. The harness's job is to end the process.
- On failure the runner preserves the case's temp `XDG_STATE_HOME` (log
  included) and prints its path; on success it removes it. A failing pty
  case with no log is nearly undebuggable.

### 8. Golden-update workflow

`YEW_PTY_UPDATE=1 make test-pty` rewrites every mismatching golden, prints
`golden updated: <name>` per file, and **exits 1 regardless**. Why exit 1:
no CI configuration, no `make -k`, no wrapper can turn a golden update into
a passing run. Updating is a deliberate local act followed by reading the
diff and committing it.

- Updates are never implicit. A missing golden is a **failure** ("golden
  not found: run with YEW_PTY_UPDATE=1"), not an invitation to create one —
  otherwise a typo'd case name silently self-approves.
- `bans.sh` fails if `YEW_PTY_UPDATE` appears in any CI file.
- Review rule, stated in `tests/pty/README` (the one doc this sprint adds):
  a commit changing a golden must say why. A golden diff is a rendering
  behaviour change by definition.

### 9. `tests/pty/demo_paint.c` — the first-paint milestone

A standalone binary (`build/demo_paint`) linking every `src` object except
`src/main.o`, driving the real stack end to end: `yew_tty_open` →
`yew_tty_raw` → probe to DONE → `yew_tty_altscreen(true)` →
`yew_input_enable` → `yew_grid_init` → paint → `yew_render_frame` → one
`write` → `yew_grid_flip` → read keys until `q` → exit 0 (`yew_tty_close`
runs the restore blob).

| Scene (`--scene`) | Content | Proves |
|---|---|---|
| `basic` | box border, `yew` centred, a styled bottom strip | first paint; CUP/SGR/EL basics |
| `wide` | CJK, emoji, emoji+VS16, a ZWJ family, a combining stack, and a wide glyph placed at `cols-1` | s05 §4's three rules, s02 widths, interner spill |
| `colors` | a 64-step RGB gradient plus indexed and default cells | s05 §7 at each tier (three cases, `YEW_COLORS` pinned) |
| `damage` | paints, then changes one cell per keypress | minimal output: the second frame is asserted ≤ 32 bytes |
| `resize` | draws a frame sized to the live `rows`/`cols` | SIGWINCH → `yew_tty_winsize` → `yew_grid_resize` → full repaint |
| `echo` | renders the decoded `Key` (code, mods, text) into the grid | the s04 kitty-vs-legacy equivalence case |

`demo_paint` is a fixture, not a product surface: it lives under `tests/`,
is built only by `make test-pty`, and is never installed.

### 10. Defer

- The real editor UI — buffers, viewport, statusline, modes — was deferred to
  Sprint 14. The original driver-defer PTY pinned that staging guard; after
  Sprint 14 landed, current PTYs exercise the delivered editor instead
  (S59-A10).
- Pane/tab/group goldens → Sprints 22–24; they land in `tests/pty/goldens/`
  beside these, using this registry unchanged.
- Mouse and paste *behaviour* goldens → Sprints 27 and 14; `ptc_bytes` can
  already feed the bytes and this sprint proves they reach the child, but
  nothing consumes them yet.
- Scroll-region sequences (`CSI r`, `IL`, `DL`) → Sprint 15, which adds
  them to the renderer **and** to the §4 table in the same commit. Until
  then the VT rejects them.
- Script tests driven by Fletch → Sprint 37, unchanged.

## Testing Strategy

- **Unit** (`tests/unit/test_vt.c`): the VT driven directly, no pty. Feed
  each §4 sequence and assert the resulting `VtScreen`; feed each SGR row;
  feed every sequence split at every byte boundary and assert identical
  screens (chunk safety); feed a sequence outside the set and assert
  `nerrors == 1` with the bytes recorded; feed a text write past `cols` and
  assert the no-autowrap error; feed nested `CSI ?2026h`; feed a write onto
  a wide tail and assert the orphan error.
- **Unit** (`tests/unit/test_snapshot.c`): snapshot round-trip (write,
  parse, write, byte-identical); legend assignment for a screen with 60
  distinct styles; trailing-blank trim and reconstruction; pinned diff
  output for a one-cell, a one-row and a legend-only difference.
- **Fuzz** (`tests/fuzz/fuzz_vt.c`, joins `make fuzz`): random bytes into
  `vt_feed` — never crash, never allocate unboundedly, `nerrors` only
  increases. Sprint 5's render round-trip property (render a fuzzed grid,
  feed the stream to the VT, assert the reconstruction equals `back`) moves
  out of `tests/unit/` into its permanent home here.
- **PTY cases** (`tests/pty/registry.c`), minimum set:
  1. `probe_modern` — asserts the probe queries arrived in s03's order, the
     kitty push is `CSI > 21 u`, and the enable blob matches s04 §8 byte
     for byte.
  2. `probe_dumb` — no kitty push, no 2026 pair, 16-colour tier, paint
     still happens.
  3. `paint_basic`, `paint_wide`, `paint_colors_{truecolor,256,16}`,
     `paint_damage`, `paint_resize` — §9's scenes.
  4. `restore_quit` — after `q`, the trailing bytes are exactly s03's
     restore blob and the child exits 0.
  5. `restore_crash` — `demo_paint --crash` raises `SIGSEGV`; the blob was
     written *before* death and the child is `WIFSIGNALED` with `SIGSEGV`.
     This is invariant 6 under a real terminal — the coverage Sprint 3
     deferred here.
  6. `restore_suspend` — external `SIGTSTP`: assert the blob, then
     `SIGCONT`, assert raw mode re-entered and a full repaint follows.
  7. `input_keys_{modern,legacy}` — the `echo` scene under both profiles
     with the same `ptc_keys` script and the **same** golden. This is why
     `ptc_keys` takes a spelling rather than bytes.
  8. Historical `driver_defer` evidence pinned the Sprint 14 staging guard;
     current successor cases open files through the real editor (S59-A10).
- **Determinism** (invariant 5): the CI determinism lane runs
  `make test-pty` twice and diffs the runner's stdout; separately, every
  case snapshots twice from the same child and byte-compares before the
  golden comparison, so a nondeterministic frame fails as "unstable
  snapshot" rather than as a golden mismatch.
- **fd hygiene**: after the last case the runner asserts `/proc/self/fd`
  (when readable) holds no pty devices, and the valgrind lane runs the pty
  suite with `--track-fds=yes`.

## Definition of Done

1. `make test-pty` builds and runs green on gcc and clang, zero warnings
   under the locked flags; wired into `make test` and a CI `pty` lane.
   Sprint 1's hard-error stub is gone.
2. No libutil: `grep -rn 'forkpty\|openpty\|-lutil' tests/ Makefile` →
   empty; §1 is the only pty-creation site.
3. **MILESTONE first paint**: `build/demo_paint --scene basic` under the
   harness matches `tests/pty/goldens/paint_basic.golden` byte for byte on
   two consecutive runs.
4. ≥ 12 registered pty cases covering the Testing Strategy list; every case
   has a golden and no golden is orphaned (`bans.sh` cross-checks
   `goldens/*.golden` against the registry, as it does unit registration).
5. The closed set is enforced: a deliberately seeded `CSI 5 L` in
   `render.c` makes ≥ 1 case fail with `unknown sequence: ESC [ 5 L`;
   removing it restores green. The drill is written down in
   `tests/pty/README`.
6. `YEW_PTY_UPDATE=1 make test-pty` rewrites goldens, prints one line per
   file, and **exits 1**; without it a missing golden is a failure, never
   an auto-create; `bans.sh` rejects the variable in CI files.
7. Timeout discipline: a case whose child runs `sleep 300` is killed at its
   budget and reported as a timeout within budget + 1 s, the runner exits
   1, and no process survives (`pgrep -f demo_paint` empty afterwards).
8. `restore_crash` proves the restore guarantee end to end: the blob is in
   the child's output tail and the child is `WIFSIGNALED` with `SIGSEGV`.
9. `input_keys_modern` and `input_keys_legacy` produce the same golden from
   the same key script.
10. Every case snapshots twice and byte-compares before the golden
    comparison; a seeded nondeterminism (an uninitialized cell) is reported
    as "unstable snapshot".
11. valgrind `--track-fds=yes` reports no leaked descriptors across a full
    pty run; the runner's live-child list is empty at exit.
12. The child environment is constructed, not inherited: a unit test asserts
    the current enumerated allowlist consists of §2's original baseline plus
    explicit later owning-sprint additions, and `COLORTERM=truecolor make
    test-pty` changes no golden (S59-A10).
