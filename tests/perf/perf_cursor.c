#define _POSIX_C_SOURCE 200809L

#include "text/cursor.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

enum {
    LONG_LINE_BYTES = 8 * 1024 * 1024,
    PERF_ROUNDS = 9
};

typedef struct {
    const char *name;
    const u8 *pattern;
    size_t pattern_len;
} PerfCase;

static volatile u64 perf_cursor_sink;

static i64 now_ns(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return -1;
    return (i64)ts.tv_sec * INT64_C(1000000000) + ts.tv_nsec;
}

static bool measure(const PerfCase *pc, i64 *worst_out)
{
    static const i64 budget_ns = INT64_C(5000000);
    u8 *bytes = malloc(LONG_LINE_BYTES);
    u8 *cross_bytes = malloc(LONG_LINE_BYTES + 2U);
    TextBuf *tb;
    TextBuf *cross_tb;
    Cursor cursor;
    u64 clusters;
    i64 worst = 0;
    size_t at;
    int round;

    if (bytes == NULL || cross_bytes == NULL) {
        free(cross_bytes);
        free(bytes);
        return false;
    }
    for (at = 0U; at < LONG_LINE_BYTES; at += pc->pattern_len)
        memcpy(bytes + at, pc->pattern, pc->pattern_len);
    memcpy(cross_bytes, bytes, LONG_LINE_BYTES);
    cross_bytes[LONG_LINE_BYTES] = '\n';
    cross_bytes[LONG_LINE_BYTES + 1U] = 'z';
    tb = sag_textbuf_from_owned_bytes(bytes, LONG_LINE_BYTES);
    cross_tb = sag_textbuf_from_owned_bytes(cross_bytes,
                                             LONG_LINE_BYTES + 2U);
    if (tb == NULL || cross_tb == NULL) {
        sag_textbuf_free(cross_tb);
        sag_textbuf_free(tb);
        return false;
    }
    clusters = LONG_LINE_BYTES / pc->pattern_len;

    for (round = 0; round < PERF_ROUNDS; round++) {
        static const u8 inserted = 'q';
        i64 start;
        i64 elapsed;
        i64 delete_elapsed;

        cursor.pos = BYTEOFF(LONG_LINE_BYTES - pc->pattern_len);
        cursor.anchor = cursor.pos;
        cursor.goal_col = (GCol){clusters - 1U};
        start = now_ns();
        if (start < 0) {
            sag_textbuf_free(tb);
            sag_textbuf_free(cross_tb);
            return false;
        }
        sag_cursor_right(tb, &cursor);
        elapsed = now_ns() - start;
        if (elapsed < 0 || cursor.pos.v != LONG_LINE_BYTES ||
            cursor.goal_col.v != clusters) {
            (void)fprintf(stderr,
                          "cursor-perf: direct-right mismatch "
                          "case=%s round=%d pos=%llu goal=%llu\n",
                          pc->name, round,
                          (unsigned long long)cursor.pos.v,
                          (unsigned long long)cursor.goal_col.v);
            sag_textbuf_free(tb);
            sag_textbuf_free(cross_tb);
            return false;
        }
        if (elapsed > worst)
            worst = elapsed;
        perf_cursor_sink = cursor.pos.v + cursor.goal_col.v;

        cursor.pos = BYTEOFF(LONG_LINE_BYTES);
        cursor.anchor = cursor.pos;
        cursor.goal_col = (GCol){clusters};
        start = now_ns();
        if (start < 0) {
            sag_textbuf_free(tb);
            sag_textbuf_free(cross_tb);
            return false;
        }
        sag_cursor_left(tb, &cursor);
        elapsed = now_ns() - start;
        if (elapsed < 0 ||
            cursor.pos.v != LONG_LINE_BYTES - pc->pattern_len ||
            cursor.goal_col.v != clusters - 1U) {
            sag_textbuf_free(tb);
            sag_textbuf_free(cross_tb);
            return false;
        }
        if (elapsed > worst)
            worst = elapsed;
        perf_cursor_sink = cursor.pos.v + cursor.goal_col.v;

        start = now_ns();
        sag_textbuf_insert(tb, BYTEOFF(LONG_LINE_BYTES), &inserted, 1U);
        elapsed = now_ns() - start;
        if (start < 0 || elapsed < 0) {
            sag_textbuf_delete(tb,
                               (Span){LONG_LINE_BYTES,
                                      LONG_LINE_BYTES + 1U});
            sag_textbuf_free(cross_tb);
            sag_textbuf_free(tb);
            return false;
        }
        if (elapsed > worst)
            worst = elapsed;
        cursor.pos = BYTEOFF(LONG_LINE_BYTES + 1U);
        cursor.anchor = cursor.pos;
        cursor.goal_col = (GCol){clusters + 1U};
        start = now_ns();
        if (start < 0) {
            sag_textbuf_delete(tb,
                               (Span){LONG_LINE_BYTES,
                                      LONG_LINE_BYTES + 1U});
            sag_textbuf_free(cross_tb);
            sag_textbuf_free(tb);
            return false;
        }
        sag_cursor_left(tb, &cursor);
        elapsed = now_ns() - start;
        if (elapsed < 0 || cursor.pos.v != LONG_LINE_BYTES ||
            cursor.goal_col.v != clusters) {
            (void)fprintf(stderr,
                          "cursor-perf: post-edit first-left mismatch "
                          "case=%s round=%d pos=%llu goal=%llu\n",
                          pc->name, round,
                          (unsigned long long)cursor.pos.v,
                          (unsigned long long)cursor.goal_col.v);
            sag_textbuf_delete(tb,
                               (Span){LONG_LINE_BYTES,
                                      LONG_LINE_BYTES + 1U});
            sag_textbuf_free(cross_tb);
            sag_textbuf_free(tb);
            return false;
        }
        if (elapsed > worst)
            worst = elapsed;

        start = now_ns();
        if (start < 0) {
            sag_textbuf_delete(tb,
                               (Span){LONG_LINE_BYTES,
                                      LONG_LINE_BYTES + 1U});
            sag_textbuf_free(cross_tb);
            sag_textbuf_free(tb);
            return false;
        }
        sag_cursor_left(tb, &cursor);
        elapsed = now_ns() - start;
        start = now_ns();
        sag_textbuf_delete(tb,
                           (Span){LONG_LINE_BYTES,
                                  LONG_LINE_BYTES + 1U});
        delete_elapsed = now_ns() - start;
        if (start < 0 || delete_elapsed < 0 || elapsed < 0 ||
            cursor.pos.v != LONG_LINE_BYTES - pc->pattern_len ||
            cursor.goal_col.v != clusters - 1U) {
            (void)fprintf(stderr,
                          "cursor-perf: post-edit second-left mismatch "
                          "case=%s round=%d pos=%llu goal=%llu\n",
                          pc->name, round,
                          (unsigned long long)cursor.pos.v,
                          (unsigned long long)cursor.goal_col.v);
            sag_textbuf_free(cross_tb);
            sag_textbuf_free(tb);
            return false;
        }
        if (elapsed > worst)
            worst = elapsed;
        if (delete_elapsed > worst)
            worst = delete_elapsed;
        perf_cursor_sink = cursor.pos.v + cursor.goal_col.v;

        cursor.pos = BYTEOFF(0U);
        cursor.anchor = cursor.pos;
        cursor.goal_col = (GCol){0U};
        start = now_ns();
        sag_cursor_line_end(tb, &cursor);
        elapsed = now_ns() - start;
        if (elapsed < 0 || cursor.pos.v != LONG_LINE_BYTES ||
            cursor.goal_col.v != SAG_GCOL_EOL) {
            sag_textbuf_free(cross_tb);
            sag_textbuf_free(tb);
            return false;
        }
        if (elapsed > worst)
            worst = elapsed;

        start = now_ns();
        sag_cursor_left(tb, &cursor);
        elapsed = now_ns() - start;
        if (elapsed < 0 ||
            cursor.pos.v != LONG_LINE_BYTES - pc->pattern_len ||
            cursor.goal_col.v != clusters - 1U) {
            sag_textbuf_free(cross_tb);
            sag_textbuf_free(tb);
            return false;
        }
        if (elapsed > worst)
            worst = elapsed;

        cursor.pos = BYTEOFF(0U);
        cursor.anchor = cursor.pos;
        cursor.goal_col = (GCol){0U};
        start = now_ns();
        sag_cursor_buf_end(tb, &cursor);
        elapsed = now_ns() - start;
        if (elapsed < 0 || cursor.pos.v != LONG_LINE_BYTES ||
            cursor.goal_col.v != SAG_GCOL_EOL) {
            sag_textbuf_free(cross_tb);
            sag_textbuf_free(tb);
            return false;
        }
        if (elapsed > worst)
            worst = elapsed;

        cursor.pos = BYTEOFF(LONG_LINE_BYTES + 1U);
        cursor.anchor = cursor.pos;
        cursor.goal_col = (GCol){0U};
        start = now_ns();
        sag_cursor_left(cross_tb, &cursor);
        elapsed = now_ns() - start;
        if (elapsed < 0 || cursor.pos.v != LONG_LINE_BYTES ||
            cursor.goal_col.v != clusters) {
            sag_textbuf_free(cross_tb);
            sag_textbuf_free(tb);
            return false;
        }
        if (elapsed > worst)
            worst = elapsed;

        start = now_ns();
        sag_cursor_left(cross_tb, &cursor);
        elapsed = now_ns() - start;
        if (elapsed < 0 ||
            cursor.pos.v != LONG_LINE_BYTES - pc->pattern_len ||
            cursor.goal_col.v != clusters - 1U) {
            sag_textbuf_free(cross_tb);
            sag_textbuf_free(tb);
            return false;
        }
        if (elapsed > worst)
            worst = elapsed;
        perf_cursor_sink = cursor.pos.v + cursor.goal_col.v;
    }
    (void)printf("cursor-perf: case=%s line_bytes=%u rounds=%u "
                 "worst_us=%lld "
                 "budget_us=5000%s\n",
                 pc->name, LONG_LINE_BYTES, PERF_ROUNDS,
                 (long long)(worst / INT64_C(1000)),
                 worst <= budget_ns ? "" : " OVER-BUDGET");
    if (cursor.pos.v != LONG_LINE_BYTES - pc->pattern_len ||
        cursor.goal_col.v != clusters - 1U || worst > budget_ns) {
        sag_textbuf_free(cross_tb);
        sag_textbuf_free(tb);
        return false;
    }
    sag_textbuf_free(cross_tb);
    sag_textbuf_free(tb);
    *worst_out = worst;
    return true;
}

static bool measure_contextual_reverse(void)
{
    static const i64 budget_ns = INT64_C(5000000);
    static const u8 ri[] = {0xF0U, 0x9FU, 0x87U, 0xA6U};
    static const u8 extend[] = {0xCCU, 0x81U};
    const size_t extend_len = LONG_LINE_BYTES - 1U;
    u8 *bytes = malloc(LONG_LINE_BYTES);
    TextBuf *tb;
    Cursor cursor;
    i64 start;
    i64 ri_elapsed;
    i64 extend_elapsed;
    size_t at;

    if (bytes == NULL)
        return false;
    for (at = 0U; at < LONG_LINE_BYTES; at += sizeof(ri))
        memcpy(bytes + at, ri, sizeof(ri));
    tb = sag_textbuf_from_owned_bytes(bytes, LONG_LINE_BYTES);
    cursor.pos = BYTEOFF(LONG_LINE_BYTES);
    cursor.anchor = cursor.pos;
    cursor.goal_col = (GCol){LONG_LINE_BYTES / sizeof(ri) / 2U};
    start = now_ns();
    sag_cursor_left(tb, &cursor);
    ri_elapsed = now_ns() - start;
    if (start < 0 || ri_elapsed < 0 ||
        cursor.pos.v != LONG_LINE_BYTES - 2U * sizeof(ri) ||
        cursor.goal_col.v != LONG_LINE_BYTES / sizeof(ri) / 2U - 1U) {
        sag_textbuf_free(tb);
        return false;
    }
    sag_textbuf_free(tb);

    bytes = malloc(extend_len);
    if (bytes == NULL)
        return false;
    bytes[0] = 'a';
    for (at = 1U; at < extend_len; at += sizeof(extend))
        memcpy(bytes + at, extend, sizeof(extend));
    tb = sag_textbuf_from_owned_bytes(bytes, (u64)extend_len);
    cursor.pos = BYTEOFF((u64)extend_len);
    cursor.anchor = cursor.pos;
    cursor.goal_col = (GCol){1U};
    start = now_ns();
    sag_cursor_left(tb, &cursor);
    extend_elapsed = now_ns() - start;
    if (start < 0 || extend_elapsed < 0 || cursor.pos.v != 0U ||
        cursor.goal_col.v != 0U) {
        sag_textbuf_free(tb);
        return false;
    }
    sag_textbuf_free(tb);

    (void)printf("cursor-perf: case=contextual-reverse line_bytes=%u "
                 "ri_us=%lld extend_us=%lld budget_us=5000%s\n",
                 LONG_LINE_BYTES,
                 (long long)(ri_elapsed / INT64_C(1000)),
                 (long long)(extend_elapsed / INT64_C(1000)),
                 ri_elapsed <= budget_ns && extend_elapsed <= budget_ns
                     ? ""
                     : " OVER-BUDGET");
    return ri_elapsed <= budget_ns && extend_elapsed <= budget_ns;
}

static bool measure_edit_position(const PerfCase *pc, u64 at)
{
    static const i64 budget_ns = INT64_C(5000000);
    static const u8 inserted = 'q';
    u8 *bytes = malloc(LONG_LINE_BYTES);
    TextBuf *tb;
    Cursor cursor;
    u64 gcol = at / pc->pattern_len;
    i64 worst = 0;
    size_t fill;
    int round;

    if (bytes == NULL || at >= LONG_LINE_BYTES ||
        at % pc->pattern_len != 0U) {
        free(bytes);
        return false;
    }
    for (fill = 0U; fill < LONG_LINE_BYTES; fill += pc->pattern_len)
        memcpy(bytes + fill, pc->pattern, pc->pattern_len);
    tb = sag_textbuf_from_owned_bytes(bytes, LONG_LINE_BYTES);
    if (tb == NULL)
        return false;

    for (round = 0; round < PERF_ROUNDS; round++) {
        i64 start = now_ns();
        i64 elapsed;

        sag_textbuf_insert(tb, BYTEOFF(at), &inserted, 1U);
        elapsed = now_ns() - start;
        if (start < 0 || elapsed < 0)
            goto fail;
        if (elapsed > worst)
            worst = elapsed;

        cursor.pos = BYTEOFF(at);
        cursor.anchor = cursor.pos;
        cursor.goal_col = (GCol){gcol};
        start = now_ns();
        sag_cursor_right(tb, &cursor);
        elapsed = now_ns() - start;
        if (start < 0 || elapsed < 0 || cursor.pos.v != at + 1U ||
            cursor.goal_col.v != gcol + 1U)
            goto fail;
        if (elapsed > worst)
            worst = elapsed;

        start = now_ns();
        sag_textbuf_delete(tb, (Span){at, at + 1U});
        elapsed = now_ns() - start;
        if (start < 0 || elapsed < 0)
            goto fail;
        if (elapsed > worst)
            worst = elapsed;

        cursor.pos = BYTEOFF(at);
        cursor.anchor = cursor.pos;
        cursor.goal_col = (GCol){gcol};
        start = now_ns();
        sag_cursor_right(tb, &cursor);
        elapsed = now_ns() - start;
        if (start < 0 || elapsed < 0 ||
            cursor.pos.v != at + pc->pattern_len ||
            cursor.goal_col.v != gcol + 1U)
            goto fail;
        if (elapsed > worst)
            worst = elapsed;
        perf_cursor_sink = cursor.pos.v + cursor.goal_col.v;
    }
    (void)printf("cursor-perf: case=%s edit_at=%llu rounds=%u "
                 "worst_us=%lld budget_us=5000%s\n",
                 pc->name, (unsigned long long)at, PERF_ROUNDS,
                 (long long)(worst / INT64_C(1000)),
                 worst <= budget_ns ? "" : " OVER-BUDGET");
    sag_textbuf_free(tb);
    return worst <= budget_ns;

fail:
    sag_textbuf_free(tb);
    return false;
}

int main(void)
{
    static const u8 ascii[] = {'x'};
    static const u8 latin2[] = {0xc3U, 0xa9U};
    const PerfCase cases[] = {
        {"ascii", ascii, sizeof(ascii)},
        {"u00e9", latin2, sizeof(latin2)}
    };
    size_t i;

    for (i = 0U; i < SAG_ARRAY_LEN(cases); i++) {
        i64 worst;

        if (!measure(&cases[i], &worst))
            return 1;
        if (!measure_edit_position(&cases[i], 0U) ||
            !measure_edit_position(&cases[i], LONG_LINE_BYTES / 2U))
            return 1;
    }
    if (!measure_contextual_reverse())
        return 1;
    return 0;
}
