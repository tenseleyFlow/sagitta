#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "edit/ed.h"
#include "fl/record.h"

enum {
    TAP_ITERS = 1000000U,
    EMIT_EVENTS = 10000U,
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
    cx.source = SAG_SRC_TEST;
    start = now_ns();
    for (i = 0U; i < count; i++) {
        if (sag_cmd_invoke(id, &cx) != SAG_CMD_OK)
            return false;
    }
    *elapsed = now_ns() - start;
    perf_record_sink += ed->dispatch_count + ed->rec.ev.len;
    return true;
}

int main(int argc, char **argv)
{
    bool gate = argc == 2 && strcmp(argv[1], "--gate") == 0;
    Ed ed;
    CmdId escape;
    CmdCtx cx = {0};
    u64 baseline;
    u64 recorded;
    u64 overhead;
    u64 emit_start;
    u64 emit_elapsed;
    Bytebuf out;
    u32 i;

    if (argc > 2 || (argc == 2 && !gate)) {
        (void)fprintf(stderr, "usage: perf_record [--gate]\n");
        return 2;
    }
    sag_ed_init(&ed);
    if (!sag_ed_open_scratch(&ed)) {
        sag_ed_free(&ed);
        return 2;
    }
    escape = sag_cmd_lookup("ed.mode.escape", 14U);
    if (escape.v == 0U) {
        sag_ed_free(&ed);
        return 2;
    }
    RecEventVec_reserve(&ed.rec.ev, TAP_ITERS);
    sag_cmd_set_record_tap(NULL);
    if (!invoke_many(&ed, escape, TAP_ITERS, &baseline)) {
        sag_ed_free(&ed);
        return 2;
    }
    ed.rec.active = true;
    ed.rec.reg = (u8)'a';
    ed.rec.mode_at_start = (u8)SAG_MODE_L;
    sag_cmd_set_record_tap(sag_record_tap);
    if (!invoke_many(&ed, escape, TAP_ITERS, &recorded)) {
        sag_ed_free(&ed);
        return 2;
    }
    overhead = recorded > baseline ? (recorded - baseline) / TAP_ITERS : 0U;

    ed.rec.active = false;
    ed.rec.ev.len = 0U;
    RecEventVec_reserve(&ed.rec.ev, EMIT_EVENTS);
    cx.ed = &ed;
    cx.win = ed.win;
    cx.count = 1U;
    cx.source = SAG_SRC_TEST;
    ed.rec.active = true;
    for (i = 0U; i < EMIT_EVENTS; i++)
        sag_record_tap(escape, &cx);
    ed.rec.active = false;
    bytebuf_init(&out);
    emit_start = now_ns();
    sag_record_emit(&ed.rec, &ed, &out);
    emit_elapsed = now_ns() - emit_start;
    perf_record_sink += out.len;

    (void)printf("tap_overhead_ns %llu budget %u\n",
                 (unsigned long long)overhead, TAP_BUDGET_NS);
    (void)printf("emit_10k_ns %llu budget %u\n",
                 (unsigned long long)emit_elapsed, EMIT_BUDGET_NS);
    bytebuf_free(&out);
    sag_cmd_set_record_tap(sag_record_tap);
    sag_ed_free(&ed);
    if (gate && (overhead > TAP_BUDGET_NS ||
                 emit_elapsed > EMIT_BUDGET_NS))
        return 1;
    return 0;
}
