/*
 * Sprint 21 §4 / DoD 6-8: replace.
 *
 * The row table below is the template and flag grammar; the tests after
 * it are the properties a table cannot express — that a whole run is
 * one undo step, that back-to-front application survives
 * length-changing replacements, and that a zero-width pattern both
 * fires and terminates.
 *
 * A zero-width pattern looping forever and `:%s/^/> /` matching nothing
 * are the two opposite failures of the same rule, so both are here.
 * (The looping example cannot be written out here: a star followed by a
 * slash would close this comment.)
 */
#include "harness.h"

#include <stdio.h>
#include <string.h>

#include "edit/ed.h"
#include "search/replace.h"
#include "search/searchui.h"
#include "text/piece.h"
#include "util/arena.h"

typedef struct RepRow {
    const char *text;
    const char *pat;
    const char *tpl;
    u32 flags;
    const char *want;
    u32 want_count;
} RepRow;

static void rep_fixture(Ed *ed, const char *text)
{
    EditCtx ec;

    sag_ed_init(ed);
    SAG_ASSERT(sag_ed_open_scratch(ed));
    ec = sag_ed_edit_ctx(ed);
    SAG_ASSERT(sag_edit_insert(&ec, BYTEOFF(0U), (const u8 *)text,
                               (u64)strlen(text)));
}

static void rep_read(const Ed *ed, Bytebuf *out)
{
    TextIter it;
    u64 len = sag_textbuf_len(ed->buffer.tb);

    bytebuf_init(out);
    if (len == 0U)
        return;
    if (!sag_textiter_begin(&it, ed->buffer.tb, BYTEOFF(0U)))
        return;
    for (;;) {
        const u8 *chunk = NULL;
        size_t n = 0U;

        if (!sag_textiter_chunk(&it, ed->buffer.tb, &chunk, &n) || n == 0U)
            break;
        bytebuf_append(out, chunk, n);
        if (!sag_textiter_advance(&it, ed->buffer.tb))
            break;
    }
}

/* Runs one substitution over the whole buffer and returns the count. */
static u32 rep_run(Ed *ed, const char *pat, const char *tpl, u32 flags)
{
    Arena arena;
    SearchOpts opts;
    SagRe *re;
    SagReplPlan plan;
    SagReplErr err;
    EditCtx ec;
    u32 n;
    u64 nlines = sag_textbuf_line_count(ed->buffer.tb);

    arena_init(&arena);
    sag_search_opts_init(&opts);
    if ((flags & SAG_SUB_ICASE) != 0U) {
        opts.ignorecase = true;
        opts.smartcase = false;
    }
    re = sag_search_compile(&arena, pat, strlen(pat), &opts, NULL);
    SAG_ASSERT_NOT_NULL(re);
    sag_repl_plan_init(&plan);
    (void)memset(&err, 0, sizeof(err));
    SAG_ASSERT(sag_repl_plan_build(&plan, re, ed->buffer.tb, LINENO(0U),
                                   LINENO(nlines == 0U ? 0U : nlines - 1U),
                                   tpl, strlen(tpl), flags, &err));
    ec = sag_ed_edit_ctx(ed);
    n = sag_repl_plan_apply(&plan, &ec);
    sag_ed_finish_edit(ed, &ec);
    sag_repl_plan_free(&plan);
    arena_free_all(&arena);
    return n;
}

#define G SAG_SUB_GLOBAL

void test_replace_rows(void)
{
    static const RepRow rows[] = {
        /* --- basics and the g flag ------------------------------- */
        {"foo bar foo\n", "foo", "X", 0U, "X bar foo\n", 1U},
        {"foo bar foo\n", "foo", "X", G, "X bar X\n", 2U},
        {"a\nb\na\n", "a", "Z", G, "Z\nb\nZ\n", 2U},
        /* Without g, one per LINE — not one per buffer. */
        {"aa\naa\n", "a", "b", 0U, "ba\nba\n", 2U},
        {"aa\naa\n", "a", "b", G, "bb\nbb\n", 4U},
        /* Deleting: an empty template is legal. */
        {"hello world\n", " world", "", 0U, "hello\n", 1U},

        /* --- template escapes ------------------------------------ */
        {"abc\n", "b", "\\0\\0", 0U, "abbc\n", 1U},
        {"ab\n", "(a)(b)", "\\2\\1", 0U, "ba\n", 1U},
        {"xy\n", "x", "\\n", 0U, "\ny\n", 1U},
        {"xy\n", "x", "\\t", 0U, "\ty\n", 1U},
        {"xy\n", "x", "\\r", 0U, "\ry\n", 1U},
        {"xy\n", "x", "\\\\", 0U, "\\y\n", 1U},
        /* `&` is LITERAL — the deliberate divergence from vim/sed. */
        {"cat\n", "cat", "&", 0U, "&\n", 1U},
        {"cat\n", "cat", "a&b", 0U, "a&b\n", 1U},
        {"cat\n", "cat", "\\0&\\0", 0U, "cat&cat\n", 1U},

        /* --- case operators -------------------------------------- */
        {"abc\n", "abc", "\\uabc", 0U, "Abc\n", 1U},
        {"ABC\n", "ABC", "\\lABC", 0U, "aBC\n", 1U},
        {"abc\n", "abc", "\\Uabc\\E!", 0U, "ABC!\n", 1U},
        {"ABC\n", "ABC", "\\LABC\\E!", 0U, "abc!\n", 1U},
        /* \U spanning a capture: the case state has to survive the
         * group expansion, not just literal template text. */
        {"foo\n", "(foo)", "\\U\\1\\E-\\1", 0U, "FOO-foo\n", 1U},
        {"foo bar\n", "(foo) (bar)", "\\U\\1 \\2", 0U, "FOO BAR\n", 1U},
        {"foo\n", "(f)(oo)", "\\u\\1\\2", 0U, "Foo\n", 1U},
        /* \u applies to the next character only, even across a group. */
        {"ab cd\n", "(ab) (cd)", "\\u\\1-\\u\\2", 0U, "Ab-Cd\n", 1U},

        /* --- anchors and zero width ------------------------------ */
        {"a\nb\n", "^", "> ", G, "> a\n> b\n", 2U},
        {"a\nb\n", "$", ";", G, "a;\nb;\n", 2U},
        {"ab\n", "x*", "-", G, "-a-b-\n", 3U},
        /* A file with no final newline still gets its last line. */
        {"a\nb", "^", "> ", G, "> a\n> b", 2U},

        /* --- CRLF ------------------------------------------------ */
        /* $ sits before the CR, so the line ending survives intact. */
        {"a\r\nb\r\n", "$", "!", G, "a!\r\nb!\r\n", 2U},

        /* --- case flags ------------------------------------------ */
        {"Foo foo\n", "foo", "X", G, "Foo X\n", 1U},
        {"Foo foo\n", "foo", "X", G | SAG_SUB_ICASE, "X X\n", 2U},

        /* --- p flag: infer case from the matched text ------------- */
        {"HELLO\n", "hello", "world", SAG_SUB_ICASE | SAG_SUB_PRESERVE,
         "WORLD\n", 1U},
        {"Hello\n", "hello", "world", SAG_SUB_ICASE | SAG_SUB_PRESERVE,
         "World\n", 1U},
        {"hello\n", "hello", "world", SAG_SUB_ICASE | SAG_SUB_PRESERVE,
         "world\n", 1U},
        /* Mixed case is "anything else": verbatim. */
        {"hELLo\n", "hello", "world", SAG_SUB_ICASE | SAG_SUB_PRESERVE,
         "world\n", 1U},
        /* One capital is not a shout: "I" titlecases, it does not
         * uppercase the whole replacement. */
        {"A\n", "a", "xy", SAG_SUB_ICASE | SAG_SUB_PRESERVE, "Xy\n", 1U},

        /* --- length-changing replacements, back to front ---------- */
        {"aaa\n", "a", "LONGER", G, "LONGERLONGERLONGER\n", 3U},
        {"abcabc\n", "abc", "z", G, "zz\n", 2U},
        {"xAx\n", "A", "", G, "xx\n", 1U},

        /* --- unicode --------------------------------------------- */
        {"\xE6\xBC\xA2\xE5\xAD\x97\n", "\xE6\xBC\xA2", "X", 0U,
         "X\xE5\xAD\x97\n", 1U},
        {"caf\xC3\xA9\n", "\xC3\xA9", "e", 0U, "cafe\n", 1U},
        /* A replacement containing multibyte text. */
        {"x\n", "x", "\xE6\xBC\xA2", 0U, "\xE6\xBC\xA2\n", 1U},

        /* --- higher group references ----------------------------- */
        {"ab\n", "(a)(b)", "\\{2}\\{1}", 0U, "ba\n", 1U},
        {"ab\n", "(a)(b)", "\\{1}", 0U, "a\n", 1U}
    };
    size_t i;

    for (i = 0U; i < SAG_ARRAY_LEN(rows); i++) {
        Ed ed;
        Bytebuf got;
        u32 n;

        rep_fixture(&ed, rows[i].text);
        n = rep_run(&ed, rows[i].pat, rows[i].tpl, rows[i].flags);
        rep_read(&ed, &got);
        if (got.len != strlen(rows[i].want) ||
            memcmp(got.data, rows[i].want, got.len) != 0)
            (void)fprintf(stderr,
                          "row %zu s/%s/%s/ on \"%s\": got \"%.*s\", "
                          "want \"%s\"\n", i, rows[i].pat, rows[i].tpl,
                          rows[i].text, (int)got.len,
                          (const char *)got.data, rows[i].want);
        SAG_ASSERT_EQ_U64(got.len, (u64)strlen(rows[i].want));
        SAG_ASSERT(memcmp(got.data, rows[i].want, got.len) == 0);
        if (n != rows[i].want_count)
            (void)fprintf(stderr, "row %zu: count %u, want %u\n", i,
                          (unsigned)n, (unsigned)rows[i].want_count);
        SAG_ASSERT_EQ_U64(n, rows[i].want_count);
        bytebuf_free(&got);
        sag_ed_free(&ed);
    }
}

/*
 * DoD 6.  The statement a user would make: however many matches were
 * replaced, ONE undo puts it all back, byte for byte.
 */
static void assert_one_undo(const char *text, const char *pat,
                            const char *tpl, u32 flags, u32 want_count)
{
    Ed ed;
    Bytebuf before;
    Bytebuf after;
    Bytebuf undone;
    EditCtx ec;
    u32 n;

    rep_fixture(&ed, text);
    rep_read(&ed, &before);
    n = rep_run(&ed, pat, tpl, flags);
    SAG_ASSERT_EQ_U64(n, want_count);
    rep_read(&ed, &after);
    /* It really did change, or the undo assertion proves nothing. */
    SAG_ASSERT(after.len != before.len ||
               memcmp(after.data, before.data, after.len) != 0);

    ec = sag_ed_edit_ctx(&ed);
    SAG_ASSERT(sag_undo(&ec));
    sag_ed_finish_edit(&ed, &ec);
    rep_read(&ed, &undone);
    SAG_ASSERT_EQ_U64(undone.len, before.len);
    SAG_ASSERT(memcmp(undone.data, before.data, undone.len) == 0);

    bytebuf_free(&before);
    bytebuf_free(&after);
    bytebuf_free(&undone);
    sag_ed_free(&ed);
}

void test_replace_whole_run_is_one_undo_step(void)
{
    assert_one_undo("foo foo foo\n", "foo", "X", G, 3U);
    assert_one_undo("a\nb\nc\n", "^", "> ", G, 3U);
    assert_one_undo("aaa\n", "a", "LONGER", G, 3U);
    assert_one_undo("abcabc\n", "abc", "z", G, 2U);
    assert_one_undo("x\n", "x", "y", 0U, 1U);
    assert_one_undo("ab\n", "x*", "-", G, 3U);
}

void test_replace_ten_thousand_matches_is_one_undo_step(void)
{
    Ed ed;
    Bytebuf before;
    Bytebuf undone;
    EditCtx ec;
    Bytebuf src;
    u32 i;
    u32 n;

    bytebuf_init(&src);
    for (i = 0U; i < 10000U; i++)
        bytebuf_append(&src, "foo\n", 4U);
    bytebuf_push_u8(&src, 0U);
    rep_fixture(&ed, (const char *)src.data);
    rep_read(&ed, &before);

    n = rep_run(&ed, "foo", "barbaz", G);
    SAG_ASSERT_EQ_U64(n, 10000U);
    SAG_ASSERT_EQ_U64(sag_textbuf_len(ed.buffer.tb), 10000U * 7U);

    ec = sag_ed_edit_ctx(&ed);
    SAG_ASSERT(sag_undo(&ec));
    sag_ed_finish_edit(&ed, &ec);
    rep_read(&ed, &undone);
    SAG_ASSERT_EQ_U64(undone.len, before.len);
    SAG_ASSERT(memcmp(undone.data, before.data, undone.len) == 0);

    bytebuf_free(&src);
    bytebuf_free(&before);
    bytebuf_free(&undone);
    sag_ed_free(&ed);
}

/* DoD 8: every unknown escape is an error, named. */
void test_replace_rejects_unknown_escapes(void)
{
    static const char *const bad[] = {"\\q", "a\\zb", "\\", "\\{", "\\{2",
                                      "\\{}"};
    size_t i;

    for (i = 0U; i < SAG_ARRAY_LEN(bad); i++) {
        SagReplErr err;

        (void)memset(&err, 0, sizeof(err));
        SAG_ASSERT(!sag_repl_check(bad[i], strlen(bad[i]), 1U, &err));
        SAG_ASSERT_NOT_NULL(err.msg);
        SAG_ASSERT(err.off <= strlen(bad[i]));
    }
    /* A group the pattern does not have is caught before any edit. */
    {
        SagReplErr err;

        (void)memset(&err, 0, sizeof(err));
        SAG_ASSERT(!sag_repl_check("\\3", 2U, 1U, &err));
        SAG_ASSERT_NOT_NULL(err.msg);
        /* ...and is fine when it does. */
        SAG_ASSERT(sag_repl_check("\\3", 2U, 4U, &err));
    }
}

/* The zero-width rule, stated as termination rather than output. */
void test_replace_zero_width_terminates(void)
{
    Ed ed;
    Bytebuf got;
    u32 n;

    /* Combining sequence: the advance must be one GRAPHEME, so the
     * inserted markers never land between e and its accent. */
    rep_fixture(&ed, "e\xCC\x81x\n");
    n = rep_run(&ed, "q*", "-", G);
    rep_read(&ed, &got);
    /*
     * Before e-acute, before x, and before the newline — three, not
     * four: the offset past the final newline belongs to a line the
     * range does not cover, the same way `x*` over "ab\n" gives three.
     * The point of the accent is the ADVANCE: stepping one byte instead
     * of one grapheme would put a marker between e and its combining
     * acute and split the cluster.
     */
    SAG_ASSERT_EQ_U64(n, 3U);
    SAG_ASSERT(memcmp(got.data, "-e\xCC\x81-x-\n", got.len) == 0);
    bytebuf_free(&got);
    sag_ed_free(&ed);
}

/* §4's confirm semantics, driven through the same walker the key
 * handler uses. */
static void confirm_case(const char *text, const char *pat,
                         const char *tpl, const char *keys,
                         const char *want)
{
    Ed ed;
    Arena arena;
    SearchOpts opts;
    SagRe *re;
    SagReplPlan plan;
    SagReplConfirm walk;
    EditCtx ec;
    Bytebuf got;
    size_t k;
    u64 nlines;

    rep_fixture(&ed, text);
    nlines = sag_textbuf_line_count(ed.buffer.tb);
    arena_init(&arena);
    sag_search_opts_init(&opts);
    re = sag_search_compile(&arena, pat, strlen(pat), &opts, NULL);
    SAG_ASSERT_NOT_NULL(re);
    sag_repl_plan_init(&plan);
    SAG_ASSERT(sag_repl_plan_build(&plan, re, ed.buffer.tb, LINENO(0U),
                                   LINENO(nlines - 1U), tpl, strlen(tpl),
                                   G, NULL));
    sag_repl_confirm_begin(&walk, &plan);
    for (k = 0U; keys[k] != '\0'; k++)
        (void)sag_repl_confirm_answer(&walk, (u8)keys[k]);
    ec = sag_ed_edit_ctx(&ed);
    (void)sag_repl_plan_apply(&plan, &ec);
    sag_ed_finish_edit(&ed, &ec);
    rep_read(&ed, &got);
    if (got.len != strlen(want) || memcmp(got.data, want, got.len) != 0)
        (void)fprintf(stderr,
                      "confirm \"%s\" on s/%s/%s/: got \"%.*s\", want "
                      "\"%s\"\n", keys, pat, tpl, (int)got.len,
                      (const char *)got.data, want);
    SAG_ASSERT_EQ_U64(got.len, (u64)strlen(want));
    SAG_ASSERT(memcmp(got.data, want, got.len) == 0);
    bytebuf_free(&got);
    sag_repl_plan_free(&plan);
    arena_free_all(&arena);
    sag_ed_free(&ed);
}

void test_replace_confirm_keys(void)
{
    /* y replaces, n skips. */
    confirm_case("a a a\n", "a", "X", "yny", "X a X\n");
    confirm_case("a a a\n", "a", "X", "nnn", "a a a\n");
    /* a takes this and all remaining. */
    confirm_case("a a a\n", "a", "X", "na", "a X X\n");
    /* l takes this one and stops. */
    confirm_case("a a a\n", "a", "X", "yl", "X X a\n");
    /* q stops, KEEPING what was already approved. */
    confirm_case("a a a\n", "a", "X", "yq", "X a a\n");
    /* Esc behaves as q. */
    confirm_case("a a a\n", "a", "X", "y\x1B", "X a a\n");
    /* Answering nothing changes nothing. */
    confirm_case("a a a\n", "a", "X", "", "a a a\n");
}

void test_replace_confirm_scroll_keys_are_not_answers(void)
{
    Ed ed;
    Arena arena;
    SearchOpts opts;
    SagRe *re;
    SagReplPlan plan;
    SagReplConfirm walk;

    rep_fixture(&ed, "a a\n");
    arena_init(&arena);
    sag_search_opts_init(&opts);
    re = sag_search_compile(&arena, "a", 1U, &opts, NULL);
    sag_repl_plan_init(&plan);
    SAG_ASSERT(sag_repl_plan_build(&plan, re, ed.buffer.tb, LINENO(0U),
                                   LINENO(0U), "X", 1U, G, NULL));
    sag_repl_confirm_begin(&walk, &plan);
    /* ^E and ^Y scroll the view while deciding; they must not advance
     * past the match being asked about. */
    SAG_ASSERT(!sag_repl_confirm_answer(&walk, 0x05U));
    SAG_ASSERT(!sag_repl_confirm_answer(&walk, 0x19U));
    SAG_ASSERT(sag_repl_confirm_current(&walk) == &plan.v[0]);
    SAG_ASSERT(sag_repl_confirm_pending(&walk));
    sag_repl_plan_free(&plan);
    arena_free_all(&arena);
    sag_ed_free(&ed);
}

/* A confirm run ended with q is still exactly one undo step. */
void test_replace_confirm_run_ended_with_q_is_one_undo_step(void)
{
    Ed ed;
    Arena arena;
    SearchOpts opts;
    SagRe *re;
    SagReplPlan plan;
    SagReplConfirm walk;
    EditCtx ec;
    Bytebuf before;
    Bytebuf undone;

    rep_fixture(&ed, "a a a a\n");
    rep_read(&ed, &before);
    arena_init(&arena);
    sag_search_opts_init(&opts);
    re = sag_search_compile(&arena, "a", 1U, &opts, NULL);
    sag_repl_plan_init(&plan);
    SAG_ASSERT(sag_repl_plan_build(&plan, re, ed.buffer.tb, LINENO(0U),
                                   LINENO(0U), "XY", 2U, G, NULL));
    sag_repl_confirm_begin(&walk, &plan);
    (void)sag_repl_confirm_answer(&walk, (u8)'y');
    (void)sag_repl_confirm_answer(&walk, (u8)'y');
    (void)sag_repl_confirm_answer(&walk, (u8)'q');
    ec = sag_ed_edit_ctx(&ed);
    SAG_ASSERT_EQ_U64(sag_repl_plan_apply(&plan, &ec), 2U);
    sag_ed_finish_edit(&ed, &ec);

    ec = sag_ed_edit_ctx(&ed);
    SAG_ASSERT(sag_undo(&ec));
    sag_ed_finish_edit(&ed, &ec);
    rep_read(&ed, &undone);
    SAG_ASSERT_EQ_U64(undone.len, before.len);
    SAG_ASSERT(memcmp(undone.data, before.data, undone.len) == 0);

    bytebuf_free(&before);
    bytebuf_free(&undone);
    sag_repl_plan_free(&plan);
    arena_free_all(&arena);
    sag_ed_free(&ed);
}

/* The range is honoured, and `^` is still measured against the buffer
 * rather than the range — the Sprint 20 window trap, restated. */
void test_replace_range_does_not_move_the_anchors(void)
{
    Ed ed;
    Arena arena;
    SearchOpts opts;
    SagRe *re;
    SagReplPlan plan;
    EditCtx ec;
    Bytebuf got;

    rep_fixture(&ed, "a\nb\nc\nd\n");
    arena_init(&arena);
    sag_search_opts_init(&opts);
    /* \A matches only at the start of the BUFFER, so a range starting
     * at line 1 must find nothing at all. */
    re = sag_search_compile(&arena, "\\A", 2U, &opts, NULL);
    sag_repl_plan_init(&plan);
    SAG_ASSERT(sag_repl_plan_build(&plan, re, ed.buffer.tb, LINENO(1U),
                                   LINENO(2U), "X", 1U, G, NULL));
    SAG_ASSERT_EQ_U64(plan.len, 0U);
    sag_repl_plan_free(&plan);

    /* And ^ inside the range hits exactly the range's lines. */
    re = sag_search_compile(&arena, "^", 1U, &opts, NULL);
    sag_repl_plan_init(&plan);
    SAG_ASSERT(sag_repl_plan_build(&plan, re, ed.buffer.tb, LINENO(1U),
                                   LINENO(2U), "> ", 2U, G, NULL));
    SAG_ASSERT_EQ_U64(plan.len, 2U);
    ec = sag_ed_edit_ctx(&ed);
    (void)sag_repl_plan_apply(&plan, &ec);
    sag_ed_finish_edit(&ed, &ec);
    rep_read(&ed, &got);
    SAG_ASSERT(memcmp(got.data, "a\n> b\n> c\nd\n", got.len) == 0);
    bytebuf_free(&got);
    sag_repl_plan_free(&plan);
    arena_free_all(&arena);
    sag_ed_free(&ed);
}

void test_replace_count_only_changes_nothing(void)
{
    Ed ed;
    Arena arena;
    SearchOpts opts;
    SagRe *re;
    SagReplPlan plan;

    rep_fixture(&ed, "a a\nb a\n");
    arena_init(&arena);
    sag_search_opts_init(&opts);
    re = sag_search_compile(&arena, "a", 1U, &opts, NULL);
    sag_repl_plan_init(&plan);
    SAG_ASSERT(sag_repl_plan_build(&plan, re, ed.buffer.tb, LINENO(0U),
                                   LINENO(1U), "X", 1U,
                                   G | SAG_SUB_COUNT_ONLY, NULL));
    /* The n flag reports what a run WOULD do; the plan is built and
     * simply never applied. */
    SAG_ASSERT_EQ_U64(plan.len, 3U);
    SAG_ASSERT_EQ_U64(plan.lines, 2U);
    SAG_ASSERT_EQ_U64(sag_textbuf_len(ed.buffer.tb), 8U);
    sag_repl_plan_free(&plan);
    arena_free_all(&arena);
    sag_ed_free(&ed);
}
