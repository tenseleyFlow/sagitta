#define _POSIX_C_SOURCE 200809L

#include "unicode/grapheme.h"
#include "unicode/utf8.h"
#include "unicode/width.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "perf_policy.h"

enum { CORPUS_MIN = 1024 * 1024, PERF_ROUNDS = 7 };

/* The ASCII megabyte must scan inside one 5 ms keypress budget. */
#define UNICODE_ASCII_BUDGET_NS INT64_C(5000000)

typedef struct {
    const char *name;
    const u8 *pattern;
    size_t pattern_len;
    u8 *data;
    size_t len;
    size_t clusters;
    int cells;
    i64 best_ns;
} PerfCase;

static volatile size_t perf_sink;

static i64 now_ns(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        perror("clock_gettime");
        return -1;
    }
    return (i64)ts.tv_sec * 1000000000LL + (i64)ts.tv_nsec;
}

static bool prepare(PerfCase *pc)
{
    size_t repeats = (CORPUS_MIN + pc->pattern_len - 1u) / pc->pattern_len;
    size_t i;

    if (repeats > SIZE_MAX / pc->pattern_len)
        return false;
    pc->len = repeats * pc->pattern_len;
    pc->data = malloc(pc->len);
    if (pc->data == NULL)
        return false;
    for (i = 0u; i < repeats; i++)
        memcpy(pc->data + i * pc->pattern_len, pc->pattern,
               pc->pattern_len);
    return true;
}

static bool scan(const u8 *s, size_t len, size_t *clusters, int *cells)
{
    YewGbState state;
    size_t pos = 0u;
    size_t cluster_start = 0u;
    size_t count = 0u;
    int width = 0;
    bool have_cluster = false;
    bool simple_ascii = false;

    yew_gb_init(&state);
    while (pos < len) {
        u32 cp;
        size_t used;
        bool boundary;

        if (state.prev_gcb == YEW_GCB_OTHER &&
            s[pos] >= 0x20u && s[pos] <= 0x7eu) {
            if (have_cluster) {
                width += simple_ascii
                             ? 1
                             : yew_cluster_width(s + cluster_start,
                                                 pos - cluster_start);
                count++;
            }
            if (!yew_gb_boundary(&state, s[pos]))
                return false;
            cluster_start = pos;
            have_cluster = true;
            simple_ascii = true;
            pos++;
            continue;
        }
        used = yew_utf8_decode(s + pos, len - pos, &cp);
        if (used == 0u || used > len - pos)
            return false;
        boundary = yew_gb_boundary(&state, cp);
        if (boundary && have_cluster) {
            width += simple_ascii
                         ? 1
                         : yew_cluster_width(s + cluster_start,
                                             pos - cluster_start);
            count++;
        }
        if (boundary)
            cluster_start = pos;
        simple_ascii = false;
        have_cluster = true;
        pos += used;
    }
    if (have_cluster) {
        width += simple_ascii
                     ? 1
                     : yew_cluster_width(s + cluster_start,
                                         len - cluster_start);
        count++;
    }
    *clusters = count;
    *cells = width;
    perf_sink = count + (size_t)width;
    return true;
}

static bool measure(PerfCase *pc)
{
    int round;

    if (!scan(pc->data, pc->len, &pc->clusters, &pc->cells))
        return false;
    pc->best_ns = INT64_MAX;
    for (round = 0; round < PERF_ROUNDS; round++) {
        i64 start = now_ns();
        i64 elapsed;
        size_t clusters;
        int cells;

        if (start < 0)
            return false;
        if (!scan(pc->data, pc->len, &clusters, &cells))
            return false;
        elapsed = now_ns() - start;
        if (elapsed < 0)
            return false;
        if (elapsed < pc->best_ns)
            pc->best_ns = elapsed;
        if (clusters != pc->clusters || cells != pc->cells)
            return false;
        perf_sink = pc->clusters + (size_t)cells;
    }
    return true;
}

/* Strict by default; advisory (hosted runners) fails only a zero or
 * beyond-100x scan.  Cluster/cell agreement stays a hard check. */
static bool ascii_failed(i64 best_ns, bool advisory)
{
    return best_ns < 0 ||
           yew_perf_timing_failed((u64)best_ns,
                                  (u64)UNICODE_ASCII_BUDGET_NS, advisory);
}

static int selftest_policy(void)
{
    const i64 budget = UNICODE_ASCII_BUDGET_NS;
    const i64 ceiling = budget * YEW_PERF_ADVISORY_SANITY_MULTIPLIER;

    if (ascii_failed(budget, false) || !ascii_failed(budget + 1, false) ||
        ascii_failed(budget + 1, true) || ascii_failed(ceiling, true) ||
        !ascii_failed(ceiling + 1, true) || !ascii_failed(0, true) ||
        !ascii_failed(-1, true)) {
        fprintf(stderr, "unicode-perf: policy selftest failed\n");
        return 1;
    }
    printf("perf-unicode-policy: strict/advisory/sanity ok\n");
    return 0;
}

int main(int argc, char **argv)
{
    static const u8 ascii[] = {'a'};
    static const u8 cjk[] = {0xe6u, 0xbcu, 0xa2u};
    static const u8 emoji[] = {
        0xf0u, 0x9fu, 0x91u, 0xa8u, 0xe2u, 0x80u, 0x8du,
        0xf0u, 0x9fu, 0x91u, 0xa9u, 0xe2u, 0x80u, 0x8du,
        0xf0u, 0x9fu, 0x91u, 0xa7u, 0xe2u, 0x80u, 0x8du,
        0xf0u, 0x9fu, 0x91u, 0xa6u
    };
    PerfCase cases[] = {
        {"ascii", ascii, sizeof(ascii), NULL, 0u, 0u, 0, 0},
        {"cjk", cjk, sizeof(cjk), NULL, 0u, 0u, 0, 0},
        {"emoji", emoji, sizeof(emoji), NULL, 0u, 0u, 0, 0}
    };
    size_t i;
    int status = 0;
    bool advisory = yew_perf_advisory();

    if (argc == 2 && strcmp(argv[1], "--selftest-policy") == 0)
        return selftest_policy();
    if (argc != 1) {
        fprintf(stderr, "usage: %s [--selftest-policy]\n", argv[0]);
        return 2;
    }
    printf("unicode-perf: mode %s\n", yew_perf_mode(advisory));
    for (i = 0u; i < YEW_ARRAY_LEN(cases); i++) {
        if (!prepare(&cases[i]) || !measure(&cases[i])) {
            fprintf(stderr, "unicode-perf: %s failed\n", cases[i].name);
            status = 1;
            break;
        }
        printf("unicode-perf: %s bytes=%zu clusters=%zu cells=%d best_us=%lld%s\n",
               cases[i].name, cases[i].len, cases[i].clusters,
               cases[i].cells, (long long)(cases[i].best_ns / 1000LL),
               i != 0u ? "" :
               cases[i].best_ns < 0 ? " SANITY-FAIL" :
               yew_perf_timing_verdict((u64)cases[i].best_ns,
                                       (u64)UNICODE_ASCII_BUDGET_NS,
                                       advisory));
        if (i == 0u && ascii_failed(cases[i].best_ns, advisory))
            status = 1;
    }
    for (i = 0u; i < YEW_ARRAY_LEN(cases); i++)
        free(cases[i].data);
    return status;
}
