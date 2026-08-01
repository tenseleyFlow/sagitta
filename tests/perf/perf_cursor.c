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

static volatile u64 perf_cursor_sink;

static i64 now_ns(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return -1;
    return (i64)ts.tv_sec * INT64_C(1000000000) + ts.tv_nsec;
}

int main(void)
{
    static const i64 budget_ns = INT64_C(5000000);
    u8 *bytes = malloc(LONG_LINE_BYTES);
    TextBuf *tb;
    Cursor cursor;
    i64 worst = 0;
    int round;

    if (bytes == NULL)
        return 2;
    memset(bytes, 'x', LONG_LINE_BYTES);
    tb = sag_textbuf_from_owned_bytes(bytes, LONG_LINE_BYTES);
    if (tb == NULL)
        return 2;

    for (round = 0; round < PERF_ROUNDS; round++) {
        i64 start;
        i64 elapsed;

        cursor.pos = BYTEOFF(LONG_LINE_BYTES - 1U);
        cursor.anchor = cursor.pos;
        cursor.goal_col = (GCol){LONG_LINE_BYTES - 1U};
        start = now_ns();
        if (start < 0) {
            sag_textbuf_free(tb);
            return 2;
        }
        sag_cursor_right(tb, &cursor);
        elapsed = now_ns() - start;
        if (elapsed < 0) {
            sag_textbuf_free(tb);
            return 2;
        }
        if (elapsed > worst)
            worst = elapsed;
        perf_cursor_sink = cursor.pos.v + cursor.goal_col.v;
    }
    (void)printf("cursor-perf: line_bytes=%u rounds=%u worst_us=%lld "
                 "budget_us=5000%s\n",
                 LONG_LINE_BYTES, PERF_ROUNDS,
                 (long long)(worst / INT64_C(1000)),
                 worst <= budget_ns ? "" : " OVER-BUDGET");
    if (cursor.pos.v != LONG_LINE_BYTES ||
        cursor.goal_col.v != LONG_LINE_BYTES || worst > budget_ns) {
        sag_textbuf_free(tb);
        return 1;
    }
    sag_textbuf_free(tb);
    return 0;
}
