# Sprint 29: Fletch Lexer, Parser, AST, Diagnostics

## Prerequisites

- Sprint 28 — `.docs/fletch-spec.md` frozen. This sprint implements
  spec §1 (lexical), §2 (grammar), §3 (motion tokens, lexing side only),
  §12 (pure-literal mode). Cite sections in code comments, not this file.
- Sprint 0 — `Arena`, `Vec(T)`, `Interner`, `Bytebuf`; the diag module.
  Assumption (s00's entry says "diag/log module" without the API): a
  `DiagCtx` with file registration, `diag_emit(dc, level, span, fmt, …)`,
  caret rendering `path:line:col: error: msg` + source line + `^~~~`.
  If the landed names differ, adapt here — the *rendering contract* is
  what's binding.
- Sprint 1 — unit harness + CI lanes. Sprint 2 — UTF-8 decode (arrow-glyph
  aliases are multibyte; identifiers stay ASCII for 1.0 per spec §1).
- Fuzz harness pattern: s2 landed the decoder fuzzer; reuse its driver
  shape (stdin bytes → one run; corpus dir; ASan lane).

## Goals

Land `src/fl/lex.c|h`, `src/fl/ast.h`, `src/fl/parse.c|h`: the full token
inventory including the motion sublanguage, a recursive-descent +
Pratt-expression parser producing an arena-allocated AST, yew-quality
caret diagnostics with expected/found wording, panic-mode error recovery
(one typo → one error), the pure-literal entry point for data files, an
s-expression AST dumper for golden tests, and lexer/parser fuzzers. No
evaluation of any kind: compile/VM land in Sprint 30, stdlib names resolve
in Sprint 31, the REPL in Sprint 32. `yew fl` does not exist yet — the
test surface is the unit harness plus a tiny internal dump tool.

## Deliverables

### 1. Token inventory — `src/fl/lex.h`

```c
typedef struct { u32 file_id; u32 line; u32 col; u32 len; } FlSpan; /* 1-based; col in bytes */

typedef enum FlTokKind {
    /* literals/names */  FL_T_INT, FL_T_FLOAT, FL_T_STRING, FL_T_IDENT,
    /* punctuation    */  FL_T_LPAREN, FL_T_RPAREN, FL_T_LBRACKET, FL_T_RBRACKET,
                          FL_T_LBRACE, FL_T_RBRACE, FL_T_COMMA, FL_T_COLON,
                          FL_T_SEMI, FL_T_DOT, FL_T_ATBRACKET, /* "@[" one token */
    /* operators      */  FL_T_PLUS, FL_T_MINUS, FL_T_STAR, FL_T_SLASH,
                          FL_T_PERCENT, FL_T_EQ, FL_T_EQEQ, FL_T_BANGEQ,
                          FL_T_LT, FL_T_LE, FL_T_GT, FL_T_GE,
    /* keywords (22)  */  FL_T_AND, FL_T_AS, FL_T_BREAK, FL_T_CATCH, /* … */ FL_T_WHILE,
    /* layout         */  FL_T_NEWLINE, FL_T_EOF,
    /* motion space   */  FL_M_COUNT, FL_M_UNIT,      /* payload: 'l','w','b','c' */
                          FL_M_ARROW,                 /* payload: '<','>','^','v' + alt flag */
                          FL_M_H, FL_M_LPAREN, FL_M_RPAREN,
                          FL_M_INSERT,                /* i"…" — string payload */
                          FL_M_DEL, FL_M_ESC, FL_M_WORD, FL_M_END, /* "]" */
    FL_T_ERROR,
} FlTokKind;

typedef struct {
    FlTokKind kind;
    FlSpan    sp;
    union { i64 i; double f; u32 str_id;      /* interned string/ident */
            struct { u8 ch; bool alt; } m;    /* unit/arrow payload */
            u32 count; } v;
} FlTok;

typedef struct FlLexer FlLexer;
void  fl_lex_init(FlLexer *lx, Arena *a, DiagCtx *dc, Interner *in,
                  const char *src, size_t len, u32 file_id);
FlTok fl_lex_next(FlLexer *lx);
```

Rules (all from spec §1; the table below is the implementation checklist):

| Surface | Rule | Pitfall |
|---|---|---|
| comments | `#` → end of line; the newline still emits `FL_T_NEWLINE` | comment must not swallow the terminator |
| ints | dec, `0x`, `_` separators; overflow past i64 = error at lex | `_` legal only between digits (`1_`, `_1`, `0x_` are errors) |
| floats | `d.d[e±d]`; `1.` and `.5` are errors | `1.foo` must lex as INT DOT IDENT (method-ish on int), so only consume `.` when a digit follows |
| strings | escapes `\n \t \r \\ \" \0 \xNN \u{…}`; unknown escape = error with caret **on the escape**, not string start | `\u{}` and `\u{>10FFFF}` and surrogates are errors; encode via s2 `utf8_encode` |
| keywords | perfect-hash or length-bucketed strcmp over the 22 words | `letx` is an IDENT — check terminator |
| `@[` | single token; lexer pushes motion mode | `@` alone is an error naming `@[` |
| motion mode | entered at `@[`, left at matching `]` (`FL_M_END`); `H(`…`)` nests without leaving motion mode | depth counter, not boolean — `H(` inside `@[` |
| motion words | `l w b c` → UNIT; `< > ^ v` → ARROW; `a` immediately before an arrow sets `alt`; `→←↑↓` (UTF-8) decode to ARROW aliases; digits → COUNT (u32, cap 65535, 0 = error); `i"…"` → INSERT (string rules apply); `del`/`esc` keywords; other identifiers → `FL_M_WORD` (interned) | whitespace-insensitive: `4>` and `4 >` identical; `av` is alt-down but `avx` is WORD `avx` — longest-match with terminator check |
| newlines | always emitted; the **parser** decides significance | lexer stays stateless about continuations; CRLF: `\r\n` is one NEWLINE, bare `\r` an error |

`FL_T_ERROR` tokens carry an already-emitted diagnostic; the lexer never
prints, never exits — it reports through `DiagCtx` and continues at the
next codepoint (invalid UTF-8 in source: one error per run, resync at next
valid boundary using s2's decoder).

### 2. AST — `src/fl/ast.h`

Arena-allocated, no destructor, `FlSpan` on every node. ~26 kinds:

```c
typedef enum {
    FL_A_LET, FL_A_ASSIGN, FL_A_FN, FL_A_MACRO, FL_A_IMPORT,
    FL_A_IF, FL_A_WHILE, FL_A_FOR, FL_A_RETURN, FL_A_BREAK, FL_A_CONTINUE,
    FL_A_EDIT, FL_A_TRY, FL_A_EXPR_STMT, FL_A_BLOCK,
    FL_A_BINOP, FL_A_UNOP, FL_A_CALL, FL_A_INDEX, FL_A_FIELD,
    FL_A_IDENT, FL_A_LIT,            /* FlLit tagged: nil/bool/int/float/str */
    FL_A_LIST, FL_A_MAP, FL_A_FN_EXPR,
    FL_A_MOTION_BLOCK, FL_A_MOTION,  /* one word: count + kind + payload */
} FlAstKind;

typedef struct FlNode FlNode;
struct FlNode {
    u8     kind;                      /* FlAstKind */
    FlSpan sp;
    union {
        struct { u32 name; FlNode *init; }                      let;
        struct { FlNode *tgt, *val; }                           assign;
        struct { u32 name; u32 *params; u32 nparams;
                 FlNode *body; }                                fn;
        struct { FlNode *cond, *then, *els; }                   ifs;
        struct { u32 var, var2; FlNode *iter, *body; }          fors;
        struct { FlNode *l, *r; u8 op; }                        bin;
        struct { FlNode *callee; FlNode **args; u32 nargs; }    call;
        struct { FlNode **items; u32 n; }                       list;   /* also BLOCK, MOTION_BLOCK */
        struct { FlNode **keys, **vals; u32 n; }                map;
        struct { u32 count; u8 mkind; u8 ch; bool alt;
                 u32 payload; }                                 motion; /* payload: intern id or string id */
        /* … one arm per kind; keep FlNode ≤ 48 bytes, _Static_assert */
    } as;
};
```

Child arrays are arena-allocated at final size (count first, then fill) —
never realloc'd, so parent pointers into them are stable. Pitfall: the
same discipline Cgfried s17 pinned; growing arrays dangle sibling refs.

### 3. Parser — `src/fl/parse.c|h`

```c
typedef struct { FlNode **stmts; u32 n; bool incomplete; } FlProgram;
FlProgram fl_parse(Arena *a, DiagCtx *dc, Interner *in,
                   const char *src, size_t len, u32 file_id);
FlNode   *fl_parse_literal(Arena *a, DiagCtx *dc, Interner *in,
                           const char *src, size_t len, u32 file_id);
```

- Recursive descent for statements, precedence-climbing (Pratt) for
  expressions with the spec §2 ladder: `or < and < ==/!= < rel < +- <
  */% < unary < postfix`. Left-assoc throughout; no right-assoc ops exist.
- Newline handling: the parser tracks bracket depth from the token stream;
  `FL_T_NEWLINE` is a TERM at depth 0 unless the previous token is a
  binary operator, `,`, or `=` (spec §1 continuation rule). Inside
  `( [ { @[`, newlines are skipped. One function
  (`static bool nl_terminates(Parser*)`) owns this — the s32 REPL reuses
  the parser's `incomplete` flag, so this logic must live here, once.
- `incomplete` is set only when EOF arrives with depth > 0 or after a
  continuation token **and no error was emitted** — the REPL's multiline
  detection. Pitfall: an actual syntax error inside an open bracket must
  set `had_error`, not `incomplete`, or the REPL loops forever.
- Depth cap: 256 nested brackets/blocks → error `"nesting too deep"`
  (recursion bound; the fuzzer will find unbounded recursion otherwise).
- `fl_parse_literal`: separate entry, spec §12 grammar only. Shares the
  lexer; rejects any token outside the literal set with `"pure-literal
  mode: <found> not allowed"`. Never touches identifier resolution, never
  allocates closures — the "grants nothing, runs nothing" property is
  structural: the function cannot even *represent* a call.

### 4. Diagnostics: expected/found + recovery

- Wording contract (goldens pin it): `expected ')', found 'let'` — token
  spellings quoted, keywords as themselves, categories for literals
  (`found integer`). Every parse error names both.
- Special-cased messages (each a fixture): `{` at statement start (spec
  §1 wording); `@` without `[`; `=` at expression position → `"'=' is a
  statement; did you mean '=='?"`; motion word outside `@[ ]`.
- Panic-mode recovery: on error, discard tokens until a sync point —
  `; NEWLINE(depth 0) } ] EOF` or a statement keyword (`let fn if while
  for return break continue import macro edit try catch else`) — then
  resume. One synthetic rule: after syncing to `}`/`]`, consume it only if
  it closes an open construct. Target: one diagnostic per actual mistake;
  the classic cascade fixture (missing `)` before a 10-line block) must
  produce exactly 1 error.
- Cap: 20 errors then `"too many errors, giving up"` (fuzz hygiene).

### 5. AST dumper — golden-test surface

`void fl_ast_dump(Bytebuf *out, const FlProgram *p, const Interner *in);`
— deterministic s-expressions, one stmt per line:
`(let total (lit int 0))`, `(motion-block (motion 2 arrow v) (motion 1
ins "!") …)`. No pointers, no addresses. This is the parser's only output
until s30 and the substrate for every golden below.

### 6. Fuzzers — `tests/fuzz/fuzz_fl_lex.c`, `tests/fuzz/fuzz_fl_parse.c`

- Input: arbitrary bytes on stdin (s2 driver shape), cap 1 MiB.
- Lex fuzzer: drain tokens to EOF; assert forward progress every
  iteration (position strictly increases or kind == EOF) — the classic
  lexer hang is a token that consumes zero bytes.
- Parse fuzzer: `fl_parse` then `fl_parse_literal` on the same input;
  assert: no crash, no hang (5 s alarm in the driver), error count ≤ 20,
  arena high-water ≤ 64 MiB (OOM guard), and `incomplete && had_error`
  never both set.
- Seed corpus: every fixture from this sprint + the spec §14 example +
  minimized motion blocks. Corpus checked in under
  `tests/fuzz/corpus/fl_parse/`.

### 7. Defer

- Compile/eval of any node → hard error is impossible (no entry point
  exists); s30 lands `compile.c`.
- Identifier resolution, arity checks, undefined-name errors → Sprint 30
  (compiler) and Sprint 32 (did-you-mean).
- Motion execution → Sprint 34; the parser only builds `FL_A_MOTION`.
- `yew fl` CLI → Sprint 32. This sprint may add a hidden
  `yew --fl-dump-ast FILE` debug flag (undocumented, subject to removal)
  as the golden-test driver; if the driver plumbing is awkward, dump via
  unit tests only — do not invent CLI surface.

## Testing Strategy

- **Unit** (`tests/unit/test_fl_lex.c`): token-by-token assertions for
  every row of the §1 table — each escape, `_` separator edge cases,
  `1.foo`, `0x` without digits, count `65536` overflow, `av` vs `avx`,
  Unicode arrow aliases, CRLF, comment-then-newline. ≥ 60 assertions.
- **Unit** (`tests/unit/test_fl_parse.c`): dump-goldens for each grammar
  production (positive) and each special-cased diagnostic (negative,
  asserting message text via a capturing diag sink — structural, not
  stderr-scraped). Spec §14 example parses clean; its dump is a golden.
- **Recovery fixtures**: 8 single-typo programs, each asserting exactly
  1 error and that all *following* statements still appear in the dump.
- **Pure-literal**: every `pl_` production positive; negatives for ident,
  call, `@[`, import, nested fn — each names pure-literal mode in the
  message. A 500 KiB generated literal file parses < 50 ms (perf smoke,
  not a gate).
- **Fuzz**: both fuzzers 10 min each in the ASan lane, zero findings;
  corpus replay is part of `make test` (seconds, not minutes).
- **Determinism lane**: dump the same file twice, byte-compare.

## Definition of Done

1. `src/fl/{lex.c,lex.h,ast.h,parse.c,parse.h}` build in both compiler
   lanes, zero warnings. The original touched-directory restriction was a
   Sprint 29 landing-scope control; release closeout preserves that history
   without requiring later owning sprints to leave the subsystem untouched.
2. Token inventory implements every spec §1 row; unit tests cover each
   (≥ 60 lex assertions).
3. Every spec §2 production has ≥ 1 positive dump-golden; every §2
   terminal appears in some fixture. Coverage listed in a comment table
   at the top of `test_fl_parse.c` (production → test name) with zero
   gaps — s33 formalizes this, don't wait for it to be honest.
4. `_Static_assert(sizeof(FlNode) <= 48)`. Lexer and parser allocation remains caller-arena-owned:
   AST construction takes the caller's arena, the
   lexer/parser implementation contains no direct allocator calls, and the
   repository allocator gate continues to reject raw libc allocation.
5. Expected/found wording matches Deliverable 4 exactly in goldens; the
   four special-cased messages each have a fixture.
6. All 8 recovery fixtures: exactly 1 diagnostic each.
7. `fl_parse_literal` rejects all 5 negative classes naming pure-literal
   mode; accepts the s25 workspace-state schema example verbatim.
8. Spec §14 example: parses with 0 errors; golden committed.
9. Fuzzers: 10 min each, ASan/UBSan clean, no hang (alarm never fires),
   corpus committed and replayed in `make test`.
10. `incomplete` flag: unit tests for `fn f(` (incomplete), `fn f((`
    (incomplete), `fn f()}` (error, not incomplete), trailing `+`
    (incomplete), trailing `+ 1` (complete).
