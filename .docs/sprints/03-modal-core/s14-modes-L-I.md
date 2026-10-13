# Sprint 14: Modes L and I — The Notepad Milestone

## Prerequisites

- Sprint 3 — `Tty`: raw mode, alt screen, restore guarantee,
  `yew_tty_signal_fd`/`yew_tty_drain_signals` (WINCH/CONT/CHLD self-pipe),
  `yew_tty_winsize`, capability probe with its poll deadline
  (`yew_tty_probe_deadline`), `yew_tty_suspend`.
- Sprint 4 — normalized `Key` events, bracketed paste, kitty protocol.
  **Assumption beyond its index entry**: `input.c` exposes a nonblocking
  drain `bool yew_input_feed(Input *, const u8 *, size_t)` and a queue pop
  `bool yew_input_pop(Input *, Key *)`, plus paste-start/paste-end key
  kinds. If Sprint 4 shaped it differently, the adapter lands here.
- Sprint 5 — `Grid`/`Cell` double buffer, damage diff → escape stream,
  wide-glyph continuation cells, synchronized output.
- Sprint 6 — pty harness (feed keys, snapshot grid, byte-compare goldens).
- Sprint 7/8/9/10 — `TextBuf`; `yew_file_load`/`yew_file_save` + the
  journal (`yew_journal_replay`) and the kill -9 torture harness;
  `Cursor`/`CursorSet`, typed coords, grapheme motion; the undo tree with
  transactions and the clean-state node.
- Sprint 13 — command registry (`yew_cmd_invoke`), key trie, layer stack,
  chord machine, `Mode`/`ModeDesc`, `BindRow` tables.
- Binding: `00-decisions.md` (invariants 1, 3, 4, 5, 6, 9),
  `01-architecture.md` §Process model — the loop below is its prose made
  executable.

## Goals

**The vertical slice**: `yew file.txt` opens, moves, inserts, saves, and
quits — a real editor over the whole stack. This sprint lands `Ed` /
`Workspace` / `Win`, the `poll(2)` event loop for real (with typeahead
coalescing and the timer heap), `mode.c` with the Esc law, the L-mode
command-key table, I mode over `TextBuf` with grapheme arrows, argv file
open with journal-recovery prompt, save via the Sprint 8 atomic path, and
quit with a dirty-check prompt. The viewport here is a **naive
follow-cursor** clamp (no scrolloff, no wrap, no horizontal scroll) and the
footer is a **one-row raw** status/message strip — Sprint 15 replaces both
with the real engines and specs. At this sprint's landing, W/B/H/E/F entries
hard-errored naming Sprints 16/16/17/18/52. Later owning sprints delivered
W/B/H/E/F through the same mode-entry and command-dispatch surfaces
(S59-A13). Panes/tabs are Sprint 22/23: `Ed` holds exactly one `Win`, full
screen.

## Deliverables

### 1. Editor state — `src/edit/ed.h`, `src/ui/win.h`

```c
typedef struct Win {                     /* a view; buffers are shared   */
    Buffer   *buf;
    CursorSet cs;            /* s09; multi-cursor lands in Sprint 17     */
    Viewport  vp;            /* §5 here; Sprint 15 owns the real engine  */
    Rect      rect;          /* full screen minus footer this sprint     */
} Win;

typedef struct Buffer {
    TextBuf  *tb;
    FileMeta  meta;          /* s08: eol, bom, binary, mode/uid/mtime    */
    char     *path;          /* arena-owned realpath; NULL = [no name]   */
    UndoTree *undo;          /* s10                                      */
    Journal  *jrn;           /* s08; opened on first modification        */
    MarkSet   marks;
} Buffer;

typedef struct Workspace { Buffer *bufs; u32 nbufs; char *dir; } Workspace;

typedef struct Ed {
    Tty        tty;   Input in;   Grid grid;
    Workspace  ws;    Win  *win;          /* exactly one until Sprint 22 */
    Mode       mode;  Mode  prev_unit;    /* H's memory (Sprint 17)      */
    KeyStack   keys;  Chord chord;
    TimerHeap  timers;
    Msg        msg;                       /* §8 one-row footer state     */
    bool       quit;  int exit_code;
    bool       layout_dirty, full_damage;
} Ed;
```

- `bool yew_buf_dirty(const Buffer *b)` asks the undo tree whether the
  current node is the clean node (Sprint 10). **No cached dirty flag** —
  the same law Sprint 23 states for tabs: a flag can lie after undo.
- Ownership: `Ed` is a single stack object in `main()`. `Ed *ed` threads
  through every `CmdCtx`; there is no global editor pointer (the Sprint 3
  static sliver in `tty.c` is the only global state in the program).

### 2. The event loop — `src/edit/loop.c`, `src/edit/loop.h`

```c
int yew_loop_run(Ed *ed);          /* returns the process exit code     */
```

```c
for (;;) {
    i64 now = yew_now_ms();                       /* CLOCK_MONOTONIC    */
    int tmo = yew_loop_deadline(ed, now);         /* -1 = block forever */
    struct pollfd fds[] = {
        { ed->tty.rfd,               POLLIN, 0 },
        { yew_tty_signal_fd(&ed->tty), POLLIN, 0 },
        /* job/LSP/AI fds append here from Sprint 19 on                 */
    };
    if (poll(fds, 2, tmo) < 0 && errno != EINTR) return yew_bug_io();

    /* 1. DRAIN ALL READY INPUT BEFORE RENDERING.  One render per burst.*/
    size_t drained = 0;
    if (fds[0].revents & POLLIN) {
        u8 b[4096]; ssize_t n;
        while ((n = read(ed->tty.rfd, b, sizeof b)) > 0) {
            yew_input_feed(&ed->in, b, (size_t)n);
            if ((drained += (size_t)n) >= YEW_INPUT_BURST_MAX) break;
        }   /* n<0 && EAGAIN = drained; n==0 = terminal died → quit      */
    }
    /* 2. Signals, coalesced (a window drag delivers dozens of WINCH).  */
    bool winch = false, cont = false, chld = false;
    yew_tty_drain_signals(&ed->tty, &winch, &cont, &chld);
    if (winch || cont) yew_ed_resize(ed, cont);   /* Sprint 15 §7       */

    /* 3. Dispatch every queued key; commands mutate the model only.    */
    Key k;
    while (yew_input_pop(&ed->in, &k)) yew_dispatch_key(ed, k, now);

    /* 4. Timers fire only once the queue is empty — a chord deadline
     *    must never expire in the middle of a typeahead burst.         */
    if (yew_input_empty(&ed->in)) {
        yew_dispatch_tick(ed, now);
        yew_timers_fire(&ed->timers, ed, now);
    }
    if (ed->quit) return ed->exit_code;

    /* 5. Pump async jobs (no-op until Sprint 19), then lay out, then
     *    render exactly once.  Damage tracking keeps it O(changed).    */
    if (ed->layout_dirty) yew_layout(ed);
    yew_render(ed);
}
```

- `YEW_INPUT_BURST_MAX = 256 KiB`. **Pitfall:** an uncapped drain lets
  `cat 100mb | yew` starve signals and the renderer indefinitely — the
  user sees a frozen editor. Hitting the cap renders and loops, so a huge
  paste shows progress; below the cap, a burst still costs exactly one
  frame.
- **Pitfall — rendering per keystroke.** Key-repeat at 30/s and a paste of
  a 40 KB block both arrive as one poll wakeup. Rendering per key turns a
  paste into thousands of frames and blows the 5 ms p99 gate by three
  orders of magnitude. The drain-then-render order is the whole point of
  the architecture's event-loop rule; the pty harness counts frames to
  keep it true (Testing).
- Timers: a small binary min-heap in `loop.c`
  (`yew_timer_add(&ed->timers, at_ms, TimerFn, ctx) → TimerId`,
  `yew_timer_cancel`). Consumers this sprint: message expiry (§8) and the
  Sprint 13 chord deadline (which is *not* a heap timer — it is folded
  into `yew_loop_deadline` via `yew_dispatch_deadline`, so cancelling a
  chord cannot leave a stale heap entry).
- `yew_loop_deadline` = min(chord deadline, next timer, probe deadline
  while `!yew_tty_probe_done`), converted to a poll timeout; `-1` when
  nothing is pending. **Pitfall:** never pass `0` when nothing is pending —
  a busy loop pins a core and the battery budget with it.
- Startup order: `yew_tty_open` → `yew_tty_raw` → alt screen →
  `yew_tty_probe_start` → load file → **first paint** (conservative caps)
  → loop. First paint never waits on the probe (Sprint 3 §6); a caps
  change at probe DONE sets `full_damage` and costs one extra frame.

### 3. Modes — `src/edit/mode.c`

```c
CmdStatus yew_mode_enter(Ed *ed, Mode m);        /* the only transition */
```

- Entering a unit mode (L/W/B) records `ed->prev_unit = m` (Sprint 17's H
  memory reads it).
- Leaving I closes the open insert transaction (§6) and clamps every
  cursor (`yew_cursor_clamp`, s09).
- **The Esc law:** `ed.mode.escape` returns to L from *every* mode and
  collapses pending state in this order — pending chord, pending count,
  active prompt layer, selection (Sprint 17), then mode. Esc in L with
  nothing pending is a no-op that still forces a statusline repaint (the
  user pressed Esc to confirm where they are; showing them is free).
- Mode changes set `full_damage` for the footer only (the mode chip
  recolors the whole footer, Sprint 15) — never the document.
- `ed.mode.enter` with `"W"|"B"|"H"|"E"|"F"` returned
  `YEW_CMD_ERR_DEFERRED` naming Sprints 16/16/17/18/52 at this sprint's
  landing. Those owning sprints subsequently replaced the staging guards
  with live entries through this one transition path (S59-A13).

### 4. The L-mode table (pinned) — `src/edit/keys_default.c`

Arrows first: **arrows always move; the mode chooses the unit.** In L the
unit is the line.

| Key | Command | Effect |
|---|---|---|
| `<left>` | `ed.move.line.home` | start of line (col 0) |
| `<right>` | `ed.move.line.end` | end of line, before EOL bytes |
| `<up>` | `ed.move.line.up` | line above, `goal_col` sticky (s09) |
| `<down>` | `ed.move.line.down` | line below |
| `A-<left>` | `ed.move.line.first_nonblank` | smart home — L's alt variant |
| `A-<right>` | `ed.move.line.last_nonblank` | smart end |
| `A-<up>` | `ed.move.line.half_page_up` | vertical alt = half page |
| `A-<down>` | `ed.move.line.half_page_down` | |
| `<home>` `<end>` | aliases of `line.home` / `line.end` | |
| `<pgup>` `<pgdn>` | `ed.view.page_up` / `page_down` | cursor follows |

Command keys:

| Key | Command | Notes |
|---|---|---|
| `i` | `ed.mode.enter I` | insert at cursor |
| `a` | `ed.edit.insert.after` | grapheme-right then enter I; at EOL stays |
| `o` / `O` | `ed.edit.line.open_below` / `open_above` | insert EOL bytes + enter I |
| `x` | `ed.edit.delete.grapheme` | REPEATABLE (`4x`) |
| `d d` | `ed.edit.line.delete` | chord; REPEATABLE by count |
| `u` | `ed.edit.undo` | s10 |
| `C-r` | `ed.edit.redo` | |
| `g g` | `ed.move.buf.home` | chord |
| `G` | `ed.move.buf.end` | TAKES_COUNT: `12G` → line 12 |
| `s` | `ed.file.save` | §7 |
| `q` | `ed.quit` | prefix **and** complete (s13 §6) |
| `q !` | `ed.quit_force` | discards without prompting |
| `C-z` | `ed.suspend` | `yew_tty_suspend` |
| `C-l` | `ed.redraw` | full damage; the panic button |
| `1`–`9` | count prefix | `0` continues a count, else `ed.move.line.home` |
| `w` `b` `h` `e` `f` | `ed.mode.enter W/B/H/E/F` | staging-deferred here; delivered by 16/16/17/18/52 (S59-A13) |
| `<esc>` | `ed.mode.escape` | |

`t`-prefixed tab keys (Sprint 23), `p`-prefixed pane keys (Sprint 22) and
`/` search (Sprint 21) are **reserved and unbound** — pressing them yields
the standard `unbound: t` message rather than a wrong action.

### 5. I mode and the insert path

| Key | Command |
|---|---|
| any text key | `ed.edit.insert.text` (`sarg` = the key's UTF-8 bytes) |
| `<cr>` | `ed.edit.insert.newline` |
| `<tab>` | `ed.edit.insert.tab` (literal `\t`; `expandtab` is Sprint 36) |
| `<bs>` / `<del>` | `ed.edit.delete.grapheme_left` / `.grapheme` |
| `<left>` `<right>` | `ed.move.char.prev` / `.next` — **graphemes**, s09 |
| `<up>` `<down>` | `ed.move.line.up` / `.down` (I borrows the line unit's vertical) |
| `<home>` `<end>` | line home/end |
| `<esc>` | `ed.mode.escape` |
| paste block | one `ed.edit.insert.text` carrying the whole payload |

- `ed.edit.insert.newline` inserts `yew_filemeta_eol_bytes(meta)` — **not
  a hard `"\n"`**. Pitfall: hard-coding LF silently converts a CRLF file
  to mixed EOL on the first Enter, which the Sprint 8 round-trip contract
  would then faithfully preserve as damage.
- **Bracketed paste is one command.** Sprint 4 delivers paste as
  start/payload/end; the dispatcher accumulates the payload and issues a
  single `ed.edit.insert.text`. Per-keystroke pasting would mint N undo
  steps, N journal records, N mark adjustments, and N damage rows for one
  user action.
- Undo-transaction boundaries in I mode (pinned): a transaction opens on
  the first buffer-changing command after entering I and closes on (a)
  leaving I, (b) any command without `YEW_CMD_CHANGES_BUFFER` (a motion —
  arrow keys break the run, as in every editor worth using), (c) an
  explicit `ed.edit.undo_barrier`, (d) a newline insert. No timer-based
  boundary: time-dependent undo granularity is untestable and
  nondeterministic (invariant 5).
- Every buffer-changing command routes through one helper so the journal,
  undo, marks and cursors stay on the single op stream s09 §3 requires:

```c
/* The ONLY place src/edit/ mutates text.  Three consumers, one stream:
 * journal (s08) → undo (s10) → marks+cursors (s09).                   */
static void ed_apply(Ed *ed, u8 op, ByteOff at, const u8 *b, u64 n);
```

### 6. Naive viewport (placeholder, replaced in Sprint 15)

```c
typedef struct Viewport { LineNo top; u16 rows, cols; } Viewport;
void yew_win_follow_cursor(Win *w);   /* clamp top so the cursor is in  */
```

Rules this sprint: if `cur_line < top`, `top = cur_line`; if
`cur_line >= top + rows`, `top = cur_line - rows + 1`. No scrolloff, no
wrap, no horizontal scroll — a line longer than `cols` is clipped at the
right edge (its remainder is unreachable by rendering, not by the model).
Sprint 15 replaced this placeholder wholesale. Its initial two-consumer rule
kept that replacement local; it was not a permanent ban on state consumers.
Later owners legitimately consume viewport state for persistence, tab-view
preservation, status/search display, window lifecycle, Fletch inspection, and
Git integration. Generic follow, wrap, scroll, and clamp algorithms remain in
`src/ui/viewport.c` (S59-A14).

### 7. Open, save, quit — `src/edit/file_cmds.c`

**Open (argv).** `yew [path]`:

| Argument | Behavior |
|---|---|
| none | empty scratch buffer, path `NULL`, footer shows `[no name]` |
| regular file | `yew_file_load`; `YEW_LOAD_ENOENT` → empty buffer at that path (new file) |
| directory | hard error naming Sprint 25 (dir = workspace) |
| `YEW_LOAD_TOO_LARGE` / `EACCES` / `EISDIR` | exit 3 with the s08 message |
| ≥ 2 files | hard error naming Sprint 23 (tabs) |

**Journal recovery** (Sprint 8 deferred the prompt to here): if a
header-matched journal exists for the loaded realpath, push a prompt layer
before first paint:

```
recover unsaved changes to src/main.c?   [r]ecover  [d]iscard  [esc] open as-is
```

`r` runs `yew_journal_replay` into the loaded buffer and leaves it dirty;
`d` unlinks the journal; Esc keeps the journal on disk untouched (the
user's bytes are sacred even when confusing — s08's stale-journal
doctrine).

**Save.** `ed.file.save` → `yew_file_save`. On `YEW_SAVE_OK`: mark the
undo tree's clean node, `yew_journal_discard`, message
`"wrote src/main.c, 128 lines"`. On `YEW_SAVE_CHANGED_ON_DISK`: prompt
`file changed on disk — [o]verwrite [esc] cancel`. On any I/O error: the
error message stays until the next keypress (severity ERROR, §8) and the
buffer stays dirty. A buffer with `path == NULL` messages
`no file name (save-as lands in Sprint 18)`.

**Quit.** `ed.quit` with a clean buffer exits 0 through `yew_tty_close`.
Dirty → prompt layer (the same shape Sprint 23 reuses per tab):

```
save changes to src/main.c?   [w]rite  [d]iscard  [esc] cancel
```

`w` saves and quits **only on save success** — an I/O error aborts the
quit (invariant 1: no path where "quit" discards bytes the user asked to
keep). `ed.quit_force` skips the prompt and leaves the journal in place so
the next open can recover.

### 8. One-row footer and messages (placeholder for Sprint 15)

```c
typedef enum { YEW_MSG_INFO, YEW_MSG_WARN, YEW_MSG_ERROR } MsgSev;
void yew_msg(Ed *ed, MsgSev sev, const char *fmt, ...);
```

Last screen row, single line, no color roles yet: `[MODE] path[*]
line:col` on the left, the current message (if any) taking the row when
present. INFO expires after 4 s via a heap timer; WARN 8 s; ERROR persists
until the next keypress. Sprint 15 lands the real statusline spec (mode
chip recoloring the footer, encoding/EOL/percent segments, severity
attributes, truncation, expand key) — this row exists so the prompts above
have somewhere to live.

### 9. Rendering the buffer — `src/ui/draw.c`

`void yew_draw_win(Ed *ed, Win *w)`: for each screen row, take the logical
line's `Span` (s07), iterate its graphemes (s09/s02), write cells with the
Sprint 5 wide-glyph continuation convention, expanding `\t` to the next
tab stop and invalid bytes via the s02 escape policy. Cursor placement:
the hardware cursor goes to `(row, CCol)` from `yew_off_to_ccol` — the
only width math is that call (conversions live in `src/unicode/`).

- Damage: `yew_grid_damage_rows(lo, hi)` for edited line ranges; a mode
  change damages only the footer; `ed.redraw` and resize damage
  everything. **Pitfall:** an edit that changes the *line count* damages
  every row from the edit to the bottom of the viewport — computing damage
  from the edited byte range alone leaves stale lines below an inserted
  newline.

### 10. Defer

- Scrolloff, wrap, horizontal scroll, gutter, real statusline, message
  severity attrs, SIGWINCH relayout spec → Sprint 15.
- W/B/H/E/F modes were deferred to 16/16/17/18/52; their `ed.mode.enter`
  args were `YEW_CMD_DEFERRED` at this landing and are live now (S59-A13).
- Multiple files/tabs → 23; panes → 22; save-as / `:w path` → 18.
- Registers, clipboard yank/paste keys → Sprint 12's API exists; the L-mode
  bindings land with selections in Sprint 17.
- Search, jumplist → 20/21. Macro record/replay → 35. Config → 36.

## Testing Strategy

- **Unit** (`tests/unit/test_loop.c`): `yew_loop_deadline` over a matrix
  of {chord pending, timer pending, probe pending, nothing} — asserts
  `-1` when idle and the minimum otherwise; timer heap ordering with
  equal deadlines (stable by insertion, determinism); cancel-then-fire
  never runs a cancelled callback.
- **Unit** (`tests/unit/test_modes.c`): Esc collapse order (chord →
  count → prompt → mode) driven through `yew_dispatch_key`; the original
  staging test pinned deferred sprint numbers, while current coverage pins
  the live later-owned entries through the same route (S59-A13).
- **Unit** (`tests/unit/test_edit_cmds.c`): every L and I command against
  a fixture buffer with the s09 golden text (ZWJ family, CJK, CRLF,
  invalid byte) — `x` on a 25-byte ZWJ cluster removes exactly 25 bytes;
  `<bs>` at col 0 joins lines; newline insert emits CRLF in a CRLF
  buffer; `12G` lands on line 12; `4x` deletes 4 clusters in one command
  invocation with one undo step.
- **Pty** (`tests/pty/notepad_*.golden`): open a fixture → arrows → `i` →
  type `héllo 漢字 👨‍👩‍👧‍👦` → Esc → `s` → `q`; goldens for each stage; a
  second run byte-compares (invariant 5). Separate goldens: journal
  recovery prompt (all three answers), dirty-quit prompt (all three), the
  `q` vs `q !` chord timeout (harness advances the fake clock), save error
  path via a read-only fixture dir.
- **Frame counting** (`tests/pty/burst_frames.c`): feed 4 KB of text as
  one paste and as 4096 individual keys inside one poll window; assert the
  harness observes **exactly one** synchronized-output frame in both cases
  (this is the typeahead-coalescing gate).
- **Torture** (extends `tests/torture/`): the s08 kill -9 suite gains a
  live-editor lane — `yew` runs under a pty, a scripted edit list is fed,
  `SIGKILL` lands at each `YEW_FAULT_AT` step of the save, and the s08
  checker asserts the same invariant (dst is old-or-new; journal replay
  reproduces the edits). The 500-iteration external-SIGKILL lane runs
  against the live editor too.
- **Perf** (`tests/perf/latency.c`): keypress→paint p99 over 10 000 keys
  on a 10 kLOC fixture; cold start → first paint on the same fixture.
  Baselines committed; the gate is enforced from this sprint on
  (invariant 4), recalibrated in Sprint 56.
- Lanes: gcc, clang, ASan/UBSan, valgrind, determinism (double-run pty
  goldens), bans.

## Definition of Done

1. `yew tests/fixtures/hello.txt` opens, arrows move, `i` inserts, Esc
   returns to L, `s` writes, `q` exits 0 — proven by a pty golden chain,
   byte-identical across two runs.
2. `load → edit → save → cmp` preserves everything the s08 contract
   promises: CRLF, BOM, no-final-newline, invalid bytes, on a 6-fixture
   matrix driven through the **live editor**, not the API.
3. Typeahead coalescing: the burst test observes exactly 1 frame for
   4096 keys and for a 4 KB paste; a per-key render regression fails it.
4. Latency gate live: keypress→paint p99 ≤ 5 ms and cold start → first
   paint ≤ 20 ms on the CI reference runner, with committed baselines.
5. kill -9 torture green against the live editor: 100 % of iterations
   satisfy the s08 checker across the shim sweep, 4 seeds, and 500
   external-SIGKILL iterations. `make torture` includes the new lane.
6. Terminal restore holds for the live editor: `kill -SEGV`, `kill -TERM`,
   `kill -9`, and `ed.suspend`+`fg` each leave a usable terminal
   (pty test asserts the restore blob on the wire, invariant 6).
7. Every L and I table row from §4/§5 is exercised by a unit or pty test.
   Later owning sprints delivered W/B/H/E/F through the same
   `ed.mode.enter` route, with keyboard entry and mode-state coverage; the
   original future-sprint messages were staging guards (S59-A13).
8. Undo granularity: typing `hello`, Esc, `u` restores the pre-insert
   buffer in one step; typing `hel`, `<left>`, `lo`, Esc needs two.
9. Dirty-quit and journal-recovery prompts: all 3 + 3 answers covered;
   `w` on a failing save aborts the quit and keeps the buffer dirty.
10. Later owners legitimately consume viewport state for explicit
    persistence, view preservation/restoration, display, integration, and
    lifecycle duties. Direct uses are classified; generic follow, wrap,
    scroll, and clamp algorithms remain owned by `src/ui/viewport.c`
    (S59-A14).
11. No direct key→function wiring: `scripts/check-cmd-dispatch.sh` still
    passes with ≥ 35 new commands registered.
12. ≥ 120 assertions across the three new unit files; ≥ 10 pty goldens;
    ASan/UBSan/valgrind clean including the pty and torture binaries.
