# Sprint 38: Macro UX — Indicator, Browser, the Edit-a-Macro Flow, Library

## Prerequisites

- Sprint 35 — `src/fl/record.c|h`: `Rec`, `RecEvent`, `yew_record_start/
  stop/active`, `yew_record_emit`, `yew_record_status` (`RecStatus`),
  `yew_macro_list`, `yew_macro_replay`, the register storage convention
  (recordings are `YEW_REG_CHARWISE` values holding Fletch source), the
  three-line header comment the emitter already writes, and the round-trip
  law harness that every flow here must keep green.
- Sprint 12 — `Registers`, `RegVal`, `RegInfo`, `yew_reg_get`, the routing
  choke point (`yew_reg_yank`/`yew_reg_delete`) and its grep gate, `RegType`.
  s12 pinned "macro registers and record/replay into `a`–`z` → Sprint 38".
- Sprint 15 — `src/ui/statusline.c`: the segment model (`{text, width,
  priority}`), the mode chip's recolor rule, the truncation policy, and
  `yew_str_cells()` for width math. Sprint 26 — the fuzzy finder engine and
  the picker skin (s54 reuses it the same way). **Assumption beyond the
  index entry**: the picker is `yew_picker_open(Ed*, const PickerSpec*)`
  with a row-provider callback and region registration; if s26 landed
  another shape, adapt — the requirement is "one picker implementation".
- Sprint 18 — E mode, `:` commands, `CmdArgv`, completion sources.
  Sprint 21 — scratch buffers. Sprint 34 — flapi, origins, the registration
  ledger. Sprint 36 — `bind()`, the option table (`macro.dir` exists and is
  unread until this sprint), config load order and origins, `runtime/
  init.fl` as the keymap's source of truth. Sprint 37 — `--batch`,
  interactive refusals, the script-test runner.
- Sprint 6 — the pty harness (`PtyCase`, `ptc_keys`, `ptc_snapshot`,
  profiles); Sprint 41 (forward) — the Fletch syntax definition that makes
  the macro scratch buffer highlighted; until then it renders plain and the
  flow is unaffected.
- Binding: `00-decisions.md` invariants 3, 5, 9, 10; `02-fletch.md`'s vim
  comparison table — the first two rows are this sprint's product spec.

## Goals

Make the round-trip law visible in the hands, not just in the harness —
**this is the vim-killer feature the law buys**. Vim's macros are opaque:
`qa` records keystroke bytes, and "editing" one means pasting register
gibberish into a buffer, decoding `^[` and `^R` by eye, fixing a character,
and yanking it back — a workflow so hostile that users re-record instead.
Because Sprint 35 emits readable Fletch that records *commands, not keys*, a
recording here is already a program: a wrong step is a line you delete, a
repeated step is a `for` loop you write, and a good macro is a function you
name, save to a file, and send to a colleague whose keymap is different and
whose yew runs it unchanged. So this sprint lands the recording
indicator, the macro register browser, replay-with-counts and replay-last
polish, and the headline **edit-a-macro flow** (open a recording as a `.fl`
scratch buffer, edit it as ordinary source, re-bind it to a register or name
it as a function), then the macro library (`~/.config/yew/macros/*.fl`
autoloaded as named functions, with naming and collision rules), the sharing
format (a macro file is just Fletch, with a header-comment convention
recording the yew version and keymap it was recorded against), and pty
goldens for the indicator and the edit flow. No new recording semantics:
Sprint 35 froze them, and every flow here ends in the same
`fl_compile_str` + `fl_call_chunk` replay path so the law cannot move.
Highlighting of the macro scratch is Sprint 41; distributing macro
collections is Sprint 55.

**Completion record:** implementation complete on trunk at `be4ac3b`
(2026-08-10). The closing gates were clean GCC and Clang fast tiers (1,545
unit tests / 16,935,842 assertions), the full Clang ASan/UBSan tier, 21 macro
script tests, the unchanged 2,000-seed P1–P5 round-trip law with the new
record→edit→store fixture, six native pty cases, focused Valgrind over every
macro and picker unit plus four ownership-relevant pty flows, and the
200-file library scan at 2.739 ms against its 5 ms gate.

Two literal test clauses in the authored contract are structurally
unreachable under its own pinned namespace rules. Distinct filenames plus
`<stem>.<dot-free binding>` canonical names and the same-stem-only bare alias
cannot produce a cross-file collision; the loader nevertheless rejects a
collision defensively. Likewise, Sprint 41 highlighting and Sprint 55
package installation deliberately expose no callable Sprint 38 surface, so
there is no honest stub to hard-error. Completion preserves those absences
rather than inventing APIs that the Defer section explicitly rejects.

## Deliverables

### 1. Recording indicator — `src/ui/statusline.c`

One new segment, inserted immediately right of the mode chip:

| Segment | Content | Attr | Drop priority |
|---|---|---|---|
| `recording` | `●REC a` while recording; ` 12` events appended once ≥ 10 events | `mode.select` role, bold, **blinking off** | **never dropped** (0, with the chip) |

- Never dropped, and never truncated below `●a`: a user who does not know
  they are recording will type into a macro and be baffled by the replay.
  It outranks the path, the position, and the percentage.
- The event counter updates at most once per input burst (it is derived
  from `yew_record_status`, which is a read of `Rec.ev.len` — no timer, no
  extra damage). **Pitfall:** marking the footer damaged on every keystroke
  because a counter changed re-renders the row 30×/s during key-repeat;
  the segment compares its rendered text and damages only on change.
- No blinking, ever: a blinking indicator needs a timer, and a timer that
  fires while the input queue is non-empty violates s14's loop rule. Bold
  plus a colored glyph is legible and free.
- `--batch` never renders it (there is no statusline); `yew_record_status`
  is still readable so scripts can assert.

### 2. Replay polish and per-register listing

| Command | Binding (in `runtime/init.fl`) | Behavior |
|---|---|---|
| `ed.macro.record` | `q` | `q a` starts into `a`; bare `q` while recording stops |
| `ed.macro.replay` | `@` | `@a`; `5@a` runs it five times → **five** undo nodes |
| `ed.macro.replay_last` | `@ @` | replays `RecStatus.last_reg`; counts apply |
| `ed.macro.list` | `Q` | opens the browser (§3) |
| `ed.macro.edit` | — (E mode: `:macro edit a`) | §4 |
| `ed.macro.name` | — (`:macro name a dup_line`) | §4 |
| `ed.macro.reload` | — (`:macro reload`) | rescan the library (§5) |

- `:macros` lists every non-empty macro register as `NAME  EVENTS  BYTES
  FIRST-LINE`, deterministic order `a`…`z`. It is porcelain: byte-stable,
  tab-separated, scriptable.
- Replay of an **empty or non-macro** register: if the register's bytes do
  not compile, the error is the ordinary Fletch caret diagnostic pointing
  into the register's text — which is exactly right, because a register
  holding yanked prose *is* a syntax error, and saying so beats "invalid
  macro".
- `5@a` is five separate outermost Fletch entries and therefore five
  transactions (Sprint 34 §5, Sprint 35 §4). Pinned here because the
  alternative ("a count is one big undo step") is a tempting and wrong
  simplification: `5@a` then one `u` should undo one run.

### 3. The macro register browser — `src/ui/macrobrowse.c`

`ed.macro.list` opens the s26 picker with a macro-specific row provider:

```
 ●  a   12 ev   47 B   macro rec_a = @[ l< H(lv) yank v paste esc ]
    b    3 ev   19 B   macro rec_b = @[ w> del ]
    d   —      1.2 K   dup_line            (library: ~/.config/yew/macros/edit.fl)
```

- Rows: the 26 named registers that hold macros, then library functions
  (§5), one namespace in one list — a user does not care which storage a
  macro lives in.
- Glyph `●` marks the register currently being recorded into (never more
  than one).
- Keys: `Enter` replays · `e` **edit** (§4) · `y` yanks the source to the
  unnamed register · `d` clears the register (with confirm) · `n` names it
  into the library · `/` fuzzy-filters over name **and source text** (s26
  scoring), which is how you find "the one that did the thing with parens".
- Preview pane shows the full source, syntax-highlighted once Sprint 41
  lands. Region-registered for mouse like every list (invariant 9: the
  keyboard path is complete without it).

### 4. THE EDIT-MACRO FLOW

```c
CmdStatus yew_macro_edit(Ed *ed, u8 reg);          /* ed.macro.edit      */
CmdStatus yew_macro_store(Ed *ed, Buffer *scratch);/* the :w path        */
```

1. `ed.macro.edit a` opens a **scratch buffer** named `*macro a*` with the
   register's source, `lang = "fletch"`, `path = NULL`, and a hidden
   `macro_reg = 'a'` attribute on the buffer. It is a real buffer: every
   motion, every mode, undo, search, and multi-cursor work on it, because it
   is *source code* and yew already edits source code.
2. The user edits it. There is nothing to learn.
3. `:w` (or `ed.file.save` on that buffer) runs `yew_macro_store`:
   **compile first** with `fl_compile_str`. On success the register is
   replaced (through `ed.reg.set`, honoring s12's routing gate), the buffer
   is marked clean, and the message is `stored macro @a (3 lines)`.
   On failure **nothing is stored** and the caret diagnostic renders against
   the scratch buffer's real line/col, so the error points at the line under
   the cursor. Refusing to store a non-compiling macro is the entire safety
   property: a register can never hold a macro that will fail at replay
   time, which is vim's characteristic failure.
4. `:macro name a dup_line` (or `n` in the browser) promotes the register to
   a **named function**: it rewrites the source's `macro rec_a = …`
   declaration to `macro dup_line = …`, appends it to the library file
   `<macro.dir>/user.fl` (created with a header if absent), reloads the
   library (§5), and — pinned — **leaves the register untouched**, so
   `@a` still works while the name becomes available. Promotion is additive;
   nothing a user recorded is ever consumed by naming it.
5. Closing the scratch with unsaved changes prompts like any dirty buffer
   (s14's dirty-quit machinery), because it *is* one.

**Pitfall — the scratch buffer must not be special.** The temptation is a
bespoke mini-editor for macros with its own keymap and its own save path.
That is a second editor to maintain, it will diverge, and it throws away the
only reason the round-trip law is worth having. `*macro a*` differs from any
other buffer in exactly two ways: it has no file path, and `ed.file.save`
routes to `yew_macro_store` when `macro_reg` is set. Two lines of
difference, one grep gate (`grep -rn 'macro_reg' src/` hits only
`macrobrowse.c`, `file_cmds.c`, and `buf.h`).

**Pitfall — round-trip on the edited macro.** An edited macro is *not*
recorder output, so the round-trip law does not cover it — but the replay
path is identical, so nothing new can break. What is added: on store, the
macro is parsed and its motion words are validated against
`yew_cmd_by_word` (Sprint 34), so `@[ yankk ]` is rejected at store time
with "no command has word 'yankk'; did you mean 'yank'?" rather than at
replay time, mid-edit.

### 5. The macro library

Location: `macro.dir` (option, Sprint 36) defaulting to
`$XDG_CONFIG_HOME/yew/macros/`. Every `*.fl` in it is loaded at startup
after `init.fl` (origin `FL_ORIGIN_CONFIG`, so the config reload ledger tears
them down with the rest — no new teardown machinery).

```c
u32 yew_macrolib_scan(Ed *ed, DiagCtx *dc);   /* returns count loaded */
```

Naming and collision rules, pinned:

| Rule | Detail |
|---|---|
| a file is a module | every top-level `fn`/`macro` binding not prefixed `_` is exported |
| macro name | `<stem>.<binding>` — `edit.fl` exporting `dup_line` gives `edit.dup_line` |
| bare alias | a binding whose name equals the file stem is *also* reachable bare (`surround.fl` exporting `surround` → `surround`) |
| zero-arg only | only zero-argument bindings become **macros** (replayable from the browser and `@`); others are ordinary functions callable from Fletch |
| scan order | bytewise by filename (`yew_sort_stable`) — deterministic, invariant 5 |
| collision within the scan | a **hard error** naming both files and the binding; neither is loaded. Silently picking one makes behavior depend on filename luck |
| collision with a register | none possible — registers are single letters and library names always contain a `.` or are a file stem of ≥ 2 chars; the browser shows both namespaces and the validator rejects a 1-char stem |
| a file that fails to compile | reported with carets, **skipped**; the rest of the library loads (one bad file must not cost a user their whole library) |

`:macro reload` rescans; `macro.dir` changing at runtime triggers a rescan
through the option's `on_change`.

### 6. The sharing format

A macro file **is** Fletch — there is no macro format, and that sentence is
the feature. The only convention is a header of comments, which the recorder
already emits (Sprint 35 §2) and which this sprint formalizes and reads:

```fletch
# yew-macro: 1                     # header schema version
# recorded-with: yew 1.0.0         # the version that recorded it
# keymap: default                      # provenance only — see below
# recorded: 2026-07-31T14:22:07Z       # UTC, second resolution
# describe: wrap the selection in parens and reindent
macro surround_parens = @[ H(w>) i"(" v i")" esc ]
```

| Field | Required | Read by |
|---|---|---|
| `yew-macro:` | yes for library files | the loader (unknown version → skip with a message) |
| `recorded-with:` | no | the browser; a **warning only** if the major version differs |
| `keymap:` | no | the browser's info pane |
| `recorded:` | no | browser sort order |
| `describe:` | no | the browser row and `:macros` |

**`keymap:` is provenance, not a dependency — pinned.** Because Sprint 35
records resolved commands rather than keystrokes, a macro recorded under a
heavily customized keymap replays correctly under the default one and vice
versa. The field exists so a reader knows where a macro came from, and so a
bug report can say "recorded under X". Any code that *refuses* to run a
macro because of a keymap mismatch is a review rejection: it would be
asserting a dependency the design deliberately does not have.

Header comments are comments (spec §1), so they cannot affect execution, and
a macro file with no header is valid Fletch that simply loses the browser
metadata. Sharing is `cp`, `curl`, or a paste into a chat window; there is
no archive format, no registry, and no `yew macro install` — a macro is a
file, and Sprint 55's `yew pkg` handles the case where someone wants a
directory of them versioned.

### 7. `--batch` and script surface

- `ed.macro.record` remains interactive-refused under `--batch` (Sprint 37's
  table): recording needs keys.
- `ed.macro.replay`, `ed.macro.list`, the library, and `yew_macro_store` all
  work headless, so `yew --batch -e 'ed.run("ed.macro.replay", {sarg:"a"})'`
  and library-defined functions are usable in scripts and in the script-test
  suite.
- New script-test assertions in Sprint 37's `t.*` set are not needed; macro
  tests use `t.text`, `t.reg`, and `t.undo`.

### 8. Defer

- Syntax highlighting of the macro scratch buffer and the browser preview →
  **Sprint 41** (the Fletch language pack). The flow works unhighlighted;
  nothing here waits on it.
- Distributing macro *collections* (versioned, updatable) → **Sprint 55**
  (`yew pkg`), which treats a macro directory as a package. `yew macro
  install` is explicitly not a thing.
- Recording into a *file* directly (rather than a register then promote) →
  not for 1.0; the two-step flow is one command longer and one concept
  shorter.
- Cross-version migration of macros (a `recorded-with:` major mismatch doing
  anything but warning) → post-1.0, and only if the command registry ever
  breaks a `word`, which the Sprint 34 registration rules make a review
  event.
- Undo-tree-aware "macro from the last N edits" (record retroactively) →
  post-1.0; pinned so it is a decision.

## Testing Strategy

- **Unit** (`test_macrolib.c`): every reachable namespace row — stem alias,
  `_`-prefixed exclusion, non-zero-arg bindings not becoming macros, a
  non-compiling file skipped with the rest loading, 1-char stem rejected,
  and bytewise scan order. The canonical-name and bare-alias rules are
  injective, so the retained defensive collision rejection is reviewed as an
  unreachable invariant guard rather than reached by weakening the public
  namespace for a fixture.
- **Unit** (`test_macro_header.c`): header parsing incl. missing fields,
  unknown `yew-macro:` version skipped, a major `recorded-with:`
  mismatch warning **and still loading**, a file with no header loading
  fine, a `keymap:` mismatch changing nothing about execution.
- **Script** (`tests/script/macroux_*.fl`): `yew_macro_store` refusing a
  non-compiling edit and leaving the register byte-identical; storing a
  valid edit and replaying it; an unknown motion word rejected at store
  time with the did-you-mean; `:macro name` leaving the register intact and
  the library gaining the function; library reload after a file change;
  `5@a` producing 5 undo nodes and one `u` undoing one run.
- **Round-trip**: the Sprint 35 harness runs unchanged and must stay green
  (this sprint adds no emission or replay semantics) — asserted by the same
  `make test-roundtrip` in `make test`. Additionally, a corpus case records
  a macro, edits it through `yew_macro_edit`/`yew_macro_store` with a no-op
  edit (retype a byte, delete it), and asserts the stored source is
  byte-identical to the emitted source.
- **Pty** (`tests/pty/`): `macro_indicator` — `q a`, three keys, `q`: the
  indicator appears with the register, shows the event count, and vanishes;
  goldens at 80 and 40 columns proving it survives truncation.
  `macro_edit_flow` — record, `Q`, `e`, edit a character, `:w`, `@a`, final
  grid golden equals typing the edited macro by hand.
  `macro_browser` — the browser with three registers and two library
  entries, filter typed, preview pane.
- **Determinism**: `:macros` output byte-identical across two runs; library
  scan order stable; all pty goldens in the determinism lane's double run.
- **Perf**: library scan of 200 macro files ≤ 5 ms (it is on the startup
  path and inside the 20 ms first-paint budget); the indicator adds zero
  frames to a 4 000-key burst (frame counter asserted, per s14's rule).

## Definition of Done

1. `make test` green on gcc and clang at `-Werror`; ASan/UBSan/valgrind/
   determinism lanes clean; `make test-roundtrip` still green with the
   record→edit→store no-op fixture added.
2. Indicator: present while recording, names the register, never dropped or
   truncated below `●a` at 40 columns, damages the footer only on change
   (frame count unchanged across a 4 000-key burst).
3. `5@a` produces exactly 5 undo nodes; one `u` undoes exactly one run.
4. Browser: registers and library entries in one deterministic list; all six
   keys (`Enter e y d n /`) work; fuzzy filter searches name **and** source;
   byte-stable golden.
5. **Edit flow**: `ed.macro.edit` opens an ordinary buffer (proven by a test
   that runs multi-cursor editing and undo inside it); `:w` compiles first
   and stores nothing on failure, with the caret at the right line;
   an unknown motion word is rejected at store time with a suggestion.
6. `grep -rn 'macro_reg' src/` hits only three files — the scratch buffer is
   not a special editor.
7. `:macro name` leaves the register intact, appends to the library, and the
   named function is callable immediately (no restart).
8. Library: The seven reachable namespace rows are tested; unique filename
   stems, dotted canonical names, same-stem-only bare aliases, and one-byte
   stem rejection prove injectivity. Collision rejection remains defensive
   unreachable code and names both files if that invariant is ever violated.
   A broken file is skipped and the rest load; scan order is bytewise and
   stable.
9. Header: all five fields parsed; unknown schema version skipped; a
   `keymap:` mismatch demonstrably changes nothing about execution (a macro
   recorded under a rebound keymap replays byte-identically under the
   default one — the sprint's headline claim, as an executable test).
10. `--batch`: replay, list, and library work headless; `ed.macro.record`
    refuses naming itself per Sprint 37's contract.
11. Perf: 200-file library scan ≤ 5 ms; committed baseline; startup budget
    unchanged.
12. Pty goldens for the indicator (2 widths), the edit flow, and the browser
    are byte-stable across the determinism lane's double run.
13. ≥ 90 assertions across the two unit files and ≥ 15 script tests. Sprints 41 and 55 delivered the deferred integrations: the Fletch language pack highlights macro scratch buffers, and `yew pkg` owns distribution of macro collections. Sprint 38 intentionally exposed no fictional staging commands for either owner.
