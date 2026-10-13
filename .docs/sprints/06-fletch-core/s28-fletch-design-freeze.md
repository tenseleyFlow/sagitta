# Sprint 28: Fletch Design Freeze — The Language Spec

## Prerequisites

- Sprint 0 — diag module (caret rendering, `file:line:col` format), exit-code
  contract, `yew_log`/`YEW_BUG`. The spec cites these conventions; it does
  not depend on their code.
- Sprint 2 — UTF-8/grapheme semantics: the spec's string type is defined in
  terms of s2's grapheme segmentation.
- Sprint 13 — the named-command registry (`ed.*`). Assumption (its index
  entry implies but does not enumerate): every key binding resolves to a
  registry command name. The motion vocabulary below mirrors those keys;
  s35's round-trip harness verifies the mirror against the real keymap.
- Binding plan docs: `.docs/plan/02-fletch.md` (the eight design
  requirements bind every section below), `00-decisions.md` (Fletch table,
  invariant 10), `overview.md` MODES (the keyboard the terse layer mirrors).
- Campaign note: this sprint produces **a document, no code**. Campaign 06
  may run any time after Sprint 2; only the keymap-mirror claim is verified
  later (s35).

## Goals

Freeze the Fletch language: write `.docs/fletch-spec.md`, the single
citable definition of grammar, tokens, motion vocabulary, semantics,
scoping, modules, errors, pure-literal mode, and the capability model.
Frozen means: section numbers are stable forever; Sprints 29–33 implement
*sections*, the conformance suite (s33) is organized *by section*, and
later campaigns cite `fletch-spec §N` — never vibes. Post-freeze changes go
through the §16 amendment log only. Implementation starts in Sprint 29;
editor API surface (buffer/cursor/span/window handles, `bind`, `set`, `on`)
is *named* here but specified and landed in Sprint 34; the recorder in
Sprint 35; `--batch` in Sprint 37.

## Deliverables

### 1. `.docs/fletch-spec.md` — frozen section skeleton

| § | Title | Implemented by |
|---|---|---|
| 1 | Lexical structure (tokens, escapes, comments, numbers) | s29 |
| 2 | Grammar (complete EBNF) | s29 |
| 3 | Motion literals (the terse layer) | s29 lex/parse, s34 dispatch |
| 4 | Values and types | s30 |
| 5 | Expressions and operators | s30 |
| 6 | Statements | s30 |
| 7 | Scoping and closures | s30 |
| 8 | Functions | s30 |
| 9 | Errors | s30/s32 |
| 10 | Transactions (`edit`) | s30 ops, s34 semantics |
| 11 | Modules and import | s31 |
| 12 | Pure-literal mode | s29 |
| 13 | Capability model | s31 io, s34/s54 grants |
| 14 | Worked example | s33 fixture |
| 15 | Reserved words and style | all |
| 16 | Amendment log | (empty at freeze) |

Every normative sentence lives under a numbered section. The spec carries
its own conformance hook: each § ends with a "Conformance:" line naming the
`tests/fletch/` file(s) that will cover it (paths reserved now, files land
s33).

### 2. §1–2: token inventory + EBNF

The spec embeds this grammar verbatim (spec may refine, not restructure):

```ebnf
program      = { stmt } EOF ;
stmt         = let_stmt | assign_stmt | fn_decl | macro_decl | import_stmt
             | if_stmt | while_stmt | for_stmt | return_stmt | "break" TERM
             | "continue" TERM | edit_stmt | try_stmt | expr_stmt ;
let_stmt     = "let" IDENT [ "=" expr ] TERM ;
assign_stmt  = target "=" expr TERM ;
target       = IDENT | postfix "[" expr "]" | postfix "." IDENT ;
fn_decl      = "fn" IDENT "(" [ params ] ")" block ;
macro_decl   = "macro" IDENT "=" motion_block TERM ;
import_stmt  = "import" ( IDENT | STRING "as" IDENT ) TERM ;
if_stmt      = "if" expr block { "else" "if" expr block } [ "else" block ] ;
while_stmt   = "while" expr block ;
for_stmt     = "for" IDENT [ "," IDENT ] "in" expr block ;
return_stmt  = "return" [ expr ] TERM ;
edit_stmt    = "edit" block ;
try_stmt     = "try" block "catch" IDENT block ;
expr_stmt    = expr TERM ;
block        = "{" { stmt } "}" ;
params       = IDENT { "," IDENT } ;
expr         = or_e ;
or_e         = and_e { "or" and_e } ;
and_e        = eq_e { "and" eq_e } ;
eq_e         = rel_e { ( "==" | "!=" ) rel_e } ;
rel_e        = add_e { ( "<" | "<=" | ">" | ">=" ) add_e } ;
add_e        = mul_e { ( "+" | "-" ) mul_e } ;
mul_e        = unary { ( "*" | "/" | "%" ) unary } ;
unary        = ( "not" | "-" ) unary | postfix ;
postfix      = primary { "(" [ args ] ")" | "[" expr "]" | "." IDENT } ;
args         = expr { "," expr } ;
primary      = literal | IDENT | "(" expr ")" | fn_expr | motion_block ;
fn_expr      = "fn" "(" [ params ] ")" ( block | expr ) ;
literal      = "nil" | "true" | "false" | INT | FLOAT | STRING
             | list_lit | map_lit ;
list_lit     = "[" [ expr { "," expr } [ "," ] ] "]" ;
map_lit      = "{" [ entry { "," entry } [ "," ] ] "}" ;
entry        = ( IDENT | STRING | INT ) ":" expr ;
motion_block = "@[" { motion } "]" ;
motion       = [ COUNT ] motion_word ;
motion_word  = UNIT | [ "a" ] ARROW | "H" "(" { motion } ")"
             | "i" STRING | "del" | "esc" | CMDWORD ;
TERM         = NEWLINE | ";" | EOF ;
```

Pinned lexical rules the spec must state:

- Comments: `#` to end of line. No block comments.
- `TERM`: newline ends a statement **unless** the line is syntactically
  incomplete — open `(`/`[`/`{`/`@[`, or trailing binary operator /
  `,` / `=`. `;` is an explicit separator. The REPL (s32) reuses exactly
  this rule for continuation prompts.
- Integers: decimal, `0x` hex, `_` digit separators (`1_000_000`); i64;
  literal overflow is a compile error. Floats: `digits.digits[e±digits]`,
  f64. No octal, no leading-`.` floats.
- Strings: double-quoted, single line. Escapes: `\n \t \r \\ \" \0 \xNN
  \u{H..HHHHHH}`. An unknown escape is an error, never passthrough.
- Inside `@[ … ]` the lexer switches to the **motion token space** (§3);
  `l`, `v`, `del` etc. are motion words there and ordinary identifiers
  everywhere else. Pitfall: this is a lexer mode, not parser lookahead —
  pin it in §1 so s29 builds it that way.
- `{` at statement position is a hard error with the fix in the message
  ("map literal cannot start a statement; parenthesize") — blocks only
  follow keywords, so bare `{` is always a mistake.

### 3. §3: motion-literal vocabulary (the round-trip law's alphabet)

Mirrors the keyboard exactly (invariant 10). One key event = one motion
word; the recorder (s35) emits only this vocabulary, and every word maps
back to a key/registry action.

| Literal | ASCII name | Key origin | Meaning |
|---|---|---|---|
| `l` `w` `b` `c` | line/word/block/char | `L` `W` `B` keys; `c` = I-mode's char unit | switch active unit |
| `>` | right | → | unit-next / unit-end (L mode: end of line) |
| `<` | left | ← | unit-prev / unit-home (L mode: start of line) |
| `^` | up | ↑ | unit-up (L: line above; B: enclosing) |
| `v` | down | ↓ | unit-down |
| `a>` `a<` `a^` `av` | alt-right … | Alt+arrows | alt-variant (WORD/subword, grow/shrink) |
| `H( … )` | highlight | `H` … `Esc` | selection extended by contained motions |
| `i"…"` | insert | `I`, text, `Esc` | insert literal text (string escapes apply) |
| `del` | — | Delete | delete selection-or-unit |
| `esc` | — | Esc | return to L, collapse pending state |
| `N›word‹` | count | count prefix | repeat next word N times (`4>`) |
| `→ ← ↑ ↓` | — | — | Unicode aliases for `> < ^ v`: accepted on input, **never emitted by the recorder** (canonical form is ASCII) |
| CMDWORD | e.g. `yank`, `paste` | any bound key | bare identifier resolving to a registry command `ed.*` |

Semantics pinned in §3: a motion block is an expression evaluating to
`nil`; executing it requires an editor host (headless VM without one raises
kind `"motion"` — s30 default host, until s34). CMDWORD resolution happens
at run time against the command registry, so the vocabulary automatically
covers every future command — the round-trip law needs no vocabulary
freeze beyond the structural words above.

### 4. §4–8: semantics to pin (decisions, not options)

- **Types**: nil, bool, int (i64, wrapping two's complement), float (f64),
  string (immutable, UTF-8, grapheme-indexed per s2), list, map
  (insertion-ordered; keys: string | int | bool), function/closure, and
  reserved editor handles (buffer, cursor, span, window, regex — §
  reserved, constructible only from s34 on).
- **Truthiness**: `nil` and `false` are falsy; everything else truthy
  (including `0` and `""`).
- **Equality**: `==` is value equality for nil/bool/int/float/string
  (int==float compares numerically), reference identity for
  list/map/function/handle.
- **Arithmetic**: int op int → int (`/` truncates toward zero, `%` takes
  dividend's sign, ÷0 raises kind `"div"`); any float operand promotes to
  float. `+` also concatenates string+string and list+list; all other
  mixed-type arithmetic raises kind `"type"`.
- **Comparisons**: numbers numerically; strings bytewise lexicographic
  (documented as such — no collation).
- **Assignment is a statement**, not an expression; assigning to an
  undeclared name is an error (no implicit globals).
- **Scoping**: lexical. `let` binds in the innermost block; shadowing
  legal. Closures capture variables **by reference** (shared upvalue — the
  `counter` example in §14 is normative). Top-level `let`/`fn`/`macro` of a
  module are its globals.
- **`.` on maps** is sugar for string index: `e.msg` ≡ `e["msg"]`. Modules
  are immutable namespaces; `.` reads them the same way.
- **`macro name = @[…]`** is sugar for `let name = fn() @[…]`.

### 5. §9–10: error semantics

- `error(v)` raises; string argument becomes `{ kind: "user", msg: v }`.
  Every raised error is a map with at least `kind`, `msg`; the VM adds
  `trace` (list of `"fn (file:line)"` strings) when uncaught by any frame.
- Pinned runtime kinds (closed set for 1.0): `"type" "arity" "name"
  "index" "key" "div" "capability" "io" "import" "motion" "user"`.
- `try { } catch e { }` catches anything raised in the block.
- **Transaction law** (design req 4): a top-level `edit { }` opens one
  undo transaction; an error that escapes the block **rolls the buffer
  back to pre-block state, then re-raises**. Nested `edit` joins the
  enclosing transaction. Headless (no editor): `edit` is a plain block.
- Compile errors are diagnostics (never catchable); runtime errors are
  values (always catchable).

### 6. §11: modules

- `import str` — builtin (`str list map math fmt io re`). `import
  "lib/x.fl" as x` — file: resolved relative to the importing file, then
  `$XDG_CONFIG_HOME/yew/fl/`. Cached by realpath; cycles are an error
  naming the cycle. Exports: every top-level binding not `_`-prefixed.

### 7. §12: pure-literal mode

The config/state/syntax-def data format (design req 6; s25 froze the
workspace-state schema on this). Grammar subset, **parseable without
executing any code**:

```ebnf
pl_file  = pl_value EOF ;
pl_value = "nil" | "true" | "false" | [ "-" ] INT | [ "-" ] FLOAT
         | STRING | pl_list | pl_map ;
pl_list  = "[" [ pl_value { "," pl_value } [ "," ] ] "]" ;
pl_map   = "{" [ pl_entry { "," pl_entry } [ "," ] ] "}" ;
pl_entry = ( IDENT | STRING | INT ) ":" pl_value ;
```

Comments and trailing commas allowed. No identifiers (beyond keyword
literals and bare map keys), no expressions, no imports. §12 states the
security property outright: a pure-literal file grants nothing and runs
nothing, so state files are safe to load from any origin. s29 ships the
dedicated entry point.

### 8. §13: capability model

Capabilities: `fs.read`, `fs.write`, `shell`, `net`. Granted per **script
origin**, checked at the host boundary (io module s31, editor API s34,
plugins s54):

| Origin | Grant |
|---|---|
| user config (`init.fl`) | all |
| workspace `.yew.fl` | all, after the trusted-dir prompt (s36) |
| plugin | per-capability prompt, persisted (s54) |
| `yew fl` / `--batch` CLI | all (the user invoked it) |
| REPL | all |
| pure-literal data | none — never executes |

Denied access raises kind `"capability"` (catchable). No ambient
authority: the check reads the *origin of the calling function's defining
module*, not the current stack top — a plugin calling a user-config helper
gains nothing. Pin that sentence; it is the whole model.

### 9. §15: reserved words + style

Reserved (22): `and as break catch continue edit else false fn for if
import in let macro nil not or return true try while`. Motion words are
not reserved (motion token space is separate). Style: 4-space indent,
`snake_case`, `_`-prefix private, one statement per line, `#` comment
sentences. The default config (s36) and all `runtime/*.fl` must conform.

### 10. §14: the 30-line worked example

Normative, exercises every construct; becomes `tests/fletch/14-example.fl`
in s33. This exact program (spec may polish comments only):

```fletch
# fletch-spec §14 — one of everything
import str

let greet = "hi\tthere\n"                 # string escapes
let nums  = [1, 2.5, 0x10]                # list, int, float, hex
let cfg   = { tabwidth: 4, wrap: false, name: nil }

fn clamp(x, lo, hi) {
    if x < lo { return lo }
    else if x > hi { return hi }
    return x
}

fn counter(start) {                       # closure captures by reference
    let n = start
    return fn() { n = n + 1; return n }
}

let next = counter(clamp(9, 0, 8))
while next() < 10 { }                     # while (runs twice)

let total = 0
for x in nums {
    if x == 2.5 { continue }
    total = total + x
}

macro dup = @[ l< H(lv) yank v paste esc ]    # terse layer

fn shout(s) {
    try { edit { @[ 2v i"!" del ] } }
    catch e { return str.upper(e.kind) or "?" }
}
```

### 11. Freeze process + this sprint file's own deliverable

- Review checklist committed at the end of this sprint file's execution:
  every design requirement 1–8 from `02-fletch.md` cites the § that
  satisfies it (a table in the spec's preface — the traceability matrix).
- Freeze = the matrix is complete and review signs off. After freeze,
  changes append to §16 (`date, §, old → new, reason, approver`) — s33's
  conformance suite tests the *amended* spec, but amendments never
  renumber sections.

**Amendment numbering (learned the hard way during planning).** Amendments
carry a single **globally sequential id in sprint order** — `A1`, `A2`, … —
not a per-section counter. Three later sprints drafted amendments
independently and two of them both picked "A1", because a per-section
scheme (`§16-A1`) makes collision invisible until someone reads both files.
§16's table therefore has an **Id** column, and it is the registry: an
amendment is not filed until it appears there with the next free id.

Amendments already committed against this spec by later sprints — the
implementer of Sprint 28 writes these rows into §16 as part of the freeze,
so the spec ships already knowing its own future:

| Id | Sprint | § | Change | Reason |
|---|---|---|---|---|
| A1 | 30 | 9/16 | error kinds 11 → 12, adding `"limit"` | stack/step-limit exhaustion is user-triggerable and fits none of the 11 closed kinds |
| A2 | 34 | 9/16 | error kinds 12 → 13, adding `"handle"` | a stale/closed editor handle is neither a type nor an index error; `catch` must distinguish "wrong argument" from "the buffer closed under you" |
| A3 | 55 | 11 | import resolution gains a fourth row, `$YEW_RUNTIME_DIR/`, searched **last** | shipped presets must be importable, and searching last lets a user copy shadow the shipped one |

- Pitfall: **§9's kind count is 13, not 12.** s31/s32/s33 were written
  between A1 and A2 and correctly say 12 for their point in time; anything
  written after Sprint 34 says 13. s33's count assertion is updated in A2's
  own commit, and s58's audit front `FLETCH` re-checks the arithmetic.
- Defer: editor API semantics beyond names (`buf`, `win`, `bind`, `set`,
  `on`) → Sprint 34. Recorder emission rules (run folding `>>>>` → `4>`,
  mode-context naming) → Sprint 35. `--batch` stdio contract → Sprint 37.
  The spec lists these as "host-provided; see sprint N" — no semantics
  invented here that s34/s35 would have to honor blind.

## Testing Strategy

- **Document, so review is the test**: the traceability matrix
  (requirement → §) and the §-to-conformance-file map must be complete;
  reviewer checks both mechanically.
- **Grammar sanity**: run the EBNF through a hand check — every
  nonterminal referenced is defined, every token named in §2 appears in
  §1's inventory. (A throwaway grep suffices; do not build tooling.)
- **Example dry-run**: the §14 example is hand-traced in review (expected:
  `total == 17`, `next()` returns 9 then 10, `dup` defined, `shout`
  headless raises kind `"motion"` caught → returns `"MOTION"`). These
  expected values are written into the spec as the example's normative
  output — s33 asserts them.
- **Pure-literal safety argument**: §12 includes a negative list (no call,
  no ident, no import productions reachable) that s29's
  `fl_parse_literal` fuzzer will enforce.

## Definition of Done

1. `.docs/fletch-spec.md` exists with all 16 sections; §16 empty.
2. Preface traceability matrix maps all 8 design requirements from
   `02-fletch.md` to sections; no requirement unmapped.
3. §2 EBNF is complete: every nonterminal defined exactly once; every
   terminal in §1's token inventory; grep finds no `TODO`/`TBD`.
4. §3 vocabulary table covers: 4 unit prefixes, 4 arrows + 4 alt-variants
   + 4 Unicode aliases, `H()`, `i"…"`, `del`, `esc`, counts, CMDWORD —
   with key origin for each row.
5. §14 example is exactly the program above (≤ 35 lines), with normative
   expected results stated.
6. Pinned decision sentences present verbatim-checkable: no implicit
   globals; capture by reference; insertion-ordered maps; `nil`/`false`
   falsy only; int `/` truncates; assignment is a statement; pure-literal
   grants nothing; capability checks read defining-module origin.
7. Each § carries a "Conformance:" line naming its future
   `tests/fletch/` file.
8. Closed error-kind set (11 kinds) and reserved-word set (22 words)
   enumerated; counts stated in the spec so s33 can assert them.
9. Review sign-off recorded in the spec preface (date + reviewer); freeze
   declared. No code, no new directories outside `.docs/`.
