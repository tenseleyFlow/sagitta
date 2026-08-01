#define _POSIX_C_SOURCE 200809L

#include "unicode/grapheme.h"
#include "unicode/width.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

enum { CORPUS_MIN = 1024 * 1024, PERF_ROUNDS = 7 };

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
    size_t pos = 0u;
    size_t count = 0u;
    int width = 0;

    while (pos < len) {
        SagCluster cluster;
        size_t before = pos;

        if (!sag_cluster_next(s, len, &pos, &cluster) || pos <= before)
            return false;
        count++;
        width += (int)cluster.cells;
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
        int cells;

        if (start < 0)
            return false;
        cells = sag_str_width(pc->data, pc->len, 4u);
        elapsed = now_ns() - start;
        if (elapsed < 0)
            return false;
        if (elapsed < pc->best_ns)
            pc->best_ns = elapsed;
        if (cells != pc->cells)
            return false;
        perf_sink = pc->clusters + (size_t)cells;
    }
    return true;
}

int main(void)
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

    for (i = 0u; i < SAG_ARRAY_LEN(cases); i++) {
        if (!prepare(&cases[i]) || !measure(&cases[i])) {
            fprintf(stderr, "unicode-perf: %s failed\n", cases[i].name);
            status = 1;
            break;
        }
        printf("unicode-perf: %s bytes=%zu clusters=%zu cells=%d best_us=%lld%s\n",
               cases[i].name, cases[i].len, cases[i].clusters,
               cases[i].cells, (long long)(cases[i].best_ns / 1000LL),
               i == 0u && cases[i].best_ns > 5000000LL ? " OVER-BUDGET" : "");
        if (i == 0u && cases[i].best_ns > 5000000LL)
            status = 1;
    }
    for (i = 0u; i < SAG_ARRAY_LEN(cases); i++)
        free(cases[i].data);
    return status;
}
