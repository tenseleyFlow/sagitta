#define _POSIX_C_SOURCE 200809L

#include "harness.h"

#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "edit/ed.h"
#include "edit/loop.h"
#include "ui/cmdcomp.h"
#include "ui/cmdline.h"

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
    ed->tty.wfd = open("/dev/null", O_WRONLY);
    SAG_ASSERT(ed->tty.wfd >= 0);
    sag_timers_init(&ed->timers);
    sag_tty_probe_config(&ed->tty, 0, probe_disabled);
}

static void loop_ed_free(Ed *ed)
{
    sag_timers_free(&ed->timers);
    sag_input_free(&ed->in);
    bytebuf_free(&ed->tty.pending);
    if (ed->tty.wfd >= 0)
        (void)close(ed->tty.wfd);
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

/*
 * The poll deadline and the idle tick must agree on what "work pending"
 * means.
 *
 * sag_loop_deadline returns 0 — do not sleep — while a sliced completion
 * scan has more to read, because sag_cmdline_comp_tick is going to
 * advance it on the idle path.  The two conditions have to be the SAME
 * condition.  When the deadline tested only `cmdline.active` while the
 * tick also required SAG_PROMPT_CMD, a listing left pending under a
 * search prompt spun the loop at a zero timeout forever against a tick
 * that declined to touch it: a hot loop burning a core, with nothing on
 * screen to show for it.
 *
 * Driven with a 1 us budget for the same reason the cmdcomp tests are —
 * the scan checks its clock every 256 entries, so this leaves a scan
 * genuinely pending on any filesystem rather than hoping one is slow.
 */
void test_loop_deadline_and_comp_tick_share_one_condition(void)
{
    char root[] = "/tmp/sagitta-loopcomp-XXXXXX";
    CompFilter filter;
    Arena arena;
    Vec_CompItem out = {0};
    SagCompQuery q;
    Ed ed;
    u32 i;

    SAG_ASSERT_NOT_NULL(mkdtemp(root));
    for (i = 0U; i < 600U; i++) {
        char path[512];
        int fd;

        (void)snprintf(path, sizeof(path), "%s/entry%03u", root,
                       (unsigned)i);
        fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
        SAG_ASSERT(fd >= 0);
        SAG_ASSERT_EQ_I64(close(fd), 0);
    }

    loop_ed_init(&ed);
    arena_init(&ed.arena);
    ed.ws.dir = root;
    arena_init(&arena);
    sag_comp_filter_init(&filter);

    /* Leave a scan genuinely mid-flight. */
    (void)memset(&q, 0, sizeof(q));
    q.kind = SAG_COMP_PATH;
    q.source = sag_comp_source(SAG_COMP_PATH);
    q.stem = "e";
    (void)sag_comp_filter_run(&ed, &filter, &arena, &q, 1, &out);
    SAG_ASSERT(sag_comp_listing_pending());

    /* Nothing pending as far as the LOOP is concerned until a prompt is
     * up: an idle editor must still be allowed to sleep. */
    SAG_ASSERT(!ed.cmdline.active);
    SAG_ASSERT(!sag_cmdline_comp_scanning(&ed));
    SAG_ASSERT_EQ_I64(sag_loop_deadline(&ed, 1000), -1);

    /* `:` — the tick will act, so the loop must not sleep. */
    ed.cmdline.active = true;
    ed.cmdline.kind = SAG_PROMPT_CMD;
    SAG_ASSERT(sag_cmdline_comp_scanning(&ed));
    SAG_ASSERT_EQ_I64(sag_loop_deadline(&ed, 1000), 0);

    /*
     * A search prompt — the tick declines, so the loop MUST be allowed
     * to sleep.  This is the pair that used to disagree.
     */
    ed.cmdline.kind = SAG_PROMPT_SEARCH_F;
    SAG_ASSERT(!sag_cmdline_comp_scanning(&ed));
    SAG_ASSERT(!sag_cmdline_comp_tick(&ed));
    SAG_ASSERT_EQ_I64(sag_loop_deadline(&ed, 1000), -1);

    sag_comp_listing_invalidate();
    Vec_CompItem_free(&out);
    sag_comp_filter_free(&filter);
    arena_free_all(&arena);
    arena_free_all(&ed.arena);
    ed.ws.dir = NULL;
    loop_ed_free(&ed);
    for (i = 0U; i < 600U; i++) {
        char path[512];

        (void)snprintf(path, sizeof(path), "%s/entry%03u", root,
                       (unsigned)i);
        SAG_ASSERT_EQ_I64(unlink(path), 0);
    }
    SAG_ASSERT_EQ_I64(rmdir(root), 0);
}
