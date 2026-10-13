# Sprint 13: Keymap Engine and the Named-Command Registry

## Prerequisites

- Sprint 0 — `Arena`, `Vec(T)`, `Strmap` (insertion-ordered), `Interner`,
  `Bytebuf`, `yew_sort_stable`, `yew_log`, `YEW_BUG`, exit-code contract.
- Sprint 1 — unit harness (explicit registry table), `make test`,
  `scripts/bans.sh`, CI lanes (gcc/clang/ASan/UBSan/valgrind/determinism).
- Sprint 4 — normalized key events. **Assumption beyond the index entry**
  (its entry says "normalized KeyEvent model" without a shape): `src/term/
  input.h` exposes

  ```c
  typedef struct Key {
      u32 code;      /* YEW_K_* for named keys; else a Unicode codepoint  */
      u16 mods;      /* YEW_MOD_{CTRL,ALT,SHIFT,SUPER} bitmask            */
      u8  kind;      /* YEW_KEY_{PRESS,REPEAT,RELEASE}                    */
      u8  ntext;     /* UTF-8 bytes in text[]; 0 for non-text keys        */
      u8  text[8];
  } Key;
  ```

  If Sprint 4 landed a different field set, this sprint adds the adapter in
  `src/term/input.c`; nothing below may parse bytes (architecture: *"the
  keymap never sees raw bytes"*).
- Binding: `00-decisions.md` (Modes row; invariants 3, 5, 9, 10),
  `01-architecture.md` §Modal engine — *"Every action is a named editor
  command … One dispatch surface, no parallel paths."*

## Goals

Land the two surfaces every later interactive sprint stands on:
`src/edit/cmd.c` — the **named-command registry** (`"ed.*"`, one descriptor
table, one `yew_cmd_invoke` choke point serving keys, E mode, Fletch, and
the macro recorder) and `src/edit/keymap.c` — key-trie keymaps with stacked
mode/user/plugin layers, the chord/ambiguity timeout state machine, and
count prefixes. Binding tables are **data** (`BindRow[]` C arrays now,
Fletch `bind()` rows in Sprint 36 — same builder, so the swap is
mechanical). Mode *behavior* is Sprint 14 (L/I), 16 (W/B), 17 (H), 18 (E),
52 (F); this sprint lands only `Mode` identity + the layer plumbing. The
recorder tap point exists and is NULL until Sprint 35; `ed.macro.*`
hard-errors naming it. No rendering: the pending-chord hint is state here,
pixels in Sprint 15.

## Deliverables

### 1. Canonical key identity — `src/edit/keymap.h`

```c
typedef struct { u64 v; } KeyId;      /* (code << 16) | canonical mods   */
#define YEW_CHORD_MAX 8               /* keys per bindable sequence      */
```

`KeyId yew_keyid(Key k)` is the ONLY key→lookup-token function. Rules,
pinned (each is a unit-test row):

| Input | `KeyId` | Why |
|---|---|---|
| `a` | `('a' << 16)` | plain text key |
| `A` (shift held) | `('A' << 16)`, **no SHIFT bit** | shift is spent producing the codepoint; `S-a` would double-count |
| `ctrl+a` | `('a' << 16) \| CTRL` | ctrl-letters normalize to *lowercase* + CTRL, legacy 0x01–0x1A included |
| `shift+Tab` | `(YEW_K_TAB << 16) \| SHIFT` | shift IS a modifier for named keys — no codepoint absorbs it |
| `alt+left` | `(YEW_K_LEFT << 16) \| ALT` | ESC-prefix or kitty form — Sprint 4 already collapsed them |
| `YEW_KEY_REPEAT` | same as PRESS | key-repeat drives motion; never a separate binding |
| `YEW_KEY_RELEASE` | **dropped before lookup** | kitty reports them; no 1.0 binding consumes them |

**Pitfall — mods on text keys.** A terminal that reports `SHIFT` alongside
`A` and one that does not must produce the same `KeyId`, or a binding works
on kitty and dies on xterm. Canonicalization masks SHIFT off whenever
`k.ntext > 0` and the text is not a control character.

### 2. Sequence text format and its round-trip law

```c
u32  yew_key_parse_seq(const char *s, KeyId *out, u32 max);  /* 0 = error */
void yew_key_format_seq(const KeyId *seq, u32 n, Bytebuf *out);
```

Grammar: space-separated key atoms. An atom is a bare printable character
(`a`, `!`, `4`), or `<name>`, optionally prefixed by modifiers `C-` (ctrl),
`A-` (alt), `S-` (shift), `M-` (super), in that canonical order.

Named atoms: `<left> <right> <up> <down>`, `<esc> <cr> <tab> <bs> <del>
<space>`, `<home> <end> <pgup> <pgdn> <ins>`, `<f1>`…`<f12>`, and `<lt>`
for a literal `<` (the only escape). Examples: `"g g"`, `"q !"`, `"C-r"`,
`"A-<left>"`, `"C-A-<up>"`.

**Round-trip law (unit-gated):** `format(parse(s)) == canonical(s)` for
every atom and for every default table row. Why it matters: the same parser
serves the C tables, Fletch's `bind()` (Sprint 36), E-mode `:map` listing
(Sprint 18), the which-key hint (Sprint 15), and the recorder's key naming
(Sprint 35). One grammar, five consumers, zero re-implementations.

### 3. The named-command registry — `src/edit/cmd.h`, `src/edit/cmd.c`

**Naming grammar (pinned).**

```
name    = "ed" "." ( app_verb | domain "." tail )
tail    = seg [ "." seg [ "." seg ] ]        ; total 3–4 segments
seg     = [a-z] [a-z0-9_]*                   ; ≤ 16 bytes
app_verb= "quit" | "quit_force" | "suspend" | "redraw" | "repeat" | "nop"
```

Two-segment names are exhausted by the closed `app_verb` list (this is why
Sprint 23 may write `ed.quit`). Everything else is `ed.<domain>.<object>.<verb>`
or `ed.<domain>.<verb>`. Domains are a closed set — adding one is a review
event, like adding a dependency:

| Domain | Owns | Examples |
|---|---|---|
| `move` | cursor motion, no text change | `ed.move.line.home`, `ed.move.unit.next` |
| `edit` | buffer mutation | `ed.edit.insert.text`, `ed.edit.undo` |
| `mode` | mode transitions | `ed.mode.enter`, `ed.mode.escape` |
| `sel` `cursor` | selections, multi-cursor set | `ed.sel.expand`, `ed.cursor.add.below` |
| `view` `ui` | viewport, chrome | `ed.view.center`, `ed.ui.message_expand` |
| `file` `buf` `tab` `group` `pane` `win` | files and workspace objects | `ed.file.save`, `ed.tab.goto`, `ed.pane.split_h` |
| `reg` `search` `macro` `job` `git` `lsp` `ai` `plug` | later campaigns | `ed.macro.record`, `ed.git.stage` |

Verbs come from a closed list wherever one fits: `home end next prev up
down left right goto insert delete replace yank paste toggle open close
save new enter leave grow shrink expand contract list reload cancel`.
Segments are interned (Sprint 0 `Interner`) → the registry key is a `u32`.

```c
typedef struct { u32 v; } CmdId;      /* 0 = YEW_CMD_NONE                */

typedef enum { YEW_SRC_KEY, YEW_SRC_CMDLINE, YEW_SRC_FLETCH,
               YEW_SRC_REPLAY, YEW_SRC_MOUSE, YEW_SRC_TEST } CmdSource;

typedef struct CmdCtx {
    Ed         *ed;
    Win        *win;          /* NULL unless YEW_CMD_NEEDS_WIN           */
    u32         count;        /* 1 when absent                           */
    bool        count_given;  /* distinguishes "1" from "no count"       */
    i64         iarg;         /* numeric argument (binding/E-mode/Fletch)*/
    const char *sarg; u32 sarg_len;  /* text argument; NOT NUL-required  */
    CmdSource   source;
} CmdCtx;

typedef enum { YEW_CMD_OK = 0, YEW_CMD_ERR_ARG, YEW_CMD_ERR_STATE,
               YEW_CMD_ERR_IO, YEW_CMD_ERR_DEFERRED } CmdStatus;

typedef CmdStatus (*CmdFn)(CmdCtx *cx);

typedef enum { YEW_ARITY_NONE, YEW_ARITY_INT, YEW_ARITY_STR,
               YEW_ARITY_OPT_INT, YEW_ARITY_OPT_STR } CmdArity;

enum {                                  /* CmdDesc.flags                 */
    YEW_CMD_REPEATABLE     = 1u << 0,   /* dispatcher runs it count times*/
    YEW_CMD_TAKES_COUNT    = 1u << 1,   /* command consumes count itself */
    YEW_CMD_RECORDABLE     = 1u << 2,   /* recorder emits it (Sprint 35) */
    YEW_CMD_NEEDS_WIN      = 1u << 3,   /* refuses without a focused Win */
    YEW_CMD_CHANGES_BUFFER = 1u << 4,   /* opens an undo transaction     */
    YEW_CMD_PROMPTS        = 1u << 5,   /* pushes a modal keymap layer   */
    YEW_CMD_DEFERRED       = 1u << 6,   /* hard-errors naming its sprint */
};

typedef struct CmdDesc {
    const char *name;  CmdFn fn;  u8 arity;  u32 flags;
    const char *help;               /* one line: E-mode + `yew --help-cmds` */
} CmdDesc;

CmdId     yew_cmd_register(const CmdDesc *d);       /* YEW_BUG on dup    */
CmdId     yew_cmd_lookup(const char *name, u32 len);/* 0 = unknown       */
const CmdDesc *yew_cmd_desc(CmdId id);              /* NULL if unknown   */
CmdStatus yew_cmd_invoke(CmdId id, CmdCtx *cx);     /* THE choke point   */
u32       yew_cmd_count(void);
const CmdDesc *yew_cmd_at(u32 i);                   /* insertion order   */
```

- REPEATABLE and TAKES_COUNT are mutually exclusive (checked in
  `yew_cmd_register`) — the classic ambiguity: `3 ed.move.line.down` runs
  three times; `3 ed.tab.goto` switches to tab 3. Neither flag = the count
  is ignored silently.
- `YEW_CMD_DEFERRED` descriptors carry the sprint number in `help` and
  render `command not implemented yet: <name> lands in Sprint N`
  (invariant 3) — how `ed.mode.enter W` behaves until Sprint 16: the name
  exists, it is discoverable, it never silently no-ops.
- The registry is insertion-ordered (`Strmap` over an append-only array):
  completion and `--help-cmds` output are deterministic (invariant 5).

### 4. The one-dispatch-surface doctrine

`yew_cmd_invoke` is the only path from an intent to an effect: keys
(`dispatch.c`), E mode (`cmdline.c`, Sprint 18), Fletch (`flapi.c`, Sprint
34), mouse (region hit, Sprint 22) all funnel in, and the recorder (Sprint
35) taps the funnel rather than any of the inputs.

```c
CmdStatus yew_cmd_invoke(CmdId id, CmdCtx *cx) {
    const CmdDesc *d = yew_cmd_desc(id);
    if (!d) return YEW_CMD_ERR_ARG;
    if ((d->flags & YEW_CMD_NEEDS_WIN) && !cx->win)
        return yew_cmd_fail(cx, "no window");
    if (d->flags & YEW_CMD_DEFERRED)  return yew_cmd_deferred(cx, d);
    /* Tap BEFORE execution and AFTER argument resolution: the recorder
     * must capture what actually ran, not what a key looked like, so a
     * rebound key replays as its command (invariant 10).               */
    if (g_record_tap && (d->flags & YEW_CMD_RECORDABLE))
        g_record_tap(id, cx);
    u32 n = (d->flags & YEW_CMD_REPEATABLE) ? cx->count : 1;
    CmdStatus st = YEW_CMD_OK;
    for (u32 i = 0; i < n && st == YEW_CMD_OK; i++) st = d->fn(cx);
    return st;
}
```

**Review-rejection rule.** No feature may be wired directly to a key. A
function reachable from a keymap is `static CmdStatus cmd_<name>(CmdCtx *)`
in its owning module, referenced exactly twice in the tree: its definition
and its `CmdDesc` row. Why the teeth: a key wired straight to a function is
invisible to E mode, to Fletch, and to the recorder — permanently
unscriptable and unrecordable, and every later campaign inherits the hole.
`scripts/check-cmd-dispatch.sh` (run by `make test`) enforces it:

```sh
# every cmd_* symbol appears exactly twice (definition + CmdDesc row)
grep -rhon '\bcmd_[a-z0-9_]*\b' src/ | sort | uniq -c |
  awk '$1 != 2 { print; bad=1 } END { exit bad }'
```

### 5. Key trie and layers — `src/edit/keymap.c`

```c
typedef struct { CmdId cmd; i64 iarg; const char *sarg; } Binding;
typedef struct { u64 key; u32 child; } KeyEdge;     /* sorted by key     */
typedef struct { u32 edge_lo, edge_n, bind; } KeyNode;  /* bind 0 = none */

typedef struct Keymap {
    Vec(KeyNode) nodes;      /* nodes[0] = root                          */
    Vec(KeyEdge) edges;      /* grouped per node, ascending by key       */
    Vec(Binding) binds;      /* binds[0] reserved                        */
    const char  *name;       /* "mode:L", "user", "plug:vimish", "prompt"*/
} Keymap;

typedef struct BindRow {                 /* THE data format             */
    const char *seq;      /* "g g", "A-<left>", "q !"                    */
    const char *cmd;      /* "ed.move.buf.home"                          */
    i64         iarg;
    const char *sarg;
} BindRow;

bool yew_keymap_build(Keymap *km, const char *name,
                      const BindRow *rows, u32 n);   /* frozen on return */
```

- Build = parse every `seq`, resolve every `cmd` through
  `yew_cmd_lookup`, insert into a scratch tree, then flatten to the two
  sorted arrays. Lookup is binary search over an edge window: no hashing,
  deterministic iteration for `:map` listing.
- **Tables are frozen; mutation is rebuild.** `bind()` (Sprint 36) appends
  to a `Vec(BindRow)` and rebuilds — a few hundred rows, microseconds.
  Why: an incrementally-mutated trie needs edge insertion, node splitting,
  and its own invalidation story for a chord in flight. Rebuild has one
  code path and makes the C-array → Fletch swap a change of *row source*,
  nothing else.
- Build-time validation (returns false; the caller hard-errors with
  file:row context): 1. unparseable `seq`; 2. unknown `cmd`; 3. duplicate
  `seq` in one table; 4. arity mismatch; 5. **`<esc>` used as a prefix** —
  Esc must resolve instantly in every layer or the modal feel dies under a
  500 ms wait; 6. sequence longer than `YEW_CHORD_MAX`.

**Layer stack and precedence.**

```c
typedef struct KeyStack { const Keymap *l[YEW_LAYER_MAX]; u32 n; } KeyStack;
/* index 0 = mode layer, 1 = user layer, 2.. = plugin layers in load order,
 * top = transient modal layers (prompts, pickers) pushed by PROMPTS cmds */
```

Resolution walks the stack **top-down** (later layers shadow earlier ones,
exactly as Sprint 54 assumes) and the first layer with *any* match —
terminal or prefix — **owns the sequence to completion**. Lower layers are
not consulted again until the chord resolves or is abandoned.

> Why ownership instead of per-key merging: with merging, a plugin binding
> `g` and a mode binding `g g` behave in a way that depends on load order
> and that no user can debug. Ownership is one sentence ("the plugin layer
> took `g`") and `:map g` prints the owning layer's name. Plugin layers sit
> *above* the user layer because they are opt-in and removable —
> last-loaded-wins is the only rule that composes with the dynamic
> enable/disable Sprint 54 needs.

### 6. Chord and ambiguity — the timeout state machine

```c
typedef struct Chord {
    KeyId seq[YEW_CHORD_MAX]; u8 n;
    i32   layer;         /* owning layer index; -1 while empty          */
    u32   count; bool count_given;
    i64   deadline;      /* CLOCK_MONOTONIC ms; 0 = unarmed             */
} Chord;

typedef enum { YEW_MATCH_NONE, YEW_MATCH_PREFIX,
               YEW_MATCH_FULL, YEW_MATCH_FULL_PREFIX } KeyMatch;

void yew_dispatch_key (Ed *ed, Key k, i64 now_ms);   /* src/edit/dispatch.c */
void yew_dispatch_tick(Ed *ed, i64 now_ms);          /* poll timeout path   */
i64  yew_dispatch_deadline(const Ed *ed);            /* -1 = none           */
```

```
                       ┌──────────── key extends seq ────────────┐
IDLE ── key ─→ classify ┤                                        ↓
                       │ FULL          → invoke, reset          PENDING
                       │ PREFIX        → arm deadline ──────────→ (n>0)
                       │ FULL_PREFIX   → arm deadline, remember bind
                       │ NONE          → message "unbound", reset
PENDING ── key that does NOT extend ─→ fire remembered bind (if any),
                                       then RE-FEED the key from IDLE
PENDING ── tick: now >= deadline ────→ fire remembered bind (if any),
                                       else "unbound sequence", reset
```

**The pinned resolution rule** for a bound prefix that is also a complete
binding (`q` = `ed.quit`, `q !` = `ed.quit_force`): the shorter binding
**waits**. It fires on timeout expiry or on the first key that cannot
extend the sequence — never immediately. Alternatives rejected:
firing-immediately makes the longer binding unreachable; requiring an
explicit terminator makes every prefix binding feel like a different
editor. Timeout default **500 ms**, option `chord_timeout_ms` (Sprint 36),
env `YEW_CHORD_TIMEOUT_MS` for tests.

- The non-extending key is **re-fed, never dropped** — `q` then `i` quits
  and then inserts. A dropped key is the bug users report as "yew
  ignores my typing".
- A dead sequence (no complete binding in the chain) drops the whole
  sequence with one message and does **not** re-feed the tail: after `g`
  `z` in a table with only `g g`, replaying `z` alone would run a command
  the user never asked for.
- **Pitfall — the deadline must not fire mid-burst.** `yew_dispatch_key`
  never reads the clock; the loop passes `now_ms` and calls
  `yew_dispatch_tick` only when the input queue is empty (Sprint 14 §2).
  Bursts therefore resolve chords by *key arrival*, and the timer matters
  only once the user actually stopped typing.
- Esc while PENDING cancels the chord and the count, consumes the key, and
  does **not** fall through to `ed.mode.escape` — one Esc, one layer of
  pending state. `Chord` is readable (`yew_dispatch_pending`) for Sprint
  15's which-key hint and Sprint 35's recorder.

### 7. Count prefixes and their propagation

- Digits become a count when the chord is empty, the mode has
  `takes_count` (L/W/B/H — not I, where `4` is text), and the key is
  `1`–`9`, or `0` with a count already in progress.
- `count = count*10 + d`, clamped at `YEW_COUNT_MAX = 99999` with a
  message. **Pitfall:** an unclamped count turns `999999999x` into a
  billion-iteration loop no keystroke can interrupt — latency is a gate
  (invariant 4), so the clamp is not optional.
- `CmdCtx.count`/`count_given` feed the `YEW_CMD_REPEATABLE` loop;
  `YEW_CMD_TAKES_COUNT` commands read `cx->count` themselves. Count resets
  after every dispatch and on Esc — never sticky. The recorder folds it to
  `4>` and Fletch parses `COUNT motion_word` (s28 §3) off the same field.

### 8. Mode identity — `src/edit/mode.h`

```c
typedef enum { YEW_MODE_L, YEW_MODE_W, YEW_MODE_B,
               YEW_MODE_H, YEW_MODE_I, YEW_MODE_E, YEW_MODE_F,
               YEW_MODE__N } Mode;

typedef struct ModeDesc {
    const char *name;        /* "L" … "F"  — statusline chip text        */
    const char *long_name;   /* "line", "word", … — messages, :mode      */
    bool        is_unit;     /* L/W/B: sets granularity                  */
    bool        takes_count; /* digits are counts, not text              */
    u8          layer;       /* index of its Keymap in the mode tables   */
} ModeDesc;

extern const ModeDesc yew_modes[YEW_MODE__N];   /* one table, no code    */
```

`ed.mode.enter` takes a `YEW_ARITY_STR` argument (`"L"`, `"I"`, …);
`ed.mode.escape` is the Esc law (*always* returns to L). Both are
registered here; their *implementations* land in Sprint 14 (`mode.c`) with
W/B/H/E/F entries flagged `YEW_CMD_DEFERRED` until 16/16/17/18/52.

### 9. Default binding tables — `src/edit/keys_default.c`

One file, one array per mode layer, nothing but data:

```c
static const BindRow keys_L[] = {
    { "<esc>",  "ed.mode.escape",    0, NULL },
    { "g g",    "ed.move.buf.home",  0, NULL },   /* chord               */
    { "q",      "ed.quit",           0, NULL },   /* prefix AND complete */
    { "q !",    "ed.quit_force",     0, NULL },
};
```

This sprint ships the skeleton (Esc, `q`/`q !`, `g g`, `ed.nop`) purely to
exercise the engine; the real L and I tables are Sprint 14's Deliverable 4.
`yew_keys_default_install(Ed *ed)` builds every mode layer plus an empty
user layer. **Pitfall:** `sarg` is arena-duplicated at build time, never
aliased into the row — Sprint 36's rows are Fletch-owned and get freed.

### 10. Defer

- Mode behavior, the event loop, rendering → Sprint 14; statusline chip,
  which-key hint, message severities → 15.
- `ed.mode.enter` for W/B/H/E/F → 16/16/17/18/52 (`YEW_CMD_DEFERRED`);
  `ed.macro.*` → 35 (`g_record_tap` stays NULL until then).
- Fletch `bind()` and user config layers → 36; `:map` UI → 18; plugin
  layers → 54 (the stack supports them; nothing pushes one).
- Mouse-sourced dispatch → 22/27 (`YEW_SRC_MOUSE` reserved).

## Testing Strategy

- **Unit** (`test_keyid.c`): every row of §1; a shift-reporting and a
  non-shift-reporting terminal give identical `KeyId` for 16 printable
  keys; legacy `0x01`–`0x1A` map to `C-a`…`C-z`.
- **Unit** (`test_keyseq.c`): parse/format round-trip over the atom corpus
  (all named atoms × all modifier subsets); malformed inputs (`"<>"`,
  `"C-"`, `"<nope>"`, a 9-key sequence) return 0 and never write past
  `max`.
- **Unit** (`test_cmdreg.c`): duplicate registration is `YEW_BUG`; the
  name-grammar validator accepts the §3 examples and rejects `ed`, `ed.`,
  `ed.Move.x`, `ed.move.a.b.c.d`, a 17-byte segment, an off-list domain;
  `yew_cmd_at` order equals registration order across processes;
  REPEATABLE+TAKES_COUNT is rejected.
- **Unit** (`test_keymap.c`): the six build validations each fire;
  binary-search lookup matches a linear oracle over a 300-row table;
  layer ownership (plugin `g` vs mode `g g`); `:map` listing byte-stable.
- **Unit** (`test_chord.c`, fake clock): the `q`/`q !` matrix (timeout →
  `ed.quit`; `!` in time → `ed.quit_force`; `i` in time → quit then
  insert with the `i` **re-fed**); dead-sequence drop; Esc cancel;
  `yew_dispatch_deadline` equals the poll timeout the loop would use.
- **Unit** (`test_count.c`): `4<down>` → one invoke with `count=4`;
  `10<down>` → 10; bare `0` is not a count; `9999999` clamps; reset after
  dispatch and after Esc; I-mode digits are text.
- **Dispatch integrity**: `scripts/check-cmd-dispatch.sh` in `make test`;
  seed a second call site of a `cmd_*` and confirm it fails.
- Lanes: gcc + clang zero-warning, ASan/UBSan, valgrind, determinism.

## Definition of Done

1. `make test` green on gcc and clang, `-std=c11 -pedantic -Wall -Wextra
   -Werror`, ASan/UBSan/valgrind clean.
2. Key-sequence round-trip holds for ≥ 80 atoms and for every row of every
   shipped table.
3. All six `yew_keymap_build` validations fire, including the
   `<esc>`-as-prefix rejection.
4. Chord matrix green (four `q`-family outcomes, dead-sequence drop, Esc
   cancel, re-feed), driven entirely by a fake clock:
   `grep -rn 'clock_gettime\|time(' src/edit/keymap.c src/edit/dispatch.c`
   → empty.
5. Layer ownership proven by a 3-layer test where merging and ownership
   differ; the reported owning-layer name is asserted.
6. All seven §7 count rows tested; `YEW_COUNT_MAX` clamp asserted.
7. Registry determinism: two `--help-cmds` runs byte-identical; ≥ 40
   commands registered, every one with a non-empty `help`.
8. `scripts/check-cmd-dispatch.sh` passes on the tree and fails on a
   seeded second call site.
9. Every DEFERRED command returns `YEW_CMD_ERR_DEFERRED` naming its
   sprint (no silent stubs, invariant 3).
10. Keymap lookup cost: 10⁶ single-key dispatches through a 300-row table
    in < 150 ms on the CI runner (crude ceiling; the keypress→paint gate
    lands in Sprint 14).
11. ≥ 140 assertions across the six new unit files.
12. `src/edit/keys_default.c` holds data only — no function definitions
    besides the single `yew_keys_default_install`.
