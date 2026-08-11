# Syntax Definition Author's Manual

Syntax definitions are pure Fletch data. They declare how yew detects a
language, moves between contexts, and assigns semantic attributes. They do
not execute code, import modules, request capabilities, or name colors.

Run the strict checker before committing a definition:

```sh
yew syn check --strict runtime/syntax/example.fl
```

Then prove that the committed fixture corpus enters every reachable context
and fires every reachable rule:

```sh
yew syn check --coverage runtime/syntax/example.fl tests/syn/example/*
```

Coverage excludes include-only helper contexts, whose copied rules are
accounted for at each reachable inclusion site. An uncovered context or rule
is an error; the diagnostic names it so the author can add a fixture or delete
dead definition code.

The file must contain one map literal. Comments and trailing commas are
allowed. Unknown keys, unknown context names, and unknown attributes are
errors.

## Top-level map

| Key | Type | Required | Default | Example |
|---|---|---:|---|---|
| `syntax` | int | yes | none | `syntax: 1` |
| `language` | map | yes | none | `language: { name: "ini" }` |
| `contexts` | map of context maps | yes | none | `contexts: { main: { rules: [] } }` |
| `root` | string | no | `"main"` | `root: "prototype"` |
| `attrs` | map of string aliases | no | `{}` | `attrs: { kw: "keyword.control" }` |

`syntax` must be `1`. A different value is not interpreted as a nearby
version. The keys of `contexts` name contexts. Their insertion order affects
diagnostic order, but not matching semantics. The compiler expands local
`attrs` aliases before checking the closed attribute vocabulary.

## Language detection

The `language` map describes the language and its detection hints.

| Key | Type | Required | Default | Example |
|---|---|---:|---|---|
| `name` | string | yes | none | `name: "ini"` |
| `extensions` | list of strings | no | `[]` | `extensions: ["ini", "conf"]` |
| `filenames` | list of strings | no | `[]` | `filenames: [".editorconfig", "*.service"]` |
| `shebangs` | list of strings | no | `[]` | `shebangs: ["sh", "bash"]` |
| `first_line` | regex string | no | absent | `first_line: "^\\s*\\[.+\\]$"` |
| `priority` | int | no | `0` | `priority: 20` |
| `comment` | map | no | `{}` | `comment: { line: ";" }` |

Extension strings omit the leading dot and are compared case-insensitively.
A filename is either exact or a one-star glob. `name` is the unique id used
by the statusline and `yew syn list`; it also resolves deterministic ties.
`first_line` uses yew's non-backtracking regex syntax.

The optional `comment` map exposes comment delimiters to structural motion
and editor options. It does not create highlight rules.

| Key | Type | Required | Default | Example |
|---|---|---:|---|---|
| `line` | string | no | absent | `line: "//"` |
| `block` | two-string list | no | absent | `block: ["/*", "*/"]` |

Detection follows this order. The first matching class wins; ties within a
class use higher `priority`, then lexicographically smaller `name`.

1. A language explicitly assigned with `ed.syn.set` or `set { lang: ... }`.
2. An exact `filenames` match on the basename.
3. A glob `filenames` match on the basename.
4. An `extensions` match, longest compound extension first.
5. A `shebangs` match on line 1.
6. A `first_line` regex match.
7. No language (`YEW_LANG_NONE`), which disables highlighting quietly.

For a shebang, yew compares the interpreter basename. In
`#!/usr/bin/env python3`, it skips `env` and compares `python3`; in
`#!/bin/sh -e`, it compares `sh`.

## Rules

A context tries its rules in order at each scan position. The first matching
rule wins. Each rule is a map containing either `match` or `aux`, never both.
All other keys are optional.

| Key | Type | Default | Example |
|---|---|---|---|
| `match` | regex string | none; `match` or `aux` is required | `match: "\\bif\\b"` |
| `attr` | attribute string | context `default` | `attr: "keyword.control"` |
| `captures` | map int to attribute string | `{}` | `captures: { 1: "type" }` |
| `consume` | int capture index | `0` (whole match) | `consume: 1` |
| `push` | string or list of strings | absent (`stay`) | `push: ["outer", "inner"]` |
| `pop` | int 1–4 or bool | absent (`stay`) | `pop: 1` |
| `set` | context string | absent (`stay`) | `set: "main"` |
| `icase` | bool | context `icase` | `icase: true` |
| `first_line` | bool | `false` | `first_line: true` |
| `set_aux` | int capture index | absent (keep current aux) | `set_aux: 1` |
| `aux_int` | bool | `false` | `aux_int: true` |
| `aux_add` | int 0–255 | `0` | `aux_add: 2` |
| `aux_add_capture` | int capture index 1–7 | absent | `aux_add_capture: 2` |
| `strip` | bool | `false` | `strip: true` |
| `aux` | aux matcher string | none; `match` or `aux` is required | `aux: "line_eq"` |
| `aux_pre` | string | `""` | `aux_pre: "r"` |
| `aux_post` | string | `""` | `aux_post: "#"` |
| `value` | bool | absent (keep current flag) | `value: true` |
| `if_value` | bool | absent (match either state) | `if_value: false` |

Regex matches are anchored at the current scan position. A leading `^` can
therefore match only when the scan is at byte 0. Capture 0 means the whole
match; captures 1–7 are available. `consume: N` advances to the end of
capture N and leaves the rest of the match for later rules. This is the
supported substitute for positive lookahead.

`first_line: true` restricts the rule to the document's first physical line.
It is independent of the active context: multiline contexts opened on line 1
retain the fact that later lines are not first. Empty first lines count, and
incremental rescans preserve the same document-position state.

`push`, `pop`, and `set` are mutually exclusive. A list in `push` is pushed
left to right and may contain at most four contexts. `pop: true` means
`pop: 1`; an integer pop may not exceed four. If no state operation appears,
the rule stays in its current context.

`set_aux` normally interns the selected capture's text into the syntax state.
With `aux_int: true`, it instead stores the capture's expanded indentation
width as an integer; `aux_add` is added with saturation. When
`aux_add_capture` names a matched, single-digit capture, that digit replaces
the constant addition. An absent or empty capture keeps the `aux_add`
fallback. YAML block scalars use these fields for explicit and implicit body
indentation. With `strip: true`, `set_aux` also sets the strip flag used by
indented heredocs.

`value` sets or clears the state's single value bit. `if_value` makes a rule
eligible only while that bit is set or clear. The bit is cleared at every
physical EOL, so a definition never inherits token history from another line.
JavaScript uses the pair for its pinned regex-literal-versus-division
heuristic: identifiers, literals, and closing brackets set `value: true`;
operators, opening brackets, separators, and expression-leading keywords
clear it; `/` with `if_value: false` opens a regex, while `/` with
`if_value: true` is division. The two documented ambiguous cases are `)`
after a control-flow head and `}` after a block. Both failures are bounded to
one line because the regex context pops at EOL.

### Aux matchers

Aux matchers compare against the state's interned aux value. They keep
runtime-derived delimiters out of the regex compiler and render path.

| `aux` value | Match | Consumption | Example use |
|---|---|---|---|
| `line_eq` | whole line equals aux, ignoring leading tabs when `strip` is set | whole line | sh heredoc terminator |
| `literal` | `aux_pre + aux + aux_post` starts at the scan position | whole constructed literal | Rust raw strings; Python and TOML dynamic triple delimiters |
| `fence_close` | column ≤ 3; line has at least `len(aux)` copies of `aux[0]`, then nothing else | whole line | Markdown fence closer |
| `indent_lt` | byte 0; expanded indent is less than integer aux | zero bytes, then pop once | YAML block scalar end |
| `line_empty` | the physical line has zero bytes | zero bytes, then one state operation | end a backslash continuation across a blank line |
| `line_start` | byte 0 of any physical line | zero bytes, then one state operation | resume normal string or preprocessor rules after a continuation |

`indent_lt` must be the first rule in its context. `indent_lt`, `line_empty`,
and `line_start` are the only sanctioned zero-width matchers. Zero-width state
transitions are bounded by the engine's per-line transition limit;
`line_empty` runs only in the dedicated empty-line transition pass.

### Deferred entry for heredocs and similar blocks

Some constructs announce a multiline body on one line but do not enter that
body until the next line. A shell heredoc is the canonical example:
`cat <<EOF | tee out` keeps the pipe in ordinary shell syntax, while the next
line begins the string-like body. Do not push the body context directly.
Capture the delimiter into `aux`, push a pending context that includes the
ordinary rules, then use its `at_eol` action to replace it with the body:

```fletch
{ match: "<<(-?)\\s*([A-Za-z_][A-Za-z0-9_]*)",
  attr: "operator", captures: { 2: "string" },
  set_aux: 2, strip: true, push: "heredoc_pending" },

heredoc_pending: {
    include: "main",
    at_eol: "set:heredoc",
},
heredoc: {
    default: "string.special",
    at_eol: "stay",
    unit: "atom",
    rules: [ { aux: "line_eq", attr: "string", pop: 1 } ],
},
```

Use separate pending/body pairs when the opener selects different body
semantics, such as quoted shell delimiters suppressing expansion. With
`strip: true`, `line_eq` ignores leading tabs only; spaces remain significant
for `<<-`.

## Embedded languages

An opener rule can delegate a region to another installed syntax definition.
The opener must also name exactly one string `push` target: that target is the
host bridge context whose `end: true` rules decide when control returns from
the guest. `embed` is a state operation, so it may not be combined with
`pop`, `set`, or a multi-context `push`.

| Key | Type | Required | Default | Example |
|---|---|---:|---|---|
| `embed.lang` | string | yes | none | `lang: "javascript"`, `lang: "@2"`, or `lang: "@self"` |
| `embed.ctx` | string | no | guest `root` | `ctx: "jsx_tag"` |
| `embed.end` | `"line"`, `"inline"`, `"inline-root"`, or `"line-continuation"` | yes | none | `end: "line"` |
| `embed.defer` | bool | no | `false` | `defer: true` |
| `embed.fallback` | attribute string | no | bridge context `default` | `fallback: "code"` |
| `end` on a rule | bool | no | `false` | `end: true` |

`embed.lang` names a definition. `"@N"` selects the text of capture group
N from the opener, while `"@self"` re-enters the current definition at its
root with a fresh definition-level aux slot. `embed.ctx` enters a named guest
context instead of its root. `embed.end: "line"` tests the bridge's exit
rules once at byte 0; `"inline"` tests them before guest rules at every scan
position. `"inline-root"` also tests at every position, but only while the
guest is at its entry context: nested guest contexts must balance and return
to that root before the host delimiter can close the bridge. This lets a
tagged HTML or CSS template suspend guest text at `${`, parse a balanced
JavaScript expression, and resume the same guest afterward.

`"line-continuation"` has line-end lifetime. An odd run of trailing
backslashes retains the guest and all of its contexts for the next physical
line; an even run (including zero) ends the guest and applies the bridge's
line-end operation. Make recipes use this mode so a backslash-continued shell
command preserves shell strings, substitutions, and heredoc state across
recipe lines. With `defer: true`, the bridge is entered on the opener but the
guest begins at the next physical line. An unavailable, unknown, or
depth-refused guest uses `embed.fallback` until the bridge exits.

Markdown fences show the complete shape:

```fletch
{ match: "^ {0,3}(`{3,}|~{3,})[ \\t]*([A-Za-z0-9_+.-]*)",
  attr: "punct", captures: { 1: "punct", 2: "attribute" },
  set_aux: 1, push: "fenced",
  embed: { lang: "@2", defer: true, end: "line", fallback: "code" } },

fenced: {
    default: "code", at_eol: "stay", unit: "atom",
    rules: [ { aux: "fence_close", attr: "punct", pop: 1, end: true } ],
},
```

### How the bridge carries its end delimiter

The guest does not own or compile the host delimiter. The host bridge frame
remains directly below the guest and retains the host definition, bridge
context, and host aux value. Before a guest rule runs, yew tests the bridge
context's `end: true` rules using the host definition and host aux slot. A
markdown `fence_close` therefore reads the captured fence run exactly as a
shell `line_eq` reads a captured heredoc terminator: both are host text tested
while a guest is active. On a match, all guest frames are discarded and the
exit rule's host operation is applied; an unterminated guest string cannot
leak past the host boundary.

The combined stack limit is 16 frames across host and guests, and the limit
is four simultaneous definition levels including the root. A refused embed
uses its fallback styling and remains bounded by the host exit rules. Run
`yew syn check --embed` to print the installed definitions' combined-depth
table. Literal chains that exceed 16 are errors. Capture-selected guests are
open-ended, so the checker reports their worst installed guest and remaining
headroom. Static and capture-selected maxima are computed separately and both
are truncated to the four definition levels the runtime can enter. Repeated
`@self` entry is modeled through that fourth level and is subject to the same
static frame cap as every other literal target. An enterable dynamic chain
that exceeds the frame budget is a warning. Runtime refusal remains the guard
for capture-selected and user-installed definitions that were not statically
available to the checker.

> **Pitfall — the guest's fast-scan will skip straight over the exit
> delimiter.** While a guest with `embed.end: "inline"` or `"inline-root"`
> is eligible to exit, the scan mask must be the union of the guest context's
> first-byte set and the bridge exit rules' first-byte set. Otherwise
> JavaScript, for example, can skip `<` in `</script>` and color the rest of
> the document as JavaScript.

> **Pitfall — line-ended modes must be tested before `at_eol`, and before the
> guest's own line-start rules.** The fixed order is bridge exit rules, guest
> context rules, guest `at_eol`, continuation decision, then host `at_eol`;
> bridge frames do not run `at_eol` while a guest remains above them.

### Embedded-language scope in 1.0

The 1.0 bridge supports markdown fences, HTML script/style bodies, make
recipes delegated to shell, shell command substitutions, JavaScript and
TypeScript tagged templates, and JSX through HTML's tag context. HTML closes
`<script>` at `</script>` even inside a JavaScript string, matching the HTML
tokenizer; spell the literal as `<\/script>` when it must remain guest text.
Markdown indented code and fences with unknown info strings stay `code`.

Make recipe shell state follows odd trailing-backslash continuation across
physical lines. Tagged HTML/CSS templates suspend guest parsing for balanced
`${...}` JavaScript expressions and resume the guest after the matching `}`;
guest escape rules consume escaped delimiters before the host bridge tests
them. JSX delegates tag and attribute lexing to HTML, whose expression
children and attribute expressions bridge into JavaScript's shared balanced
expression context, then return to HTML. Balanced expressions and nested JSX
trees therefore retain the correct host/guest alternation for both JavaScript
and TypeScript hosts.

The following are explicit post-1.0 work: bidirectional host/guest
interleaving such as PHP, ERB, Jinja, Handlebars, and Vue SFC; open-ended
`lang=` or `type=` dispatch; guessing a fence language from its contents;
multiple line-ended guests on one physical line; per-region indentation,
folding, comment strings, or LSP routing; and raising either stack cap without
state-count measurements. Semantic LSP highlighting remains a post-1.0 buffer
overlay rather than a per-region bridge feature; the binding Sprint 46/47
contracts explicitly exclude semantic tokens from 1.0.

## Contexts

Each key in `contexts` names a context. A context controls rule order,
unmatched-byte styling, line-end state, case sensitivity, and B-mode units.

| Key | Type | Default | Example |
|---|---|---|---|
| `rules` | list of rule maps or include strings | `[]` | `rules: ["include:escapes", { match: "x" }]` |
| `default` | attribute string | `"text"` | `default: "string"` |
| `at_eol` | string | `"stay"` | `at_eol: "pop:2"` |
| `icase` | bool | `false` | `icase: true` |
| `unit` | `"span"` or `"atom"` | absent | `unit: "atom"` |
| `include` | string or list of strings | `[]` | `include: ["escapes", "interp"]` |

`at_eol` accepts `"stay"`, `"pop"`, `"pop:N"`, or `"set:ctx"`. Give
single-line strings and values an explicit pop policy so malformed input
cannot restyle later lines. Multiline constructs such as block comments and
heredocs normally keep `"stay"`.

`unit: "span"` exposes the whole context region to B mode while preserving
nested units. Use it for structural containers whose children remain useful
motion targets. `unit: "atom"` exposes the region but hides units inside it;
strings, comments, heredocs, and fenced code bodies should normally be atoms.
An absent `unit` makes the context invisible to B mode and is appropriate for
transient helpers such as a deferred-entry context. Inclusion copies rules
only, so a helper's `unit` never leaks into the including context.

A context-level `include` splices the named contexts' rules before its own
rules. An in-list `"include:NAME"` splices them at that exact position.
Inclusion is recursive, preserves order, and rejects cycles. It copies only
rules; an included context's `default`, `at_eol`, and `unit` have no effect
at the inclusion site. Keep include-only contexts to a `rules` key so the
strict checker does not warn.

```fletch
common_escapes: {
    rules: [ { match: "\\\\[ntr\"\\\\]", attr: "string.escape" } ],
},
dq_string: {
    default: "string",
    rules: [
        "include:common_escapes",
        { match: "\"", attr: "string", pop: 1 },
    ],
},
```

## Semantic attributes

Definitions emit meanings, not colors. The following 54 names are the
complete 1.0 vocabulary. Themes resolve fallbacks once when loaded.

| id | Name | Fallback | Meaning |
|---:|---|---|---|
| 0 | `text` | — | ordinary text |
| 1 | `keyword` | `text` | language keyword |
| 2 | `keyword.control` | `keyword` | control-flow keyword |
| 3 | `keyword.op` | `keyword` | word-shaped operator |
| 4 | `keyword.storage` | `keyword` | storage or visibility keyword |
| 5 | `type` | `text` | type name |
| 6 | `type.builtin` | `type` | built-in type |
| 7 | `constant` | `text` | named constant or enum member |
| 8 | `constant.builtin` | `constant` | built-in non-boolean constant |
| 9 | `number` | `constant` | numeric literal |
| 10 | `boolean` | `constant` | boolean literal |
| 11 | `character` | `string` | character or rune literal |
| 12 | `string` | `text` | string body and delimiters |
| 13 | `string.escape` | `string` | escape sequence |
| 14 | `string.interp` | `string` | interpolation delimiters |
| 15 | `string.special` | `string` | regex, raw string, or heredoc body |
| 16 | `comment` | `text` | comment |
| 17 | `comment.doc` | `comment` | documentation comment |
| 18 | `comment.todo` | `comment` | task or fix marker in a comment |
| 19 | `function` | `text` | function definition or call name |
| 20 | `function.builtin` | `function` | built-in function |
| 21 | `function.macro` | `function` | macro invocation |
| 22 | `method` | `function` | method name |
| 23 | `variable` | `text` | ordinary identifier |
| 24 | `variable.builtin` | `variable` | built-in or magic variable |
| 25 | `variable.param` | `variable` | parameter or lifetime |
| 26 | `variable.member` | `variable` | field or mapping key |
| 27 | `namespace` | `text` | module, package, or section name |
| 28 | `label` | `text` | label, target, or statement label |
| 29 | `attribute` | `text` | decorator, annotation, or attribute |
| 30 | `preproc` | `text` | preprocessor directive |
| 31 | `operator` | `text` | symbolic operator |
| 32 | `punct` | `text` | punctuation not otherwise classed |
| 33 | `punct.bracket` | `punct` | paired bracket |
| 34 | `punct.delim` | `punct` | comma, semicolon, or colon delimiter |
| 35 | `tag` | `text` | markup tag name |
| 36 | `tag.attr` | `tag` | markup attribute name |
| 37 | `heading` | `text` | document heading |
| 38 | `link` | `text` | URL or link target |
| 39 | `emphasis` | `text` | emphasized run |
| 40 | `strong` | `text` | strongly emphasized run |
| 41 | `code` | `text` | inline code or fenced body |
| 42 | `list` | `text` | list marker |
| 43 | `quote` | `text` | block quote |
| 44 | `diff.add` | `text` | added line |
| 45 | `diff.del` | `text` | removed line |
| 46 | `error` | `text` | text proven syntactically impossible |
| 47 | `warning` | `text` | legal but suspicious text |
| 48 | `whitespace.special` | `text` | semantically significant whitespace |
| 49 | `motion.unit` | `keyword` | Fletch terse-layer unit |
| 50 | `motion.arrow` | `operator` | Fletch terse-layer arrow |
| 51 | `motion.count` | `number` | Fletch terse-layer count |
| 52 | `motion.cmd` | `function` | Fletch terse-layer command word |
| 53 | `ui.invisible` | `text` | rendered whitespace glyph |

Use `error` only when the definition proves that input is impossible in the
language. A parser disagreement is not enough. Use `warning` for legal but
suspicious input.

## Starting point and bounded definitions

For a new language, start with
[`runtime/syntax/go.fl`](../../runtime/syntax/go.fl). Go is the intentionally
ordinary template: a small root rule set, bounded string/comment contexts,
captures plus `consume` for function names, explicit EOL policies, and no aux
or token-history state. Use the larger definitions only when the language has
a construct that actually requires their machinery.

YAML is deliberately an honest subset rather than a partial parser. It does
not carry plain multi-line scalars, complex keys, tag resolution, flow nesting
past four levels, cross-line flow collections, perfect `: ` disambiguation in
plain values, or YAML 1.1 boolean/sexagesimal quirks. Quoted and flow contexts
pop at EOL; block scalars terminate through `indent_lt`, so none of those
known inaccuracies can leak indefinitely.

## Complete INI definition

This is the shipped `runtime/syntax/ini.fl` byte for byte.

```fletch
# runtime/syntax/ini.fl — INI / .conf highlighter.
# Demonstrates: contexts, first-match-wins ordering, captures, consume,
# push/pop, at_eol, include, and unit marks for B mode.
{
    syntax: 1,

    language: {
        name:       "ini",
        extensions: ["ini", "cfg", "conf", "properties"],
        filenames:  [".editorconfig", "*.desktop", "*.service"],
        first_line: "^\\s*\\[[A-Za-z0-9_.-]+\\]\\s*$",
        priority:   0,
        comment:    { line: ";" },
    },

    contexts: {
        main: {
            default: "text",
            rules: [
                # 1. Whole-line comments.  `;` is canonical INI, `#` is the
                #    universal habit; both only count at line start.
                { match: "^\\s*[;#].*$", attr: "comment" },

                # 2. A section header.  The brackets are punctuation and the
                #    name is a namespace, so `captures` splits one match
                #    into three attr runs.
                { match: "^\\s*(\\[)([^\\]\\n]*)(\\])",
                  attr: "error",                 # unmatched bytes = a broken header
                  captures: { 1: "punct.bracket",
                              2: "namespace",
                              3: "punct.bracket" } },

                # 3. A key.  `consume: 1` stops the scan right after the key
                #    name, leaving `=` and the value to rules 4 and 5 — this
                #    is how you express "an identifier FOLLOWED BY `=`"
                #    without lookahead, which yew's regex engine does
                #    not have and never will (s20 §4).
                { match: "^\\s*([A-Za-z_][A-Za-z0-9_.-]*)\\s*[:=]",
                  captures: { 1: "variable.member" },
                  consume: 1 },

                # 4. The separator, now at the scan position.
                { match: "[:=]", attr: "operator", push: "value" },

                # 5. Anything else at line level is junk in a well-formed
                #    INI file.  Saying so is more useful than saying
                #    nothing, and `error` means "provably impossible", not
                #    "highlighter confused" (s39 §1).
                { match: "\\S+", attr: "error" },
            ],
        },

        # The right-hand side.  `at_eol: "pop"` is what makes values
        # line-scoped: no value can leak into the next line, ever.
        value: {
            default: "text",
            at_eol:  "pop",
            unit:    "span",
            rules: [
                "include:strings",
                { match: "[;#].*$", attr: "comment" },
                { match: "\\b(true|false|yes|no|on|off)\\b",
                  attr: "boolean", icase: true },
                { match: "[+-]?(0[xX][0-9a-fA-F]+|[0-9]+(\\.[0-9]+)?([eE][+-]?[0-9]+)?)",
                  attr: "number" },
                { match: "\\$\\{[A-Za-z_][A-Za-z0-9_]*\\}", attr: "variable" },
            ],
        },

        # Include-only: spliced into `value`'s rule list in place.
        strings: {
            rules: [
                { match: "\"", attr: "string", push: "dq" },
                { match: "'",  attr: "string", push: "sq" },
            ],
        },

        dq: {
            default: "string",
            at_eol:  "pop",          # an unterminated quote dies at EOL
            unit:    "atom",         # B mode selects the whole string
            rules: [
                { match: "\\\\.", attr: "string.escape" },
                { match: "\"",    attr: "string", pop: 1 },
            ],
        },

        sq: {
            default: "string",
            at_eol:  "pop",
            unit:    "atom",
            rules: [ { match: "'", attr: "string", pop: 1 } ],
        },
    },
}
```

### Rule-by-rule explanation

| Rule or field | Why it is there |
|---|---|
| `main` rule 1 | Whole-line comments come first so comment text cannot be mistaken for headers, keys, or junk. |
| `main` rule 2 | One regex recognizes a header; captures style the two brackets and section name separately. The `error` base attr exposes unmatched bytes. |
| `main` rule 3 | The key rule must precede the separator. `consume: 1` stops after the captured key so the separator remains available to rule 4. |
| `main` rule 4 | The separator is an operator and pushes `value`, giving the right-hand side its own rules, line-end policy, and B-mode span. |
| `main` rule 5 | Non-whitespace text that is neither a header nor a key is invalid at top level, so it receives `error`. |
| `value` include | `strings` is spliced first, so a quote opens a string before comment, boolean, number, or variable rules can claim its bytes. |
| `value` comment | A semicolon or hash after the separator styles the rest of the line as a comment. |
| `value` boolean | Case-insensitive matching accepts the conventional upper-, lower-, and mixed-case spellings. |
| `value` number | Decimal, hexadecimal, fractional, and exponent forms share one rule. |
| `value` variable | `${NAME}` is styled outside quotes; inside `dq` it is ordinary string text because that context owns the scan. |
| `value.at_eol` | `"pop"` guarantees that every value ends on its line. Without it, one value would style the rest of the file. |
| `value.unit` | `"span"` makes the right-hand side a structural unit while still allowing nested string atoms. |
| `strings` | This include-only context holds the two quote-opening rules without adding runtime stack states of its own. |
| `dq` escape | Escape matching comes before the closing quote and gives escaped bytes `string.escape`. |
| `dq` close | A double quote closes only the double-quoted context. `at_eol: "pop"` also bounds an unterminated string. |
| `sq` close | A single quote closes the single-quoted context; no escape rule is assumed for this INI dialect. |
| `dq` / `sq` unit | `"atom"` makes B-mode expansion select a complete string instead of an escape inside it. |

### Six-line trace

Offsets are zero-based, half-open byte ranges. `>` separates stack frames.
The stack column shows transitions while scanning and the line-end action.

| Input line | Context-stack trace | Emitted spans |
|---|---|---|
| `; demo` | `main` | `[0,6) comment` |
| `[server]` | `main` | `[0,1) punct.bracket`; `[1,7) namespace`; `[7,8) punct.bracket` |
| `enabled = YES ; on` | `main` through byte 8; `main>value` after `=`; EOL → `main` | `[0,7) variable.member`; `[7,8) text`; `[8,9) operator`; `[9,10) text`; `[10,13) boolean`; `[13,14) text`; `[14,18) comment` |
| `port=8080` | `main` through byte 4; `main>value` after `=`; EOL → `main` | `[0,4) variable.member`; `[4,5) operator`; `[5,9) number` |
| `home="${HOME}/yew"` | `main>value` after byte 4; `main>value>dq` after byte 5; quote pop at byte 18; EOL → `main` | `[0,4) variable.member`; `[4,5) operator`; `[5,6) string`; `[6,17) string`; `[17,18) string` |
| `bad stuff` | `main` | `[0,3) error`; `[3,4) text`; `[4,9) error` |

The trace demonstrates first-match ordering, capture styling, partial
consumption, push/pop transitions, include placement, case folding, default
attrs, and line-end cleanup.

## Porting TextMate and Sublime grammars

Port structure, not syntax. TextMate regular expressions may use features
that yew intentionally excludes, and scope names are not yew attributes.

| TextMate / Sublime concept | yew definition |
|---|---|
| `fileTypes` or `file_extensions` | `language.extensions` and `language.filenames` |
| `firstLineMatch` | `language.first_line` |
| repository entry or named context | a key in `contexts` |
| ordered `patterns` | ordered `rules` |
| `match` | `match` |
| `captures` | `captures` with group numbers 0–7 |
| `begin` | a rule that styles the opener and `push`es a context |
| `end` | a rule in the pushed context that styles the closer and `pop`s |
| `beginCaptures` / `endCaptures` | `captures` on the push / pop rules |
| `include` | context `include` or an in-list `"include:NAME"` |
| `name` on a match | `attr` |
| `contentName` or `meta_scope` | context `default` |
| `push`, `set`, `pop` in Sublime | the same state operations |
| `applyEndPatternLast` | place the pop rule after the rules that must win |

Map source scopes onto the 54 semantic attrs by meaning. Do not copy scope
names such as `meta.function.c`; use `function`, `type`, `punct.bracket`,
and the other closed names above.

Three common constructs require a deliberate rewrite:

- Lookahead and lookbehind do not port. Capture the bytes that prove the
  condition, then use `consume` to leave later bytes for the next rule.
- A backreferenced `end` pattern does not port as a regex. Capture the
  delimiter with `set_aux`, enter a context, and close it with `aux:
  "literal"` plus `aux_pre` and `aux_post` when needed.
- A TextMate `while` pattern does not port directly. Use `at_eol` for
  line-scoped constructs or a first-position `aux: "indent_lt"` rule for
  indentation-bounded constructs.

Yew's regex engine also excludes backreferences and all lookaround. Keep
rule order explicit: textual inclusion preserves the only precedence model,
whereas repository ordering in a source grammar may be implicit.

Definitions are intentionally lexical rather than partial compilers. For
example, C block comments do not nest and therefore need exactly one closer;
`#if 0` regions are not dimmed because determining whether preprocessor input
is live requires macro evaluation. A highlighter that guesses there can hide
live code, so leaving it lexically styled is the safer, documented limit.

## New-definition checklist

- Start with `syntax: 1`, a unique lowercase `language.name`, and a `main`
  context or an explicit `root`.
- Add exact filenames, one-star filename globs, extensions without dots,
  shebang interpreter basenames, and a first-line regex only where each is
  reliable. Choose a priority only to resolve a known tie.
- Declare line and block comment delimiters in `language.comment`, then add
  matching highlight rules in the appropriate contexts.
- Put specific rules before general rules. Review every catch-all for rules
  it could make unreachable.
- Give strings escape rules, explicit close rules, and an honest `at_eol`
  policy. Use `unit: "atom"` for strings and comments.
- Cover decimal and language-specific numeric forms without letting a broad
  number rule swallow identifiers.
- Separate control, storage, operator-like, and ordinary keywords when the
  language makes those distinctions.
- Add a deliberate `error` rule only for text the grammar proves invalid.
- Mark structural bodies `unit: "span"`; leave helper contexts unmarked.
- Keep include-only contexts to `rules` and verify recursive includes are
  acyclic.
- Exercise every context in a kitchen-sink fixture and account for all
  context transitions, captures, line-end actions, and aux matcher paths.
- Run `yew syn check --strict PATH` and resolve every warning.
- Run `yew syn dump PATH --tables` twice and compare the outputs.
- Add span goldens for valid, malformed, empty, and unterminated input.
- Confirm B mode still falls back cleanly when the language is unavailable
  or syntax state has not settled.

The unreachable-rule check is intentionally incomplete. It detects exact
duplicate patterns and earlier catch-all rules whose first-byte set covers a
later rule. It never claims full regex subsumption; authors must still
review ordering.
