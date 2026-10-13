# Sprint 20: The Regex Engine — Thompson NFA, Pike VM, Lazy DFA

## Prerequisites

- Sprint 2 — UTF-8 decode/encode, the generated Unicode tables (general
  category, Alphabetic, case-folding pairs), and the lossless invalid-byte
  policy. This sprint consumes the tables; it never ships its own.
- Sprint 7 — `TextBuf`, `TextIter` chunk iteration, `Span`, `ByteOff`.
  **The engine reads through the iterator; it never materializes a buffer.**
- Sprint 9 — grapheme boundaries (`yew_is_grapheme_boundary`) for the
  candidate-position validation rule in §7 and for zero-width advance.
- Sprint 0/1 — `Arena`, `Vec(T)`, `Bytebuf`, `yew_sort_stable`, `YEW_BUG`,
  the unit harness and CI lanes (gcc/clang/ASan/UBSan/determinism).
- Sprint 18 — `CmdErr`-style token highlighting; this sprint's `YewReErr`
  carries a pattern offset so the prompt can point at the bad construct.
- Binding: `00-decisions.md` — *"Search: bespoke regex: Thompson NFA/DFA
  hybrid, no backtracking (no catastrophic blowup — speed first), literal
  fast path via memchr/Boyer-Moore-Horspool"*; invariant 4 (latency budgets
  are CI gates), invariant 7 (bespoke first: no PCRE, no `<regex.h>`),
  invariant 8 (single-threaded).

## Goals

Land `src/search/`: pattern parser → Thompson construction → a program of
~12 opcodes, executed by a Pike VM (NFA simulation with submatch capture,
leftmost-first semantics) plus a lazy DFA for the non-capturing fast path,
with literal-prefix extraction feeding `memchr`/Boyer-Moore-Horspool. The
search API operates over `TextBuf` iterators so a match on a 1 GB file never
allocates a 1 GB string. **No backtracking exists anywhere in the
implementation**, and the pathological-pattern latency gate proves it in CI
forever. The search *UI* (incremental search, highlighting, replace) is
Sprint 21; syntax-highlighting patterns consume this engine from Sprint 39;
the Fletch `regex` handle is Sprint 34. Lookaround, backreferences, and
atomic groups are permanent 1.0 non-goals (§4) — they are not deferrals.

## Deliverables

### 1. Public API — `src/search/regex.h`

```c
typedef struct YewRe YewRe;                 /* arena-owned, immutable     */

enum { YEW_RE_ICASE    = 1u << 0,   /* simple case folding, s02 tables    */
       YEW_RE_DOTALL   = 1u << 1,   /* '.' also matches '\n'              */
       YEW_RE_LITERAL  = 1u << 2,   /* whole pattern is literal text      */
       YEW_RE_NOCAPTURE= 1u << 3 }; /* callers promise not to read groups */

typedef struct { u32 off; const char *msg; } YewReErr;   /* off: pattern  */

YewRe *yew_re_compile(Arena *a, const char *pat, size_t len,
                      u32 flags, YewReErr *err);         /* NULL on error */

#define YEW_RE_MAX_GROUPS 32
typedef struct { Span g[YEW_RE_MAX_GROUPS];   /* g[0] = whole match       */
                 u32  ngroups; } YewReMatch;

/* Input abstraction: one struct, two backings; no vtable, no allocation. */
typedef struct YewReInput {
    const TextBuf *tb;        /* NULL => raw bytes                        */
    const u8 *bytes; u64 len; /* raw form                                 */
    Span   window;            /* [lo,hi) of the buffer/bytes to consider  */
} YewReInput;

bool yew_re_search (const YewRe *re, const YewReInput *in, ByteOff from,
                    YewReMatch *out);      /* leftmost-first at/after from */
bool yew_re_search_back(const YewRe *re, const YewReInput *in, ByteOff before,
                    YewReMatch *out);      /* last match strictly before   */
bool yew_re_match_at(const YewRe *re, const YewReInput *in, ByteOff at,
                    YewReMatch *out);      /* anchored at `at`             */
bool yew_re_test   (const YewRe *re, const YewReInput *in, ByteOff from);
u32  yew_re_group_count(const YewRe *re);
```

**Law — no materialization.** Every entry point reads codepoints through a
cursor built on `yew_textiter_*`; nothing in `src/search/` calls a
"give me the whole buffer as a string" helper. A 1 GB file must be
searchable with O(1) auxiliary memory beyond the program and thread lists.
The one bounded exception is the reverse-DFA start-recovery window (§6),
which materializes at most 64 KiB and is documented at its call site.
DoD 2 greps for violations.

```c
/* The reader: decodes one codepoint, tracks the byte offset, and knows the
 * previous codepoint (needed for \b and ^ without a second pass).       */
typedef struct { TextIter it; const u8 *p; u64 n;      /* current chunk  */
                 ByteOff pos; u32 prev_cp; u32 cp; u32 cp_len; } ReCursor;
bool yew_re_cursor_next(ReCursor *c, const YewReInput *in);
```

### 2. Syntax table (1.0, complete)

| Construct | Syntax | Semantics |
|---|---|---|
| literal | any non-metachar | matches that codepoint |
| escape | `\.` `\*` `\+` `\?` `\(` `\)` `\[` `\]` `\{` `\}` `\|` `\^` `\$` `\\` `\/` | literal metachar |
| control escapes | `\n \t \r \f \v \a \e \0` | the obvious codepoints |
| hex | `\xHH`, `\x{H…}` (1–6 digits) | codepoint by value; > U+10FFFF is an error |
| any | `.` | any codepoint except `\n`; with `YEW_RE_DOTALL`, any |
| class | `[abc]`, `[a-z]`, `[^…]` | set of codepoints; `]` first is literal; `-` first/last is literal |
| class escapes | inside `[]`: `\w \d \s \W \D \S`, control/hex escapes, `\]`, `\\`, `\-` | union into the set |
| perl classes | `\w \W \d \D \s \S` | see the Unicode table below |
| POSIX brackets | `[[:alpha:]]` etc., negated `[[:^alpha:]]` | valid only inside `[]` |
| word boundary | `\b` `\B` | transition between `\w` and non-`\w` (incl. buffer edges) |
| anchors | `^` `$` | **line** anchors: start/end of any line |
| buffer anchors | `\A` `\z` | start/end of the search window |
| group | `(…)` | capturing; ≤ 32 groups incl. group 0 |
| non-capturing | `(?:…)` | no slot consumed |
| inline flags | `(?i)`, `(?-i)`, `(?s)`, `(?-s)` | apply from that point to the end of the enclosing group |
| scoped flags | `(?i:…)`, `(?s:…)` | apply to the group only |
| alternation | `a\|b` | leftmost-**first**: earlier branch wins ties |
| quantifiers | `*` `+` `?` `{m}` `{m,}` `{m,n}` | greedy |
| lazy | `*?` `+?` `??` `{m,n}?` | prefer fewer |

POSIX bracket classes (all 12, mapped onto s02 tables — no ASCII-only
shortcuts):

| Name | Definition |
|---|---|
| `alpha` | Alphabetic |
| `digit` | general category Nd |
| `alnum` | Alphabetic ∪ Nd |
| `upper` / `lower` | Uppercase / Lowercase |
| `space` | White_Space |
| `blank` | space or tab (U+0009, U+0020, and Zs) |
| `punct` | categories P* and S* |
| `cntrl` | category Cc |
| `graph` | assigned, non-space, non-Cc |
| `print` | graph ∪ U+0020 |
| `xdigit` | `[0-9A-Fa-f]` (ASCII by definition) |
| `word` | `\w` (below) |

`^`/`$` are **line** anchors unconditionally — there is no `MULTILINE`
flag. Why: this is an editor. A user typing `^foo` in the search prompt
means "at the start of a line" 100% of the time, and a flag that changes
the meaning of the most common anchor is a trap. `\A`/`\z` cover the
buffer-anchored case for scripts. `$` matches before an `\n` and at the
window end; with a CRLF buffer (s08 keeps `\r\n` verbatim) `$` also matches
before the `\r` of a `\r\n` pair — pinned, else every anchor fails on
Windows files.

### 3. Unicode policy (pinned)

- **Patterns and inputs operate on codepoints, not bytes and not grapheme
  clusters.** One VM step consumes one codepoint: `é` written as `e` +
  U+0301 is two codepoints and `.` matches only the `e`. This is standard
  for codepoint engines, and the alternative (cluster-stepping) breaks
  `\b`, classes, and the DFA simultaneously. Say so in the docs.
- Invalid UTF-8 in the *input* decodes to one synthetic codepoint per
  invalid byte in U+DC80–U+DCFF (surrogate-escape, matching s02's lossless
  policy). They match `.` and negated classes, never `\w`/`\d`/`\s`.
  Consequence: a search never crashes on a binary file and never mangles it.
- `YEW_RE_ICASE` uses **simple** folding from the s02 tables, applied at
  compile time (a literal `k` becomes a CLASS of {k, K, U+212A}). **Full
  folding is out** (`ß` → `ss`): a length-changing fold breaks the
  one-codepoint-per-step invariant that makes the VM linear. Documented as
  the one place we differ from "expected" case-insensitivity.
- No locale, ever — `setlocale` is never called; the dotless-i pair folds
  per the simple default mappings. No normalization either: `é` (NFC) does
  not match `é` (NFD), because we deliberately have not built a normalizer.
- `\w` = Alphabetic ∪ Nd ∪ Pc (includes `_`); `\d` = Nd; `\s` = White_Space;
  `\b` is defined off `\w`. Consequence worth testing because it surprises
  people: in `漢字foo` there is no `\b` between `字` and `f` — both are
  Alphabetic.

### 4. What is out of 1.0 — and why that is a feature

| Construct | Status | Why |
|---|---|---|
| lookahead `(?=…)` `(?!…)` | **out, permanent** | see below |
| lookbehind `(?<=…)` `(?<!…)` | **out, permanent** | same |
| backreferences `\1` in the *pattern* | **out, permanent** | matching with backrefs is NP-hard; no linear-time algorithm exists |
| atomic groups `(?>…)`, possessive `a*+` | **out, permanent** | they only mean something to a backtracker |
| recursion, conditionals, `\G`, `\K` | **out, permanent** | ditto |
| named groups `(?<name>…)` | out for 1.0 | no consumer yet; purely additive later |
| byte-level (non-UTF-8) mode | out for 1.0 | post-1.0 perf option (§6) |

The linear-time guarantee is the product feature: **no pattern a user can
type into the search prompt can hang the editor.** Incremental search
(Sprint 21) recompiles and re-scans on *every keystroke*; with a
backtracking engine, typing `(a+)+` while the cursor sits in a file of a's
freezes the editor mid-keystroke and the 5 ms p99 gate becomes a lie. A
Pike VM is O(n·m) worst case, always, with no pattern-dependent cliff.
Lookaround is implementable in an NFA simulation only by nesting
simulations, whose cost is not O(n·m) and whose interaction with capture
priority is a research problem. We trade three constructs for a hard
latency bound; `yew_re_compile` rejects them with `lookaround is not
supported (yew's regex engine is linear-time by design; see man
yew-regex)` — not a "Sprint N" message, because it is never coming.

### 5. Program and opcodes — `src/search/regex.c`

```c
typedef enum {
    RE_CHAR,      /* arg = codepoint; match and advance                  */
    RE_CLASS,     /* arg = class index; match if cp ∈ class; advance     */
    RE_ANY,       /* any cp (arg = 1 to also match '\n')                 */
    RE_SPLIT,     /* try x first, then y — priority defines greediness   */
    RE_JMP,       /* pc = x                                              */
    RE_SAVE,      /* arg = slot (2*g start, 2*g+1 end); no input consumed*/
    RE_BOL, RE_EOL,      /* line anchors (CRLF-aware, §2)                */
    RE_BOT, RE_EOT,      /* window anchors \A \z                         */
    RE_WORDB, RE_NWORDB, /* \b \B                                        */
    RE_MATCH      /* accept                                              */
} ReOp;

typedef struct { u8 op; u32 x, y, arg; } ReInst;
_Static_assert(sizeof(ReInst) <= 16, "inst bloat");

struct YewRe {
    ReInst  *prog; u32 nprog;      /* forward program                    */
    ReInst  *rprog; u32 nrprog;    /* reverse program (§6)               */
    ReClass *classes; u32 nclasses;/* sorted, non-overlapping intervals  */
    u32      ngroups;
    ReLit    lit;                  /* literal prefilter (§7)             */
    u32      flags;
    u32      min_len;              /* min codepoints a match consumes    */
};
```

Thompson construction, standard, non-recursive over the AST:

| AST node | Emitted |
|---|---|
| literal `c` | `CHAR c` |
| class | `CLASS k` |
| `e1 e2` | code(e1) then code(e2) |
| `e1\|e2` | `SPLIT L1, L2`; L1: code(e1); `JMP end`; L2: code(e2) |
| `e*` (greedy) | L: `SPLIT body, end`; body: code(e); `JMP L` |
| `e*?` (lazy) | L: `SPLIT end, body`; body: code(e); `JMP L` |
| `e+` | body: code(e); `SPLIT body, end` (lazy: operands swapped) |
| `e?` | `SPLIT body, end` (lazy: swapped); body: code(e) |
| `(e)` group g | `SAVE 2g`; code(e); `SAVE 2g+1` |
| `e{m,n}` | m copies of code(e), then (n−m) copies of `e?`; `{m,}` → m copies then `e*` |

**Leftmost-first (Perl-style) is pinned**, not leftmost-longest (POSIX):
`a|ab` against `"ab"` matches `a`. Why: users write alternations expecting
order to matter, and every incremental-search UI they have used behaves
this way. The VM implements it by *thread priority* — threads are added in
program order and the first `RE_MATCH` reached kills all lower-priority
threads while leaving higher-priority ones alive.

Limits (each produces a compile error naming itself):

| Limit | Value |
|---|---|
| `YEW_RE_MAX_GROUPS` | 32 |
| `YEW_RE_MAX_PROG` | 4096 instructions |
| `YEW_RE_MAX_REPEAT` | 1000 (`{m,n}` bound) |
| `YEW_RE_MAX_CLASSES` | 256 |
| pattern length | 64 KiB |

`{m,n}` expands by copying, so the program-size limit is what actually
bounds `(a{100}){100}` — checked *during* emission, not after, so a
pathological pattern never allocates 10 MB of program before being
rejected.

### 6. Execution engines

Three engines, one dispatcher. The dispatcher is a table, not a heuristic
pile:

| API call | Engine |
|---|---|
| `yew_re_test` | literal prefilter → lazy DFA |
| `yew_re_search`, `NOCAPTURE` or `ngroups == 1` | prefilter → forward DFA for the end, reverse DFA for the start |
| `yew_re_search` with groups | prefilter → forward DFA to locate a match end → Pike VM anchored in the recovered window |
| `yew_re_match_at` | Pike VM, anchored (captures free) |
| `yew_re_search_back` | §6c |

**(a) Pike VM — `src/search/pike.c`.** Two thread lists (`clist`, `nlist`),
each a sparse set over pc values (dense array + sparse index → O(1)
membership, no clearing between steps — the classic trick, and it is what
makes per-step cost O(m) not O(m log m)). Each thread carries a capture
array; arrays are refcounted and copy-on-write so a `SAVE` on a shared
array copies once instead of every thread copying at every step.

```c
static void addthread(ReList *l, u32 pc, Caps *c, const ReCtx *x) {
    /* Explicit stack, NOT recursion: (a*)* can chain thousands of
     * SPLIT/JMP with no input consumed, and a recursive addthread blows
     * the C stack on a pattern the user typed. */
    push(pc, c);
    while (pop(&pc, &c)) {
        if (sparse_has(l, pc)) { caps_release(c); continue; }  /* THE fix */
        sparse_add(l, pc);
        switch (prog[pc].op) {
        case RE_JMP:   push(prog[pc].x, c); break;
        case RE_SPLIT: push(prog[pc].y, caps_share(c));        /* lower  */
                       push(prog[pc].x, c);            break;  /* higher */
        case RE_SAVE:  c = caps_set(c, prog[pc].arg, x->pos);
                       push(pc + 1, c);                break;
        case RE_BOL: case RE_EOL: case RE_BOT: case RE_EOT:
        case RE_WORDB: case RE_NWORDB:
            if (assertion_holds(prog[pc].op, x)) push(pc + 1, c);
            else caps_release(c);
            break;
        default: /* consuming op: stays in the list */ break;
        }
    }
}
```

**Pitfall — the empty-width loop.** Without the `sparse_has` check *inside*
`addthread` (not merely before the call), `(a*)*` re-adds the same pc
forever at a single input position and the editor hangs on a pattern with
no input at all. This one line is the difference between "linear-time
engine" and "linear-time engine that hangs"; it gets a dedicated unit test
that fails if the check is moved.

Priority-cut rule, pinned: when a thread reaches `RE_MATCH`, record its
captures as the current best and **stop processing the remainder of
`clist`** (all lower-priority threads), but let already-added higher-
priority threads in `nlist` continue. That is exactly leftmost-first.

Complexity: time O(n·m), space O(m·k) where k = capture slots. Both are
bounded by the §5 limits, so a single search has a computable worst case:
4096 instructions × 32 slots.

**(b) Lazy DFA — `src/search/dfa.c`.** States are sets of NFA pcs plus the
assertion context bits (at-BOL, at-EOL, prev-is-word), built on demand and
cached. Transitions are keyed by **codepoint**: a 128-entry direct table
for ASCII plus a sorted interval list (binary search) for the rest. A
byte-level DFA (RE2-style) is the post-1.0 perf option — faster and much
fiddlier; the codepoint DFA is already ~10× the Pike VM on plain text.

Cache policy: `YEW_DFA_MAX_STATES` = 1024 states / 1 MiB, whichever first.
On overflow **flush the whole cache** and continue (state IDs are indices,
so a flush is a memset plus a generation bump). If a single search flushes
twice, abandon the DFA and finish on the Pike VM — a pattern that thrashes
the cache runs *slower* on a DFA rebuilding every state per character.

**(c) Finding the start: the reverse program.** A forward DFA reports where
a match *ends*, not where it starts. Instead of a second engine we compile
the AST again with concatenation order flipped (`rprog`; anchors swap:
`RE_BOL`↔`RE_EOL`, `RE_BOT`↔`RE_EOT`, `\b` unchanged) and run the reverse
DFA backwards from the reported end. Backward scanning needs bytes in
reverse, which the forward-only `TextIter` does not provide: we materialize
a **bounded window** of ≤ 64 KiB ending at the match end
(`yew_re_window_fill`), scan it backwards, and if no start is found inside
it fall back to a Pike VM run anchored at the window's first line start.
This is the engine's single materialization and it is O(64 KiB), never
O(file).

`yew_re_search_back` (used by `?pattern` and `N`) has no reverse engine
either: it scans **forward** inside `[w_lo, before)`, where `w_lo` walks
backwards in 256 KiB steps snapped to line starts, keeping the last match
and stopping at the first window that yields one. Worst case on a
match-less buffer is one full forward pass — documented, and Sprint 21
budgets the scan so the UI reports rather than freezes.

### 7. Literal prefilter — `src/search/literal.c`

```c
typedef enum { RE_LIT_NONE, RE_LIT_BYTE, RE_LIT_BMH, RE_LIT_WHOLE } ReLitKind;
typedef struct ReLit { u8 kind; u8 s[32]; u32 n; u32 skip[256]; bool anchored; }
    ReLit;

u64  yew_lit_find(const ReLit *l, const u8 *hay, u64 n);   /* offset or ~0 */
void yew_bmh_build(ReLit *l);            /* 256-entry skip table          */
```

Extraction from the AST: walk the leading concatenation collecting literal
`CHAR` nodes until a non-literal node, a quantifier with min 0, or an
alternation is reached. Encode the collected codepoints as UTF-8 bytes.

| Prefix | Prefilter |
|---|---|
| ≥ 2 bytes, ≤ 32 | Boyer-Moore-Horspool with the 256-entry skip table |
| exactly 1 byte | `memchr` — libc's is SIMD-optimized and is the fastest thing we are allowed to call (stdlib is not a dependency) |
| whole pattern literal (`YEW_RE_LITERAL` or no metachars) | BMH/memchr only; the VM is never entered |
| none extractable | no prefilter; the DFA scans |

Rules, pinned:

- The prefilter runs on **bytes**, one `TextIter` chunk at a time, with a
  carry of `n-1` bytes across chunk boundaries so a literal spanning two
  pieces is still found. **Pitfall:** forgetting the carry misses matches
  only on files whose piece boundaries fall inside a match — i.e. only
  after editing, only sometimes, and never in a unit test built from one
  contiguous buffer. The differential fuzzer builds its buffers by random
  insertion specifically to hit this.
- With `YEW_RE_ICASE`, extract a prefilter only for an ASCII prefix (folded
  BMH table); otherwise none. Never a *wrong* skip — a prefilter that can
  miss a match is a correctness bug, not a perf regression.
- A candidate may land **mid-UTF-8-sequence** (BMH knows no encoding).
  Reject candidates that are not codepoint boundaries before the anchored
  match — cheap (`(b & 0xC0) != 0x80`), and omitting it produces phantom
  matches inside multibyte text.

### 8. Errors

`YewReErr.off` is a byte offset into the pattern, so Sprint 18's prompt can
highlight the exact construct and Sprint 21's incremental search can show
the caret while typing.

| Condition | Message |
|---|---|
| unterminated `[` | `unterminated character class` |
| unterminated `(` | `unterminated group` |
| stray `)` | `unmatched )` |
| nothing to repeat | `nothing to repeat` |
| `{2,1}` | `invalid repeat: min 2 > max 1` |
| `{0,2000}` | `repeat count 2000 exceeds 1000` |
| trailing `\` | `trailing backslash` |
| unknown escape | `unknown escape '\q'` |
| bad `\x{…}` | `invalid codepoint escape` |
| POSIX class outside `[]` | `[:alpha:] is only valid inside [...]` |
| unknown POSIX class | `unknown POSIX class '[:foo:]'` |
| > 32 groups | `too many capture groups (max 32)` |
| program too large | `pattern too complex (program limit 4096)` |
| lookaround / backref / atomic | `lookaround is not supported (yew's regex engine is linear-time by design; see man yew-regex)` and analogues |

### 9. Defer

- Search UI, highlighting, replace, jumplist → **Sprint 21**.
- Syntax-definition patterns compiled through this engine → **Sprint 39/40**
  (a def whose pattern fails to compile errors at load with `YewReErr.off`
  pointing into the def source).
- Fletch `regex` handle and `buf.find(re)` → **Sprint 34**.
- Byte-level DFA, prefix-accelerated DFA start states, SIMD class matching
  → post-1.0 performance work (Sprint 56 may open one of them if a gate
  demands it; not before).
- Named groups → post-1.0, additive.
- Lookaround/backrefs/atomic groups → **never** (§4).

## Testing Strategy

- **Unit** (`tests/unit/test_re_parse.c`): every row of the §2 syntax table
  compiles and produces the expected program shape (spot-checked by opcode
  sequence for ~15 canonical patterns); every row of the §8 error table
  fires with the correct `off`.
- **Unit** (`tests/unit/test_re_match.c`): a golden match table — ≥ 150
  rows of (pattern, input, flags, expected match span, expected captures) —
  covering greedy vs lazy, leftmost-first alternation (`a|ab` → `a`),
  `{m,n}` boundaries, all 12 POSIX classes, `\b` at buffer edges and
  between CJK/ASCII/emoji, `^`/`$` on LF and CRLF buffers, `\A`/`\z`,
  nested groups, empty-alternative `(|a)`, and zero-width matches.
- **Unit** (`tests/unit/test_re_unicode.c`): ICASE folding table (ASCII,
  `k`/K/U+212A, Greek sigma final/medial, dotless i pairs); invalid-byte
  inputs (the U+DC80 escapes match `.` and never `\w`); a match spanning
  three pieces of a piece tree built by interleaved 1-byte inserts (the
  chunk-carry case from §7).
- **Unit** (`tests/unit/test_re_engines.c`): engine agreement — for every
  golden row, the Pike VM, the DFA (`yew_re_test`), and the forward+reverse
  DFA span must agree. Disagreement is a hard failure, not a tolerance.
- **Fuzz** (`tests/fuzz/fuzz_re_compile.c`): random bytes and
  random-but-plausible pattern strings → `yew_re_compile` never crashes,
  never loops, and on failure `err.off <= len`.
- **Fuzz** (`tests/fuzz/fuzz_re_diff.c`) — the differential campaign:
  (1) a random pattern generator over the §2 grammar (weighted,
  depth-capped, always terminating) emitting pattern text plus its AST;
  (2) random inputs drawn from the pattern's own alphabet (so matches are
  frequent), plus s02 corpus slices and invalid-byte noise;
  (3) the oracle `tests/fuzz/re_ref.c` — a deliberately simple **recursive
  backtracking** matcher over the overlapping semantic subset (literals,
  classes, `.`, anchors, groups, alternation, greedy/lazy quantifiers,
  `{m,n}`) with leftmost-first semantics and a 10⁶-step budget; it is
  allowed to be exponential because it is the oracle, not the product, and
  budget-exceeded cases are *skipped*, not failed;
  (4) compare match/no-match, match span, and every capture span, plus
  `YEW_RE_NOCAPTURE` booleans and DFA-vs-VM spans, running each case
  against both a contiguous byte input and a piece tree built by random
  insertions. Seeded; failing seeds are printed and committed as fixtures.
- **Perf / latency gate** (`tests/perf/re_pathological.c`) — the
  no-backtracking proof, in CI:

  | Pattern | Input | Assertion |
  |---|---|---|
  | `a(a*)*b` | `a`×10³, ×10⁴, ×10⁵ | each ≤ budget; `t(10⁵)/t(10⁴) ≤ 15` |
  | `(a\|a)*b` | same | same |
  | `(a+)+b` | same | same |
  | `a?{n}a{n}` (n=25) | `a`×25 | ≤ 5 ms (the classic exponential case) |
  | `.*.*.*.*=.*` | 10 KiB line, no `=` | ≤ budget |

  Absolute budget: 100 000 a's against `a(a*)*b` in ≤ 50 ms on the CI
  runner (calibrated in Sprint 56). Baselines committed under
  `tests/perf/baselines/`.
- **Perf** (`tests/perf/re_throughput.c`): literal search over a 1 GB
  generated file ≥ 500 MiB/s (memchr path) and a non-literal pattern
  ≥ 50 MiB/s (DFA path); memory high-water during the 1 GB search < 4 MiB.
- Lanes: gcc/clang zero-warning, ASan/UBSan, valgrind on the unit binaries,
  determinism (same seed → identical fuzz trace and identical results).

## Definition of Done

1. `make test` green on both compilers, zero warnings; ASan/UBSan/valgrind/
   determinism lanes green.
2. No materialization: `grep -rn 'textbuf_to_bytes\|materialize' src/search/`
   shows only `yew_re_window_fill`'s bounded 64 KiB window with its
   documented comment; the 1 GB throughput test's peak RSS delta < 4 MiB.
3. **No backtracking anywhere**: `grep -rn 'backtrack' src/search/` returns
   only comments explaining its absence; the pathological table is green
   with `t(10⁵)/t(10⁴) ≤ 15` on all four scaling patterns, and
   `a(a*)*b` × 100 000 a's completes in ≤ 50 ms against a committed
   baseline.
4. Golden match table ≥ 150 rows green with captures compared
   element-wise; leftmost-first pinned by explicit rows.
5. Engine agreement: Pike VM, DFA test, and forward+reverse DFA spans agree
   on 100% of golden rows and 100% of fuzz cases.
6. Differential fuzz: ≥ 5 000 000 (pattern, input) pairs × 4 seeds, zero
   mismatches; oracle-budget skip rate reported and < 2%.
7. Compile fuzz: 1 000 000 inputs, zero crashes, `err.off` always in range.
8. Every §8 error row fires with the exact message and offset;
   lookaround/backref/atomic give the permanent-non-goal message, **not** a
   sprint deferral.
9. Empty-width protection: the `(a*)*` unit case completes; moving the
   `sparse_has` check out of `addthread` makes it fail (documented in the
   test comment).
10. Chunk-carry: a match spanning three pieces of an insertion-built piece
    tree is found, and mid-UTF-8 prefilter candidates are rejected.
11. `_Static_assert(sizeof(ReInst) <= 16)`; all §5 limits enforced during
    emission, each with a firing test.
12. DFA cache overflow flushes rather than grows, and a thrashing pattern
    falls back to the Pike VM after two flushes (test with a lowered cap).
13. ≥ 300 assertions across the four unit files.
