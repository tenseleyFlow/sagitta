/*
 * Sprint 21 DoD 5: search stays inside the keypress budget on a big file.
 *
 * The unit tests prove the overlay's scan is bounded by SCOPE.  This
 * proves the thing that actually matters to a user: that the bound
 * translates into latency, on an input large enough that an unbounded
 * implementation would be obviously, unusably slow rather than subtly
 * so.
 *
 * Three measurements:
 *
 *   keystroke   recompile + preview + overlay refresh, per typed
 *               character, against the 5 ms keypress→paint gate
 *               (invariant 4)
 *   step        `n` through a thousand matches, average <= 1 ms
 *   rss         the overlay must not grow with the file: its spans
 *               cover a viewport, not a buffer
 *
 * The file is built once and searched with a pattern whose matches are
 * mostly far outside the viewport, so an implementation that scans the
 * whole buffer per keystroke fails on time rather than passing by luck.
 */
#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <time.h>

#include "edit/ed.h"
#include "search/overlay.h"
#include "search/searchui.h"
#include "text/piece.h"
#include "util/arena.h"

#define DEFAULT_BYTES (1024ULL * 1024ULL * 1024ULL)

static i64 now_ns(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return -1;
    return (i64)ts.tv_sec * INT64_C(1000000000) + ts.tv_nsec;
}

static u64 peak_rss(void)
{
    struct rusage ru;

    if (getrusage(RUSAGE_SELF, &ru) != 0)
        return 0U;
    return (u64)ru.ru_maxrss * 1024ULL;
}

static u64 env_u64(const char *name, u64 fallback)
{
    const char *value = getenv(name);
    char *end = NULL;
    unsigned long long parsed;

    if (value == NULL || value[0] == '\0')
        return fallback;
    parsed = strtoull(value, &end, 10);
    if (end == NULL || *end != '\0' || parsed == 0ULL)
        return fallback;
    return (u64)parsed;
}

static bool read_limits(const char *path, u64 *keypress_ns, u64 *step_ns,
                        u64 *rss_bytes)
{
    FILE *file = fopen(path, "r");
    char line[256];

    if (file == NULL)
        return false;
    *keypress_ns = 0U;
    *step_ns = 0U;
    *rss_bytes = 0U;
    while (fgets(line, sizeof(line), file) != NULL) {
        char name[96];
        unsigned long long value;

        if (sscanf(line, "%95s %llu", name, &value) != 2 || value == 0ULL)
            continue;
        if (strcmp(name, "search.keypress_p99_ns") == 0)
            *keypress_ns = (u64)value;
        else if (strcmp(name, "search.step_mean_ns") == 0)
            *step_ns = (u64)value;
        else if (strcmp(name, "search.overlay_rss_bytes") == 0)
            *rss_bytes = (u64)value;
    }
    if (ferror(file) || fclose(file) != 0)
        return false;
    return *keypress_ns != 0U && *step_ns != 0U && *rss_bytes != 0U;
}

static int cmp_i64(const void *a, const void *b)
{
    i64 x = *(const i64 *)a;
    i64 y = *(const i64 *)b;

    return x < y ? -1 : (x > y ? 1 : 0);
}

int main(int argc, char **argv)
{
    u64 bytes = env_u64("SAG_SEARCH_LATENCY_BYTES", DEFAULT_BYTES);
    u64 keypress_limit = 0U;
    u64 step_limit = 0U;
    u64 rss_limit = 0U;
    Ed ed;
    EditCtx ec;
    Bytebuf src;
    Arena arena;
    SearchOpts opts;
    /* Typed one character at a time, as a user would. */
    static const char typed[] = "ZQNEEDLE";
    i64 samples[sizeof(typed) - 1U];
    u64 nsamples = 0U;
    u64 rss_before;
    u64 rss_after;
    int status = 0;

    if (argc != 3 || strcmp(argv[1], "--baseline") != 0) {
        (void)fprintf(stderr, "usage: search_latency --baseline FILE\n");
        return 2;
    }
    if (!read_limits(argv[2], &keypress_limit, &step_limit, &rss_limit)) {
        (void)fprintf(stderr, "search_latency: cannot read %s\n", argv[2]);
        return 2;
    }

    sag_ed_init(&ed);
    if (!sag_ed_open_scratch(&ed)) {
        (void)fprintf(stderr, "search_latency: cannot open a buffer\n");
        return 2;
    }
    bytebuf_init(&src);
    {
        static const char row[] =
            "static int compute_value(int argument, int other) { 0; }\n";
        u64 rowlen = (u64)(sizeof(row) - 1U);

        while (src.len + rowlen < bytes)
            bytebuf_append(&src, row, (size_t)rowlen);
        /* One match, far from the top: a viewport-bounded scan will not
         * find it, and must not go looking. */
        bytebuf_append(&src, "ZQNEEDLE\n", 9U);
    }
    ec = sag_ed_edit_ctx(&ed);
    if (!sag_edit_insert(&ec, BYTEOFF(0U), src.data, src.len)) {
        (void)fprintf(stderr, "search_latency: insert failed\n");
        return 2;
    }
    bytebuf_free(&src);
    ed.win->rect.h = 50U;
    ed.win->rect.w = 120U;
    sag_search_opts_init(&opts);
    ed.search_opts = opts;

    rss_before = peak_rss();
    arena_init(&arena);

    /*
     * Type the pattern one character at a time.  Every keystroke
     * recompiles and refreshes, which is exactly what the prompt does.
     */
    {
        size_t n;

        for (n = 1U; n <= sizeof(typed) - 1U; n++) {
            SagRe *re;
            i64 start;
            i64 end;

            start = now_ns();
            re = sag_search_compile(&arena, typed, n, &opts, NULL);
            if (re != NULL)
                sag_overlay_refresh(&ed, ed.win, re, (u32)n,
                                    SAG_OVERLAY_BUDGET_US);
            end = now_ns();
            samples[nsamples++] = end - start;
        }
    }
    qsort(samples, (size_t)nsamples, sizeof(*samples), cmp_i64);
    {
        i64 p99 = samples[nsamples - 1U]; /* few samples: take the max */

        (void)printf("search.keypress_p99 %12lld ns (limit %llu)\n",
                     (long long)p99, (unsigned long long)keypress_limit);
        if ((u64)p99 > keypress_limit) {
            (void)fprintf(stderr,
                          "search_latency: a search keystroke took %lld ns "
                          "against a %llu ns budget — something is "
                          "scanning more than the viewport\n",
                          (long long)p99,
                          (unsigned long long)keypress_limit);
            status = 1;
        }
    }

    /* `n` through many matches: each step is one engine search from the
     * cursor, and must not degrade with file size. */
    {
        SagRe *re = sag_search_compile(&arena, "compute_value", 13U, &opts,
                                       NULL);
        i64 start;
        i64 end;
        u32 steps = 1000U;
        u32 i;

        sag_reg_set_search(&ed.regs, (const u8 *)"compute_value", 13U);
        ed.search.re = re;
        ed.search.reverse = false;
        sag_ed_cursor(&ed)->pos = BYTEOFF(0U);
        start = now_ns();
        for (i = 0U; i < steps; i++)
            (void)sag_search_step(&ed, ed.win, true, 1U);
        end = now_ns();
        {
            i64 mean = (end - start) / (i64)steps;

            (void)printf("search.step_mean    %12lld ns (limit %llu)\n",
                         (long long)mean, (unsigned long long)step_limit);
            if ((u64)mean > step_limit) {
                (void)fprintf(stderr,
                              "search_latency: `n` averaged %lld ns over "
                              "%u steps against a %llu ns budget\n",
                              (long long)mean, (unsigned)steps,
                              (unsigned long long)step_limit);
                status = 1;
            }
        }
    }

    rss_after = peak_rss();
    {
        u64 delta = rss_after > rss_before ? rss_after - rss_before : 0U;

        (void)printf("search.overlay_rss  %12llu bytes (limit %llu)\n",
                     (unsigned long long)delta,
                     (unsigned long long)rss_limit);
        if (delta > rss_limit) {
            (void)fprintf(stderr,
                          "search_latency: searching grew RSS by %llu bytes "
                          "— the overlay is holding more than a viewport\n",
                          (unsigned long long)delta);
            status = 1;
        }
    }
    arena_free_all(&arena);
    sag_ed_free(&ed);
    return status;
}
