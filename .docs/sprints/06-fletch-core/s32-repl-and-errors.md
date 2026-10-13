# Sprint 32: `yew fl` — REPL, Evaluation, Error Quality, VM Fuzzer

## Prerequisites

- Sprint 28 — spec §9 (errors are values, `kind`/`msg`/`trace`; compile
  errors are diagnostics and never catchable), §1's continuation rule (the
  REPL reuses it verbatim), §13 (the REPL's origin grants all four
  capabilities).
- Sprint 29 — `fl_parse` and `FlProgram.incomplete`, set only when EOF
  arrives with open brackets or after a continuation token **and no error
  was emitted**. s29 pinned that the REPL consumes this flag, so the logic
  lives in the parser, once. Also the `DiagCtx` caret contract and the
  20-error cap.
- Sprint 30 — `FlVm`, `FlValue`, `fl_compile`, `fl_vm_run`, the line-run
  table carrying `line` *and* `col`, `FlFn.name_id` (0 = anonymous),
  `vm->step_limit`, kind `"limit"` (spec amendment §16-A1), `fl_disasm_chunk`.
- Sprint 31 — the seven builtin modules, `fl_std_list_natives`, `FlOrigin`,
  `fl_cap_origin`, module cache and cycle errors.
- Sprint 0 — `yew_bug`/`YEW_BUG` (structured report, exit 4), the exit-code
  enum `YEW_EXIT_{OK,ERR,BATCH,IO,BUG}`, `yew_log`, `Bytebuf`, `Interner`,
  `yew_sort_stable`; `src/args.c`'s subcommand plumbing (s00 reserved
  `yew fl` for this sprint).
- Sprint 3/4 — `Tty` raw mode with the restore guarantee on every exit path
  (invariant 6) and normalized `Key` events from `input.c`.
- Sprint 18 — the cmdline editor: `CmdLine` (one-line `TextBuf` + `Cursor` +
  `CompMenu` + `HistCur`), `yew_cmdline_key`, `CmdHist`
  (`yew_hist_open/add/flush/prev/next`), `CompSource`, `yew_comp_score`.
  **Assumption stated per AUTHORING**: s18's entry does not promise an
  `Ed`-free entry point. This sprint extracts one (§2) — a refactor of s18's
  widget, not a fork of it.
- Sprint 6 — the fork-under-pty harness (feed keys, snapshot the grid,
  byte-compare goldens). The REPL is tested with it.
- Binding: invariant 3 (no silent stubs), 5 (determinism), 6 (terminal
  restore), 7 (bespoke), and `00-decisions.md`'s exit-code contract.

## Goals

Ship the first user-facing Fletch surface: `yew fl` (interactive REPL) and
`yew fl FILE` (evaluate a script), both with the exit-code contract; make
runtime errors *good* — stack traces with function names, call sites, and
`line:col`, plus "did you mean" suggestions over in-scope names with a
pinned distance metric and candidate ordering; land the VM fuzzer with an
explicit, defended decision about random bytecode; and make every internal
VM invariant break a structured `yew_bug` report rather than a crash. The
milestone this feeds (Sprint 33) is Fletch hello world. Deferred: `--batch`
and script `argv` (Sprint 37), editor handles and the real host (Sprint 34 —
motion blocks still raise `"motion"`), the recorder (Sprint 35), the
conformance suite and perf gates (Sprint 33).

## Deliverables

### 1. `yew fl` — the CLI surface

```
yew fl                      # REPL (stdin a tty) or read-stdin-as-script (not a tty)
yew fl FILE                 # evaluate FILE
yew fl -e EXPR              # evaluate EXPR, print its value if non-nil
yew fl -c FILE              # parse + compile only; no execution
yew fl --dump-ast FILE      # s29's dumper, promoted from the hidden flag
yew fl --dump-bytecode FILE # fl_disasm_chunk for every function
yew fl --list-natives       # one "module.name" per line (s31; s33 consumes it)
```

Exit-code contract (`00-decisions.md`; `yew fl` **never** returns 2 — that
code belongs to `--batch`, Sprint 37):

| Situation | Code |
|---|---|
| script ran to completion; REPL exited via `:quit` or EOF | 0 |
| compile diagnostics emitted | 1 |
| uncaught runtime error | 1 |
| `FILE` unreadable, or a `capability`/`io` error escaping to the top | 3 |
| internal VM invariant break (`yew_bug`) | 4 |

Uncaught runtime errors print the §5 trace to **stderr**; `io.print` output
goes to stdout, so a script's data and its diagnostics never interleave in a
pipe. Origin is `FL_ORIGIN_CLI` (spec §13: all four capabilities — the user
invoked it) except the REPL, which is `FL_ORIGIN_REPL`, also all four.

Pitfall: `yew fl` runs with **no terminal ownership** in file mode — no
alternate screen, no raw mode, no `Tty` at all. Only the interactive REPL
touches termios, and it installs the s03 restore path before the first byte
of output (invariant 6 applies to `yew fl` exactly as it applies to the
editor).

### 2. REPL line editing — reuse, do not re-implement

s18's law is *there is exactly one text editor in this program*, and it
holds here. The prompt's text is a `TextBuf`, its caret is a `Cursor`, and
its editing keys are the `YEW_MODE_E` bindings. What blocks direct reuse is
that `yew_cmdline_key` takes an `Ed *`, and `yew fl` has no editor. The fix
is a seam, not a second widget:

```c
/* src/ui/cmdline.h — extracted from yew_cmdline_key this sprint. Pure
 * editing: motion, insert, delete, history walk. No Ed, no completion
 * sources, no accept semantics. */
bool yew_cmdline_edit_key(CmdLine *cl, CmdHist *h, const Key *k);
/* yew_cmdline_key(Ed*, Key*) becomes a thin wrapper: edit_key, then the
 * Ed-dependent parts (completion sources, accept, cancel, err rendering). */
```

`CompSource` gains a `void *ud` passed to `enumerate` — a one-field
widening so a completion source can be driven by an `FlVm` instead of an
`Ed`. Both changes are refactors of s18 with its unit tests unchanged;
DoD 3 asserts s18's suite still passes untouched.

The REPL then composes: s03 `Tty` raw mode → s04 `Key` decoding →
`yew_cmdline_edit_key` → `CmdHist` opened on kind `"fl"` (a fourth history
file under `$XDG_STATE_HOME/yew/history/`) → Fletch completion (§4).

**Non-tty stdin** (`yew fl < script.fl`, or a pipe): no raw mode, no
prompt, no history, no editing — read all of stdin and evaluate it as one
script. Pin this; a REPL that emits escape sequences into a pipe is how
CI logs become unreadable.

### 3. Multiline continuation

```
fl> fn f(x) {
...     return x + 1
... }
fl> f(41)
42
```

The loop accumulates raw lines into a `Bytebuf` and, after each line, calls
`fl_parse` over the **entire accumulated text** with a *silenced* `DiagCtx`
(a capture sink, not the printing one):

| Parse outcome | Action |
|---|---|
| `!incomplete && !had_error` | compile + run, print per §6, clear the buffer |
| `incomplete && !had_error` | print the `... ` continuation prompt, keep reading |
| `had_error` | re-parse once with the printing sink so carets render, then clear the buffer |

Three pitfalls, each a real REPL bug:

- **Re-parse the whole text, never incrementally.** `incomplete` is a
  property of the full source; deciding it per line makes `}` on its own
  line look like an error.
- **Silence the trial parse.** Printing the intermediate parse's diagnostics
  means every multiline entry emits a bogus `unexpected end of input` on
  each line.
- s29 pinned that a syntax error inside an open bracket sets `had_error`,
  not `incomplete`. Trusting `incomplete` alone makes the REPL wait forever
  for a `)` the user already typed wrong.

Key handling at the top level: `Ctrl-C` discards the pending buffer and
returns to the `fl> ` prompt (never exits); `Ctrl-D` on an empty buffer
exits 0; `Ctrl-D` with a pending buffer discards it like `Ctrl-C`; `Ctrl-L`
clears the screen. The prompt strings are `fl> ` and `... `, pinned because
the pty goldens compare them byte-exactly.

### 4. `:`-commands

Recognized only when the *pending buffer is empty* and the line's first
non-space byte is `:`. They are REPL-only — a `.fl` file starting with `:`
is an ordinary syntax error, which is correct and needs no special case.

| Command | Effect |
|---|---|
| `:help` | one screen: the `:`-command table, the prompt keys, and a pointer to `.docs/fletch-spec.md` |
| `:help NAME` | the `sig` string and one-line doc of a native (`:help str.slice`), or the spec § for a keyword |
| `:quit` / `:q` | exit 0 |
| `:load PATH` | evaluate `PATH` in the current VM; its globals persist (this is how you iterate on a config) |
| `:reload` | re-evaluate the last `:load`ed path |
| `:globals` | user globals only (builtin modules excluded), insertion order, `name = repr` truncated per §6 |
| `:disasm NAME` | `fl_disasm_chunk` for the named function |
| `:caps` | the current origin and its granted capabilities |

Unknown `:`-command → `unknown command ':gloabls'` plus a §7 suggestion
over the eight names. Completion (Tab) offers, in this order: `:`-commands
when the line starts with `:`, then in-scope locals and globals, then
`module.` members after a dot, then builtin module names — enumerated
deterministically from insertion-ordered maps, ranked by
`yew_comp_score`.

### 5. Result printing

| Value | Printed as |
|---|---|
| `nil` | **nothing at all** — no blank line |
| everything else | `fmt.repr(v)` (s31), so strings are quoted and escaped |

Why `repr` rather than `str`: at a REPL the difference between the string
`"a\nb"` and a two-line result is information the user needs, and `repr`
output can be pasted straight back in. Why nothing for `nil`: otherwise
every `let`, every `io.print`, and every void call spams a line.

Bounds, all pinned because a REPL that hangs printing is worse than one
that truncates:

- Output over 2 000 bytes is elided: `… (14 823 more bytes)`.
- Containers nested deeper than 8 levels print `…` at the cutoff.
- **Cycles are detected** with a visited-object set: a list containing
  itself prints `[[...]]`, a map `{a: {...}}`. One `let l = []` and
  `list.push(l, l)` is all it takes, and without this the REPL recurses
  until `yew_bug`.
- No magic `_` binding for the last result. Explicit `let` is the whole
  language; a hidden global would be the only name in Fletch nobody
  declared.

### 6. Runtime error quality — stack traces

Spec §9: every raised error is a map with `kind` and `msg`; the VM adds
`trace` (a list of `"fn (file:line:col)"` strings) when it escapes every
frame. The printed form:

```
error: type: cannot add int and string
  at surround        (init.fl:12:9)
  at <fn>            (init.fl:31:5)
  at <module init>   (lib/edit.fl:4:1)
  at <repl>          (repl:1:1)

  12 |     let s = c.span() + 1
     |             ~~~~~~~~~^
```

Frame naming (a closed table — s33 asserts every row):

| Frame | Name printed |
|---|---|
| named `fn f` | `f` |
| `fn(…)` expression | `<fn>` |
| `macro m = @[…]` | `macro m` |
| a module's top-level chunk | `<module init>` |
| the REPL's entry chunk | `<repl>` |
| a `yew fl FILE` entry chunk | `<script>` |
| a native | `<native str.upper>` |

Rules:

- The site is the **call instruction's own** `line:col`, read from the
  chunk's line-run table (s30) at the pc the VM saved into `frame->ip`
  *before* dispatching the `CALL`. Pitfall: reading the live `ip` gives the
  instruction *after* the call, which lands on the next source line often
  enough to be maddening and rarely enough to survive review.
- Traces longer than 32 frames print the first 16, `… 214 frames elided …`,
  and the last 16. Infinite recursion (kind `"limit"`) must not print 256
  frames.
- The caret block renders only the **innermost** frame, through the s00
  diag machinery, and only when the source text is still available:
  `FlSrcTab` maps `file_id → (path, source bytes)` and is populated by every
  entry point (REPL line, `-e` string, loaded file). A file changed on disk
  since it was compiled is detected by size+mtime and the caret is
  **skipped**, never guessed — pointing a caret at the wrong line is worse
  than no caret.
- The trace is built when the error escapes the outermost frame, not at
  raise time: a `try`/`catch` in a tight loop must not pay for string
  formatting it will discard.
- Errors caught by a `catch` carry `kind` and `msg` only, per spec §9.

### 7. "Did you mean" — pinned metric and ordering

Applies at four sites, each with its own candidate set:

| Site | Candidates |
|---|---|
| compile-time `undefined variable 'x'` | locals in scope, enclosing-function locals reachable as upvalues, module globals, the seven builtin module names |
| runtime `"name"` on a module member | that module's keys (insertion-ordered) |
| runtime `"key"` on a map read | that map's string keys |
| unknown `:`-command | the eight `:` names |

Metric: **Damerau–Levenshtein (optimal string alignment)** — transposition
matters because `lenght`/`length` is the most common real typo, and OSA is
the cheap variant (no full DL alphabet table). Costs are scaled ×2 so a
half-cost case can be expressed in integers:

| Edit | Cost |
|---|---|
| insert / delete / substitute | 2 |
| adjacent transposition | 2 |
| substitution differing only in case | 1 |

Acceptance threshold, by candidate length (the *candidate*, not the typo):

| Length | Max scaled distance |
|---|---|
| ≤ 2 | never suggest |
| 3 | 2 (one edit) |
| ≥ 4 | 4 (two edits) |

Why the length floor: at length 2, everything is within one edit of
everything, and "did you mean `if`?" for `in` is noise that trains users to
ignore the whole feature.

Candidate ordering — deterministic, and the tie-breaks are the point:

1. ascending scaled distance;
2. then **scope proximity**: innermost locals → enclosing locals/upvalues →
   module globals → builtin module names (a shadowed local is what the user
   most likely meant);
3. then byte-lexicographic via `yew_sort_stable`.

At most 3 are printed: `did you mean 'total'? (or 'totals', 'tally')`.
Cost bound: the candidate set is capped at 512 names; above that, prefix-
filter on the first byte (case-insensitively) before scoring, and if still
above, suggest nothing. Early-exit the DP when the running row minimum
exceeds the threshold.

**Pitfall:** scoring against every interned string in the process — the
easy implementation, since the `Interner` is right there — suggests private
helpers, module-internal names, and string literals. The candidate set is
per-site by construction, and DoD 7 asserts a fixture where an out-of-scope
name that is one edit away is **not** suggested.

### 8. The VM fuzzer — and the bytecode decision

**Decision: no bytecode verifier. Random bytecode never enters the VM,
because no code path can put it there.**

The alternatives were a load-time verifier (validate jump targets, constant
indices, stack effects, operand ranges before running) and fully defensive
dispatch (bounds-check every push, pop, slot, and constant on every
instruction). Both were rejected for 1.0:

- **There is no bytecode loader.** Fletch has no `.flc` format, no compile
  cache, no serialization of `FlChunk`. The only producer of bytecode is
  `fl_compile`, in-process, from an AST that already parsed. A verifier
  would guard a door that does not exist, cost ~600 lines, and — worse —
  create a *second* definition of well-formed bytecode that must be kept in
  sync with the compiler forever. Two definitions drift; the drift is
  discovered by the thing they were supposed to prevent.
- **Defensive dispatch costs the hot loop.** `02-fletch.md` req 7 wants ~1 µs
  motion dispatch and < 1 ms config load. s30 already bought memory safety
  the right way: `max_stack` is computed at compile time from the opcode
  effect table and checked **once per call**, so pushes need no checks.
- **Source-level fuzzing is the primary line of defence** and it is already
  where the attack surface is: s29 fuzzes the lexer and parser, and this
  sprint fuzzes source → compile → run. Every byte an adversary controls
  passes through the parser first.

What ships instead:

```c
/* src/fl/vm.h — a COMPILER ASSERTION, not a security boundary. Runs in
 * debug/sanitize/fuzz builds after every fl_compile; compiled out of
 * release. It validates OUR compiler's output. If yew ever grows a
 * bytecode cache or a .flc format, a REAL verifier lands with it, in the
 * same commit — nothing may load foreign bytecode without one. */
#if FL_VM_CHECKS
bool fl_chunk_check(const FlChunk *ch, const char **why);
#endif
```

`fl_chunk_check` walks the instruction stream decoded by operand width and
asserts: every jump target lands on an instruction start and inside the
chunk; every constant/local/upvalue index is in range; the running stack
depth never goes negative and its high-water mark equals `max_stack`; every
path reaches a terminator. A failure is `YEW_BUG` (§9): it means our
compiler is broken, which is not a user error.

```c
/* tests/fuzz/fuzz_fl_vm.c — stdin bytes → parse → compile → run.
 * Deny-all host (fl_host_null), zero capabilities (io is unreachable),
 * step limit 5e6, arena high-water cap 64 MiB, alarm(5). */
```

Per-input invariants: no crash, no ASan/UBSan report, no hang; the run ends
in exactly one of {value, raised error with a kind from the closed 12, step
limit → `"limit"`, compile diagnostics ≤ 20}; `vm->sp` is back at
`vm->stack` and `nframes == 0` and `nhandlers == 0` after an uncaught raise
(the unwinder must leave the VM reusable); GC-stress mode on for one run in
sixteen. Seed corpus: every s29 fixture, every s30 golden, the spec §14
example, and minimized crashers under `tests/fuzz/corpus/fl_vm/`.

`fl_vm_set_step_limit` is also the seam Sprint 54's plugin sandbox uses to
bound a runaway plugin — noted so nobody adds a second timeout mechanism.

### 9. `yew_bug` on VM invariant breaks

Internal invariants are separated from user errors by one question: *can a
Fletch program cause this?* If yes it raises a catchable error (kind
`"limit"` for resource exhaustion); if no, it is a bug in yew and gets
the structured exit-4 report. Never a bare crash, never a silent recovery.

| Invariant | Detected where |
|---|---|
| stack underflow (`sp < frame->slots`) | `FL_VM_CHECKS` per instruction; release: implied by `max_stack` |
| `sp` above `stack + FL_STACK_MAX` | once per `CALL` (release too) |
| unknown opcode | switch `default:` / the padded goto table's `L_BAD_OP` |
| frame underflow on `RETURN` | `RETURN` |
| upvalue index ≥ `cl->nup` | `FL_VM_CHECKS` |
| constant index ≥ `nconsts` | `FL_VM_CHECKS` |
| handler stack under/overflow | `TRY_PUSH`/`TRY_POP` |
| GC reached a reachable-but-white object | the stress lane's post-mark audit |
| `fl_chunk_check` failure | after `fl_compile` in checked builds |

```c
/* Report format — one line per fact, so a paste into an issue is complete.
 * Built in a STATIC 4 KiB buffer with no allocation and no FlValue
 * formatting: yew_bug may fire mid-collection, when the heap is not a
 * thing you may touch. */
yew: internal error at src/fl/vm.c:812: fl vm: stack underflow
  opcode : ADD (0x12) at pc 0x0047
  fn     : surround/2  chunk init.fl (file_id 3)
  frames : 4 (depth cap 256)   sp offset: -1   handlers: 1
  build  : 1.0.0-dev gcc-14 cgoto=1
  hint   : YEW_FL_DUMP_BAD_CHUNK=<path> writes a disassembly
```

`YEW_FL_DUMP_BAD_CHUNK=path` writes `fl_disasm_chunk` of the offending
function to `path` before exiting 4 (Cgfried's `CGF_DUMP_BAD_IR` pattern);
the dump must itself be re-readable, which the tests assert. Exit is
`YEW_EXIT_BUG` (4) through `yew_bug`, the only site allowed to call
`exit()` (s00's contract; `scripts/bans.sh` already enforces it).

### 10. Defer

- **`--batch`, script `argv`, the stdio contract, exit code 2** →
  Sprint 37. `yew fl --batch` is not an alias and does not exist; `yew
  --batch` continues to hard-error naming Sprint 37.
- **Editor handles and real motion execution** → Sprint 34: the REPL runs
  against `fl_host_null`, so `@[ … ]` raises `"motion"` with the trace
  showing `<repl>`. `bind`/`set`/`on` raise `"name"` naming Sprint 34
  (s31's deferred natives).
- **The recorder** → Sprint 35; **the conformance suite and perf gates** →
  Sprint 33 (this sprint adds no baselines).
- **A stepping debugger, breakpoints, `:step`** → not in 1.0. The trace plus
  `:disasm` plus `io.print` is the debugging story, and it is stated in the
  `:help` text so nobody looks for a hidden flag.
- **Bytecode serialization** → not in 1.0; §8's comment in `vm.h` is the
  binding record of why, and of what must land with it if it ever does.

## Testing Strategy

- **Unit — continuation** (`tests/unit/test_fl_repl.c`): drive the REPL's
  line-accumulation function directly with the s29 `incomplete` matrix
  (`fn f(` → continue, `fn f((` → continue, `fn f()}` → error not continue,
  trailing `+` → continue, `+ 1` → complete) plus a 12-line nested entry;
  assert the trial parse emits nothing to the printing sink.
- **Unit — traces** (`test_fl_trace.c`): every row of §6's frame-naming
  table; a call chain 5 deep asserting each `line:col` against a fixture
  whose call sites sit at known columns; a 300-frame recursion asserting the
  elision line and exactly 32 printed frames; a source-modified-on-disk case
  asserting the caret block is omitted and the trace still prints.
- **Unit — suggestions** (`test_fl_didyoumean.c`): a table of ≥ 25
  (typo, in-scope names, expected suggestion list) rows covering the length
  floor, the case-only half cost, transposition, the 3-suggestion cap, the
  scope-proximity tie-break (a global and a local at equal distance → the
  local first), and a **negative**: an out-of-scope name one edit away is
  not suggested. Ordering asserted exactly, not as a set.
- **Pty** (`tests/pty/fl_repl_*.golden`, s06 harness): a session that types
  a multiline function, a syntax error, a `:globals`, a Tab completion, a
  `Ctrl-C` mid-entry, and `Ctrl-D`; grid snapshots byte-compared. A second
  golden asserts the terminal is restored after a `yew_bug` forced with a
  test-only opcode injection.
- **Script/CLI** (`scripts/smoke.sh` additions): each exit-code row of §1
  driven once — success 0, compile error 1, uncaught error 1, missing file
  3, `--selftest-fl-bug` 4; `yew fl < file` (non-tty) evaluates without
  emitting escape sequences (assert stdout is escape-free);
  `yew fl -e 'io.print("hi")'` prints exactly `hi`.
- **Fuzz**: `fuzz_fl_vm` 10 min in the ASan lane and 10 min under GC stress,
  zero findings; corpus replay in `make test` (seconds); the forward-progress
  and VM-reusable-after-raise invariants asserted per input.
- **Determinism lane**: run the same script twice and byte-compare stdout
  *and* stderr, including a trace and a suggestion (suggestion ordering is
  the part that rots first).
- **Regression fixtures**: every crasher the fuzzer finds is minimized into
  `tests/fuzz/corpus/fl_vm/` and becomes a named unit case before its fix
  merges (s02's rule).

## Definition of Done

1. `src/fl/repl.c|h` plus the `yew fl` subcommand build in both compiler
   lanes with zero warnings; sanitize, valgrind, determinism, GC-stress and
   both `FL_CGOTO` settings are green.
2. All seven `yew fl` invocation forms in §1 work; `--help` documents them
   and `scripts/smoke.sh` covers every exit-code row (0, 1, 1, 3, 4).
3. s18's unit suite passes **unchanged**; the `CompReq.ud` widening lands;
   `grep -rn 'char .*\[512\]' src/fl/repl.c` finds nothing — there is no
   second line editor. **(amended, C1)** The prompt shares the TEXT layer
   (TextBuf, Cursor, `yew_edit_*`, grapheme motion) rather than s18's key
   handler, so there is no `yew_cmdline_edit_key` extraction and s18 is
   untouched.
4. The s29 `incomplete` matrix drives the REPL correctly (5 pinned cases +
   the 12-line entry); the trial parse emits zero output to the printing
   sink, asserted via a capturing sink.
5. Pty goldens for the REPL session and for the post-`yew_bug` terminal
   restore are committed and byte-exact; a second run of the harness is
   byte-identical. **(amended, C2)** The harness VT models only the
   ALTERNATE screen's grid, so a primary-screen child's goldens pin
   `alt=0`, the cursor and the modes; the session's content is asserted
   against the byte log with `ptc_expect_output`/`ptc_reject_output`.
6. Trace format: every row of the frame-naming table asserted; call sites
   are the CALL instruction's own `line:col` (fixture with known columns);
   elision fires at 32 frames.
7. Suggestions: ≥ 25 table rows green, ordering asserted exactly, and the
   out-of-scope negative case passes. No site scores against the global
   interner — `grep -n 'yew_intern_str' src/fl/` shows no use inside the
   suggestion code.
8. `fuzz_fl_vm` runs 10 min ASan-clean and 10 min GC-stress-clean; every
   per-input invariant is checked; after an uncaught raise the VM satisfies
   `sp == stack && nframes == 0 && nhandlers == 0`; `tests/fuzz/crashes/`
   is empty; corpus committed and replayed by `make test`.
9. `fl_chunk_check` is present, runs in checked builds after every compile,
   and each of its four checks has a unit test that fires it via a
   deliberately corrupted chunk (test-only mutator, never a shipped API).
10. Every §9 invariant row produces the structured report (driven by a
    test-only injection hook), exits 4, and `YEW_FL_DUMP_BAD_CHUNK` writes a
    dump that `fl_disasm_chunk`'s own reader accepts. No bare `abort()` or
    `assert()` in `src/fl/` — `scripts/bans.sh` gains the rule.
11. `vm.h` carries the no-verifier decision and its condition (any future
    bytecode loader lands with a real verifier, same commit) as a comment
    with the why, and this file's §8 is cited by section.
12. Deferred surfaces hard-error naming their sprint: `yew --batch` →
    Sprint 37; `bind`/`set`/`on` → Sprint 34; `@[…]` raises `"motion"` at
    the REPL with a trace naming `<repl>`.

### Corrections after landing

**C1, 2026-08-09** — DoD 3 amended. §2 states that "what blocks direct
reuse is that `yew_cmdline_key` takes an `Ed *`". That is not what
blocks it. `yew_cmdline_key` handles only printable insertion, and it
inserts by calling `yew_ed_invoke(ed, "ed.edit.insert.text", &cx)`;
every editing key is a `YEW_MODE_E` binding dispatched through the same
Sprint 19 registry. Reuse therefore needs the registry, and the
registry needs an `Ed` — a dependency `yew fl` should not acquire for
one text field, and one that could only be removed by reopening s19's
contract across every command implementation.

The law §2 is defending — exactly one text editor in this program — is
kept at the layer where it means something: the prompt's line is a
`TextBuf`, its caret is a `Cursor`, its edits go through
`yew_edit_insert`/`yew_edit_delete`, and its motion is s02's
grapheme-aware `yew_cursor_*`, so an emoji deletes in one keystroke
because the editor's own code says where the cluster starts. What is
not shared is command dispatch, which is an editor concern. `EditCtx`
carries no undo tree and no journal; `edit.c` already treats both as
optional, so this is composition and not a special case.

Consequences: s18 is untouched and its suite passes by construction;
`CompReq.ud` still lands, since a completion source driven by the VM
rather than an `Ed` needs it either way.

**C2, 2026-08-09** — DoD 5 amended. `yew fl` deliberately does not take
the alternate screen: a session belongs in the scrollback next to the
shell history that produced it. The pty harness's VT only renders cells
for the alternate screen — primary-screen text goes to a byte log — so
a grid golden for the prompt would be empty. Rather than rewrite a
harness that ~200 landed goldens depend on, the session case pins the
grid properties that survive (`alt=0`, cursor, modes — which is itself
the assertion that the prompt never took the screen) and asserts the
content against the byte log. `ptc_no_altscreen` is added so settling
does not wait for a `smcup` that is never coming.

The bug case snapshots the prompt BEFORE the fault, which the selftest
now waits for a keypress to trigger: §9's report carries a `vm.c` line
number, and freezing that into a grid would rot the golden on the next
edit to the VM. The report is checked as bytes instead, along with the
ordering that matters — the restore blob precedes it, because
`yew_bug`'s prehook hands the terminal back before it prints.

### DoD walk, 2026-08-09

| # | verdict | evidence |
|---|---|---|
| 1 | met | gcc and clang warning-free; sanitize, determinism, GC-stress, both `FL_CGOTO` settings and valgrind green |
| 2 | met | all seven forms exercised; exit rows 0/1/1/3/4 checked; `smoke.sh` green |
| 3 | met, **amended C1** | s18 untouched, its suite passes; `CompReq.ud` landed; the `char[512]` grep is empty |
| 4 | met | `test_fl_repl.c` drives the s29 matrix; silence asserted through the fixture's capturing sink |
| 5 | met, **amended C2** | `s32_repl_session`, `s32_bug_restores_the_terminal`; both stable across the runner's double execution |
| 6 | met | every frame-naming row; call sites from the CALL's own line:col; elision at exactly 32 |
| 7 | met | 27 CHECK rows + the metric-cost table; ordering exact; out-of-scope negative; no `yew_intern_str` in `suggest.c` |
| 8 | met | 10 min ASan-clean, 10 min GC-stress-clean, `crashes/` empty, corpus committed and replayed |
| 9 | met | `fl_chunk_check` runs under `FL_VM_CHECKS` after every compile; five tests via the test-only mutator |
| 10 | met | structured report, exit 4, `YEW_FL_DUMP_BAD_CHUNK` round-trips; no bare `abort()`/`assert()` in `src/fl/` |
| 11 | met | `vm.h` carries the no-verifier decision and its condition |
| 12 | met | `--batch` → Sprint 37; `bind`/`set`/`on` → Sprint 34; `@[…]` raises `motion` with a `<repl>` trace |

**Seven defects found by the sprint's own tests**, five of them in code
this sprint wrote: the VM stack-slot leak (fuzzer), the parser
diagnostic bound (fuzzer), the REPL history leak (sanitize + valgrind),
the first-line-only history entry (the pty case's saved history file),
and LF never submitting (the valgrind lane, by being slow enough to
lose the cooked-mode race). Two were in the harness rather than the
product: `YEW_PTY_EXCLUDE` matched a single substring so a skip list
failed open, and `ptc_expect_output` named no bytes on failure.
