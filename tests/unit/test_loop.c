#include "harness.h"

#include <limits.h>
#include <string.h>

#include "edit/ed.h"
#include "edit/loop.h"

typedef struct TimerTrace {
    u32 values[64];
    size_t len;
} TimerTrace;

typedef struct TimerMark {
    TimerTrace *trace;
    u32 value;
} TimerMark;

typedef struct TimerSpawn {
    TimerHeap *timers;
    TimerMark first;
    TimerMark added;
    i64 at_ms;
} TimerSpawn;

static const char *probe_disabled(const char *name)
{
    return strcmp(name, "SAG_TTY_PROBE") == 0 ? "0" : NULL;
}

static const char *probe_enabled(const char *name)
{
    (void)name;
    return NULL;
}

static void timer_mark(Ed *ed, void *ctx)
{
    TimerMark *mark = ctx;

    (void)ed;
    mark->trace->values[mark->trace->len++] = mark->value;
}

static void timer_spawn(Ed *ed, void *ctx)
{
    TimerSpawn *spawn = ctx;

    timer_mark(ed, &spawn->first);
    (void)sag_timer_add(spawn->timers, spawn->at_ms, timer_mark,
                        &spawn->added);
}

static void loop_ed_init(Ed *ed)
{
    (void)memset(ed, 0, sizeof(*ed));
    ed->tty.rfd = -1;
    ed->tty.wfd = -1;
    sag_timers_init(&ed->timers);
    sag_tty_probe_config(&ed->tty, 0, probe_disabled);
}

static void loop_ed_free(Ed *ed)
{
    sag_timers_free(&ed->timers);
    sag_input_free(&ed->in);
    bytebuf_free(&ed->tty.pending);
}

void test_loop_deadline_matrix(void)
{
    Ed ed;
    TimerMark mark = {0};
    TimerId id;
    Key key;
    static const u8 queued[] = "x";

    loop_ed_init(&ed);
    SAG_ASSERT_EQ_I64(sag_loop_deadline(&ed, 1000), -1);
    sag_input_feed(&ed.in, queued, sizeof(queued) - 1U);
    SAG_ASSERT_EQ_I64(sag_loop_deadline(&ed, 1000), 0);
    SAG_ASSERT(sag_input_next(&ed.in, 1000, &key));
    SAG_ASSERT_EQ_U64(key.code, (u32)'x');
    SAG_ASSERT_EQ_I64(sag_loop_deadline(&ed, 1000), -1);

    ed.chord.n = 1U;
    ed.chord.deadline = 1400;
    SAG_ASSERT_EQ_I64(sag_loop_deadline(&ed, 1000), 400);
    ed.in.deadline = 1250;
    SAG_ASSERT_EQ_I64(sag_loop_deadline(&ed, 1000), 250);
    id = sag_timer_add(&ed.timers, 1200, timer_mark, &mark);
    SAG_ASSERT(id != SAG_TIMER_NONE);
    SAG_ASSERT_EQ_I64(sag_loop_deadline(&ed, 1000), 200);

    sag_tty_probe_config(&ed.tty, 1000, probe_enabled);
    SAG_ASSERT(!sag_tty_probe_done(&ed.tty));
    SAG_ASSERT_EQ_I64(sag_loop_deadline(&ed, 1000), 50);
    SAG_ASSERT_EQ_I64(sag_loop_deadline(&ed, 1049), 1);
    SAG_ASSERT_EQ_I64(sag_loop_deadline(&ed, 1050), 0);
    sag_tty_probe_tick(&ed.tty, 1050);
    SAG_ASSERT(sag_tty_probe_done(&ed.tty));
    SAG_ASSERT_EQ_I64(sag_loop_deadline(&ed, 1100), 100);
    SAG_ASSERT_EQ_I64(sag_loop_deadline(&ed, 1200), 0);

    ed.in.deadline = 0;
    ed.chord.n = 0U;
    SAG_ASSERT(sag_timer_cancel(&ed.timers, id));
    SAG_ASSERT_EQ_I64(sag_loop_deadline(&ed, 1200), -1);
    loop_ed_free(&ed);
}

void test_loop_deadline_clamps_poll_range(void)
{
    Ed ed;
    TimerTrace trace = {0};
    TimerMark mark = {&trace, 1U};
    TimerId id;

    loop_ed_init(&ed);
    ed.chord.n = 1U;
    ed.chord.deadline = INT64_MAX;
    SAG_ASSERT_EQ_I64(sag_loop_deadline(&ed, 0), INT_MAX);
    SAG_ASSERT_EQ_I64(sag_loop_deadline(&ed, INT64_MAX), 0);
    ed.chord.n = 0U;
    id = sag_timer_add(&ed.timers, INT64_MAX, timer_mark, &mark);
    SAG_ASSERT_EQ_I64(sag_loop_deadline(&ed, -1), INT_MAX);
    SAG_ASSERT_EQ_I64(sag_loop_deadline(&ed, INT64_MAX - 7), 7);
    SAG_ASSERT(sag_timer_cancel(&ed.timers, id));
    SAG_ASSERT_EQ_I64(sag_timers_deadline(&ed.timers, 0), -1);
    loop_ed_free(&ed);
}

void test_timer_heap_stable_equal_deadlines(void)
{
    TimerHeap timers;
    TimerTrace trace = {0};
    TimerMark marks[8];
    TimerId ids[8];
    size_t i;

    sag_timers_init(&timers);
    for (i = 0U; i < SAG_ARRAY_LEN(marks); i++) {
        marks[i].trace = &trace;
        marks[i].value = (u32)i;
        ids[i] = sag_timer_add(&timers, 500, timer_mark, &marks[i]);
        SAG_ASSERT(ids[i] != SAG_TIMER_NONE);
        if (i != 0U)
            SAG_ASSERT(ids[i] > ids[i - 1U]);
    }
    SAG_ASSERT_EQ_I64(sag_timers_deadline(&timers, 100), 400);
    sag_timers_fire(&timers, NULL, 499);
    SAG_ASSERT_EQ_U64(trace.len, 0U);
    sag_timers_fire(&timers, NULL, 500);
    SAG_ASSERT_EQ_U64(trace.len, SAG_ARRAY_LEN(marks));
    for (i = 0U; i < SAG_ARRAY_LEN(marks); i++)
        SAG_ASSERT_EQ_U64(trace.values[i], i);
    SAG_ASSERT_EQ_I64(sag_timers_deadline(&timers, 500), -1);
    sag_timers_free(&timers);
}

void test_timer_cancel_never_fires(void)
{
    TimerHeap timers;
    TimerTrace trace = {0};
    TimerMark marks[7];
    TimerId ids[7];
    static const i64 deadlines[] = {30, 10, 20, 10, 40, 15, 35};
    static const u32 expected[] = {1, 5, 2, 6, 4};
    size_t i;

    sag_timers_init(&timers);
    for (i = 0U; i < SAG_ARRAY_LEN(marks); i++) {
        marks[i].trace = &trace;
        marks[i].value = (u32)i;
        ids[i] = sag_timer_add(&timers, deadlines[i], timer_mark,
                               &marks[i]);
    }
    SAG_ASSERT(sag_timer_cancel(&timers, ids[3]));
    SAG_ASSERT(sag_timer_cancel(&timers, ids[0]));
    SAG_ASSERT(!sag_timer_cancel(&timers, ids[3]));
    SAG_ASSERT(!sag_timer_cancel(&timers, SAG_TIMER_NONE));
    SAG_ASSERT(!sag_timer_cancel(NULL, ids[1]));
    SAG_ASSERT_EQ_I64(sag_timers_deadline(&timers, 0), 10);
    sag_timers_fire(&timers, NULL, 100);
    SAG_ASSERT_EQ_U64(trace.len, SAG_ARRAY_LEN(expected));
    for (i = 0U; i < SAG_ARRAY_LEN(expected); i++)
        SAG_ASSERT_EQ_U64(trace.values[i], expected[i]);
    SAG_ASSERT_EQ_U64(timers.len, 0U);
    sag_timers_free(&timers);
}

void test_timer_callback_may_add_due_timer(void)
{
    TimerHeap timers;
    TimerTrace trace = {0};
    TimerMark tail = {&trace, 3U};
    TimerSpawn spawn;

    sag_timers_init(&timers);
    spawn.timers = &timers;
    spawn.first = (TimerMark){&trace, 1U};
    spawn.added = (TimerMark){&trace, 2U};
    spawn.at_ms = 100;
    (void)sag_timer_add(&timers, 100, timer_spawn, &spawn);
    (void)sag_timer_add(&timers, 100, timer_mark, &tail);
    sag_timers_fire(&timers, NULL, 100);
    SAG_ASSERT_EQ_U64(trace.len, 3U);
    SAG_ASSERT_EQ_U64(trace.values[0], 1U);
    SAG_ASSERT_EQ_U64(trace.values[1], 3U);
    SAG_ASSERT_EQ_U64(trace.values[2], 2U);
    SAG_ASSERT_EQ_U64(timers.len, 0U);
    sag_timers_free(&timers);
}
