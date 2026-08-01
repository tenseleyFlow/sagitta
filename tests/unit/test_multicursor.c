#define _POSIX_C_SOURCE 200809L

#include "harness.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "edit/multicursor.h"
#include "text/journal.h"
#include "util/buf.h"

static Cursor test_cursor(u64 pos, u64 anchor, u64 goal)
{
    Cursor cursor;

    cursor.pos = BYTEOFF(pos);
    cursor.anchor = BYTEOFF(anchor);
    cursor.goal_col = (GCol){goal};
    return cursor;
}

static void assert_cursor(const Cursor *cursor, u64 pos, u64 anchor,
                          u64 goal)
{
    SAG_ASSERT_EQ_U64(cursor->pos.v, pos);
    SAG_ASSERT_EQ_U64(cursor->anchor.v, anchor);
    SAG_ASSERT_EQ_U64(cursor->goal_col.v, goal);
}

void test_multicursor_lifecycle_and_point_dedupe(void)
{
    CursorSet set;

    sag_cset_init(&set, test_cursor(10U, 10U, 4U));
    SAG_ASSERT_EQ_U64(set.curs.len, 1U);
    SAG_ASSERT_EQ_U64(set.primary, 0U);
    SAG_ASSERT(sag_cset_add(&set, test_cursor(5U, 5U, 2U)));
    SAG_ASSERT_EQ_U64(set.curs.len, 2U);
    SAG_ASSERT_EQ_U64(set.primary, 1U);
    assert_cursor(&set.curs.data[0], 5U, 5U, 2U);
    assert_cursor(&set.curs.data[1], 10U, 10U, 4U);

    SAG_ASSERT(!sag_cset_add(&set, test_cursor(10U, 10U, 99U)));
    SAG_ASSERT_EQ_U64(set.curs.len, 2U);
    SAG_ASSERT_EQ_U64(set.primary, 1U);
    assert_cursor(&set.curs.data[1], 10U, 10U, 4U);
    SAG_ASSERT(!sag_cset_add(&set, test_cursor(5U, 5U, 77U)));
    assert_cursor(&set.curs.data[0], 5U, 5U, 2U);
    sag_cset_check(&set);

    sag_cset_free(&set);
    SAG_ASSERT_NULL(set.curs.data);
    SAG_ASSERT_EQ_U64(set.curs.len, 0U);
    SAG_ASSERT_EQ_U64(set.curs.cap, 0U);
}

void test_multicursor_selection_union_semantics(void)
{
    CursorSet primary_union;
    CursorSet earlier_union;
    CursorSet chained_union;

    sag_cset_init(&primary_union, test_cursor(8U, 2U, 80U));
    SAG_ASSERT(!sag_cset_add(&primary_union,
                             test_cursor(5U, 10U, 50U)));
    SAG_ASSERT_EQ_U64(primary_union.curs.len, 1U);
    SAG_ASSERT_EQ_U64(primary_union.primary, 0U);
    assert_cursor(&primary_union.curs.data[0], 10U, 2U, 80U);

    sag_cset_init(&earlier_union, test_cursor(100U, 100U, 100U));
    SAG_ASSERT(sag_cset_add(&earlier_union, test_cursor(8U, 2U, 80U)));
    SAG_ASSERT(!sag_cset_add(&earlier_union,
                             test_cursor(5U, 12U, 50U)));
    SAG_ASSERT_EQ_U64(earlier_union.curs.len, 2U);
    SAG_ASSERT_EQ_U64(earlier_union.primary, 1U);
    assert_cursor(&earlier_union.curs.data[0], 2U, 12U, 50U);
    assert_cursor(&earlier_union.curs.data[1], 100U, 100U, 100U);

    /* This pins interval-component merging when a later union expands
     * backwards over a point that did not overlap the first selection. */
    sag_cset_init(&chained_union, test_cursor(200U, 200U, 200U));
    SAG_ASSERT(sag_cset_add(&chained_union, test_cursor(5U, 5U, 5U)));
    SAG_ASSERT(sag_cset_add(&chained_union, test_cursor(10U, 100U, 10U)));
    SAG_ASSERT(!sag_cset_add(&chained_union,
                             test_cursor(50U, 0U, 50U)));
    SAG_ASSERT_EQ_U64(chained_union.curs.len, 2U);
    assert_cursor(&chained_union.curs.data[0], 100U, 0U, 5U);
    assert_cursor(&chained_union.curs.data[1], 200U, 200U, 200U);

    sag_cset_free(&chained_union);
    sag_cset_free(&earlier_union);
    sag_cset_free(&primary_union);
}

void test_multicursor_touch_and_adjacency(void)
{
    CursorSet touching;
    CursorSet adjacent;
    CursorSet edge_point;

    sag_cset_init(&touching, test_cursor(2U, 0U, 2U));
    SAG_ASSERT(!sag_cset_add(&touching, test_cursor(4U, 2U, 4U)));
    assert_cursor(&touching.curs.data[0], 4U, 0U, 2U);

    sag_cset_init(&adjacent, test_cursor(1U, 0U, 1U));
    SAG_ASSERT(sag_cset_add(&adjacent, test_cursor(3U, 2U, 3U)));
    SAG_ASSERT_EQ_U64(adjacent.curs.len, 2U);
    assert_cursor(&adjacent.curs.data[0], 1U, 0U, 1U);
    assert_cursor(&adjacent.curs.data[1], 3U, 2U, 3U);

    sag_cset_init(&edge_point, test_cursor(0U, 0U, 7U));
    SAG_ASSERT(!sag_cset_add(&edge_point, test_cursor(4U, 0U, 4U)));
    SAG_ASSERT_EQ_U64(edge_point.curs.len, 1U);
    assert_cursor(&edge_point.curs.data[0], 4U, 0U, 7U);

    sag_cset_free(&edge_point);
    sag_cset_free(&adjacent);
    sag_cset_free(&touching);
}

static void build_permutation(CursorSet *set, bool reverse)
{
    Cursor a = test_cursor(12U, 4U, 12U);
    Cursor b = test_cursor(10U, 15U, 10U);
    Cursor c = test_cursor(40U, 40U, 40U);

    sag_cset_init(set, test_cursor(30U, 30U, 30U));
    if (reverse) {
        (void)sag_cset_add(set, c);
        (void)sag_cset_add(set, b);
        (void)sag_cset_add(set, a);
    } else {
        (void)sag_cset_add(set, a);
        (void)sag_cset_add(set, b);
        (void)sag_cset_add(set, c);
    }
}

void test_multicursor_permutation_determinism(void)
{
    CursorSet forward;
    CursorSet reverse;
    size_t i;

    build_permutation(&forward, false);
    build_permutation(&reverse, true);
    SAG_ASSERT_EQ_U64(forward.curs.len, reverse.curs.len);
    SAG_ASSERT_EQ_U64(forward.primary, reverse.primary);
    for (i = 0U; i < forward.curs.len; i++) {
        assert_cursor(&forward.curs.data[i], reverse.curs.data[i].pos.v,
                      reverse.curs.data[i].anchor.v,
                      reverse.curs.data[i].goal_col.v);
    }
    sag_cset_free(&reverse);
    sag_cset_free(&forward);
}

void test_multicursor_adjust_bias_and_merge(void)
{
    CursorSet insert;
    CursorSet before;
    CursorSet deletion;
    CursorSet collapse;

    sag_cset_init(&insert, test_cursor(5U, 5U, 5U));
    sag_cset_adjust(&insert, SAG_JOURNAL_INS, BYTEOFF(5U), 3U);
    assert_cursor(&insert.curs.data[0], 8U, 5U, 5U);
    sag_cset_adjust(&insert, SAG_JOURNAL_INS, BYTEOFF(4U), 2U);
    assert_cursor(&insert.curs.data[0], 10U, 7U, 5U);

    sag_cset_init(&before, test_cursor(3U, 2U, 3U));
    sag_cset_adjust(&before, SAG_JOURNAL_INS, BYTEOFF(4U), 9U);
    assert_cursor(&before.curs.data[0], 3U, 2U, 3U);

    sag_cset_init(&deletion, test_cursor(12U, 5U, 12U));
    sag_cset_adjust(&deletion, SAG_JOURNAL_DEL, BYTEOFF(6U), 3U);
    assert_cursor(&deletion.curs.data[0], 9U, 5U, 12U);
    sag_cset_adjust(&deletion, SAG_JOURNAL_DEL, BYTEOFF(4U), 3U);
    assert_cursor(&deletion.curs.data[0], 6U, 4U, 12U);
    sag_cset_adjust(&deletion, SAG_JOURNAL_DEL, BYTEOFF(20U), 2U);
    assert_cursor(&deletion.curs.data[0], 6U, 4U, 12U);

    sag_cset_init(&collapse, test_cursor(9U, 9U, 90U));
    SAG_ASSERT(sag_cset_add(&collapse, test_cursor(7U, 7U, 70U)));
    sag_cset_adjust(&collapse, SAG_JOURNAL_DEL, BYTEOFF(6U), 4U);
    SAG_ASSERT_EQ_U64(collapse.curs.len, 1U);
    SAG_ASSERT_EQ_U64(collapse.primary, 0U);
    assert_cursor(&collapse.curs.data[0], 6U, 6U, 90U);

    sag_cset_free(&collapse);
    sag_cset_free(&deletion);
    sag_cset_free(&before);
    sag_cset_free(&insert);
}

void test_multicursor_remove_and_normalize_clamp(void)
{
    TextBuf *tb = sag_textbuf_from_bytes((const u8 *)"abcd", 4U);
    CursorSet set;

    sag_cset_init(&set, test_cursor(99U, 99U, 9U));
    SAG_ASSERT(sag_cset_add(&set, test_cursor(1U, 1U, 1U)));
    SAG_ASSERT(sag_cset_add(&set, test_cursor(3U, 3U, 3U)));
    sag_cset_normalize(tb, &set);
    SAG_ASSERT_EQ_U64(set.curs.len, 3U);
    SAG_ASSERT_EQ_U64(set.primary, 2U);
    assert_cursor(&set.curs.data[2], 4U, 4U, 9U);

    sag_cset_remove_all_but_primary(&set);
    SAG_ASSERT_EQ_U64(set.curs.len, 1U);
    SAG_ASSERT_EQ_U64(set.primary, 0U);
    assert_cursor(&set.curs.data[0], 4U, 4U, 9U);
    sag_cset_free(&set);
    sag_textbuf_free(tb);
}

void test_multicursor_edit_guard_names_sprint17(void)
{
    CursorSet set;
    Bytebuf output;
    int pipefd[2];
    pid_t child;
    pid_t waited;
    int status;
    ssize_t count;
    u8 chunk[256];

    sag_cset_init(&set, test_cursor(1U, 1U, 1U));
    SAG_ASSERT(sag_cset_add(&set, test_cursor(2U, 2U, 2U)));
    bytebuf_init(&output);
    SAG_ASSERT_EQ_I64(fflush(NULL), 0);
    SAG_ASSERT_EQ_I64(pipe(pipefd), 0);
    child = fork();
    SAG_ASSERT(child >= 0);
    if (child == 0) {
        (void)close(pipefd[0]);
        if (dup2(pipefd[1], STDERR_FILENO) < 0)
            _exit(126);
        (void)close(pipefd[1]);
        (void)setenv("SAG_LOG", "/dev/null", 1);
        sag_cset_require_single_edit(&set);
        _exit(0);
    }
    (void)close(pipefd[1]);
    for (;;) {
        count = read(pipefd[0], chunk, sizeof(chunk));
        if (count > 0) {
            bytebuf_append(&output, chunk, (size_t)count);
            continue;
        }
        if (count < 0 && errno == EINTR)
            continue;
        break;
    }
    (void)close(pipefd[0]);
    do {
        waited = waitpid(child, &status, 0);
    } while (waited < 0 && errno == EINTR);

    SAG_ASSERT_EQ_I64(waited, child);
    SAG_ASSERT(WIFEXITED(status));
    SAG_ASSERT_EQ_I64(WEXITSTATUS(status), SAG_EXIT_BUG);
    bytebuf_append(&output, "", 1U);
    SAG_ASSERT(strstr((const char *)output.data, "Sprint 17") != NULL);
    bytebuf_free(&output);
    sag_cset_free(&set);
}
