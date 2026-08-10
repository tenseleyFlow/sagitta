#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "edit/ed.h"
#include "fl/record.h"

enum {
    TAP_ITERS = 1000000U,
    EMIT_EVENTS = 10000U,
    PERF_SAMPLES = 7U,
    TAP_BUDGET_NS = 200U,
    EMIT_BUDGET_NS = 10000000U
};

static volatile u64 perf_record_sink;

static u64 now_ns(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return 0U;
    return (u64)ts.tv_sec * UINT64_C(1000000000) + (u64)ts.tv_nsec;
}

static bool invoke_many(Ed *ed, CmdId id, u32 count, u64 *elapsed)
{
    CmdCtx cx = {0};
    u64 start;
    u32 i;

    cx.ed = ed;
    cx.win = ed->win;
    cx.count = 1U;
    cx.source = YEW_SRC_TEST;
    start = now_ns();
    for (i = 0U; i < count; i++) {
        if (yew_cmd_invoke(id, &cx) != YEW_CMD_OK)
            return false;
    }
    *elapsed = now_ns() - start;
    perf_record_sink += ed->dispatch_count + ed->rec.ev.len;
    return true;
}

static i64 elapsed_delta(u64 recorded, u64 baseline)
{
    if (recorded >= baseline)
        return (i64)((recorded - baseline) / TAP_ITERS);
    return -(i64)((baseline - recorded) / TAP_ITERS);
}

static void sort_i64(i64 *values, u32 len)
{
    u32 i;

    for (i = 1U; i < len; i++) {
        i64 value = values[i];
        u32 j = i;

        while (j != 0U && values[j - 1U] > value) {
            values[j] = values[j - 1U];
            j--;
        }
        values[j] = value;
    }
}

static void sort_u64(u64 *values, u32 len)
{
    u32 i;

    for (i = 1U; i < len; i++) {
        u64 value = values[i];
        u32 j = i;

        while (j != 0U && values[j - 1U] > value) {
            values[j] = values[j - 1U];
            j--;
        }
        values[j] = value;
    }
}

int main(int argc, char **argv)
{
    bool gate = argc == 2 && strcmp(argv[1], "--gate") == 0;
    Ed ed;
    CmdId escape;
    CmdCtx cx = {0};
    i64 tap_samples[PERF_SAMPLES];
    u64 emit_samples[PERF_SAMPLES];
    i64 overhead;
    u64 emit_start;
    u64 emit_elapsed;
    Bytebuf out;
    u32 i;

    if (argc > 2 || (argc == 2 && !gate)) {
        (void)fprintf(stderr, "usage: perf_record [--gate]\n");
        return 2;
    }
    yew_ed_init(&ed);
    if (!yew_ed_open_scratch(&ed)) {
        yew_ed_free(&ed);
        return 2;
    }
    escape = yew_cmd_lookup("ed.mode.escape", 14U);
    if (escape.v == 0U) {
        yew_ed_free(&ed);
        return 2;
    }
    RecEventVec_reserve(&ed.rec.ev, TAP_ITERS);
    for (i = 0U; i < PERF_SAMPLES; i++) {
        u64 baseline;
        u64 recorded;

        yew_cmd_set_record_tap(NULL);
        if (!invoke_many(&ed, escape, TAP_ITERS, &baseline)) {
            yew_ed_free(&ed);
            return 2;
        }
        ed.rec.active = true;
        ed.rec.reg = (u8)'a';
        ed.rec.mode_at_start = (u8)YEW_MODE_L;
        ed.rec.ev.len = 0U;
        yew_cmd_set_record_tap(yew_record_tap);
        if (!invoke_many(&ed, escape, TAP_ITERS, &recorded)) {
            yew_ed_free(&ed);
            return 2;
        }
        ed.rec.active = false;
        tap_samples[i] = elapsed_delta(recorded, baseline);
    }
    sort_i64(tap_samples, PERF_SAMPLES);
    overhead = tap_samples[PERF_SAMPLES / 2U];

    ed.rec.ev.len = 0U;
    RecEventVec_reserve(&ed.rec.ev, EMIT_EVENTS);
    cx.ed = &ed;
    cx.win = ed.win;
    cx.count = 1U;
    cx.source = YEW_SRC_TEST;
    ed.rec.active = true;
    for (i = 0U; i < EMIT_EVENTS; i++)
        yew_record_tap(escape, &cx);
    ed.rec.active = false;
    bytebuf_init(&out);
    for (i = 0U; i < PERF_SAMPLES; i++) {
        out.len = 0U;
        emit_start = now_ns();
        yew_record_emit(&ed.rec, &ed, &out);
        emit_samples[i] = now_ns() - emit_start;
    }
    sort_u64(emit_samples, PERF_SAMPLES);
    emit_elapsed = emit_samples[PERF_SAMPLES / 2U];
    perf_record_sink += out.len;

    (void)printf("tap_overhead_ns %lld budget %u median_samples %u\n",
                 (long long)overhead, TAP_BUDGET_NS, PERF_SAMPLES);
    (void)printf("emit_10k_ns %llu budget %u\n",
                 (unsigned long long)emit_elapsed, EMIT_BUDGET_NS);
    bytebuf_free(&out);
    yew_cmd_set_record_tap(yew_record_tap);
    yew_ed_free(&ed);
    if (gate && (overhead > (i64)TAP_BUDGET_NS ||
                 emit_elapsed > EMIT_BUDGET_NS))
        return 1;
    return 0;
}
