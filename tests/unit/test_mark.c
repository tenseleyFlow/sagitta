#define _POSIX_C_SOURCE 200809L

#include "harness.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

#include "text/journal.h"
#include "text/mark.h"

typedef struct {
    MarkId id;
    u64 pos;
    MarkBias bias;
} OracleMark;

typedef struct {
    MarkId ids[8];
    u64 rel[8];
    size_t len;
} CollapseLog;

static void record_collapse(void *ctx, MarkId id, u64 rel_off)
{
    CollapseLog *log = ctx;

    SAG_ASSERT(log->len < SAG_ARRAY_LEN(log->ids));
    log->ids[log->len] = id;
    log->rel[log->len] = rel_off;
    log->len++;
}

static void assert_adjust(u8 op, u64 at, u64 len, u64 pos, MarkBias bias,
                          u64 expected)
{
    MarkSet *ms = sag_marks_new();
    MarkId id = sag_mark_add(ms, BYTEOFF(pos), bias);

    sag_marks_adjust(ms, op, BYTEOFF(at), len);
    SAG_ASSERT_EQ_U64(sag_mark_pos(ms, id).v, expected);
    sag_marks_free(ms);
}

void test_mark_bias_table(void)
{
    assert_adjust(SAG_JOURNAL_INS, 4U, 3U, 5U, SAG_BIAS_LEFT, 8U);
    assert_adjust(SAG_JOURNAL_INS, 4U, 3U, 5U, SAG_BIAS_RIGHT, 8U);
    assert_adjust(SAG_JOURNAL_INS, 4U, 3U, 4U, SAG_BIAS_LEFT, 4U);
    assert_adjust(SAG_JOURNAL_INS, 4U, 3U, 4U, SAG_BIAS_RIGHT, 7U);
    assert_adjust(SAG_JOURNAL_INS, 4U, 3U, 3U, SAG_BIAS_LEFT, 3U);
    assert_adjust(SAG_JOURNAL_INS, 4U, 3U, 3U, SAG_BIAS_RIGHT, 3U);

    assert_adjust(SAG_JOURNAL_DEL, 4U, 3U, 8U, SAG_BIAS_LEFT, 5U);
    assert_adjust(SAG_JOURNAL_DEL, 4U, 3U, 8U, SAG_BIAS_RIGHT, 5U);
    assert_adjust(SAG_JOURNAL_DEL, 4U, 3U, 5U, SAG_BIAS_LEFT, 4U);
    assert_adjust(SAG_JOURNAL_DEL, 4U, 3U, 5U, SAG_BIAS_RIGHT, 4U);
    assert_adjust(SAG_JOURNAL_DEL, 4U, 3U, 3U, SAG_BIAS_LEFT, 3U);
    assert_adjust(SAG_JOURNAL_DEL, 4U, 3U, 3U, SAG_BIAS_RIGHT, 3U);
}

static int stale_handle_exit(MarkSet *ms, MarkId stale, bool deleting)
{
    pid_t child;
    pid_t waited;
    int status;

    SAG_ASSERT_EQ_I64(fflush(NULL), 0);
    child = fork();
    SAG_ASSERT(child >= 0);
    if (child == 0) {
        (void)close(STDERR_FILENO);
        (void)setenv("SAG_LOG", "/dev/null", 1);
        if (deleting)
            sag_mark_del(ms, stale);
        else
            (void)sag_mark_pos(ms, stale);
        _exit(0);
    }
    do {
        waited = waitpid(child, &status, 0);
    } while (waited < 0 && errno == EINTR);
    SAG_ASSERT_EQ_I64(waited, child);
    SAG_ASSERT(WIFEXITED(status));
    return WEXITSTATUS(status);
}

void test_mark_generational_handles(void)
{
    MarkSet *ms = sag_marks_new();
    MarkId stale = sag_mark_add(ms, BYTEOFF(9U), SAG_BIAS_LEFT);
    MarkId inside_a;
    MarkId inside_b;
    MarkId edge;
    MarkId fresh;
    CollapseLog log = {0};

    sag_mark_del(ms, stale);
    fresh = sag_mark_add(ms, BYTEOFF(12U), SAG_BIAS_RIGHT);
    SAG_ASSERT_EQ_U64(fresh.id, stale.id);
    SAG_ASSERT(fresh.gen != stale.gen);
    SAG_ASSERT_EQ_U64(sag_mark_pos(ms, fresh).v, 12U);
    SAG_ASSERT_EQ_I64(stale_handle_exit(ms, stale, false), SAG_EXIT_BUG);
    SAG_ASSERT_EQ_I64(stale_handle_exit(ms, stale, true), SAG_EXIT_BUG);

    inside_a = sag_mark_add(ms, BYTEOFF(5U), SAG_BIAS_LEFT);
    inside_b = sag_mark_add(ms, BYTEOFF(7U), SAG_BIAS_RIGHT);
    edge = sag_mark_add(ms, BYTEOFF(4U), SAG_BIAS_RIGHT);
    sag_marks_observe_collapse(ms, (Span){4U, 8U}, record_collapse, &log);
    SAG_ASSERT_EQ_U64(log.len, 3U);
    SAG_ASSERT_EQ_U64(log.ids[0].id, edge.id);
    SAG_ASSERT_EQ_U64(log.rel[0], 0U);
    SAG_ASSERT_EQ_U64(log.ids[1].id, inside_a.id);
    SAG_ASSERT_EQ_U64(log.ids[1].gen, inside_a.gen);
    SAG_ASSERT_EQ_U64(log.rel[1], 1U);
    SAG_ASSERT_EQ_U64(log.ids[2].id, inside_b.id);
    SAG_ASSERT_EQ_U64(log.rel[2], 3U);
    sag_marks_adjust(ms, SAG_JOURNAL_DEL, BYTEOFF(4U), 4U);
    SAG_ASSERT_EQ_U64(sag_mark_pos(ms, inside_a).v, 4U);
    SAG_ASSERT_EQ_U64(sag_mark_pos(ms, inside_b).v, 4U);
    SAG_ASSERT_EQ_U64(sag_mark_pos(ms, edge).v, 4U);
    sag_marks_adjust(ms, SAG_JOURNAL_INS, BYTEOFF(4U), 4U);
    SAG_ASSERT_EQ_U64(sag_mark_pos(ms, inside_a).v, 4U);
    SAG_ASSERT_EQ_U64(sag_mark_pos(ms, inside_b).v, 8U);
    SAG_ASSERT_EQ_U64(sag_mark_pos(ms, edge).v, 8U);
    SAG_ASSERT(sag_mark_repair(ms, edge, BYTEOFF(4U)));
    SAG_ASSERT(sag_mark_repair(ms, inside_a, BYTEOFF(5U)));
    SAG_ASSERT(sag_mark_repair(ms, inside_b, BYTEOFF(7U)));
    SAG_ASSERT_EQ_U64(sag_mark_pos(ms, edge).v, 4U);
    SAG_ASSERT_EQ_U64(sag_mark_pos(ms, inside_a).v, 5U);
    SAG_ASSERT_EQ_U64(sag_mark_pos(ms, inside_b).v, 7U);
    SAG_ASSERT(!sag_mark_repair(ms, stale, BYTEOFF(99U)));
    SAG_ASSERT_EQ_U64(sag_mark_pos(ms, fresh).v, 12U);
    sag_marks_free(ms);
}

static u64 mark_random(u64 *state)
{
    u64 x = *state;

    x ^= x >> 12U;
    x ^= x << 25U;
    x ^= x >> 27U;
    *state = x;
    return x * UINT64_C(2685821657736338717);
}

static void oracle_adjust(OracleMark *marks, size_t count, u8 op, u64 at,
                          u64 len)
{
    size_t i;

    for (i = 0U; i < count; i++) {
        if (op == SAG_JOURNAL_INS) {
            if (marks[i].pos > at ||
                (marks[i].pos == at && marks[i].bias == SAG_BIAS_RIGHT))
                marks[i].pos += len;
        } else if (marks[i].pos < at) {
            continue;
        } else if (marks[i].pos < at + len) {
            marks[i].pos = at;
        } else {
            marks[i].pos -= len;
        }
    }
}

void test_mark_random_oracle(void)
{
    enum { MARK_COUNT = 10000, EDIT_COUNT = 10000 };
    OracleMark *oracle = sag_xmalloc(sizeof(*oracle) * MARK_COUNT);
    MarkSet *ms = sag_marks_new();
    u64 state = UINT64_C(0x243f6a8885a308d3);
    u64 text_len = 100000U;
    size_t i;

    for (i = 0U; i < MARK_COUNT; i++) {
        oracle[i].pos = mark_random(&state) % (text_len + 1U);
        oracle[i].bias = (mark_random(&state) & 1U) != 0U
                             ? SAG_BIAS_RIGHT
                             : SAG_BIAS_LEFT;
        oracle[i].id = sag_mark_add(ms, BYTEOFF(oracle[i].pos),
                                    oracle[i].bias);
    }
    for (i = 0U; i < EDIT_COUNT; i++) {
        u8 op;
        u64 at;
        u64 len;
        size_t j;
        bool equal = true;

        if ((mark_random(&state) & 1U) == 0U || text_len == 0U) {
            op = SAG_JOURNAL_INS;
            at = mark_random(&state) % (text_len + 1U);
            len = mark_random(&state) % 17U;
            text_len += len;
        } else {
            op = SAG_JOURNAL_DEL;
            at = mark_random(&state) % text_len;
            len = 1U + mark_random(&state) % (text_len - at);
            if (len > 16U)
                len = 16U;
            text_len -= len;
        }
        sag_marks_adjust(ms, op, BYTEOFF(at), len);
        oracle_adjust(oracle, MARK_COUNT, op, at, len);
        for (j = 0U; j < MARK_COUNT; j++) {
            if (sag_mark_pos(ms, oracle[j].id).v != oracle[j].pos) {
                equal = false;
                break;
            }
        }
        SAG_ASSERT(equal);
    }
    sag_marks_free(ms);
    free(oracle);
}
