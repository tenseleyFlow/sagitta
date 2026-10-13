# Sprint 12: Registers and the System Clipboard

## Prerequisites

- Sprint 7 — `TextBuf`, `Span`, `TextIter` (yank = iterate a span into a
  `Bytebuf`, no intermediate copies), `yew_textbuf_line_span`.
- Sprint 8 — `FileMeta.eol` and `yew_filemeta_eol_bytes()` (linewise paste
  synthesizes separators in the *destination* buffer's EOL style).
- Sprint 9 — `Cursor`, `CursorSet`, `yew_off_to_ccol` / `yew_ccol_to_off`
  (blockwise paste is column arithmetic and every bit of it lives in
  `src/unicode/`), `yew_textbuf_line_start`.
- Sprint 10 — `EditCtx`, `yew_edit_insert/delete`, `yew_undo_begin/end`
  (`YEW_TXN_PASTE` — one paste is exactly one undo node) and
  `yew_undo_reopen` (the kill-ring cycle amends the paste node instead of
  branching the tree).
- Sprint 3 — `Tty`, raw mode, capability probing, the terminal-restore
  guarantee; Sprint 4 — the input escape-sequence state machine (§7's
  security rule adds one discard rule to it); Sprint 5 — `render.c` and
  synchronized output (mode 2026) BSU/ESU framing.
- Sprint 0 — `Bytebuf`, `Vec(T)`, `Arena`, `yew_log`, `YEW_BUG`, XDG helper.
- Binding: `00-decisions.md` Clipboard row (*"Registers internal; system
  clipboard via OSC 52 (bespoke, zero deps); optional xclip/wl-copy/pbcopy
  subprocess fallback"*), invariant 7 (bespoke — no base64 library),
  invariant 8 (single-threaded core: the subprocess writer is an event-loop
  citizen, never a blocking call).
- Assumption beyond index entries: s03 exposes a raw tty write
  (`yew_tty_write(const u8*, u64)`) and s05 owns the output stream; if s05
  did not leave a hook for out-of-band sequences, this sprint adds
  `yew_term_oob_queue()` in `src/term/render.c` (§6).

## Goals

Land `src/text/register.c|h` and `src/util/base64.c|h`: the full register
file (named `a`–`z` with `A`–`Z` append, unnamed, yank register `0`, the
numbered delete shift `1`–`9`, small-delete `-`, last-insert `.`, search,
cmdline, file/alt-file, blackhole), the three register types and the exact
paste semantics of each including linewise line-opening and blockwise column
padding, a kill-ring history with bounded depth and bytes, an OSC 52 writer
with tmux/screen passthrough wrapping, size chunking, a documented practical
limit, and the security rule that yew **writes** the system clipboard and
never reads it through the terminal; subprocess fallbacks with a detection
order and the `YEW_CLIPBOARD` override; and a pinned default for the "does
`y` go to the system clipboard" question. Keybindings (`y`, `p`, `"a`) are
Sprint 13/14; the `:registers` display is Sprint 18; the register picker is
Sprint 26. Blockwise *selection* was deferred to Sprint 17; this sprint
shipped blockwise storage and paste through the direct test API while other
producers hard-errored. Sprint 17 subsequently delivered the production path
through the same routing API (S59-A11).

## Deliverables

### 1. Register values — `src/text/register.h`

```c
typedef enum { YEW_REG_CHARWISE = 0, YEW_REG_LINEWISE = 1,
               YEW_REG_BLOCKWISE = 2 } RegType;

typedef struct RegVal {
    u8      type;         /* RegType                                      */
    bool    ragged;       /* BLOCKWISE: rows keep natural length (to-EOL) */
    u32     width;        /* BLOCKWISE: block width in CCol               */
    Bytebuf bytes;        /* payload; LINEWISE always ends with EOL bytes */
    Vec(Span) rows;       /* BLOCKWISE only: row slices into `bytes`      */
    i64     t_wall;       /* set time, for the Sprint 26 picker           */
} RegVal;
```

Registers own their bytes (a `Bytebuf` per register, reused across writes so
typing `dd` in a loop does not churn the allocator). Payload bytes are
**never transformed** on store or on paste — invalid UTF-8, NULs, and CRLF
travel through a register byte-exact (invariant 2). The only bytes yew
ever *synthesizes* are linewise EOL separators (§4) and blockwise padding
spaces (§5), and both are called out in the tables below.

### 2. The register file and the routing table

```c
typedef struct Registers {
    RegVal named[26];             /* a–z                                  */
    RegVal unnamed;               /* "                                    */
    RegVal numbered[10];          /* 0–9                                  */
    RegVal small_del;             /* -                                    */
    RegVal last_insert;           /* .                                    */
    RegVal search, cmdline;       /* / and :                              */
    RegVal file, alt_file;        /* % and #                              */
    RegVal ring[YEW_KILL_RING_MAX];  /* §6                                */
    u32    ring_head, ring_len;
    u64    ring_bytes;
} Registers;
```

| Name | Written by | Read | Notes |
|---|---|---|---|
| `"` unnamed | every yank/delete/change that did not name `_` | default paste source | always mirrors the most recent write |
| `a`–`z` | explicit `"x` prefix | `"x` | replaces |
| `A`–`Z` | explicit `"X` prefix | — | **appends** to the lowercase register; §3 |
| `0` | every **yank**, and only yanks | `"0p` | survives deletes — the register that makes "yank, delete something, paste the yank" work |
| `1` | any delete/change whose payload contains an LF **or** whose type is `LINEWISE`; shifts `9←8 … 2←1` first | `"1p` | see the pinned rule below |
| `2`–`9` | shift targets only | | `9` is discarded off the end |
| `-` | any other delete/change (charwise, no LF) | `"-p` | never shifts |
| `.` | text of the last insert session — the payload of the last `YEW_TXN_TYPE` node (s10) | read-only | write attempt is `YEW_BUG` |
| `/` | last search pattern (Sprint 21 writes it) | read-only here | |
| `:` | last E-mode command (Sprint 18 writes it) | read-only here | |
| `%` | current buffer path | read-only | computed on read, not stored |
| `#` | alternate buffer path (Sprint 23) | read-only | |
| `_` blackhole | writes are discarded entirely | reads empty | does not touch `"`, `0`, `1`–`9`, `-`, or the ring |
| `+` / `*` | system clipboard (§7/§8) | | `*` is an **alias** of `+`; OSC 52 exposes no PRIMARY/CLIPBOARD distinction we can honor portably, and the subprocess layer maps `*` to `--primary` only for wl-copy/xclip/xsel |

**Pinned shift rule** (vim's is folklore; ours is one sentence): a delete
goes to `1` (after shifting) iff its payload contains at least one `0x0A`
**or** its type is `LINEWISE`; every other delete goes to `-`. Yanks never
shift and never touch `1`–`9` or `-`.

```c
void    yew_reg_init(Registers *r);
void    yew_reg_free(Registers *r);
RegVal *yew_reg_get(Registers *r, u8 name);       /* NULL if unknown      */
void    yew_reg_append(Registers *r, u8 name, const RegVal *v);  /* A–Z    */

/* typed producer routes delivered by their owning sprints */
void yew_reg_set_macro(Registers *r, u8 name, const RegVal *v, bool append);
void yew_reg_set_cmdline(Registers *r, const u8 *bytes, size_t len);
void yew_reg_set_search(Registers *r, const u8 *bytes, size_t len);

/* the two document-edit ROUTING functions */
void yew_reg_yank  (Registers *r, u8 explicit_name /*0 = none*/,
                    const RegVal *v);
void yew_reg_delete(Registers *r, u8 explicit_name, const RegVal *v);
```

**Pitfall — one routing choke point.** `yew_reg_yank`/`yew_reg_delete`
implement the whole table above (unnamed mirror, `0`, the shift, `-`, the
ring push, the §9 clipboard sync). The raw storage setter is private
`reg_set` in `register.c`; public writers use a typed producer or the
yank/delete front doors. Reintroducing public `yew_reg_set` would let one
code path get the unnamed mirror wrong without being obvious — the same
doctrine as s09's mark choke point and s10's edit choke point. The ban and
positive control in `scripts/bans.sh` enforce this stronger post-Sprint-58
surface (S59-A12).

### 3. Append semantics (`A`–`Z`)

| Existing type | Appended type | Result |
|---|---|---|
| CHARWISE | CHARWISE | bytes concatenated; type CHARWISE |
| CHARWISE | LINEWISE | destination EOL bytes appended to the existing payload, then the new payload; type becomes **LINEWISE** |
| LINEWISE | CHARWISE | new payload appended after the existing trailing EOL, then destination EOL bytes appended; type stays LINEWISE |
| LINEWISE | LINEWISE | concatenated; type LINEWISE |
| any | BLOCKWISE | append is **refused**: `yew_log(WARN)` + status message, register unchanged. Rationale: there is no defensible union of a block and a non-block, and silently flattening one would be a data surprise |
| BLOCKWISE | BLOCKWISE | rows concatenated; `width = max(width)`; non-ragged rows re-padded to the new width; `ragged` is the OR |

### 4. Linewise storage and paste — pinned

Storage: **a linewise payload always ends with EOL bytes.** When the yanked
region is the final line of a buffer with `missing_final_nl`, the separator
is synthesized on store using `yew_filemeta_eol_bytes(meta)`.

Paste (`p` = after, `P` = before), at cursor line `l`:

| Case | Insert offset | Bytes inserted |
|---|---|---|
| `P` (before) | `line_start(l)` | payload verbatim |
| `p` (after), line `l` has EOL bytes | `line_span(l).hi` | payload verbatim |
| `p` (after), `l` is the last line and lacks a final EOL | `len` | destination EOL bytes, then payload **minus its trailing EOL** |

Cursor after a linewise paste: first non-blank grapheme of the **first**
pasted line (both `p` and `P`). Recorded as the transaction's `after`
cursor snapshot (s10 §6) so undo/redo place it identically.

**Pitfall — never translate payload EOLs.** Pasting LF-styled text into a
CRLF buffer inserts LF bytes; only the *synthesized* separator uses the
destination style. Bytes are sacred (invariant 2), and `FileMeta` already
models the result (`YEW_EOL_MIXED`), which the statusline surfaces in
Sprint 15. Rewriting payload bytes to "help" is how an editor silently
changes 40 000 lines of a file the user never touched.

Charwise paste, for completeness: `P` inserts at `cursor.pos`; `p` inserts
at `yew_grapheme_next(cursor.pos)`, except at end-of-line where it inserts
at `line_span(l).hi - eol_len` (before the EOL bytes, never after them).
Cursor lands on the **last** grapheme of the pasted text.

### 5. Blockwise storage and paste — pinned

Storage: `rows` are byte slices into `bytes`, one per source line; `width`
is the block's width in **CCol**, not bytes, because columns are visual.
Non-ragged rows are right-padded with spaces to `width` **at store time** —
so paste is a straight per-row insert. `ragged` blocks (the to-EOL variant
from Sprint 17's `$`-block) keep natural row lengths and are never padded.

Paste at cursor line `l0`, target column `c`:

- `P`: `c = yew_off_to_ccol(line(l0), cursor.pos)`.
- `p`: `c = yew_off_to_ccol(line(l0), yew_grapheme_next(cursor.pos))`.
- For each row `i` of `rows`, destination line `l = l0 + i`:
  1. If `l` exceeds the last line, append destination EOL bytes to create
     it (one insert per missing line, all inside the one transaction).
  2. `off = yew_ccol_to_off(line(l), c, tabw)` — s09 guarantees this lands
     on a **glyph start**, never inside a wide glyph or a tab.
  3. `pad = c - yew_off_to_ccol(line(l), off)` — the shortfall in cells,
     which is nonzero when the line is short **or** when `c` landed inside
     a tab's cell run and rounded left. Insert `pad` **spaces**, then the
     row's bytes, at `off`.
- Cursor lands at the first row's insertion offset.

Padding is real spaces that become buffer content. Virtual padding was
rejected: the buffer must never render bytes it does not contain, or the
save path and the render path disagree and one of them is wrong. Tabs are
never used for padding — a tab's width depends on `tabwidth`, so a block
pasted at `tabwidth=4` would shear at `tabwidth=8`.

**Pitfall — `pad` is computed in cells and inserted as bytes.** Computing it
in bytes puts a CJK block one column left on every line containing a wide
glyph. All three quantities (`c`, the round-left offset, `pad`) come from
`src/unicode/`; `register.c` performs no width arithmetic (grep gate,
DoD 12 — the same rule s09 established).

### 6. Kill-ring history

```c
#define YEW_KILL_RING_MAX 256
void yew_reg_ring_push (Registers *r, const RegVal *v);
bool yew_reg_ring_cycle(Registers *r, EditCtx *ec, i32 delta);
u32  yew_reg_ring_list (const Registers *r, RegInfo *out, u32 max);
```

- Every write to the **unnamed** register also pushes onto the ring. Named
  registers do not; the blackhole does not.
- Depth `registers.ring_depth`, default **32** (0 disables, max
  `YEW_KILL_RING_MAX`). Byte cap `registers.ring_bytes_max`, default
  **8 MiB**; the oldest entries are dropped until the cap holds. A single
  entry larger than the cap is stored alone (never truncated — see §1) and
  evicts everything else.
- `yew_reg_ring_cycle` is the "paste, then reach further back" flow: valid
  only immediately after a paste with no intervening edit. It deletes the
  just-pasted span, pastes ring entry `head ± delta` with that entry's own
  `RegType`, and **amends the same undo node** via `yew_undo_reopen(ut,
  YEW_TXN_PASTE)` — cycling five times must not leave five nodes and four
  dead branches in the undo tree. If `reopen` returns false (an edit
  intervened), the cycle is refused with a status message rather than
  guessing.
- The ring is **not** the numbered registers. `1`–`9` are a delete history
  routed by §2's shift rule; the ring is an unnamed-write history including
  yanks. They overlap in content and differ in policy; unifying them is a
  reviewer's instinct and a behavior regression. Stated here so it is a
  decision, not an oversight.

### 7. OSC 52 — `src/term/osc52.c|h`, `src/util/base64.c|h`

Base64 encoder, bespoke (invariant 7):

```c
u64  yew_base64_len(u64 n);                       /* 4*ceil(n/3), no NUL */
void yew_base64_encode(const u8 *in, u64 n, u8 *out);  /* RFC 4648 §4    */
```

Standard alphabet, `=` padding, **no line wrapping** (a `\n` inside an OSC
string terminates it on some terminals and corrupts the screen on others).
RFC 4648 test vectors, all six committed as unit cases:

| Input | Output |
|---|---|
| `""` | `""` |
| `"f"` | `Zg==` |
| `"fo"` | `Zm8=` |
| `"foo"` | `Zm9v` |
| `"foob"` | `Zm9vYg==` |
| `"foobar"` | `Zm9vYmFy` |

Sequence: `ESC ] 52 ; <targets> ; <base64> ST` where `ST` is `ESC \`
(`0x1b 0x5c`). We emit ST, not BEL — BEL is the legacy terminator and some
terminals treat a stray `0x07` in a long string as data. Default targets
`c` (CLIPBOARD); `YEW_CLIPBOARD_TARGET` accepts `c`, `p`, or `cp`.

Multiplexer wrapping — detection order and exact framing:

| Condition (checked in order) | Wrapper |
|---|---|
| `YEW_OSC52=off` | do not emit at all |
| `YEW_OSC52=plain\|tmux\|screen` | forced, skip detection |
| `$TMUX` set | `ESC P tmux ; <seq with every ESC doubled> ESC \` |
| `$STY` set, or `TERM` matches `screen*` | `ESC P <seq> ESC \`, with the **base64 split into ≤ 768-byte pieces**, each piece its own DCS wrapper |
| otherwise | `<seq>` unwrapped |

Notes that save an afternoon: tmux passthrough needs
`set -g allow-passthrough on` (tmux ≥ 3.3) *or* `set -g set-clipboard on`,
and in the latter case tmux consumes OSC 52 itself so the passthrough wrapper
is wrong. We cannot query which is configured — `YEW_OSC52=plain` is the
documented escape hatch, named in `--help` and the man page. screen truncates
long DCS strings; 768 is the empirically safe chunk, a named constant
(`YEW_OSC52_SCREEN_CHUNK`) carrying that provenance in a comment.

Practical size limit: terminals cap OSC string length (xterm's default is
small, tmux's buffer generous but finite). `YEW_OSC52_MAX` default
**100 000 bytes of base64** ≈ 74 KiB of payload. Over the limit we **never
truncate**: try the subprocess path (§8), and if none exists set the `+`
register anyway and show `clipboard: 3.2 MiB exceeds the OSC 52 limit;
register + still holds it`. Silently clipping a yank at 74 KiB is the same
category of harm as corrupting a save.

Output routing is through `void yew_term_oob_queue(const u8 *seq, u64 n);`
in `src/term/render.c`. **Pitfall — never write OSC 52 inside a frame.**
`render.c` wraps each frame in synchronized-output BSU/ESU (mode 2026, s05);
an OSC or DCS between them is at best ignored and at worst renders as garbage
on terminals that buffer the region. The queue is flushed **after** ESU and
before the next BSU, at most once per input burst — a 1.3 MiB base64 blob
also has no business inside a latency-budgeted frame (invariant 4).

**Security rule — we write, we never read.** yew contains no OSC 52 query
emitter (`ESC ] 52 ; c ; ? ST`), pinned here so nobody "adds paste support"
later. A terminal configured to answer a clipboard query types the clipboard
contents back on the tty **as input**, where every process sharing that tty —
a `$SHELL -c` job (s19), an LSP server that inherited stdio (s46), a plugin
subprocess (s54) — can read it; the query turns the user's clipboard into
ambient data on a shared channel. Because the reply arrives as an input
escape sequence, the rule is enforced on both sides: we never emit the query,
and s04's decoder **discards** an unsolicited OSC 52 reply with a
`yew_log(WARN)` instead of decoding it as text (pty test, DoD 8). Inbound
paste is bracketed paste — the terminal's own explicit path — and a real read
(`"+p`) goes through a **subprocess** (§8), which never touches the tty:
**clipboard reads only ever go through a subprocess.** Grep gate for
`52;c;?` in `src/` (DoD 12).

One honest limitation: an OSC 52 write has **no success signal**, and many
terminals disable clipboard writes by default (xterm's `allowWindowOps`,
tmux's `set-clipboard`). That is exactly why §8 prefers a subprocess whenever
one is plausibly local — a subprocess has an exit status.

### 8. Subprocess fallbacks — `src/text/clipboard.c|h`

```c
typedef enum { YEW_CLIP_NONE, YEW_CLIP_WL, YEW_CLIP_XCLIP,
               YEW_CLIP_XSEL, YEW_CLIP_PB, YEW_CLIP_OSC52,
               YEW_CLIP_CUSTOM } YewClipBackend;

YewClipBackend yew_clip_detect(void);              /* cached per session  */
bool yew_clip_write(const RegVal *v, u8 target);   /* '+' or '*'          */
bool yew_clip_read (RegVal *out, u8 target);       /* subprocess only     */
```

Detection order (first row whose probe passes wins):

| # | Backend | Write command | Read command | Probe |
|---|---|---|---|---|
| 1 | wl-copy | `wl-copy --type text/plain;charset=utf-8` (`--primary` for `*`) | `wl-paste --no-newline` | `$WAYLAND_DISPLAY` set **and** `wl-copy` on `PATH` |
| 2 | xclip | `xclip -selection clipboard -in` (`-selection primary` for `*`) | `xclip -selection clipboard -out` | `$DISPLAY` set **and** on `PATH` |
| 3 | xsel | `xsel --clipboard --input` (`--primary` for `*`) | `xsel --clipboard --output` | `$DISPLAY` set **and** on `PATH` |
| 4 | pbcopy | `pbcopy` | `pbpaste` | target is `arm64-macos` **and** on `PATH` |
| 5 | OSC 52 | §7 | **never** | stdout is a tty |
| 6 | none | — | — | register-only; status message on `"+y` |

**Remote-session rule:** if `$SSH_TTY` or `$SSH_CONNECTION` is set, rows 1–4
are skipped for *writes* and OSC 52 is preferred — an `xclip` reachable over
forwarded X writes the clipboard of a machine the user is not looking at.
Reads still use rows 1–4 when available (a remote read of a remote clipboard
is at least coherent) and otherwise fail with a message; there is no OSC 52
read, ever (§7).

`YEW_CLIPBOARD` override, grammar:

```
YEW_CLIPBOARD = auto | osc52 | wl | xclip | xsel | pb | none
              | cmd:<write-argv>[|<read-argv>]      # argv words split on space
```

`cmd:` never goes through a shell — the words are `execvp`'d directly.

Execution rules:

- `fork` + `execvp` with a pipe on the child's stdin. **Never `system()` or
  `popen()`** — a shell in the path is a quoting bug waiting to become a
  command injection, and `popen` discards the exit detail we need. Grep
  gate, DoD 12.
- The write is nonblocking: the pipe fd joins the event loop (invariant 8 —
  no threads, and a hung tool must never delay a keystroke past the 5 ms
  budget). `YEW_CLIPBOARD_TIMEOUT_MS` default 1 000, then `SIGKILL` and a
  status message. Reads are synchronous under the same timeout, capped at
  `registers.clip_read_max` (default 64 MiB) so a hostile clipboard cannot
  exhaust memory.
- **Pitfall — clipboard tools do not exit.** `wl-copy` and `xclip` stay
  resident to *own* the selection until another client takes it; waiting for
  exit hangs forever and killing them drops the clipboard. Policy: the write
  succeeds **when the pipe is fully written and closed**, not when the child
  exits; children are reaped opportunistically with `SIGCHLD` +
  `waitpid(WNOHANG)` and nothing ever blocks on them. **Double-fork** the
  writer so the resident grandchild reparents to init — otherwise every yank
  leaves a child that outlives yew's own exit path.
- `SIGPIPE` is ignored process-wide (set in s03's tty setup; asserted here)
  so a tool that died mid-write yields `EPIPE`, not a signal. On `EPIPE`:
  log, fall through to the next backend, remember the demotion for the
  session.

### 9. Clipboard ↔ register interaction — the pinned default

Option `clipboard.sync` (Fletch config, Sprint 36; the enum and the default
land here):

| Value | Meaning |
|---|---|
| `off` | never touch the system clipboard implicitly; only `"+y` / `"+p` do |
| **`yank`** | **default** — every **yank** (never a delete) also writes the system clipboard; pastes still read the internal unnamed register unless `"+p` is used |
| `all` | every yank *and* delete writes the system clipboard |
| `unnamed` | `"` **is** `+`: the unnamed register is the system clipboard in both directions (vim's `clipboard=unnamedplus`) |

Default is `yank`. Why: the near-universal first complaint about a terminal
editor is "I yanked and could not paste into my browser", and the
near-universal harm of `all` is that every `x` and `d` destroys whatever the
user copied from elsewhere. Yank-only fixes the former without causing the
latter; deletes still populate `-` and `1`–`9` for in-editor use. The second
reason is latency: `yank` never makes a paste *read* the system clipboard, so
`p` costs no `fork`/`exec` and no pipe round-trip, whereas `unnamed` pays
~1 ms of process spawn inside the keypress path — 20 % of the 5 ms budget for
a keystroke that should cost microseconds (invariant 4).

Sync scheduling, pinned: the clipboard write is **queued, not performed** in
the yank path, coalesced to one write per input burst (a macro that yanks 500
times spawns one `wl-copy`, not 500), and performed after render alongside
the §7 OOB flush. Payloads over `YEW_OSC52_MAX` skip the OSC 52 route per §7.

### 10. Defer

- Keybindings (`y`, `p`, `P`, `"x`, `"X`) → Sprint 13/14; selections and the
  yank of a selection → Sprint 17.
- **Blockwise register production** was deferred to Sprint 17; storage and
  paste were complete here while non-test producers hard-errored. Sprint 17
  subsequently delivered production through the register choke point.
- `:registers` display → Sprint 18 (`yew_reg_list` returns the rows);
  register/kill-ring **picker** → Sprint 26.
- Writers for `/` (21), `:` (18), and `#` (23) were deferred here. The
  delivered register producers replace these staging guards through the same
  typed slots; absent or unknown producers still never lie (S59-A11).
- Macro registers and record/replay into `a`–`z` → Sprint 38; Fletch access
  → Sprint 34; `clipboard.sync` as a *configurable* option → Sprint 36 (the
  enum, the default, and all four behaviors are pinned here).
- Image/rich-content clipboard and any PRIMARY-vs-CLIPBOARD divergence
  beyond the `*` alias → not for 1.0.

## Testing Strategy

- **Unit** (`test_base64.c`): the six RFC 4648 vectors; `yew_base64_len` vs
  the encoder for n = 0…1024; round-trip fuzz against a test-only decoder
  (10 000 random binary buffers incl. NULs and high bytes); no output byte
  outside the alphabet + `=`.
- **Unit** (`test_register.c`): every §2 routing row driven by a yank and by
  a delete; the shift rule at its boundary (charwise-with-LF → `1`,
  linewise-without-LF → `1`, charwise-without-LF → `-`); all six §3 append
  rows; blackhole leaving `"`, `0`, `1`, `-` and the ring untouched.
- **Unit** (`test_paste.c`): every §4 and §5 row — linewise `p`/`P` at
  first/middle/last line and at a last line with no final newline; charwise
  `p` at EOL landing before the EOL bytes; blockwise onto short lines, onto
  a line ending inside a tab (round-left plus pad), onto CJK glyphs at the
  target column, past the last line (extension), and ragged. Every paste is
  one undo node and undo restores content **and** cursor.
- **Unit** (`test_ring.c`): depth and byte-cap eviction; an oversized entry
  evicting everything; 5 cycles amending exactly one undo node; cycle
  refused after an intervening edit.
- **Unit** (`test_osc52.c`): byte-exact goldens for plain, tmux (ESC
  doubling on a payload containing `0x1b`), screen (1 800-byte base64 → 3
  DCS chunks, boundaries pinned); the over-limit path returns false and
  emits nothing; targets `c`, `p`, `cp`.
- **Unit** (`test_clipboard.c`): detection order over a synthetic
  `PATH`/env matrix (12 combinations incl. `$SSH_TTY`); every
  `YEW_CLIPBOARD` value incl. `cmd:`; a fake tool (`tests/unit/fakeclip.c`)
  recording argv and payload — exact argv, byte-exact payload incl. NULs; a
  never-exiting fake tool — write reported successful, no zombies after 100
  writes, no call blocking > 2 ms; an immediately-exiting one — `EPIPE`
  handled and the backend demoted.
- **Pty** (`test_osc52_frame.c`): yank under the s06 harness — OSC 52 bytes
  appear after ESU, never between BSU and ESU, and the grid golden is
  unchanged (invariant 5). **Pty** (`test_osc52_reply.c`): an unsolicited
  `ESC ] 52 ; c ; <b64> ST` on input reaches the buffer zero times, logs a
  WARN, leaves the grid unchanged.
- **Determinism**: the same yank emits byte-identical OSC 52 output across
  two runs and both compilers. **Sanitizers**: ASan/UBSan on all six unit
  binaries; valgrind clean — register `Bytebuf` reuse and ring eviction are
  the leak surfaces.

## Definition of Done

1. `make test` green under gcc and clang, `-std=c11 -pedantic -Wall
   -Wextra -Werror`, zero warnings; ASan/UBSan/valgrind/determinism green.
2. All six RFC 4648 vectors pass; base64 round-trip fuzz green on 10 000
   binary buffers; encoder emits no newline (`grep`-asserted in the test).
3. Every row of the §2 routing table has a test; the pinned `1`-vs-`-`
   shift rule is tested at both sides of its boundary.
4. All six §3 append rows tested, including the refused BLOCKWISE case.
5. All §4 linewise rows and all §5 blockwise rows tested, including the
   short-line pad, the tab round-left pad, the CJK column, and buffer
   extension past the last line.
6. A paste of any type is exactly **one** undo node; undo restores content
   and cursor byte/offset-exactly; 5 ring cycles produce exactly one node.
7. OSC 52 goldens byte-exact for plain, tmux (ESC doubled), and screen
   (768-byte chunking, 3 chunks); over-`YEW_OSC52_MAX` payloads are never
   truncated and never emitted.
8. Pty: OSC 52 bytes never appear between BSU and ESU; an unsolicited
   OSC 52 reply on input reaches the buffer zero times.
9. Detection order correct for all 12 env/PATH combinations incl. the
   `$SSH_TTY` rule; every `YEW_CLIPBOARD` value honored.
10. Non-exiting clipboard tool: 100 writes complete, none blocking > 2 ms,
    zero zombies, write reported successful.
11. `clipboard.sync` default is `yank`; a delete under the default does not
    write the system clipboard (asserted via the fake tool's log).
12. Grep gates: `grep -rn 'popen\|system(' src/` → nothing;
    `grep -rn '52;c;?\|52;.*?' src/` → nothing;
    no raw public `yew_reg_set(` symbol exists, private `reg_set` storage
    remains in `register.c`, and public production writers use only typed
    `yew_reg_set_*`, `yew_reg_yank`, or `yew_reg_delete` routes;
    `grep -rn 'width\|ccol' src/text/register.c` shows call sites into
    `src/unicode/` only, zero width arithmetic (S59-A12).
13. Sprint 17 blockwise production and the Sprint 21/18/23 writers for `/`,
    `:`, and `#` route through the register choke points established here;
    absent or unknown producers remain explicit rather than lying (S59-A11).
14. ≥ 130 assertions across the six unit files plus the two pty tests.
