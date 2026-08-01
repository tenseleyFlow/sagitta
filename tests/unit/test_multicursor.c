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
#include "text/mark.h"
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
    assert_cursor(&collapse.curs.data[0], 6U, 6U, 70U);

    sag_cset_free(&collapse);
    sag_cset_free(&deletion);
    sag_cset_free(&before);
    sag_cset_free(&insert);
}

static u64 multicursor_random(u64 *state)
{
    u64 x = *state;

    x ^= x >> 12U;
    x ^= x << 25U;
    x ^= x >> 27U;
    *state = x;
    return x * UINT64_C(2685821657736338717);
}

static u64 multicursor_oracle_adjust(u64 pos, bool right_bias, u8 op,
                                     u64 at, u64 len)
{
    if (op == SAG_JOURNAL_INS) {
        if (pos > at || (pos == at && right_bias))
            pos += len;
    } else if (pos >= at + len) {
        pos -= len;
    } else if (pos >= at) {
        pos = at;
    }
    return pos;
}

void test_multicursor_adjust_random_oracle(void)
{
    CursorSet set;
    u64 state = UINT64_C(0xd1b54a32d192ed03);
    u64 text_len = 1000U;
    u64 pos = 750U;
    u64 anchor = 250U;
    u64 i;

    sag_cset_init(&set, test_cursor(pos, anchor, 17U));
    for (i = 0U; i < 10000U; i++) {
        u8 op;
        u64 at;
        u64 len;

        if (text_len == 0U || (multicursor_random(&state) & 1U) == 0U) {
            op = SAG_JOURNAL_INS;
            at = multicursor_random(&state) % (text_len + 1U);
            len = multicursor_random(&state) % 9U;
            text_len += len;
        } else {
            op = SAG_JOURNAL_DEL;
            at = multicursor_random(&state) % text_len;
            len = 1U + multicursor_random(&state) % (text_len - at);
            if (len > 8U)
                len = 8U;
            text_len -= len;
        }
        pos = multicursor_oracle_adjust(pos, true, op, at, len);
        anchor = multicursor_oracle_adjust(anchor, false, op, at, len);
        sag_cset_adjust(&set, op, BYTEOFF(at), len);
        SAG_ASSERT_EQ_U64(set.curs.len, 1U);
        SAG_ASSERT_EQ_U64(set.primary, 0U);
        assert_cursor(&set.curs.data[0], pos, anchor, 17U);
    }
    sag_cset_free(&set);
}

static void integrated_move(const TextBuf *tb, CursorSet *set, u64 motion)
{
    size_t i;

    for (i = 0U; i < set->curs.len; i++) {
        Cursor *cursor = &set->curs.data[i];

        switch (motion) {
        case 0U: sag_cursor_left(tb, cursor); break;
        case 1U: sag_cursor_right(tb, cursor); break;
        case 2U: sag_cursor_up(tb, cursor); break;
        case 3U: sag_cursor_down(tb, cursor); break;
        case 4U: sag_cursor_line_home(tb, cursor); break;
        default: sag_cursor_line_end(tb, cursor); break;
        }
    }
    sag_cset_normalize(tb, set);
}

static void integrated_assert(const TextBuf *tb, const CursorSet *set,
                              const MarkSet *marks, const MarkId *ids,
                              size_t mark_count)
{
    size_t i;

    sag_cset_check(set);
    SAG_ASSERT(set->curs.len != 0U);
    SAG_ASSERT((size_t)set->primary < set->curs.len);
    for (i = 0U; i < set->curs.len; i++) {
        SAG_ASSERT(sag_is_grapheme_boundary(tb, set->curs.data[i].pos));
        SAG_ASSERT(sag_is_grapheme_boundary(tb,
                                            set->curs.data[i].anchor));
    }
    for (i = 0U; i < mark_count; i++) {
        ByteOff pos = sag_mark_pos(marks, ids[i]);

        SAG_ASSERT(pos.v <= sag_textbuf_len(tb));
        SAG_ASSERT(sag_is_grapheme_boundary(tb, pos));
    }
}

void test_multicursor_integrated_edit_motion_fuzz(void)
{
    static const u64 seeds[] = {
        1U, UINT64_C(0x243f6a8885a308d3),
        UINT64_C(0x9e3779b97f4a7c15), UINT64_C(0xd1b54a32d192ed03)
    };
    static const u8 initial[] = "alpha\nbeta\ngamma\n";
    enum { MARK_COUNT = 24 };
    size_t seed_i;

    for (seed_i = 0U; seed_i < SAG_ARRAY_LEN(seeds); seed_i++) {
        TextBuf *tb = sag_textbuf_from_bytes(initial, sizeof(initial) - 1U);
        MarkSet *marks = sag_marks_new();
        MarkId ids[MARK_COUNT];
        CursorSet set;
        u64 state = seeds[seed_i];
        size_t i;

        sag_cset_init(&set, test_cursor(0U, 0U, 0U));
        SAG_ASSERT(sag_cset_add(&set, test_cursor(6U, 6U, 0U)));
        SAG_ASSERT(sag_cset_add(&set, test_cursor(11U, 11U, 0U)));
        for (i = 0U; i < MARK_COUNT; i++) {
            u64 pos = (u64)i % (sag_textbuf_len(tb) + 1U);

            ids[i] = sag_mark_add(marks, BYTEOFF(pos),
                                  (i & 1U) == 0U ? SAG_BIAS_LEFT
                                                : SAG_BIAS_RIGHT);
        }

        for (i = 0U; i < 10000U; i++) {
            u64 action = multicursor_random(&state) % 12U;

            if (action < 6U) {
                integrated_move(tb, &set, action);
            } else if (action == 6U || action == 7U) {
                static const u8 payloads[] = {'x', '\n'};
                ByteOff at = set.curs.data[set.primary].pos;
                const u8 *payload = &payloads[action - 6U];

                sag_textbuf_insert(tb, at, payload, 1U);
                sag_marks_adjust(marks, SAG_JOURNAL_INS, at, 1U);
                sag_cset_adjust(&set, SAG_JOURNAL_INS, at, 1U);
                sag_cset_normalize(tb, &set);
            } else if (action == 8U) {
                ByteOff at = set.curs.data[set.primary].pos;

                if (at.v < sag_textbuf_len(tb)) {
                    ByteOff end = sag_grapheme_next_boundary(tb, at);
                    u64 len = end.v - at.v;

                    sag_textbuf_delete(tb, (Span){at.v, end.v});
                    sag_marks_adjust(marks, SAG_JOURNAL_DEL, at, len);
                    sag_cset_adjust(&set, SAG_JOURNAL_DEL, at, len);
                    sag_cset_normalize(tb, &set);
                }
            } else if (action == 9U && set.curs.len < 16U) {
                u64 pos = multicursor_random(&state) %
                          (sag_textbuf_len(tb) + 1U);
                LineNo line = sag_textbuf_line_of(tb, BYTEOFF(pos));
                GCol goal = sag_off_to_gcol(tb,
                    sag_textbuf_line_span(tb, line), BYTEOFF(pos));

                (void)sag_cset_add(&set, test_cursor(pos, pos, goal.v));
            } else if (action == 10U && set.curs.len > 1U) {
                sag_cset_remove_all_but_primary(&set);
            } else {
                integrated_move(tb, &set,
                                multicursor_random(&state) % 6U);
            }
            integrated_assert(tb, &set, marks, ids, MARK_COUNT);
        }
        sag_cset_free(&set);
        sag_marks_free(marks);
        sag_textbuf_free(tb);
    }
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
