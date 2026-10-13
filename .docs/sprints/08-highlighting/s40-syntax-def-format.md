# Sprint 40: Syntax Definitions — Fletch Data In, Packed Tables Out

## Prerequisites

- Sprint 39 — `SynDef`/`SynCtx`/`SynRule`/`SynState`, the interned state
  table, `yew_syn_line`, `SynBuf` and its splice, `yew_syn_settle`, the
  54-entry `SynAttr` table, `YEW_SYN_DEPTH_MAX = 16`, the `first[32]`
  first-byte bitmap the engine's fast path reads, and the four aux
  matchers the engine already dispatches (`line_eq`, `literal`,
  `fence_close`, `indent_lt`). This sprint fills those tables; it changes
  none of their shapes.
- Sprint 28 — `fletch-spec.md` **§12 pure-literal mode**: definitions are
  data, not programs. §13's grant table says pure-literal data *grants
  nothing and runs nothing*, which is why a definition dropped into a
  config directory needs no capability prompt.
- Sprint 29 — `fl_parse_literal(Arena*, DiagCtx*, Interner*, src, len,
  file_id)` returning an `FlNode` tree with an `FlSpan {file_id, line, col,
  len}` on **every** node, and the `path:line:col: error:` + caret
  rendering contract. Every diagnostic in §6 is emitted through that.
- Sprint 20 — `yew_re_compile` / `yew_re_match_at` / `YewReErr {off, msg}`,
  the `ReLit` literal-prefilter extraction (reused here to build first-byte
  sets), the closed syntax table, and the permanent non-goals: **no
  lookahead, no lookbehind, no backreferences.** §3's `consume` field
  exists precisely because lookahead does not.
- Sprint 16 — `UnitOps`, `UnitCtx` (whose `Buffer *buf` comment already
  reads *"lang slot (S40)"*), `BlockProvider`, `yew_block_register`,
  `yew_block_level`, the containment chain and its priority table where
  `syntax` sits at priority 40 with a NULL `enclosing`, and
  `yew_block_provider_syntax_install` hard-erroring until now.
- Sprint 8 — `FileMeta.realpath`, `yew_xdg_state_dir()`, the atomic-write
  discipline (temp in the same dir → `fsync` → `rename` → `fsync` dir).
  Assumption beyond its entry: a sibling `yew_xdg_cache_dir()` exists; if
  not, it lands here next to it, same shape.
- Sprint 18 — the `yew` subcommand/argument surface `yew syn` plugs into;
  Sprint 13 — the command registry for `ed.syn.*`.
- Binding: `00-decisions.md` (*"Definitions are declarative Fletch data
  compiled to tables"*, cold start ≤ 20 ms, invariants 3, 4, 7).

## Goals

Turn language definitions into a first-class, documented, validated data
format: the complete pure-literal Fletch schema (language block, contexts,
match rules with attr assignment and a push/pop/set/stay state op, capture
attrs, the four aux matchers, context inclusion for reuse, and the
per-context default and end-of-line rules), the compiler that packs a
parsed literal into Sprint 39's tables, `yew syn compile` with an XDG cache
keyed on mtime+size+content-hash, definition-author documentation with a
complete worked INI highlighter explained rule by rule, a validator whose
errors carry `file:line:col` into the `.fl` source, and — the campaign's
one hard cross-edge — **Sprint 16's B-mode syntax provider slot, filled**:
syntax context spans become structural units, nesting depth becomes
expand/shrink levels, and the heuristic providers keep running underneath
so B mode is unchanged in a plain `.txt`. Actual language definitions are
Sprints 41 and 42; this sprint ships exactly one (`runtime/syntax/ini.fl`,
the worked example) plus the author's manual.

## Deliverables

### 1. The schema — `.docs/syntax-def.md` (authoritative)

A definition file is one pure-literal Fletch map (spec §12: comments and
trailing commas allowed, no identifiers beyond bare map keys, no
expressions, no imports). Top level:

| Key | Type | Req | Meaning |
|---|---|---|---|
| `syntax` | int | **yes** | schema version; `1` for 1.0. Unknown → error, never a guess |
| `language` | map | **yes** | §2 |
| `contexts` | map | **yes** | name → context (§4); insertion order is significant only for diagnostics |
| `root` | string | no | starting context; default `"main"` |
| `attrs` | map | no | local aliases: `{ kw: "keyword.control" }`, expanded before validation |

### 2. The `language` block

| Key | Type | Meaning |
|---|---|---|
| `name` | string | unique id; the statusline shows it; `yew syn list` keys on it |
| `extensions` | list of string | without the dot, case-folded: `["c", "h"]` |
| `filenames` | list of string | exact names or one-star globs: `["Makefile", "*.mk", ".editorconfig"]` |
| `shebangs` | list of string | interpreter basenames matched against `#!` line 1: `["sh", "bash", "dash"]` |
| `first_line` | string | s20 regex applied to line 1 when nothing else matched |
| `priority` | int | tie-break, higher wins; default 0 |
| `comment` | map | `{ line: "//", block: ["/*", "*/"] }` — consumed by s16's scope heuristic and Sprint 36's `commentstring`, not by the highlighter |

Detection order, pinned (first hit wins; ties → higher `priority`, then
lexicographic `name` so the result is deterministic — invariant 5):

1. explicit `ed.syn.set LANG` / `set { lang: … }` on the buffer
2. exact `filenames` match on the basename
3. glob `filenames` match on the basename
4. `extensions` match (longest extension first: `.tar.gz`-style compounds
   win over `.gz`)
5. `shebangs` against line 1 when it begins `#!` — the basename after the
   last `/`, and after `env` when line 1 is `#!/usr/bin/env python3`
   (pitfall: the `env` form is the majority case in the field; a matcher
   that only reads the first word gets it wrong on most scripts)
6. `first_line` regexes, in `priority` then `name` order
7. `YEW_LANG_NONE` — highlighting off, no error, no message

### 3. Rules

A rule is a map. `match` **or** one aux matcher is required; everything
else is optional.

| Key | Type | Meaning |
|---|---|---|
| `match` | string | s20 pattern, **anchored at the scan position** (compiled and run through `yew_re_match_at`; a leading `^` therefore means "line start", which is only reachable at position 0) |
| `attr` | string | attr for matched bytes not covered by `captures`; default = the context's `default` |
| `captures` | map int→string | group index → attr; group 0 is the whole match; groups 1–7 supported (`SynRule.caps[8]`) |
| `consume` | int | advance only to the end of capture group N; default 0 = whole match. **The no-lookahead workaround** |
| `push` | string or list | push context(s), left to right |
| `pop` | int | pop N levels (1–4); `true` is sugar for 1 |
| `set` | string | replace the top of stack |
| `icase` | bool | compile this pattern case-insensitively |
| `set_aux` | int | intern capture group N's text into the state's `aux` |
| `strip` | bool | with `set_aux`: set `YEW_SYN_F_STRIP` (heredoc `<<-`) |
| `aux` | string | one of the four aux matchers below; replaces `match` |
| `aux_pre` / `aux_post` | string | literal affixes for the `literal` matcher |
| `value` | bool | set/clear the state's one-bit `YEW_SYN_F_VALUE` flag |

`push`, `pop`, and `set` are mutually exclusive; naming two is a
validation error. Absence of all three is `stay`.

**The four aux matchers** are a closed set — there is no user-supplied
pattern templating, because a pattern built at run time cannot be compiled
at load time and would put `yew_re_compile` inside the render path.

| `aux` | Fires when | Consumer |
|---|---|---|
| `line_eq` | the whole line — after leading tabs iff `YEW_SYN_F_STRIP` — equals `aux`; consumes the line | shell heredocs (s41) |
| `literal` | `aux_pre .. aux .. aux_post` appears at the scan position; consumes it | Rust `r#"…"#` (s42) |
| `fence_close` | at column ≤ 3, the line is ≥ `len(aux)` repetitions of `aux[0]` and nothing else | markdown fences (s41) |
| `indent_lt` | scan position 0 only, and the line's indent (tabs expanded) is < the integer in `aux`; **consumes nothing**, pops once | YAML block scalars (s42) |

`indent_lt` is the engine's one sanctioned zero-width rule; s39's
per-line pop counter bounds it, and the validator rejects it anywhere but
position 0 (`at_bol` is implied and cannot be turned off).

### 4. Contexts

| Key | Type | Meaning |
|---|---|---|
| `rules` | list of rule/include | ordered; **first match wins** (s39 §4) |
| `default` | string | attr for bytes no rule matched; default `"text"` |
| `at_eol` | string | `"stay"` (default), `"pop"`, `"pop:N"`, or `"set:ctx"` — applied after the last byte of the line |
| `icase` | bool | default for every rule in the context |
| `unit` | string | `"span"` or `"atom"` — the B-mode mapping, §5 |
| `include` | string or list | context-level form: the named contexts' rules are spliced **ahead of** `rules`, in the order listed. The in-list form `"include:NAME"` splices at that exact position and is the way to put an include *after* some of your own rules |

`at_eol: "pop"` is how single-line constructs terminate honestly. A string
context with `at_eol: "pop"` means an unterminated `"` colors to end of
line and stops — it does not swallow the rest of the file. A context that
*should* span lines (block comment, heredoc, triple-quoted string) leaves
`at_eol` at `"stay"`.

**Context inclusion** is compile-time rule splicing, not a runtime push:

```fletch
common_escapes: { rules: [ { match: "\\\\[ntr\"\\\\]", attr: "string.escape" } ] },
dq_string: { default: "string",
             rules: [ "include:common_escapes",
                      { match: "\"", attr: "string", pop: 1 } ] },
```

The compiler replaces `"include:NAME"` with `NAME`'s rule list **in place**,
preserving order, recursively, with cycle detection. Why textual splicing
rather than a stack push: rule order is the only semantics the format has
(first match wins), and a runtime push would put an included context's
rules *after* everything and silently change meaning. An included
context's `default`, `at_eol`, and `unit` are ignored at the inclusion
site — only its `rules` are spliced, and the validator warns if an
include-only context sets them.

### 5. The compiler — `src/syn/defs.c|h`

```c
typedef struct SynDefErr {          /* one diagnostic, already emitted   */
    FlSpan  sp;                     /* into the .fl source               */
    u32     pat_off;                /* byte offset into a pattern, or ~0 */
    const char *msg;
} SynDefErr;

SynDef *yew_syn_def_compile(Arena *a, DiagCtx *dc, const u8 *src, size_t n,
                            u32 file_id, u32 *n_err, u32 *n_warn);
SynDef *yew_syn_def_load   (Arena *a, DiagCtx *dc, const char *path);
u32     yew_syn_lang_for   (const char *path, const u8 *line1, u32 l1_len);
const SynDef *yew_syn_def_for(u32 lang);      /* compiles on first use   */
```

Pipeline, in order:

1. `fl_parse_literal` → `FlNode` tree (spec §12). Any parse error is
   already a caret diagnostic; the compiler stops.
2. **Schema walk.** Unknown keys are errors, not ignored (invariant 3's
   spirit: a typo'd `matchh:` must not silently produce a definition that
   highlights nothing).
3. **Attr resolution.** Every `attr`/`captures`/`default` string is looked
   up in the 54-entry table (after `attrs` alias expansion); unknown names
   are errors with a did-you-mean (edit distance ≤ 2).
4. **Include splicing** with cycle detection.
5. **Pattern compilation** through `yew_re_compile`. Flags: `icase` from
   the rule or context. Patterns are never `YEW_RE_LITERAL`-flagged
   automatically — an author writing a literal gets one anyway via the
   prefilter.
6. **First-byte sets.** For each rule, the 256-bit `first[32]` set is
   derived from the compiled program: an s20 `ReLit` of kind `BYTE`/`BMH`
   contributes its first byte (plus its case-folded twin under `icase`); a
   leading `RE_CLASS` contributes the class's byte set (its interval list
   projected onto UTF-8 lead bytes); a leading `RE_CHAR` contributes the
   lead byte of its UTF-8 encoding; anything else (anchors first,
   alternation with no common prefix) contributes **all 256 bits** — the
   conservative direction. A context's set is the union of its rules'.
   > **Pitfall — a first-byte set that is too small is a correctness bug,
   > not a slow path.** The engine skips bytes outside the set without
   > consulting the regex engine at all, so a missing bit silently deletes
   > matches. The compiler asserts the inclusion direction with a
   > brute-force self-check in debug builds: for each rule and each of the
   > 256 byte values, if `yew_re_match_at` succeeds on a synthetic buffer
   > starting with that byte, the bit must be set. It runs on every
   > definition in `make test`.
7. **Validation** (§7). Errors abort; warnings do not.
8. **Packing** into one arena block, pointer-free and position independent:

   ```
   [ SynDefHdr | ctxs[nctxs] | rules[nrules] | prog blob | class blob |
     string blob (names, aux literals) ]
   ```

   Every internal reference is a `u32` byte offset from the block start, so
   the block is memcpy-able and cache-serializable verbatim. `SynDef`'s
   pointer fields are fixed up once at load into the in-memory view.

**Laziness is a startup budget requirement.** Cold start → first paint is
≤ 20 ms (`00-decisions.md`), and compiling 13 definitions costs far more
than that. Therefore: `yew_syn_lang_for` reads only the *detection* tables
(name, extensions, filenames, shebangs, first_line, priority), which are
built into the binary as a generated C table (`src/syn/langs_gen.c`,
produced by `scripts/gen-langtab` from `runtime/syntax/*.fl` and checked
in, same discipline as the Unicode tables); the definition body itself is
compiled or cache-loaded on **first use of that language in this process**.
Opening a C file compiles exactly one definition.

### 6. `yew syn` and the XDG cache

```
yew syn list                    # name, extensions, source path, cache state
yew syn compile [--all | FILE]  # compile, validate, write cache
yew syn check FILE [--strict]   # validate only; --strict promotes warnings
yew syn dump FILE [--tables|--spans INPUT]   # golden-test surface
yew syn cache clear|path
```

Exit codes per the locked contract: 0 ok · 1 error · 2 `--batch`-style
failure · 3 I/O · 4 internal bug.

Cache layout: `$XDG_CACHE_HOME/yew/syn/<name>.stab` (default
`~/.cache/yew/syn/`). Header, little-endian, fixed 64 bytes:

| Field | Bytes | Purpose |
|---|---|---|
| magic `"SAGSYN\0\0"` | 8 | reject foreign files |
| `table_version` | 4 | `YEW_SYN_TABLE_VERSION`, bumped on any struct change |
| `abi_tag` | 4 | `sizeof(void*) << 8 \| endianness << 1 \| …` |
| `yew_version_hash` | 8 | FNV-1a of the `--version` string |
| `src_mtime` (sec, nsec) | 16 | fast path |
| `src_size` | 8 | fast path |
| `src_hash` | 8 | FNV-1a-64 of the definition bytes — the **authority** |
| `blob_len`, `blob_crc` | 8 | integrity |

Invalidation, in order — the two-tier check is deliberate:

1. Any of magic / `table_version` / `abi_tag` / `yew_version_hash`
   mismatched → recompile.
2. `src_mtime` **and** `src_size` match → trust the cache, load, done.
3. Otherwise read the source and hash it. `src_hash` matches → the file was
   touched but not changed: rewrite the header's mtime/size and load the
   cached blob without recompiling.
4. Else recompile and rewrite.

> **Pitfall — mtime alone is not an invalidation key.** `git checkout`,
> `cp -p`, `tar -x`, and coarse-granularity filesystems all produce a new
> file with an old timestamp or an old file with a new one. The hash is
> the authority; mtime+size is only the fast path that avoids a read.
> Conversely, hash-only means reading every definition at startup, which
> the 20 ms cold-start budget cannot afford — hence the two tiers.

Cache-write rules: atomic (temp + `fsync` + `rename` + dir `fsync`, s08's
discipline) and **never fatal**. A corrupt, truncated, or unreadable cache
entry is discarded and recompiled with one `yew_log(WARN)`; a write failure
(read-only `$HOME`, full disk) logs once and the process continues with the
in-memory tables. The cache is an optimization and is never load-bearing;
`yew --clean` and `YEW_NO_SYN_CACHE=1` bypass it entirely, and the
determinism lane runs with the cache both cold and warm to prove identical
output. Read is one `read()` into a buffer — **no `mmap`**, same reasoning
as s08: a cache file rewritten by a concurrent `yew syn compile` under an
mmap is a SIGBUS.

### 7. The B-mode syntax provider — Sprint 16's slot, filled

`src/syn/synblock.c`:

```c
/* s16 §8 promised this function hard-errors until now.  It now installs
 * the provider registered at priority 40 with a NULL `enclosing`.      */
void yew_block_provider_syntax_install(SynEngine *e);

static bool syn_enclosing(void *ctx, UnitCtx *u, ByteOff p, Span inner,
                          Span *out);      /* the s16 BlockProvider vtable */
bool yew_syn_in_string_or_comment(const Buffer *b, ByteOff off);
```

**Which contexts are units.** A context participates in B mode iff its
definition marks it:

| `unit` | Meaning | Examples |
|---|---|---|
| absent | invisible to B mode; its region is not a unit and its nesting level is skipped | `preproc_continuation`, `fstring_brace` |
| `"span"` | the context's whole region is a structural unit, and units *inside* it are also offered | C `brace_body`, Fletch `block`, markdown `fenced` |
| `"atom"` | the region is a unit and **nothing inside it is** — expansion from inside jumps straight to the whole region | any string, any comment, a heredoc body |

`"atom"` is the row that matters: expanding a selection from inside a
string should select the whole string, never the escape sequence under the
cursor. A definition that marks strings `"span"` produces B-mode levels
nobody wants.

**Computing the enclosing span.** Sprint 39 gives us, for free, the entry
state of every line — a context stack per line boundary. For position `p`
on line `L`:

1. Re-run `yew_syn_line` on line `L` up to `p`, recording the stack at each
   byte (the engine exposes `yew_syn_stack_at(e, entry, line, len, p,
   SynState *out)` for this — one line of work, microseconds).
2. Walk that stack from the top down, skipping contexts with no `unit`
   mark, to find the innermost unit context that strictly contains `inner`.
   **Stack depth is the level**: the context at stack depth `d` is B-mode
   level `depth − d`, so `A-↑` (expand) walks outward one stack frame at a
   time and `A-↓` (contract) walks back in. That mapping is the whole
   integration — s16's containment chain already replays a stack for
   contraction, so the levels line up with no new machinery.
3. Find the region's **start**: scan backwards over `entry[]` (4 bytes per
   line, a `u32` scan) to the last line whose entry-state stack is shorter
   than `d` or whose frame at depth `d` differs; re-run that line to get
   the exact byte. Find the **end** symmetrically, forward.
4. Return `false` (chain falls through) when any of: the buffer has no
   language; `settled_to` does not cover `[start_line, end_line]` — an
   unsettled region must never produce a *wrong* unit; the scan exceeds
   `YEW_SYN_UNIT_SCAN_LINES = 100000` lines in either direction; the
   innermost unit context is not strictly larger than `inner`.

`YEW_SYN_UNIT_SCAN_LINES` is 50× s16's `YEW_BLOCK_SCAN_LINES` because this
scan reads a `u32` array, not text: 100 000 lines is 400 KiB of sequential
`u32` compares, well under the 5 ms keypress gate, whereas s16's heuristic
scan reads bytes through the piece tree.

**The heuristic fallback still runs — confirmed and re-gated.** Providers
2–4 (`scope`, `indent`, `paragraph`) stay registered exactly as s16 landed
them, and `yew_block_level` still takes the *smallest* containing span with
ties to higher priority. Consequences, all asserted in this sprint's tests:

- A plain `.txt` behaves identically to Sprint 16 — the syntax provider
  returns `false` immediately. **s16's DoD 4 (≥ 3 containment levels with
  no language) is re-run in this sprint's CI unchanged.**
- An unsupported language behaves identically. Adding a definition in
  Sprint 41/42 can only make B mode *finer*, never coarser: the syntax
  provider wins only when its span is smaller.
- The one behavioral change to an existing provider: `scope`'s line-local
  quote/comment heuristic (s16 §5) now calls
  `yew_syn_in_string_or_comment()` when a definition is loaded and the
  region is settled, falling back to the heuristic otherwise. This is the
  exactness s16 §5 promised ("Sprint 40's syntax provider makes it
  exact"): `x = "a[b"` and `/* ) */` stop confusing bracket matching.
  s16's heuristic unit tests keep running with no definition attached.

### 8. The worked example — `runtime/syntax/ini.fl`

The author's manual (`.docs/syntax-def.md`) carries this file in full, then
explains every rule. It is also the compiler's own golden.

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

Rule-by-rule commentary in the manual (abridged here, complete there):

| # | Why it is where it is |
|---|---|
| 1 | Comments come first so a `;` before a `[` cannot be read as a header. First-match-wins makes ordering the whole specification |
| 2 | `attr: "error"` catches the bytes `captures` does not — for a well-formed header there are none, so `error` only appears on `[unterminated` |
| 3 | `consume: 1` is the format's answer to lookahead. Without it the rule would eat the `=` and rule 4 could never fire |
| 4 | Pushing `value` rather than styling inline is what gives the RHS its own default, its own `at_eol`, and a B-mode unit |
| 5 | A deliberate `error` rule. Definitions that never say `error` are definitions that never help you find a typo |
| `value.at_eol` | `"pop"` — the single most important field in the file. Without it, one unterminated construct restyles the rest of the document |
| `strings` | Include-only context: no `default`, no `at_eol` — the validator warns if it grows them |
| `dq` / `sq` | `unit: "atom"` so `A-↑` inside a string selects the string, not the escape |

The manual additionally carries: a trace table (input line → per-byte
context stack → emitted spans) for a 6-line INI sample; the attr table from
s39 §1 reproduced as the author's vocabulary; a "porting a TextMate/Sublime
grammar" section mapping `begin`/`end`/`patterns`/`include` onto
`push`/`pop`/`rules`/`include:` and naming the three constructs that do not
port (lookahead, backreferenced `end` patterns — use `set_aux` + the
`literal` matcher, and `while` patterns — use `at_eol`/`indent_lt`); and a
checklist for a new definition (detection, comments, strings with escapes
and an `at_eol` policy, numbers, keywords, an `error` rule, `unit` marks,
`yew syn check --strict` clean, a kitchen-sink fixture with 100 % context
coverage).

### 9. Validation and diagnostics

Every diagnostic renders through s29's `DiagCtx` at the `FlSpan` of the
offending literal node: `runtime/syntax/ini.fl:23:19: error: …` plus the
source line and caret.

| # | Condition | Severity | Message |
|---|---|---|---|
| 1 | missing/unknown `syntax:` version | error | `unknown schema version 2 (this build understands 1)` |
| 2 | unknown top-level or context or rule key | error | `unknown key 'matchh' (did you mean 'match'?)` |
| 3 | unknown attr name | error | `unknown attr 'strng' (did you mean 'string'?)` — did-you-mean over the 54-name table, edit distance ≤ 2 |
| 4 | unknown context in `push`/`set`/`include`/`root` | error | `no context named 'strings2'` |
| 5 | two state ops on one rule | error | `rule has both 'push' and 'pop'; a rule performs exactly one state op` |
| 6 | `consume: N` where the pattern has no group N | error | `consume: 3 but the pattern has 2 capture groups` |
| 7 | `captures` key out of 0–7 | error | `capture group 9 is out of range (0–7)` |
| 8 | pattern fails `yew_re_compile` | error | caret on the string literal + `invalid pattern at offset 7: unterminated character class` |
| 9 | `pop: N` with N > 4, or `push` list longer than 4 | error | names the limit |
| 10 | static acyclic push depth > 12 | error | `context nesting can reach depth 14; the cap is 16 with 4 levels reserved for runtime recursion (see YEW_SYN_DEPTH_MAX)` |
| 11 | include cycle | error | `include cycle: strings → escapes → strings` |
| 12 | **a context that can never be popped** | error | see below |
| 13 | **unreachable rule** | warning | see below |
| 14 | context never pushed/set and not `root` | warning | `context 'legacy' is unreachable` |
| 15 | `error` as a context `default` | warning | `default: "error" paints every unmatched byte red; did you mean "text"?` |
| 16 | include-only context sets `default`/`at_eol`/`unit` | warning | `only 'rules' is used at an include site` |
| 17 | `indent_lt` on a rule that is not first in its context | error | it is a zero-width pop and must be tried before any consuming rule |

**#8's caret placement, honestly.** `YewReErr.off` is an offset into the
*compiled pattern*, and the `.fl` source is the *escaped* form — `"\\d"` is
four source bytes for two pattern bytes — so mapping the offset back
through the escape decoding would need the lexer's byte map, which s29 does
not retain. We do the honest thing: the caret points at the whole string
literal, and the pattern-relative offset is stated in the message text
(`invalid pattern at offset 7: …`). Precise sub-string carets are a
post-1.0 nicety, not a reason to invent a parallel lexer.

**#12 — the state that can never be popped.** Build the context graph:
node per context, an edge `C → parent` for every rule in `C` with
`pop`/`set` that reduces the stack, plus an edge for `at_eol: pop|set`.
A context reachable from `root` with **no outgoing reducing edge at all**
is an error, because once entered, every subsequent line of the file is
styled inside it forever — the exact failure this campaign exists to
prevent. Note what the check deliberately does *not* fire on: a C block
comment context has a `*/` pop rule, so an unterminated `/*` in a real file
is legitimate, bounded by s39's propagation, and not a definition bug.

**#13 — the unreachable rule check is a deliberate approximation.** Full
subsumption between two regexes is not something we are going to implement,
so the check is defined to have **zero false positives** and admitted
incompleteness. It fires only when, for a rule `R` at index `i`, some
earlier rule `E` at index `j < i` in the same context satisfies either:

- (a) `E.match` is byte-identical to `R.match` (copy-paste duplicate), or
- (b) `E`'s first-byte set is a superset of `R`'s first-byte set **and**
  `E`'s compiled program is a single unbounded class-or-any repetition with
  minimum length ≤ 1 (`.`, `.*`, `\S+`, `[^\n]+`) — i.e. `E` swallows
  everything `R` could start on.

The message names both rules by index and line:
`rule 7 is unreachable: rule 3 (line 41) matches everything it could
match`. The manual states the incompleteness outright: "this check catches
the two mistakes people actually make; it cannot catch every unreachable
rule, and it never reports a reachable one."

`yew syn check FILE` exits 1 if any error fired, 0 otherwise (warnings
print either way); `--strict` promotes warnings to errors. CI runs
`yew syn check --strict runtime/syntax/*.fl`.

### 10. Defer

- Actual language definitions (C, Fletch, sh, make, markdown) and the
  theme format → **Sprint 41**; (python, rust, go, js/ts, fortran, json,
  yaml, toml) → **Sprint 42**. `runtime/syntax/` contains only `ini.fl`
  this sprint.
- Cross-definition dispatch (a markdown fence highlighting embedded js) →
  not in 1.0's core; `SynState.def` stays 0 and the compiler rejects any
  future `embed:` key naming the extension sprint.
- Themes, colors, `NO_COLOR` → **Sprint 41**; `src/syn/theme.c` still does
  not exist and `ed.theme.set` hard-errors naming it.
- Plugin-supplied definitions: already work, by construction — a
  pure-literal file grants nothing (spec §12/§13), so a `.fl` in
  `$XDG_CONFIG_HOME/yew/syntax/` is discovered and compiled with no
  capability prompt. Plugin *packaging* of definitions is **Sprint 55**.
- LSP semantic tokens layered over these attrs → **post-1.0**; the later
  binding Sprint 46/47 contracts explicitly exclude them from 1.0.
- Indentation rules, folding ranges, and `commentstring` *use* (the
  `language.comment` block is parsed and exposed but only s16's scope
  heuristic reads it) → **Sprint 36** options / post-1.0 folding.

## Testing Strategy

- **Unit** (`tests/unit/test_syn_defs.c`): every schema key parsed and
  packed; every §9 row fires with the exact message and a correct
  `file:line:col` (structural assertion through a capturing diag sink, not
  stderr scraping); include splicing preserves order; cycle detection;
  the depth-12 static check; `consume`/`captures` bounds.
- **Unit** (`tests/unit/test_syn_firstbyte.c`): the brute-force
  first-byte self-check over all 256 bytes × every rule of `ini.fl` and of
  s39's toy definition — the inclusion direction must hold; a deliberately
  narrowed set makes the test fail.
- **Unit** (`tests/unit/test_syn_detect.c`): the 7-step detection order,
  including `#!/usr/bin/env python3`, `#!/bin/sh -e`, `Makefile` vs
  `foo.mk`, extension ties broken by `priority` then `name`, and
  `YEW_LANG_NONE` for `.xyz`.
- **Unit** (`tests/unit/test_syn_cache.c`): the four-step invalidation
  ladder with a fake clock and a temp XDG root — touched-but-unchanged
  file reloads without recompiling (compile counter asserted); truncated,
  magic-wrong, version-wrong, and CRC-wrong caches each recompile with a
  warning and never fail; a read-only cache dir logs once and works.
- **Unit** (`tests/unit/test_syn_block.c`): the B-mode mapping — level
  ordering on a nested INI fixture; `atom` contexts stopping expansion
  inside a string; `false` returned when unsettled, when scanning past
  100 000 lines, and when the language is `YEW_LANG_NONE`;
  `yew_syn_in_string_or_comment` on `x = "a[b"`.
- **Regression — s16 unchanged**: s16's `test_block.c` and its DoD 4 prose
  fixture run **in this sprint's CI** with the syntax provider installed
  and a `.txt` buffer, asserting byte-identical results to Sprint 16.
- **Script tests** (`tests/script/syn_*.fl`): `yew syn check` exit codes;
  `yew syn compile --all` idempotence; `yew syn dump --tables` determinism.
- **Golden** (`tests/syn/ini/*.ini` + `.spans`): the worked example over a
  kitchen-sink fixture; the dump prints **context names, never numeric
  ids**, so adding a rule cannot churn every golden.
- **Fuzz** (`tests/fuzz/fuzz_syn_def.c`): random bytes and
  random-but-plausible definition literals → `yew_syn_def_compile` never
  crashes, never loops, never produces a `SynDef` that fails s39's
  invariants, error count bounded, arena high-water ≤ 64 MiB. Then feed
  every surviving definition to s39's line fuzzer.
- **Perf**: `yew_syn_lang_for` over 10 000 paths ≤ 5 ms total (it is on the
  file-open path); compiling `ini.fl` cold ≤ 3 ms; cache load ≤ 200 µs;
  cold start with a warm cache unchanged from Sprint 38's baseline within
  noise (the 20 ms gate must not move).
- Lanes: gcc/clang zero-warning, ASan/UBSan, valgrind, determinism (cold
  cache vs warm cache produce byte-identical `--tables` dumps).

## Definition of Done

1. `make && make test` green on both compilers, zero warnings;
   ASan/UBSan/valgrind/determinism lanes green.
2. `.docs/syntax-def.md` documents every key in §1–§4 with a type, a
   default, and an example; contains `ini.fl` verbatim plus the rule-by-rule
   table, the 6-line trace table, the TextMate porting section, and the
   new-definition checklist. `grep -n 'TODO\|TBD'` → empty.
3. `yew syn check --strict runtime/syntax/ini.fl` exits 0 with zero
   warnings; the same command over 17 deliberately broken fixtures
   (`tests/syn/bad/*.fl`, one per §9 row) produces exactly the tabled
   message with the correct `line:col`, one diagnostic each.
4. First-byte self-check green for every rule of every shipped definition
   over all 256 byte values; narrowing any set by one bit fails the test.
5. B-mode: `yew_block_provider_syntax_install` no longer hard-errors; the
   provider returns spans on an INI fixture giving ≥ 4 hand-computed
   containment levels, and `false` in all four fallback conditions.
6. **s16 is not regressed**: its `test_block.c` suite and DoD 4's prose
   fixture pass unchanged with the provider installed; a `.txt` buffer
   still yields ≥ 3 levels.
7. `yew_syn_in_string_or_comment` makes s16's three heuristic cases exact
   (`don't`, `x = 'a[b'`, `/* ) */`) with a definition loaded, and the
   heuristic still handles them without one.
8. Cache: the four-step ladder unit-tested; a touched-but-unchanged file
   does **not** recompile (counter == 0); every corruption class
   recompiles rather than failing; `YEW_NO_SYN_CACHE=1` and `--clean`
   bypass it; cold-vs-warm `--tables` dumps are byte-identical.
9. Laziness: opening one `.ini` file compiles exactly **one** definition
   (counter asserted); `scripts/gen-langtab` output regenerates
   byte-identically in CI (same discipline as the Unicode tables).
10. Deferrals hard-error naming their sprint: `ed.theme.set` (41), an
    `embed:` key (its own sprint), a definition naming a language shipped
    in 41/42 that does not exist yet (`no definition for 'rust' — Sprint
    42`).
11. Fuzz: 10 minutes on the definition fuzzer, zero findings, ASan/UBSan
    clean; corpus committed and replayed in `make test`.
12. Perf: `yew_syn_lang_for` ≤ 0.5 µs per path; `ini.fl` cold compile
    ≤ 3 ms; cache load ≤ 200 µs; the cold-start baseline is unchanged
    within 5 %.
13. ≥ 200 assertions across the five new unit files; ≥ 6 golden span
    files for `ini.fl`, all byte-stable across two runs.
