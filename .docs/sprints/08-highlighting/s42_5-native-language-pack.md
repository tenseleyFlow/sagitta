# Sprint 42.5: Native Language Pack — Wolf and 48 Built-in Modes

## Prerequisites

- Sprint 39 — the incremental line-state engine, bounded context stack,
  interned line states, damage-driven settling, differential oracle, and
  latency budgets.
- Sprint 40 — pure Fletch syntax data, deterministic detection, lazy
  compilation, generated `src/syn/langs_gen.c`, XDG cache, strict checker,
  coverage accounting, and author documentation.
- Sprints 41 and 42 — the original 17 definition files / 14 language families,
  both themes, golden corpus, language-pack fuzzing, and cumulative syntax
  performance gates.
- Sprint 41.5 — `html.fl` and `css.fl`, cross-definition embedding, qualified
  span dumps, 19 installed definition modes total, and the distinction
  between global installed definitions and the per-buffer limits
  `YEW_SYN_DEF_MAX == 4` / `YEW_SYN_DEPTH_MAX == 16`.
- Sprint 42's implementation is complete, but its **daily-driver milestone
  remains `PENDING`** until `.docs/dogfood-log.md` satisfies the field gate.
  Work in this sprint may supply that evidence; automated tests do not.
- Wolf lexical source of truth: sibling repository
  `~/GithubOrgs/tenseleyFlow/wolf-lang` at commit
  `ab23a23f2b190d6f9b45271149b5ac7032b8133e`, especially
  `spec/01-grammar.md`, `spec/grammar.ebnf`, and `crates/wolf_lex/`.
- Binding: `00-decisions.md` (bespoke first, deterministic, lazy, speed
  gates, no dependencies, invariants 2–5 and 7) and
  `.docs/syntax-def.md` (pure-data definition contract).

**Campaign pause.** Campaign 09 is deliberately paused. No Sprint 43–47
implementation begins until every Definition of Done item below is green.
This is a sequencing decision, not a deletion: Sprint 43 remains the next
implementation contract after this sprint.

## Goals

Make yew useful on a broad polyglot tree before beginning completion or LSP
work. "Native" here means a syntax definition ships with yew, is selected by
the core detector, compiles through yew's own Fletch-data compiler, works with
`--clean`, needs no plugin and no external process, and is covered by the same
strict/golden/fuzz/performance gates as the original pack. It does **not** mean
semantic analysis: symbol knowledge remains Sprint 44 and LSP remains Sprints
45–47.

The sprint has three inseparable outcomes: remove the accidental 32-entry
registry/cache ceilings and index built-in detection; replace duplicated
shell-script language matrices with one checked manifest; and ship 29
additional modes, led by the maintainer's Wolf language, for exactly **48
built-in syntax modes**. Built-in definition bodies remain external runtime
data and lazy. The larger pack must not enlarge `SynState`, raise an embed cap,
slow ordinary editing, or add a dependency.

## Deliverables

### 1. Remove the global 32-entry registry/cache ceilings — `src/syn/defs.c`,
`src/syn/registry.c|h`

The current registry has two unrelated process-lifetime arrays:

```c
static SynLangDesc desc[32];
static BuiltinDef builtins[32];
```

Both are replaced by one exact-sized, process-lifetime registry initialized
from `yew_syn_builtin_langs_len`:

```c
typedef struct BuiltinDef {
    Arena arena;
    DiagCtx dc;
    SynDef *def;
    bool tried;
} BuiltinDef;

typedef struct BuiltinRegistry {
    Arena first_line_arena;
    SynLangDesc *desc;
    BuiltinDef *loaded;
    YewRe **first_line_re;
    size_t len;
    bool ready;
} BuiltinRegistry;

void yew_syn_builtin_registry_build(BuiltinRegistry *out,
                                    const SynLangSeed *seed, size_t len);
void yew_syn_builtin_registry_free(BuiltinRegistry *registry);
static void builtin_registry_init(void); /* production owner in defs.c */
static const SynLangDesc *builtin_desc_at(size_t i);
```

`yew_syn_builtin_registry_build()` lives in the small internal registry module
and is the injectable unit-test seam: it overflow-checks every allocation,
allocates exactly `len` descriptor, loaded-definition, and first-line-regex
slots with `yew_xcalloc`, and materializes descriptor views without compiling
definition bodies. `builtin_registry_init()` calls it once with
`yew_syn_builtin_langs` / `yew_syn_builtin_langs_len` and leaves production
descriptor addresses stable for the process lifetime. The free function lets
unit fixtures release injected registries, loaded definitions, and the
`first_line_arena`; production initializes that arena once and populates its
exact-sized pointer table lazily. The
production registry is intentionally process-lifetime, as the current static
arenas are, and remains reachable rather than definitely lost under Valgrind.
Discovery reset never frees it. Allocation failure follows the existing fatal
OOM contract. Initialization is idempotent and single-threaded. No arbitrary
replacement maximum is introduced.

This also removes the third hidden ceiling in `detection_re()`: its
`slots[32]` cache becomes `BuiltinRegistry.first_line_re`, keyed by the
descriptor's registry ordinal. User-definition regexes continue to be owned
by their `DefMeta`. A test registry containing more than 32 distinct
`first_line` expressions must compile and match its last expression; cache
exhaustion may never silently turn a declared detector into "no match."

The public surface stays source-compatible:

```c
const SynLangDesc *yew_syn_lang_desc(u32 lang);
u32 yew_syn_lang_named(const char *name);
u32 yew_syn_lang_by_name(const u8 *name, u32 len);
u32 yew_syn_lang_count(void);
const SynDef *yew_syn_def_for(u32 lang);
```

Language ids remain deterministic and 1-based, but filename sort order stops
being their source: inserting `cpp.fl` into an alphabetical glob would
otherwise renumber nearly every existing mode. `scripts/gen-langtab` reads
the checked id ledger in §2, errors on a definition missing from it, and emits
rows in numeric-id order. Adding a definition appends an id; an existing id
never changes, and removing one requires a separate compatibility contract
rather than implicit reuse. User-discovered ids remain disjoint and collision
rules remain unchanged.

> **Do not confuse three different limits.** The removed `32` values cap
> process-global built-ins and their first-line regex cache.
> `YEW_SYN_DEF_MAX == 4` is the number of simultaneously active definition
> levels represented in one line's `SynState`; `YEW_SYN_DEPTH_MAX == 16` is
> the context-frame cap. The engine may keep up to
> `YEW_SYN_RESIDENT_MAX == 255` definitions loaded for one buffer, which is
> why a document may visit all 29 new fence languages without nesting more
> than four at one source position. All three public limits stay unchanged;
> `sizeof(SynState) == 84` and the 4-byte-per-line state array stay unchanged.

### 2. One checked syntax-pack manifest — `tests/syn/manifest.tsv`

The definition list is currently duplicated across
`scripts/check-syn-assets.sh`, `scripts/gen-syn-goldens.sh`, and
`scripts/gen-s42-fuzz-corpus.sh`, including a literal `19` count. Replace it
with a committed seven-column, ASCII-tab-delimited manifest. In the example
below `⇥` renders one byte `0x09`; the file itself contains tabs, not the
arrow glyph:

```text
# definition⇥language⇥source-patterns⇥golden-rule⇥scope⇥cohort⇥min-goldens
c⇥c⇥tests/syn/c/*.c⇥replace-extension⇥base⇥s41⇥18
markdown⇥markdown⇥tests/syn/embed/markdown/*.md⇥replace-extension⇥embed⇥s41_5⇥4
wolf⇥wolf⇥tests/syn/wolf/*.lu⇥replace-extension⇥base⇥s42_5⇥12
```

Rules:

1. `definition` is the runtime filename stem, not a display label
   (`fortran_fixed`, `objective_c`).
2. `language` is the canonical `language.name` (`fortran-fixed`,
   `objective-c`). It is repeated on embed rows and must match the compiled
   definition.
3. `source-patterns` is a comma-separated list of repository-relative exact
   paths or single-`*` shell patterns, with no whitespace. It can therefore
   express `Dockerfile`, `Dockerfile.*`, `CMakeLists.txt`, and ordinary
   extension fixtures without inventing fake suffixes.
4. `golden-rule` is the closed set `replace-extension|append-spans` and tells
   both generators whether `x.ext` maps to `x.spans` or `x.ext.spans`.
5. `scope` is the closed set `base|embed`; a definition has exactly one
   `base` row and may have additional `embed` rows.
6. `cohort` is the closed set `s41|s42|s41_5|s42_5`. Asset checks derive
   historical and new-pack totals from it instead of path heuristics.
7. `min-goldens` is the minimum number of distinct matched source/golden pairs
   for that row. It is 12 for Wolf, at least 4 for each other new base row,
   and preserves the existing committed minimum for every old/base/embed row.
8. Rows are sorted by definition, then `base` before `embed`, then the bytewise
   `source-patterns` field. Each pattern must match at least one source; the
   union within a row must not select a `.spans` file or duplicate a source.
9. Consumers set `LC_ALL=C`, split only on tab and comma, reject pattern
   metacharacters other than the single `*`, and never use `eval`. For
   `replace-extension`, the final non-leading `.` suffix is removed; a basename
   without one must use `append-spans`.

Every `runtime/syntax/*.fl` must have exactly one base row; every base row
must name an existing definition and contain at least one source fixture and
one `.spans` golden. HTML and CSS gain ordinary base fixture directories so
they no longer exist only as embedded test guests. A script failure names the
missing or duplicate row.

Update these consumers:

- `scripts/check-syn-assets.sh` derives the built-in count, strict checks,
  coverage inputs, golden checks, determinism checks, and per-cohort totals
  from the manifest. It contains no expected definition count and no language
  name list.
- `scripts/gen-syn-goldens.sh` reads the same rows and preserves its
  `all|embed` scope option.
- Replace `scripts/gen-s42-fuzz-corpus.sh` with
  `scripts/gen-syn-fuzz-corpus.sh`; update `Makefile` and generate one
  compiler-fuzzer seed per unique base row. The obsolete sprint-specific
  script is deleted.
- `scripts/gen-langtab` parses `runtime/syntax/*.fl` for metadata and
  `tests/syn/builtin-ids.txt` for ids, emits rows in id order, and errors on a
  missing, duplicate, renamed, or orphaned ledger entry. A manifest check
  proves runtime stems equal manifest `definition` values and generated names
  equal manifest `language` values.

`tests/syn/builtin-ids.txt` is the authoritative
`id|canonical-language|definition-stem` ledger. It freezes the 19 mappings
generated at baseline commit `4058b25` (`1|c|c` through `19|yaml|yaml`), assigns
`20|wolf|wolf`, then assigns 21–48 to §4 in table order. The stem column makes
shared-body names such as `fortran_fixed|fortran-fixed` and
`objective_c|objective-c` unambiguous. `scripts/check-syn-assets.sh` diffs
`yew --clean syn list` against the full ledger by name and ordinal. The file
makes an accidental id renumbering a review-visible failure instead of a
workspace-state or cache surprise.

### 3. Wolf is a first-class built-in — `runtime/syntax/wolf.fl`

Wolf is the acceptance language, not merely one row in the pack. Its
definition declares `language.name: "wolf"`, extension `lu`, no filename or
shebang detector, line comment `//`, and the lexical contract frozen in
`tests/syn/wolf/lexical-contract.txt`. The contract cites the Wolf commit in
Prerequisites and serializes the 50 keywords in grammar order as lowercase
ASCII, one keyword per line, with LF after every line including the last.
Its SHA-256 is
`ca543483de3e53fa455e51c24dcfb59fca5141435a59f680d2710ba051326359`.

Required coverage:

| Wolf construct | Required highlighting behavior |
|---|---|
| `//`, `//!`, `///` | `comment`, `comment.doc`, `comment.doc`; `////` is an ordinary line comment; no block-comment context exists |
| 50 reserved keywords | exact closed set from `reserved_kw`; flow/control, storage, type, concurrency, and unsafe families use the closest existing semantic attrs |
| contextual words | `c`, `rc`, `pool`, `from`, `timeout`, `noalias`, `pkg`, `self`, `out`, `inout`, `lateout`, and `reg` style specially only in their bounded contexts; elsewhere they remain identifiers (`in` remains a reserved word) |
| numbers | decimal, float with digits on both sides of `.`, exponent, `0x`, `0o`, `0b`, and `_`; `1.`, `1..10`, and `1.e5` follow Wolf's pinned token split |
| ordinary strings | every `"..."` is interpolated; `{{`/`}}` precede `{`; nested interpolation re-enters Wolf expressions and balances nested delimiters |
| interpolation format specs | the first top-level `:` inside `{...}` begins the format spec; colons inside nested delimiters or strings do not; escaped braces and nested width interpolation such as `{value:>{width}}` remain balanced |
| multiline strings | `"""..."""` persists across lines, supports interpolation, and terminates only on the triple delimiter; dedent semantics are not guessed by the highlighter |
| raw strings | `r"..."`, `r#"..."#`, arbitrary hash count via aux; no escapes or interpolation |
| generalized literals | `IDENT"..."` with no whitespace, raw body, no interpolation; reserved-keyword prefixes are not generalized literals |
| declarations and attributes | names after `fn`, `proc`, `type`, `struct`, `enum`, `trait`, `impl`, `let`, `var`, and `const`; `#[...]`; operators and delimiters from the normative punctuation table |
| unsafe surface | `unsafe`, contextual `c`, `asm`, and operand directions receive pinned attrs; `unsafe c {}` enters Wolf's brace-balanced neutral-code context |

Wolf fixture sources live under `tests/syn/wolf/`; they are self-contained
and do not make yew's CI depend on a sibling checkout. At minimum: normative
kitchen sink, all strings, number ambiguities, all 50 keywords, contextual
keywords in and out of context, malformed/unterminated input, Unicode
identifiers, and a real-shaped module derived from Wolf's public corpus.

**Honest boundary:** `unsafe c { ... }` is recognized but is not delegated to
`c.fl`. `wolf.fl` owns a small `inline_c_neutral` context: its default is
`code`; `{` recursively pushes the same context; `}` pops; C string, character,
line-comment, and block-comment subcontexts prevent braces inside those
lexemes from changing depth. It deliberately does not copy C keyword/type
rules. Exact cross-definition C highlighting still needs a balanced
structural embed boundary; ending a guest at the first `}` would be wrong.
No new embed mode lands in a language-pack sprint. The limitation is stated
in `wolf.fl` and `.docs/syntax-def.md`.

### 4. Ship 28 more modes — exactly 48 built-ins total

Every row below names one new runtime file, its canonical `language.name`,
its primary detector, and a construct that must have a dedicated golden.
Definitions are pure data and standalone; no Tree-sitter grammar, TextMate
bundle, generated third-party table, or executable plugin is imported.

| File stem / canonical name | Exact detector arrays | Required edge coverage / pinned approximation |
|---|---|---|
| `cpp` | ext `cc cp cpp cxx c++ hh hpp hxx h++ inl ipp tpp`; files/shebangs none | raw strings with delimiters, nested templates, preprocessor, attributes, C++ comments |
| `objective_c` / `objective-c` | ext `m mm`; files/shebangs none | Objective-C directives, message sends, literals, C/C++ comments |
| `java` | ext `java`; files/shebangs none | annotations, text blocks, generics, numeric suffixes |
| `kotlin` | ext `kt kts`; files/shebangs none | nested block comments, raw strings, `${...}`, safe-call/elvis operators |
| `csharp` | ext `cs csx`; files/shebangs none | verbatim/interpolated strings; raw delimiters of 3–16 quotes; interpolated raw strings with one `$`; multi-`$` raw bodies stay string without interpolation |
| `swift` | ext `swift`; files/shebangs none | nested block comments, raw-string hashes, interpolation, attributes |
| `zig` | ext `zig`; files/shebangs none | builtins, error unions, multiline string lines, container declarations |
| `lua` | ext `lua`; shebang `lua lua5.1 lua5.2 lua5.3 lua5.4 luajit`; files none | long brackets with matched `=` runs for strings and comments |
| `ruby` | ext `rb rake gemspec`; files `Rakefile Gemfile Guardfile Vagrantfile Berksfile Capfile Podfile`; shebang `ruby jruby` | interpolation, `%q/%Q`, symbols, regex heuristic; quoted/unquoted heredocs and `<<-`/`<<~` indented terminators, without dedent rendering semantics |
| `perl` | ext `pl pm t`; shebang `perl`; files none | sigils, quote-like operators, regex/substitution boundaries; quoted/unquoted heredocs with column-zero terminators; indented `<<~` heredocs are deferred |
| `r` | ext `r`; file `.Rprofile`; shebangs none | `%...%` operators, backtick names, raw strings, comments |
| `julia` | ext `jl`; files/shebangs none | nested block comments, macros, triple strings, command literals |
| `dart` | ext `dart`; files/shebangs none | raw strings, interpolation, annotations, cascade/null-aware operators |
| `powershell` | ext `ps1 psm1 psd1`; shebang `pwsh powershell`; files none | expandable/verbatim here-strings, variables, attributes, comments |
| `zsh` | ext `zsh`; files `.zshenv .zprofile .zshrc .zlogin .zlogout`; shebang `zsh` | parameter flags, substitutions, heredocs, glob qualifiers |
| `fish` | ext `fish`; shebang `fish`; files none | command substitutions, variables, escapes, comments |
| `sql` | ext `sql`; files/shebangs none | ANSI core plus quoted identifiers, doubled quotes, dollar strings, comments |
| `nix` | ext `nix`; files/shebangs none | indented strings, `${...}`, path/URI literals, comments |
| `haskell` | ext `hs lhs`; files/shebangs none | nested block comments, pragmas, qualified names, operators, literate form |
| `ocaml` | ext `ml mli mll mly`; files/shebangs none | nested comments, quoted strings, labels/variants, module names |
| `xml` | ext `xml xsd xsl xslt svg`; files/shebangs none | comments, CDATA, entities, processing instructions, attributes |
| `graphql` | ext `graphql gql`; files/shebangs none | descriptions, variables, directives, fragments, block strings |
| `protobuf` | ext `proto`; files/shebangs none | declarations, field numbers, options, comments |
| `hcl` | ext `hcl tf tfvars`; files/shebangs none | quoted/unquoted heredocs and `${...}`/`%{...}` templates; `<<-` accepts leading horizontal whitespace at the terminator but does not model dedent width |
| `dockerfile` | files `Dockerfile Containerfile Dockerfile.* *.Dockerfile Containerfile.* *.Containerfile`; extensions/shebangs none | directives, continuations, variables, comments; shell bodies stay `code` |
| `cmake` | ext `cmake`; file `CMakeLists.txt`; shebangs none | commands, `${...}`, generator expressions, bracket strings/comments |
| `meson` | files `meson.build meson_options.txt`; extensions/shebangs none | function calls, kwargs, strings, comments |
| `diff` | ext `diff patch`; files/shebangs none | file headers, hunk headers, additions, deletions, metadata |

Together with Wolf, these are 29 new modes. Together with the existing 19,
`yew --clean syn list` must report exactly 48 deterministic rows. The list is
the contract; additions discovered during implementation become a later
sprint rather than quietly expanding this one.

Definitions may intentionally approximate parser-only distinctions, but each
approximation must be bounded to a line or a named context and documented at
the top of the definition. Unterminated constructs must never leak beyond the
language's real lexical lifetime. `error` is used only for lexically impossible
input, not for uncertain parser semantics.

One reusable engine addition is in scope: aux matcher `line_eq_ws`, identical
to `line_eq` except that it ignores leading ASCII spaces and tabs before the
captured delimiter. Ruby `<<-`/`<<~` and HCL `<<-` use it; shell keeps the
existing tab-only `line_eq` behavior. C++/Lua/Swift/Wolf delimiter families
use the existing `literal` matcher. No definition may require two simultaneous
aux values. If a construct does, the table's pinned approximation is the
shipped behavior and receives a golden. This is the sprint's only syntax
schema/engine extension: update the strict compiler's closed aux set,
`.docs/syntax-def.md`, malformed-definition diagnostics, focused runtime
tests, and differential/fuzz coverage in the same commit.

### 5. Detection, embedding names, and user definitions

For every new mode, unit tests cover each declared extension, exact/glob
filename, and shebang plus at least one near miss. Collision winners follow
the existing precedence: explicit override, exact filename, glob, longest
compound extension, shebang, first line, then none; priority and canonical
name break ties. `.m` belongs to `objective-c` in the shipped set; users who
need MATLAB/Octave can override it with `ed.syn.set` until such a mode ships.

The current O(definitions × detector rows) built-in scan is replaced by
generated, immutable detection indexes emitted by `scripts/gen-langtab`:

- exact filenames plus case-folded extensions and shebang basenames are stable
  key-sorted arrays searched with bespoke binary lookup;
- duplicate keys retain a contiguous candidate run ordered by priority then
  canonical name, preserving `better()` exactly;
- one-star filename globs and first-line regex descriptors remain separate,
  stable linear lists because their predicates are not exact keys; and
- path precedence stays exact filename, glob, then longest compound extension;
  line precedence stays shebang then first-line regex.

`SynDetectIndex` is an internal injectable view over these arrays. Tests build
0/1/19/48/257-row synthetic indexes, including duplicate keys and compound
extensions. Built-in lookup may not linearly visit every definition. Eagerly
loaded user definitions remain in their current sorted list and are scanned
after the indexed built-in candidates; final category winners still use the
same priority/name rule. The 10,000-path performance gate runs with `--clean`
so it measures the built-in index deterministically, while correctness tests
exercise mixed built-in/user collisions.

Canonical names in the table are also Markdown fence names. Thus `wolf`,
`cpp`, `csharp`, `graphql`, and `hcl` fences use the same lazy embedded
runtime as the original pack. Common alternative fence labels are **not** a
second hidden registry: alias support is deferred until it has an explicit
schema and collision policy. File detection is unaffected.

User definitions under `$XDG_CONFIG_HOME/yew/syntax/*.fl` remain first-class,
sorted, eagerly validated and compiled during discovery as the current
contract requires, and available without a plugin or rebuild. The 48-mode
count is for `--clean` built-ins only; normal `yew syn list` may be larger. A
user definition still may not shadow a built-in canonical name.

### 6. Pack-wide validation and performance re-gate

Each new mode has at least four source/golden pairs: kitchen sink,
multiline/stateful construct, malformed or unterminated construct, and
detection-shaped real file. Wolf has at least twelve. That yields at least
124 new `.spans` files. Coverage is 100% of contexts and rules for all 48
definitions; dead rules are deleted, never waived.

Add one curated large fixture per language family under
`tests/perf/fixtures/syn/`:

| Fixture | Family | Minimum shape |
|---|---|---|
| `wolf_kitchen.lu` | Wolf | 5,000 lines; all string modes and nested interpolation |
| `native_systems.cpp` | compiled systems | 6,000 lines; C++ raw strings/templates/preprocessor |
| `native_vm.kt` | JVM/CLR | 5,000 lines; Kotlin raw/interpolated strings and nested comments |
| `native_script.rb` | scripting | 5,000 lines; heredocs/interpolation/regex-like literals |
| `native_functional.hs` | functional | 5,000 lines; nested comments/operators/pragmas |
| `native_data.xml` | data/schema | 5,000 lines; CDATA/entities/large attributes |
| `native_build.hcl` | build/infra | 5,000 lines; heredocs/template directives |

Sprint 41/42/41.5 baselines remain unchanged, using Sprint 41.5's explicit
guest-unloaded legacy lane and resident-fixture lane. New rows use the same
hard ceilings: per-line median ≤ 3.0 µs, p99 ≤ 12 µs, 200×100 cold viewport
restyle ≤ 1.5 ms, edit-to-settled ≤ 60 µs, scroll ≥ 120 fps equivalent.
The worst definition is reported by canonical name.

Additional scale gates:

| Scenario | Budget |
|---|---:|
| cold start with 48 definitions installed, warm cache | ≤ 20 ms; 0 or 1 definition compiled |
| `yew --clean syn list`, warm filesystem cache | ≤ 2 ms median / 5 ms p99; 0 definitions compiled |
| indexed built-in detection over 10,000 mixed paths under `--clean` | ≤ 1.2 ms p99 and existing 5 ms hard gate |
| `yew syn compile --all`, cold syntax cache | ≤ 100 ms total |
| all 48 warm cache loads | ≤ 15 ms total |
| stripped full binary growth vs `4058b25` | ≤ 48 KiB |
| installed `runtime/syntax/*.fl` total | ≤ 1.5 MiB |
| per-buffer syntax entry storage | unchanged: ≤ 4 B/line + 1% |

The large pack is external data. Only detection metadata enters the binary;
a language definition body is compiled on first use or explicit `syn compile`.

**Closeout adjudication (2026-08-12).** The checked historical control
`4058b25` cannot reproduce its own 1.2x baseline trip on the recorded reference
machine even when pinned to CPU 5 at 1,001 samples: it reports 39 regressions
across 88 rows shared with current `trunk`. Current `f1dd66e` reports 10
regressions in that shared set (12 including new pack rows), while all hard
ceilings in this section, scroll floors, memory limits, embedded-runtime gates,
and size limits are green. Representative current/control results are 0.343 us
/ 34.548 us for `comment_edit`, 0.727 us / 2.425 us for `markdown_line`, and
0.522 ms / 0.562 ms for the Rust 200x100 viewport. The baseline file remains
byte-identical.

Therefore the historical relative component is adjudicated **not measurable
on this host** rather than waived or recalibrated. Sprint 42.5 accepts the
same-machine control comparison plus every unchanged absolute/hard gate as its
closeout evidence. Sprint 56 still owns reference-hardware calibration and may
replace this provisional relative dataset; it must not weaken the absolute
budgets above.

### 7. Documentation and honest deferrals

Update `.docs/syntax-def.md` with a "built-in pack" table generated from
the manifest, the distinction between lexical/embedded/semantic highlighting,
the Wolf worked example, and the global-registry vs embedded-stack limit.
Update `README.md` with `yew syn list`, the built-in count, user-definition
location, and a short statement that no plugin or language server is needed
for lexical highlighting.

Deferred explicitly:

- Campaign 09 / Sprints 43–47, including shadow text, symbol completion,
  JSON-RPC, LSP lifecycle, diagnostics, completion, hover, and navigation —
  resume only after this sprint is green. Semantic-token overlays remain an
  explicit post-1.0 non-goal under the binding Sprint 46/47 contracts.
- PHP, ERB, Jinja, Handlebars, Vue, and Svelte — their normal files require
  bidirectional host/guest interleaving, which Sprint 41.5 explicitly does
  not provide. Shipping a misleading partial mode is worse than a named gap.
- Wolf `unsafe c` guest delegation, Dockerfile instruction-to-shell
  delegation, language-name aliases, and per-embedded-region LSP/comment/
  indentation routing — separate syntax-runtime work, not hidden here.
- Additional modes such as Elixir, Erlang, Clojure, Scheme, Assembly, CUDA,
  GLSL, MATLAB/Octave, and templating engines — field demand or a later
  language-pack sprint. User definitions cover them immediately in the same
  pure-data format.

## Testing Strategy

- **Registry unit tests** (`tests/unit/test_syn_registry.c`): 0, 1, 19, 32,
  33, 48, and 257 generated seed rows; no truncation; descriptor pointer
  stability; one lazy compile per requested id; unknown ids return `NULL`;
  allocation-size overflow reaches the existing fatal `YEW_BUG` contract and
  is asserted through the test bug hook; the 19 old ids match
  `tests/syn/builtin-ids.txt` exactly.
- **Manifest gate** (`scripts/check-syn-assets.sh`): syntax, sort order,
  exact canonical-name match, exactly one base row per definition, no orphan
  rows/directories, source and golden present per row, both set equalities, and no
  literal built-in count or copied language list in the three consumer
  scripts.
- **Golden spans**: ≥124 new files; Wolf ≥12; every new mode covers its table
  edge, unterminated state, and one real-shaped file. Two consecutive dumps,
  gcc vs clang, and cold vs warm cache are byte-identical.
- **Coverage**: `yew syn check --coverage` reports 100% contexts, 100% rules,
  and 100% embed sites for every manifest definition. No exclusion is added
  for a new rule.
- **Detection** (`tests/unit/test_syn_detect.c`): every exact detector in §4
  and a near miss; case-insensitive extensions; `.m` policy; exact/glob
  precedence; indexed 0/1/19/48/257-row and duplicate-key cases; more than 32
  first-line regexes with the last one matching; all 48 names round-trip
  through `yew_syn_lang_named`, `yew_syn_lang_by_name`,
  `yew_syn_lang_desc`, and `yew --clean syn list`.
- **Wolf conformance** (`tests/unit/test_syn_wolf.c`): exact 50-keyword set and
  canonical byte serialization whose pinned SHA-256 is stated in §3, every
  §3 row, interpolation,
  format-spec colon/nested-width cases, delimiter depth, raw hash counts 0–16,
  ordinary/generalized ambiguity, `1.`/`1..10`/`1.e5`, contextual words in
  and out of position, plus nested `unsafe c` braces with braces inside C
  strings/comments ignored for depth.
- **Differential** (`tests/unit/test_syn_all.c`): all 29 new definitions run
  2,000 random edits × 2 seeds against from-scratch highlighting; Wolf and
  the eight state-heaviest definitions additionally run 25,000 edits × 4
  seeds. The long sanitizer lane runs 100,000 × 4 for Wolf plus a rotating
  heavy definition.
- **Fuzz**: every unique base manifest row is a whole-definition seed for
  `fuzz_syn_def`; the line/edit fuzzers receive the new kitchen sinks.
  Targeted 10-minute ASan/UBSan runs for Wolf, C++, Kotlin, Ruby, Perl,
  PowerShell, Haskell, XML, and HCL; zero findings.
- **Pty**: Wolf plus one representative from each §6 family in both themes,
  truecolor and 256-color; switching `ed.syn.set` among three definitions in
  one buffer repaints deterministically; a Markdown document fences every 29
  new canonical name and resolves guests lazily without blocking input.
- **Perf/size**: every §6 budget, old baselines unchanged, new baselines
  committed; build sizes compared to `4058b25` with the same compiler/flags.
- **Lanes**: gcc and clang warning-free, ASan/UBSan, targeted Valgrind for
  registry allocation/lazy load/cache/discovery, determinism, fuzz replay,
  and `make check` before the longer sanitizer/fuzz/perf gates.
- **Dogfood**: real yew sessions during this sprint may be logged toward the
  Sprint 42 daily-driver milestone. CI activity, generated goldens, and
  benchmark loops do not count.

## Definition of Done

1. `make && make test` and `make check` are green on gcc and clang with
   `-std=c11 -pedantic -Wall -Wextra -Werror`; ASan/UBSan, targeted Valgrind,
   determinism, fuzz replay, and syntax perf gates are green.
2. `rg 'desc\[32\]|builtins\[32\]|slots\[32\]' src/syn/defs.c` is empty.
   Registry tests cover 33 and 257 rows without truncation; the last of more
   than 32 first-line regexes matches; no new fixed definition-count macro or
   array cap exists.
3. `sizeof(SynState) == 84`, `YEW_SYN_DEF_MAX == 4`,
   `YEW_SYN_DEPTH_MAX == 16`, `YEW_SYN_RESIDENT_MAX == 255`, and per-line entry
   storage remain unchanged.
4. `tests/syn/manifest.tsv` has exactly 48 base rows and all intended embed
   rows; its `definition` set equals `runtime/syntax/*.fl`, while its
   `language` set equals the id ledger, generated language rows, and
   `yew --clean syn list` exactly.
5. The original 19 canonical-name/stem→id rows exactly match the table
   generated at `4058b25`; the 29 new modes append deterministically and
   repeated generator runs are byte-identical.
6. `runtime/syntax/wolf.fl` satisfies every §3 row against the frozen Wolf
   lexical contract and canonical keyword bytes whose SHA-256 is pinned in
   §3; ≥12 Wolf goldens and the Wolf conformance unit, including format
   specs and contextual `out`, are green.
7. All 28 remaining files in §4 ship with their stated detection and edge
   golden. `yew --clean syn list` reports exactly 48 modes; normal discovery
   still adds user definitions without allowing a built-in shadow.
   `line_eq_ws` has strict compiler/runtime/documentation tests, and existing
   shell `line_eq` goldens remain byte-identical.
8. Every definition passes `yew syn check --strict`; pack coverage is 100%
   contexts, rules, and embed sites with no waiver. Each new mode has at least
   four source/golden pairs and the sprint adds ≥124 deterministic goldens.
9. Every declared detector and near miss is unit-tested; the generated exact
   indexes preserve collision/precedence behavior and meet the 10,000-path
   gate; all 48 ids/names round-trip through the public registry surfaces and
   canonical Markdown fences resolve each new definition lazily.
10. Manifest-driven asset, golden, and fuzz scripts contain no copied
    language matrix and no literal expected definition count;
    `scripts/gen-s42-fuzz-corpus.sh` no longer exists.
11. Differential testing is green at §Testing Strategy volumes; targeted
    10-minute sanitizer fuzz runs report zero findings; malformed and
    unterminated fixtures always make forward progress and remain bounded.
12. All old syntax perf rows stay within their existing absolute budgets. The
    1.2x relative trip is green on a host where `4058b25` reproduces its own
    baselines; otherwise the dated same-machine control adjudication in §6 is
    the binding fallback. Every new §6 budget is green with committed
    baselines. Cold start compiles at most one of 48 definitions.
13. Stripped full-binary growth is ≤48 KiB versus `4058b25`; installed syntax
    data is ≤1.5 MiB; Valgrind reports zero definite leaks or invalid accesses
    across registry initialization, lazy load, discovery reset, and cache use.
14. Both themes render the family PTY matrix byte-identically across two runs
    and both compilers; the 29-language Markdown fence fixture resolves within
    one idle load per settle call and never blocks a keystroke.
15. At closeout, `.docs/syntax-def.md`, `README.md`, `.docs/sprints/index.md`,
    `.docs/HANDOFF.md`, `.docs/milestones.md`, and `.docs/dogfood-log.md` agree
    on 48 built-ins, Sprint 42's pending field milestone unless separately
    earned, Sprint 42.5 complete, and Sprint 43 as the next implementation
    frontier; no unresolved placeholder marker remains in a changed document.
16. No Sprint 43–47 implementation lands in the Sprint 42.5 commit range.
    Campaign 09 remains paused until items 1–15 are green, then Sprint 43 is
    the next binding contract.
