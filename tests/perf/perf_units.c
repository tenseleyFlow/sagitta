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
    return sag_textbuf_from_owned_bytes(bytes, len);
}

static bool measure_engine(UnitCtx *u, const UnitOps *ops)
{
    ByteOff p = BYTEOFF(0U);
    u64 len = sag_textbuf_len(u->tb);
    i64 start = now_ns();
    i64 elapsed;
    int call;

    if (start < 0)
        return false;
    for (call = 0; call < PERF_UNIT_CALLS; call++) {
        ByteOff next;

        if (p.v == len &&
            (ops != &sag_unit_block || call % 100 == 0))
            p = BYTEOFF(0U);
        next = ops->next(u, p, false);
        if ((p.v < len && next.v <= p.v) || next.v > len)
            return false;
        p = next;
        perf_unit_sink ^= p.v + (u64)call;
    }
    elapsed = now_ns() - start;
    if (elapsed < 0)
        return false;
    (void)printf("perf-units: %-5s calls=%d total_ms=%.3f ns/op=%.1f\n",
                 ops->name, PERF_UNIT_CALLS,
                 (double)elapsed / 1000000.0,
                 (double)elapsed / (double)PERF_UNIT_CALLS);
    (void)fflush(stdout);
    return elapsed <= (i64)PERF_UNIT_CALLS * PERF_UNIT_KEY_NS;
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
    int i;

    for (i = 0; i < DEPTH; i++)
        bytes[i] = (u8)'{';
    bytes[DEPTH] = (u8)'x';
    for (i = 0; i < DEPTH; i++)
        bytes[DEPTH + 1 + i] = (u8)'}';
    bytes[sizeof(bytes) - 1U] = (u8)'\n';
    tb = sag_textbuf_from_bytes(bytes, sizeof(bytes));
    if (tb == NULL)
        return false;
    buffer.tb = tb;
    buffer.tabwidth = 4U;
    u = (UnitCtx){tb, &buffer, NULL};
    start = now_ns();
    for (i = 0; i < DEPTH; i++) {
        if (!sag_block_level(&u, BYTEOFF(DEPTH), (u32)i, &span)) {
            sag_textbuf_free(tb);
            return false;
        }
        perf_unit_sink ^= span.lo + span.hi;
    }
    elapsed = now_ns() - start;
    sag_textbuf_free(tb);
    if (elapsed < 0)
        return false;
    (void)printf("perf-units: nested-depth=%d total_ms=%.3f\n", DEPTH,
                 (double)elapsed / 1000000.0);
    return elapsed <= (i64)DEPTH * PERF_UNIT_KEY_NS;
}

int main(void)
{
    static const UnitOps *const engines[] = {
        &sag_unit_line, &sag_unit_word, &sag_unit_block, &sag_unit_char,
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
    for (i = 0U; i < SAG_ARRAY_LEN(engines); i++)
        if (!measure_engine(&u, engines[i]))
            ok = false;
    sag_textbuf_free(tb);
    if (!measure_nested())
        ok = false;
    if (!ok)
        (void)fprintf(stderr, "perf-units: keystroke budget exceeded\n");
    return ok ? 0 : 1;
}
