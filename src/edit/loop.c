#define _POSIX_C_SOURCE 200809L

#include "edit/loop.h"
#include "ui/picker.h"

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
#include "edit/theme_cmds.h"
#include "ui/mouse.h"
#include "edit/shell.h"
#include "fl/flruntime.h"
#include "mod/ai/ai.h"
#include "mod/ai/http.h"
#include "mod/git/fussmode.h"
#include "mod/git/git.h"
#include "mod/lsp/lsp.h"
#include "term/input.h"
#include "term/tty.h"
#include "util/log.h"
#include "ws/symidx.h"
#include "ws/symwalk.h"

enum {
    /* Background indexing must not race the next key after a paint.  The
     * grace period is short enough to be invisible at rest and long enough
     * for a continuing input burst to reach poll first. */
    YEW_BACKGROUND_IDLE_MS = 12
};

static const char *loop_getenv(const char *name)
{
    return getenv(name);
}

struct YewTimer {
    i64 at_ms;
    u64 order;
    TimerId id;
    TimerFn fn;
    void *ctx;
};

static bool timer_before(const YewTimer *a, const YewTimer *b)
{
    return a->at_ms < b->at_ms ||
           (a->at_ms == b->at_ms && a->order < b->order);
}

static void timer_swap(YewTimer *a, YewTimer *b)
{
    YewTimer tmp = *a;

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

static YewTimer timer_pop(TimerHeap *timers)
{
    YewTimer first = timers->v[0];

    timers->len--;
    if (timers->len != 0U) {
        timers->v[0] = timers->v[timers->len];
        timer_down(timers, 0U);
    }
    return first;
}

void yew_timers_init(TimerHeap *timers)
{
    if (timers == NULL)
        return;
    (void)memset(timers, 0, sizeof(*timers));
    timers->next_id = 1U;
}

void yew_timers_free(TimerHeap *timers)
{
    if (timers == NULL)
        return;
    free(timers->v);
    (void)memset(timers, 0, sizeof(*timers));
}

TimerId yew_timer_add(TimerHeap *timers, i64 at_ms, TimerFn fn, void *ctx)
{
    YewTimer timer;
    size_t at;

    if (timers == NULL || fn == NULL)
        YEW_BUG("timer add requires a heap and callback");
    if (timers->next_id == YEW_TIMER_NONE || timers->next_order == UINT64_MAX)
        YEW_BUG("timer identity space exhausted");
    if (timers->len == timers->cap) {
        size_t cap = timers->cap == 0U ? 8U : timers->cap * 2U;

        if (cap < timers->cap || cap > SIZE_MAX / sizeof(*timers->v))
            YEW_BUG("timer heap capacity overflow");
        timers->v = yew_xrealloc(timers->v, cap * sizeof(*timers->v));
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

bool yew_timer_cancel(TimerHeap *timers, TimerId id)
{
    size_t i;

    if (timers == NULL || id == YEW_TIMER_NONE)
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

i64 yew_timers_deadline(const TimerHeap *timers, i64 now_ms)
{
    if (timers == NULL || timers->len == 0U)
        return -1;
    if (timers->v[0].at_ms <= now_ms)
        return 0;
    if (now_ms < 0 && timers->v[0].at_ms > INT64_MAX + now_ms)
        return INT64_MAX;
    return timers->v[0].at_ms - now_ms;
}

void yew_timers_fire(TimerHeap *timers, Ed *ed, i64 now_ms)
{
    while (timers != NULL && timers->len != 0U &&
           timers->v[0].at_ms <= now_ms) {
        YewTimer timer = timer_pop(timers);

        timer.fn(ed, timer.ctx);
    }
}

i64 yew_now_ms(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        YEW_BUG("clock_gettime(CLOCK_MONOTONIC) failed: %s",
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

static i64 background_deadline(const Ed *ed, i64 now_ms)
{
    i64 ready_ms;

    if (!yew_symidx_pending(ed) &&
        !(ed->ws.sym_walk.running && ed->ws.sym_walk.job == 0U))
        return -1;
    if (ed->fl_idle_since_ms < 0)
        return 0;
    ready_ms = ed->fl_idle_since_ms;
    if (ready_ms <= INT64_MAX - YEW_BACKGROUND_IDLE_MS)
        ready_ms += YEW_BACKGROUND_IDLE_MS;
    else
        ready_ms = INT64_MAX;
    return absolute_deadline(ready_ms, now_ms);
}

int yew_loop_deadline(const Ed *ed, i64 now_ms)
{
    i64 deadline;

    if (ed == NULL)
        return -1;
    if (ed->in.rd < ed->in.buf.len)
        return 0;
    /*
     * Sprint 26 §7.2: a sliced rescan is work waiting to be done, so
     * the loop must not sleep on poll while one is in flight.  Without
     * this the list would stop filling in and only resume when the user
     * happened to press another key.
     */
    if (yew_picker_scanning(ed))
        return 0;
    /* The sliced completion scan is the same kind of pending work, and
     * omitting it here is the same bug: the menu would stop filling in
     * and only resume when the user happened to press another key.  The
     * predicate is shared with the tick deliberately — see its comment
     * for what a mismatched pair does. */
    if (yew_cmdline_comp_scanning(ed))
        return 0;
    /* Syntax propagation is sliced on a 16 ms idle cadence.  It is work,
     * but unlike picker scans it must not turn an idle editor into a busy
     * loop while a million-line wave is settling. */
    if (yew_ed_syn_pending(ed))
        deadline = 16;
    else
        deadline = -1;
    deadline = deadline_min(deadline, background_deadline(ed, now_ms));
    deadline = deadline_min(
        deadline, absolute_deadline(yew_dispatch_deadline(ed), now_ms));
    deadline = deadline_min(deadline,
                            yew_input_deadline(&ed->in, now_ms));
    deadline = deadline_min(deadline,
                            yew_timers_deadline(&ed->timers, now_ms));
    /* Filter timeouts and SIGTERM->SIGKILL escalation are deadlines too:
     * without this the loop could sleep past a job's kill window. */
    deadline = deadline_min(deadline, yew_job_deadline(ed, now_ms));
    deadline = deadline_min(deadline, yew_ai_deadline(ed, now_ms));
    deadline = deadline_min(deadline, yew_fuss_deadline(ed, now_ms));
    /* Sprint 27 §4: the dwell and the drag auto-scroll are CLOCKS.  A
     * pointer resting on a group emits no further events, so without
     * this the loop would sleep through the 400 ms the dwell is
     * counting. */
    deadline = deadline_min(deadline, yew_mouse_deadline(ed, now_ms));
    if (ed->fl != NULL && !ed->fl_idle_fired &&
        ed->fl_idle_since_ms >= 0)
        deadline = deadline_min(
            deadline,
            absolute_deadline(ed->fl_idle_since_ms + 500, now_ms));
    if (!yew_tty_probe_done(&ed->tty))
        deadline = deadline_min(deadline,
                                yew_tty_probe_deadline(&ed->tty, now_ms));
    if (deadline < 0)
        return -1;
    return deadline > INT_MAX ? INT_MAX : (int)deadline;
}

static void loop_seed_probe(Ed *ed)
{
    if (ed->probe_seeded || !yew_tty_probe_done(&ed->tty))
        return;
    ed->in.caps = ed->tty.caps;
    if (ed->input_ready)
        yew_input_enable(ed->tty.wfd, &ed->tty.caps);
    if (ed->render_ready) {
        yew_render_init(&ed->render, &ed->tty.caps, loop_getenv);
        yew_theme_sync_surfaces(ed);
    }
    yew_input_seed(&ed->in, &ed->tty.pending);
    ed->tty.pending.len = 0U;
    ed->probe_seeded = true;
    ed->full_damage = true;
}

static int loop_read_input(Ed *ed)
{
    size_t drained = 0U;

    while (drained < YEW_INPUT_BURST_MAX) {
        u8 bytes[4096];
        size_t room = YEW_INPUT_BURST_MAX - drained;
        size_t want = room < sizeof(bytes) ? room : sizeof(bytes);
        ssize_t n = read(ed->tty.rfd, bytes, want);

        if (n > 0) {
            if (ed->probe_seeded)
                yew_input_feed(&ed->in, bytes, (size_t)n);
            else
                (void)yew_tty_probe_feed(&ed->tty, bytes, (size_t)n);
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

    /* Every event carries the loop's clock in, because nothing in the
     * core reads one itself (invariant 5) — and Sprint 27's dwell is
     * driven from mouse motion as much as from the timer path. */
    ed->now_ms = now_ms;
    switch (key.kind) {
    case YEW_EV_KEY:
        yew_ed_handle_key(ed, key, now_ms);
        break;
    case YEW_EV_PASTE_BEGIN:
        yew_ed_handle_paste(ed, NULL, 0U, false);
        break;
    case YEW_EV_PASTE_DATA:
        bytes = yew_input_paste_chunk(&ed->in, &len);
        yew_ed_handle_paste(ed, bytes, len, false);
        break;
    case YEW_EV_PASTE_END:
        yew_ed_handle_paste(ed, NULL, 0U, true);
        break;
    case YEW_EV_MOUSE:
        yew_mouse_event(ed, &key);
        break;
    case YEW_EV_FOCUS:
        /*
         * Sprint 27 §1: focus-out cancels any gesture.  A drag whose
         * release lands in another window never reports that release to
         * us, so without this the router would sit with a button
         * logically down forever and the next click would be read as
         * the drop of a gesture the user abandoned minutes ago.
         */
        if (key.code == YEW_KEY_FOCUS_OUT)
            yew_mouse_cancel(ed);
        else if (key.code == YEW_KEY_FOCUS_IN) {
            yew_git_invalidate(ed);
            (void)yew_git_refresh(ed, true);
        }
        break;
    default:
        break;
    }
}

int yew_loop_run(Ed *ed)
{
    if (ed == NULL)
        return YEW_EXIT_BUG;
    for (;;) {
        /* Two fixed slots (tty, signal pipe) plus up to four per job. */
        struct pollfd fds[2U + YEW_JOB_MAX * 4U + YEW_HTTP_POOL_MAX];
        u32 nfds = 2U;
        i64 now = yew_now_ms();
        int result;
        bool winch = false;
        bool cont = false;
        bool chld = false;
        bool had_input = false;
        Win *burst_win;
        u64 burst_cursors;
        Key key;

        if (ed->fl_idle_since_ms < 0)
            ed->fl_idle_since_ms = now;
        burst_win = ed->win;
        burst_cursors = yew_fl_cursor_burst_state(burst_win);

        /* A completed startup probe may already own typed-ahead bytes. */
        loop_seed_probe(ed);
        fds[0].fd = ed->tty.rfd;
        fds[0].events = POLLIN;
        fds[0].revents = 0;
        fds[1].fd = yew_tty_signal_fd(&ed->tty);
        fds[1].events = POLLIN;
        fds[1].revents = 0;
        yew_job_collect_fds(ed, fds, &nfds);
        yew_ai_collect_fds(ed, fds, &nfds);
        result = poll(fds, (nfds_t)nfds, yew_loop_deadline(ed, now));
        if (result < 0 && errno != EINTR)
            return YEW_EXIT_IO;
        if ((fds[0].revents & (POLLERR | POLLNVAL)) != 0 ||
            (fds[1].revents & (POLLERR | POLLHUP | POLLNVAL)) != 0)
            return YEW_EXIT_IO;
        now = yew_now_ms();
        ed->now_ms = now;
        if ((fds[0].revents & (POLLIN | POLLHUP)) != 0 &&
            loop_read_input(ed) != 0)
            return YEW_EXIT_IO;
        if ((fds[0].revents & POLLHUP) != 0) {
            yew_input_eof(&ed->in);
            ed->quit = true;
        }

        if ((fds[1].revents & POLLIN) != 0 || result < 0)
            yew_tty_drain_signals(&ed->tty, &winch, &cont, &chld);
        if (winch || cont)
            yew_ed_resize(ed, cont);

        /* Jobs are pumped before dispatch so a burst of output is one
         * render, and after input is drained so keystrokes always win the
         * race for this iteration (invariant 4). */
        yew_job_pump(ed, fds, nfds);
        if (chld) {
            bool was_reaped[YEW_JOB_MAX];
            bool external_completion = false;
            u32 before = ed->jobs.len;
            u32 i;

            for (i = 0U; i < before; i++)
                was_reaped[i] = ed->jobs.v[i].reaped;
            yew_job_reap(ed);
            for (i = 0U; i < before && i < ed->jobs.len; i++) {
                if (!was_reaped[i] && ed->jobs.v[i].reaped &&
                    !yew_git_job_owned(ed, ed->jobs.v[i].id))
                    external_completion = true;
            }
            if (external_completion) {
                yew_git_invalidate(ed);
                (void)yew_git_refresh(ed, true);
            }
        }
        yew_job_tick(ed, now);
        /* Completion is delivered here, not from reap: a job is done
         * when the child is gone AND its pipes have drained. */
        yew_job_settle(ed);
        /* TTL polling is an event-loop responsibility.  Rendering only
         * consumes the last published snapshot and therefore cannot fork. */
#if YEW_WITH_FUSS
        (void)yew_git_refresh(ed, false);
#endif
        yew_fuss_tick(ed, now);
        yew_ai_pump(ed, fds, nfds);
        yew_lsp_pump(ed);
        if (ed->jobs.dirty) {
            /* One refresh per iteration, not one per state change: a
             * burst of exits redraws the table and badge exactly once. */
            ed->jobs.dirty = false;
            yew_jobs_table_refresh(ed);
            ed->footer_dirty = true;
        }

        yew_tty_probe_tick(&ed->tty, now);
        loop_seed_probe(ed);
        while (yew_input_next(&ed->in, now, &key)) {
            had_input = true;
            loop_dispatch_event(ed, key, now);
        }
        if (had_input) {
            ed->fl_idle_since_ms = now;
            ed->fl_idle_fired = false;
        }

        /* Deadline work cannot split a queued typeahead burst. */
        yew_dispatch_tick(ed, now);
        yew_lsp_highlight_cursor(ed, ed->win);
        yew_timers_fire(&ed->timers, ed, now);
        /*
         * Sprint 26 §7.2: a sliced rescan continues here, on the idle
         * path, AFTER input has been drained — so a keystroke always
         * wins the race for this iteration (invariant 4) and the list
         * fills in behind it.
         */
        (void)yew_picker_tick(ed);
        /* The completion scan is sliced for the same reason and drains
         * in the same place. */
        (void)yew_cmdline_comp_tick(ed);
        /* Symbol indexing is stale-safe, so preserve input-to-paint latency
         * by waiting for a short idle window.  The deadline above wakes the
         * loop even when no further event arrives. */
        if (!had_input &&
            (ed->fl_idle_since_ms < 0 ||
             now - ed->fl_idle_since_ms >= YEW_BACKGROUND_IDLE_MS)) {
            yew_symidx_pump(ed, YEW_SYMIDX_FULL_US);
            yew_symwalk_pump(ed, YEW_SYMWALK_BUDGET_US);
        }
        yew_mouse_tick(ed, now);
        /* Coalesced events run after input and deadline work, never inside
         * the keypress path.  A paste therefore produces one callback. */
        yew_fl_hook_flush_change(ed);
        yew_fl_hook_flush_cursor(ed, burst_win, burst_cursors);
        if (!ed->fl_idle_fired && ed->fl_idle_since_ms >= 0 &&
            now - ed->fl_idle_since_ms >= 500) {
            yew_fl_hook_fire(ed, FL_EV_ED_IDLE, NULL, 0U);
            ed->fl_idle_fired = true;
        }
        if (yew_ed_syn_pending(ed))
            yew_ed_syn_tick(ed, had_input ? YEW_SYN_FRAME_BUDGET_US :
                                           YEW_SYN_IDLE_BUDGET_US,
                            had_input);
        if (ed->quit)
            return ed->exit_code;
        if (ed->layout_dirty)
            yew_ed_layout(ed);
        yew_ed_render(ed);
        if (ed->quit)
            return ed->exit_code;
    }
}
