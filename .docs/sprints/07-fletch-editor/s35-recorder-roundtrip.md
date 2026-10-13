# Sprint 35: The Macro Recorder and the Round-Trip Law

## Prerequisites

- Sprint 13 — `yew_cmd_invoke` and its **recorder tap**, which s13 wrote
  and left NULL for this sprint:

  ```c
  if (g_record_tap && (d->flags & YEW_CMD_RECORDABLE)) g_record_tap(id, cx);
  ```

  Also `CmdDesc.flags`, `YEW_CMD_{REPEATABLE,TAKES_COUNT,RECORDABLE}`,
  `CmdCtx` (`count`, `count_given`, `iarg`, `sarg`/`sarg_len`, `source`,
  `win`), `CmdSource` incl. `YEW_SRC_REPLAY`, `yew_key_format_seq`,
  `yew_dispatch_pending` (the `Chord` reader s13 exposed *for this sprint*),
  `ed.macro.*` reserved as `YEW_CMD_DEFERRED` naming Sprint 35.
- Sprint 34 — `CmdDesc.word` and `yew_cmd_by_word()` (the CMDWORD half of
  the bijection), `src/fl/motion_tab.c`, `edit{}`/`fl_txn_*`, handles,
  origins, `fl_hook_*`, `ed.run`. **This sprint adds no editor surface** —
  it observes the one that exists and prints it.
- Sprint 28 — `fletch-spec` §3 motion vocabulary (the emitter's entire
  alphabet), §10 transaction law, §14 example. Sprint 29 — the lexer's
  motion token space, `fl_parse`, `fl_ast_dump`. Sprints 30–33 — `FlVm`,
  compile+run, `yew fl`, conformance suite.
- Sprint 10 — `YEW_TXN_MACRO`, `yew_undo_begin/end/abort`, `yew_undo_dump`.
- Sprint 12 — `Registers`, `RegVal` (`YEW_REG_CHARWISE`), `yew_reg_get/set`;
  s12 pinned macro registers as "Sprint 38", and s12's `yew_reg_set`
  choke-point grep gate still holds — the recorder writes registers through
  `ed.reg.set`, not by poking `Registers`.
- Sprint 14 — the event loop's input-burst structure; Sprint 17 — multi-
  cursor as one transaction (`cursor_index`); Sprint 18 — E-mode prompts
  (`YEW_CMD_PROMPTS`), the committed-command model.
- Sprint 1 — unit harness, CI lanes, determinism lane. Sprint 6 — pty
  harness (`PtyCase`, `ptc_keys`, `ptc_snapshot`).
- Binding: `00-decisions.md` **invariant 10** (this sprint is where it stops
  being a promise), invariants 3, 4, 5; `02-fletch.md` requirement 1 and the
  vim comparison table.

## Goals

Land `src/fl/record.c|h` and `tests/roundtrip/`: a recorder that consumes
**resolved command dispatches** (plus a thin key tap for context), emits
terse Fletch source in the frozen §3 vocabulary with run folding and
mode-context annotation, stores recordings in the s12 register file, and
replays them **through the ordinary VM** — there is no macro fast path. The
sprint's centre of gravity is the round-trip law harness: a generator of
random editing sessions, a corpus, and a property test asserting that
replaying a recording's emitted source from the same initial state produces
byte-identical buffer, cursor, and register state, running in CI forever.
Recording *UX* — the statusline indicator, the register browser, the
edit-a-macro flow, the macro library — is Sprint 38; this sprint ships the
minimum commands to record, stop, and replay so the harness has a subject.
`--batch` replay is Sprint 37.

### Implementation amendment (2026-08-10)

The command envelope grew after this sprint was drafted. The binding law for
Sprint 35 is therefore: **every successfully accepted recordable resolved
envelope round-trips**. Portable count, integer/string argument, bang, and
range values are captured. Live `Win *`, explicit cursor identities, and
internal cursor/option plumbing are contextual rather than serializable and
are rejected by recorder preflight before the command executes. A replay uses
the current window and current cursor set. Prompt-internal edits to the
command-line's private window are allowed to execute but are discarded; only
the committed `YEW_SRC_CMDLINE` command is recorded.

The other amendments reflected below are equally binding: emitted recordings
are append-safe IIFE chunks rather than duplicate global declarations; cache
identity is exact register bytes; P5 applies to generated mode-closed sessions;
failure artifacts go under `YEW_RT_TMP`/`build/tmp` for deliberate review; and
the Sprint 36/37 deferrals are tested through E mode and native C integration
tests in this sprint.

**Completion record:** complete on trunk at `30100a6` (2026-08-10). The
closing gates were `make check`, the full 2,000-seed P1–P5 law plus corpus and
count-folding sentinel, GCC/Clang warning-clean builds, focused Clang
ASan/UBSan, recorder fuzz/perf, three E-mode pty goldens, and an independent
strict review with verdict **APPROVE — SHIP**.

## Deliverables

### 1. What is recorded: commands, not keys

```c
typedef struct RecEvent {
    CmdId  cmd;
    u32    count;  bool count_given;
    bool   bang;
    i64    iarg;
    u64    range_lo, range_hi;
    u32    sarg_at, sarg_len;   /* slice of Rec.blob; NUL-safe          */
    u8     mode;                /* Mode at dispatch time                */
    u8     src;                 /* CmdSource; REPLAY events are ignored */
    u8     range_kind;
    bool   range_given;
} RecEvent;

typedef struct Rec {
    Vec(RecEvent) ev;  Bytebuf blob;
    u8   reg;                   /* target register, 'a'–'z'             */
    u8   mode_at_start;
    u32  txn_depth_at_start;    /* §5 guard                             */
    bool active, in_prompt, append, import_ed;
} Rec;

bool yew_record_start(Ed *ed, u8 reg);
CmdStatus yew_record_stop(Ed *ed);           /* emits + stores           */
bool yew_record_active(const Ed *ed);
CmdStatus yew_record_preflight(CmdId id, const CmdCtx *cx);
void yew_record_tap(CmdId id, const CmdCtx *cx); /* registry recorder tap */
void yew_record_key(Ed *ed, Key k);              /* thin context tap       */
```

**The pitfall this design exists to kill.** vim records keystroke bytes, so
a macro recorded before you rebind `d` does something else afterwards, and a
macro shared with a colleague who uses a different keymap is a random text
generator. yew records *what the key meant* — the resolved `CmdId` with
its resolved arguments, captured at s13's tap, which fires **after argument
resolution and before execution**. A rebound key replays as its command; a
macro moves between keymaps unchanged. This is also why the tap is in
`yew_cmd_invoke` and not in `dispatch.c`: mouse clicks, E-mode commands, and
Fletch calls all funnel through the same choke point, so all three record.

**What the key tap is for, and only that.** `yew_record_key` supplies the
two things the command stream cannot carry:

| Need | Why commands don't carry it |
|---|---|
| the mode at the start of the recording | the first command may be a motion whose meaning is unit-relative |
| prompt sessions (`YEW_CMD_PROMPTS`) | keys typed into an E-mode/search prompt are consumed by the prompt, not dispatched; the recorder must capture the **committed command**, not the keystrokes that composed it |

Prompt handling, pinned: while a `YEW_CMD_PROMPTS` command owns the modal
layer, the key tap sets `rec->in_prompt`; the tap ignores everything until
the prompt commits, and the committed command arrives at `yew_cmd_invoke`
like any other and is recorded once. A cancelled prompt records nothing.

Events with `src == YEW_SRC_REPLAY` are **dropped** — recording during a
replay would double every op, and the guard is one comparison rather than a
mode flag someone forgets to clear.

### 2. Emission — terse Fletch per spec §3

```c
void yew_record_emit(const Rec *r, const Ed *ed, Bytebuf *out);
```

Output is an immediately invoked function containing local motion macros and
resolved `ed.run` calls, plus the header comment convention Sprint 38
formalizes:

```fletch
# yew-macro: 1
# recorded-with: yew 1.0.0
# keymap: default
import ed
(fn() {
  macro rec_a = @[ l< H(lv) yank v paste esc ];
  rec_a();
})();
```

Command → motion word, the reverse of Sprint 34's `motion_tab.c`:

| Recorded command | Emitted | Notes |
|---|---|---|
| `ed.mode.enter` `sarg` `"L"`/`"W"`/`"B"` | `l` / `w` / `b` | unit switch |
| char unit (I-mode entry without text) | `c` | |
| `ed.move.unit.next/prev/up/down` | `>` `<` `^` `v` | ASCII only — the Unicode aliases are input-side (spec §3) |
| the `.alt` variants | `a>` `a<` `a^` `av` | |
| `ed.mode.enter "H"` … `ed.mode.escape` | `H( … )` | the enclosed run is emitted inside the parens |
| `ed.edit.insert.text` | `i"…"` | consecutive inserts **concatenate** into one string; escapes per spec §1 |
| `ed.edit.delete.unit` | `del` | |
| `ed.mode.escape` | `esc` | |
| any other `RECORDABLE` command | its `CmdDesc.word` | `yank`, `paste`, … |
| `count_given` | `N` prefix on the next word | `4>` |
| `iarg`/`sarg` on a CMDWORD | **not representable in §3** | see below |

**Commands carrying an envelope.** The §3 vocabulary has no syntax for
string/integer arguments, bang, or resolved ranges beyond counts and
`i"…"`. Such a command is emitted as an `ed.run` call **outside the local
motion macro**, splitting motion runs while preserving event order:

```fletch
(fn() {
  macro rec_a_0 = @[ l< H(lv) ];
  rec_a_0();
  ed.run("ed.file.save", { sarg: "out.txt", bang: true,
                           range_kind: "lines", range_given: true,
                           range_lo: 1, range_hi: 3 });
  macro rec_a_1 = @[ v paste ];
  rec_a_1();
})();
```

Pinned: the emitter never invents motion syntax. Splitting is ugly and rare
and always correct; a bespoke argument syntax inside `@[ ]` would be a
second grammar the spec did not freeze, and the round-trip law would be
protecting a language nobody agreed to.

**Run folding.**

| Input run | Emitted | Rule |
|---|---|---|
| `> > > >` (four events, no counts) | `4>` | fold runs of ≥ 2 identical events |
| `4>` then `3>` (both `count_given`) | `4> 3>` | **never** merge counted events |
| `> > >` where the command is `TAKES_COUNT` | `> > >` | folding is illegal — see below |
| `i"a" i"b" i"c"` | `i"abc"` | insert concatenation, a separate rule |
| `del del` where `del` is `REPEATABLE` | `2del` | same rule as arrows |

**Pitfall — folding is only legal for `YEW_CMD_REPEATABLE`.** s13 pinned the
distinction: `3 ed.move.line.down` runs three times (`REPEATABLE`), but
`3 ed.tab.goto` switches to tab 3 (`TAKES_COUNT`). Folding three
`ed.tab.goto` events into `3tabgoto` silently changes the meaning from "go
to the current tab three times" to "go to tab 3". The emitter checks the
flag; the round-trip harness fails loudly if it ever forgets, which is
exactly the class of bug the law exists to catch.

**No cross-event count arithmetic.** `4>` followed by `3>` is *not* folded
to `7>`. The transformation is sound for repeatables and unsound the moment
a command's per-invocation side effect is not idempotent-by-count; more
importantly the recording should read like what the user did.

**Mode-context annotation.** A recording is emitted **mode-independent**: if
it contains any unit-relative motion (`> < ^ v` or `del`), the emitter
prepends the explicit unit word for `mode_at_start` (`l`, `w`, or `b`). Why:
without it, a macro recorded in W mode and replayed from L mode moves by
lines and the law is false for a reason no test would explain. The prepended
word is a real motion, not a comment, so replay is self-contained.

Emission is **deterministic**: same `Rec` → byte-identical source, pinned by
the determinism lane.

### 3. Storage and the recording commands

| Command | Arity | Flags | Behavior |
|---|---|---|---|
| `ed.macro.record` | OPT_STR | PROMPTS | `q a` starts recording into `a`; bare `q` while recording stops |
| `ed.macro.stop` | NONE | — | explicit stop |
| `ed.macro.replay` | STR | REPEATABLE, RECORDABLE | `@a` — replay register `a`; a count replays it N times |
| `ed.macro.replay_last` | NONE | REPEATABLE, RECORDABLE | `@@` |
| `ed.macro.list` | NONE | — | Sprint 38's browser; here it prints to the message line |

Recordings live in the s12 register file as **`YEW_REG_CHARWISE` values
holding the Fletch source text**. Consequences worth stating, because they
are the whole product argument:

- `"ap` pastes a macro into a buffer as readable source (Sprint 38's edit
  flow is built entirely on this);
- `"Aq` appends a second recording to `a` and the result is still valid
  Fletch (each recording is a self-contained IIFE, and only the first chunk
  emits `import ed`, so local macro names cannot collide);
- registers already persist and already have a picker (s12/s26), so macros
  inherit both for free.

Writes go through the registry (`ed.reg.set`), never `yew_reg_set` — s12's
routing choke point still owns the register file (its DoD-12 grep gate must
still pass with `record.c` in the tree).

Nesting: `ed.macro.record` while recording is refused with a message.
Recursion: a macro that replays itself is allowed but bounded by the VM's
call depth, and the resulting error is an ordinary Fletch error.

### 4. Replay is the VM, and only the VM

```c
CmdStatus yew_macro_replay(Ed *ed, u8 reg, u32 count);
```

Implementation, in full, because its brevity *is* the deliverable:

```c
/* Compile the register's text as ordinary Fletch and run it.  There is no
 * interpreter in record.c.  Two execution paths — a "fast" event replayer
 * and the VM — would drift the first time a command's argument handling
 * changed, and the round-trip law would keep passing (it tests the VM
 * path) while real replays diverged.  One path, one truth.             */
CmdStatus yew_macro_replay(Ed *ed, u8 reg, u32 count) {
    const RegVal *v = yew_reg_get(&ed->regs, reg);
    if (!v || v->bytes.len == 0) return yew_cmd_fail(...);
    FlChunk *c = fl_compile_str(ed->fl, v->bytes.p, v->bytes.len, "macro:a");
    if (!c) return YEW_CMD_ERR_ARG;                /* caret diag shown  */
    for (u32 i = 0; i < count; i++)                /* each run: one txn */
        if (!fl_call_chunk(ed->fl, c, YEW_SRC_REPLAY)) return YEW_CMD_ERR_STATE;
    return YEW_CMD_OK;
}
```

Pinned properties:

- **One replay = one undo step.** `fl_call_chunk` is an outermost Fletch
  entry, so Sprint 34 §5's implicit transaction wraps it
  (`YEW_TXN_MACRO`). A count of 5 produces 5 nodes, not 1 — five presses of
  `u` undo five runs, which is what `5@a` means.
- **An error aborts and rolls back** (spec §10) — vim's silent mid-replay
  misfire is not reachable.
- Compilation is cached per register by exact source bytes, so a
  1 000-iteration replay compiles once and same-timestamp rewrites cannot
  return stale code. Register writes invalidate the corresponding cache.
- Grep gate (DoD 6): `grep -c 'yew_cmd_invoke\|yew_edit_\|yew_textbuf_'
  src/fl/record.c` → `0`. The recorder observes and prints; it never acts.

### 5. Transaction and multi-cursor interaction

- Recording **starts and stops outside transactions**. `yew_record_start`
  captures `txn_depth_at_start`; attempts to start or stop inside an active
  edit transaction fail with `YEW_CMD_ERR_STATE` and a message.
- A multi-cursor command (s17 `yew_mc_run`) invokes `d->fn` once per cursor
  but calls `yew_cmd_invoke` **once**, so the tap fires once and the
  recording says "do this to every cursor" — which is what replays
  correctly against a different-sized cursor set. Recording per cursor would
  hard-code the cursor count into the macro.
- `edit{}` blocks executed *during* recording (from a bound Fletch function)
  record their constituent commands, not the block — replay re-derives the
  transaction from Sprint 34 §5's implicit rule.

### 6. The round-trip law harness — `tests/roundtrip/`

```
tests/roundtrip/
  gen.c                         session generator (seeded, deterministic)
  runner.c                      property test; `make test-roundtrip`
  fixtures/*.txt                documented initial fixture bytes
tests/corpus/recorder/*.rec     committed replay seeds/prefixes
```

**The property**, stated once and tested forever:

> For every recorded session `S` starting from editor state `E0`:
> let `T = emit(record(S, E0))`. Then `run(T, E0)` and `S(E0)` are equal in
> **buffer bytes**, **cursor set** (`pos`, `anchor`, `goal_col`, primary
> index, for every window), **register file** (every named, numbered, and
> special register: type, bytes, blockwise width/ragged), and **mode**.

Explicitly **not** compared, with the reason stated in `runner.c`'s header:
the undo tree's *shape*. Recording produces N transactions; replay produces
one (Sprint 34 §5). Comparing shapes would assert a falsehood. What *is*
asserted about undo is the corollary that matters: undoing once after a
replay restores `E0` exactly, and that is a separate property in the same
runner (P2 below).

Properties checked per session:

| # | Property |
|---|---|
| P1 | the round-trip law above |
| P2 | one `yew_undo` after replay reproduces `E0` byte-exactly |
| P3 | `emit` is deterministic: two emissions of the same `Rec` are byte-identical |
| P4 | `T` parses with zero diagnostics (`fl_parse`) and re-emitting the parsed AST through `fl_ast_dump` is stable |
| P5 | for generated **mode-closed** `S`, replaying `T` twice from `E0` equals running `S` twice — no hidden recorder/cache state |

**The generator** (`gen.c`) produces a session as a list of portable resolved
envelopes from a seeded xorshift PRNG:

- the successful, state-contained command pool is explicit; registry coverage
  requires every other `YEW_CMD_RECORDABLE` command to appear on the reasoned
  denylist. `make test-roundtrip-coverage` fails on an unclassified command;
- weights: motions 45 %, inserts 20 %, deletes 12 %, selections 10 %,
  yank/paste 8 %, mode switches 5 %;
- counts: 60 % none, 30 % 2–9, 10 % 10–99;
- insert payloads drawn from a fixture alphabet that includes ASCII, CJK,
  a ZWJ emoji sequence, a combining-mark cluster, and a lone `0x80` byte
  (invariant 2 rides along on invariant 10);
- session length 1–200 events; initial fixtures span empty, one-line,
  10 000-line, CRLF, no-final-newline, and binary-ish buffers.

Commands with side effects outside the compared state are excluded by an
explicit **denylist** in `gen.c`, each row carrying its reason (`ed.quit`,
`ed.file.save`, `ed.job.*`, anything `YEW_CMD_PROMPTS`). A denylist, not an
allowlist: new commands are in the harness by default, and excluding one is
a visible decision.

**Failure reporting.** On divergence the runner finds the shortest diverging
prefix, then prints:

```
roundtrip: FAIL seed=41207 fixture=crlf_small.txt
  diverged at event 7 of 12 (shrunk from 96)
  emitted source:
      1 | macro rt = @[ w< 3> H(w>) yank
      2 |   v paste esc ]
                 ~~~~~ event 7: ed.edit.paste (word "paste")
  record path: cursor 0 @ 42:3   registers: "=(char,5)"…
  replay path: cursor 0 @ 42:1   registers: "=(char,5)"…
  buffer differs at byte 118:
      record | hello  world\r\n
      replay | hello world\r\n
                     ^
  minimized session written to build/tmp/fail-41207.rec
```

The three things that make a failure actionable: the **divergent op**
located in both the event stream and the emitted source, the first differing
byte, and a replayable `.rec` artifact. The runner never dirties the source
corpus automatically; a failing artifact is reviewed and committed with its
fix.

**CI.** `make test-roundtrip` runs seeds `0…1999` (deterministic, part of
`make test`, budget ≤ 60 s) plus every `corpus/*.rec`. A nightly lane runs
`YEW_RT_SEEDS=100000` with a varying `YEW_RT_BASE_SEED`; any failure is
minimized and uploaded for review. Accepted regressions are committed to the
corpus, which never shrinks.

### 7. Recording state for the UI (Sprint 38's data)

```c
typedef struct RecStatus { bool active; u8 reg; u32 nevents; u8 last_reg; }
u32 yew_record_status(const Ed *ed, RecStatus *out);
u32 yew_macro_list(const Ed *ed, RegInfo *out, u32 max);  /* over s12 */
```

Read-only; the statusline indicator, the browser, and the edit flow are
Sprint 38. This sprint's only UI is a message on start/stop
(`recording @a` / `recorded @a (12 events, 47 bytes)`).

### 8. Defer

- Statusline recording indicator, register browser, the **edit-macro flow**,
  the macro library (`~/.config/yew/macros/*.fl`), the sharing header's
  full convention and its loader → **Sprint 38**. The header comment is
  *emitted* here (three lines) so recordings made before 38 are already
  well-formed.
- Default keybindings for `q` / `@` / `@@` → **Sprint 36** (the default
  config in Fletch owns the keymap); until then the commands are reachable
  from E mode and `yew fl`.
- `--batch` replay and macro-driven script tests → **Sprint 37**.
- Recording of plugin-registered commands → automatic once Sprint 54 lands
  (they are registry commands with `word`s); nothing to build.
- Cross-version macro compatibility checking (the `recorded-with:` header
  compared against the running version) → **Sprint 38**.

## Testing Strategy

- **Unit** (`tests/unit/test_record_emit.c`): every row of §2's command →
  word table; all five folding rows incl. the `TAKES_COUNT` negative;
  insert concatenation across 3 events and across an intervening motion
  (must **not** concatenate); string escaping of `"`/`\n`/`\t`/`\0`/invalid
  UTF-8 in an `i"…"` payload; the argument-split case producing a valid
  three-part program; mode-annotation prepending for L/W/B and its absence
  when no unit-relative motion was recorded.
- **Unit** (`test_record_tap.c`, fake dispatcher): `YEW_SRC_REPLAY` events
  dropped; prompt sessions recording exactly one committed command and
  nothing for a cancelled prompt; a multi-cursor command recording one
  event; `YEW_CMD_RECORDABLE`-less commands never recorded.
- **Native integration** (`tests/unit/test_record_integration.c` and replay
  runtime suites): record → stop yields source that parses; five replays
  produce five undo nodes; an erroring macro rolls back; exact-byte compile
  caching invalidates on register write; uppercase append replays both
  recordings. Script/`--batch` coverage remains Sprint 37.
- **Round-trip** (`make test-roundtrip`): §6 in full — 2 000 seeds plus the
  corpus in `make test`; the shrinker exercised by a deliberately broken
  emitter under `YEW_RT_SELFTEST=1` (fold a `TAKES_COUNT` command) which
  must fail, shrink to ≤ 3 events, and name the right op.
- **Fuzz** (`tests/fuzz/fuzz_record.c`): random `RecEvent` streams (random
  `CmdId`s, counts, arbitrary `sarg` bytes) → `yew_record_emit` → `fl_parse`
  must produce **zero** diagnostics. The emitter generating unparseable
  source is the law's most direct failure and the fuzzer is its cheapest
  guard. Corpus committed.
- **Pty** (`tests/pty/`): drive `ed.macro.record`, `ed.macro.stop`, and
  `ed.macro.replay` through E mode (default `q`/`@` bindings are Sprint 36);
  final grid matches the same edit typed twice, with start/stop message-line
  goldens.
- **Determinism**: the same session emits byte-identical source across two
  runs and both compilers; `make test-roundtrip` output is byte-stable.
- **Perf**: recording overhead ≤ 200 ns per dispatch (measured by a 10⁶-op
  dispatch benchmark with the tap installed vs NULL) — the tap is on the
  keypress path and invariant 4 does not exempt it. `emit` of a 10 000-event
  recording ≤ 10 ms.

## Definition of Done

1. `make test` green at `-Werror`; gcc and clang builds, ASan/UBSan,
   valgrind, and determinism remain CI gates. `make test-roundtrip` is part
   of `make test` and completes in ≤ 60 s.
2. **The law holds**: 2 000 generated seeds plus the whole committed corpus
   pass properties P1–P5, from six initial fixtures incl. CRLF, no-final-
   newline, and invalid-UTF-8 content.
3. The shrinker works: `YEW_RT_SELFTEST=1` fails, shrinks a 96-event session
   to ≤ 3 events, names the divergent op and the first differing byte, and
   writes a replayable `.rec`.
4. Emitter fuzzer: 10 min in the ASan lane, zero unparseable emissions, zero
   crashes; corpus committed and replayed in `make test`.
5. Every row of §2's mapping table and all five folding rows have a unit
   test; the `TAKES_COUNT` no-fold rule has both a unit test and a
   round-trip fixture that fails without it.
6. **One execution path**: `grep -c 'yew_cmd_invoke\|yew_edit_\|
   yew_textbuf_\|yew_reg_set' src/fl/record.c` → `0`; replay goes through
   the ordinary cached Fletch compiler plus `fl_call_chunk` only.
7. `5@a` produces exactly 5 undo nodes; a macro whose 3rd command errors
   leaves the buffer byte-identical to the pre-replay state.
8. Recording during replay records nothing (`YEW_SRC_REPLAY` drop asserted);
   `q` while recording stops rather than nesting.
9. Generator coverage: every `YEW_CMD_RECORDABLE` command is either reached
   by the generator or on the denylist with a stated reason;
   `make test-roundtrip-coverage` fails on an unreached, unlisted command.
10. Every `YEW_CMD_RECORDABLE` command emits a word that `yew_cmd_by_word`
    maps back to the same `CmdId` (bijection asserted over the whole
    registry — Sprint 34 enforces the field, this asserts the round trip).
11. Perf: tap overhead ≤ 200 ns/dispatch, emit of 10 000 events ≤ 10 ms;
    baselines committed to the perf lane.
12. Pty goldens for record/replay byte-stable across the determinism lane's
    double run.
13. ≥ 120 recorder/replay unit assertions and ≥ 20 committed `.rec` corpus
    files. Macro-driven script tests are explicitly Sprint 37.
14. `ed.macro.*` no longer `YEW_CMD_DEFERRED`. Sprint 38 delivered the browser, edit flow, and library through the same recorder/VM path; current closeout tests those live surfaces rather than their landing guards.
