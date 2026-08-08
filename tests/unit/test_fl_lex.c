/*
 * Sprint 29 DoD 2: every row of the spec §1 token table, asserted
 * token by token.
 *
 * Diagnostics are checked through a CAPTURING SINK rather than by
 * scraping stderr, so a test asserts the sentence the user reads and
 * stays indifferent to where the compiler happens to write it.
 */
#define _POSIX_C_SOURCE 200809L

#include "harness.h"

#include <stdio.h>
#include <string.h>

#include "fl/lex.h"
#include "util/arena.h"
#include "util/intern.h"

typedef struct LexFix {
    Arena arena;
    Interner in;
    DiagCtx dc;
    FlLexer lx;
    /* Last diagnostic's bare message; the caret block is exercised
     * separately so the two concerns fail independently. */
    char msg[512];
    u32 ndiag;
} LexFix;

static void capture(void *ctx, FlDiagLevel level, FlSpan sp,
                    const char *msg, const char *rendered)
{
    LexFix *f = ctx;

    (void)level;
    (void)sp;
    (void)rendered;
    f->ndiag++;
    (void)snprintf(f->msg, sizeof(f->msg), "%s", msg);
}

static void lf_open(LexFix *f, const char *src)
{
    (void)memset(f, 0, sizeof(*f));
    arena_init(&f->arena);
    interner_init(&f->in, &f->arena);
    fl_diag_init(&f->dc, &f->arena);
    fl_diag_set_sink(&f->dc, capture, f);
    (void)fl_diag_add_file(&f->dc, "t.fl", src, strlen(src));
    fl_lex_init(&f->lx, &f->arena, &f->dc, &f->in, src, strlen(src), 0U);
}

static void lf_close(LexFix *f)
{
    interner_free(&f->in);
    arena_free_all(&f->arena);
}

static FlTok nx(LexFix *f) { return fl_lex_next(&f->lx); }

/* Asserts the next token's kind and returns it for payload checks. */
static FlTok expect_kind(LexFix *f, FlTokKind want)
{
    FlTok t = nx(f);

    SAG_ASSERT_EQ_U64((u64)t.kind, (u64)want);
    return t;
}

static const char *interned(LexFix *f, u32 id)
{
    return sag_intern_str(&f->in, id);
}

/* ---------------------------------------------------------------- */
/* §1.1 comments, §1.2 terminators                                  */
/* ---------------------------------------------------------------- */

void test_fl_lex_comment_keeps_its_newline(void)
{
    LexFix f;

    /* The comment runs to end of line and the NEWLINE still arrives:
     * swallowing it would join two statements into one. */
    lf_open(&f, "let # trailing\nlet");
    (void)expect_kind(&f, FL_T_LET);
    (void)expect_kind(&f, FL_T_NEWLINE);
    (void)expect_kind(&f, FL_T_LET);
    (void)expect_kind(&f, FL_T_EOF);
    SAG_ASSERT_EQ_U64(f.ndiag, 0U);
    lf_close(&f);
}

void test_fl_lex_crlf_is_one_newline_and_bare_cr_is_an_error(void)
{
    LexFix f;

    lf_open(&f, "a\r\nb");
    (void)expect_kind(&f, FL_T_IDENT);
    (void)expect_kind(&f, FL_T_NEWLINE);
    (void)expect_kind(&f, FL_T_IDENT);
    (void)expect_kind(&f, FL_T_EOF);
    SAG_ASSERT_EQ_U64(f.ndiag, 0U);
    lf_close(&f);

    lf_open(&f, "a\rb");
    (void)expect_kind(&f, FL_T_IDENT);
    (void)expect_kind(&f, FL_T_ERROR);
    SAG_ASSERT(strstr(f.msg, "carriage return") != NULL);
    lf_close(&f);
}

/* ---------------------------------------------------------------- */
/* §1.3 identifiers and the 22 keywords                             */
/* ---------------------------------------------------------------- */

void test_fl_lex_keywords_need_a_terminator(void)
{
    LexFix f;
    static const struct { const char *text; FlTokKind kind; } words[] = {
        {"and", FL_T_AND},       {"as", FL_T_AS},
        {"break", FL_T_BREAK},   {"catch", FL_T_CATCH},
        {"continue", FL_T_CONTINUE}, {"edit", FL_T_EDIT},
        {"else", FL_T_ELSE},     {"false", FL_T_FALSE},
        {"fn", FL_T_FN},         {"for", FL_T_FOR},
        {"if", FL_T_IF},         {"import", FL_T_IMPORT},
        {"in", FL_T_IN},         {"let", FL_T_LET},
        {"macro", FL_T_MACRO},   {"nil", FL_T_NIL},
        {"not", FL_T_NOT},       {"or", FL_T_OR},
        {"return", FL_T_RETURN}, {"true", FL_T_TRUE},
        {"try", FL_T_TRY},       {"while", FL_T_WHILE}
    };
    size_t i;

    /* Spec §15.1 pins the count at 22 and Sprint 33 asserts it; the
     * table above is the enumeration, so check it here too. */
    SAG_ASSERT_EQ_U64(SAG_ARRAY_LEN(words), (u64)FL_KEYWORD_COUNT);
    for (i = 0U; i < SAG_ARRAY_LEN(words); i++) {
        lf_open(&f, words[i].text);
        (void)expect_kind(&f, words[i].kind);
        (void)expect_kind(&f, FL_T_EOF);
        lf_close(&f);
    }
    /* A keyword with anything glued to it is an IDENT -- the classic
     * bug is matching a prefix and leaving `x` behind. */
    lf_open(&f, "letx let_ notx _let");
    (void)expect_kind(&f, FL_T_IDENT);
    (void)expect_kind(&f, FL_T_IDENT);
    (void)expect_kind(&f, FL_T_IDENT);
    (void)expect_kind(&f, FL_T_IDENT);
    (void)expect_kind(&f, FL_T_EOF);
    SAG_ASSERT_EQ_U64(f.ndiag, 0U);
    lf_close(&f);
}

/* ---------------------------------------------------------------- */
/* §1.4 numbers                                                     */
/* ---------------------------------------------------------------- */

void test_fl_lex_integers_decimal_hex_and_separators(void)
{
    LexFix f;
    FlTok t;

    lf_open(&f, "0 42 1_000_000 0xFF 0xFF_FF 0X10");
    t = expect_kind(&f, FL_T_INT); SAG_ASSERT_EQ_I64(t.v.i, 0);
    t = expect_kind(&f, FL_T_INT); SAG_ASSERT_EQ_I64(t.v.i, 42);
    t = expect_kind(&f, FL_T_INT); SAG_ASSERT_EQ_I64(t.v.i, 1000000);
    t = expect_kind(&f, FL_T_INT); SAG_ASSERT_EQ_I64(t.v.i, 255);
    t = expect_kind(&f, FL_T_INT); SAG_ASSERT_EQ_I64(t.v.i, 65535);
    t = expect_kind(&f, FL_T_INT); SAG_ASSERT_EQ_I64(t.v.i, 16);
    (void)expect_kind(&f, FL_T_EOF);
    SAG_ASSERT_EQ_U64(f.ndiag, 0U);
    lf_close(&f);
}

void test_fl_lex_underscore_must_separate_digits(void)
{
    LexFix f;
    static const char *const bad[] = {"1_", "0x_", "1__0"};
    size_t i;

    for (i = 0U; i < SAG_ARRAY_LEN(bad); i++) {
        lf_open(&f, bad[i]);
        (void)expect_kind(&f, FL_T_ERROR);
        SAG_ASSERT(strstr(f.msg, "separate digits") != NULL);
        lf_close(&f);
    }
    /* `_1` is not a number at all -- it is a perfectly good identifier,
     * and reporting it as a malformed literal would be a worse message
     * than the parser's. */
    lf_open(&f, "_1");
    (void)expect_kind(&f, FL_T_IDENT);
    SAG_ASSERT_EQ_U64(f.ndiag, 0U);
    lf_close(&f);
}

void test_fl_lex_int_overflow_is_an_error_not_a_wrap(void)
{
    LexFix f;
    FlTok t;

    /* §1.4: a literal that does not fit in i64 is a compile error,
     * never a silent wrap. */
    lf_open(&f, "9223372036854775808");
    (void)expect_kind(&f, FL_T_ERROR);
    SAG_ASSERT(strstr(f.msg, "i64") != NULL);
    lf_close(&f);

    lf_open(&f, "9223372036854775807");
    t = expect_kind(&f, FL_T_INT);
    SAG_ASSERT_EQ_I64(t.v.i, INT64_MAX);
    SAG_ASSERT_EQ_U64(f.ndiag, 0U);
    lf_close(&f);
}

void test_fl_lex_hex_needs_a_digit(void)
{
    LexFix f;

    lf_open(&f, "0x");
    (void)expect_kind(&f, FL_T_ERROR);
    SAG_ASSERT(strstr(f.msg, "hexadecimal") != NULL);
    lf_close(&f);
}

void test_fl_lex_floats_and_the_dot_rule(void)
{
    LexFix f;
    FlTok t;

    lf_open(&f, "1.5 2.0e3 7.5E-2 1.0e+1");
    t = expect_kind(&f, FL_T_FLOAT); SAG_ASSERT(t.v.f > 1.49 && t.v.f < 1.51);
    t = expect_kind(&f, FL_T_FLOAT); SAG_ASSERT(t.v.f > 1999.0 && t.v.f < 2001.0);
    t = expect_kind(&f, FL_T_FLOAT); SAG_ASSERT(t.v.f > 0.074 && t.v.f < 0.076);
    t = expect_kind(&f, FL_T_FLOAT); SAG_ASSERT(t.v.f > 9.9 && t.v.f < 10.1);
    (void)expect_kind(&f, FL_T_EOF);
    SAG_ASSERT_EQ_U64(f.ndiag, 0U);
    lf_close(&f);

    /*
     * `1.foo` is INT DOT IDENT.
     *
     * The dot is consumed only when a digit follows (§1.4 has no
     * trailing-dot float), which is what keeps field access on a numeric
     * literal from lexing as a broken float.
     */
    lf_open(&f, "1.foo");
    t = expect_kind(&f, FL_T_INT); SAG_ASSERT_EQ_I64(t.v.i, 1);
    (void)expect_kind(&f, FL_T_DOT);
    (void)expect_kind(&f, FL_T_IDENT);
    SAG_ASSERT_EQ_U64(f.ndiag, 0U);
    lf_close(&f);

    /* `1.` is INT then DOT; `.5` is DOT then INT.  Both are rejected by
     * the parser, not the lexer -- §1.4 bans the FORMS, and the lexer's
     * job is to hand over what is actually written. */
    lf_open(&f, "1.");
    (void)expect_kind(&f, FL_T_INT);
    (void)expect_kind(&f, FL_T_DOT);
    lf_close(&f);

    lf_open(&f, ".5");
    (void)expect_kind(&f, FL_T_DOT);
    (void)expect_kind(&f, FL_T_INT);
    lf_close(&f);
}

/* ---------------------------------------------------------------- */
/* §1.5 strings                                                     */
/* ---------------------------------------------------------------- */

void test_fl_lex_string_escapes(void)
{
    LexFix f;
    FlTok t;

    lf_open(&f, "\"a\\nb\\tc\\rd\\\\e\\\"f\"");
    t = expect_kind(&f, FL_T_STRING);
    SAG_ASSERT_EQ_I64(strcmp(interned(&f, t.v.str_id),
                             "a\nb\tc\rd\\e\"f"), 0);
    SAG_ASSERT_EQ_U64(f.ndiag, 0U);
    lf_close(&f);

    /* \xNN is a BYTE; \u{...} is a codepoint encoded as UTF-8. */
    lf_open(&f, "\"\\x41\\x7A\"");
    t = expect_kind(&f, FL_T_STRING);
    SAG_ASSERT_EQ_I64(strcmp(interned(&f, t.v.str_id), "Az"), 0);
    lf_close(&f);

    lf_open(&f, "\"\\u{48}\\u{1F600}\"");
    t = expect_kind(&f, FL_T_STRING);
    SAG_ASSERT_EQ_I64(strcmp(interned(&f, t.v.str_id),
                             "H\xF0\x9F\x98\x80"), 0);
    SAG_ASSERT_EQ_U64(f.ndiag, 0U);
    lf_close(&f);
}

void test_fl_lex_unknown_escape_points_at_the_escape(void)
{
    LexFix f;
    FlTok t;

    /*
     * §1.5: unknown escapes are errors, never passed through -- and the
     * caret goes on the escape, not on the opening quote.  A long line
     * with one bad escape is unreadable the other way.
     */
    lf_open(&f, "\"aaaa\\dbbbb\"");
    t = expect_kind(&f, FL_T_ERROR);
    SAG_ASSERT(strstr(f.msg, "unknown escape") != NULL);
    SAG_ASSERT_EQ_U64(t.sp.line, 1U);
    SAG_ASSERT_EQ_U64(t.sp.col, 6U); /* the backslash, 1-based */
    lf_close(&f);
}

void test_fl_lex_string_error_cases(void)
{
    LexFix f;

    lf_open(&f, "\"no end");
    (void)expect_kind(&f, FL_T_ERROR);
    SAG_ASSERT(strstr(f.msg, "unterminated") != NULL);
    lf_close(&f);

    /* A newline inside a string is an error; use \n. */
    lf_open(&f, "\"a\nb\"");
    (void)expect_kind(&f, FL_T_ERROR);
    SAG_ASSERT(strstr(f.msg, "newline in string") != NULL);
    lf_close(&f);

    lf_open(&f, "\"\\x4\"");
    (void)expect_kind(&f, FL_T_ERROR);
    SAG_ASSERT(strstr(f.msg, "two hexadecimal") != NULL);
    lf_close(&f);

    lf_open(&f, "\"\\u{}\"");
    (void)expect_kind(&f, FL_T_ERROR);
    SAG_ASSERT(strstr(f.msg, "hexadecimal digits") != NULL);
    lf_close(&f);

    /* Past the scalar range, and a surrogate: both would put bytes in
     * the string that no decoder accepts back. */
    lf_open(&f, "\"\\u{110000}\"");
    (void)expect_kind(&f, FL_T_ERROR);
    SAG_ASSERT(strstr(f.msg, "scalar value") != NULL);
    lf_close(&f);

    lf_open(&f, "\"\\u{D800}\"");
    (void)expect_kind(&f, FL_T_ERROR);
    SAG_ASSERT(strstr(f.msg, "scalar value") != NULL);
    lf_close(&f);
}

/* ---------------------------------------------------------------- */
/* Operators, delimiters, and `@`                                   */
/* ---------------------------------------------------------------- */

void test_fl_lex_operators_and_delimiters(void)
{
    LexFix f;
    static const struct { const char *text; FlTokKind kind; } ops[] = {
        {"+", FL_T_PLUS}, {"-", FL_T_MINUS}, {"*", FL_T_STAR},
        {"/", FL_T_SLASH}, {"%", FL_T_PERCENT}, {"=", FL_T_EQ},
        {"==", FL_T_EQEQ}, {"!=", FL_T_BANGEQ}, {"<", FL_T_LT},
        {"<=", FL_T_LE}, {">", FL_T_GT}, {">=", FL_T_GE},
        {"(", FL_T_LPAREN}, {")", FL_T_RPAREN}, {"[", FL_T_LBRACKET},
        {"]", FL_T_RBRACKET}, {"{", FL_T_LBRACE}, {"}", FL_T_RBRACE},
        {",", FL_T_COMMA}, {":", FL_T_COLON}, {";", FL_T_SEMI},
        {".", FL_T_DOT}
    };
    size_t i;

    for (i = 0U; i < SAG_ARRAY_LEN(ops); i++) {
        lf_open(&f, ops[i].text);
        (void)expect_kind(&f, ops[i].kind);
        (void)expect_kind(&f, FL_T_EOF);
        SAG_ASSERT_EQ_U64(f.ndiag, 0U);
        lf_close(&f);
    }
}

void test_fl_lex_at_without_bracket_names_the_pair(void)
{
    LexFix f;

    lf_open(&f, "@x");
    (void)expect_kind(&f, FL_T_ERROR);
    SAG_ASSERT(strstr(f.msg, "@[") != NULL);
    lf_close(&f);

    /* `!` alone points at the operator that does exist. */
    lf_open(&f, "!x");
    (void)expect_kind(&f, FL_T_ERROR);
    SAG_ASSERT(strstr(f.msg, "not") != NULL);
    lf_close(&f);
}

/* ---------------------------------------------------------------- */
/* §1.6 / §3 the motion token space                                 */
/* ---------------------------------------------------------------- */

void test_fl_lex_motion_units_arrows_and_counts(void)
{
    LexFix f;
    FlTok t;

    lf_open(&f, "@[l w b c]");
    (void)expect_kind(&f, FL_T_ATBRACKET);
    t = expect_kind(&f, FL_M_UNIT); SAG_ASSERT_EQ_U64(t.v.m.ch, (u64)'l');
    t = expect_kind(&f, FL_M_UNIT); SAG_ASSERT_EQ_U64(t.v.m.ch, (u64)'w');
    t = expect_kind(&f, FL_M_UNIT); SAG_ASSERT_EQ_U64(t.v.m.ch, (u64)'b');
    t = expect_kind(&f, FL_M_UNIT); SAG_ASSERT_EQ_U64(t.v.m.ch, (u64)'c');
    (void)expect_kind(&f, FL_M_END);
    (void)expect_kind(&f, FL_T_EOF);
    SAG_ASSERT_EQ_U64(f.ndiag, 0U);
    lf_close(&f);

    lf_open(&f, "@[< > ^ v]");
    (void)expect_kind(&f, FL_T_ATBRACKET);
    t = expect_kind(&f, FL_M_ARROW); SAG_ASSERT_EQ_U64(t.v.m.ch, (u64)'<');
    SAG_ASSERT(!t.v.m.alt);
    t = expect_kind(&f, FL_M_ARROW); SAG_ASSERT_EQ_U64(t.v.m.ch, (u64)'>');
    t = expect_kind(&f, FL_M_ARROW); SAG_ASSERT_EQ_U64(t.v.m.ch, (u64)'^');
    t = expect_kind(&f, FL_M_ARROW); SAG_ASSERT_EQ_U64(t.v.m.ch, (u64)'v');
    (void)expect_kind(&f, FL_M_END);
    lf_close(&f);

    /* Whitespace-insensitive: `4>` and `4 >` are the same motion. */
    lf_open(&f, "@[4> 12 <]");
    (void)expect_kind(&f, FL_T_ATBRACKET);
    t = expect_kind(&f, FL_M_COUNT); SAG_ASSERT_EQ_U64(t.v.count, 4U);
    (void)expect_kind(&f, FL_M_ARROW);
    t = expect_kind(&f, FL_M_COUNT); SAG_ASSERT_EQ_U64(t.v.count, 12U);
    (void)expect_kind(&f, FL_M_ARROW);
    (void)expect_kind(&f, FL_M_END);
    SAG_ASSERT_EQ_U64(f.ndiag, 0U);
    lf_close(&f);
}

void test_fl_lex_motion_alt_prefix_is_longest_match(void)
{
    LexFix f;
    FlTok t;

    /* `av` is alt-down... */
    lf_open(&f, "@[av a> a< a^]");
    (void)expect_kind(&f, FL_T_ATBRACKET);
    t = expect_kind(&f, FL_M_ARROW);
    SAG_ASSERT_EQ_U64(t.v.m.ch, (u64)'v'); SAG_ASSERT(t.v.m.alt);
    t = expect_kind(&f, FL_M_ARROW);
    SAG_ASSERT_EQ_U64(t.v.m.ch, (u64)'>'); SAG_ASSERT(t.v.m.alt);
    t = expect_kind(&f, FL_M_ARROW);
    SAG_ASSERT_EQ_U64(t.v.m.ch, (u64)'<'); SAG_ASSERT(t.v.m.alt);
    t = expect_kind(&f, FL_M_ARROW);
    SAG_ASSERT_EQ_U64(t.v.m.ch, (u64)'^'); SAG_ASSERT(t.v.m.alt);
    (void)expect_kind(&f, FL_M_END);
    SAG_ASSERT_EQ_U64(f.ndiag, 0U);
    lf_close(&f);

    /* ...but `avx` is the command word `avx`, and `list` is not `l`. */
    lf_open(&f, "@[avx list yank]");
    (void)expect_kind(&f, FL_T_ATBRACKET);
    t = expect_kind(&f, FL_M_WORD);
    SAG_ASSERT_EQ_I64(strcmp(interned(&f, t.v.str_id), "avx"), 0);
    t = expect_kind(&f, FL_M_WORD);
    SAG_ASSERT_EQ_I64(strcmp(interned(&f, t.v.str_id), "list"), 0);
    t = expect_kind(&f, FL_M_WORD);
    SAG_ASSERT_EQ_I64(strcmp(interned(&f, t.v.str_id), "yank"), 0);
    (void)expect_kind(&f, FL_M_END);
    SAG_ASSERT_EQ_U64(f.ndiag, 0U);
    lf_close(&f);
}

void test_fl_lex_motion_unicode_arrow_aliases(void)
{
    LexFix f;
    FlTok t;

    /* §3: accepted on input, never emitted by the recorder. */
    lf_open(&f, "@[\xE2\x86\x92 \xE2\x86\x90 \xE2\x86\x91 \xE2\x86\x93]");
    (void)expect_kind(&f, FL_T_ATBRACKET);
    t = expect_kind(&f, FL_M_ARROW); SAG_ASSERT_EQ_U64(t.v.m.ch, (u64)'>');
    t = expect_kind(&f, FL_M_ARROW); SAG_ASSERT_EQ_U64(t.v.m.ch, (u64)'<');
    t = expect_kind(&f, FL_M_ARROW); SAG_ASSERT_EQ_U64(t.v.m.ch, (u64)'^');
    t = expect_kind(&f, FL_M_ARROW); SAG_ASSERT_EQ_U64(t.v.m.ch, (u64)'v');
    (void)expect_kind(&f, FL_M_END);
    SAG_ASSERT_EQ_U64(f.ndiag, 0U);
    lf_close(&f);

    /* The alt prefix works on an alias too. */
    lf_open(&f, "@[a\xE2\x86\x93]");
    (void)expect_kind(&f, FL_T_ATBRACKET);
    t = expect_kind(&f, FL_M_ARROW);
    SAG_ASSERT_EQ_U64(t.v.m.ch, (u64)'v'); SAG_ASSERT(t.v.m.alt);
    (void)expect_kind(&f, FL_M_END);
    lf_close(&f);
}

void test_fl_lex_motion_h_nests_without_leaving_the_mode(void)
{
    LexFix f;
    FlTok t;

    /*
     * The depth counter, not a boolean.
     *
     * With a flag the inner `)` would drop back to ordinary tokens and
     * the trailing `del` would lex as an identifier.
     */
    lf_open(&f, "@[H(2>) del]");
    (void)expect_kind(&f, FL_T_ATBRACKET);
    (void)expect_kind(&f, FL_M_H);
    (void)expect_kind(&f, FL_M_LPAREN);
    t = expect_kind(&f, FL_M_COUNT); SAG_ASSERT_EQ_U64(t.v.count, 2U);
    (void)expect_kind(&f, FL_M_ARROW);
    (void)expect_kind(&f, FL_M_RPAREN);
    (void)expect_kind(&f, FL_M_DEL);
    (void)expect_kind(&f, FL_M_END);
    (void)expect_kind(&f, FL_T_EOF);
    SAG_ASSERT_EQ_U64(f.ndiag, 0U);
    lf_close(&f);
}

void test_fl_lex_motion_insert_del_esc(void)
{
    LexFix f;
    FlTok t;

    lf_open(&f, "@[i\"hi\\n\" del esc]");
    (void)expect_kind(&f, FL_T_ATBRACKET);
    t = expect_kind(&f, FL_M_INSERT);
    SAG_ASSERT_EQ_I64(strcmp(interned(&f, t.v.str_id), "hi\n"), 0);
    (void)expect_kind(&f, FL_M_DEL);
    (void)expect_kind(&f, FL_M_ESC);
    (void)expect_kind(&f, FL_M_END);
    SAG_ASSERT_EQ_U64(f.ndiag, 0U);
    lf_close(&f);
}

void test_fl_lex_motion_words_are_ordinary_identifiers_outside(void)
{
    LexFix f;

    /* §1.6's whole point: `del` and `v` are motion words inside the
     * block and perfectly ordinary identifiers outside it. */
    lf_open(&f, "del v l");
    (void)expect_kind(&f, FL_T_IDENT);
    (void)expect_kind(&f, FL_T_IDENT);
    (void)expect_kind(&f, FL_T_IDENT);
    (void)expect_kind(&f, FL_T_EOF);
    SAG_ASSERT_EQ_U64(f.ndiag, 0U);
    lf_close(&f);
}

void test_fl_lex_motion_count_bounds(void)
{
    LexFix f;
    FlTok t;

    lf_open(&f, "@[65535>]");
    (void)expect_kind(&f, FL_T_ATBRACKET);
    t = expect_kind(&f, FL_M_COUNT);
    SAG_ASSERT_EQ_U64(t.v.count, 65535U);
    lf_close(&f);

    lf_open(&f, "@[65536>]");
    (void)expect_kind(&f, FL_T_ATBRACKET);
    (void)expect_kind(&f, FL_T_ERROR);
    SAG_ASSERT(strstr(f.msg, "65535") != NULL);
    lf_close(&f);

    lf_open(&f, "@[0>]");
    (void)expect_kind(&f, FL_T_ATBRACKET);
    (void)expect_kind(&f, FL_T_ERROR);
    SAG_ASSERT(strstr(f.msg, "zero") != NULL);
    lf_close(&f);
}

void test_fl_lex_motion_block_may_span_lines(void)
{
    LexFix f;

    /* §1.2 makes an unclosed `@[` a continuation, so a newline inside
     * the block is layout rather than a terminator. */
    lf_open(&f, "@[2>\n  del\n]");
    (void)expect_kind(&f, FL_T_ATBRACKET);
    (void)expect_kind(&f, FL_M_COUNT);
    (void)expect_kind(&f, FL_M_ARROW);
    (void)expect_kind(&f, FL_M_DEL);
    (void)expect_kind(&f, FL_M_END);
    (void)expect_kind(&f, FL_T_EOF);
    SAG_ASSERT_EQ_U64(f.ndiag, 0U);
    lf_close(&f);
}

void test_fl_lex_unterminated_motion_block(void)
{
    LexFix f;

    lf_open(&f, "@[2>");
    (void)expect_kind(&f, FL_T_ATBRACKET);
    (void)expect_kind(&f, FL_M_COUNT);
    (void)expect_kind(&f, FL_M_ARROW);
    (void)expect_kind(&f, FL_T_ERROR);
    SAG_ASSERT(strstr(f.msg, "']'") != NULL);
    lf_close(&f);
}

/* ---------------------------------------------------------------- */
/* Invalid UTF-8, and the caret block itself                        */
/* ---------------------------------------------------------------- */

void test_fl_lex_invalid_utf8_reports_once(void)
{
    LexFix f;

    /*
     * One diagnostic per RUN, not per byte: a binary file would
     * otherwise bury the useful first message under the parser's
     * twenty-error cap.
     */
    lf_open(&f, "\xFF\xFE\xFD\xFC a");
    (void)expect_kind(&f, FL_T_ERROR);
    SAG_ASSERT(strstr(f.msg, "invalid UTF-8") != NULL);
    while (fl_lex_next(&f.lx).kind != FL_T_EOF)
        ;
    SAG_ASSERT_EQ_U64(f.ndiag, 1U);
    lf_close(&f);
}

void test_fl_lex_caret_block_renders_the_source_line(void)
{
    LexFix f;
    Bytebuf out;
    FlTok t;

    /* The rendering contract Sprint 29's prerequisites call binding:
     * `path:line:col: error: msg`, the source line, then the caret. */
    lf_open(&f, "let a = 1\nlet b = @x\n");
    while ((t = fl_lex_next(&f.lx)).kind != FL_T_ERROR) {
        SAG_ASSERT(t.kind != FL_T_EOF);
    }
    bytebuf_init(&out);
    fl_diag_render(&out, &f.dc, FL_DIAG_ERROR, t.sp, "boom");
    bytebuf_push_u8(&out, (u8)'\0');
    SAG_ASSERT(strstr((const char *)out.data, "t.fl:2:9: error: boom\n")
               != NULL);
    SAG_ASSERT(strstr((const char *)out.data, "let b = @x\n") != NULL);
    SAG_ASSERT(strstr((const char *)out.data, "\n        ^") != NULL);
    bytebuf_free(&out);
    lf_close(&f);
}

void test_fl_lex_spellings_cover_every_kind(void)
{
    int k;

    /*
     * Every kind has a spelling, because `expected X, found Y` reaches
     * for one on whatever the parser happened to stop at.  A missing arm
     * would surface as "invalid token" inside an otherwise precise
     * message, so the gap is asserted here rather than discovered in a
     * golden.
     */
    for (k = 0; k < (int)FL_T_KIND__N; k++) {
        const char *s = fl_tok_spelling((FlTokKind)k);

        SAG_ASSERT_NOT_NULL(s);
        if (k != (int)FL_T_ERROR && k != (int)FL_T_KIND__N)
            SAG_ASSERT_EQ_I64(strcmp(s, "invalid token") == 0, 0);
    }
}
