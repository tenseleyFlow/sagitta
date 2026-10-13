# Sprint 4: Input Decoding — Escape Parser, Kitty Protocol, Paste, Mouse

## Prerequisites

- Sprint 0 — `Bytebuf`, `Vec(T)`, `yew_log()`, `YEW_BUG`, exit codes.
- Sprint 1 — unit harness (explicit registry table), `make test`,
  `scripts/bans.sh`, CI lanes. `make fuzz` hard-errors naming Sprint 2;
  this sprint lands the first real fuzzer behind it.
- Sprint 2 — UTF-8 decode. Assumption beyond the index entry: the decoder
  is *streaming* (state in, `{codepoint, status}` out) — a keystroke can
  straddle two `read(2)`s. If s02 landed whole-buffer only, add the
  streaming wrapper in `src/unicode/` here.
- Sprint 3 — `Tty`, `TtyCaps` (`kitty_kbd`, `kitty_flags`), raw mode with
  `ISIG`/`IEXTEN`/`IXON`/`ICRNL` cleared, the restore blob (it already
  disables 2004/1002/1006/1004 and pops kitty flags with `CSI < u`), the
  SIGWINCH/SIGCONT self-pipe, and `t->pending` — bytes the probe recognizer
  did not consume, in arrival order. Sprint 3 §5 promises the SIGCONT path
  calls `yew_input_enable()`; this sprint provides it.
- Binding: `00-decisions.md` (Terminal row; invariants 3, 5, 9),
  `01-architecture.md` — *"Key events are normalized once in `input.c`
  (kitty protocol when available, legacy decoding otherwise) — the keymap
  never sees raw bytes."*

## Goals

Land `src/term/input.c|h`: one escape-sequence state machine turning an
arbitrary byte stream into normalized `Key` events, with the kitty keyboard
protocol negotiated on top of a complete legacy decoder, bracketed paste
(2004), SGR mouse (1006 over 1002 tracking), and focus events (1004). The
output contract is the point: from Sprint 13 on the keymap consumes `Key`
and nothing else. Also `tests/fuzz/fuzz_input.c`, flipping `make fuzz` from
its Sprint 1 stub. Keymaps, chords and dispatch land in Sprint 13; mouse
*routing* in Sprint 27 (click-to-focus previewed in Sprint 22); paste
*insertion* in Sprint 14; OSC 52 clipboard replies in Sprint 12. Nothing
here draws — the grid is Sprint 5.

## Deliverables

### 1. The normalized event — `src/term/input.h`

```c
typedef enum {                 /* bitmask; identical to kitty's, which is */
    YEW_MOD_SHIFT = 1,         /* also (CSI modifier param - 1)           */
    YEW_MOD_ALT = 2, YEW_MOD_CTRL = 4, YEW_MOD_SUPER = 8,
    YEW_MOD_HYPER = 16, YEW_MOD_META = 32, YEW_MOD_CAPS = 64,
    YEW_MOD_NUM = 128,
} YewMods;

typedef enum { YEW_EV_NONE = 0, YEW_EV_KEY, YEW_EV_PASTE_BEGIN,
    YEW_EV_PASTE_DATA, YEW_EV_PASTE_END, YEW_EV_MOUSE, YEW_EV_FOCUS } EvKind;

typedef struct Key {
    u32 code;      /* < 0x110000: the key's Unicode codepoint (base       */
                   /* layout when known). >= YEW_KEY_BASE: a named key    */
    u16 kind;      /* EvKind */    u16 mods;   /* YewMods bitmask         */
    u16 col, row;  /* MOUSE only, 0-based cells (the wire is 1-based)     */
    u8  ev;        /* 1 press, 2 repeat, 3 release (legacy: always 1)     */
    u8  button;    /* MOUSE only, YEW_MB_*                                */
    u8  ntext;     /* bytes used in text[]                                */
    u8  text[16];  /* UTF-8 the key produced; NOT NUL-terminated          */
} Key;
_Static_assert(sizeof(Key) == 32, "Key is half a cache line");
```

- One code space: printable keys carry their codepoint (always the
  *unshifted* value, so `ctrl+a` matches on any shift state); named keys
  start at `YEW_KEY_BASE = 0x110000`, one past the last legal scalar, so
  the ranges cannot collide. `FOCUS` sets `code` to `YEW_KEY_FOCUS_IN/OUT`.
- `code` and `text` diverge and must both exist: Shift+2 on US is
  `code='2', mods=SHIFT, text="@"`. Keymaps match `code`+`mods`; insert
  mode consumes `text`. 16 bytes holds any grapheme a keyboard emits (ZWJ
  families arrive by *paste*); overflow truncates on a codepoint boundary.

Named codes (`YEW_KEY_BASE + n`; ordinals are serialized from Sprint 25 on,
so append only): `ESCAPE ENTER TAB BACKSPACE INSERT DELETE` · `UP DOWN
RIGHT LEFT HOME END PAGE_UP PAGE_DOWN BEGIN` · `F1…F35` · `KP_0…KP_9
KP_DECIMAL KP_DIVIDE KP_MULTIPLY KP_SUBTRACT KP_ADD KP_ENTER KP_EQUAL
KP_SEPARATOR KP_LEFT KP_RIGHT KP_UP KP_DOWN KP_PAGE_UP KP_PAGE_DOWN
KP_HOME KP_END KP_INSERT KP_DELETE KP_BEGIN` · `CAPS_LOCK SCROLL_LOCK
NUM_LOCK PRINT_SCREEN PAUSE MENU` · `FOCUS_IN FOCUS_OUT`.

### 2. Parser object and API

```c
typedef struct In {
    Bytebuf buf; size_t rd;      /* unconsumed bytes; compact when rd>4096 */
    u8      state, utf8_state;   /* §3; s02 streaming decoder state        */
    u32     utf8_cp;
    u16     par[16]; u8 sub[16]; u8 npar;  /* ';' params, first ':' subparam */
    u8      priv, inter;         /* CSI '?','>','<','='; intermediate      */
    i64     deadline;            /* CLOCK_MONOTONIC ms; 0 = unarmed (§5)   */
    bool    in_paste; Bytebuf paste;   /* current chunk only               */
    TtyCaps caps; u32 dropped;
} In;

void   yew_input_init(In *in, const TtyCaps *caps);
void   yew_input_seed(In *in, const Bytebuf *pending);   /* Sprint 3 §6   */
void   yew_input_feed(In *in, const u8 *b, size_t n);
/* false = buffer exhausted mid-sequence (need bytes or a deadline tick).
 * kind may be YEW_EV_NONE for a recognized-and-dropped sequence, so
 * callers loop until false.                                             */
bool   yew_input_next(In *in, i64 now_ms, Key *out);
i64    yew_input_deadline(const In *in, i64 now_ms);  /* poll timeout, -1 */
void   yew_input_eof(In *in);         /* flush: a pending ESC → Escape    */
const u8 *yew_input_paste_chunk(const In *in, size_t *n);
void   yew_input_enable(int wfd, const TtyCaps *caps);   /* §8 blob       */
void   yew_input_disable(int wfd);
```

- **Progress law**: every `yew_input_next` either returns false or consumes
  ≥ 1 byte. No state both returns true and advances nothing — that shape
  is every input-loop hang, and the fuzzer asserts it directly.
- Time is injected exactly as in `tty.c`: no clock syscalls here, so unit
  tests drive the ESC deadline with a fake clock. The byte buffer *is* the
  event queue — the loop's typeahead coalescing drains `yew_input_next`
  until false before rendering, so a `Vec(Key)` would only duplicate `buf`.

### 3. The state machine

```
GROUND --0x1b--> ESC          (arm deadline iff the buffer is now empty)
GROUND --0x00..0x1f,0x7f--> C0 key (§4)   --0x20..0x7e--> ASCII text key
GROUND --0x80..0xff--> UTF8 (s02 streaming decoder)

ESC --'['--> CSI   --'O'--> SS3   --']','P','_','^','X'--> STRING (§10)
ESC --0x1b--> emit Escape, stay in ESC
ESC --other--> that byte's GROUND key with mods |= ALT (§5)

CSI --'0'..'9'--> par[npar] (saturate 65535)   --';'--> npar++
CSI --':'--> subparam        --'?','>','<','='--> priv (position 0 ONLY)
CSI --0x20..0x2f--> inter    --0x40..0x7e--> dispatch (§6)
CSI --anything else--> abort, drop, GROUND
SS3 --0x40..0x7e--> dispatch (§6)
```

- `priv` is legal only at parameter position 0; `CSI 1?A` is malformed and
  dropped. Pitfall: accepting a private marker mid-parameters lets a fuzzed
  stream reach the mouse dispatch with garbage coordinates.
- Parameters **saturate**, never wrap: `CSI 99999999999999C` clamps to
  65535 and is dropped as an oversized CUF-shape. Wraparound turns a huge
  count into a plausible small one — silently wrong beats nothing at all.

### 4. Legacy C0 and single-byte encodings

| Byte | Key | Note |
|---|---|---|
| `0x00` | `' '` + CTRL | Ctrl+Space |
| `0x01`–`0x07` | `'a'+b-1` + CTRL | |
| `0x08` | `BACKSPACE` + CTRL | plain Backspace sends `0x7f` |
| `0x09` | `TAB` | Ctrl+I is indistinguishable without kitty — that is flag 1's job |
| `0x0a` | `'j'` + CTRL | not Enter: `ICRNL` is cleared in s03 §2 |
| `0x0b`–`0x0c` | `'k'`,`'l'` + CTRL | |
| `0x0d` | `ENTER` | Enter is CR because `ICRNL` is off |
| `0x0e`–`0x1a` | `'n'`..`'z'` + CTRL | `0x1a` is Ctrl+Z, a key — `ISIG` is off |
| `0x1b` | `ESCAPE` or sequence intro | §5 |
| `0x1c`–`0x1f` | `'\'`,`']'`,`'^'`,`'_'` + CTRL | |
| `0x20`–`0x7e` | text key; `code` = `text` = the byte | |
| `0x7f` | `BACKSPACE` | |
| `0x80`–`0xff` | UTF-8 → text key, `code` = codepoint | invalid bytes follow the s02 lossless policy: logged, never a `Key` |

Ctrl+letter normalizes to the **lowercase** codepoint. Ctrl+Shift+A is
`0x01` on legacy terminals — indistinguishable from Ctrl+A — so the default
keymap binds no `ctrl+shift+<letter>`; noted in `input.h`, restated in s13.

### 5. The lone-ESC problem

`0x1b` is simultaneously the Escape key, the Alt prefix, and the intro byte
of ~40 sequences. Nothing resolves it from the stream alone: `ESC [ A` is
Up, and a user pressing Escape, `[`, `A` produces identical bytes.

1. **Bounded timeout, only when the buffer is empty.** If bytes already
   follow the ESC, decide immediately — no wait, no latency. Only when the
   ESC is the last byte do we arm `deadline = now_ms +
   YEW_ESC_TIMEOUT_MS` (default **25 ms**) and return false;
   `yew_input_deadline()` hands that to the poll loop. On expiry: emit
   `ESCAPE`, GROUND. On expiry mid-CSI (a truncated sequence): emit
   `ESCAPE`, then reparse the remainder from GROUND.
2. **The kitty escape hatch.** With the disambiguate flag (§7) Escape
   arrives as `CSI 27u`, so a bare `0x1b` can only start a sequence. When
   kitty is negotiated the timeout is **never armed** and Escape has zero
   latency. This is the strongest single argument for the protocol.

- Pitfall (the number): below ~20 ms a function key split across two reads
  on a loaded ssh link decodes as Escape plus garbage; above ~50 ms Escape
  feels mushy in a mode-switching-heavy editor. 25 ms, with kitty as the
  real answer.
- Pitfall (tests): every unit test and the fuzzer pass explicit `now_ms`
  (the fuzzer a monotonically increasing counter) so timeout paths are
  exercised rather than accidentally skipped.

### 6. CSI and SS3 dispatch

Modifier parameter: `CSI 1 ; <m> <final>` and `CSI <n> ; <m> ~`, where
**`m - 1` is the `YewMods` bitmask**; absent `m` means 1 means none.

| Seq | m−1 | Mods | Seq | m−1 | Mods |
|---|---|---|---|---|---|
| `CSI C` | 0 | — | `CSI 1;7C` | 6 | Ctrl+Alt |
| `CSI 1;2C` | 1 | Shift | `CSI 1;8C` | 7 | Ctrl+Alt+Shift |
| `CSI 1;3C` | 2 | Alt | `CSI 1;9C` | 8 | Super |
| `CSI 1;4C` | 3 | Shift+Alt | `CSI 1;17C` | 16 | Hyper |
| `CSI 1;5C` | 4 | **Ctrl** | `CSI 1;33C` | 32 | Meta |
| `CSI 1;6C` | 5 | Ctrl+Shift | `CSI 1;65C` / `;129C` | 64 / 128 | Caps / Num (kitty) |

| Final | CSI | SS3 | Key |
|---|---|---|---|
| `A B C D` | `CSI [1;m] X` | `ESC O X` | Up Down Right Left |
| `E` | `CSI 1;m E` | `ESC O E` | Begin (keypad 5) |
| `F` `H` | `CSI 1;m X` | `ESC O X` | End, Home |
| `P Q R S` | `CSI 1;m X` | `ESC O X` | F1 F2 F3 F4 |
| `Z` | `CSI Z` | — | `TAB` + SHIFT |
| `u` | `CSI …u` | — | kitty key (§7) |
| `~` | `CSI n[;m]~` | — | table below |
| `M` `m` | `CSI < …` | — | SGR mouse (§9); bare `CSI M` = X10 mouse |
| `I` `O` | `CSI I` / `CSI O` | — | focus in / out |
| `M` `X` `j`–`y` | — | `ESC O X` | KP_ENTER, KP_EQUAL, and keypad app mode: `j`=KP_MULTIPLY `k`=KP_ADD `l`=KP_SEPARATOR `m`=KP_SUBTRACT `n`=KP_DECIMAL `o`=KP_DIVIDE `p`–`y`=KP_0–KP_9 |

`CSI <n>[;m]~`:

| n | Key | n | Key | n | Key | n | Key |
|---|---|---|---|---|---|---|---|
| 1 | Home | 6 | PageDown | 17–21 | F6–F10 | 28,29 | F15,F16 |
| 2 | Insert | 7 | Home | 23,24 | F11,F12 | 31–34 | F17–F20 |
| 3 | Delete | 8 | End | 25,26 | F13,F14 | 200 | paste begin |
| 4 | End | 11–15 | F1–F5 | | | 201 | paste end |
| 5 | PageUp | | | | | | |

Gaps (9, 10, 16, 22, 27, 30) are historical holes in the DEC table; they
are dropped, not guessed.

- **Pitfall — `CSI R` is ambiguous.** `CSI <row>;<col>R` is the DSR
  cursor-position report; `CSI 1;<m>R` is modified F3. We never send DSR
  (s03's probe set is DA1 / DECRQM / kitty-query only), so the rule is:
  `priv == 0 && npar == 2 && par[0] != 1` → drop as a stray CPR, log once;
  anything else with final `R` is F3. Without it a nested program's reply
  becomes a phantom F3.
- **Pitfall — `CSI O` vs SS3.** `ESC [ O` is focus-out; `ESC O` is SS3. One
  byte apart, unrelated meanings; kept apart structurally, pinned by a
  fixture containing both.
- Unknown finals, unknown `~` numbers, unknown private markers: consumed to
  the final byte and **dropped**, `dropped++`, logged once per shape.
  Never a `Key`, never a resync failure — which is what makes a Sprint 3
  probe reply arriving after the caps deadline harmless.

### 7. Kitty keyboard protocol

Negotiation: s03's probe already sent `CSI ? u` and recorded
`caps.kitty_kbd` / `caps.kitty_flags` — no re-query. If `kitty_kbd`,
`yew_input_enable` pushes `CSI > 21 u`; s03's restore blob pops with
`CSI < u` on every exit path including the crash handler. If not, the
legacy tables carry the whole load: there is no degraded middle state.

| Bit | Flag | Ask | Why |
|---|---|---|---|
| 1 | Disambiguate escape codes | **yes** | Kills the §5 timeout; Ctrl+I ≠ Tab, Ctrl+M ≠ Enter; Alt+key stops being a guess |
| 2 | Report event types | no | Release/repeat on every keystroke; nothing in 1.0 consumes them and every consumer would need a filter. Parsed defensively anyway (Sprint 54 plugins may push flags) |
| 4 | Report alternate keys | **yes** | `shifted-key` + `base-layout-key`: bindings by physical position — the difference between a usable and unusable modal editor on AZERTY |
| 8 | Report all keys as escape codes | no | Routes plain text through `CSI u`, making every character a sequence parse and putting IME/dead-key output at the mercy of the terminal. Flag 1 already buys the disambiguation |
| 16 | Report associated text | **yes** | Supplies `text[]`; without it flag 1's events have `code` but nothing insertable |

Requested value: **21** (`1|4|16`). This table lives in `input.h` as a
comment — the next reader will otherwise "helpfully" add flag 8.

Encoding: `CSI <key>[:<shifted>[:<base>]][;<mods>[:<event>]][;<text-cp>[:…]] u`

| Example | Meaning |
|---|---|
| `CSI 97u` | `a` — `code=97`, `text="a"` |
| `CSI 97:65;2u` | Shift+a — `code=97`, `mods=SHIFT`, shifted-key 65 |
| `CSI 97;5u` | Ctrl+a — no text |
| `CSI 27u` / `13u` / `9u` / `127u` | Escape / Enter / Tab / Backspace |
| `CSI 1;5C` | Ctrl+Right — functional keys keep legacy finals |
| `CSI 57414u` | KP_ENTER |
| `CSI 233;;233u` | `é` via a dead key: text codepoint 233 |
| `CSI 97;1:3u` | `a` release (only if flag 2 were pushed) |

- `code` takes the **base-layout key** when the third subfield is present,
  else the first field — bindings must survive a layout switch.
- `text[]` is built by UTF-8-encoding the third parameter's *codepoint*
  list. Pitfall: they are codepoints, not bytes; encoding them as Latin-1
  is the classic bug and turns `é` into mojibake.
- Functional codes: `57358` CapsLock, `57359` ScrollLock, `57360` NumLock,
  `57361` PrintScreen, `57362` Pause, `57363` Menu, `57376`–`57398` F13–F35
  (note the gap at `57364`–`57375` — do not compute F13 as 57364),
  `57399`–`57408` KP_0–KP_9, `57409`–`57416` KP_DECIMAL DIVIDE MULTIPLY
  SUBTRACT ADD ENTER EQUAL SEPARATOR, `57417`–`57427` KP_LEFT RIGHT UP DOWN
  PAGE_UP PAGE_DOWN HOME END INSERT DELETE BEGIN, `57441`–`57452` the
  modifier keys themselves. Private-use codes not in the table are dropped
  — never `YEW_KEY_BASE + garbage`.
- Pitfall (SIGCONT): a `su`/`ssh` run while we were stopped may have reset
  the stack. s03's `'C'` byte makes the loop call `yew_input_enable` again;
  the surplus pop is harmless because we leave the alternate screen too.

### 8. Bracketed paste (2004) — and the security rule

```c
static const char YEW_IN_ENABLE[] =
    "\x1b[?2004h"   /* bracketed paste                                   */
    "\x1b[?1002h"   /* button-event mouse tracking (press/release/drag)  */
    "\x1b[?1006h"   /* SGR mouse encoding — must follow 1002 (§9)        */
    "\x1b[?1004h";  /* focus in/out reporting                            */
/* then, iff caps.kitty_kbd:  "\x1b[>21u"                                */
```

Framing: `CSI 200~` … payload … `CSI 201~`.

**The rule: bytes between the markers are TEXT — never decoded as keys,
never dispatched, never matched against a keymap.** Enforced structurally,
not by policy: once `state == PASTE` the only exits are the terminator and
EOF, and the only events emitted are `PASTE_DATA`/`PASTE_END`. There is no
code path from `PASTE` to `emit_key`. This is a security property: the
whole point of bracketed paste is that clipboard content cannot execute
itself by containing `:!rm -rf /` or a `q` that replays a macro.

- **Chunking**: `PASTE_BEGIN`, then `PASTE_DATA` events of ≤ 4096 bytes,
  then `PASTE_END`. The payload is never fully buffered — a 200 MB paste
  costs 4 KiB of parser memory. Chunk bytes are valid until the next
  `yew_input_next`.
- **Pitfall — the terminator straddles a read.** Hold back the last
  `sizeof("\x1b[201~")-1 == 6` bytes unless at EOF; emitting them early
  splits the terminator across a chunk and the paste never ends.
- **Pitfall — the payload is not scrubbed.** Control bytes, NULs, invalid
  UTF-8 and embedded `\x1b` pass through verbatim; the *consumer* (Sprint
  14) decides policy under the s02 lossless and s08 no-corruption rules. A
  parser that helpfully strips control characters mangles binary pastes.
- **Pitfall — a hostile clipboard containing `\x1b[201~`** ends the paste
  early and the remainder arrives as keys. We cannot unframe what the
  terminal already framed, so we bound the blast radius: paste bytes always
  become text, and Sprint 14 makes a paste one undo transaction — worst
  case is visible, reversible insertion, never silent execution. `input.h`
  says so explicitly, so no later sprint invents a "run pasted commands"
  convenience.
- Unterminated paste at EOF: `yew_input_eof` emits the tail then a
  synthetic `PASTE_END`. An unclosed paste must not swallow shutdown.

### 9. SGR mouse (1006) and focus (1004)

`CSI < <Cb> ; <Cx> ; <Cy> M` = press/motion, `… m` = release. `Cx`/`Cy` are
**1-based**; the parser subtracts one exactly once, here, so everything
downstream is in `Grid` coordinates.

| `Cb` | Final | Event / button |
|---|---|---|
| 0 / 1 / 2 | `M` | press left / middle / right |
| 0 / 1 / 2 | `m` | release left / middle / right |
| 32 / 33 / 34 | `M` | drag with left / middle / right held |
| 35 | `M` | motion, no button — needs 1003, which we never enable → dropped |
| 64 / 65 | `M` | wheel up / down |
| 66 / 67 | `M` | wheel left / right |
| 128 / 129 | `M` | buttons 8 / 9 (back / forward) |

Modifier bits OR into `Cb`: `+4` Shift, `+8` Alt, `+16` Ctrl — so `Cb=48`
with final `M` is "drag with left held, Ctrl down". Decode order: strip
4/8/16 into `mods`; bit 6 selects the wheel set, bit 7 the extended set,
bit 5 marks motion, the low two bits select the button.

- **Pitfall — wheel events have no release.** A drag state machine keyed on
  "press without release" hangs on the first scroll; wheels are impulses
  and Sprint 27's router must treat them so.
- **Pitfall — legacy X10 fallback.** A terminal that accepted 1002 but not
  1006 sends `CSI M <Cb+32> <Cx+32> <Cy+32>` as three raw bytes. We must
  consume exactly 3 or lose stream sync permanently, so we decode them
  (subtract 32, clamp at 223 where the encoding runs out) and log once.
  Consuming a variable count here is the worst failure mode in the module.
- **Pitfall — enable order.** 1006 is an *encoding* selector, not a
  tracking mode: without 1002 it does nothing, and some terminals ignore it
  if it arrives first. The blob's order is load-bearing and byte-pinned.

Focus: `CSI I` → `FOCUS_IN`, `CSI O` → `FOCUS_OUT`. Later consumers: pause
cursor blink, cancel AI shadow requests (Sprint 49), refresh the git status
cache (Sprint 51). Pitfall: focus-out can arrive while a modal prompt owns
input — it is an event, never a mode change, and must close nothing.

### 10. String sequences and hostile input

`OSC`/`DCS`/`APC`/`PM`/`SOS` bodies are consumed to `ESC \` (ST) or `BEL`,
then dropped, with the body capped at **8 KiB** — past that, drop to GROUND
and log. An unterminated OSC is otherwise unbounded memory growth driven by
whatever is on the other end of the pty (a `cat` of a binary file suffices).
The same reasoning caps `buf` at `YEW_IN_MAX_BUFFER` (1 MiB): a 1 MiB
sequence is not a keystroke. `buf` compacts when `rd > 4096`, not per event
— per-event compaction makes a 200 MB paste quadratic.

### 11. Defer

- Keymaps, chord/prefix timeouts, dispatch, the `"ed.*"` registry → Sprint
  13. `input.c` has no notion of a binding.
- What a click *means* → Sprint 27 (click-to-focus previewed in Sprint 22);
  mouse events are produced and, until then, discarded by the loop.
- Paste insertion and transactions → Sprint 14. OSC 52 clipboard *replies*
  → Sprint 12 (OSC bodies are dropped today, which is correct until then).
- Kitty graphics / DCS responses: never; they stay in the dropped bucket.
- Any editor action reachable from a key still hard-errors naming Sprint 14.

## Testing Strategy

- **Unit** (`tests/unit/test_input.c`): table-driven `{bytes, expected
  Key[]}` fixtures covering every row of §4, §6 (all finals, all `~`
  numbers, all 13 modifier values), §7 (each example plus alternate-key and
  text-codepoint forms), §8 (paste with and without a straddling
  terminator, embedded ESC, embedded `CSI 201~`), §9 (every `Cb` row, both
  finals, X10 fallback, the 1-based→0-based conversion) and focus. ≥ 120
  vectors.
- **Chunking independence** (invariant 5): every fixture re-fed split at
  every byte boundary and in 1-byte dribbles → identical `Key` sequence and
  `dropped` count. The highest-value test in the sprint.
- **Lone-ESC matrix**: bare `ESC` + expiry → Escape; `ESC` then `[A` before
  expiry → Up with the deadline disarmed; `ESC i` in one feed → Alt+i;
  bare `ESC` with `kitty_kbd` → no deadline, `yew_input_deadline() == -1`;
  truncated `CSI 1;` + expiry → Escape then two text keys.
- **Enable blob** byte-compared against §8's exact string, with and without
  kitty — Sprint 6's VT asserts the same bytes from the other side.
- **Fuzz** (`tests/fuzz/fuzz_input.c`; `make fuzz` becomes real): random
  streams seeded from the unit fixtures, asserting (1) progress — never
  true without consuming; (2) termination — N bytes yield ≤ N events;
  (3) `ntext ≤ 15` and `text` is valid UTF-8; (4) `code < 0x110000` or a
  defined `YEW_KEY_*`; (5) `kind` is a defined `EvKind` and mouse
  `col`/`row` never read from an unset param; (6) `buf.cap + paste.cap ≤
  fed + YEW_IN_MAX_BUFFER`; (7) **no `Key` is ever emitted while
  `in_paste`**; (8) split-invariance. ASan/UBSan, ≥ 60 s in CI.
- **Regression fixtures** (`tests/unit/fixtures/input/`): CPR-vs-F3;
  `CSI O` immediately after `ESC O P`; a late s03 probe reply; a mouse
  report split across four reads; a 5 MB paste (chunk count and peak memory
  asserted).
- No pty tests yet — Sprint 6 covers this module retroactively by scripting
  real key bytes into a forked child under both kitty and legacy profiles.

## Definition of Done

1. `make && make test` green on gcc and clang under the locked flags;
   ASan/UBSan and valgrind lanes clean.
2. `_Static_assert(sizeof(Key) == 32)` holds; `input.h` documents the
   `YEW_KEY_BASE` split and the `code`-vs-`text` divergence.
3. ≥ 120 fixture vectors covering every row of §4, §6, §7, §8, §9; ≥ 300
   assertions in `test_input.c`.
4. Chunking independence passes for splits at **every** byte offset of
   every fixture — identical events and identical `dropped` counts.
5. Lone-ESC matrix green, including the kitty case asserting
   `yew_input_deadline() == -1` and that no deadline is ever armed.
6. Enable blob byte-compared against §8; the kitty push is exactly
   `\x1b[>21u` and appears only when `caps.kitty_kbd`.
7. `make fuzz` runs `fuzz_input` 60 s with zero crashes, hangs or assertion
   failures under ASan/UBSan; all 8 properties are asserts, not comments.
8. The security rule is proven mechanically: no `emit_key` call is
   reachable from the paste state (grep + the fuzz property).
9. 1 000 well-formed-but-unknown CSI sequences yield 0 events and
   `dropped == 1000`.
10. Progress law: 64 KiB of random bytes drains in ≤ 65536 iterations
    (asserted).
11. `grep -rn 'read(\|poll(' src/term/input.c` → empty. The parser does no
    I/O; the event loop owns every syscall.
12. Mouse coordinates leaving `input.c` are 0-based —
    `CSI <0;1;1M` must yield `col=0,row=0`.
