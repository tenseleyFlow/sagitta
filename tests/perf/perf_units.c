#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "edit/block.h"
#include "edit/ed.h"
#include "edit/motion.h"

enum {
    PERF_UNIT_LINES = 10000,
    PERF_UNIT_CALLS = 100000,
    PERF_UNIT_KEY_NS = 5000000
};

static volatile u64 perf_unit_sink;

static i64 now_ns(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return -1;
    return (i64)ts.tv_sec * INT64_C(1000000000) + ts.tv_nsec;
}

static TextBuf *make_fixture(void)
{
    static const u8 row[] = "item value words\n";
    size_t row_len = sizeof(row) - 1U;
    size_t len = row_len * PERF_UNIT_LINES;
    u8 *bytes = malloc(len);
    size_t line;

    if (bytes == NULL)
        return NULL;
    for (line = 0U; line < PERF_UNIT_LINES; line++)
        (void)memcpy(bytes + line * row_len, row, row_len);
    return yew_textbuf_from_owned_bytes(bytes, len);
}

static bool measure_engine(UnitCtx *u, const UnitOps *ops)
{
    ByteOff p = BYTEOFF(0U);
    u64 len = yew_textbuf_len(u->tb);
    i64 total_start = now_ns();
    i64 total_elapsed;
    i64 max_elapsed = 0;
    u32 over_budget = 0U;
    int call;

    if (total_start < 0)
        return false;
    for (call = 0; call < PERF_UNIT_CALLS; call++) {
        i64 start;
        i64 elapsed;
        ByteOff next;

        /* Every timed call must advance.  Endpoint fixed points are part of
         * the API contract, but measuring them would hide real keystroke
         * latency for engines whose first unit spans the whole fixture. */
        if (p.v == len)
            p = BYTEOFF(0U);
        start = now_ns();
        if (start < 0)
            return false;
        next = ops->next(u, p, false);
        elapsed = now_ns() - start;
        if (elapsed < 0 || next.v <= p.v || next.v > len)
            return false;
        if (elapsed > max_elapsed)
            max_elapsed = elapsed;
        if (elapsed > PERF_UNIT_KEY_NS)
            over_budget++;
        p = next;
        perf_unit_sink ^= p.v + (u64)call;
    }
    total_elapsed = now_ns() - total_start;
    if (total_elapsed < 0)
        return false;
    (void)printf("perf-units: %-5s calls=%d total_ms=%.3f ns/op=%.1f "
                 "max_ms=%.3f over_5ms=%u\n",
                 ops->name, PERF_UNIT_CALLS,
                 (double)total_elapsed / 1000000.0,
                 (double)total_elapsed / (double)PERF_UNIT_CALLS,
                 (double)max_elapsed / 1000000.0, over_budget);
    (void)fflush(stdout);
    /* At least 99% of representative calls must meet the keypress budget.
     * A percentile gate tolerates scheduler preemption without averaging
     * slow engine work into invisibility. */
    return over_budget <= PERF_UNIT_CALLS / 100;
}

static bool measure_nested(void)
{
    enum { DEPTH = 32 };
    u8 bytes[DEPTH * 2U + 2U];
    TextBuf *tb;
    Buffer buffer = {0};
    UnitCtx u;
    Span span;
    i64 start;
    i64 elapsed;
    i64 max_elapsed = 0;
    int i;

    for (i = 0; i < DEPTH; i++)
        bytes[i] = (u8)'{';
    bytes[DEPTH] = (u8)'x';
    for (i = 0; i < DEPTH; i++)
        bytes[DEPTH + 1 + i] = (u8)'}';
    bytes[sizeof(bytes) - 1U] = (u8)'\n';
    tb = yew_textbuf_from_bytes(bytes, sizeof(bytes));
    if (tb == NULL)
        return false;
    buffer.tb = tb;
    buffer.tabwidth = 4U;
    u = (UnitCtx){tb, &buffer, NULL};
    for (i = 0; i < DEPTH; i++) {
        start = now_ns();
        if (start < 0) {
            yew_textbuf_free(tb);
            return false;
        }
        if (!yew_block_level(&u, BYTEOFF(DEPTH), (u32)i, &span)) {
            yew_textbuf_free(tb);
            return false;
        }
        elapsed = now_ns() - start;
        if (elapsed < 0 || elapsed > PERF_UNIT_KEY_NS) {
            yew_textbuf_free(tb);
            return false;
        }
        if (elapsed > max_elapsed)
            max_elapsed = elapsed;
        perf_unit_sink ^= span.lo + span.hi;
    }
    yew_textbuf_free(tb);
    (void)printf("perf-units: nested-depth=%d max_ms=%.3f\n", DEPTH,
                 (double)max_elapsed / 1000000.0);
    return true;
}

int main(void)
{
    static const UnitOps *const engines[] = {
        &yew_unit_line, &yew_unit_word, &yew_unit_block, &yew_unit_char,
    };
    TextBuf *tb = make_fixture();
    Buffer buffer = {0};
    UnitCtx u;
    size_t i;
    bool ok = true;

    if (tb == NULL)
        return 2;
    buffer.tb = tb;
    buffer.tabwidth = 4U;
    u = (UnitCtx){tb, &buffer, NULL};
    for (i = 0U; i < YEW_ARRAY_LEN(engines); i++)
        if (!measure_engine(&u, engines[i]))
            ok = false;
    yew_textbuf_free(tb);
    if (!measure_nested())
        ok = false;
    if (!ok)
        (void)fprintf(stderr, "perf-units: keystroke budget exceeded\n");
    return ok ? 0 : 1;
}
