#define _POSIX_C_SOURCE 200809L

/* Sprint 53's whole-file diff remains off the keystroke path, while every
 * frame performs only a logarithmic viewport lookup. */
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "mod/git/gutter.h"
#include "util/base.h"

enum {
    GUTTER_LINES = 100000,
    GUTTER_HUNKS = 500,
    GUTTER_DIFF_SAMPLES = 7,
    GUTTER_LOOKUP_SAMPLES = 5001,
    GUTTER_VIEW_LINES = 80,
    GUTTER_DIFF_BUDGET_NS = 25000000,
    GUTTER_LOOKUP_BUDGET_NS = 50000
};

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

static bool fixture_make(u64 **left_out, u64 **right_out)
{
    u64 *left = malloc((size_t)GUTTER_LINES * sizeof(*left));
    u64 *right = malloc((size_t)GUTTER_LINES * sizeof(*right));
    u32 i;

    if (left == NULL || right == NULL) {
        free(right);
        free(left);
        return false;
    }
    for (i = 0U; i < GUTTER_LINES; i++)
        left[i] = right[i] = UINT64_C(0x9e3779b97f4a7c15) ^ (u64)i;
    for (i = 0U; i < GUTTER_HUNKS; i++) {
        u32 line = i * (GUTTER_LINES / GUTTER_HUNKS) + 73U;

        right[line] ^= UINT64_C(0xd1b54a32d192ed03);
    }
    *left_out = left;
    *right_out = right;
    return true;
}

static bool measure_diff(const u64 *left, const u64 *right,
                         GitHunkVec *kept, u64 *median_out)
{
    u64 samples[GUTTER_DIFF_SAMPLES];
    size_t i;

    for (i = 0U; i < YEW_ARRAY_LEN(samples); i++) {
        Arena arena;
        GitHunkVec hunks = {0};
        u64 start;

        arena_init(&arena);
        start = now_ns();
        if (!yew_diff_lines(&arena, left, GUTTER_LINES, right, GUTTER_LINES,
                            YEW_DIFF_MAX_D, &hunks)) {
            arena_free_all(&arena);
            return false;
        }
        samples[i] = now_ns() - start;
        arena_free_all(&arena);
        if (hunks.len != GUTTER_HUNKS) {
            (void)fprintf(stderr,
                          "perf_git_gutter: expected %u hunks, got %zu\n",
                          GUTTER_HUNKS, hunks.len);
            GitHunkVec_free(&hunks);
            return false;
        }
        gutter_sink ^= hunks.data[i % hunks.len].buf_lo.v;
        if (i + 1U == YEW_ARRAY_LEN(samples))
            *kept = hunks;
        else
            GitHunkVec_free(&hunks);
    }
    sort_u64(samples, YEW_ARRAY_LEN(samples));
    *median_out = samples[YEW_ARRAY_LEN(samples) / 2U];
    return true;
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

int main(int argc, char **argv)
{
    u64 *left;
    u64 *right;
    GitHunkVec hunks = {0};
    u64 diff_median = 0U;
    u64 lookup_p99;
    bool gate = argc == 2 && strcmp(argv[1], "--gate") == 0;
    bool ok;

    if (argc > 2 || (argc == 2 && !gate)) {
        (void)fprintf(stderr, "usage: %s [--gate]\n", argv[0]);
        return 2;
    }
    if (!fixture_make(&left, &right)) {
        (void)fprintf(stderr, "perf_git_gutter: fixture allocation failed\n");
        return 2;
    }
    ok = measure_diff(left, right, &hunks, &diff_median);
    lookup_p99 = ok ? measure_lookup(&hunks) : UINT64_MAX;
    (void)printf("git_gutter diff_100k_500_median_ns=%llu "
                 "lookup_p99_ns=%llu hunks=%zu sink=%llu\n",
                 (unsigned long long)diff_median,
                 (unsigned long long)lookup_p99, hunks.len,
                 (unsigned long long)gutter_sink);
    if (gate && ok && (diff_median > GUTTER_DIFF_BUDGET_NS ||
                       lookup_p99 > GUTTER_LOOKUP_BUDGET_NS)) {
        (void)fprintf(stderr,
                      "perf_git_gutter: budget exceeded (diff <= %u ns, "
                      "lookup <= %u ns)\n",
                      GUTTER_DIFF_BUDGET_NS, GUTTER_LOOKUP_BUDGET_NS);
        ok = false;
    }
    GitHunkVec_free(&hunks);
    free(right);
    free(left);
    return ok ? 0 : 1;
}
