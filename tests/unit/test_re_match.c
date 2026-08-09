/*
 * Sprint 20 DoD 4: the golden match table.
 *
 * Every row is (pattern, input, flags, expected span, expected captures).
 * Captures are compared element-wise, not just counted — a run that gets
 * the whole match right and group 2 wrong is a failure, and that is
 * exactly the class of bug a "did it match?" test misses.
 */
#include "harness.h"

#include <stdio.h>
#include <string.h>

#include "search/regex.h"
#include "text/piece.h"
#include "util/arena.h"

#include "re_golden.h"


static void check_row(const ReRow *row, bool use_textbuf)
{
    Arena arena;
    SagReErr err = {0, NULL};
    SagRe *re;
    SagReMatch m;
    SagReInput in;
    TextBuf *tb = NULL;
    size_t ilen = strlen(row->input);
    bool got;

    arena_init(&arena);
    re = sag_re_compile(&arena, row->pat, strlen(row->pat), row->flags,
                        &err);
    if (re == NULL) {
        (void)fprintf(stderr, "compile failed for /%s/: %s\n", row->pat,
                      err.msg == NULL ? "?" : err.msg);
        SAG_ASSERT_NOT_NULL(re);
        arena_free_all(&arena);
        return;
    }
    if (use_textbuf) {
        tb = sag_textbuf_from_bytes((const u8 *)row->input, (u64)ilen);
        in = sag_re_input_textbuf(tb);
    } else {
        in = sag_re_input_bytes((const u8 *)row->input, (u64)ilen);
    }
    (void)memset(&m, 0, sizeof(m));
    got = sag_re_search(re, &in, BYTEOFF(0U), &m);
    if (got != row->match) {
        (void)fprintf(stderr,
                      "/%s/ on \"%s\"%s: expected %s, got %s\n", row->pat,
                      row->input, use_textbuf ? " [textbuf]" : "",
                      row->match ? "match" : "no match",
                      got ? "match" : "no match");
    }
    SAG_ASSERT(got == row->match);
    if (!row->match) {
        if (tb != NULL)
            sag_textbuf_free(tb);
        arena_free_all(&arena);
        return;
    }
    if (m.g[0].lo != (u64)row->lo || m.g[0].hi != (u64)row->hi)
        (void)fprintf(stderr, "/%s/ on \"%s\": span %llu..%llu, want %lld..%lld\n",
                      row->pat, row->input,
                      (unsigned long long)m.g[0].lo,
                      (unsigned long long)m.g[0].hi,
                      (long long)row->lo, (long long)row->hi);
    SAG_ASSERT_EQ_U64(m.g[0].lo, (u64)row->lo);
    SAG_ASSERT_EQ_U64(m.g[0].hi, (u64)row->hi);
    if (row->g1lo != NO) {
        SAG_ASSERT_EQ_U64(m.g[1].lo, (u64)row->g1lo);
        SAG_ASSERT_EQ_U64(m.g[1].hi, (u64)row->g1hi);
    }
    if (row->g2lo != NO) {
        SAG_ASSERT_EQ_U64(m.g[2].lo, (u64)row->g2lo);
        SAG_ASSERT_EQ_U64(m.g[2].hi, (u64)row->g2hi);
    }
    if (tb != NULL)
        sag_textbuf_free(tb);
    arena_free_all(&arena);
}

void test_re_golden_table_bytes(void)
{
    size_t i;

    /* The table is the contract; keep it above the DoD floor. */
    SAG_ASSERT(SAG_ARRAY_LEN(rows) >= 130U);
    for (i = 0U; i < SAG_ARRAY_LEN(rows); i++)
        check_row(&rows[i], false);
}

void test_re_golden_table_textbuf(void)
{
    size_t i;

    /* Same rows through the piece tree: the engine must read identically
     * through TextIter as it does over a flat array. */
    for (i = 0U; i < SAG_ARRAY_LEN(rows); i++)
        check_row(&rows[i], true);
}

void test_re_leftmost_first_is_pinned(void)
{
    Arena arena;
    SagRe *re;
    SagReMatch m;
    SagReInput in = sag_re_input_bytes((const u8 *)"ab", 2U);

    arena_init(&arena);
    /* Leftmost-FIRST, not leftmost-longest: earlier branch wins ties.
     * A POSIX engine would answer 0..2 here. */
    re = sag_re_compile(&arena, "a|ab", 4U, 0U, NULL);
    SAG_ASSERT_NOT_NULL(re);
    SAG_ASSERT(sag_re_search(re, &in, BYTEOFF(0U), &m));
    SAG_ASSERT_EQ_U64(m.g[0].hi, 1U);

    re = sag_re_compile(&arena, "ab|a", 4U, 0U, NULL);
    SAG_ASSERT_NOT_NULL(re);
    SAG_ASSERT(sag_re_search(re, &in, BYTEOFF(0U), &m));
    SAG_ASSERT_EQ_U64(m.g[0].hi, 2U);
    arena_free_all(&arena);
}

void test_re_empty_width_loop_terminates(void)
{
    Arena arena;
    SagRe *re;
    SagReMatch m;
    SagReInput in = sag_re_input_bytes((const u8 *)"aaaa", 4U);

    /*
     * DoD 9.  `(a*)*` chains SPLIT/JMP without consuming input.  This
     * completes only because addthread's sparse_has check lives INSIDE
     * its loop; moving that check to the call site makes this test hang
     * rather than fail, which is why it is called out by name.
     */
    arena_init(&arena);
    re = sag_re_compile(&arena, "(a*)*", 5U, 0U, NULL);
    SAG_ASSERT_NOT_NULL(re);
    SAG_ASSERT(sag_re_search(re, &in, BYTEOFF(0U), &m));
    SAG_ASSERT_EQ_U64(m.g[0].lo, 0U);
    SAG_ASSERT_EQ_U64(m.g[0].hi, 4U);

    re = sag_re_compile(&arena, "(|a)*", 5U, 0U, NULL);
    SAG_ASSERT_NOT_NULL(re);
    SAG_ASSERT(sag_re_search(re, &in, BYTEOFF(0U), &m));

    re = sag_re_compile(&arena, "(a?)*b", 6U, 0U, NULL);
    SAG_ASSERT_NOT_NULL(re);
    SAG_ASSERT(!sag_re_search(re, &in, BYTEOFF(0U), &m));
    arena_free_all(&arena);
}

void test_re_match_at_is_anchored(void)
{
    Arena arena;
    SagRe *re;
    SagReMatch m;
    SagReInput in = sag_re_input_bytes((const u8 *)"xxabc", 5U);

    arena_init(&arena);
    re = sag_re_compile(&arena, "abc", 3U, 0U, NULL);
    SAG_ASSERT_NOT_NULL(re);
    /* search finds it; match_at only succeeds at the exact offset. */
    SAG_ASSERT(sag_re_search(re, &in, BYTEOFF(0U), &m));
    SAG_ASSERT_EQ_U64(m.g[0].lo, 2U);
    SAG_ASSERT(!sag_re_match_at(re, &in, BYTEOFF(0U), &m));
    SAG_ASSERT(!sag_re_match_at(re, &in, BYTEOFF(1U), &m));
    SAG_ASSERT(sag_re_match_at(re, &in, BYTEOFF(2U), &m));
    SAG_ASSERT_EQ_U64(m.g[0].hi, 5U);
    arena_free_all(&arena);
}

void test_re_search_from_offset_and_back(void)
{
    Arena arena;
    SagRe *re;
    SagReMatch m;
    SagReInput in = sag_re_input_bytes((const u8 *)"a b a b a", 9U);

    arena_init(&arena);
    re = sag_re_compile(&arena, "a", 1U, 0U, NULL);
    SAG_ASSERT_NOT_NULL(re);
    SAG_ASSERT(sag_re_search(re, &in, BYTEOFF(0U), &m));
    SAG_ASSERT_EQ_U64(m.g[0].lo, 0U);
    SAG_ASSERT(sag_re_search(re, &in, BYTEOFF(1U), &m));
    SAG_ASSERT_EQ_U64(m.g[0].lo, 4U);
    SAG_ASSERT(sag_re_search(re, &in, BYTEOFF(5U), &m));
    SAG_ASSERT_EQ_U64(m.g[0].lo, 8U);
    SAG_ASSERT(!sag_re_search(re, &in, BYTEOFF(9U), &m));
    /* Backward search returns the last match strictly before the point. */
    SAG_ASSERT(sag_re_search_back(re, &in, BYTEOFF(9U), &m));
    SAG_ASSERT_EQ_U64(m.g[0].lo, 8U);
    SAG_ASSERT(sag_re_search_back(re, &in, BYTEOFF(8U), &m));
    SAG_ASSERT_EQ_U64(m.g[0].lo, 4U);
    SAG_ASSERT(sag_re_search_back(re, &in, BYTEOFF(1U), &m));
    SAG_ASSERT_EQ_U64(m.g[0].lo, 0U);
    SAG_ASSERT(!sag_re_search_back(re, &in, BYTEOFF(0U), &m));
    arena_free_all(&arena);
}

void test_re_group_count_and_min_len(void)
{
    Arena arena;
    SagRe *re;

    arena_init(&arena);
    re = sag_re_compile(&arena, "a(b)(c)", 7U, 0U, NULL);
    SAG_ASSERT_NOT_NULL(re);
    SAG_ASSERT_EQ_U64(sag_re_group_count(re), 3U);   /* incl. group 0 */
    SAG_ASSERT_EQ_U64(sag_re_min_len(re), 3U);

    re = sag_re_compile(&arena, "a*", 2U, 0U, NULL);
    SAG_ASSERT_EQ_U64(sag_re_min_len(re), 0U);
    re = sag_re_compile(&arena, "ab|c", 4U, 0U, NULL);
    SAG_ASSERT_EQ_U64(sag_re_min_len(re), 1U);
    re = sag_re_compile(&arena, "a{3,5}", 6U, 0U, NULL);
    SAG_ASSERT_EQ_U64(sag_re_min_len(re), 3U);
    arena_free_all(&arena);
}
