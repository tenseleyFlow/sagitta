# Fletch — Language Specification

**Version:** 1.0 (frozen)
**Status:** FROZEN — see §16 for the amendment process.
**Frozen by:** Sprint 28.

---

## Preface

Fletch is the scripting and macro language of the sagitta editor. This
document is its single citable definition. Sprints 29–33 implement
*sections* of it; the conformance suite (Sprint 33) is organised *by
section*; every later campaign cites `fletch-spec §N` rather than
describing behaviour again.

### What "frozen" means

**Section numbers are stable forever.** A section is never renumbered,
never merged, never deleted. Post-freeze changes are appended to §16's
amendment log and nowhere else — an amendment may change a section's
*content*, never the *numbering* around it, because 29 later sprints
cite these numbers by hand and a renumber silently redirects all of
them.

### Traceability matrix

Every design requirement in `.docs/plan/02-fletch.md` maps to the
sections that satisfy it. No requirement is unmapped; this table is the
mechanical check a reviewer runs.

| # | Design requirement | Satisfied by |
|---|---|---|
| 1 | **Round-trip law** — recorder output is valid Fletch; running it reproduces the recorded edit exactly | §3 (the vocabulary the recorder may emit), §1 (motion token space), §2 (`motion_block` grammar), §14 (the normative example) |
| 2 | **Terse motion layer** — motion literals use the keymap's own vocabulary in ASCII, arrows accepted as aliases | §3 (full table with key origin per row), §1 (lexer mode switch inside `@[ … ]`) |
| 3 | **Real language** — lexical scoping, first-class functions/closures, modules, proper errors with line/col diagnostics | §7 (scoping, capture by reference), §8 (functions), §11 (modules), §9 (errors and diagnostics) |
| 4 | **Transactional edits** — a top-level invocation is one undo step; uncaught error rolls back | §10 (the transaction law), §9 (what escaping means) |
| 5 | **Editor values** — buffer, cursor, span, window, regex are first-class handles | §4 (the handle types, reserved here, constructible from Sprint 34) |
| 6 | **Data literals** — clean enough to serve as the config, syntax-definition and workspace-state format | §12 (pure-literal mode and its security property), §4 (map ordering), §5 (literal syntax) |
| 7 | **Embeddable & small** — VM inside sagitta, ~0 start cost | §2 (a grammar with no ambiguity requiring backtracking), §4 (value model sized for a register VM), §11 (import caching) |
| 8 | **Sandboxable** — fs/shell/net are capabilities granted per script origin | §13 (the capability model and the defining-module rule), §12 (pure-literal grants nothing) |

### Review sign-off

| Date | Reviewer | Verdict |
|---|---|---|
| 2026-08-06 | Sprint 28 implementation review | Sections 1–16 present; matrix complete; grammar closed (every nonterminal defined once, every terminal in §1's inventory); §14 hand-traced. **FROZEN.** |

### Reading conventions

- **Normative** sentences use "must", "is", "raises". Rationale is
  marked as such and binds nothing.
- Every section ends with a **Conformance:** line naming the
  `tests/fletch/` file that covers it. Those paths are reserved now; the
  files land in Sprint 33.
- Where a section names a host-provided surface without specifying it,
  it says "host-provided; see Sprint N". Nothing is invented here that a
  later sprint would have to honour blind.

---

## §1 Lexical structure

Source is UTF-8. Invalid bytes are a compile error; the decoder's
escape policy (Sprint 2) applies to *buffer text*, not to program text.

### 1.1 Comments

`#` begins a comment that runs to end of line. **There are no block
comments.** Rationale: block comments require nesting rules, and every
language that has them has a nesting bug.

### 1.2 Terminators

```
TERM = NEWLINE | ";" | EOF
```

A newline ends a statement **unless the line is syntactically
incomplete**: an unclosed `(`, `[`, `{` or `@[`, or a trailing binary
operator, `,` or `=`. `;` is an explicit separator and always ends a
statement.

The REPL (Sprint 32) reuses **exactly this rule** to decide whether to
issue a continuation prompt. Rationale: two implementations of "is this
statement finished" drift, and the drift appears as a REPL that hangs on
input the file parser accepts.

### 1.3 Identifiers and keywords

An identifier begins with a letter or `_` and continues with letters,
digits or `_`. The 22 reserved words are enumerated in §15 and may not
be used as identifiers.

### 1.4 Numbers

- **Integers**: decimal, or `0x` hexadecimal. `_` may separate digits
  (`1_000_000`, `0xFF_FF`). Type is i64. A literal that does not fit in
  i64 is a **compile error**, never a silent wrap.
- **Floats**: `digits "." digits [ ("e"|"E") ["+"|"-"] digits ]`. Type
  is f64.
- There is **no octal** and **no leading-dot float**. Rationale: `010`
  meaning eight is a defect other languages have apologised for, and
  `.5` collides with the `.` field accessor.

### 1.5 Strings

Double-quoted, single line. A newline inside a string literal is a
compile error; use `\n`.

Escapes: `\n` `\t` `\r` `\\` `\"` `\0` `\xNN` `\u{H..HHHHHH}`.

**An unknown escape is an error, never passed through.** Rationale:
passthrough makes `"\d"` mean `\d` in one language and `d` in the next,
and the difference is invisible until a regex misbehaves.

### 1.6 The motion token space

Inside `@[ … ]` the lexer switches to a **separate token space** (§3):
`l`, `v`, `del`, `esc` and the rest are motion words there, and ordinary
identifiers everywhere else.

**This is a lexer mode, not parser lookahead.** Pinned because an
implementation that disambiguates in the parser must re-lex on backtrack,
and the two token streams then disagree about what `v` was.

### 1.7 The `{`-at-statement-position rule

A `{` in statement position is a **hard error**, and the message carries
the fix: *"map literal cannot start a statement; parenthesize"*. Blocks
only ever follow a keyword, so a bare `{` is always a mistake.

### 1.8 Token inventory

Every terminal used by §2's grammar:

| Class | Tokens |
|---|---|
| Literals | `INT` `FLOAT` `STRING` `nil` `true` `false` |
| Identifier | `IDENT` |
| Keywords | the 22 words of §15 |
| Operators | `+` `-` `*` `/` `%` `==` `!=` `<` `<=` `>` `>=` `=` |
| Delimiters | `(` `)` `[` `]` `{` `}` `,` `:` `.` `;` |
| Motion | `@[` `]` `UNIT` `ARROW` `COUNT` `CMDWORD` `H` `i` `del` `esc` `a` |
| Structural | `NEWLINE` `EOF` |

**Conformance:** `tests/fletch/01-lexical.fl`

---

## §2 Grammar

Complete EBNF. Every nonterminal is defined exactly once; every terminal
appears in §1.8.

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

Operator precedence is expressed by the cascade above: `or` binds
loosest, then `and`, equality, relational, additive, multiplicative,
unary, postfix. All binary operators are left-associative. Unary
operators are right-associative.

**Conformance:** `tests/fletch/02-grammar.fl`

---

## §3 Motion literals — the terse layer

A motion block mirrors the keyboard **exactly** (invariant 10). One key
event is one motion word; the recorder (Sprint 35) emits only this
vocabulary, and every word maps back to a key or registry action.

| Literal | ASCII name | Key origin | Meaning |
|---|---|---|---|
| `l` `w` `b` `c` | line / word / block / char | `L` `W` `B` keys; `c` is I-mode's char unit | switch the active unit |
| `>` | right | → | unit-next / unit-end (in L mode: end of line) |
| `<` | left | ← | unit-prev / unit-home (in L mode: start of line) |
| `^` | up | ↑ | unit-up (L: line above; B: enclosing block) |
| `v` | down | ↓ | unit-down |
| `a>` `a<` `a^` `av` | alt-right, alt-left, alt-up, alt-down | Alt + arrows | the alt variant (WORD/subword; grow/shrink) |
| `H( … )` | highlight | `H` … `Esc` | selection extended by the contained motions |
| `i"…"` | insert | `I`, text, `Esc` | insert literal text (§1.5 escapes apply) |
| `del` | — | Delete | delete the selection, or the unit when there is none |
| `esc` | — | Esc | return to L mode, collapse pending state |
| `N‹word›` | count | count prefix | repeat the next word N times (`4>`) |
| `→` `←` `↑` `↓` | — | — | Unicode aliases for `> < ^ v`. **Accepted on input, never emitted by the recorder** — the canonical form is ASCII |
| `CMDWORD` | e.g. `yank`, `paste` | any bound key | a bare identifier resolving to a registry command `ed.*` |

### 3.1 Semantics

A motion block is an **expression** evaluating to `nil`.

Executing one **requires an editor host**. A headless VM without one
raises kind `"motion"` (§9). Sprint 30 supplies a default host that does
so; the real host lands in Sprint 34.

`CMDWORD` resolution happens **at run time** against the command
registry. Consequence, and the reason the vocabulary needs no further
freezing: the terse layer automatically covers every command any future
sprint adds, so the round-trip law does not require a vocabulary
amendment each time the editor grows a verb.

**Conformance:** `tests/fletch/03-motion.fl`

---

## §4 Values and types

| Type | Notes |
|---|---|
| `nil` | the absent value |
| `bool` | `true` / `false` |
| `int` | i64, **wrapping** two's complement |
| `float` | f64 |
| `string` | immutable, UTF-8, **grapheme-indexed** per Sprint 2 |
| `list` | ordered, mutable, heterogeneous |
| `map` | **insertion-ordered**; keys are string, int or bool |
| `function` | first-class, closes over its environment |
| handles | `buffer` `cursor` `span` `window` `regex` — **reserved here, constructible only from Sprint 34 on** |

**Maps are insertion-ordered.** Rationale: the same requirement invariant
5 places on rendering. A map that iterates in hash order makes the
workspace-state file (§12) differ byte-for-byte between runs, and a
diffable state file is the entire point of the format.

**Strings are grapheme-indexed**, not byte- or codepoint-indexed. Index
0 is the first extended grapheme cluster.

**Conformance:** `tests/fletch/04-values.fl`

---

## §5 Expressions and operators

### 5.1 Truthiness

**`nil` and `false` are falsy; everything else is truthy** — including
`0`, `0.0`, `""` and `[]`.

Rationale: a language where `0` is falsy makes `if count` mean "count is
not zero" in one file and "count exists" in another, and the two readings
are both plausible at the call site.

### 5.2 Equality

`==` is **value equality** for `nil`, `bool`, `int`, `float` and
`string`. An `int` compared with a `float` compares **numerically**.

`==` is **reference identity** for `list`, `map`, `function` and
handles.

### 5.3 Arithmetic

- int ⊕ int → int. **`/` truncates toward zero.** **`%` takes the
  dividend's sign.** Division or modulo by zero raises kind `"div"`.
- Any float operand promotes the operation to float.
- `+` additionally concatenates string+string and list+list.
- **Every other mixed-type arithmetic raises kind `"type"`.** There is no
  implicit string/number coercion.

### 5.4 Comparisons

Numbers compare numerically. **Strings compare bytewise
lexicographically** — documented as such, with no collation. Rationale:
a locale-sensitive comparison makes a sort's output depend on the
developer's environment, which invariant 5 forbids.

**Conformance:** `tests/fletch/05-expressions.fl`

---

## §6 Statements

**Assignment is a statement, not an expression.** Consequence: `if x = 1`
does not parse, so the classic `=`/`==` typo is a compile error rather
than a silent truth.

**Assigning to an undeclared name is an error.**
**There are no implicit globals.**
A name must be introduced by `let`, by a parameter, or by a top-level
`fn`/`macro`.

`break` and `continue` are legal only inside `while` or `for`; elsewhere
they are a compile error.

`for x in expr` iterates a list's values, a map's keys, or a string's
graphemes. `for k, v in expr` iterates a map's entries in insertion
order, or a list's index/value pairs.

**Conformance:** `tests/fletch/06-statements.fl`

---

## §7 Scoping and closures

Scoping is **lexical**.

`let` binds in the innermost enclosing block. Shadowing an outer binding
is legal.

**Closures capture variables by reference — a shared upvalue.** The
`counter` function in §14 is **normative**: two calls to the returned
closure return 9 then 10, because both see the same `n`. (See §16-A4;
this sentence said "10 then 11" before Sprint 30 and contradicted the
§14.1 table it points at.)

Rationale: capture-by-value and capture-by-reference are
indistinguishable until a closure mutates, at which point they differ
permanently. Pinning the answer here means Sprint 30 does not have to
guess and Sprint 35's recorder does not have to care.

A module's top-level `let`, `fn` and `macro` bindings are its globals.

**Conformance:** `tests/fletch/07-scoping.fl`

---

## §8 Functions

Functions are first-class values. `fn name(params) { … }` is a
declaration; `fn(params) { … }` and `fn(params) expr` are expressions.

Calling with the wrong number of arguments raises kind `"arity"`.

A function with no explicit `return` evaluates to `nil`.

`macro name = @[ … ]` is **sugar for** `let name = fn() @[ … ]`.

`.` **on maps is sugar for string index**: `e.msg` is exactly
`e["msg"]`. Modules are immutable namespaces read the same way.

**Conformance:** `tests/fletch/08-functions.fl`

---

## §9 Errors

`error(v)` raises. A **string** argument becomes
`{ kind: "user", msg: v }`.

Every raised error is a map carrying at least `kind` and `msg`. The VM
adds `trace` — a list of `"fn (file:line)"` strings — when the error is
caught by no frame.

### 9.1 The closed kind set

Runtime error kinds, **closed for 1.0**:

`"type"` `"arity"` `"name"` `"index"` `"key"` `"div"` `"capability"`
`"io"` `"import"` `"motion"` `"user"` `"limit"` `"handle"`

**Count: 13.** Sprint 34 asserts this number. `"limit"` was added by
amendment **A1** (Sprint 30), and `"handle"` by amendment **A2**
(Sprint 34) — see §16.

`"limit"` covers VM resource exhaustion: call depth, value-stack depth,
the step limit, and Sprint 31's io result caps. It is **catchable**, and
that is the point of it — infinite recursion is user-triggerable, so it
may not become an abort, and it is none of the other twelve.

`"handle"` covers stale, closed, or otherwise unresolvable editor
handles. It is **catchable**, so a script can distinguish an invalid
argument (`"type"`) from an editor object that closed after the script
obtained its handle.

### 9.2 Catching

`try { } catch e { }` catches anything raised in the block, binding the
error map to `e`.

### 9.3 Compile errors versus runtime errors

**Compile errors are diagnostics and are never catchable. Runtime errors
are values and are always catchable.** Diagnostics use Sprint 0's
`file:line:col` format with caret rendering.

**Conformance:** `tests/fletch/09-errors.fl`

---

## §10 Transactions (`edit`)

**The transaction law** (design requirement 4):

A **top-level `edit { }` opens one undo transaction.** An error that
escapes the block **rolls the buffer back to the pre-block state and
then re-raises**.

A **nested `edit` joins the enclosing transaction** — it does not open a
second one. Consequence: a helper function that wraps its work in `edit`
composes correctly when called from another `edit`, which is what makes
the law usable rather than a footgun.

**Headless** (no editor host): `edit` is a plain block.

Rationale: "one macro invocation is one undo step" is the property users
actually rely on. Without the rollback half, a macro that fails halfway
leaves the buffer in a state the user never authored and cannot undo in
one step.

**Conformance:** `tests/fletch/10-transactions.fl`

---

## §11 Modules and import

`import str` imports a **builtin**. The builtins are: `str` `list` `map`
`math` `fmt` `io` `re`.

`import "lib/x.fl" as x` imports a **file**, resolved in this order:

1. relative to the importing file,
2. `$XDG_CONFIG_HOME/sagitta/fl/`.

Modules are **cached by realpath**, so a module imported by two paths
that resolve to one file is one instance.

**Import cycles are an error naming the cycle** — the message lists the
chain, because "circular import" without the chain is unactionable in a
tree of any size.

**Exports:** every top-level binding whose name is not `_`-prefixed.

**Conformance:** `tests/fletch/11-modules.fl`

---

## §12 Pure-literal mode

The data format for config, workspace state (Sprint 25 froze its schema
on this) and syntax definitions — design requirement 6.

A grammar subset **parseable without executing any code**:

```ebnf
pl_file  = pl_value EOF ;
pl_value = "nil" | "true" | "false" | [ "-" ] INT | [ "-" ] FLOAT
         | STRING | pl_list | pl_map ;
pl_list  = "[" [ pl_value { "," pl_value } [ "," ] ] "]" ;
pl_map   = "{" [ pl_entry { "," pl_entry } [ "," ] ] "}" ;
pl_entry = ( IDENT | STRING | INT ) ":" pl_value ;
```

Comments (§1.1) and trailing commas are allowed.

### 12.1 The negative list — what is unreachable

No production above can reach, directly or transitively:

- **no call** — `postfix`'s call form is absent,
- **no identifier** as a *value* — `IDENT` appears only as a bare map
  key; there is no `primary = IDENT`,
- **no import** — `import_stmt` is not reachable,
- **no function literal**, **no motion block**, **no operator**, **no
  assignment**, **no statement of any kind**.

Sprint 29's `fl_parse_literal` fuzzer enforces this list.

### 12.2 The security property

**A pure-literal file grants nothing and runs nothing.** State files are
therefore safe to load from any origin, including a repository checked
out from a stranger. This is stated as a property, not an aspiration:
§12.1 is what makes it checkable.

Sprint 29 ships the dedicated entry point.

**Conformance:** `tests/fletch/12-pure-literal.fl`

---

## §13 Capability model

Capabilities: **`fs.read`**, **`fs.write`**, **`shell`**, **`net`**.

Granted per **script origin**, checked at the host boundary — the `io`
module (Sprint 31), the editor API (Sprint 34) and plugins (Sprint 54).

| Origin | Grant |
|---|---|
| user config (`init.fl`) | all |
| workspace `.sagitta.fl` | all, after the trusted-directory prompt (Sprint 36) |
| plugin | per-capability prompt, persisted (Sprint 54) |
| `sag fl` / `--batch` CLI | all — the user invoked it |
| REPL | all |
| pure-literal data | **none — it never executes** |

Denied access raises kind `"capability"`, which is catchable.

### 13.1 No ambient authority

**The check reads the origin of the calling function's defining module, not the current stack top.**

Consequence, and the whole model in one sentence: a plugin that calls a
helper defined in the user's config **gains nothing** — the helper runs
with the plugin's authority, not the config's, because authority travels
with where code was *defined*, never with who is *running* it.

**Conformance:** `tests/fletch/13-capability.fl`

---

## §14 Worked example

Normative. Exercises every construct; becomes
`tests/fletch/14-example.fl` in Sprint 33.

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

### 14.1 Normative expected results

Sprint 33 asserts these:

| Claim | Value | Why |
|---|---|---|
| `total` | **17** | `nums` is `[1, 2.5, 16]`; `2.5` is skipped by `continue`; `1 + 16 = 17`. `0x10` is 16 |
| `next()` first call | **9** | `clamp(9, 0, 8)` returns 8 (9 > hi), so `n` starts at 8 and the first call increments to 9 |
| `next()` second call | **10** | the closure captured `n` **by reference** (§7), so the increment persisted |
| `while next() < 10 { }` | **runs twice** | the guard calls `next()`, getting 9 then 10; the second stops the loop |
| `dup` | **defined** | `macro` is sugar for a zero-argument function (§8) |
| `shout` headless | returns **`"MOTION"`** | with no editor host the motion block raises kind `"motion"` (§3.1); `catch` binds it and `str.upper("motion")` is `"MOTION"` |

**Conformance:** `tests/fletch/14-example.fl`

---

## §15 Reserved words and style

### 15.1 Reserved words

**22 words**, and Sprint 33 asserts this count:

```
and      as       break    catch    continue edit
else     false    fn       for      if       import
in       let      macro    nil      not      or
return   true     try      while
```

**Motion words are not reserved.** The motion token space (§1.6) is
separate, so `del` is a motion word inside `@[ … ]` and a perfectly
ordinary identifier outside it.

### 15.2 Style

- 4-space indent, never tabs.
- `snake_case` for names.
- `_`-prefix marks a binding private to its module (§11).
- One statement per line; `;` is for the REPL and for compressed motion
  blocks, not for source files.
- Comments are sentences, and carry the *why*.

The default config (Sprint 36) and every file under `runtime/*.fl` must
conform.

**Conformance:** `tests/fletch/15-style.fl`

---

## §16 Amendment log

Post-freeze changes are appended here and nowhere else. **Sections are
never renumbered.**

An amendment carries a **single globally sequential id in sprint order**
— `A1`, `A2`, … — **not** a per-section counter.

Rationale, learned during planning: three later sprints drafted
amendments independently and two of them both picked "A1", because a
per-section scheme (`§16-A1`) makes a collision invisible until someone
reads both files. **This table is the registry: an amendment is not
filed until it appears here with the next free id.**

An id may be RESERVED here during planning; it is **filed** only when
the Filed and Reviewer columns are populated by the sprint that lands
it. A reserved row is a promise, not a change to the spec.

| Id | Sprint | § | Change | Reason | Filed | Reviewer |
|---|---|---|---|---|---|---|
| A1 | 30 | 9 / 16 | error kinds 11 → 12, adding `"limit"` | stack/step-limit exhaustion is user-triggerable and fits none of the 11 closed kinds | 2026-08-08 | Sprint 30 implementation review |
| A2 | 34 | 9 / 16 | error kinds 12 → 13, adding `"handle"` | a stale or closed editor handle is neither a type nor an index error; `catch` must distinguish "wrong argument" from "the buffer closed under you" | 2026-08-10 | Sprint 34 implementation review |
| A3 | 55 | 11 | import resolution gains a fourth row, `$SAG_RUNTIME_DIR/`, searched **last** | shipped presets must be importable, and searching last lets a user's copy shadow the shipped one | reserved | — |
| A4 | 30 | 7 | §7's closure sentence reads "9 then 10", was "10 then 11" | §7 cited §14.1 as normative and then disagreed with it; `clamp(9, 0, 8)` is 8, so the first call yields 9 | 2026-08-08 | Sprint 30 implementation review |

**A4 is out of sprint order, deliberately.** A2 and A3 were reserved
during planning for Sprints 34 and 55, and the rule above forbids
renumbering. The ordering rule exists to make id COLLISIONS visible, and
A4 collides with nothing; taking the next free id is the only option
that leaves the reserved rows alone. A later sprint filing after 34 and
55 takes A5 — sprint order resumes there.

**Pitfall — §9's kind count is 13, not 12.** A1 and A2 are filed.
Sprints 31, 32 and 33 were written between them and correctly say 12
for their point in time; anything written after Sprint 34 says 13.
Sprint 34's conformance assertion and Sprint 58's audit front `FLETCH`
re-check the arithmetic.

### Deferred to later sprints — named here, not invented here

| Surface | Sprint |
|---|---|
| Editor API semantics beyond the names `buf`, `win`, `bind`, `set`, `on` | 34 |
| Recorder emission rules (run folding `>>>>` → `4>`, mode-context naming) | 35 |
| `--batch` stdio contract | 37 |

These are **host-provided**. The spec names them so no later sprint has
to honour a semantics this document invented without them.

**Conformance:** `tests/fletch/16-amendments.fl`
