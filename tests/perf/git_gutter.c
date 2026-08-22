#define _POSIX_C_SOURCE 200809L

/* Sprint 53's whole-file diff runs through the real editor idle path. */
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "edit/ed.h"
#include "mod/git/editor.h"
#include "text/piece.h"
#include "util/base.h"
#include "util/buf.h"

enum {
    GUTTER_LINES = 100000,
    GUTTER_HUNKS = 500,
    GUTTER_EDITS = 200,
    GUTTER_DIFF_SAMPLES = 7,
    GUTTER_LOOKUP_SAMPLES = 5001,
    GUTTER_VIEW_LINES = 80,
    GUTTER_MAX_TICKS = 20000,
    GUTTER_DIFF_BUDGET_NS = 25000000,
    GUTTER_KEYPRESS_BUDGET_NS = 5000000,
    GUTTER_LOOKUP_BUDGET_NS = 50000
};

typedef struct TickSamples {
    u64 *data;
    size_t len;
    size_t cap;
} TickSamples;

static volatile u64 gutter_sink;

static u64 now_ns(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        (void)fprintf(stderr, "perf_git_gutter: clock_gettime: %s\n",
                      strerror(errno));
        return 0U;
    }
    return (u64)ts.tv_sec * UINT64_C(1000000000) + (u64)ts.tv_nsec;
}

static i64 now_us(void *ctx)
{
    (void)ctx;
    return (i64)(now_ns() / UINT64_C(1000));
}

static void sort_u64(u64 *values, size_t len)
{
    size_t i;

    for (i = 1U; i < len; i++) {
        u64 value = values[i];
        size_t at = i;

        while (at > 0U && values[at - 1U] > value) {
            values[at] = values[at - 1U];
            at--;
        }
        values[at] = value;
    }
}

static bool samples_push(TickSamples *samples, u64 value)
{
    if (samples->len == samples->cap) {
        size_t cap = samples->cap == 0U ? 64U : samples->cap * 2U;
        u64 *data;

        if (cap < samples->cap)
            return false;
        data = realloc(samples->data, cap * sizeof(*data));
        if (data == NULL)
            return false;
        samples->data = data;
        samples->cap = cap;
    }
    samples->data[samples->len++] = value;
    return true;
}

static size_t viewport_lookup(const GitHunkVec *hunks, u32 top, u32 bottom)
{
    size_t lo = 0U;
    size_t hi = hunks->len;
    size_t found;

    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2U;
        const GitHunk *h = &hunks->data[mid];
        u32 extent = h->buf_n.v == 0U ? 1U : h->buf_n.v;

        if (h->buf_lo.v + extent <= top)
            lo = mid + 1U;
        else
            hi = mid;
    }
    found = lo;
    while (lo < hunks->len && hunks->data[lo].buf_lo.v < bottom)
        lo++;
    return lo - found;
}

static void fixture_make(Bytebuf *base, Bytebuf *live)
{
    u32 i;

    bytebuf_init(base);
    bytebuf_init(live);
    for (i = 0U; i < GUTTER_LINES; i++) {
        bytebuf_printf(base, "line-%06u shared payload\n", i);
        if (i % (GUTTER_LINES / GUTTER_HUNKS) == 73U)
            bytebuf_printf(live, "line-%06u changed payload\n", i);
        else
            bytebuf_printf(live, "line-%06u shared payload\n", i);
    }
}

static bool fixture_open(Ed *ed, const Bytebuf *base, const Bytebuf *live,
                         i64 now_ms)
{
    yew_ed_init(ed);
    if (!yew_ed_open_memory(ed, live->data, live->len, "git-gutter")) {
        yew_ed_free(ed);
        return false;
    }
    ed->buffer.path = arena_strdup(&ed->arena, "git-gutter.txt");
    yew_git_editor_test_clock(ed, now_us, NULL);
    if (!yew_git_editor_test_base(ed, &ed->buffer, base->data, base->len,
                                  "perf-index", false, now_ms)) {
        yew_ed_free(ed);
        return false;
    }
    return true;
}

static bool tick_timed(Ed *ed, i64 now_ms, TickSamples *ticks)
{
    u64 start = now_ns();

    yew_git_editor_tick(ed, now_ms);
    return samples_push(ticks, now_ns() - start);
}

static bool pump_until(Ed *ed, i64 now_ms, u64 published,
                       TickSamples *ticks)
{
    YewGitEditorStats stats;
    u32 count = 0U;

    do {
        if (!tick_timed(ed, now_ms, ticks))
            return false;
        yew_git_editor_stats(ed, &stats);
        count++;
    } while (stats.diff_published < published && count < GUTTER_MAX_TICKS);
    return stats.diff_published >= published;
}

static u64 measure_lookup(const GitHunkVec *hunks)
{
    u64 samples[GUTTER_LOOKUP_SAMPLES];
    size_t i;

    for (i = 0U; i < YEW_ARRAY_LEN(samples); i++) {
        u32 top = (u32)((i * 7919U) % (GUTTER_LINES - GUTTER_VIEW_LINES));
        u64 start = now_ns();

        gutter_sink ^= viewport_lookup(hunks, top, top + GUTTER_VIEW_LINES);
        samples[i] = now_ns() - start;
    }
    sort_u64(samples, YEW_ARRAY_LEN(samples));
    return samples[(YEW_ARRAY_LEN(samples) * 99U + 99U) / 100U - 1U];
}

static bool verify_debounce(Ed *ed, i64 first_edit_ms,
                            TickSamples *ticks)
{
    YewGitEditorStats before;
    YewGitEditorStats after;
    u32 i;

    yew_git_editor_stats(ed, &before);
    for (i = 0U; i < GUTTER_EDITS; i++) {
        yew_textbuf_insert(ed->buffer.tb,
                          BYTEOFF(yew_textbuf_len(ed->buffer.tb)),
                          (const u8 *)"x", 1U);
        ed->now_ms = first_edit_ms + (i64)i;
        yew_git_editor_prepare(ed, ed->win);
    }
    if (!tick_timed(ed, first_edit_ms + GUTTER_EDITS + 148, ticks))
        return false;
    yew_git_editor_stats(ed, &after);
    if (after.diff_started != before.diff_started)
        return false;
    if (!pump_until(ed, first_edit_ms + GUTTER_EDITS + 149,
                    before.diff_published + 1U, ticks))
        return false;
    yew_git_editor_stats(ed, &after);
    return after.diff_started == before.diff_started + 1U &&
        after.diff_published == before.diff_published + 1U;
}

int main(int argc, char **argv)
{
    Bytebuf base;
    Bytebuf live;
    TickSamples ticks = {0};
    u64 diff_samples[GUTTER_DIFF_SAMPLES];
    u64 lookup_p99 = UINT64_MAX;
    u64 keypress_p99 = UINT64_MAX;
    u64 max_slice_us = 0U;
    bool debounce_ok = false;
    bool gate = argc == 2 && strcmp(argv[1], "--gate") == 0;
    bool ok = true;
    size_t i;

    if (argc > 2 || (argc == 2 && !gate)) {
        (void)fprintf(stderr, "usage: %s [--gate]\n", argv[0]);
        return 2;
    }
    fixture_make(&base, &live);
    for (i = 0U; i < YEW_ARRAY_LEN(diff_samples); i++) {
        YewGitEditorStats stats;
        const HunkList *hunks;
        Ed ed;
        u64 start;

        if (!fixture_open(&ed, &base, &live, 1000)) {
            ok = false;
            break;
        }
        start = now_ns();
        if (!pump_until(&ed, 1000, 1U, &ticks))
            ok = false;
        diff_samples[i] = now_ns() - start;
        yew_git_editor_stats(&ed, &stats);
        hunks = yew_git_editor_test_hunks(&ed, &ed.buffer);
        if (hunks == NULL || hunks->h.len != GUTTER_HUNKS ||
            stats.diff_started != 1U || stats.diff_published != 1U ||
            stats.diff_max_slice_us > YEW_DIFF_BUDGET_US)
            ok = false;
        if (stats.diff_max_slice_us > max_slice_us)
            max_slice_us = stats.diff_max_slice_us;
        if (i + 1U == YEW_ARRAY_LEN(diff_samples) && hunks != NULL) {
            lookup_p99 = measure_lookup(&hunks->h);
            debounce_ok = verify_debounce(&ed, 2000, &ticks);
            yew_git_editor_stats(&ed, &stats);
            if (stats.diff_max_slice_us > max_slice_us)
                max_slice_us = stats.diff_max_slice_us;
        }
        yew_ed_free(&ed);
        if (!ok)
            break;
    }
    if (ok) {
        sort_u64(diff_samples, YEW_ARRAY_LEN(diff_samples));
        sort_u64(ticks.data, ticks.len);
        keypress_p99 = ticks.data[(ticks.len * 99U + 99U) / 100U - 1U];
    }
    (void)printf("git_gutter diff_100k_500_median_ns=%llu "
                 "slice_max_us=%llu keypress_p99_ns=%llu "
                 "lookup_p99_ns=%llu edits=%u diff_starts=%u sink=%llu\n",
                 (unsigned long long)(ok ?
                     diff_samples[YEW_ARRAY_LEN(diff_samples) / 2U] :
                     UINT64_MAX),
                 (unsigned long long)max_slice_us,
                 (unsigned long long)keypress_p99,
                 (unsigned long long)lookup_p99, GUTTER_EDITS,
                 debounce_ok ? 1U : 0U,
                 (unsigned long long)gutter_sink);
    if (!debounce_ok) {
        (void)fprintf(stderr,
                      "perf_git_gutter: 200 edits did not debounce once\n");
        ok = false;
    }
    if (gate && ok &&
        (diff_samples[YEW_ARRAY_LEN(diff_samples) / 2U] >
             GUTTER_DIFF_BUDGET_NS ||
         max_slice_us > YEW_DIFF_BUDGET_US ||
         keypress_p99 > GUTTER_KEYPRESS_BUDGET_NS ||
         lookup_p99 > GUTTER_LOOKUP_BUDGET_NS)) {
        (void)fprintf(stderr,
                      "perf_git_gutter: budget exceeded "
                      "(diff <= %u ns, slice <= %u us, keypress <= %u ns, "
                      "lookup <= %u ns)\n",
                      GUTTER_DIFF_BUDGET_NS, YEW_DIFF_BUDGET_US,
                      GUTTER_KEYPRESS_BUDGET_NS, GUTTER_LOOKUP_BUDGET_NS);
        ok = false;
    }
    free(ticks.data);
    bytebuf_free(&live);
    bytebuf_free(&base);
    return ok ? 0 : 1;
}
