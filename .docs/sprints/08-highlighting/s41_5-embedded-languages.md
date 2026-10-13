# Sprint 41.5: Embedded Languages — Cross-Definition Dispatch

## Prerequisites

- **Sprint 39 — this sprint modifies it, and that is the whole point.**
  `SynState` (40 B, `_Static_assert`ed), the `u16 ctx[YEW_SYN_DEPTH_MAX]`
  stack with `YEW_SYN_DEPTH_MAX = 16`, the saturating `lost` counter and
  its push/pop balance law, the reserved `u8 def` byte asserted 0, the
  interned-`u32` state table (`yew_syn_state_intern`,
  `YEW_SYN_MAX_STATES = 65536`, `YEW_SYN_STATE_{UNKNOWN,ROOT}`), the
  **per-line purity law** (`yew_syn_line` depends only on `(def,
  entry_state, line, len)` — no buffer access, no globals, no clock), the
  `first[32]` fast-scan, `SynBuf.entry[]` + its splice, `yew_syn_settle`
  and its budgets (`YEW_SYN_FRAME_BUDGET_US = 1000`,
  `YEW_SYN_IDLE_BUDGET_US = 4000`), and the `(line, buf_gen, entry_state)`
  span cache.
- **Sprint 40** — the schema, `yew_syn_def_compile` / `yew_syn_def_load` /
  `yew_syn_def_for(lang)` and its **laziness rule** (a definition body is
  compiled on first use *in this process*, detection tables come from the
  generated `src/syn/langs_gen.c`), the packed pointer-free table block,
  the XDG cache, the four aux matchers, `set_aux`/`strip`, `include:`,
  `unit:` marks and the B-mode provider (`yew_syn_stack_at`), the
  validator table (§9), the first-byte-set inclusion law, and its pinned
  refusal: **an `embed:` key is a compile error naming this sprint.**
- **Sprint 41** — `markdown.fl`'s `fenced` context (`set_aux: 1` holds the
  fence run; `fence_close`; `unit: "atom"`), `sh.fl`'s heredoc machinery
  and the *deferred entry* idiom (`at_eol: "set:…"`), `make.fl`'s recipe
  context, both themes and the 54-attr table, the frozen perf fixtures and
  §10's budget table, and §6 — the deferral this sprint discharges.
  s41 DoD 13 (`SynState.def == 0`) is **superseded** here; §7 says how.
- **Sprint 42** — the other eight languages, `javascript.fl`/
  `typescript.fl` (the `value` flag, `template`/`template_expr`, and the
  documented JSX limitation), 17 definition files / 14 languages, the
  coverage gate, `yew syn dump --spans --check`, and §11's deferral of
  JSX and embedded dispatch to this file.
- Sprint 21 — the budgeted-overlay precedent this sprint's async
  definition loading copies. Sprint 15 — the 16 ms idle tick and the
  statusline segment table. Sprint 56 — §9's budget row is owed to this
  sprint by name (s56 §9).
- Binding: `00-decisions.md` (bespoke line-state-machine engine, cold
  start ≤ 20 ms, keypress p99 ≤ 5 ms, scroll ≥ 120 fps, invariants 3, 4,
  5, 7, 8).

**Scheduling note.** This sprint runs *after* Sprint 42: its central cost
is re-gating every golden the two langpacks commit, and paying that once
against the final state representation is the whole argument in s41 §6.
s60 §9 lists embedded highlighting in `.docs/backlog-1.1.md` — landing
this before the 1.0 freeze deletes that row, landing it after makes this
1.1's first sprint. The content is the same either way.

## Goals

Make a line's entry state name **which definition** is running, not just
which context, and use that to let one language's definition highlight a
region of another's: markdown fenced blocks (any installed language, named
by the info string), HTML `<script>`/`<style>`, make recipe lines → `sh`,
shell `$(…)`/`` `…` `` re-entering shell at its root, and JS/TS tagged
template literals + JSX. Concretely: widen `SynState` from a flat context
stack to a stack of `(def, ctx)` frames with a per-definition-level `aux`
slot (§1); add the `embed:` delegation rule to Sprint 40's schema,
including how the host's end delimiter is carried so the guest knows when
to hand back — the fenced-block closing fence and the heredoc terminator
are **the same problem** (§2); pin the combined depth cap across
definitions and what happens at it (§3); ship `html.fl` and `css.fl`,
without which "HTML `<script>`" is not a feature (§4); extend the async
side so a definition needed by an embed is *never* compiled inside the
pure per-line function (§5); re-gate every s41/s42 golden, fixture, and
coverage run against the new dump format (§7); and state honestly what
stays unsupported (§8). The five deferral notes in s39 §9, s40 §10, s41
§11, s42 §11 and s56 §9 all point here and all close here. Nothing in the
attr table, the theme layer, or the render path changes.

## Deliverables

### 1. The widened state — `src/syn/engine.h`

s39's `u16 ctx[16]` is a stack of context ids, and a context id is only
meaningful *relative to one definition's table*. Cross-definition dispatch
therefore cannot be a single `def` byte beside the stack: when the guest
pops back, the engine must know which definition the frame beneath
belonged to. The def travels **with the frame**.

```c
#define YEW_SYN_DEF_MAX 4        /* definitions on the stack, root included */

typedef struct SynFrame {
    u16 ctx;      /* context id within defs[def]'s table — NEVER global   */
    u8  def;      /* definition id; index into SynEngine.defs[]           */
    u8  fl;       /* YEW_SYN_FR_BRIDGE: this frame owns an embed boundary */
} SynFrame;
_Static_assert(sizeof(SynFrame) == 4, "frame packs to 4 bytes");

typedef struct SynState {
    SynFrame f[YEW_SYN_DEPTH_MAX];  /* 64 B — was u16 ctx[16] = 32 B      */
    u32      aux[YEW_SYN_DEF_MAX];  /* 16 B — one live aux PER def level  */
    u8 depth;    /* 1 … YEW_SYN_DEPTH_MAX, unchanged                      */
    u8 lost;     /* saturating over-depth counter, unchanged (s39 §2)     */
    u8 ndef;     /* 1 … YEW_SYN_DEF_MAX — definition-stack depth          */
    u8 flags;    /* _VALUE | _STRIP | _DEGRADED | _EMBED_LOST | _EMBED_PEND */
} SynState;
_Static_assert(sizeof(SynState) == 84, "state tuple size");

#define YEW_SYN_DEF_OF(s)  ((s)->f[(s)->depth - 1].def)   /* active def   */
#define YEW_SYN_AUX_OF(s)  ((s)->aux[(s)->ndef - 1])      /* live aux slot */
```

`SynState.def` — s39's reserved byte — is **deleted, not repurposed**, so
no code can keep reading a field whose meaning changed underneath it. A
grep gate in DoD 3 proves the identifier is gone from `src/`.

| Quantity | s39 | here | Consequence |
|---|---|---|---|
| `sizeof(SynState)` | 40 B | 84 B | interner rows and hash width only |
| **per-buffer-line storage** | **4 B** | **4 B** | **unchanged** — the line array is still `Vec_u32` of interned ids |
| interner table, worst case (65536 states) | 2.6 MiB | 5.5 MiB | arena, grown on demand; real definitions reach hundreds of states, so the realized cost is kilobytes |
| interner hash | one 40 B pass | one 84 B pass | §1's normalization keeps it one pass, not a loop over live frames |

**Why the per-line cost does not move, said plainly:** the line array
stores interned *ids*, never tuples. A 1 M-line file still costs 4 MiB and
still fits the 1.6× memory gate. What grows is the *number of distinct
states*: a markdown buffer fencing eight languages carries roughly
`states(markdown) + Σ states(guest_i)` — additive, not multiplicative,
because the host stack below a bridge frame is one frame deep in practice.
The pathological product (one guest entered from many host contexts) is
bounded by `YEW_SYN_MAX_STATES = 65536`, whose exhaustion path is
unchanged from s39 §2: log once, reuse `YEW_SYN_STATE_ROOT`, degrade,
never fail.

> **Pitfall — the unused tail must be normalized or interning silently
> stops working.** Two logically identical states must be *byte* identical
> or they intern to different ids, and then s39 §6's fixpoint test
> (`held == next`) never fires and **every keystroke walks the whole
> file**. So: `yew_syn_state_intern` takes states through
> `syn_state_canon()`, which zeroes `f[depth … 15]` and `aux[ndef … 3]`
> before hashing. Every pop must clear the frame it left; every embed exit
> must clear its aux slot. This is a performance cliff that presents as
> "the editor got slow on big files", not as a wrong color, which is why
> DoD 5 asserts it directly: 10 000 random push/pop/embed/exit sequences
> that return to the same logical state must return the same `u32`, and a
> deliberately un-normalized build must fail that test.

`aux` is per **definition level**, not per frame, deliberately: within one
definition s39's single-live-`aux` rule is unchanged (python's f-strings,
rust's raw strings, sh's heredocs still get exactly one live aux, s42's
documented limits intact). The embed boundary is the *only* thing that
buys a fresh slot — which is what makes a heredoc inside `$(…)` inside a
heredoc work, and why `@self` (§2) costs an `ndef` level even though it
re-enters the same definition.

### 2. The delegation rule — `embed:` in the definition schema

Sprint 40's compiler rejected `embed:` naming this sprint. It is now a
context-level and rule-level key. `.docs/syntax-def.md` gains a section.

```fletch
# runtime/syntax/markdown.fl — the fence rule, now delegating.
{ match: "^ {0,3}(`{3,}|~{3,})[ \\t]*([A-Za-z0-9_+.-]*)",
  attr: "punct", captures: { 1: "punct", 2: "attribute" },
  set_aux: 1,                       # the fence RUN, for fence_close
  push: "fenced",
  embed: { lang: "@2",              # the INFO STRING names the language
           defer: true,             # body starts on the NEXT line
           end:   "line",
           fallback: "code" } },

fenced: {
    default: "code", at_eol: "stay", unit: "atom",
    rules: [ { aux: "fence_close", attr: "punct", pop: 1, end: true } ],
},
```

| Key | Type | Default | Meaning |
|---|---|---|---|
| `embed.lang` | string | required | a definition `name`; `"@N"` = the text of capture group N of this rule; `"@self"` = this definition's own root |
| `embed.ctx` | string | guest `root` | enter the guest at a **named context**, not its root — how JSX borrows `html`'s tag rules (§4) |
| `embed.end` | `"line"` \| `"inline"` \| `"inline-root"` \| `"line-continuation"` | required | when the host's exit rules are tested (below); the latter two close §4's balanced-expression and recipe-continuation cases |
| `embed.defer` | bool | `false` | push the guest frame at end of line instead of at the match — s41 §3's deferred-entry idiom, generalized |
| `embed.fallback` | attr | context `default` | attr for the region when the language is unknown, unavailable, or refused at the cap |
| `end` (on a *rule*) | bool | `false` | marks a **host exit rule**; legal only in a context carrying `embed` |

`embed` is a state op: naming it together with `push`/`pop`/`set` on the
same rule is a validation error, exactly as those three are mutually
exclusive today. The `push` above is the *bridge* push — the host enters
`fenced` normally, and `embed` says what happens once it is there.

**How the boundary is built.** A rule with `embed` performs, in order:

1. push the bridge frame: `f[depth] = {ctx: target, def: host_def,
   fl: YEW_SYN_FR_BRIDGE}`; `depth++`;
2. resolve `embed.lang` to a definition id (§5 — pure, no I/O);
3. push the guest frame: `f[depth] = {ctx: guest_root_or_embed_ctx,
   def: guest, fl: 0}`; `depth++`; `ndef++`; `aux[ndef-1] = 0`.

With `defer: true`, step 1 happens at the match and steps 2–3 at end of
line, so the remainder of the fence line stays host text.

**How the guest hands back — the end-delimiter carry.** This is the hard
part, and the observation that makes it one mechanism instead of four:
**a fenced block's closing fence, a heredoc's terminator, a `</script>`,
and a `$(…)`'s closing paren are all "text captured by the host, tested
while the guest is running".** The engine therefore does not hand the
delimiter *to* the guest at all. The bridge frame stays on the stack under
the guest, still carrying the host's definition, its context, and — via
`aux[ndef - 2]` — the host's live aux. Before running any guest rule, the
engine tries the bridge context's `end: true` rules, evaluated **in the
host's definition**, so `fence_close` reads the host's fence run and
`line_eq` reads the host's heredoc delimiter with no new machinery.

| `embed.end` | Exit rules tested | Cost | Used by |
|---|---|---|---|
| `"line"` | at scan position 0 only, once per line | one rule attempt per line | markdown fences, make recipes, heredoc-shaped hosts |
| `"inline"` | at every scan position, before the guest's context rules | one first-byte bitset test per byte (see the pitfall) | `</script>`, `</style>`, `$(…)`'s `)`, `` ` `` |
| `"inline-root"` | at every scan position, but only while the guest remains at its entry frame | the `inline` cost plus one depth comparison | balanced `${…}` and JSX attribute expressions; nested guest frames consume their own closing bytes first |
| `"line-continuation"` | as `line`, but bridge and guest survive an odd trailing-backslash run and exit after the first non-continued physical line | one trailing-byte scan at EOL | Make recipes continued onto tabbed or unprefixed physical lines |

On a match the engine pops every frame down to but not including the
bridge frame, clears `aux[ndef - 1]`, decrements `ndef`, and then applies
the exit rule's own op in the bridge context (`pop: 1` above pops the
bridge itself). Frames the guest had open are discarded — an unterminated
string inside a fenced block dies with the fence, which is what a reader
expects and what CommonMark specifies.

> **Pitfall — the guest's fast-scan will skip straight over the exit
> delimiter.** s39's scan loop consults `ctx->first[32]` and consumes, in
> one tight loop, every byte on which no rule of the *current* context can
> start. Inside a `<script>` the current context is JavaScript's, whose
> first set has no reason to contain `<`. The scanner then walks past
> `</script>` without ever testing the exit rule, and the rest of the file
> is JavaScript. The fix is mechanical and mandatory: while `ndef > 1` and
> `embed.end == "inline"` or `"inline-root"`, the fast-scan mask is `ctx->first ∪
> bridge_end_first`, computed once per `(context, bridge)` pair and cached
> in a 64-entry direct-mapped table on `SynEngine`. This is the same
> failure shape as s40 §5's "a first-byte set that is too small is a
> correctness bug, not a slow path", and it gets the same brute-force
> self-check: for every embed site and all 256 byte values, if the exit
> rule can match on that byte, the merged mask has the bit.

> **Pitfall — `end: "line"` must be tested before `at_eol`, and before the
> guest's own line-start rules.** A markdown fence closing on a line that
> is also legal JavaScript (` ``` ` is not, but `~~~` inside a heredoc
> body is) must close the fence. Order per line, pinned: merged exit rules
> → guest context rules → guest `at_eol` → host `at_eol` (bridge frames
> never run `at_eol` while a guest is above them).

### 3. Depth across definitions, and what happens at the cap

`YEW_SYN_DEPTH_MAX` stays **16 total frames** — the cap is on the stack,
not per definition, so markdown → html → js competes for the same 16 with
js's own nesting. `YEW_SYN_DEF_MAX` is **4** definitions including the
root: markdown → html → js is three, and one level of slack is enough for
`@self` on top (a `$(…)` inside a fenced `sh` block inside markdown).

| Event | Behavior |
|---|---|
| `embed` push with `depth + 2 > YEW_SYN_DEPTH_MAX` and one bridge slot remains | **guest refused**; retain the bridge, `lost += 2`, `flags \|= YEW_SYN_F_EMBED_LOST`; the region styles with `embed.fallback` and the host exit rule clears the protected debt before applying its own operation |
| `embed` push at `depth == YEW_SYN_DEPTH_MAX` | **whole boundary refused**; no bridge can be represented, so `lost += 2`, `flags \|= YEW_SYN_F_EMBED_LOST \| YEW_SYN_F_EMBED_ORPHAN`; ordinary pops affect real frames rather than this protected debt, which clears at physical EOL; the current physical line remains in the host definition |
| `embed` push with `ndef == YEW_SYN_DEF_MAX` | same retained-bridge refusal and accounting as the first row |
| a retained refused embed's exit rule fires | clear the protected two-count embed debt, discard the bridge boundary, then apply the exit rule's ordinary operation |
| `YEW_SYN_F_EMBED_LOST` set | statusline `syn!`, one `yew_log(WARN)` per buffer, `ed.syn.status` names the refused language |

Refusing the *whole* embed rather than truncating the guest's stack is the
only honest option: a guest entered with 1 frame of headroom would push,
get refused, and produce a stack that its own pop rules cannot balance —
exactly s39's asymmetric-cap corruption, one level up.

**The static check moves to load time, because dynamic embeds have no
static target.** Sprint 40 §9 row 10 rejects a definition whose acyclic
push depth exceeds 12. That check is per definition and cannot see across
a boundary. Additions:

| Row | Condition | Severity | Message |
|---|---|---|---|
| 18 | `embed.lang` is a literal name that no installed definition provides | error | `embed: no definition named 'scss'` |
| 19 | context carries `embed` but has no `end: true` rule and no reducing `at_eol` | error | `context 'fenced' embeds a language it can never leave` (s40 §9 row 12, extended across the boundary) |
| 20 | `end: true` on a rule in a context with no `embed` | error | `'end' marks a rule that returns from an embedded language` |
| 21 | `embed` together with `push`/`pop`/`set` on one rule | error | `embed is a state op; a rule performs exactly one` |
| 22 | `embed.lang: "@N"` where the rule has no capture group N | error | as s40 row 6 |
| 23 | combined depth of a statically named chain > `YEW_SYN_DEPTH_MAX` | error | `markdown→html→javascript can reach depth 19; the cap is 16` |
| 24 | `embed.lang: "@N"` (dynamic) whose worst installed guest exceeds the budget | **warning** | names the deepest installed definition and the headroom |

Row 23/24's arithmetic is `yew syn check --embed`, a new mode that walks
every definition's embed sites, resolves the static ones, and prints a
combined-depth table. CI runs it over `runtime/syntax/*.fl` and fails on
any error row. It is a warning, not an error, for dynamic sites because
the guest set is open by construction (a user drops a `.fl` in the config
dir and markdown can fence it) — the runtime refusal in the table above is
the real guard, and it degrades instead of lying.

### 4. Shipped embed sites, and the two new definitions

"HTML `<script>`/`<style>`" is not a feature without an `html.fl` and a
`css.fl` to host and be hosted, so both ship here. That takes the count to
**16 languages / 19 definition files** and removes `html`/`css` from s42
§11's post-1.0 list; §7 re-states every count s42 gated on.

| # | Host | Site | Guest | `end` | Notes |
|---|---|---|---|---|---|
| 1 | `markdown` | fenced block | `@2` — the info string, through the same name lookup `yew syn list` uses; unknown → `code` | `line` | `defer: true`; `fence_close` on the host's `aux` is unchanged from s41 §5 |
| 2 | `markdown` | indented code block | none | — | deliberately not embedded: no info string, no language, and guessing is worse than `code` |
| 3 | `html` | `<script>` … `</script>` | `javascript`; `type="module"` too; a `type` the table does not know → `fallback: "text"` | `inline` | the `type` attribute is captured on the open tag and resolved once |
| 4 | `html` | `<style>` … `</style>` | `css` | `inline` | |
| 5 | `make` | recipe line (the `^\t` context) | `sh` | `line-continuation` | the recipe context's `at_eol: "pop"` is the exit, so a recipe body never leaks. An odd trailing-backslash run keeps both bridge and guest across tabbed or unprefixed continuation lines |
| 6 | `sh` | `$(…)` and `` `…` `` | `@self` | `inline` | §4's payoff, below |
| 7 | `javascript` / `typescript` | tagged templates `` html`…` ``, `` css`…` ``, `` gql`…` `` (a closed 3-row table) | `html` / `css` / `fallback` | `inline` | the tag is the capture; an untagged template is unchanged from s42 |
| 8 | `javascript` / `typescript` | JSX element tree | `html`, `embed.ctx: "jsx_tag"` | `inline-root` | nested markup stays in HTML; balanced `{...}` expressions borrow JavaScript's `template_expr` context and return to HTML |

**`$(…)` as `@self` — why re-entering the same definition is not a
no-op.** s41 shipped `$(…)` as a plain context push, which gets nested
quoting wrong: in `echo "a $(grep "b" f) c"` the inner `"` opens a *new*
string, but a plain push leaves the outer `dq` frame on the stack and its
rules keep matching. `@self` pushes the definition's **root**, so the
inner region is top-level shell — command position, its own quoting, its
own heredocs — and the bridge frame's `end: "inline"` rule (`\)` with
paren balance) restores the outer string on the way out. It consumes an
`ndef` level, so the region gets its own `aux` slot: that is what makes a
heredoc inside a substitution inside a heredoc terminate on the right word.

**JSX as `embed.ctx: "jsx_tag"` — entering a guest away from its root.** JSX
is markup embedded in expressions embedded in markup. A JS-only definition
means either a copy of html's tag rules inside `javascript.fl` or
expression rules inside `html.fl` — two places to fix every bug, either
way. Entering `html` at its `jsx_tag` context gives JSX real tag/attribute and
nested-child styling from one source. `{` inside an attribute or child embeds
into `javascript` at its balanced `template_expr` context through an
`inline-root` boundary, then returns to the same HTML tree. The root closing
tag returns from HTML to the surrounding JS/TS context. The depth cap bounds
that alternation at four definitions and refuses deeper guest alternation
honestly. s42's "JSX tags style as expressions" limitation is retired and
its known-wrong goldens lose their comments.

### 5. Loading a definition an embed needs — the latency hazard, discharged

s41 §6 reason 2 named this the second reason for a separate sprint.
`yew_syn_line` is **pure**: no I/O, no globals, no clock. Compiling or
cache-loading a definition inside it breaks the purity law, the
differential oracle, and the frame budget in one move. Dispatch is
therefore two phase, split along s21's budgeted-overlay shape.

```c
/* Phase 1 — inside yew_syn_line.  PURE: reads only the generated
 * detection table and the engine's resident-definition array.          */
u32  yew_syn_lang_by_name(const u8 *name, u32 len);   /* langs_gen.c    */
const SynDef *yew_syn_def_resident(SynEngine *e, u32 lang); /* or NULL  */

/* Phase 2 — inside yew_syn_settle.  Impure, budgeted, off the pure path. */
#define YEW_SYN_EMBED_LOADS_PER_SETTLE 1
#define YEW_SYN_EMBED_LOAD_BUDGET_US   2000   /* charged to the idle tick */
bool yew_syn_embed_pump(SynBuf *s, SynEngine *e, i64 budget_us);
```

| Phase-1 outcome | State produced | What the user sees |
|---|---|---|
| name resolves, definition resident | normal guest frames | correct colors immediately |
| name resolves, definition **not resident** | bridge frame only, `flags \|= YEW_SYN_F_EMBED_PEND`, pending lang recorded in the bridge frame's `ctx` shadow slot | region styles `embed.fallback` (`code`) — the s41 behavior, exactly |
| name does not resolve | no pending flag, no retry | `embed.fallback` forever, one `yew_log` line, never an error |

`yew_syn_embed_pump` runs at the head of each *idle* settle call (never
the keystroke call), loads **at most one** definition per call through
s40's cache path, then invalidates from the first line whose entry state
carries `EMBED_PEND` for that language — an ordinary `yew_syn_edit`-shaped
damage, so the existing budgeted wave repaints it. A markdown file fencing
eight languages resolves all eight within eight idle ticks (~128 ms of
idle) and never blocks a keystroke.

> **Pitfall — do not retry a pending load inside the wave.** The settle
> loop is where the load happens *and* where the reruns happen; a naive
> implementation loads inside the per-line branch, so a file with 400
> fenced blocks loads a definition, invalidates, walks back, loads the
> next, invalidates again — quadratic. The pump runs **once per settle
> call, before the wave**, and the wave never loads.

**Cold start is untouched.** Nothing here runs at file open: opening a
`.md` compiles `markdown.fl` and nothing else (s40 DoD 9's counter
assertion is re-run here with the count still 1). The 20 ms gate is
protected because the only path that compiles a guest is an idle tick.

### 6. Reading the stack when the definition varies — every consumer

Three surfaces index a context table, and every one of them was written
when there was exactly one. Each is now a bug waiting for a fence:

| Consumer | Was | Is |
|---|---|---|
| `yew_syn_line`'s scan loop (s39 §4) | `e->def->ctxs[st->ctx[depth-1]]` | `syn_ctx(e, &st->f[depth-1])` — resolves through `f.def` |
| B-mode provider (s40 §7) | `unit` mark read from the buffer's definition | read from `defs[f.def]`; a guest's `atom` string inside a fenced block is a B-mode atom, and the *bridge* frame is a `span` level, so `A-↑` from inside embedded JS expands: token → JS string → JS block → the whole fenced block → the markdown document |
| span cache (s39 §7) | keyed `(line, buf_gen, entry_state)` | **unchanged, and that is the point** — see the pitfall |

```c
/* The only sanctioned way to turn a frame into a context.  Bounds-checks
 * both indices and YEW_BUGs on either, because the failure mode of the
 * unchecked version is reading rule bytes out of a foreign definition's
 * packed block — a wrong-colors bug that presents as a crash on a
 * different file three days later.                                    */
static inline const SynCtx *syn_ctx(const SynEngine *e, const SynFrame *f);
```

> **Pitfall — one interner for the whole engine, never one per
> definition.** The per-buffer line array `SynBuf.entry[]` is keyed by
> *buffer line*, and there is exactly one of it: an embedded region does
> **not** get a second `SynBuf`, a second state table, or a second wave.
> The tempting "optimization" — intern each definition's states in its own
> table so ids stay small — makes id 7 mean `markdown:fenced` on one line
> and `javascript:template` on the next. Then s39 §6's fixpoint test
> (`held == next`, a `u32` compare) compares ids from different
> namespaces, accepts a false fixpoint, and stops the wave early: the tail
> of the file keeps a stale entry state that no edit will ever correct.
> The `def` lives *inside* the interned tuple precisely so that one global
> id space stays sound, and the span cache's `entry_state` key inherits
> that soundness for free. DoD 6 asserts it: two states differing only in
> `f[0].def` must intern to different ids, and a single-table build with
> the def field masked out must fail the differential oracle.

`ed.syn.status` gains the cross-definition line, and it is the first thing
a bug report should carry:

```
settled 4210/98765, wave 4210, root=markdown active=javascript
defs=2/4 depth=5/16 states=612 degraded=no embed_pending=0
```

### 7. The re-gate — why this is its own sprint, stated as work

Widening the state changes what a golden *is*, so every golden the two
langpacks committed must be regenerated and re-reviewed. That is the cost
s41 §6 reason 1 refused to pay twice, and it is the bulk of this sprint's
mechanical effort.

1. **Dump format.** `yew syn dump --spans` prints context names, never
   ids (s40's rule). Names are now **definition-qualified**:
   `markdown:fenced`, `javascript:template_expr`, `sh:heredoc`. Every
   `.spans` golden from s41 (≥ 90 files) and s42 (its full fixture matrix)
   changes on every line, so they are regenerated wholesale by `make
   syn-goldens` and reviewed as one commit that contains **no other
   change** — a mixed commit here is unreviewable.
2. **The def-is-zero assertions.** s41 DoD 13 and s42 §11 assert
   `SynState.def == 0` over the whole corpus. Replacement, same guarantee
   in the new shape: for every state produced by a definition with no
   `embed` site, `ndef == 1` and `f[i].def == root_def` for all
   `i < depth`. The 12 definitions with no embed site keep proving that
   property; the seven hosts that now embed (`css`, `html`, `javascript`,
   `make`, `markdown`, `sh`, and `typescript`) are excluded by name in one
   table, not by weakening the assertion.
3. **Coverage.** `yew syn check --coverage` runs over 19 files; the two
   new definitions arrive with 100 % context and rule coverage like every
   other, and each *embed site* is added to the coverage ledger — an embed
   never taken by any fixture fails CI exactly as an unfired rule does.
4. **`--strict`.** `yew syn check --strict runtime/syntax/*.fl` exits 0
   with zero warnings across all 19, including the new §3 rows.
5. **Perf.** Every budget in s41 §10 and s42 §9 re-runs **unchanged** on
   the existing frozen fixtures — the new state must not cost the old path
   anything — plus §9's fixtures and their stated allowances.
6. **The differential oracle** (s39's `test_syn_diff.c`) gains the
   embedding fixtures. It is the only test that proves incrementality, and
   a cross-definition state that does not settle is what it exists to
   catch.

### 8. Honest scope — what this does not do in 1.0

- **No host/guest interleaving on one line where both need every byte.**
  PHP's `<?php … ?>`, ERB, Jinja, Handlebars and Vue SFC templates re-enter
  their host mid-expression in both directions. The bridge model handles
  one direction per boundary; the other needs the host's rules live
  *inside* the guest's context — a rule-merging compiler feature, not a
  state-representation feature. Post-1.0, named in `.docs/syntax-def.md`.
- **`</script>` inside a JavaScript string ends the block.** This looks
  like a bug and is the specified behavior — HTML's tokenizer ends the
  script element there too, which is why every real codebase writes
  `"<\/script>"`. Documented in `html.fl`'s header with the workaround.
- **No `<style lang="scss">` / `<script lang="ts">` beyond a closed
  attribute table.** The `type`/`lang` attribute is matched against a
  fixed 6-row table; anything else takes `fallback`. Open-ended attribute
  dispatch means `yew_re_compile` at render time, which s40 §3 already
  forbids.
- **One guest per line for `end: "line"` sites.** Same shape as s41's
  "two heredocs on one line" limit, same reason: one live `aux` per
  definition level.
- **No per-region indentation, folding, comment string, or LSP dispatch.**
  A `<script>` block does not get JavaScript's `commentstring` or its own
  language server. B-mode units are the only non-highlight consumer
  crossing the boundary (§6) — the whole 1.0 surface.
- **Markdown indented code blocks, and fences with an unknown info
  string,** stay `code`. Guessing a language from block content is a
  classifier, and one that is wrong 10 % of the time is worse than a
  definition that is honestly plain.

### 9. Perf — the alternating-definition budget

New frozen fixtures under `tests/perf/fixtures/syn/`, committed once:

| Fixture | Content | Lines | Size |
|---|---|---|---|
| `md_embed.md` | 400 fenced blocks over 8 languages, **alternating every 5 lines** | 6 000 | 190 KiB |
| `html_embed.html` | 200 `<script>`/`<style>` pairs, inline handlers | 4 000 | 130 KiB |
| `sh_subst.sh` | 500 `$(…)` sites, nested 3 deep, heredocs inside two of them | 2 000 | 62 KiB |
| `tsx_embed.tsx` | JSX with expression children, 60 tagged templates | 3 000 | 110 KiB |

The alternating fixture is the adversarial case by construction: every
five lines the active definition changes, so the merged first-byte mask,
the context table, and the packed rule block all miss cache, and the state
count is the sum over eight guests. Budgets — all measured by
`tests/perf/perf_syn.c`, baselines in `tests/perf/baselines/syn.txt`,
median regression trip at 1.2×:

| Scenario | Budget | Why this number |
|---|---|---|
| **Full-viewport restyle, 200×100, cold, any fixture incl. `md_embed.md`** | **≤ 1.5 ms** | **unchanged from s41 §10** — non-negotiable; embedding may not buy itself a bigger frame |
| Per-line median, `md_embed.md` | ≤ 3.5 µs | s41's 3.0 µs + 17 % for the merged mask and the exit-rule attempt; stated as an allowance, not discovered after the fact |
| Per-line p99, `md_embed.md` | ≤ 14 µs | s41's 12 µs + the same allowance |
| Per-line median, all s41/s42 fixtures | ≤ 3.0 µs | **unchanged** — a file with no embed pays nothing |
| Definition switch, amortized | ≤ 250 ns | one merged-mask table lookup + one `SynDef` pointer load |
| `end: "inline"` scan overhead | ≤ 8 % over the same content with the site removed | measured as a pair on `html_embed.html` |
| Single-char edit inside an embedded region → settled | ≤ 60 µs | **unchanged**; the fixpoint is still ≤ 2 lines because the bridge frame does not change |
| `yew_syn_embed_pump`, one definition, warm cache | ≤ 2 ms, ≤ 1 per settle call | s40's cache-load budget is 200 µs; 2 ms covers a cold compile |
| `md_embed.md` open → all 8 guests resident | ≤ 8 idle ticks, editor never blocked | §5 |
| Cold start, 19 definitions installed, warm cache | ≤ 20 ms, exactly 0 or 1 compiled | the gate embedding was deferred to protect |
| Interner rows, `md_embed.md` fully settled | ≤ 2 500 states | ~10× the single-language figure; the headroom to 65 536 is the argument |
| State-array memory, every fixture | ≤ 4 B/line + 1 % | the per-line cost did not move (§1) |
| Scroll throughput, `md_embed.md` and `html_embed.html` | ≥ 120 fps equivalent | s15's gate, now with two definitions in the frame |

The unchanged s41/s42 baseline rows are measured before an idle embed pump:
they are the guest-unloaded path promised by “a file with no embed pays
nothing,” even when a frozen source happens to contain a newly recognized
host site.  The four fixtures introduced above are then pumped and own the
resident-guest budgets.  Because Make's line-continuation bridge is distinct
from those four shapes, the original Make fixture is also pumped *after* its
legacy timings and remeasured as `make_embed_resident`; its line median/p99 and
200-row viewport retain the 3.0 µs / 12 µs / 1.5 ms hard ceilings.  This split
preserves the old baselines without hiding resident host/guest work.

### 10. Defer

- Host/guest interleaving (PHP, ERB, Jinja, Vue SFC), open-ended
  `lang=`/`type=` dispatch, guessing a fence's language from its content,
  and multiple `end: "line"` guests on one line → **post-1.0**, each named
  in the definition file that would host it. An `interleave:` key is a
  compile error naming this list (invariant 3).
- Per-embedded-region options, `commentstring`, indentation, folding, and
  LSP routing → **post-1.0**; `ed.syn.status` reports the active
  definition so the gap is visible rather than silent. Semantic
  highlighting from LSP → **post-1.0**, layered per buffer rather than per
  region; the later binding Sprint 46/47 contracts explicitly exclude it
  from 1.0.
- This sprint originally deferred languages beyond its 16 families. The later
  binding Sprint 42.5 contract pulls zsh, fish, lua, ruby, sql, nix, zig,
  haskell, and further named modes into the built-in pack; PHP and mixed
  template interleaving remain post-1.0. Definitions remain data: dropping a
  `.fl` into `$XDG_CONFIG_HOME/yew/syntax/` makes it fenceable in Markdown
  without a rebuild or capability prompt.
- Reference-hardware calibration of §9 → **Sprint 56** (s56 §9 owes this
  sprint its budget row by name). Raising `YEW_SYN_DEF_MAX` above 4 or
  `YEW_SYN_DEPTH_MAX` above 16 → post-1.0, and only with a state-count
  measurement; both are `_Static_assert`ed against `sizeof(SynState)`.

## Testing Strategy

- **Unit** (`tests/unit/test_syn_state2.c`): `sizeof(SynFrame) == 4`,
  `sizeof(SynState) == 84`; canonicalization — 10 000 random
  push/pop/embed/exit sequences returning to the same logical state return
  the same `u32`, and an un-normalized build fails; two states differing
  only in `f[0].def` intern differently; `ndef`/`aux`-slot lifecycle
  across nested embeds; the §3 refusal table row by row, including that a
  refused embed's exit rule still balances the stack.
- **Unit** (`tests/unit/test_syn_embed.c`), driven by a hand-built pair of
  toy definitions in the s39 §8 style so the tests do not depend on the
  shipped langpack: every §2 key; `defer` on and off; `@self`; `embed.ctx`
  entering away from the root; both `end` modes; exit-rule precedence
  against the guest's `at_eol`; the merged first-byte mask brute-forced
  over all 256 bytes at every embed site (a deliberately narrowed mask
  fails).
- **Unit** (`tests/unit/test_syn_embed_pump.c`): a fake definition loader
  with a counter — a pending embed resolves in exactly one pump; a
  400-fence file performs ≤ 1 load per settle call and **never** loads
  inside the wave (the quadratic guard); an unresolvable name never
  retries; loads are charged to the idle budget, never the keystroke one.
- **Differential** (extends `test_syn_diff.c`): the s39 oracle with
  embedding fixtures — N random edits, settle to fixpoint, compare the
  full `entry` array **and** every line's spans against a from-scratch
  highlight. Edits are targeted at the boundary lines (open fence, close
  fence, `</script>`, the `)` of a substitution) because that is where a
  stale bridge frame hides.
- **Golden spans** (`tests/syn/embed/`): a fence per shipped language; a
  fence whose info string names an uninstalled language (must be `code`,
  no error); nested fences of different run lengths with code inside;
  ` ``` ` inside a `~~~` block; `</script>` inside a JS string (asserting
  the *documented* behavior); `<script type="module">` and an unknown
  `type`; a make recipe with a `\`-continuation and a `#`; `$( "$(…)" )`;
  a heredoc inside `$(…)` inside a heredoc; JSX with expression children
  and a nested element; `` html`<b>${x}</b>` ``. Plus the full s41/s42
  corpus regenerated with qualified names.
- **Regression — nothing without an embed changes.** The s41/s42 golden
  corpus is regenerated once for the name qualification, and a script
  asserts the regeneration changed **only** the context-name column: same
  span count, same offsets, same attrs, every file. This is what separates
  "the dump format changed" from "the highlighting changed".
- **Fuzz**: s39's line fuzzer gains embedding toy definitions (random
  `embed` targets including self-reference and cycles) — assert forward
  progress, `depth ∈ [1,16]`, `ndef ∈ [1,4]`, balanced exit, no state id
  out of range, no `syn_ctx` bounds trip, 5 s alarm never fires. s40's
  definition fuzzer gains the `embed` keys. 10 minutes each, ASan/UBSan.
- **Pty goldens**: `md_embed.md` at 80×24 in both themes (byte-exact SGR
  runs across a fence boundary); typing an opening fence at the top of a
  5 000-line document — viewport correct one frame later, `syn…` appears
  then clears; a fence naming a not-yet-resident language showing `code`
  and repainting exactly once after the pump.
- **Perf**: §9, `make perf`, baselines committed; pump-count and
  loads-per-settle assertions run under the fake clock, so they are hard
  gates, not timing flakes.
- **Determinism lane**: same fixture twice → identical `entry` arrays and
  span dumps; cold vs warm syntax cache identical; guest-load order made
  irrelevant — a run that pre-resides all 19 definitions must produce
  byte-identical output to one that lazily pumps them.

## Definition of Done

1. `make && make test` green on gcc and clang under `-std=c11 -pedantic
   -Wall -Wextra -Werror`, zero warnings; ASan/UBSan/valgrind/determinism
   lanes green.
2. `_Static_assert`s hold: `sizeof(SynFrame) == 4`, `sizeof(SynState) ==
   84`, `YEW_SYN_DEF_MAX == 4`, `YEW_SYN_DEPTH_MAX == 16` unchanged.
3. `grep -rn '\.def\b' src/syn/` matches only `SynFrame.def`; the s39
   `SynState.def` identifier does not exist anywhere in `src/`; the s39
   `YEW_BUG` on `def != 0` is gone and its replacement (DoD 4) is in.
4. **Non-embedding definitions are provably unchanged**: across the whole
   s41/s42 fixture corpus every produced state has `ndef == 1` and
   `f[i].def == root` for all `i < depth`, asserted for the 12 definitions
   with no embed site (the seven that embed are excluded by an explicit name
   table, not by weakening the check).
5. Canonicalization: 10 000 random push/pop/embed/exit round trips intern
   to the same id; a build with the tail-zeroing removed fails the test
   **and** shows the fixpoint economy collapse (s39 DoD 6's ≤ 2-line
   assertion fails), documented in the test comment.
6. One global interner: two states differing only in `f[0].def` get
   different ids; a build with per-definition interners fails the
   differential oracle (proved once and recorded in the test comment).
7. Merged first-byte masks: brute-force self-check over all 256 byte
   values × every embed site in all 19 definitions; narrowing any mask by
   one bit fails; `grep`-gated so a new embed site without a mask entry
   cannot compile.
8. Depth: the §3 table's four rows unit-tested; a markdown → html → js →
   `@self` chain refuses the fourth at `ndef == 4` with `EMBED_LOST`,
   `syn!` in the statusline, and a stack that still balances exactly;
   `yew syn check --embed` prints the combined-depth table and exits 0 for
   the shipped set.
9. Validator rows 18–24 each fire with the exact message and correct
   `file:line:col` over `tests/syn/bad/*.fl`, one diagnostic each; `yew
   syn check --strict runtime/syntax/*.fl` exits 0 with zero warnings for
   all 19; `yew syn check --coverage` reports 100 % contexts, 100 % rules,
   **and 100 % embed sites**.
10. Laziness: opening `md_embed.md` compiles exactly one definition at
    open (counter asserted); the eight guests resolve in ≤ 8 idle ticks;
    `yew_syn_embed_pump` is never called from the keystroke settle path
    (counter asserted); a 400-fence file performs ≤ 400 loads total and
    ≤ 1 per settle call.
11. The re-gate is complete and reviewable: the s41/s42 golden corpus is
    regenerated in **one commit with no other change**, and the
    column-diff script proves span counts, offsets and attrs are
    byte-identical to the pre-sprint corpus — only the qualified context
    names differ.
12. Differential oracle: 100 000 random edits × 4 seeds over the embedding
    fixtures, with ≥ 25 % of edits landing on a boundary line — zero
    divergences from from-scratch highlighting.
13. Every §9 budget green with committed baselines; the guest-unloaded s41
    §10 and s42 §9 rows re-run **unchanged** on their original fixtures with
    no regression above the 1.2× trip; all dedicated resident fixtures and
    `make_embed_resident` meet their hard ceilings; cold start still ≤ 20 ms
    with 19 definitions installed.
14. Deferrals hard-error naming their target: an `interleave:` key, a
    `lang=`/`type=` value outside the closed 6-row table (falls back with
    one log line, never an error), and `YEW_SYN_DEF_MAX` raised without a
    state-count measurement (`_Static_assert`).
15. `.docs/syntax-def.md` gains the embed section — every §2 key with a
    type, a default and an example, the end-delimiter carry explained on
    the fence/heredoc pair, the §8 scope list, and the two pitfalls from
    §2 verbatim; `grep -n 'TODO\|TBD'` → empty.
16. ≥ 160 assertions across the three new unit files; ≥ 24 new golden span
    files under `tests/syn/embed/`, all byte-stable across two runs.
