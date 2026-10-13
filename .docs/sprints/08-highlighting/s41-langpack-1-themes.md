# Sprint 41: Langpack 1 (C, Fletch, sh, make, markdown) and Themes

## Prerequisites

- Sprint 39 — the engine, the 54-entry `SynAttr` table, `SynState` with
  `aux`/`flags`/`lost`, `YEW_SYN_DEPTH_MAX = 16`,
  `YEW_SYN_LINE_BYTE_CAP = 64 KiB`, `yew_syn_settle` and its budgets, the
  span cache, and `src/syn/theme.h`'s `ThemeEnt` interface with its
  built-in debug palette.
- Sprint 40 — the schema (§1–§4), `yew_syn_def_compile`, the four aux
  matchers, `consume`, `captures`, `include:`, `at_eol`, `unit` marks, the
  XDG cache, `yew syn check|compile|dump`, the validator, and
  `.docs/syntax-def.md` (this sprint's definitions are its worked
  examples in anger).
- Sprint 5 — the colour tiers and `yew_rgb_to_256` (the 256-colour column
  of §7's table is literally that function's output), the 16-colour table
  and its tie rule, the `attrs` bitmask with **bits 10–15 free**, and
  s05 §2's explicit deferral: *"Underline colours (`58;…`) are Sprint 41's
  problem."* — discharged in §9.
- Sprint 15 — the statusline segment table and drop priorities, `ed.view.*`
  registration shape, `full_damage`, the scroll-throughput gate.
- Sprint 3 — `TtyCaps.truecolor`, the env-lookup injection shape used by
  `yew_render_tier`; Sprint 36 — the options model and `set { … }` (this
  sprint registers `theme`, `theme_auto`; if Sprint 36 has not landed in
  your branch order, the option is settable via `ed.theme.set` and the
  option row is added there).
- Sprint 28/29 — pure-literal Fletch (themes are data too, same grants-
  nothing property) and `.docs/fletch-spec.md` §3's motion vocabulary,
  which `fletch.fl` must highlight exactly.
- Binding: `00-decisions.md` (*"Ships ≥ 12 languages + default dark/light
  themes"*, scroll ≥ 120 fps equivalent, keypress p99 ≤ 5 ms — invariants
  4, 5, 7).

## Goals

Ship the first five language definitions and the whole theme layer: `c`
(preprocessor lines and their continuations, string/char escapes, and an
honest account of what comment nesting does and does not do), `fletch`
(dogfooding — the terse motion literals get the four attrs Sprint 39
reserved for them), `sh` (POSIX plus the bashisms people actually type,
with heredocs carried in the state's `aux`), `make` (where a tab is
semantics, not whitespace), and `markdown`. **Decision, taken here:
markdown ships without embedded-language highlighting** — a fenced block
is `code`, its info string is styled, and the fence text is already
carried in `aux` so the extension is additive; the mechanism is deferred to
`s41_5-embedded-languages.md` for the reasons in §6. Then the theme layer:
the attr→colour format in Fletch data, `quiver-dark` and `quiver-light`
with a full 54-attr coverage table in truecolor and 256, runtime switching,
underline colours (s05's deferral), and the `NO_COLOR`/16-colour/`TERM=dumb`
degradation ladder. Closing with the perf gate: full-viewport restyle
inside the frame budget on named, frozen fixtures. Sprint 42 adds the other
eight languages and re-gates cumulatively.

## Deliverables

### 1. `runtime/syntax/c.fl`

Contexts: `main`, `preproc`, `preproc_include`, `dq`, `sq`, `block_comment`,
`line_comment`. Highlights worth pinning:

| Construct | Rule shape | Notes |
|---|---|---|
| directive | `^\s*(#)\s*([a-z]+)` → `preproc`, `push: "preproc"` | the `#` and the word are separate captures so `# define` (legal) styles correctly |
| directive continuation | `preproc` has `at_eol: "pop"`, plus a rule `\\\s*$` with `attr: "operator"` and **no** state op | a trailing backslash means the directive continues, so the pop must not happen — expressed by making the backslash rule `set: "preproc_cont"`, a context identical to `preproc` but with `at_eol: "stay"`, which `set`s back to `preproc` on any line without a trailing backslash |
| `#include <…>` | `preproc_include` context entered from `include`/`import` | `<stdio.h>` is `string`, not two operators |
| char literal | `'([^'\\\n]\|\\(x[0-9a-fA-F]{1,2}\|[0-7]{1,3}\|u[0-9a-fA-F]{4}\|U[0-9a-fA-F]{8}\|['"?\\abfnrtv]))'` | one anchored rule, `at_eol` irrelevant because it cannot span lines |
| string escapes | same escape alternation inside `dq`, `attr: "string.escape"` | an unknown escape (`\q`) gets `error` — C says undefined, and saying so is useful |
| line-continued string | `dq` rule `\\$` with no state op, `at_eol: "stay"` | a `"abc\` continues on the next line; without this rule `at_eol: "pop"` would end it |
| numbers | int/float/hex/binary/octal with `u U l L f F` and `'` digit separators (C23) | one rule, `attr: "number"` |
| types | `\b[A-Za-z_]\w*_t\b`, the fixed-width set, `struct\|union\|enum\|typedef` followed by an identifier via `captures` | heuristic and admitted as such |
| function | `([A-Za-z_]\w*)\s*\(` with `captures: {1:"function"}` and `consume: 1` | the no-lookahead workaround from s40 §3 |
| todo | inside comments: `\b(TODO\|FIXME\|XXX\|HACK)\b` → `comment.todo` | |

**Comment nesting — the honest note (it goes in the file's header
comment).** C block comments **do not nest**: `/* /* */ */` ends at the
first `*/`, and the trailing `*/` is a syntax error in real C. Our
`block_comment` context has exactly one pop rule (`\*/`) and no push, so it
matches the language exactly. Two adjacent traps we *do* handle, and one we
deliberately do not:

- `//` containing `/*` must not open a block comment → `line_comment` has
  `at_eol: "pop"` and no rules that push.
- `"/*"` inside a string must not open a comment → the `dq` context has no
  comment rules; contexts are the mechanism, not a special case.
- **`#if 0` blocks are not greyed out.** Doing that requires evaluating the
  preprocessor, which is a compiler, not a highlighter. A definition that
  guesses gets it wrong on `#if defined(X)` and greys out live code, which
  is worse than doing nothing. Stated in the file and in
  `.docs/syntax-def.md` as the canonical example of a limit we accept.

### 2. `runtime/syntax/fletch.fl` — dogfooding

The motion sublanguage is a real lexer mode (s28 §2: *"this is a lexer
mode, not parser lookahead"*), so it is a real context: `@[` pushes
`motion`, `]` pops, and `H(` pushes `motion` again (nesting, depth-capped).
Inside `motion`, Sprint 39's reserved attrs finally pay off:

| Token | Attr | Rule |
|---|---|---|
| `l w b c` | `motion.unit` | `\b[lwbc]\b` with terminator check |
| `< > ^ v`, `a<` `a>` `a^` `av`, `→ ← ↑ ↓` | `motion.arrow` | one alternation; the Unicode aliases are accepted (s28 §3) even though the recorder never emits them |
| digits before a word | `motion.count` | `[0-9]+` |
| `H(` `)` | `punct.bracket` | pushes/pops `motion` |
| `i"…"` | `keyword` + `string` | `i` styled `keyword`, the string via `include:strings` |
| `del` `esc` | `keyword.control` | |
| any other identifier | `motion.cmd` | CMDWORD — resolved at run time, so we style it as a call |

Outside `@[…]`: the 22 reserved words (`keyword` / `keyword.control` split
per s28 §9's list), `fn`/`macro` names via `captures` + `consume`, `#`
comments, strings with the exact escape set from spec §1 (`\n \t \r \\ \"
\0 \xNN \u{…}`, unknown escape → `error`, matching the lexer's own rule),
ints with `_` separators and `0x`, floats, map keys (`variable.member`),
and `nil true false` as `constant.builtin`/`boolean`.

**Why this definition matters more than its size:** every user reads their
config, their macros, and their recorded keystrokes in Fletch. It is the
definition that gets read on day one, and it is the one whose correctness
we can check against a real lexer — DoD 5 cross-checks it against
`yew --fl-dump-ast` token spans on the spec §14 example.

### 3. `runtime/syntax/sh.fl` — POSIX plus the common bashisms

In scope: comments, `'…'` (no escapes at all — pitfall: a definition that
honours `\` inside single quotes breaks every `sed` one-liner), `"…"` with
`\$ \` \" \\ \newline` escapes and `$var`/`${…}`/`$(…)`/`` `…` ``
expansions nested inside, `$'…'` ANSI-C quoting with its own escape set,
`$((…))` arithmetic, keywords (`if then elif else fi case esac for while
until do done in function select time`), builtins and common externals as
`function.builtin`, `[[ … ]]` and `[ … ]` tests, `case` patterns, function
definitions (`name()` via `captures`+`consume`), variable assignment
(`^\s*([A-Za-z_]\w*)(\+?=)` with `consume: 2`), arrays and `${a[@]}`,
`local`/`declare`/`export`/`readonly` as `keyword.storage`, `#!` line 1 as
`preproc`, and options (`-x`, `--long`) as `constant`.

Out, and named in the file header: `shopt`-dependent parsing, arithmetic
`for` (styled generically), process substitution `<(…)` (the `<(` styles as
operators, the body as ordinary shell — it happens to work), and
zsh/fish-only syntax (never a "mostly bash" fudge). This sprint originally
deferred their separate definitions to post-1.0; the later binding Sprint
42.5 contract pulls both into the built-in pack.

**Heredocs — the pitfall this definition exists to prove.** The delimiter
is not in the grammar; it is in the *text*, and the state machine must
carry it. Sprint 40's `set_aux` + `line_eq` matcher is exactly this:

```fletch
# In `main`.  Unquoted delimiter: expansions apply inside the body.
{ match: "<<(-?)\\s*([A-Za-z_][A-Za-z0-9_]*)",
  attr: "operator", captures: { 2: "string" },
  set_aux: 2, strip: true,             # `strip` honours the `<<-` form
  push: "heredoc_pending" },
# Quoted delimiter (<<'EOF' / <<"EOF"): expansions are literal.
{ match: "<<(-?)\\s*['\"]([A-Za-z_][A-Za-z0-9_]*)['\"]",
  attr: "operator", captures: { 2: "string" },
  set_aux: 2, strip: true, push: "heredoc_pending_raw" },

# The body starts on the NEXT line, so the pending context styles the rest
# of THIS line as ordinary shell and hands over at end of line.
heredoc_pending:     { include: "main", at_eol: "set:heredoc"     },
heredoc_pending_raw: { include: "main", at_eol: "set:heredoc_raw" },

heredoc: {
    default: "string.special", at_eol: "stay", unit: "atom",
    rules: [
        { aux: "line_eq", attr: "string", pop: 1 },   # the terminator line
        { match: "\\$\\{?[A-Za-z_][A-Za-z0-9_]*\\}?", attr: "variable" },
    ],
},
heredoc_raw: {                       # same, minus the expansion rule
    default: "string.special", at_eol: "stay", unit: "atom",
    rules: [ { aux: "line_eq", attr: "string", pop: 1 } ],
},
```

Three details that are each a bug if missed, and each a test:

1. **The heredoc body starts on the *next* line**, not after the `<<EOF`
   token — `cat <<EOF | tee f` is legal shell and the pipe is not part of
   the body. A rule that pushes `heredoc` directly styles the remainder of
   the current line with `heredoc`'s `default`, which is wrong. The
   *pending* context above is the fix and it needs no new machinery: it
   `include:`s `main` so the rest of the line stays ordinary shell, and its
   `at_eol: "set:…"` swaps the top of stack exactly at the line boundary.
   `.docs/syntax-def.md` gains this as the documented "deferred entry"
   idiom; markdown's front matter and make's `define` use it too.
2. **Quoted delimiters suppress expansion** (`<<'EOF'`): two rules, two
   pending contexts, two body contexts. Selecting behavior by *which rule
   matched* rather than by a runtime flag keeps first-match-wins as the
   only control flow in the format.
3. **`<<-` strips leading tabs on the terminator only.** `strip: true` sets
   `YEW_SYN_F_STRIP`, which is what `line_eq` consults. A definition that
   strips leading *spaces* is wrong and users will file it as a bug against
   the shell.

Two heredocs on one line (`cmd <<A <<B`) is out of scope: `aux` holds one
delimiter. The file says so; the second body styles as the first's.

### 4. `runtime/syntax/make.fl` — tabs are semantics

| Construct | Rule | Attr |
|---|---|---|
| recipe line | `^\t` at position 0 → `attr: "whitespace.special"`, `push: "recipe"` (with `at_eol: "pop"`) | the tab itself is *rendered*, which is the point |
| **spaces where a tab belongs** | `^ {2,}[^ \t#].*$` on a line following a target → `warning` | this is Make's most infamous error message ("missing separator"); styling it is worth one heuristic rule, and `warning` (not `error`) because a leading-space line is legal inside `define` blocks |
| target | `^([^\t:=#][^:=#]*?)\s*(::?)` with `captures: {1:"label", 2:"operator"}`, `consume: 2` | |
| special targets | `\.(PHONY\|SUFFIXES\|DEFAULT\|PRECIOUS\|SECONDARY\|NOTPARALLEL\|ONESHELL\|DELETE_ON_ERROR)\b` | `attribute` |
| assignment | `^\s*([A-Za-z_][\w.]*)\s*([:?+!]?=)` , `consume: 2` | `variable` + `operator` |
| conditionals | `^\s*(ifeq\|ifneq\|ifdef\|ifndef\|else\|endif)\b` | `keyword.control` |
| include | `^\s*(-?include\|sinclude\|vpath\|export\|unexport\|override\|define\|endef)\b` | `preproc` |
| function call | `\$[({](\w[\w-]*)\s` with `captures: {1:"function.builtin"}` | `$(wildcard …)`, `$(patsubst …)` |
| variable ref | `\$[({][^)}]*[)}]`, `\$[A-Za-z_@<^?*+%]` | `variable` / `variable.builtin` for the automatics |
| recipe prefixes | `^[\t]([@+-]+)` | `operator` |
| comment | `#.*$` in `main` | `comment` |

**Pitfall, stated in the file:** inside a recipe line, `#` is passed to the
shell, so it is a *shell* comment, not a Make comment — but the two look
identical and the distinction only matters for `$(shell)` quoting, so
`recipe` styles `#…` as `comment` and says why. Recipe bodies are otherwise
styled generically (variables, strings, prefixes); dispatching them into
`sh` is the same embedded-language mechanism markdown wants, and is
deferred to the same place.

### 5. `runtime/syntax/markdown.fl`

Contexts: `main`, `fenced`, `indented_code`, `html_block`, `link_dest`,
`emph`/`strong` handled inline. Coverage: ATX headings (`#`–`######`,
`heading`, with the trailing-hash form), setext headings (`===`/`---` on
the following line — expressible only as a rule on the *underline* line,
which is what we do: the underline styles `heading`, the text above does
not — an admitted limitation, since a line-state machine cannot look
forward), lists (`-`/`*`/`+`/`1.` → `list`), block quotes (`>` → `quote`),
thematic breaks, inline code (`` `…` `` and `` ``…`` `` with matched run
lengths via `set_aux` + the `literal` matcher), emphasis/strong (`*`/`_`
pairs, single-line only), links and images (`[text](dest "title")` and
reference forms → `link` for the destination), autolinks, HTML blocks,
tables (pipes → `punct.delim`, the delimiter row → `operator`), footnotes,
and front matter (`---` on line 1 → `preproc` block, popped by the closing
`---`).

**Fenced code blocks:**

```fletch
{ match: "^ {0,3}(`{3,}|~{3,})[ \\t]*([A-Za-z0-9_+.-]*)",
  attr: "punct", captures: { 1: "punct", 2: "attribute" },
  set_aux: 1, push: "fenced" },

fenced: {
    default: "code", at_eol: "stay", unit: "atom",
    rules: [ { aux: "fence_close", attr: "punct", pop: 1 } ],
},
```

The info string is captured and styled; `aux` already holds the opening
fence run, which is what `fence_close` needs (a closing fence must be at
least as long as the opening one and use the same character — the rule
CommonMark actually specifies, and the one every naive `^```$` definition
gets wrong on nested fences in documentation about markdown).

### 6. The embedded-language decision

**Markdown ships without embedded highlighting in 1.0's Campaign 08.**
Fenced blocks are `code`; the info string is `attribute`. The mechanism is
deferred to a fractional sprint, `s41_5-embedded-languages.md`, which also
covers make recipes → `sh`, HTML `<script>`/`<style>`, and shell `$(…)`.
Three reasons, in order of weight:

1. **It changes Sprint 39's state representation, which is load-bearing.**
   A cross-definition state must name a *definition* as well as a context —
   that is the reserved `SynState.def` byte, currently asserted 0 — and it
   changes the interned tuple, the fixpoint comparison, and the meaning of
   every context id in a golden dump. Doing that *after* Sprints 41 and 42
   commit ~13 languages' worth of span goldens means re-gating all of them.
   Doing it in its own sprint means one set of fixtures written against the
   final representation.
2. **It creates a new latency hazard on the render path.** An embedded
   dispatch can require a definition that is not compiled yet, and Sprint
   40 made compilation lazy precisely to protect the 20 ms cold-start gate.
   "Compile a definition inside `yew_syn_line`" is either a stall or a new
   asynchronous state machine; either deserves its own budget, its own
   fallback, and its own gate — not a paragraph inside a sprint whose main
   risk is a 54-row colour table.
3. **The intermediate product is genuinely good.** Markdown with headings,
   lists, links, emphasis, tables and delimited code blocks is what most
   people want most of the time; markdown with *half-working* embedded
   highlighting (right for the languages that happen to be loaded, wrong
   after a scroll) is worse than either endpoint.

What this sprint pays forward so the extension is purely additive: `aux`
already carries the fence info string; `fenced` is marked `unit: "atom"`
so B mode already treats a code block as one structural unit; and Sprint
40's compiler already rejects an `embed:` key with a message naming the
extension sprint (invariant 3).

### 7. Theme format and the two default themes — `src/syn/theme.c`, `runtime/themes/*.fl`

```fletch
{
    theme:   1,
    name:    "quiver-dark",
    kind:    "dark",                       # "dark" | "light"
    palette: { bg: "#12141a", fg: "#c8ccd4", violet: "#c678dd", … },
    ui: {                                  # non-attr roles (s15/s17/s22)
        bg: "@bg", fg: "@fg",
        sel:  { bg: "#2c313a" },
        cursor: { bg: "@fg", fg: "@bg" }, "cursor.secondary": { reverse: true },
        gutter: { fg: "#4b5263" }, "gutter.cursor": { fg: "@fg", bold: true },
        "mode.line": { bg: "#61afef", fg: "@bg" }, …
        "search.match": { bg: "#3e4451" }, "search.current": { bg: "#e5c07b", fg: "@bg" },
        ul: { error: "#ff5f5f", warn: "#e5c07b", info: "#61afef" },   # §9
    },
    attrs: {
        keyword: { fg: "@violet", bold: false },
        comment: { fg: "#5c6370", italic: true, mono: "italic" },
        error:   { fg: "#ff5f5f", bold: true, undercurl: true, ul: "error",
                   c256: 203, mono: "reverse" },
        …
    },
}
```

Value forms for any colour field: `"#rrggbb"` · `"@name"` (palette
reference) · `{ idx: N }` (ANSI 0–15 or 256 index, passed through untouched
at every tier per s05 §7) · `"none"` (terminal default, `39`/`49`).
Per-attr style fields: `bold italic underline undercurl strike dim reverse`.
Per-attr overrides: `c256` (integer, overrides s05's converter),
`c16` (integer 0–15), `ul` (which of the three underline colours),
`mono` (the `NO_COLOR` rendition — one of `plain bold italic underline
reverse dim`).

Why explicit `c256`/`c16` overrides exist: s05's converter is deterministic
and good, but a handful of attrs (comment grey on a dark background) land
on an index that is unreadable against the user's actual palette. The
override is per-attr, opt-in, and absent by default so determinism is
preserved.

Loading: `fl_parse_literal` → `yew_theme_compile` → a flat
`ThemeEnt tab[64]` (`{YewColor fg, bg; u16 attrs;}`, 640 bytes) with the
s39 §1 fallback chain resolved once, transitively, terminating at `text`.
The draw path does one array index per span. Discovery order:
`$XDG_CONFIG_HOME/yew/themes/NAME.fl`, then `runtime/themes/NAME.fl`.
Themes are **not** cached to disk — they are ~4 KiB and compile in
microseconds.

#### Full attr coverage — every one of the 54 attrs, both themes

Colour columns are truecolor hex; the `256` columns are **the output of
`yew_rgb_to_256` on that hex**, committed here and re-derived by a unit
test — if a row disagrees, this table is wrong, not the converter (DoD 8).
`Style · Mono` gives the style bits and the `NO_COLOR` rendition; `—` means
none.

| Attr | Dark | 256 | Light | 256 | Style · Mono |
|---|---|---|---|---|---|
| `text` | `#c8ccd4` | 252 | `#24292f` | 235 | — · — |
| `keyword` | `#c678dd` | 176 | `#cf222e` | 160 | — · bold |
| `keyword.control` | `#c678dd` | 176 | `#cf222e` | 160 | bold · bold |
| `keyword.op` | `#56b6c2` | 73 | `#0550ae` | 25 | — · bold |
| `keyword.storage` | `#c678dd` | 176 | `#cf222e` | 160 | italic · italic |
| `type` | `#e5c07b` | 180 | `#953800` | 94 | — · — |
| `type.builtin` | `#e5c07b` | 180 | `#953800` | 94 | italic · italic |
| `constant` | `#d19a66` | 173 | `#0550ae` | 25 | — · — |
| `constant.builtin` | `#d19a66` | 173 | `#0550ae` | 25 | bold · bold |
| `number` | `#d19a66` | 173 | `#0550ae` | 25 | — · — |
| `boolean` | `#d19a66` | 173 | `#0550ae` | 25 | bold · bold |
| `character` | `#98c379` | 114 | `#0a3069` | 23 | — · — |
| `string` | `#98c379` | 114 | `#0a3069` | 23 | — · — |
| `string.escape` | `#56b6c2` | 73 | `#1b7c83` | 30 | — · bold |
| `string.interp` | `#ff79c6` | 212 | `#bf3989` | 132 | — · bold |
| `string.special` | `#98c379` | 114 | `#0a3069` | 23 | italic · italic |
| `comment` | `#5c6370` | 241 | `#6a737d` | 243 | italic · italic |
| `comment.doc` | `#7f8c98` | 245 | `#57606a` | 241 | italic · italic |
| `comment.todo` | `#e5c07b` | 180 | `#9a6700` | 94 | bold · reverse |
| `function` | `#61afef` | 75 | `#8250df` | 98 | — · — |
| `function.builtin` | `#61afef` | 75 | `#8250df` | 98 | italic · italic |
| `function.macro` | `#61afef` | 75 | `#8250df` | 98 | bold · bold |
| `method` | `#61afef` | 75 | `#8250df` | 98 | — · — |
| `variable` | `#c8ccd4` | 252 | `#24292f` | 235 | — · — |
| `variable.builtin` | `#e06c75` | 168 | `#cf222e` | 160 | italic · italic |
| `variable.param` | `#c8ccd4` | 252 | `#24292f` | 235 | italic · italic |
| `variable.member` | `#e06c75` | 168 | `#116329` | 22 | — · — |
| `namespace` | `#e5c07b` | 180 | `#953800` | 94 | — · — |
| `label` | `#ff79c6` | 212 | `#bf3989` | 132 | — · underline |
| `attribute` | `#e5c07b` | 180 | `#9a6700` | 94 | — · — |
| `preproc` | `#c678dd` | 176 | `#cf222e` | 160 | — · bold |
| `operator` | `#56b6c2` | 73 | `#0550ae` | 25 | — · — |
| `punct` | `#9aa0aa` | 247 | `#57606a` | 241 | — · — |
| `punct.bracket` | `#9aa0aa` | 247 | `#57606a` | 241 | — · — |
| `punct.delim` | `#9aa0aa` | 247 | `#57606a` | 241 | — · — |
| `tag` | `#e06c75` | 168 | `#116329` | 22 | — · bold |
| `tag.attr` | `#d19a66` | 173 | `#8250df` | 98 | — · — |
| `heading` | `#61afef` | 75 | `#0550ae` | 25 | bold · bold |
| `link` | `#56b6c2` | 73 | `#0550ae` | 25 | underline · underline |
| `emphasis` | `#c8ccd4` | 252 | `#24292f` | 235 | italic · italic |
| `strong` | `#c8ccd4` | 252 | `#24292f` | 235 | bold · bold |
| `code` | `#98c379` | 114 | `#116329` | 22 | — · — |
| `list` | `#c678dd` | 176 | `#cf222e` | 160 | — · bold |
| `quote` | `#5c6370` | 241 | `#6a737d` | 243 | italic · italic |
| `diff.add` | `#98c379` | 114 | `#116329` | 22 | — · — |
| `diff.del` | `#e06c75` | 168 | `#cf222e` | 160 | — · — |
| `error` | `#ff5f5f` | 203 | `#a40e26` | 124 | bold, undercurl (`ul:error`) · reverse |
| `warning` | `#e5c07b` | 180 | `#9a6700` | 94 | undercurl (`ul:warn`) · underline |
| `whitespace.special` | `#3a3f4b` | 238 | `#d0d7de` | 188 | — · — |
| `motion.unit` | `#c678dd` | 176 | `#cf222e` | 160 | bold · bold |
| `motion.arrow` | `#56b6c2` | 73 | `#0550ae` | 25 | — · — |
| `motion.count` | `#d19a66` | 173 | `#0550ae` | 25 | — · — |
| `motion.cmd` | `#61afef` | 75 | `#8250df` | 98 | — · — |
| `ui.invisible` | `#3a3f4b` | 238 | `#d0d7de` | 188 | — · — |

Every row is present in both theme files — **no attr inherits by
omission in a shipped theme**, because the fallback chain exists for
*third-party* themes, and a default theme that leans on it makes "why is
this the wrong colour" unanswerable.

### 8. Runtime switching and discovery

```c
bool yew_theme_load(Ed *ed, const char *name, DiagCtx *dc);
const ThemeEnt *yew_theme_tab(const Ed *ed);      /* the 64-entry array  */
```

| Surface | Behavior |
|---|---|
| `set { theme: "quiver-light" }` (s36) | applied at config load and on reload |
| `ed.theme.set NAME` / `:theme NAME` | switch now; unknown name → message, old theme kept |
| `ed.theme.toggle` | swap between the last dark and last light theme |
| `yew --theme NAME` | overrides config for one run |
| `YEW_THEME=NAME` | overrides config; loses to `--theme` |
| `set { theme_auto: true }` | at startup, query the terminal background with OSC 11 (s03's probe shape) and pick `kind` accordingly. **Default off** — it costs a probe round-trip against a 20 ms cold-start gate, and terminals that ignore OSC 11 make it a timeout |

Switching cost: rebuild the 640-byte table, `yew_grid_mark_all`,
`full_damage`, one frame — a full repaint, budgeted at ≤ 2 ms at 200×50,
which the s05 harness already measures. Buffer state and span caches are
untouched: **a theme change never re-runs the syntax engine**, because
attrs are semantic (the whole point of s39 §1). DoD 9 asserts
`yew_syn_line` call count is zero across a theme switch.

### 9. Colour degradation and underline colours

The ladder, evaluated per frame from `yew_render_tier` (s05 §7) plus the
env:

| Condition | Effect |
|---|---|
| `NO_COLOR` set to **anything, including empty** (the spec's rule) | all `fg`/`bg` → terminal default; each attr renders as its `mono` rendition only |
| `TERM=dumb` | as `NO_COLOR`, and all attrs dropped too — plain text |
| `YEW_COLORS=16` or tier 16 | `c16` override if present, else s05's nearest-ANSI with its documented tie rule |
| tier 256 | `c256` override if present, else `yew_rgb_to_256` |
| tier truecolor | hex verbatim |

The `mono` column is why `NO_COLOR` is still usable: comments stay italic,
keywords bold, errors reverse — structure survives when colour does not.

**Underline colours — discharging s05 §2's deferral without resizing
`Cell`.** `sizeof(Cell) == 20` is a pinned budget with a `_Static_assert`
and a cache-footprint table behind it; adding a 4-byte `sp` colour would
break both. Instead, `Cell.attrs` **bits 10–11** (free per s05 §2) become a
2-bit `YEW_CELL_UL` field selecting one of four theme underline colours:
`0 = same as fg` (the default, no `58;` emitted), `1 = ul.error`,
`2 = ul.warn`, `3 = ul.info`. `render.c` emits `CSI 58;2;R;G;B m` before an
underlined run whose selector is nonzero and `CSI 59 m` when it changes
back, **only when `tier == truecolor`** — the same population gate s05
applies to `4:3` undercurl. At lower tiers the underline is drawn in the
foreground colour and nothing is lost but a hue.

This is the plumbing Sprint 46 (LSP diagnostics) and Sprint 53 (git) will
use; this sprint's only consumer is `error`/`warning` attrs, which is
enough to golden-test it.

### 10. Perf gate — named frozen fixtures and their numbers

Fixtures under `tests/perf/fixtures/syn/`, **frozen** (a snapshot committed
once; they must not track a moving source tree, or the baselines move on
their own):

| Fixture | Content | Lines | Size |
|---|---|---|---|
| `c_kitchen.c` | concatenated snapshot of yew's own `src/` at this sprint's commit | 8 000 | 244 KiB |
| `c_comment_bomb.c` | `c_kitchen.c` with an unterminated `/*` prepended | 40 001 | 1.2 MiB |
| `c_minified.c` | one line, 512 KiB, no newline | 1 | 512 KiB |
| `fl_kitchen.fl` | the default config + macro library + spec §14 example, ×20 | 2 000 | 58 KiB |
| `sh_kitchen.sh` | 40 heredocs (incl. `<<-`, quoted, nested quotes), all bashisms | 3 000 | 92 KiB |
| `mk_kitchen.mk` | conditionals, recipes, `define` blocks, one spaces-not-tab line | 1 200 | 36 KiB |
| `md_kitchen.md` | 200 fenced blocks, tables, nested lists, front matter | 5 000 | 160 KiB |

Budgets — all measured by `tests/perf/perf_syn.c`, baselines committed to
`tests/perf/baselines/syn.txt`, median regression trip at 1.2×:

| Scenario | Budget | Why |
|---|---|---|
| **Full-viewport restyle, cold cache, 200 rows × 100 cols, any fixture** | **≤ 1.5 ms** | 18 % of the 8.3 ms 120 fps frame; layout + draw + render own the rest |
| Full-viewport restyle, 80×24 | ≤ 300 µs | |
| Per-line median, `c_kitchen.c` | ≤ 3.0 µs | |
| Per-line p99, `c_kitchen.c` | ≤ 12 µs | |
| Single-char edit → settled (fixpoint ≤ 2 lines) | ≤ 60 µs | keypress p99 is 5 ms *total* |
| Whole-file settle, `c_kitchen.c` | ≤ 45 ms accumulated, **no single frame > 1 ms** | |
| `c_comment_bomb.c`: viewport correct after the paste | ≤ 1 frame | the §6/s39 propagation guarantee |
| `c_comment_bomb.c`: full settle | ≤ 400 ms accumulated idle | |
| `c_minified.c` open → first paint | ≤ 20 ms | `YEW_SYN_LINE_BYTE_CAP` is what makes this reachable |
| Theme switch, 200×50 | ≤ 2 ms, 0 `yew_syn_line` calls | |
| Scroll throughput, `md_kitchen.md`, wrap off/on | ≥ 120 fps equivalent | s15's gate, now with highlighting in the frame |
| Peak RSS delta, `c_comment_bomb.c` (state array) | ≤ 200 KiB | 4 B/line × 40 k lines + caches |

### 11. Defer

- python, rust, go, js/ts, fortran, json, yaml, toml → **Sprint 42**.
- Embedded languages (markdown fences, make recipes, HTML `<script>`,
  shell `$(…)`) → **`s41_5-embedded-languages.md`**, per §6. `SynState.def`
  stays 0; an `embed:` key in a definition is a Sprint 40 validation error
  naming that sprint.
- Setext-heading text styling (needs forward context) and `#if 0` dimming
  (needs a preprocessor) → **not in 1.0**; both documented as limits in
  their definition files.
- Two heredocs on one line; zsh/fish; `shopt`-dependent parsing → **not in
  1.0**, named in `sh.fl`.
- User-configurable per-attr overrides in `init.fl` (`set { attrs: { … } }`
  merging over the loaded theme) → **Sprint 36**'s option surface; the
  merge point is `yew_theme_load`, which takes an override map today and
  is called with an empty one.
- Underline-colour consumers (diagnostics, git hunks) → **Sprints 46/53**;
  the 2-bit field and the emitter land here.
- Spell checking, rainbow brackets, colour-code previews → post-1.0
  plugins (Sprint 54's hook surface is sufficient).

## Testing Strategy

- **Golden spans** (`tests/syn/<lang>/*.<ext>` + `*.spans`): every
  definition gets a kitchen-sink fixture plus focused fixtures per
  pitfall — C: preprocessor continuation, `#include <>` vs `""`, `//`
  containing `/*`, `"/*"` in a string, every escape form, `\q`, a
  line-continued string. sh: all four heredoc forms, `<<-` with tabs and
  with spaces (the second must *not* terminate), `'…\…'`, `$'…'`,
  `${a[@]}`, nested `$( "$(…)" )`. make: tab recipe, spaces-not-tab,
  `define`/`endef`, all automatic variables. markdown: nested fences of
  different lengths, a fence containing ` ``` `, front matter, tables.
  fletch: the spec §14 example, every motion word, `H(` nesting, unknown
  escape. Goldens print **context names**, never ids (s40 rule).
- **Context coverage, mechanically**: `yew syn check --coverage` over each
  definition and its fixture set must report **100 % of contexts entered
  and 100 % of rules fired**; an unfired rule fails CI. This is what stops
  a definition growing dead rules nobody notices.
- **Cross-check, fletch** (`tests/unit/test_syn_fletch.c`): token spans
  from the real lexer (`fl_lex_next` over the spec §14 example) compared
  against the definition's spans; every token's byte range must be covered
  by exactly one span whose attr is in that token kind's allowed set. A
  definition that disagrees with its own language's lexer is a bug.
- **Unit** (`tests/unit/test_theme.c`): both themes parse with zero
  warnings; all 54 attrs present in each (no fallback used); the fallback
  chain resolves correctly for a *synthetic* partial theme; palette
  references, `{idx:N}`, and `"none"`; unknown key/colour errors with
  caret; `c256`/`c16`/`ul`/`mono` overrides applied.
- **Unit** (`tests/unit/test_theme_256.c`): for all 108 colour cells in
  §7's table, `yew_rgb_to_256(hex) == the tabled index`. This makes the
  documentation itself a test.
- **Unit** (`tests/unit/test_theme_degrade.c`): the §9 ladder across
  `NO_COLOR=""`, `NO_COLOR=0`, `TERM=dumb`, `YEW_COLORS=16`, `=256`,
  truecolor — asserting emitted SGR parameter sets, including that
  `NO_COLOR` emits no `38;`/`48;` at all.
- **Pty goldens**: each definition's kitchen-sink fixture rendered at 80×24
  in both themes at all three tiers (byte-exact SGR runs); the underline
  colour sequence `58;2;…`/`59` present at truecolor and absent at 256; a
  theme switch producing exactly one full repaint; a CJK+emoji line inside
  a C string proving the cluster/attr rule (s39 §7) and the wide-glyph
  style-equality rule (s05 §4).
- **Determinism lane**: same fixture + theme rendered twice → byte
  identical; cold vs warm syntax cache → identical.
- **Perf**: §10, `make perf`, baselines committed. The
  `c_comment_bomb.c` frame-budget assertion runs under a fake clock so it
  is a hard gate, not a timing-flake.
- Lanes: gcc/clang zero-warning, ASan/UBSan, valgrind on the theme and
  definition unit binaries.

## Definition of Done

1. `make && make test` green on gcc and clang, zero warnings;
   ASan/UBSan/valgrind/determinism lanes green.
2. `yew syn check --strict runtime/syntax/{c,fletch,sh,make,markdown}.fl`
   exits 0 with zero warnings; `yew syn compile --all` is idempotent.
3. **100 % context and rule coverage** for all five definitions against
   their committed fixtures (`yew syn check --coverage`), enforced in CI.
4. Every pitfall in §1–§5 has a named golden fixture: C's `//`-containing-
   `/*`, `"/*"`-in-string, backslash-continued directive and string,
   unknown escape → `error`; sh's four heredoc forms plus `<<-` with
   spaces not terminating and the quoted-delimiter no-expansion rule;
   make's tab-vs-spaces pair; markdown's longer-closing-fence and
   fence-containing-backticks pair.
5. The Fletch cross-check passes on the spec §14 example: every lexer token
   covered by exactly one span with an allowed attr; all four `motion.*`
   attrs appear.
6. Both themes define all 54 attrs explicitly (`grep -c` per file == 54 and
   a unit test asserting no fallback was consulted during their load).
7. `.docs/syntax-def.md` gains the heredoc idiom (§3's deferred-push
   pattern) and the `unit:` guidance; `.docs/themes.md` documents the
   format, every field, both defaults, and the degradation ladder.
8. All 108 tabled 256-colour values equal `yew_rgb_to_256` of their hex
   (test regenerates and compares; a mismatch fails CI).
9. Theme switch: one full repaint, ≤ 2 ms at 200×50, and **zero**
   `yew_syn_line` calls (counter asserted).
10. `NO_COLOR` (empty and non-empty) emits no `38;`/`48;` parameters in a
    full-frame golden; `TERM=dumb` emits no SGR at all; `YEW_COLORS=16`
    emits only 30–37/90–97/40–47/100–107.
11. Underline colours: `58;2;R;G;B` + `59` emitted for `error`/`warning`
    runs at truecolor only; `sizeof(Cell) == 20` still asserts; s05's
    fuzz assertion (no sequence outside the accepted set) extended to
    accept `58`/`59` and re-run.
12. All §10 budgets green with committed baselines; the
    `c_comment_bomb.c` no-frame-over-1 ms assertion is a hard gate under
    the fake clock; scroll throughput on `md_kitchen.md` still ≥ 120 fps
    equivalent with highlighting on.
13. `SynState.def == 0` asserted for every state produced by all five
    definitions across the whole fixture corpus; an `embed:` key errors
    naming `s41_5`.
14. ≥ 90 golden span files across the five languages, all byte-stable
    across two runs; ≥ 120 assertions across the three new unit files.
