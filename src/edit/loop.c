#define _POSIX_C_SOURCE 200809L

#include "edit/loop.h"

#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "edit/dispatch.h"
#include "edit/ed.h"
#include "edit/job.h"
#include "edit/shell.h"
#include "term/input.h"
#include "term/tty.h"
#include "util/log.h"

static const char *loop_getenv(const char *name)
{
    return getenv(name);
}

struct SagTimer {
    i64 at_ms;
    u64 order;
    TimerId id;
    TimerFn fn;
    void *ctx;
};

static bool timer_before(const SagTimer *a, const SagTimer *b)
{
    return a->at_ms < b->at_ms ||
           (a->at_ms == b->at_ms && a->order < b->order);
}

static void timer_swap(SagTimer *a, SagTimer *b)
{
    SagTimer tmp = *a;

    *a = *b;
    *b = tmp;
}

static void timer_up(TimerHeap *timers, size_t at)
{
    while (at != 0U) {
        size_t parent = (at - 1U) / 2U;

        if (!timer_before(&timers->v[at], &timers->v[parent]))
            break;
        timer_swap(&timers->v[at], &timers->v[parent]);
        at = parent;
    }
}

static void timer_down(TimerHeap *timers, size_t at)
{
    for (;;) {
        size_t left = at * 2U + 1U;
        size_t right = left + 1U;
        size_t next = at;

        if (left < timers->len &&
            timer_before(&timers->v[left], &timers->v[next]))
            next = left;
        if (right < timers->len &&
            timer_before(&timers->v[right], &timers->v[next]))
            next = right;
        if (next == at)
            break;
        timer_swap(&timers->v[at], &timers->v[next]);
        at = next;
    }
}

static SagTimer timer_pop(TimerHeap *timers)
{
    SagTimer first = timers->v[0];

    timers->len--;
    if (timers->len != 0U) {
        timers->v[0] = timers->v[timers->len];
        timer_down(timers, 0U);
    }
    return first;
}

void sag_timers_init(TimerHeap *timers)
{
    if (timers == NULL)
        return;
    (void)memset(timers, 0, sizeof(*timers));
    timers->next_id = 1U;
}

void sag_timers_free(TimerHeap *timers)
{
    if (timers == NULL)
        return;
    free(timers->v);
    (void)memset(timers, 0, sizeof(*timers));
}

TimerId sag_timer_add(TimerHeap *timers, i64 at_ms, TimerFn fn, void *ctx)
{
    SagTimer timer;
    size_t at;

    if (timers == NULL || fn == NULL)
        SAG_BUG("timer add requires a heap and callback");
    if (timers->next_id == SAG_TIMER_NONE || timers->next_order == UINT64_MAX)
        SAG_BUG("timer identity space exhausted");
    if (timers->len == timers->cap) {
        size_t cap = timers->cap == 0U ? 8U : timers->cap * 2U;

        if (cap < timers->cap || cap > SIZE_MAX / sizeof(*timers->v))
            SAG_BUG("timer heap capacity overflow");
        timers->v = sag_xrealloc(timers->v, cap * sizeof(*timers->v));
        timers->cap = cap;
    }
    timer.at_ms = at_ms;
    timer.order = timers->next_order++;
    timer.id = timers->next_id++;
    timer.fn = fn;
    timer.ctx = ctx;
    at = timers->len++;
    timers->v[at] = timer;
    timer_up(timers, at);
    return timer.id;
}

bool sag_timer_cancel(TimerHeap *timers, TimerId id)
{
    size_t i;

    if (timers == NULL || id == SAG_TIMER_NONE)
        return false;
    for (i = 0U; i < timers->len; i++) {
        if (timers->v[i].id == id) {
            timers->len--;
            if (i != timers->len) {
                timers->v[i] = timers->v[timers->len];
                if (i != 0U && timer_before(&timers->v[i],
                                            &timers->v[(i - 1U) / 2U]))
                    timer_up(timers, i);
                else
                    timer_down(timers, i);
            }
            return true;
        }
    }
    return false;
}

i64 sag_timers_deadline(const TimerHeap *timers, i64 now_ms)
{
    if (timers == NULL || timers->len == 0U)
        return -1;
    if (timers->v[0].at_ms <= now_ms)
        return 0;
    if (now_ms < 0 && timers->v[0].at_ms > INT64_MAX + now_ms)
        return INT64_MAX;
    return timers->v[0].at_ms - now_ms;
}

void sag_timers_fire(TimerHeap *timers, Ed *ed, i64 now_ms)
{
    while (timers != NULL && timers->len != 0U &&
           timers->v[0].at_ms <= now_ms) {
        SagTimer timer = timer_pop(timers);

        timer.fn(ed, timer.ctx);
    }
}

i64 sag_now_ms(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        SAG_BUG("clock_gettime(CLOCK_MONOTONIC) failed: %s",
                strerror(errno));
    if ((i64)ts.tv_sec > INT64_MAX / 1000)
        return INT64_MAX;
    return (i64)ts.tv_sec * 1000 + (i64)ts.tv_nsec / 1000000;
}

static i64 deadline_min(i64 a, i64 b)
{
    if (a < 0)
        return b;
    if (b < 0)
        return a;
    return a < b ? a : b;
}

static i64 absolute_deadline(i64 at_ms, i64 now_ms)
{
    if (at_ms < 0)
        return -1;
    if (at_ms <= now_ms)
        return 0;
    if (now_ms < 0 && at_ms > INT64_MAX + now_ms)
        return INT64_MAX;
    return at_ms - now_ms;
}

int sag_loop_deadline(const Ed *ed, i64 now_ms)
{
    i64 deadline;

    if (ed == NULL)
        return -1;
    if (ed->in.rd < ed->in.buf.len)
        return 0;
    deadline = absolute_deadline(sag_dispatch_deadline(ed), now_ms);
    deadline = deadline_min(deadline,
                            sag_input_deadline(&ed->in, now_ms));
    deadline = deadline_min(deadline,
                            sag_timers_deadline(&ed->timers, now_ms));
    /* Filter timeouts and SIGTERM->SIGKILL escalation are deadlines too:
     * without this the loop could sleep past a job's kill window. */
    deadline = deadline_min(deadline, sag_job_deadline(ed, now_ms));
    if (!sag_tty_probe_done(&ed->tty))
        deadline = deadline_min(deadline,
                                sag_tty_probe_deadline(&ed->tty, now_ms));
    if (deadline < 0)
        return -1;
    return deadline > INT_MAX ? INT_MAX : (int)deadline;
}

static void loop_seed_probe(Ed *ed)
{
    if (ed->probe_seeded || !sag_tty_probe_done(&ed->tty))
        return;
    ed->in.caps = ed->tty.caps;
    if (ed->input_ready)
        sag_input_enable(ed->tty.wfd, &ed->tty.caps);
    if (ed->render_ready)
        sag_render_init(&ed->render, &ed->tty.caps, loop_getenv);
    sag_input_seed(&ed->in, &ed->tty.pending);
    ed->tty.pending.len = 0U;
    ed->probe_seeded = true;
    ed->full_damage = true;
}

static int loop_read_input(Ed *ed)
{
    size_t drained = 0U;

    while (drained < SAG_INPUT_BURST_MAX) {
        u8 bytes[4096];
        size_t room = SAG_INPUT_BURST_MAX - drained;
        size_t want = room < sizeof(bytes) ? room : sizeof(bytes);
        ssize_t n = read(ed->tty.rfd, bytes, want);

        if (n > 0) {
            if (ed->probe_seeded)
                sag_input_feed(&ed->in, bytes, (size_t)n);
            else
                (void)sag_tty_probe_feed(&ed->tty, bytes, (size_t)n);
            drained += (size_t)n;
            loop_seed_probe(ed);
            continue;
        }
        /* Raw mode uses VMIN=0: zero means this drain is complete. */
        if (n == 0)
            return 0;
        if (errno == EINTR)
            continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return 0;
        return -1;
    }
    return 0;
}

static void loop_dispatch_event(Ed *ed, Key key, i64 now_ms)
{
    const u8 *bytes;
    size_t len;

    switch (key.kind) {
    case SAG_EV_KEY:
        sag_ed_handle_key(ed, key, now_ms);
        break;
    case SAG_EV_PASTE_BEGIN:
        sag_ed_handle_paste(ed, NULL, 0U, false);
        break;
    case SAG_EV_PASTE_DATA:
        bytes = sag_input_paste_chunk(&ed->in, &len);
        sag_ed_handle_paste(ed, bytes, len, false);
        break;
    case SAG_EV_PASTE_END:
        sag_ed_handle_paste(ed, NULL, 0U, true);
        break;
    default:
        break;
    }
}

int sag_loop_run(Ed *ed)
{
    if (ed == NULL)
        return SAG_EXIT_BUG;
    for (;;) {
        /* Two fixed slots (tty, signal pipe) plus up to four per job. */
        struct pollfd fds[2U + SAG_JOB_MAX * 4U];
        u32 nfds = 2U;
        i64 now = sag_now_ms();
        int result;
        bool winch = false;
        bool cont = false;
        bool chld = false;
        Key key;

        /* A completed startup probe may already own typed-ahead bytes. */
        loop_seed_probe(ed);
        fds[0].fd = ed->tty.rfd;
        fds[0].events = POLLIN;
        fds[0].revents = 0;
        fds[1].fd = sag_tty_signal_fd(&ed->tty);
        fds[1].events = POLLIN;
        fds[1].revents = 0;
        sag_job_collect_fds(ed, fds, &nfds);
        result = poll(fds, (nfds_t)nfds, sag_loop_deadline(ed, now));
        if (result < 0 && errno != EINTR)
            return SAG_EXIT_IO;
        if ((fds[0].revents & (POLLERR | POLLNVAL)) != 0 ||
            (fds[1].revents & (POLLERR | POLLHUP | POLLNVAL)) != 0)
            return SAG_EXIT_IO;
        now = sag_now_ms();
        if ((fds[0].revents & (POLLIN | POLLHUP)) != 0 &&
            loop_read_input(ed) != 0)
            return SAG_EXIT_IO;
        if ((fds[0].revents & POLLHUP) != 0) {
            sag_input_eof(&ed->in);
            ed->quit = true;
        }

        if ((fds[1].revents & POLLIN) != 0 || result < 0)
            sag_tty_drain_signals(&ed->tty, &winch, &cont, &chld);
        if (winch || cont)
            sag_ed_resize(ed, cont);

        /* Jobs are pumped before dispatch so a burst of output is one
         * render, and after input is drained so keystrokes always win the
         * race for this iteration (invariant 4). */
        sag_job_pump(ed, fds, nfds);
        if (chld)
            sag_job_reap(ed);
        sag_job_tick(ed, now);
        /* Completion is delivered here, not from reap: a job is done
         * when the child is gone AND its pipes have drained. */
        sag_job_settle(ed);
        if (ed->jobs.dirty) {
            /* One refresh per iteration, not one per state change: a
             * burst of exits redraws the table and badge exactly once. */
            ed->jobs.dirty = false;
            sag_jobs_table_refresh(ed);
            ed->footer_dirty = true;
        }

        sag_tty_probe_tick(&ed->tty, now);
        loop_seed_probe(ed);
        while (sag_input_next(&ed->in, now, &key))
            loop_dispatch_event(ed, key, now);

        /* Deadline work cannot split a queued typeahead burst. */
        sag_dispatch_tick(ed, now);
        sag_timers_fire(&ed->timers, ed, now);
        if (ed->quit)
            return ed->exit_code;
        if (ed->layout_dirty)
            sag_ed_layout(ed);
        sag_ed_render(ed);
        if (ed->quit)
            return ed->exit_code;
    }
}
