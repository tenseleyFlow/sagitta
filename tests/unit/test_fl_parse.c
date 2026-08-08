/*
 * Sprint 29 DoD 3, 5, 6, 7, 8 and 10: the parser, asserted through the
 * AST dump and a capturing diagnostic sink.
 *
 * PRODUCTION COVERAGE (spec §2 -> test).  DoD 3 wants this table with
 * zero gaps, and Sprint 33 formalises it; the point of writing it now
 * is that a gap is visible rather than merely possible.
 *
 *   program        fl_parse_spec_14_example
 *   stmt           (every row below)
 *   let_stmt       fl_parse_let_and_assign
 *   assign_stmt    fl_parse_let_and_assign
 *   target         fl_parse_let_and_assign          (ident, index, field)
 *   fn_decl        fl_parse_functions
 *   macro_decl     fl_parse_motion_blocks
 *   import_stmt    fl_parse_imports                 (both forms)
 *   if_stmt        fl_parse_control_flow            (if / else if / else)
 *   while_stmt     fl_parse_control_flow
 *   for_stmt       fl_parse_control_flow            (one and two vars)
 *   return_stmt    fl_parse_functions               (with and without expr)
 *   "break" TERM   fl_parse_control_flow
 *   "continue"     fl_parse_control_flow
 *   edit_stmt      fl_parse_edit_and_try
 *   try_stmt       fl_parse_edit_and_try
 *   expr_stmt      fl_parse_let_and_assign
 *   block          fl_parse_functions
 *   params         fl_parse_functions               (zero, one, many)
 *   expr           fl_parse_precedence_ladder
 *   or_e           fl_parse_precedence_ladder       (a or b)
 *   and_e          fl_parse_precedence_ladder       (a and b)
 *   eq_e           fl_parse_precedence_ladder       (==, !=)
 *   rel_e          fl_parse_precedence_ladder       (<, <=, >, >=)
 *   add_e          fl_parse_precedence_ladder       (+, -)
 *   mul_e          fl_parse_precedence_ladder       (*, /, %)
 *   unary          fl_parse_precedence_ladder       (not, -)
 *   postfix        fl_parse_postfix_chain           (call, index, field)
 *   args           fl_parse_postfix_chain
 *   primary        fl_parse_literals_and_primary
 *   fn_expr        fl_parse_functions               (block and expr body)
 *   literal        fl_parse_literals_and_primary
 *   list_lit       fl_parse_literals_and_primary    (incl. trailing comma)
 *   map_lit        fl_parse_literals_and_primary    (incl. trailing comma)
 *   entry          fl_parse_literals_and_primary    (ident/string/int keys)
 *   motion_block   fl_parse_motion_blocks
 *   motion         fl_parse_motion_blocks           (with and without count)
 *   motion_word    fl_parse_motion_blocks           (every alternative)
 *   TERM           fl_parse_terminators             (newline, ';', EOF)
 */
#define _POSIX_C_SOURCE 200809L

#include "harness.h"

#include <stdio.h>
#include <string.h>

#include "fl/parse.h"
#include "util/arena.h"
#include "util/intern.h"

typedef struct PFix {
    Arena arena;
    Interner in;
    DiagCtx dc;
    Bytebuf dump;
    char first[512];   /* the FIRST diagnostic; later ones are cascades */
    u32 ndiag;
    bool had_error;
    bool incomplete;
    u32 nstmts;
} PFix;

static void pcapture(void *ctx, FlDiagLevel level, FlSpan sp,
                     const char *msg, const char *rendered)
{
    PFix *f = ctx;

    (void)level;
    (void)sp;
    (void)rendered;
    if (f->ndiag == 0U)
        (void)snprintf(f->first, sizeof(f->first), "%s", msg);
    f->ndiag++;
}

static void pf_open(PFix *f)
{
    (void)memset(f, 0, sizeof(*f));
    arena_init(&f->arena);
    interner_init(&f->in, &f->arena);
    fl_diag_init(&f->dc, &f->arena);
    fl_diag_set_sink(&f->dc, pcapture, f);
    bytebuf_init(&f->dump);
}

static void pf_close(PFix *f)
{
    bytebuf_free(&f->dump);
    interner_free(&f->in);
    arena_free_all(&f->arena);
}

/* Parses `src` and leaves the s-expression dump in f->dump as a C
 * string.  Returns that string for convenience. */
static const char *parse_dump(PFix *f, const char *src)
{
    FlProgram p;

    (void)fl_diag_add_file(&f->dc, "t.fl", src, strlen(src));
    p = fl_parse(&f->arena, &f->dc, &f->in, src, strlen(src), 0U);
    f->had_error = p.had_error;
    f->incomplete = p.incomplete;
    f->nstmts = p.n;
    f->dump.len = 0U;
    fl_ast_dump(&f->dump, &p, &f->in);
    bytebuf_push_u8(&f->dump, (u8)'\0');
    return (const char *)f->dump.data;
}

/* One source, one expected dump, and no diagnostics. */
static void ok_dump(const char *src, const char *want)
{
    PFix f;
    const char *got;

    pf_open(&f);
    got = parse_dump(&f, src);
    if (strcmp(got, want) != 0) {
        (void)fprintf(stderr, "source: %s\n  want: %s\n   got: %s\n",
                      src, want, got);
    }
    SAG_ASSERT_EQ_I64(strcmp(got, want), 0);
    SAG_ASSERT_EQ_U64(f.ndiag, 0U);
    SAG_ASSERT(!f.had_error);
    pf_close(&f);
}

/* ---------------------------------------------------------------- */
/* Positive productions                                             */
/* ---------------------------------------------------------------- */

void test_fl_parse_let_and_assign(void)
{
    ok_dump("let total = 0\n", "(let \"total\" (lit int 0))\n");
    ok_dump("let bare\n", "(let \"bare\" nil)\n");
    ok_dump("x = 1\n", "(assign (id \"x\") (lit int 1))\n");
    ok_dump("x[0] = 1\n",
            "(assign (index (id \"x\") (lit int 0)) (lit int 1))\n");
    ok_dump("x.f = 1\n", "(assign (field (id \"x\") \"f\") (lit int 1))\n");
    /* expr_stmt */
    ok_dump("f()\n", "(call (id \"f\"))\n");
}

void test_fl_parse_literals_and_primary(void)
{
    ok_dump("nil\n", "(lit nil)\n");
    ok_dump("true\n", "(lit bool true)\n");
    ok_dump("false\n", "(lit bool false)\n");
    ok_dump("42\n", "(lit int 42)\n");
    ok_dump("2.5\n", "(lit float 2.5)\n");
    ok_dump("\"s\"\n", "(lit str \"s\")\n");
    ok_dump("(1)\n", "(lit int 1)\n");
    ok_dump("[1, 2]\n", "(list (lit int 1) (lit int 2))\n");
    /* §2's list_lit and map_lit both allow a trailing comma. */
    ok_dump("[1,]\n", "(list (lit int 1))\n");
    /*
     * A map literal is reachable in EXPRESSION position only: §1.7
     * makes a `{` that starts a statement a hard error, so these are
     * written as initialisers rather than bare.
     */
    ok_dump("let m = {a: 1}\n",
            "(let \"m\" (map ((lit str \"a\") (lit int 1))))\n");
    ok_dump("let m = {\"k\": 1, 2: nil,}\n",
            "(let \"m\" (map ((lit str \"k\") (lit int 1)) "
            "((lit int 2) (lit nil))))\n");
}

void test_fl_parse_precedence_ladder(void)
{
    /* §2's cascade, and every binary operator is LEFT-associative. */
    ok_dump("a or b and c\n",
            "(or (id \"a\") (and (id \"b\") (id \"c\")))\n");
    ok_dump("a == b < c\n",
            "(== (id \"a\") (< (id \"b\") (id \"c\")))\n");
    ok_dump("1 + 2 * 3\n",
            "(+ (lit int 1) (* (lit int 2) (lit int 3)))\n");
    ok_dump("1 - 2 - 3\n",
            "(- (- (lit int 1) (lit int 2)) (lit int 3))\n");
    ok_dump("1 / 2 % 3\n",
            "(% (/ (lit int 1) (lit int 2)) (lit int 3))\n");
    ok_dump("a <= b\n", "(<= (id \"a\") (id \"b\"))\n");
    ok_dump("a >= b\n", "(>= (id \"a\") (id \"b\"))\n");
    ok_dump("a != b\n", "(!= (id \"a\") (id \"b\"))\n");
    ok_dump("a > b\n", "(> (id \"a\") (id \"b\"))\n");
    /* unary is right-associative and binds tighter than any binary. */
    ok_dump("not a\n", "(unot (id \"a\"))\n");
    ok_dump("- a * b\n", "(* (u- (id \"a\")) (id \"b\"))\n");
    ok_dump("not not a\n", "(unot (unot (id \"a\")))\n");
}

void test_fl_parse_postfix_chain(void)
{
    ok_dump("f(1, 2)\n", "(call (id \"f\") (lit int 1) (lit int 2))\n");
    ok_dump("f()\n", "(call (id \"f\"))\n");
    ok_dump("a[0]\n", "(index (id \"a\") (lit int 0))\n");
    ok_dump("a.b\n", "(field (id \"a\") \"b\")\n");
    /* Postfix binds left to right, so the chain nests outward. */
    ok_dump("a.b[0].c()\n",
            "(call (field (index (field (id \"a\") \"b\") "
            "(lit int 0)) \"c\"))\n");
}

void test_fl_parse_functions(void)
{
    ok_dump("fn f() { }\n", "(fn \"f\" (params) (block))\n");
    ok_dump("fn f(a) { return a }\n",
            "(fn \"f\" (params \"a\") (block (return (id \"a\"))))\n");
    ok_dump("fn f(a, b, c) { return }\n",
            "(fn \"f\" (params \"a\" \"b\" \"c\") (block (return nil)))\n");
    /* §2's fn_expr takes a block OR a bare expression. */
    ok_dump("let g = fn() { 1 }\n",
            "(let \"g\" (fn-expr (params) (block (lit int 1))))\n");
    ok_dump("let g = fn(x) x\n",
            "(let \"g\" (fn-expr (params \"x\") (id \"x\")))\n");
}

void test_fl_parse_control_flow(void)
{
    ok_dump("if a { }\n", "(if (id \"a\") (block) nil)\n");
    ok_dump("if a { } else { }\n", "(if (id \"a\") (block) (block))\n");
    /* `else if` chains by nesting, which is what §2 describes. */
    ok_dump("if a { } else if b { } else { }\n",
            "(if (id \"a\") (block) (if (id \"b\") (block) (block)))\n");
    ok_dump("while a { break }\n",
            "(while (id \"a\") (block (break)))\n");
    ok_dump("for x in xs { continue }\n",
            "(for \"x\" (id \"xs\") (block (continue)))\n");
    ok_dump("for k, v in m { }\n",
            "(for \"k\" \"v\" (id \"m\") (block))\n");
}

void test_fl_parse_imports(void)
{
    ok_dump("import str\n", "(import \"str\")\n");
    ok_dump("import \"a/b.fl\" as b\n", "(import \"a/b.fl\" as \"b\")\n");
}

void test_fl_parse_edit_and_try(void)
{
    ok_dump("edit { }\n", "(edit (block))\n");
    ok_dump("try { } catch e { }\n",
            "(try (block) catch \"e\" (block))\n");
    /* §2 puts no TERM between a block and its `catch`; §14 writes the
     * two on separate lines. */
    ok_dump("try { }\ncatch e { }\n",
            "(try (block) catch \"e\" (block))\n");
    ok_dump("if a { }\nelse { }\n", "(if (id \"a\") (block) (block))\n");
}

void test_fl_parse_motion_blocks(void)
{
    ok_dump("@[l]\n", "(motion-block (motion 1 unit l))\n");
    ok_dump("@[2v]\n", "(motion-block (motion 2 arrow v))\n");
    ok_dump("@[av]\n", "(motion-block (motion 1 arrow v alt))\n");
    ok_dump("@[H(2>)]\n",
            "(motion-block (motion 1 highlight (motion 2 arrow >)))\n");
    ok_dump("@[i\"!\"]\n", "(motion-block (motion 1 ins \"!\"))\n");
    ok_dump("@[del esc]\n",
            "(motion-block (motion 1 del) (motion 1 esc))\n");
    ok_dump("@[yank]\n", "(motion-block (motion 1 word \"yank\"))\n");
    ok_dump("macro m = @[esc]\n",
            "(macro \"m\" (motion-block (motion 1 esc)))\n");
}

void test_fl_parse_terminators(void)
{
    /* §1.2: TERM = NEWLINE | ";" | EOF, and all three end a statement. */
    ok_dump("1\n2\n", "(lit int 1)\n(lit int 2)\n");
    ok_dump("1; 2\n", "(lit int 1)\n(lit int 2)\n");
    ok_dump("1", "(lit int 1)\n");
    /* A newline after a trailing binary operator, comma or `=` is a
     * continuation rather than a terminator. */
    ok_dump("let a = 1 +\n2\n",
            "(let \"a\" (+ (lit int 1) (lit int 2)))\n");
    ok_dump("let a = [1,\n2]\n",
            "(let \"a\" (list (lit int 1) (lit int 2)))\n");
    ok_dump("let a =\n1\n", "(let \"a\" (lit int 1))\n");
    /*
     * A BLOCK's newlines still terminate.  §1.2 lists `{` among the
     * openers that continue a line, but that is the map literal: a body
     * whose statements ran together was the bug §14 caught.
     */
    ok_dump("fn f() {\nlet a = 1\nreturn a\n}\n",
            "(fn \"f\" (params) (block (let \"a\" (lit int 1)) "
            "(return (id \"a\"))))\n");
    /* ...while a map literal's newlines do not. */
    ok_dump("let m = {\na: 1\n}\n",
            "(let \"m\" (map ((lit str \"a\") (lit int 1))))\n");
}

/* ---------------------------------------------------------------- */
/* DoD 5: the four special-cased diagnostics                        */
/* ---------------------------------------------------------------- */

/* Parses and asserts the FIRST diagnostic contains `want`, and that
 * exactly `n` diagnostics were emitted in total. */
static void diag_is(const char *src, const char *want, u32 n)
{
    PFix f;

    pf_open(&f);
    (void)parse_dump(&f, src);
    if (f.ndiag != n || strstr(f.first, want) == NULL) {
        (void)fprintf(stderr, "source: %s\n  want: %s (x%u)\n"
                              "   got: %s (x%u)\n",
                      src, want, (unsigned)n, f.first, (unsigned)f.ndiag);
    }
    SAG_ASSERT(strstr(f.first, want) != NULL);
    SAG_ASSERT_EQ_U64(f.ndiag, n);
    SAG_ASSERT(f.had_error);
    pf_close(&f);
}

void test_fl_parse_special_cased_messages(void)
{
    /* §1.7, wording carried from the spec. */
    diag_is("{a: 1}\n", "map literal cannot start a statement", 1U);
    /* `@` without `[` -- reported by the lexer, which names the pair. */
    diag_is("let x = @y\n", "@[", 1U);
    /* `=` where a condition's block was due. */
    diag_is("if x = 1 { }\n", "did you mean '=='?", 1U);
    /*
     * A motion word outside a block.  Reachable because a statement
     * error can leave the LEXER inside `@[ ... ]`, so its tokens arrive
     * where an expression was expected.
     */
    diag_is("let = @[2>]\n", "expected a name after 'let'", 1U);
}

void test_fl_parse_expected_found_wording(void)
{
    PFix f;

    /* DoD 5: every parse error names both, keywords as themselves and
     * literals as a category. */
    pf_open(&f);
    (void)parse_dump(&f, "f(1\nlet b = 2\n");
    SAG_ASSERT_EQ_I64(strcmp(f.first, "expected ')', found 'let'"), 0);
    pf_close(&f);

    pf_open(&f);
    (void)parse_dump(&f, "let 1 = 2\n");
    SAG_ASSERT(strstr(f.first, "found 'integer'") != NULL);
    pf_close(&f);
}

/* ---------------------------------------------------------------- */
/* DoD 6: eight single-typo programs, one diagnostic each           */
/* ---------------------------------------------------------------- */

void test_fl_parse_recovery_one_typo_one_error(void)
{
    static const struct { const char *src; const char *survives; } fix[] = {
        /* The classic cascade: a missing `)` before a run of later
         * statements.  Exactly one error, and everything after it must
         * still appear in the dump. */
        {"let a = (1\nlet b = 2\nlet c = 3\n", "(let \"c\" (lit int 3))"},
        {"let a = [1\nlet b = 2\n",            "(let \"b\" (lit int 2))"},
        {"f(1, 2\nlet b = 2\n",                "(let \"b\" (lit int 2))"},
        {"let = 1\nlet b = 2\n",               "(let \"b\" (lit int 2))"},
        {"fn () { }\nlet b = 2\n",             "(let \"b\" (lit int 2))"},
        {"if a { }\nelse\nlet b = 2\n",        "(let \"b\" (lit int 2))"},
        {"for in xs { }\nlet b = 2\n",         "(let \"b\" (lit int 2))"},
        {"import\nlet b = 2\n",                "(let \"b\" (lit int 2))"}
    };
    size_t i;

    for (i = 0U; i < SAG_ARRAY_LEN(fix); i++) {
        PFix f;
        const char *got;

        pf_open(&f);
        got = parse_dump(&f, fix[i].src);
        if (f.ndiag != 1U || strstr(got, fix[i].survives) == NULL) {
            (void)fprintf(stderr,
                          "recovery fixture %zu: ndiag=%u first=%s\n%s\n",
                          i, (unsigned)f.ndiag, f.first, got);
        }
        SAG_ASSERT_EQ_U64(f.ndiag, 1U);
        SAG_ASSERT(strstr(got, fix[i].survives) != NULL);
        pf_close(&f);
    }
}

void test_fl_parse_error_cap(void)
{
    PFix f;
    char src[4096];
    size_t at = 0U;
    int i;

    /* Fuzz hygiene: 20 diagnostics, then one that says so. */
    for (i = 0; i < 60; i++)
        at += (size_t)snprintf(src + at, sizeof(src) - at, "let = %d\n", i);
    pf_open(&f);
    (void)parse_dump(&f, src);
    SAG_ASSERT(f.ndiag <= (u32)FL_PARSE_MAX_ERRORS + 1U);
    SAG_ASSERT(f.had_error);
    pf_close(&f);
}

/* ---------------------------------------------------------------- */
/* DoD 10: the incomplete flag                                      */
/* ---------------------------------------------------------------- */

void test_fl_parse_incomplete_flag(void)
{
    static const struct {
        const char *src; bool incomplete; bool had_error;
    } cases[] = {
        {"fn f(",       true,  false},
        {"fn f((",      true,  false},
        {"fn f()}",     false, true},
        {"let a = 1 +", true,  false},
        {"let a = 1 + 1", false, false}
    };
    size_t i;

    /*
     * The REPL reads `incomplete` as "ask for another line", so the two
     * flags must never both be set: a real mistake inside an open
     * bracket that also claimed to be unfinished would make Sprint 32
     * wait forever for a closer that cannot fix it.
     */
    for (i = 0U; i < SAG_ARRAY_LEN(cases); i++) {
        PFix f;

        pf_open(&f);
        (void)parse_dump(&f, cases[i].src);
        if (f.incomplete != cases[i].incomplete ||
            f.had_error != cases[i].had_error) {
            (void)fprintf(stderr, "incomplete case %zu (%s): "
                                  "inc=%d(want %d) err=%d(want %d)\n",
                          i, cases[i].src, (int)f.incomplete,
                          (int)cases[i].incomplete, (int)f.had_error,
                          (int)cases[i].had_error);
        }
        SAG_ASSERT_EQ_U64((u64)f.incomplete, (u64)cases[i].incomplete);
        SAG_ASSERT_EQ_U64((u64)f.had_error, (u64)cases[i].had_error);
        SAG_ASSERT(!(f.incomplete && f.had_error));
        pf_close(&f);
    }
}

void test_fl_parse_depth_cap(void)
{
    PFix f;
    char src[2048];
    size_t i;

    /* Unbounded recursion is not a diagnosable error, so the bound is
     * checked rather than discovered by the fuzzer. */
    for (i = 0U; i < sizeof(src) - 2U; i++)
        src[i] = '(';
    src[sizeof(src) - 2U] = '1';
    src[sizeof(src) - 1U] = '\0';
    pf_open(&f);
    (void)parse_dump(&f, src);
    SAG_ASSERT(f.had_error || f.incomplete);
    pf_close(&f);
}

/* ---------------------------------------------------------------- */
/* DoD 8: the §14 worked example, and determinism                   */
/* ---------------------------------------------------------------- */

static const char *const FL_SPEC_14 =
    "# fletch-spec 14 -- one of everything\n"
    "import str\n"
    "\n"
    "let greet = \"hi\\tthere\\n\"\n"
    "let nums  = [1, 2.5, 0x10]\n"
    "let cfg   = { tabwidth: 4, wrap: false, name: nil }\n"
    "\n"
    "fn clamp(x, lo, hi) {\n"
    "    if x < lo { return lo }\n"
    "    else if x > hi { return hi }\n"
    "    return x\n"
    "}\n"
    "\n"
    "fn counter(start) {\n"
    "    let n = start\n"
    "    return fn() { n = n + 1; return n }\n"
    "}\n"
    "\n"
    "let next = counter(clamp(9, 0, 8))\n"
    "while next() < 10 { }\n"
    "\n"
    "let total = 0\n"
    "for x in nums {\n"
    "    if x == 2.5 { continue }\n"
    "    total = total + x\n"
    "}\n"
    "\n"
    "macro dup = @[ l< H(lv) yank v paste esc ]\n"
    "\n"
    "fn shout(s) {\n"
    "    try { edit { @[ 2v i\"!\" del ] } }\n"
    "    catch e { return str.upper(e.kind) or \"?\" }\n"
    "}\n";

void test_fl_parse_spec_14_example(void)
{
    PFix f;
    const char *got;

    /* DoD 8: parses with zero errors.  This example is normative and
     * exercises every construct, which is why it is the one fixture
     * worth asserting statement by statement. */
    pf_open(&f);
    got = parse_dump(&f, FL_SPEC_14);
    if (f.ndiag != 0U)
        (void)fprintf(stderr, "spec 14 diagnostic: %s\n", f.first);
    SAG_ASSERT_EQ_U64(f.ndiag, 0U);
    SAG_ASSERT(!f.had_error);
    SAG_ASSERT(!f.incomplete);
    SAG_ASSERT_EQ_U64(f.nstmts, 12U);
    /* Spot-check the shapes that the two newline bugs broke. */
    SAG_ASSERT(strstr(got, "(let \"n\" (id \"start\"))") != NULL);
    SAG_ASSERT(strstr(got, "catch \"e\"") != NULL);
    SAG_ASSERT(strstr(got,
                      "(let \"nums\" (list (lit int 1) (lit float 2.5) "
                      "(lit int 16)))") != NULL);
    pf_close(&f);
}

void test_fl_parse_dump_is_deterministic(void)
{
    PFix a;
    PFix b;

    /* The determinism lane dumps the same source twice and compares
     * bytes; doing it here too keeps the property local to the code
     * that owns it. */
    pf_open(&a);
    pf_open(&b);
    {
        const char *one = parse_dump(&a, FL_SPEC_14);
        const char *two = parse_dump(&b, FL_SPEC_14);

        SAG_ASSERT_EQ_U64(a.dump.len, b.dump.len);
        SAG_ASSERT_EQ_I64(strcmp(one, two), 0);
    }
    pf_close(&b);
    pf_close(&a);
}

/* ---------------------------------------------------------------- */
/* DoD 7: pure-literal mode                                         */
/* ---------------------------------------------------------------- */

static void pl_ok(const char *src, const char *want)
{
    PFix f;
    Bytebuf out;
    FlNode *v;

    pf_open(&f);
    (void)fl_diag_add_file(&f.dc, "d.fl", src, strlen(src));
    v = fl_parse_literal(&f.arena, &f.dc, &f.in, src, strlen(src), 0U);
    SAG_ASSERT_NOT_NULL(v);
    SAG_ASSERT_EQ_U64(f.ndiag, 0U);
    bytebuf_init(&out);
    fl_ast_dump_node(&out, v, &f.in);
    bytebuf_push_u8(&out, (u8)'\0');
    if (strcmp((const char *)out.data, want) != 0)
        (void)fprintf(stderr, "pl source: %s\n  want: %s\n   got: %s\n",
                      src, want, (const char *)out.data);
    SAG_ASSERT_EQ_I64(strcmp((const char *)out.data, want), 0);
    bytebuf_free(&out);
    pf_close(&f);
}

static void pl_rejects(const char *src)
{
    PFix f;
    FlNode *v;

    pf_open(&f);
    (void)fl_diag_add_file(&f.dc, "d.fl", src, strlen(src));
    v = fl_parse_literal(&f.arena, &f.dc, &f.in, src, strlen(src), 0U);
    if (v != NULL || f.ndiag == 0U)
        (void)fprintf(stderr, "pl should reject: %s (ndiag=%u)\n", src,
                      (unsigned)f.ndiag);
    SAG_ASSERT_NULL(v);
    SAG_ASSERT(f.ndiag != 0U);
    /* Every rejection names the mode, so the reader knows the construct
     * is refused HERE rather than invalid everywhere. */
    SAG_ASSERT(strstr(f.first, "pure-literal mode") != NULL);
    pf_close(&f);
}

void test_fl_parse_literal_accepts_the_value_grammar(void)
{
    pl_ok("nil", "(lit nil)");
    pl_ok("true", "(lit bool true)");
    pl_ok("42", "(lit int 42)");
    /* §12's pl_value carries its own sign: the `-` is part of the
     * literal, not an operator, because §12.1 says no operator is
     * reachable. */
    pl_ok("-42", "(lit int -42)");
    pl_ok("-2.5", "(lit float -2.5)");
    pl_ok("\"s\"", "(lit str \"s\")");
    pl_ok("[1, 2,]", "(list (lit int 1) (lit int 2))");
    pl_ok("{a: 1}", "(map ((lit str \"a\") (lit int 1)))");
    /* Comments are allowed (§12 cites §1.1). */
    pl_ok("# a note\n[1]", "(list (lit int 1))");
}

void test_fl_parse_literal_rejects_the_negative_list(void)
{
    /* §12.1, one per bullet. */
    pl_rejects("f(1)");        /* no call                              */
    pl_rejects("name");        /* no identifier as a value             */
    pl_rejects("import x");    /* no import                            */
    pl_rejects("fn() { }");    /* no function literal                  */
    pl_rejects("@[2>]");       /* no motion block                      */
    pl_rejects("1 + 1");       /* no operator                          */
    pl_rejects("x = 1");       /* no assignment, no statement          */
    /* And a nested reach: the value grammar must not become escapable
     * one level down. */
    pl_rejects("[f(1)]");
    pl_rejects("{a: name}");
}

void test_fl_parse_literal_accepts_a_workspace_state_document(void)
{
    /*
     * DoD 7: Sprint 25 froze the workspace-state schema on this format,
     * so a document shaped like one must parse -- that is the whole
     * reason the mode exists.
     */
    static const char *const doc =
        "{\n"
        "  version: 1,\n"
        "  root: \"/home/u/proj\",\n"
        "  tabs: [\n"
        "    { id: 1, path: \"src/main.c\", line: 42, col: 7 },\n"
        "    { id: 2, path: \"src/fl/lex.c\", line: 1, col: 1 },\n"
        "  ],\n"
        "  groups: [ { name: \"core\", members: [1, 2] } ],\n"
        "  options: { tabwidth: 4, wrap: false, theme: nil },\n"
        "  ratio: -0.5,\n"
        "}\n";
    PFix f;
    FlNode *v;

    pf_open(&f);
    (void)fl_diag_add_file(&f.dc, "state.fl", doc, strlen(doc));
    v = fl_parse_literal(&f.arena, &f.dc, &f.in, doc, strlen(doc), 0U);
    if (f.ndiag != 0U)
        (void)fprintf(stderr, "state doc diagnostic: %s\n", f.first);
    SAG_ASSERT_NOT_NULL(v);
    SAG_ASSERT_EQ_U64(f.ndiag, 0U);
    SAG_ASSERT_EQ_U64((u64)v->kind, (u64)FL_A_MAP);
    SAG_ASSERT_EQ_U64(v->as.map.n, 6U);  /* version root tabs groups options ratio */
    pf_close(&f);
}
