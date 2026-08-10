#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "text/piece.h"

enum {
    INITIAL_BYTES = 1024 * 1024,
    INSERT_OPS = 1000000
};

/*
 * MEASURED, and widened because the runner varies more than the code.
 *
 * Identical work -- same seed, same pieces=1755012 -- measured 1281,
 * 1461 and 2162 ms on three ubuntu-latest runners: a 1.7x spread BETWEEN
 * INSTANCES OF THE SAME LABEL.  2000 ms sat inside that spread, so the
 * lane failed on machine speed rather than on anything in the tree, and
 * its three retries all failed together because that runner was
 * consistently slow rather than transiently so.
 *
 * 3500 is ~1.6x the slowest observed.  Note honestly what that costs:
 * this now catches only GROSS regressions, and on a fast runner a large
 * one can still hide.  A ratio against a yardstick measured in the same
 * process would gate uniformly instead, and was tried -- three ways --
 * but the yardstick's own variance (33-65 ms under contention) exceeded
 * the signal, and the version that looked best would have passed a
 * measured regression.  Shipping that would have been worse than this.
 * Doing it properly needs calibration on a quiet machine.
 *
 * ONE constant, used by both the check and the printed line: they were
 * separate literals, which is how a budget and the number it reports
 * drift apart.
 */
#define PIECE_BUDGET_MS 3500

static i64 now_ns(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        (void)fprintf(stderr, "perf_piece: clock_gettime: %s\n",
                      strerror(errno));
        return -1;
    }
    return (i64)ts.tv_sec * INT64_C(1000000000) + ts.tv_nsec;
}

static u64 next_random(u64 *state)
{
    u64 x = *state;

    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    *state = x;
    return x;
}

typedef struct {
    u64 at;
    u8 len;
    u8 bytes[8];
} PerfInsert;

int main(void)
{
    static const u64 seed = UINT64_C(0x9e3779b97f4a7c15);
    static const i64 budget_ns =
        (i64)PIECE_BUDGET_MS * INT64_C(1000000);
    u8 *initial = malloc(INITIAL_BYTES);
    PerfInsert *ops = malloc(sizeof(*ops) * INSERT_OPS);
    TextBuf *tb;
    u64 rng = seed;
    u64 expected_len = INITIAL_BYTES;
    i64 start;
    i64 elapsed;
    size_t op;

    if (initial == NULL || ops == NULL) {
        (void)fprintf(stderr, "perf_piece: fixture allocation failed\n");
        free(initial);
        free(ops);
        return 2;
    }
    (void)memset(initial, 'x', INITIAL_BYTES);
    for (op = 0U; op < INSERT_OPS; op++) {
        u64 random = next_random(&rng);
        u64 i;

        ops[op].len = (u8)(1U + random % 8U);
        ops[op].at = next_random(&rng) % (expected_len + 1U);
        for (i = 0U; i < ops[op].len; i++)
            ops[op].bytes[i] = (u8)('a' + next_random(&rng) % 26U);
        expected_len += ops[op].len;
    }
    tb = yew_textbuf_from_bytes(initial, INITIAL_BYTES);
    free(initial);
    if (tb == NULL) {
        (void)fprintf(stderr, "perf_piece: buffer construction failed\n");
        free(ops);
        return 2;
    }
    start = now_ns();
    if (start < 0) {
        free(ops);
        yew_textbuf_free(tb);
        return 2;
    }
    for (op = 0U; op < INSERT_OPS; op++)
        yew_textbuf_insert(tb, BYTEOFF(ops[op].at), ops[op].bytes,
                           ops[op].len);
    elapsed = now_ns() - start;
    free(ops);
    if (elapsed < 0) {
        yew_textbuf_free(tb);
        return 2;
    }
    yew_textbuf_check(tb);
    (void)printf("piece-perf: seed=%016llx ops=%u bytes=%llu pieces=%u "
                 "elapsed_ms=%lld budget_ms=%d%s\n",
                 (unsigned long long)seed, INSERT_OPS,
                 (unsigned long long)yew_textbuf_len(tb),
                 yew_textbuf_piece_count(tb),
                 (long long)(elapsed / INT64_C(1000000)),
                 PIECE_BUDGET_MS,
                 elapsed < budget_ns ? "" : " OVER-BUDGET");
    if (yew_textbuf_len(tb) != expected_len || elapsed >= budget_ns) {
        yew_textbuf_free(tb);
        return 1;
    }
    yew_textbuf_free(tb);
    return 0;
}
