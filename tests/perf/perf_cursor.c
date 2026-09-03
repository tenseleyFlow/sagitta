#define _POSIX_C_SOURCE 200809L

#include "perf_policy.h"
#include "text/cursor.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

enum {
    LONG_LINE_BYTES = 8 * 1024 * 1024,
    PERF_ROUNDS = 9,
    DEFERRED_EDIT_BURST = 9
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

static i64 timing_max(i64 first, i64 second)
{
    return first > second ? first : second;
}

static bool timing_failed(i64 value, i64 budget)
{
    return yew_perf_timing_failed((uint64_t)value, (uint64_t)budget,
                                  yew_perf_advisory());
}

static const char *timing_verdict(i64 value, i64 budget)
{
    return yew_perf_timing_verdict((uint64_t)value, (uint64_t)budget,
                                   yew_perf_advisory());
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
    tb = yew_textbuf_from_owned_bytes(bytes, LONG_LINE_BYTES);
    cross_tb = yew_textbuf_from_owned_bytes(cross_bytes,
                                             LONG_LINE_BYTES + 2U);
    if (tb == NULL || cross_tb == NULL) {
        yew_textbuf_free(cross_tb);
        yew_textbuf_free(tb);
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
            yew_textbuf_free(tb);
            yew_textbuf_free(cross_tb);
            return false;
        }
        yew_cursor_right(tb, &cursor);
        elapsed = now_ns() - start;
        if (elapsed < 0 || cursor.pos.v != LONG_LINE_BYTES ||
            cursor.goal_col.v != clusters) {
            (void)fprintf(stderr,
                          "cursor-perf: direct-right mismatch "
                          "case=%s round=%d pos=%llu goal=%llu\n",
                          pc->name, round,
                          (unsigned long long)cursor.pos.v,
                          (unsigned long long)cursor.goal_col.v);
            yew_textbuf_free(tb);
            yew_textbuf_free(cross_tb);
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
            yew_textbuf_free(tb);
            yew_textbuf_free(cross_tb);
            return false;
        }
        yew_cursor_left(tb, &cursor);
        elapsed = now_ns() - start;
        if (elapsed < 0 ||
            cursor.pos.v != LONG_LINE_BYTES - pc->pattern_len ||
            cursor.goal_col.v != clusters - 1U) {
            yew_textbuf_free(tb);
            yew_textbuf_free(cross_tb);
            return false;
        }
        if (elapsed > worst)
            worst = elapsed;
        perf_cursor_sink = cursor.pos.v + cursor.goal_col.v;

        start = now_ns();
        yew_textbuf_insert(tb, BYTEOFF(LONG_LINE_BYTES), &inserted, 1U);
        elapsed = now_ns() - start;
        if (start < 0 || elapsed < 0) {
            yew_textbuf_delete(tb,
                               (Span){LONG_LINE_BYTES,
                                      LONG_LINE_BYTES + 1U});
            yew_textbuf_free(cross_tb);
            yew_textbuf_free(tb);
            return false;
        }
        if (elapsed > worst)
            worst = elapsed;
        cursor.pos = BYTEOFF(LONG_LINE_BYTES + 1U);
        cursor.anchor = cursor.pos;
        cursor.goal_col = (GCol){clusters + 1U};
        start = now_ns();
        if (start < 0) {
            yew_textbuf_delete(tb,
                               (Span){LONG_LINE_BYTES,
                                      LONG_LINE_BYTES + 1U});
            yew_textbuf_free(cross_tb);
            yew_textbuf_free(tb);
            return false;
        }
        yew_cursor_left(tb, &cursor);
        elapsed = now_ns() - start;
        if (elapsed < 0 || cursor.pos.v != LONG_LINE_BYTES ||
            cursor.goal_col.v != clusters) {
            (void)fprintf(stderr,
                          "cursor-perf: post-edit first-left mismatch "
                          "case=%s round=%d pos=%llu goal=%llu\n",
                          pc->name, round,
                          (unsigned long long)cursor.pos.v,
                          (unsigned long long)cursor.goal_col.v);
            yew_textbuf_delete(tb,
                               (Span){LONG_LINE_BYTES,
                                      LONG_LINE_BYTES + 1U});
            yew_textbuf_free(cross_tb);
            yew_textbuf_free(tb);
            return false;
        }
        if (elapsed > worst)
            worst = elapsed;

        start = now_ns();
        if (start < 0) {
            yew_textbuf_delete(tb,
                               (Span){LONG_LINE_BYTES,
                                      LONG_LINE_BYTES + 1U});
            yew_textbuf_free(cross_tb);
            yew_textbuf_free(tb);
            return false;
        }
        yew_cursor_left(tb, &cursor);
        elapsed = now_ns() - start;
        start = now_ns();
        yew_textbuf_delete(tb,
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
            yew_textbuf_free(cross_tb);
            yew_textbuf_free(tb);
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
        yew_cursor_line_end(tb, &cursor);
        elapsed = now_ns() - start;
        if (elapsed < 0 || cursor.pos.v != LONG_LINE_BYTES ||
            cursor.goal_col.v != YEW_GCOL_EOL) {
            yew_textbuf_free(cross_tb);
            yew_textbuf_free(tb);
            return false;
        }
        if (elapsed > worst)
            worst = elapsed;

        start = now_ns();
        yew_cursor_left(tb, &cursor);
        elapsed = now_ns() - start;
        if (elapsed < 0 ||
            cursor.pos.v != LONG_LINE_BYTES - pc->pattern_len ||
            cursor.goal_col.v != clusters - 1U) {
            yew_textbuf_free(cross_tb);
            yew_textbuf_free(tb);
            return false;
        }
        if (elapsed > worst)
            worst = elapsed;

        cursor.pos = BYTEOFF(0U);
        cursor.anchor = cursor.pos;
        cursor.goal_col = (GCol){0U};
        start = now_ns();
        yew_cursor_buf_end(tb, &cursor);
        elapsed = now_ns() - start;
        if (elapsed < 0 || cursor.pos.v != LONG_LINE_BYTES ||
            cursor.goal_col.v != YEW_GCOL_EOL) {
            yew_textbuf_free(cross_tb);
            yew_textbuf_free(tb);
            return false;
        }
        if (elapsed > worst)
            worst = elapsed;

        cursor.pos = BYTEOFF(LONG_LINE_BYTES + 1U);
        cursor.anchor = cursor.pos;
        cursor.goal_col = (GCol){0U};
        start = now_ns();
        yew_cursor_left(cross_tb, &cursor);
        elapsed = now_ns() - start;
        if (elapsed < 0 || cursor.pos.v != LONG_LINE_BYTES ||
            cursor.goal_col.v != clusters) {
            yew_textbuf_free(cross_tb);
            yew_textbuf_free(tb);
            return false;
        }
        if (elapsed > worst)
            worst = elapsed;

        start = now_ns();
        yew_cursor_left(cross_tb, &cursor);
        elapsed = now_ns() - start;
        if (elapsed < 0 ||
            cursor.pos.v != LONG_LINE_BYTES - pc->pattern_len ||
            cursor.goal_col.v != clusters - 1U) {
            yew_textbuf_free(cross_tb);
            yew_textbuf_free(tb);
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
                 timing_verdict(worst, budget_ns));
    if (cursor.pos.v != LONG_LINE_BYTES - pc->pattern_len ||
        cursor.goal_col.v != clusters - 1U ||
        timing_failed(worst, budget_ns)) {
        yew_textbuf_free(cross_tb);
        yew_textbuf_free(tb);
        return false;
    }
    yew_textbuf_free(cross_tb);
    yew_textbuf_free(tb);
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
    tb = yew_textbuf_from_owned_bytes(bytes, LONG_LINE_BYTES);
    cursor.pos = BYTEOFF(LONG_LINE_BYTES);
    cursor.anchor = cursor.pos;
    cursor.goal_col = (GCol){LONG_LINE_BYTES / sizeof(ri) / 2U};
    start = now_ns();
    yew_cursor_left(tb, &cursor);
    ri_elapsed = now_ns() - start;
    if (start < 0 || ri_elapsed < 0 ||
        cursor.pos.v != LONG_LINE_BYTES - 2U * sizeof(ri) ||
        cursor.goal_col.v != LONG_LINE_BYTES / sizeof(ri) / 2U - 1U) {
        yew_textbuf_free(tb);
        return false;
    }
    yew_textbuf_free(tb);

    bytes = malloc(extend_len);
    if (bytes == NULL)
        return false;
    bytes[0] = 'a';
    for (at = 1U; at < extend_len; at += sizeof(extend))
        memcpy(bytes + at, extend, sizeof(extend));
    tb = yew_textbuf_from_owned_bytes(bytes, (u64)extend_len);
    cursor.pos = BYTEOFF((u64)extend_len);
    cursor.anchor = cursor.pos;
    cursor.goal_col = (GCol){1U};
    start = now_ns();
    yew_cursor_left(tb, &cursor);
    extend_elapsed = now_ns() - start;
    if (start < 0 || extend_elapsed < 0 || cursor.pos.v != 0U ||
        cursor.goal_col.v != 0U) {
        yew_textbuf_free(tb);
        return false;
    }
    yew_textbuf_free(tb);

    (void)printf("cursor-perf: case=contextual-reverse line_bytes=%u "
                 "ri_us=%lld extend_us=%lld budget_us=5000%s\n",
                 LONG_LINE_BYTES,
                 (long long)(ri_elapsed / INT64_C(1000)),
                 (long long)(extend_elapsed / INT64_C(1000)),
                 timing_verdict(timing_max(ri_elapsed, extend_elapsed),
                                budget_ns));
    return !timing_failed(timing_max(ri_elapsed, extend_elapsed),
                          budget_ns);
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
    tb = yew_textbuf_from_owned_bytes(bytes, LONG_LINE_BYTES);
    if (tb == NULL)
        return false;

    for (round = 0; round < PERF_ROUNDS; round++) {
        i64 start = now_ns();
        i64 elapsed;

        yew_textbuf_insert(tb, BYTEOFF(at), &inserted, 1U);
        elapsed = now_ns() - start;
        if (start < 0 || elapsed < 0)
            goto fail;
        if (elapsed > worst)
            worst = elapsed;

        cursor.pos = BYTEOFF(at);
        cursor.anchor = cursor.pos;
        cursor.goal_col = (GCol){gcol};
        start = now_ns();
        yew_cursor_right(tb, &cursor);
        elapsed = now_ns() - start;
        if (start < 0 || elapsed < 0 || cursor.pos.v != at + 1U ||
            cursor.goal_col.v != gcol + 1U)
            goto fail;
        if (elapsed > worst)
            worst = elapsed;

        start = now_ns();
        yew_textbuf_delete(tb, (Span){at, at + 1U});
        elapsed = now_ns() - start;
        if (start < 0 || elapsed < 0)
            goto fail;
        if (elapsed > worst)
            worst = elapsed;

        cursor.pos = BYTEOFF(at);
        cursor.anchor = cursor.pos;
        cursor.goal_col = (GCol){gcol};
        start = now_ns();
        yew_cursor_right(tb, &cursor);
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
                 timing_verdict(worst, budget_ns));
    yew_textbuf_free(tb);
    return !timing_failed(worst, budget_ns);

fail:
    yew_textbuf_free(tb);
    return false;
}

static bool measure_two_deferred_edits(const PerfCase *pc)
{
    static const i64 budget_ns = INT64_C(5000000);
    static const u8 first = 'q';
    static const u8 second = 'r';
    const u64 midpoint = LONG_LINE_BYTES / 2U;
    const u64 shifted_midpoint = midpoint + 1U;
    const u64 clusters = LONG_LINE_BYTES / pc->pattern_len;
    u8 *bytes = malloc(LONG_LINE_BYTES);
    TextBuf *tb;
    Cursor cursor;
    i64 worst_edit = 0;
    i64 worst_left = 0;
    size_t fill;
    int round;

    if (bytes == NULL || midpoint % pc->pattern_len != 0U) {
        free(bytes);
        return false;
    }
    for (fill = 0U; fill < LONG_LINE_BYTES; fill += pc->pattern_len)
        memcpy(bytes + fill, pc->pattern, pc->pattern_len);
    tb = yew_textbuf_from_owned_bytes(bytes, LONG_LINE_BYTES);
    if (tb == NULL)
        return false;

    for (round = 0; round < PERF_ROUNDS; round++) {
        i64 start;
        i64 elapsed;

        start = now_ns();
        yew_textbuf_insert(tb, BYTEOFF(0U), &first, 1U);
        elapsed = now_ns() - start;
        if (start < 0 || elapsed < 0)
            goto fail;
        if (elapsed > worst_edit)
            worst_edit = elapsed;

        start = now_ns();
        yew_textbuf_insert(tb, BYTEOFF(shifted_midpoint), &second, 1U);
        elapsed = now_ns() - start;
        if (start < 0 || elapsed < 0)
            goto fail;
        if (elapsed > worst_edit)
            worst_edit = elapsed;

        cursor.pos = BYTEOFF(LONG_LINE_BYTES + 2U);
        cursor.anchor = cursor.pos;
        cursor.goal_col = (GCol){0U};
        start = now_ns();
        yew_cursor_left(tb, &cursor);
        elapsed = now_ns() - start;
        if (start < 0 || elapsed < 0 ||
            cursor.pos.v != LONG_LINE_BYTES ||
            cursor.goal_col.v != clusters + 1U)
            goto fail;
        if (elapsed > worst_left)
            worst_left = elapsed;
        perf_cursor_sink = cursor.pos.v + cursor.goal_col.v;

        start = now_ns();
        yew_textbuf_delete(tb,
                           (Span){shifted_midpoint,
                                  shifted_midpoint + 1U});
        elapsed = now_ns() - start;
        if (start < 0 || elapsed < 0)
            goto fail;
        if (elapsed > worst_edit)
            worst_edit = elapsed;

        start = now_ns();
        yew_textbuf_delete(tb, (Span){0U, 1U});
        elapsed = now_ns() - start;
        if (start < 0 || elapsed < 0 ||
            yew_textbuf_len(tb) != LONG_LINE_BYTES)
            goto fail;
        if (elapsed > worst_edit)
            worst_edit = elapsed;

        if (yew_grapheme_prev_boundary(tb,
                                       BYTEOFF(LONG_LINE_BYTES)).v !=
            LONG_LINE_BYTES - pc->pattern_len)
            goto fail;
    }

    (void)printf("cursor-perf: case=%s two-deferred-edits rounds=%u "
                 "edit_worst_us=%lld first_left_worst_us=%lld "
                 "budget_us=5000%s\n",
                 pc->name, PERF_ROUNDS,
                 (long long)(worst_edit / INT64_C(1000)),
                 (long long)(worst_left / INT64_C(1000)),
                 timing_verdict(timing_max(worst_edit, worst_left),
                                budget_ns));
    yew_textbuf_free(tb);
    return !timing_failed(timing_max(worst_edit, worst_left), budget_ns);

fail:
    yew_textbuf_free(tb);
    return false;
}

static bool measure_deferred_edit_burst(const PerfCase *pc)
{
    static const i64 budget_ns = INT64_C(5000000);
    static const u8 inserted = 'q';
    const u64 clusters = LONG_LINE_BYTES / pc->pattern_len;
    u64 positions[DEFERRED_EDIT_BURST];
    u8 *bytes = malloc(LONG_LINE_BYTES);
    TextBuf *tb;
    Cursor cursor;
    i64 worst_edit = 0;
    i64 worst_query = 0;
    size_t fill;
    int round;

    if (bytes == NULL)
        return false;
    for (fill = 0U; fill < LONG_LINE_BYTES; fill += pc->pattern_len)
        memcpy(bytes + fill, pc->pattern, pc->pattern_len);
    tb = yew_textbuf_from_owned_bytes(bytes, LONG_LINE_BYTES);
    if (tb == NULL)
        return false;

    for (round = 0; round < PERF_ROUNDS; round++) {
        int edit;
        i64 start;
        i64 elapsed;

        for (edit = 0; edit < DEFERRED_EDIT_BURST; edit++) {
            u64 original = (u64)LONG_LINE_BYTES * (u64)(edit + 1) /
                           (u64)(DEFERRED_EDIT_BURST + 1);

            original -= original % pc->pattern_len;
            positions[edit] = original + (u64)edit;
            start = now_ns();
            yew_textbuf_insert(tb, BYTEOFF(positions[edit]),
                               &inserted, 1U);
            elapsed = now_ns() - start;
            if (start < 0 || elapsed < 0)
                goto fail;
            if (elapsed > worst_edit)
                worst_edit = elapsed;
        }
        if (yew_textbuf_len(tb) !=
            LONG_LINE_BYTES + (u64)DEFERRED_EDIT_BURST)
            goto fail;

        cursor.pos = BYTEOFF(yew_textbuf_len(tb));
        cursor.anchor = cursor.pos;
        cursor.goal_col = (GCol){0U};
        start = now_ns();
        yew_cursor_left(tb, &cursor);
        elapsed = now_ns() - start;
        if (start < 0 || elapsed < 0 ||
            cursor.pos.v != yew_textbuf_len(tb) - pc->pattern_len ||
            cursor.goal_col.v !=
                clusters + (u64)DEFERRED_EDIT_BURST - 1U)
            goto fail;
        if (elapsed > worst_query)
            worst_query = elapsed;
        perf_cursor_sink = cursor.pos.v + cursor.goal_col.v;

        for (edit = DEFERRED_EDIT_BURST - 1; edit >= 0; edit--) {
            start = now_ns();
            yew_textbuf_delete(tb,
                               (Span){positions[edit],
                                      positions[edit] + 1U});
            elapsed = now_ns() - start;
            if (start < 0 || elapsed < 0)
                goto fail;
            if (elapsed > worst_edit)
                worst_edit = elapsed;
        }
        if (yew_textbuf_len(tb) != LONG_LINE_BYTES)
            goto fail;
        start = now_ns();
        if (yew_grapheme_prev_boundary(tb,
                                       BYTEOFF(LONG_LINE_BYTES)).v !=
            LONG_LINE_BYTES - pc->pattern_len)
            goto fail;
        elapsed = now_ns() - start;
        if (start < 0 || elapsed < 0)
            goto fail;
        if (elapsed > worst_query)
            worst_query = elapsed;
    }

    (void)printf("cursor-perf: case=%s deferred-edit-burst=%u rounds=%u "
                 "edit_worst_us=%lld query_worst_us=%lld "
                 "budget_us=5000%s\n",
                 pc->name, DEFERRED_EDIT_BURST, PERF_ROUNDS,
                 (long long)(worst_edit / INT64_C(1000)),
                 (long long)(worst_query / INT64_C(1000)),
                 timing_verdict(timing_max(worst_edit, worst_query),
                                budget_ns));
    yew_textbuf_free(tb);
    return !timing_failed(timing_max(worst_edit, worst_query), budget_ns);

fail:
    yew_textbuf_free(tb);
    return false;
}

static bool measure_ascii_context_transition(void)
{
    static const i64 budget_ns = INT64_C(5000000);
    static const u8 accent[] = {0xCCU, 0x81U};
    static const u8 inserted = 'q';
    const u64 midpoint = LONG_LINE_BYTES / 2U;
    u8 *bytes = malloc(LONG_LINE_BYTES);
    TextBuf *tb;
    Cursor cursor;
    i64 worst_edit = 0;
    i64 worst_query = 0;
    int round;

    if (bytes == NULL)
        return false;
    memset(bytes, 'x', LONG_LINE_BYTES);
    tb = yew_textbuf_from_owned_bytes(bytes, LONG_LINE_BYTES);
    if (tb == NULL)
        return false;
    yew_textbuf_insert(tb, BYTEOFF(0U), &inserted, 1U);
    yew_textbuf_delete(tb, (Span){0U, 1U});

    for (round = 0; round < PERF_ROUNDS; round++) {
        i64 start = now_ns();
        i64 elapsed;

        yew_textbuf_insert(tb, BYTEOFF(midpoint), accent, sizeof(accent));
        elapsed = now_ns() - start;
        if (start < 0 || elapsed < 0)
            goto fail;
        if (elapsed > worst_edit)
            worst_edit = elapsed;

        cursor.pos = BYTEOFF(LONG_LINE_BYTES + sizeof(accent));
        cursor.anchor = cursor.pos;
        cursor.goal_col = (GCol){0U};
        start = now_ns();
        yew_cursor_left(tb, &cursor);
        if (cursor.pos.v != LONG_LINE_BYTES + sizeof(accent) - 1U ||
            cursor.goal_col.v != LONG_LINE_BYTES - 1U ||
            yew_gcol_to_off(
                tb, (Span){0U, LONG_LINE_BYTES + sizeof(accent)},
                cursor.goal_col).v != cursor.pos.v)
            goto fail;
        elapsed = now_ns() - start;
        if (start < 0 || elapsed < 0)
            goto fail;
        if (elapsed > worst_query)
            worst_query = elapsed;
        perf_cursor_sink = cursor.pos.v + cursor.goal_col.v;

        start = now_ns();
        yew_textbuf_delete(tb,
                           (Span){midpoint, midpoint + sizeof(accent)});
        elapsed = now_ns() - start;
        if (start < 0 || elapsed < 0)
            goto fail;
        if (elapsed > worst_edit)
            worst_edit = elapsed;

        start = now_ns();
        if (yew_grapheme_prev_boundary(tb,
                                       BYTEOFF(LONG_LINE_BYTES)).v !=
            LONG_LINE_BYTES - 1U)
            goto fail;
        elapsed = now_ns() - start;
        if (start < 0 || elapsed < 0)
            goto fail;
        if (elapsed > worst_query)
            worst_query = elapsed;
    }

    (void)printf("cursor-perf: case=ascii-context-transition rounds=%u "
                 "edit_worst_us=%lld query_worst_us=%lld "
                 "budget_us=5000%s\n",
                 PERF_ROUNDS,
                 (long long)(worst_edit / INT64_C(1000)),
                 (long long)(worst_query / INT64_C(1000)),
                 timing_verdict(timing_max(worst_edit, worst_query),
                                budget_ns));
    yew_textbuf_free(tb);
    return !timing_failed(timing_max(worst_edit, worst_query), budget_ns);

fail:
    yew_textbuf_free(tb);
    return false;
}

static bool measure_giant_cluster_midpoint_edit(void)
{
    static const i64 budget_ns = INT64_C(5000000);
    static const u8 extend[] = {0xCCU, 0x81U};
    static const u8 inserted = 'q';
    const u64 len = LONG_LINE_BYTES + 1U;
    const u64 midpoint = 1U + LONG_LINE_BYTES / 2U;
    u8 *bytes = malloc((size_t)len);
    TextBuf *tb;
    Cursor cursor;
    i64 worst_edit = 0;
    i64 worst_left = 0;
    size_t at;
    int round;

    if (bytes == NULL)
        return false;
    bytes[0] = 'a';
    for (at = 1U; at < (size_t)len; at += sizeof(extend))
        memcpy(bytes + at, extend, sizeof(extend));
    tb = yew_textbuf_from_owned_bytes(bytes, len);
    if (tb == NULL)
        return false;

    for (round = 0; round < PERF_ROUNDS; round++) {
        i64 start;
        i64 elapsed;

        start = now_ns();
        yew_textbuf_insert(tb, BYTEOFF(midpoint), &inserted, 1U);
        elapsed = now_ns() - start;
        if (start < 0 || elapsed < 0)
            goto fail;
        if (elapsed > worst_edit)
            worst_edit = elapsed;

        cursor.pos = BYTEOFF(len + 1U);
        cursor.anchor = cursor.pos;
        cursor.goal_col = (GCol){0U};
        start = now_ns();
        yew_cursor_left(tb, &cursor);
        elapsed = now_ns() - start;
        if (start < 0 || elapsed < 0 || cursor.pos.v != midpoint ||
            cursor.goal_col.v != 1U)
            goto fail;
        if (elapsed > worst_left)
            worst_left = elapsed;
        perf_cursor_sink = cursor.pos.v + cursor.goal_col.v;

        yew_textbuf_delete(tb, (Span){midpoint, midpoint + 1U});
        if (yew_textbuf_len(tb) != len ||
            yew_grapheme_prev_boundary(tb, BYTEOFF(len)).v != 0U)
            goto fail;
    }

    (void)printf("cursor-perf: case=giant-extend-cluster "
                 "edit_at=%llu rounds=%u edit_worst_us=%lld "
                 "first_left_worst_us=%lld budget_us=5000%s\n",
                 (unsigned long long)midpoint, PERF_ROUNDS,
                 (long long)(worst_edit / INT64_C(1000)),
                 (long long)(worst_left / INT64_C(1000)),
                 timing_verdict(timing_max(worst_edit, worst_left),
                                budget_ns));
    yew_textbuf_free(tb);
    return !timing_failed(timing_max(worst_edit, worst_left), budget_ns);

fail:
    yew_textbuf_free(tb);
    return false;
}

static int selftest_policy(void)
{
    const uint64_t budget = UINT64_C(5000000);

    if (yew_perf_timing_failed(budget, budget, false) ||
        !yew_perf_timing_failed(budget + 1U, budget, false) ||
        yew_perf_timing_failed(budget + 1U, budget, true) ||
        yew_perf_timing_failed(
            budget * YEW_PERF_ADVISORY_SANITY_MULTIPLIER, budget, true) ||
        !yew_perf_timing_failed(
            budget * YEW_PERF_ADVISORY_SANITY_MULTIPLIER + 1U,
            budget, true) ||
        !yew_perf_timing_failed(0U, budget, true)) {
        (void)fprintf(stderr, "perf-cursor-policy: failed\n");
        return 1;
    }
    (void)printf("perf-cursor-policy: strict/advisory/sanity ok\n");
    return 0;
}

int main(int argc, char **argv)
{
    static const u8 ascii[] = {'x'};
    static const u8 latin2[] = {0xc3U, 0xa9U};
    const PerfCase cases[] = {
        {"ascii", ascii, sizeof(ascii)},
        {"u00e9", latin2, sizeof(latin2)}
    };
    bool passed = true;
    size_t i;

    if (argc == 2 && strcmp(argv[1], "--selftest-policy") == 0)
        return selftest_policy();
    if (argc != 1) {
        (void)fprintf(stderr, "usage: %s [--selftest-policy]\n", argv[0]);
        return 2;
    }

    for (i = 0U; i < YEW_ARRAY_LEN(cases); i++) {
        i64 worst;

        if (!measure(&cases[i], &worst))
            return 1;
        if (!measure_edit_position(&cases[i], 0U) ||
            !measure_edit_position(&cases[i], LONG_LINE_BYTES / 2U))
            return 1;
    }
    if (!measure_two_deferred_edits(&cases[1]))
        passed = false;
    if (!measure_deferred_edit_burst(&cases[1]))
        passed = false;
    if (!measure_ascii_context_transition())
        passed = false;
    if (!measure_giant_cluster_midpoint_edit())
        passed = false;
    if (!measure_contextual_reverse())
        passed = false;
    return passed ? 0 : 1;
}
