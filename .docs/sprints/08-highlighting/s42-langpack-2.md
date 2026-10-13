# Sprint 42: Langpack 2 — python, rust, go, js/ts, fortran, json, yaml, toml · MILESTONE: daily driver

## Prerequisites

- Sprint 39 — the engine and its bounds: `SynState` (`aux`, `flags`,
  `lost`), `YEW_SYN_DEPTH_MAX = 16`, `YEW_SYN_LINE_BYTE_CAP = 64 KiB`,
  the budgeted fixpoint settle, the 54-entry attr table, the span cache.
- Sprint 40 — the schema and everything this sprint leans on hard:
  `consume`, `captures`, `set_aux`/`strip`, the four aux matchers
  (`line_eq`, `literal`, `fence_close`, `indent_lt`), the `value` flag bit,
  `at_eol`, `include:`, `unit:` marks, the validator, `yew syn
  check --strict --coverage`, `yew syn dump --spans`, and the XDG cache.
- Sprint 41 — the five definitions that proved the format, both themes
  (all 54 attrs), the degradation ladder, the underline-colour field, the
  perf harness `tests/perf/perf_syn.c` and its committed baselines, and
  the frozen-fixture discipline.
- Sprint 20 — the regex syntax table and its permanent non-goals. **There
  is no lookahead**: every "X followed by Y" rule below is `captures` +
  `consume`.
- Sprints 8, 10, 11, 25 — the no-data-loss contract, the undo tree, the
  torture suite, and workspace resume: the dogfood gate in §10 is a
  *cross-subsystem* gate and those are what it re-verifies in the field.
- Sprint 56 calibrates reference hardware; until then the latency numbers
  in §10 are measured on the machine named in `.docs/dogfood-log.md` and
  recorded with it.
- Binding: `00-decisions.md` (*"Ships ≥ 12 languages"* — this sprint takes
  the count to 14; all six quality bars; invariants 1, 3, 4, 5, 9).

## Goals

Finish the language pack and establish and track the **daily driver**
milestone. Eight
definitions, each carrying at least one construct that a naive line-state
machine gets wrong: python's f-strings and triple-quoted strings, rust's
raw strings and the lifetime-vs-char-literal ambiguity, go (deliberately
boring — the proof that the format is unremarkable for unremarkable
languages), js/ts's regex-literal-vs-division ambiguity and template
literals with nested interpolation, **fortran in both fixed and free form**
with automatic form detection, json (and the jsonc question), yaml with an
honestly-bounded supported subset, and toml. Every language gets golden
attr-span dumps with mechanical rule coverage, and the whole set gets a
cumulative perf re-gate against Sprint 41's budgets. Then the milestone:
an explicit, checkable dogfood gate with named revocation categories, so
"maintainers switched" is a fact with evidence rather than a vibe.

## Deliverables

### 1. `runtime/syntax/python.fl`

Base coverage: keywords (with `keyword.control` for flow), soft keywords,
builtins → `function.builtin`, `self`/`cls` → `variable.builtin`,
decorators `@name.path` → `attribute`, numbers (`0b`/`0o`/`0x`, `_`
separators, `j` complex suffix), operators, `:=`, type annotations, `def`
and `class` names via `captures`+`consume`, `# type:` comments →
`comment.doc`.

**Triple-quoted strings.** Four openers (`'''`, `"""`, and their `r`/`b`/
`f`/`rb`/`fr` prefixed forms). `set_aux` captures the opening delimiter;
the context is `at_eol: "stay"`, `unit: "atom"`, and terminates on the
`literal` aux matcher. One context handles all four because the delimiter
lives in `aux` — which is the whole reason `aux` exists.

> **Pitfall — docstrings are not comments here.** A triple-quoted string in
> statement position is a docstring and "should" be `comment.doc`, but
> knowing it is in statement position requires knowing the previous line
> ended a suite header, which a line-state machine cannot see. We style
> docstrings as `string` and say so in the file header. Under-styling
> beats guessing wrong on `x = """…"""`.

**f-strings.** The state carries the quote form in `aux` and the
interpolation nests through the context stack:

| Element | Handling |
|---|---|
| `f"` / `f'` / `f"""` / `f'''` | `set_aux` the delimiter, push `fstring` (or `fstring_triple`) |
| `{{` / `}}` | literal braces — matched **before** the interpolation rule, so first-match-wins does the work |
| `{` | push `fstring_expr`; body styled as ordinary python; `string.interp` on the brace |
| `}` | pop back to the string |
| nested `{}` inside the expression (dict/set literals, comprehensions) | `fstring_expr` has its own `{` push, so nesting balances via the stack and the depth cap |
| `!r` `!s` `!a` and `:spec` inside `fstring_expr` | `string.special`; the conversion/spec suffix pops at `}` like everything else |
| a quote of the *same* kind inside `{…}` (3.12+) | **supported**, because the terminator is the `literal` matcher on `aux`, and the expression context has its own string rules; the closing quote of an inner string cannot end the outer f-string because `fstring`'s terminator rule is not active while `fstring_expr` is on top |
| a backslash inside `{…}` (illegal < 3.12) | styled `string.escape`, no error — version-dependent errors are not our job |

**Tabs in indentation** → `warning` on a line mixing tabs and spaces in its
leading whitespace. One rule, and it catches the single most common
real-world python bug a highlighter can catch.

### 2. `runtime/syntax/rust.fl`

Coverage: keywords, `keyword.storage` (`pub let mut static const unsafe
async`), primitive types → `type.builtin`, `CamelCase` → `type`, `SCREAMING`
→ `constant`, macros `name!` → `function.macro`, attributes `#[…]`/`#![…]`
→ `attribute` (with a nested-bracket context), doc comments `///` and
`//!` → `comment.doc`, numbers with `_` and type suffixes, `r#ident` raw
identifiers, `?`/`=>`/`->` operators.

**Nested block comments — Rust's do nest.** `/* /* */ */` is one comment.
`block_comment` pushes *itself* on `/*` and pops on `*/`. This is the
runtime-recursive case Sprint 39's depth cap and `lost` counter were built
for: at 16 levels the pushes are refused and balanced by `lost`, so a
pathological file styles as one flat comment and the state stays exactly
balanced. Golden fixture: 3-deep (correct), and 20-deep (degraded, but the
exit state after all closers equals the entry state).

**Raw strings.** `r"…"`, `r#"…"#`, `r##"…"##` — the terminator is `"`
followed by exactly the opening hash count.

```fletch
{ match: "b?r(#*)\"", attr: "string.special", set_aux: 1, push: "rawstr" },
rawstr: { default: "string.special", at_eol: "stay", unit: "atom",
          rules: [ { aux: "literal", aux_pre: "\"", attr: "string.special",
                     pop: 1 } ] },
```

`aux` holds the hash run; `aux_pre: "\""` makes the matcher look for
`"` + hashes. There are no escapes inside a raw string — the context has no
escape rule, which is the correct and the simplest encoding.

**Lifetime vs char literal — the pinned heuristic.** At a `'`, rules are
tried in this exact order:

| # | Pattern | Result |
|---|---|---|
| 1 | `b?'([^'\\\n]\|\\(x[0-9a-fA-F]{2}\|u\{[0-9a-fA-F]{1,6}\}\|[nrt0'"\\]))'` | a complete char literal → `character` |
| 2 | `'(?:static\|_)\b` | `variable.param` (the two named lifetimes that also look like keywords) |
| 3 | `'[A-Za-z_][A-Za-z0-9_]*` | a lifetime → `variable.param` |
| 4 | `'` alone | `error` |

**This ordering is exact for all valid Rust**, and that claim is the
heuristic: rule 1 requires a closing quote within the literal's own
grammar, and a lifetime can never be immediately followed by a quote in
valid source (`&'a'b'` does not parse). The ordering matters — swapping 1
and 3 turns every `'a'` into a lifetime. On *invalid* source the failure is
bounded: a char-literal context cannot span lines (`at_eol: "pop"`), so a
stray quote mis-styles at most one line and cannot leak.

### 3. `runtime/syntax/go.fl`

Keywords, builtins, `iota` → `constant.builtin`, interpreted strings with
Go's escape set, **raw strings** with backticks (multi-line,
`at_eol: "stay"`, no escapes at all — a `\` in a backtick string is a
backslash), rune literals (`'x'`, `'\n'`, `'\u00e9'`), struct tags inside
backtick strings → `string.special`, `//go:build` and `// +build` →
`preproc`, numbers with `_` and imaginary suffix, `:=`, labels, `func`
names via `captures`+`consume`.

Go earns its place by being boring. It is the definition to point at when
someone asks whether the format is complicated: 6 contexts, ~30 rules, no
aux matchers, no `value` flag, and it is complete. **`.docs/syntax-def.md`
cites it as the "start from this one" template.**

### 4. `runtime/syntax/javascript.fl` and `typescript.fl`

`typescript.fl` is `javascript.fl` plus a TS-only context spliced in with
`include:` — types, `interface`/`type`/`enum`/`namespace`/`declare`,
generics, `satisfies`/`as const`, parameter properties, decorators. Two
files, one shared rule body, per s40 §4's inclusion rule. `.jsx`/`.tsx`
map to these definitions with a documented limitation (JSX tags are not
styled as markup; they style as expressions) rather than shipping a
half-working JSX definition.

**Regex literal vs division — the pinned previous-token heuristic.** A `/`
starts a regex iff the previous significant token *cannot end a value*.
Sprint 40's one-bit `value` state flag carries this:

| Rules that set `value: true` (a `/` after them is **division**) | Rules that set `value: false` (a `/` after them starts a **regex**) |
|---|---|
| identifier, number, string, template literal, regex literal | every operator, `(`, `[`, `,`, `;`, `:`, `=>` |
| `)`, `]`, `}` | keywords `return typeof instanceof in of new delete void case do else yield await` |
| `this`, `super`, `true`, `false`, `null`, `undefined` | start of line and start of file (the flag is `false` in the root state) |
| `++`/`--` (postfix) | `{` |

Two known failures, both documented in the file header, both chosen
deliberately:

- **`}`** ends a block (regex-legal: `if (x) {} /re/.test(s)`) *and* an
  object literal or a template interpolation (division-legal: `({}) / 2`).
  We treat `}` as ending a value. The block case is vanishingly rare in
  real code and the failure is one mis-styled line.
- **`)`** ends a call (division) *and* an `if`/`while` head (regex-legal:
  `if (x) /re/.test(s)`). Same choice, same reasoning.

The failure mode is bounded by construction: the `regex` context has
`at_eol: "pop"`, so a misidentified regex can never swallow more than the
rest of one line. That bound is why a one-bit heuristic is acceptable at
all — the alternative is a JS parser.

**Template literals with nested interpolation.** `` ` `` pushes `template`
(`at_eol: "stay"`, `unit: "atom"`); `${` pushes `template_expr` and styles
the delimiter `string.interp`; `template_expr` contains the full main rule
set *including* the `` ` `` rule, so `` `a${`b${c}`}` `` nests through the
context stack. `}` pops. Brace balance inside the expression is maintained
by a `braces` context pushed on `{`, so an object literal inside an
interpolation does not pop the template early — the classic bug. Depth is
bounded by s39's cap; 16 levels is ~8 nested templates.

### 5. `runtime/syntax/fortran.fl` — both forms

Two definitions sharing one included rule body:
`runtime/syntax/fortran.fl` (free form) and
`runtime/syntax/fortran_fixed.fl` (fixed form), with
`include:fortran_common` carrying keywords, intrinsics, types, operators,
numbers, and strings.

**Form detection**, in this order (`yew_syn_lang_for` extended with a
per-language hook registered by these definitions):

1. **Option override**: `set { fortran_form: "free" | "fixed" | "auto" }`
   (Sprint 36), default `"auto"`.
2. **Extension** (the industry convention, and it is nearly always right):
   `.f90 .f95 .f03 .f08 .f18 .F90 .F95 .F03 .F08 .F18` → free;
   `.f .for .ftn .fpp .F .FOR .FTN .FPP` → fixed.
3. **Content heuristic**, used only when the extension is absent or in the
   ambiguous set (`.inc`, `.i`, no extension, or `.f`/`.for` when the
   caller passes `--fortran-sniff`): scan the first 100 non-blank lines and
   score.

   | Signal | Score |
   |---|---|
   | `C`, `c`, `*`, or `!` in column 1 with a statement following | fixed +2 |
   | columns 1–5 all digits or blank **and** column 6 non-blank, non-`0` | fixed +3 |
   | a line whose last non-blank character is `&` | free +3 |
   | `!` comment starting after column 1 | free +2 |
   | a statement keyword starting in column 1 | free +1 |
   | non-blank text past column 72 | fixed −2 (a fixed-form file would have it ignored) |

   Majority wins; a tie goes to **free** (the modern default, and the form
   a new file is more likely to be).
4. Statusline shows `fortran` or `fortran(fixed)` so a wrong sniff is
   visible and `ed.syn.set` fixes it in one command.

**Fixed form — columns are the grammar.** The `main` context's rules are
anchored at position 0 and consume by column:

| Columns | Content | Attr |
|---|---|---|
| 1 | `C c * !` → the whole line is a comment | `comment` |
| 1 | `#` → cpp directive (`.F` files) | `preproc` |
| 1–5 | statement label (digits) | `label` |
| 6 | non-blank, non-`0` → continuation marker | `operator` |
| 7–72 | the statement | normal rules |
| **73–80** | **ignored by the compiler** (card sequence numbers) | `comment`, dim |
| leading TAB | vendor extension meaning "resume at column 7"; a digit right after the tab is a continuation marker | handled by one rule, documented |

Columns 73+ styled as `comment` is not decoration: it is the single most
useful thing a fortran-fixed highlighter can do, because a statement that
silently runs past column 72 is a bug that is otherwise invisible.

**Free form**: `!` comments anywhere, `&` continuation at end of line and
optionally at the start of the continued line, statement labels, and the
132-column limit noted in the header but **not** styled as an error (too
many real files exceed it harmlessly under compiler extensions).

**Both forms:**

- **Everything is case-insensitive.** `icase: true` on the shared contexts.
  A definition with a lowercase-only keyword list is useless on any file
  written before 1995.
- Strings are `'…'` and `"…"`, and the escape is a **doubled quote**
  (`'it''s'`). **Pitfall, stated in the file: there is no backslash escape
  in standard Fortran.** A definition that treats `\` as an escape breaks
  every Windows path and every `\n` in a `write` format string — and gets
  filed as a bug within a day of a Fortran user opening it.
- Logical literals `.TRUE.`/`.FALSE.` → `boolean`; `.AND. .OR. .NOT. .EQ.
  .NE. .LT. .LE. .GT. .GE. .EQV. .NEQV.` → `keyword.op`.
- Numbers with kind suffixes: `1.0d0`, `1.0e-3`, `1.0_dp`, `2_int64`,
  `z'FF'` / `b'1010'` / `o'777'` BOZ literals.
- Types (`integer real complex logical character double precision type`),
  attributes (`intent(in) parameter allocatable pointer target dimension
  optional pure elemental recursive`), and the ~200 standard intrinsics as
  `function.builtin`.
- Preprocessor `#if/#define/#include` in `.F*` files → `preproc`.
- **Out**: Hollerith constants (`4Habcd`), `INCLUDE` file following,
  statement functions, and `ENTRY` — all named in the header.

This is the definition the maintainer's other projects live on, so it gets
the largest fixture set: four real-shaped fixtures (a fixed-form
subroutine with labels, continuations and past-column-72 text; a modern
free-form module; a `.F90` with cpp; a mixed-case legacy file).

### 6. `runtime/syntax/json.fl` (+ jsonc)

Strict JSON: strings with the exact RFC 8259 escape set (`\" \\ \/ \b \f
\n \r \t \uXXXX`), any other escape → `error`; numbers per the RFC grammar
(no leading `+`, no leading zeros, no hex) with violations → `error`;
`true`/`false`/`null`; structural punctuation. Keys are distinguished from
value strings by `captures` + `consume`:

```fletch
{ match: "(\"(?:\\\\.|[^\"\\\\])*\")\\s*:",
  captures: { 1: "variable.member" }, consume: 1 },
```

— the s40 `consume` idiom again, because there is no lookahead.

**Comments and trailing commas are `error` in `.json`.** They are legal in
JSON-with-comments, so `jsonc.fl` is a two-line variant (`include:` the
same body, plus comment rules, minus the error rules) mapped by
`filenames` to the files where the ecosystem actually allows them:
`tsconfig.json`, `jsconfig.json`, `.eslintrc.json`, `devcontainer.json`,
`*.jsonc`. Guessing "comments are probably fine" in `.json` hides a real
parse error; refusing to support jsonc at all annoys everyone with a
`tsconfig.json`. The allowlist is data, and users extend it in `init.fl`.

### 7. `runtime/syntax/yaml.fl` — the honest subset

YAML is the language where a line-state machine is most obviously an
approximation, so the definition's header states its boundaries first.

**In scope:**

| Construct | Handling |
|---|---|
| comments | `#` when preceded by start-of-line or whitespace |
| document markers | `---`, `...` → `preproc` |
| directives | `%YAML`, `%TAG` → `preproc` |
| block mapping keys | `^(\s*)([^\s#][^:#]*?)(:)(\s|$)` with `captures` + `consume` → `variable.member` |
| block sequence entries | `^\s*(-)(\s|$)` → `punct.delim` |
| quoted scalars | `'…'` (with `''` escape) and `"…"` (with `\` escapes), `at_eol: "pop"` — a quoted scalar spanning lines is styled per line |
| anchors / aliases / tags | `&name` → `label`, `*name` → `label`, `!!str`/`!custom` → `type` |
| flow collections | `[ ]` `{ }` to a nesting depth of 4, keys and values styled inside |
| **block scalars** | `\|`, `>`, with indentation indicator and chomping (`\|-`, `>+`, `\|2-`) — see below |
| booleans / null / numbers / timestamps | the YAML 1.2 core schema spellings only (`true false null ~`, ints, floats, `.inf`, `.nan`, RFC 3339 timestamps) |
| merge key `<<` | `keyword.op` |

**Block scalars** are where `indent_lt` earns its place: the opening
`|`/`>` rule computes the body indent (explicit indicator, else the
opening line's indent + 1) and stores it as an *integer* in `aux`; the
`block_scalar` context is `at_eol: "stay"`, `unit: "atom"`, `default:
"string"`, and its **first** rule is `{ aux: "indent_lt", pop: 1 }`, which
fires at position 0, consumes nothing, and pops so the line is rescanned by
the parent. Sprint 39's per-line zero-width pop counter is what keeps that
safe.

**Explicitly out — and this list ships in the file header and in
`.docs/syntax-def.md`:**

1. **Plain (unquoted) multi-line scalars.** A plain scalar folded across
   lines is styled line by line, so its continuation lines look like new
   keys if they contain `: `.
2. **Complex keys** (`? key` / `: value` on separate lines) — styled as
   ordinary punctuation.
3. **Tag resolution and typing.** `!!int "3"` is a string to us.
4. **Flow nesting past depth 4**, and flow collections spanning lines —
   styled generically after the cap.
5. **`key: value` disambiguation for plain scalars containing `: `.** We
   style by the first `: ` at the mapping level and accept mis-styling of
   `url: http://x` — actually correct here, because `://` has no space —
   but not of `msg: a: b`.
6. YAML 1.1 quirks (`yes`/`no`/`on`/`off` as booleans, sexagesimal
   integers): styled as plain scalars, per 1.2.

Every one of these is a *bounded* inaccuracy: none can leak state past the
end of the enclosing block, because every multi-line context in the
definition terminates on `indent_lt` or `at_eol`.

### 8. `runtime/syntax/toml.fl`

Tables `[a.b."c d"]` and array-of-tables `[[a]]` → `namespace` with the
brackets and dots as punctuation; bare / dotted / quoted keys →
`variable.member`; basic strings (`"…"`, `"""…"""`) with escapes and the
line-ending-backslash continuation; literal strings (`'…'`, `'''…'''`) with
**no** escapes; integers with `_`, `0x`/`0o`/`0b`; floats including `inf`
and `nan`; booleans; and offset/local date-times, which are worth writing
out because everyone gets them wrong:

```
(\d{4}-\d{2}-\d{2}([Tt ]\d{2}:\d{2}:\d{2}(\.\d+)?([Zz]|[+-]\d{2}:\d{2})?)?
 |\d{2}:\d{2}:\d{2}(\.\d+)?)
```

Multi-line strings use `set_aux` + the `literal` matcher exactly as
python's triple quotes do — the fourth consumer of the same mechanism,
which is the evidence that the four aux matchers were the right closed set.

### 9. Golden fixtures and the cumulative perf re-gate

**Golden format** (`yew syn dump --spans`, deterministic, committed under
`tests/syn/<lang>/`):

```
# lang=rust  def=runtime/syntax/rust.fl  schema=1
@1   root
   1   0   3  keyword          "let"
   1   3   1  text             " "
   1   4   1  variable         "x"
@2   root/block_comment
   2   0  12  comment          "/* outer /*"
```

`@N` lines print the **entry state as a `/`-joined path of context names**,
never numeric ids (s40's golden-stability rule); span lines are
`line col len attr "text"` with the text elided past 32 bytes. Regenerate
with `make syn-goldens`; CI runs `yew syn dump --spans --check` and fails
on any diff.

Per-language fixture requirements:

| Language | Fixtures beyond the kitchen sink |
|---|---|
| python | f-string with nested quotes and `{{`/`}}`; nested dict inside `{…}`; all four triple-quote openers; tab/space mixed indent |
| rust | 3-deep and 20-deep nested comments; `r###"…"###`; the four lifetime/char rows; `b'x'` |
| go | backtick string containing `\n` and a struct tag; `//go:build` |
| js/ts | the eight `value`-flag rows; both documented failure cases (asserted as *known-wrong* goldens with a comment, so a future fix shows up as a diff); `` `a${`b${c}`}` ``; a regex containing `/` in a class |
| fortran | fixed-form with labels, column-6 continuation, past-column-72 text, leading TAB; free-form `&` continuation; `.F90` with cpp; mixed-case; `'it''s'`; `write(*,*) 'C:\path\n'` (the no-backslash-escape case) |
| json | every escape, every number violation, a comment (→ `error`), the same file as `.jsonc` (→ `comment`) |
| yaml | all six out-of-scope constructs, each asserting the *documented* (possibly imperfect) output so a change is visible; `\|2-` block scalar; nested flow at depth 5 |
| toml | all six string forms; every datetime shape; dotted and quoted keys |

**Coverage gate**: `yew syn check --coverage` over all 17 definition files (14 languages) must
report 100 % of contexts entered and 100 % of rules fired by the committed
fixtures. A new rule without a fixture fails CI.

**Cumulative perf re-gate.** Sprint 41's fixtures stay; these are added,
frozen, under `tests/perf/fixtures/syn/`:

| Fixture | Content | Lines | Size |
|---|---|---|---|
| `py_kitchen.py` | numpy-shaped scientific code, 60 f-strings | 6 000 | 190 KiB |
| `rs_kitchen.rs` | generics-heavy, 40 raw strings, nested comments | 6 000 | 210 KiB |
| `go_kitchen.go` | idiomatic Go, struct tags | 5 000 | 150 KiB |
| `ts_kitchen.ts` | 200 template literals, 80 regex literals | 5 000 | 165 KiB |
| `f90_kitchen.f90` | free form, 4 000 lines | 4 000 | 140 KiB |
| `f77_kitchen.f` | fixed form, 4 000 lines | 4 000 | 130 KiB |
| `yaml_kitchen.yml` | 30 block scalars, deep nesting | 4 000 | 96 KiB |
| `json_kitchen.json` | one 2 MiB minified line + a 5 000-line pretty file | 5 001 | 2.2 MiB |

All of Sprint 41's budgets apply unchanged to every fixture — the same
table, re-run:

| Scenario | Budget | Note |
|---|---|---|
| Full-viewport restyle, 200×100, cold | ≤ 1.5 ms | **every** fixture, including `ts_kitchen.ts` |
| Per-line median / p99 | ≤ 3.0 µs / ≤ 12 µs | worst language reported by name in the baseline file |
| Single-char edit → settled | ≤ 60 µs | |
| Whole-file settle, largest fixture | ≤ 45 ms accumulated, no frame > 1 ms | |
| `json_kitchen.json` minified line, open → first paint | ≤ 20 ms | the `YEW_SYN_LINE_BYTE_CAP` path |
| Cold start with all 17 definition files (14 languages) installed, warm cache | ≤ 20 ms | proves Sprint 40's laziness (exactly 0 or 1 definitions compiled) |
| State-array memory, all fixtures open | ≤ 4 B/line + 1 % | |
| Scroll throughput, every fixture | ≥ 120 fps equivalent | |

The baseline file records **per-language** rows, so a regression names the
definition that caused it.

### 10. MILESTONE: daily driver — the dogfood gate

The milestone is earned by evidence, not by declaration. `.docs/dogfood-log.md`
is created this sprint and carries: the reference machine (CPU, RAM,
terminal emulator, `TERM`, kernel), a dated row per session (hours, files
touched, incidents, latency histogram summary), and the checklist below
with each box's evidence link. `.docs/milestones.md` records the milestone
state (`EARNED` / `REVOKED` + reason + issue id).

**Gate criteria — all must be checked, each with committed evidence:**

- [ ] **Duration.** During one continuous qualifying field-evidence window
      beginning no earlier than creation of `.docs/dogfood-log.md`, yew is
      the maintainer's primary editor for ≥ 10 working days and ≥ 40 logged
      hours, with a dated row per day. The window may span Sprint 42.5 or a
      later sprint. Falling back to another editor for a task is allowed and
      **must be logged with the reason** — an unlogged fallback invalidates
      the day.
- [ ] **Self-hosting.** ≥ 1 full post-implementation sprint's worth of
      commits authored inside yew (≥ 60 % of its eligible changed lines
      under `.docs/dogfood-log.md`'s explicit path/artifact rules), evidenced
      by per-commit dogfood-log rows. Sprint 42.5 is the designated qualifying
      sprint because Sprint 42's implementation landed before the
      field-evidence window opened.
- [ ] **Zero data-loss incidents.** No lost buffer, no corrupted save, no
      failed journal recovery, no save that changed bytes the user did not
      edit. **One occurrence revokes the milestone**, unconditionally, until
      it is fixed *and* a regression test lands in the s08/s11 suites.
- [ ] **Zero crashes.** No `SIGSEGV`/`SIGBUS`/`SIGABRT`, no `yew_bug()`
      call, exit code 4 never observed. `$XDG_STATE_HOME/yew/log`
      grepped for `BUG` across every session: empty.
- [ ] **Terminal restore, verified in the field.** ≥ 3 deliberately induced
      abnormal exits (`kill -9`, `kill -SEGV`, terminal closed under a
      running instance) each leaving the terminal usable (invariant 6),
      logged with the method.
- [ ] **Latency green on the reference machine, live.** With
      `YEW_LATENCY_LOG=1`, the keypress→paint histogram over
      ≥ 200 000 real keystrokes shows p99 ≤ 5 ms and p99.9 ≤ 12 ms. Not
      the perf harness — the live session. The histogram is committed.
- [ ] **The other budgets, same machine, same day.** Cold start → first
      paint ≤ 20 ms; open a 100 MB file → interactive ≤ 150 ms; scroll
      throughput ≥ 120 fps equivalent; RSS ≤ 1.6× file size on a clean
      open; stripped binary ≤ 1.5 MiB full-modules and ≤ 900 KiB minimal.
- [ ] **Highlighting covers our own tree.** Every file type in the repo
      opens with correct highlighting and 100 % rule coverage against its
      fixtures: `.c`/`.h` (C), `.fl` (Fletch), `Makefile`/`*.mk` (make),
      `*.md` (markdown), `*.sh` (sh), `*.yml` (yaml, CI configs),
      `*.json`/`*.toml` (packaging), plus the maintainer's Fortran
      projects in both forms. Checked by opening each and running
      `ed.syn.status` — no `degraded`, no `YEW_LANG_NONE` on a known type.
- [ ] **Workspace resume in anger.** ≥ 20 real quit/reopen cycles restoring
      tabs, groups, panes, cursors and undo exactly (Sprint 25's contract),
      zero discrepancies logged.
- [ ] **Modal completeness in practice.** Every editing task in the log was
      keyboard-reachable (invariant 9); any task that required the mouse is
      logged as a defect against the sprint that owns it.

**Blocking-bug categories — any one revokes the milestone.** These are the
named list; a bug in any category flips `.docs/milestones.md` to `REVOKED`
with the category and issue id, and the milestone is re-earned only after a
fix, a regression test, and **three consecutive clean dogfood days**.

| # | Category |
|---|---|
| B1 | Data loss: lost buffer, corrupted save, failed journal recovery, or a save that changes bytes the user did not edit (EOL/BOM/final-newline mangling counts) |
| B2 | Terminal left in raw mode, in the alternate screen, or with input modes pushed on **any** exit path |
| B3 | Any input sequence that stalls the event loop > 200 ms, or a p99 keypress→paint regression above 5 ms sustained across a session |
| B4 | Undo/redo that does not restore exact bytes, or a transaction that partially applies |
| B5 | A multi-cursor edit producing a buffer the single-cursor path would not |
| B6 | Any crash: `SIGSEGV`/`SIGBUS`/`SIGABRT`, `yew_bug()`, or exit code 4 |
| B7 | Highlighting state desync that survives a redraw — wrong colours that do not self-heal after `ed.redraw`. (A *settling* viewport is not a defect; a permanently wrong one is) |
| B8 | Workspace resume losing tabs, groups, panes, cursors, or undo history |
| B9 | Rendering nondeterminism: the same state producing different cell grids (invariant 5) — including stale half-glyphs and orphan continuation cells |
| B10 | A silent stub: any code path that no-ops instead of hard-erroring with its sprint number (invariant 3) |

Milestone declaration is a commit that (a) checks every box with an
evidence link, (b) records the reference machine, and (c) updates
`.docs/milestones.md`. Sprint 42's implementation deliverables may be complete
while this asynchronous field milestone remains `PENDING`; only the milestone
state changes to `EARNED` when that declaration commit exists.

### 11. Defer

- Embedded languages (markdown fences, make recipes, HTML `<script>`,
  shell `$(…)`, JSX) → **`s41_5-embedded-languages.md`** per Sprint 41 §6.
  At Sprint 42 closeout, `SynState.def` was still asserted 0 across all 17
  definition files / 14 families; Sprint 41.5 later superseded that state
  shape and assertion with qualified embedded frames.
- This sprint originally deferred all languages beyond its 14 families.
  Sprint 41.5 subsequently pulled HTML/CSS into the built-in pack, and the
  binding Sprint 42.5 contract pulls zsh, fish, lua, ruby, sql, nix, zig,
  haskell, and further named modes forward. PHP and bidirectional template
  interleaving remain post-1.0. Definitions remain pure data: a user can drop
  a `.fl` into `$XDG_CONFIG_HOME/yew/syntax/` with no rebuild or capability
  prompt.
- Semantic highlighting from LSP layered over these attrs → **post-1.0**;
  the later binding Sprint 46/47 contracts explicitly exclude semantic tokens
  from 1.0.
- Fortran `INCLUDE` following, Hollerith constants, statement functions →
  post-1.0, named in the definition header.
- yaml's six out-of-scope constructs → post-1.0 or never; each is
  documented with its observed behavior, not left to be discovered.
- Reference-hardware calibration of every number in §9/§10 →
  **Sprint 56**; this sprint records the machine it measured on.
- Per-attr user overrides in config → **Sprint 36**'s option surface.

## Testing Strategy

- **Golden spans**: the §9 fixture matrix, regenerated by `make
  syn-goldens`, checked by `yew syn dump --spans --check` in CI. The two
  js/ts known-wrong goldens carry a comment naming the heuristic, so a
  future improvement shows as an intentional diff.
- **Coverage gate**: `yew syn check --coverage` over all 17 definition files (14 languages) —
  100 % contexts, 100 % rules, enforced in CI.
- **Validation**: `yew syn check --strict runtime/syntax/*.fl` exits 0 with
  zero warnings for all 14.
- **Unit** (`tests/unit/test_syn_fortran.c`): the form-detection scoring
  table driven with ≥ 20 hand-scored inputs including the ambiguous set,
  the tie-goes-to-free rule, and the option override; the column map for
  fixed form (a statement at column 73 is `comment`, at 72 is code).
- **Unit** (`tests/unit/test_syn_jsvalue.c`): every row of the §4
  `value`-flag table, plus both documented failures asserted as their
  documented output, plus the bound (a misidentified regex never survives
  the end of its line).
- **Unit** (`tests/unit/test_syn_depth.c`): rust's 20-deep nested comment —
  exit state after all closers equals the entry state exactly, `lost`
  saturation sets `degraded`, and the buffer recovers when the file is
  edited back to 3-deep.
- **Fuzz**: every new definition joins Sprint 39's line fuzzer and Sprint
  40's definition fuzzer corpora; additionally, each language's kitchen-sink
  fixture is fed to the s39 *edit* fuzzer (random splices + settles vs a
  from-scratch oracle), 100 k ops × 4 seeds × 8 languages, zero
  divergences.
- **Differential across the whole pack** (`tests/unit/test_syn_all.c`): for
  every fixture in `tests/syn/`, incremental settle after 200 random edits
  must equal from-scratch highlighting exactly — the s39 oracle, now with
  real definitions rather than the toy.
- **Pty goldens**: one 80×24 render per language in both themes at
  truecolor, plus `f77_kitchen.f` proving the column-73 styling is visible.
- **Perf**: §9's re-gate, `make perf`, per-language baseline rows
  committed; the `json_kitchen.json` minified-line row is a hard gate.
- **Dogfood**: §10 — the log is the test artifact, and CI checks that
  `.docs/dogfood-log.md` has ≥ 10 dated rows and `.docs/milestones.md`
  names the reference machine before the milestone commit is allowed.
- Lanes: gcc/clang zero-warning, ASan/UBSan, valgrind, determinism (all
  goldens byte-stable across two runs and both compilers, cold and warm
  syntax cache).

## Definition of Done

1. `make && make test` green on gcc and clang, zero warnings;
   ASan/UBSan/valgrind/determinism lanes green.
2. Eleven new definition files — `python rust go javascript typescript
   fortran fortran_fixed json jsonc yaml toml` (8 languages; typescript,
   fortran_fixed and jsonc are shared-body variants) — compile, and
   `yew syn check --strict runtime/syntax/*.fl` exits 0 with zero warnings
   across all 17 files / 14 languages.
3. **100 % context and rule coverage** across all 17 definition files (14 languages) against
   the committed fixtures; CI fails on any unfired rule.
4. Every trap in §1–§8 has a named golden: python's nested-quote f-string
   and `{{`; rust's 20-deep comment, `r###`, and all four lifetime/char
   rows; go's backtick-with-`\n`; js's eight `value` rows, two documented
   failures, and `` `a${`b${c}`}` ``; fortran's fixed-form column map,
   `&` continuation, `'it''s'`, and `'C:\path\n'`; json's escape and
   number violations plus the `.json`-vs-`.jsonc` pair; yaml's six
   out-of-scope constructs and its `|2-` block scalar; toml's six string
   forms and every datetime shape.
5. Fortran form detection: ≥ 20 hand-scored inputs correct, including the
   tie rule and the option override; a fixed-form statement at column 73
   styles as `comment` and at column 72 as code (pty golden).
6. The rust depth case: 20 nested comments leave the exit state exactly
   equal to the entry state after all closers, `degraded` set and cleared
   correctly (removing s39's `lost` handling fails this test).
7. Differential: 100 k random edits × 4 seeds × 8 new languages, zero
   divergences against from-scratch highlighting.
8. `SynState.def == 0` for every state produced by every definition across
   the entire fixture corpus (asserted in the differential harness).
9. All §9 budgets green on every fixture with per-language baseline rows
   committed; the minified 2 MiB JSON line opens to first paint ≤ 20 ms;
   cold start with all 17 definition files installed compiles ≤ 1 definition.
10. Fuzz: all definitions replayed through the s39 line fuzzer and the s40
    definition fuzzer for 10 minutes each, zero findings, ASan clean.
11. `.docs/syntax-def.md` updated: go named as the template definition;
    the four aux matchers each cite their real consumer (sh, rust/python/
    toml, markdown, yaml); the `value` flag documented with the js
    heuristic as its worked example.
12. ≥ 140 golden span files across the eight new languages, all byte-stable
    across two runs; ≥ 100 assertions across the three new unit files.

### Asynchronous Daily Driver milestone gate

This field gate is intentionally not part of Sprint 42 implementation
completion. It is earned only when every §10 box has committed evidence,
`.docs/dogfood-log.md` has ≥ 10 dated rows and ≥ 40 logged hours, the live
latency histogram covers ≥ 200,000 real keystrokes with p99 ≤ 5 ms, and
`.docs/milestones.md` records `daily driver: EARNED` with the reference machine
and B1–B10 revocation list. The milestone commit may land during Sprint 42.5
or later; Sprint 42 implementation completeness does not earn it.
