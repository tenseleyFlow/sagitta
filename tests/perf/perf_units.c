#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "edit/block.h"
#include "edit/ed.h"
#include "edit/motion.h"

#include "perf_policy.h"

enum {
    PERF_UNIT_LINES = 10000,
    PERF_UNIT_CALLS = 100000,
    PERF_UNIT_KEY_NS = 5000000
};

static volatile u64 perf_unit_sink;
static bool perf_units_advisory;

/*
 * At least 99% of representative calls must meet the keypress budget.
 * Under YEW_PERF_ADVISORY (shared hosted runners) the same 99% must only
 * stay inside the 100x sanity ceiling.  Every call's progress and bounds
 * are correctness checks and stay hard in both modes.
 */
static bool population_failed(u32 over_budget, u32 over_sanity, u32 calls,
                              bool advisory)
{
    return (advisory ? over_sanity : over_budget) > calls / 100U;
}

static const char *population_verdict(u32 over_budget, u32 over_sanity,
                                      u32 calls, bool advisory)
{
    if (population_failed(over_budget, over_sanity, calls, true))
        return " SANITY-FAIL";
    if (population_failed(over_budget, over_sanity, calls, false))
        return advisory ? " WARN" : " REGRESSION";
    return " ok";
}

static void count_call(i64 elapsed, u32 *over_budget, u32 *over_sanity)
{
    if (elapsed > PERF_UNIT_KEY_NS)
        (*over_budget)++;
    if (yew_perf_sample_failed((u64)elapsed, PERF_UNIT_KEY_NS, true))
        (*over_sanity)++;
}

static int selftest_policy(void)
{
    const u32 calls = PERF_UNIT_CALLS;
    const u32 allowed = calls / 100U;
    const u64 key = PERF_UNIT_KEY_NS;
    const u64 ceiling = key * YEW_PERF_ADVISORY_SANITY_MULTIPLIER;
    u32 over_budget = 0U;
    u32 over_sanity = 0U;

    count_call((i64)key, &over_budget, &over_sanity);
    count_call((i64)key + 1, &over_budget, &over_sanity);
    count_call((i64)ceiling, &over_budget, &over_sanity);
    count_call((i64)ceiling + 1, &over_budget, &over_sanity);
    if (over_budget != 3U || over_sanity != 1U ||
        population_failed(allowed, 0U, calls, false) ||
        !population_failed(allowed + 1U, 0U, calls, false) ||
        population_failed(calls, allowed, calls, true) ||
        !population_failed(calls, allowed + 1U, calls, true) ||
        yew_perf_sample_failed(0U, key, false) ||
        !yew_perf_sample_failed(key + 1U, key, false) ||
        yew_perf_sample_failed(ceiling, key, true) ||
        !yew_perf_sample_failed(ceiling + 1U, key, true) ||
        strcmp(population_verdict(allowed + 1U, 0U, calls, true),
               " WARN") != 0) {
        (void)fprintf(stderr, "perf-units: policy selftest failed\n");
        return 1;
    }
    (void)printf("perf-units-policy: strict/advisory/sanity ok\n");
    return 0;
}

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

static TextBuf *make_source_fixture(bool comma_rows)
{
    static const u8 statement[] = "    if ready { total }\n";
    static const u8 comma[] = "        0 => 31,\n";
    const u8 *row = comma_rows ? comma : statement;
    const size_t row_len = comma_rows ? sizeof(comma) - 1U :
                                         sizeof(statement) - 1U;
    const size_t len = row_len * PERF_UNIT_LINES;
    u8 *bytes = malloc(len);

    if (bytes == NULL)
        return NULL;
    for (size_t line = 0U; line < PERF_UNIT_LINES; line++)
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
    u32 over_sanity = 0U;
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
        count_call(elapsed, &over_budget, &over_sanity);
        p = next;
        perf_unit_sink ^= p.v + (u64)call;
    }
    total_elapsed = now_ns() - total_start;
    if (total_elapsed < 0)
        return false;
    (void)printf("perf-units: %-5s calls=%d total_ms=%.3f ns/op=%.1f "
                 "max_ms=%.3f over_5ms=%u%s\n",
                 ops->name, PERF_UNIT_CALLS,
                 (double)total_elapsed / 1000000.0,
                 (double)total_elapsed / (double)PERF_UNIT_CALLS,
                 (double)max_elapsed / 1000000.0, over_budget,
                 population_verdict(over_budget, over_sanity,
                                    PERF_UNIT_CALLS, perf_units_advisory));
    (void)fflush(stdout);
    /* A percentile gate tolerates scheduler preemption without averaging
     * slow engine work into invisibility. */
    return !population_failed(over_budget, over_sanity, PERF_UNIT_CALLS,
                              perf_units_advisory);
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
        if (elapsed < 0 ||
            yew_perf_sample_failed((u64)elapsed, PERF_UNIT_KEY_NS,
                                   perf_units_advisory)) {
            (void)fprintf(stderr, "perf-units: nested level %d took %lld ns "
                          "(budget %d, %s)\n", i, (long long)elapsed,
                          PERF_UNIT_KEY_NS,
                          yew_perf_mode(perf_units_advisory));
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

static bool measure_source_rows(bool comma_rows)
{
    TextBuf *tb = make_source_fixture(comma_rows);
    Buffer buffer = {0};
    UnitCtx u;
    ByteOff p;
    const int calls = comma_rows ? 1000 : PERF_UNIT_CALLS;
    i64 total_start;
    i64 max_elapsed = 0;
    u32 over_budget = 0U;
    u32 over_sanity = 0U;

    if (tb == NULL)
        return false;
    buffer.tb = tb;
    buffer.lang = comma_rows ? "wolf" : "c";
    buffer.tabwidth = 4U;
    u = (UnitCtx){tb, &buffer, NULL};
    p = BYTEOFF((u64)(PERF_UNIT_LINES / 2) *
                (comma_rows ? sizeof("        0 => 31,\n") - 1U :
                              sizeof("    if ready { total }\n") - 1U));
    total_start = now_ns();
    if (total_start < 0) {
        yew_textbuf_free(tb);
        return false;
    }
    for (int call = 0; call < calls; call++) {
        i64 start = now_ns();
        ByteOff next;
        i64 elapsed;

        if (p.v == yew_textbuf_len(tb))
            p = BYTEOFF(0U);
        next = yew_unit_block.next(&u, p, false);
        elapsed = now_ns() - start;
        if (elapsed < 0 || next.v <= p.v ||
            next.v > yew_textbuf_len(tb)) {
            yew_textbuf_free(tb);
            return false;
        }
        if (elapsed > max_elapsed)
            max_elapsed = elapsed;
        count_call(elapsed, &over_budget, &over_sanity);
        p = next;
        perf_unit_sink ^= p.v + (u64)call;
    }
    {
        i64 total_elapsed = now_ns() - total_start;

        (void)printf("perf-units: block-%-6s calls=%d total_ms=%.3f "
                     "ns/op=%.1f max_ms=%.3f over_5ms=%u%s\n",
                     comma_rows ? "comma" : "source", calls,
                     (double)total_elapsed / 1000000.0,
                     (double)total_elapsed / (double)calls,
                     (double)max_elapsed / 1000000.0, over_budget,
                     population_verdict(over_budget, over_sanity,
                                        (u32)calls, perf_units_advisory));
    }
    yew_textbuf_free(tb);
    return !population_failed(over_budget, over_sanity, (u32)calls,
                              perf_units_advisory);
}

int main(int argc, char **argv)
{
    static const UnitOps *const engines[] = {
        &yew_unit_line, &yew_unit_word, &yew_unit_block, &yew_unit_char,
    };
    TextBuf *tb = make_fixture();
    Buffer buffer = {0};
    UnitCtx u;
    size_t i;
    bool ok = true;

    if (argc == 2 && strcmp(argv[1], "--selftest-policy") == 0) {
        yew_textbuf_free(tb);
        return selftest_policy();
    }
    if (argc != 1) {
        (void)fprintf(stderr, "usage: %s [--selftest-policy]\n", argv[0]);
        yew_textbuf_free(tb);
        return 2;
    }
    if (tb == NULL)
        return 2;
    perf_units_advisory = yew_perf_advisory();
    (void)printf("perf-units: mode %s\n", yew_perf_mode(perf_units_advisory));
    buffer.tb = tb;
    buffer.tabwidth = 4U;
    u = (UnitCtx){tb, &buffer, NULL};
    for (i = 0U; i < YEW_ARRAY_LEN(engines); i++)
        if (!measure_engine(&u, engines[i]))
            ok = false;
    yew_textbuf_free(tb);
    if (!measure_nested())
        ok = false;
    if (!measure_source_rows(false))
        ok = false;
    if (!measure_source_rows(true))
        ok = false;
    if (!ok)
        (void)fprintf(stderr, "perf-units: keystroke budget exceeded\n");
    return ok ? 0 : 1;
}
