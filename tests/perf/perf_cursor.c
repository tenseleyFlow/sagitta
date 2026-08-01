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
        i64 start;
        i64 elapsed;

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
    }
    return 0;
}
