/*
 * Sprint 57.28 §1: the yank stack on its own -- joining, eviction,
 * bounds.  The editor-level behaviour (the keys, the separation from the
 * registers) is in test_readline_keys.c.
 */
#include "harness.h"

#include <stdio.h>
#include <string.h>

#include "text/yankstack.h"

static int ys_owner_a;
static int ys_owner_b;

static void ys_kill(YewYankStack *y, const char *s, YewKillDir d, u64 seq,
                    const void *owner)
{
    yew_yank_kill(y, (const u8 *)s, strlen(s), d, seq, owner);
}

static void ys_expect(const YewYankStack *y, u32 k, const char *want)
{
    const Bytebuf *entry = yew_yank_at(y, k);
    size_t len = strlen(want);

    YEW_ASSERT_NOT_NULL(entry);
    YEW_ASSERT_EQ_U64(entry->len, len);
    if (len != 0U)
        YEW_ASSERT_EQ_MEM(entry->data, want, len);
}

/* C-k C-k appends; A-<bs> A-<bs> prepends -- readline's join. */
void test_yankstack_consecutive_kills_join_by_direction(void)
{
    YewYankStack y;

    yew_yank_init(&y);
    ys_kill(&y, "one", YEW_KILL_FORWARD, 10U, &ys_owner_a);
    ys_kill(&y, " two", YEW_KILL_FORWARD, 11U, &ys_owner_a);
    YEW_ASSERT_EQ_U64(y.len, 1U);
    ys_expect(&y, 0U, "one two");

    ys_kill(&y, "gamma", YEW_KILL_BACKWARD, 20U, &ys_owner_a);
    ys_kill(&y, "beta ", YEW_KILL_BACKWARD, 21U, &ys_owner_a);
    ys_kill(&y, "alpha ", YEW_KILL_BACKWARD, 22U, &ys_owner_a);
    YEW_ASSERT_EQ_U64(y.len, 2U);
    ys_expect(&y, 0U, "alpha beta gamma");
    ys_expect(&y, 1U, "one two");
    YEW_ASSERT_EQ_U64(y.bytes, 23U);
    yew_yank_free(&y);
}

/* Anything between two kills -- a gap in the sequence -- or a kill in
 * another Win starts a new entry. */
void test_yankstack_a_gap_or_another_win_starts_a_new_entry(void)
{
    YewYankStack y;

    yew_yank_init(&y);
    ys_kill(&y, "a", YEW_KILL_FORWARD, 1U, &ys_owner_a);
    ys_kill(&y, "b", YEW_KILL_FORWARD, 3U, &ys_owner_a);
    ys_kill(&y, "c", YEW_KILL_FORWARD, 4U, &ys_owner_b);
    YEW_ASSERT_EQ_U64(y.len, 3U);
    ys_expect(&y, 0U, "c");
    ys_expect(&y, 1U, "b");
    ys_expect(&y, 2U, "a");
    yew_yank_free(&y);
}

/* A multi-cursor kill is one entry and never joins either way. */
void test_yankstack_multicursor_kill_never_extends(void)
{
    YewYankStack y;

    yew_yank_init(&y);
    ys_kill(&y, "x", YEW_KILL_FORWARD, 1U, &ys_owner_a);
    ys_kill(&y, "yz", YEW_KILL_ALONE, 2U, &ys_owner_a);
    ys_kill(&y, "w", YEW_KILL_FORWARD, 3U, &ys_owner_a);
    YEW_ASSERT_EQ_U64(y.len, 3U);
    ys_expect(&y, 0U, "w");
    ys_expect(&y, 1U, "yz");
    ys_expect(&y, 2U, "x");
    yew_yank_free(&y);
}

/* Past YEW_YANK_MAX the oldest goes; the ring wraps its slots. */
void test_yankstack_evicts_by_count(void)
{
    YewYankStack y;
    char text[16];
    u32 i;

    yew_yank_init(&y);
    for (i = 0U; i < YEW_YANK_MAX + 5U; i++) {
        int n = snprintf(text, sizeof(text), "k%u", (unsigned)i);

        YEW_ASSERT(n > 0 && (size_t)n < sizeof(text));
        ys_kill(&y, text, YEW_KILL_FORWARD, 2U * i, &ys_owner_a);
    }
    YEW_ASSERT_EQ_U64(y.len, YEW_YANK_MAX);
    ys_expect(&y, 0U, "k36");
    ys_expect(&y, YEW_YANK_MAX - 1U, "k5");
    YEW_ASSERT_NULL(yew_yank_at(&y, YEW_YANK_MAX));
    yew_yank_free(&y);
}

/* Past bytes_max the oldest go -- but never the newest, even alone over
 * the cap: what was just killed must survive. */
void test_yankstack_evicts_by_bytes_and_keeps_the_newest(void)
{
    YewYankStack y;

    yew_yank_init(&y);
    y.bytes_max = 8U;
    ys_kill(&y, "abcd", YEW_KILL_FORWARD, 1U, &ys_owner_a);
    ys_kill(&y, "efgh", YEW_KILL_FORWARD, 3U, &ys_owner_a);
    YEW_ASSERT_EQ_U64(y.len, 2U);
    ys_kill(&y, "ij", YEW_KILL_FORWARD, 5U, &ys_owner_a);
    YEW_ASSERT_EQ_U64(y.len, 2U);
    ys_expect(&y, 0U, "ij");
    ys_expect(&y, 1U, "efgh");
    YEW_ASSERT_EQ_U64(y.bytes, 6U);
    ys_kill(&y, "0123456789", YEW_KILL_FORWARD, 7U, &ys_owner_a);
    YEW_ASSERT_EQ_U64(y.len, 1U);
    ys_expect(&y, 0U, "0123456789");
    YEW_ASSERT_EQ_U64(y.bytes, 10U);
    /* A join that crosses the cap drops the older entries too. */
    ys_kill(&y, "x", YEW_KILL_FORWARD, 9U, &ys_owner_a);
    YEW_ASSERT_EQ_U64(y.len, 1U);
    ys_kill(&y, "ab", YEW_KILL_FORWARD, 20U, &ys_owner_a);
    YEW_ASSERT_EQ_U64(y.len, 2U);
    ys_kill(&y, "cdefgh", YEW_KILL_FORWARD, 21U, &ys_owner_a);
    YEW_ASSERT_EQ_U64(y.len, 1U);
    ys_expect(&y, 0U, "abcdefgh");
    YEW_ASSERT_EQ_U64(y.bytes, 8U);
    yew_yank_free(&y);
}

/* Bounds, and the empty stack; an empty kill records nothing. */
void test_yankstack_at_bounds(void)
{
    YewYankStack y;

    yew_yank_init(&y);
    YEW_ASSERT_NULL(yew_yank_at(&y, 0U));
    YEW_ASSERT_NULL(yew_yank_at(NULL, 0U));
    yew_yank_kill(&y, (const u8 *)"", 0U, YEW_KILL_FORWARD, 1U, &ys_owner_a);
    YEW_ASSERT_EQ_U64(y.len, 0U);
    ys_kill(&y, "only", YEW_KILL_FORWARD, 2U, &ys_owner_a);
    ys_expect(&y, 0U, "only");
    YEW_ASSERT_NULL(yew_yank_at(&y, 1U));
    yew_yank_free(&y);
    YEW_ASSERT_EQ_U64(y.len, 0U);
    YEW_ASSERT_EQ_U64(y.bytes, 0U);
}
