/* Sprint 19: the three consumption modes and the *jobs* table. */
#define _POSIX_C_SOURCE 200809L

#include "edit/shell.h"

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "edit/ed.h"
#include "edit/loop.h"
#include "term/input.h"
#include "term/tty.h"
#include "text/piece.h"
#include "ui/message.h"
#include "ui/viewport.h"
#include "util/log.h"

#define YEW_JOBS_TABLE_NAME "*jobs*"

/* Filter poll tick: bounds how long the restricted loop sleeps before
 * checking for child exit.  See filter_drive. */
#define YEW_FILTER_TICK_MS 20

/* Our own table rather than strsignal(): its text is locale-dependent and
 * would leak into pty goldens, breaking deterministic rendering. */
static const char *yew_signame(int sig)
{
    switch (sig) {
    case SIGHUP:  return "SIGHUP";
    case SIGINT:  return "SIGINT";
    case SIGQUIT: return "SIGQUIT";
    case SIGILL:  return "SIGILL";
    case SIGABRT: return "SIGABRT";
    case SIGFPE:  return "SIGFPE";
    case SIGKILL: return "SIGKILL";
    case SIGSEGV: return "SIGSEGV";
    case SIGPIPE: return "SIGPIPE";
    case SIGALRM: return "SIGALRM";
    case SIGTERM: return "SIGTERM";
    case SIGUSR1: return "SIGUSR1";
    case SIGUSR2: return "SIGUSR2";
    default:      break;
    }
    return "SIG?";
}

/* ------------------------------------------------------------------ */
/* Shared helpers                                                     */
/* ------------------------------------------------------------------ */

/*
 * Elapsed time is the one nondeterministic thing a job puts on screen, and
 * invariant 5 requires the pty harness to byte-compare grids exactly.
 * YEW_JOB_ELAPSED_MS pins the displayed value so goldens stay exact
 * instead of fuzzy-matched — the same test-determinism hook pattern as
 * YEW_CHORD_TIMEOUT_MS.  Unset in normal use, where real timings show.
 */
static i64 elapsed_override(void)
{
    static bool looked_up;
    static i64 fixed = -1;

    if (!looked_up) {
        const char *value = getenv("YEW_JOB_ELAPSED_MS");

        looked_up = true;
        if (value != NULL && value[0] != '\0') {
            char *end = NULL;
            long parsed = strtol(value, &end, 10);

            if (end != NULL && *end == '\0' && parsed >= 0)
                fixed = (i64)parsed;
        }
    }
    return fixed;
}

static void fmt_elapsed(char *out, size_t n, i64 ms)
{
    i64 fixed = elapsed_override();

    if (fixed >= 0)
        ms = fixed;
    (void)snprintf(out, n, "%.2fs", (double)ms / 1000.0);
}

/* Appends straight through the edit chokepoint so marks and cursors track,
 * but outside any undo transaction: a job buffer is YEW_BUF_NOUNDO. */
static EditCtx buf_raw_edit_ctx(Ed *ed, Buffer *b)
{
    EditCtx ec = {0};
    Win *w = ed->win != NULL && ed->win->buf == b ? ed->win : NULL;

    ec.tb = b->tb;
    ec.marks = b->marks;
    ec.cset = w != NULL ? &w->cs : NULL;
    ec.undo = NULL; /* YEW_BUF_NOUNDO: no ops recorded, nothing journaled */
    ec.jrnl = NULL;
    /* meta stays NULL: the edit chokepoint treats a non-NULL meta as
     * file-backed and demands a journal path, which a scratch buffer has
     * no business owning — there is no file here to protect. */
    ec.meta = NULL;
    ec.now_ms = ed->now_ms;
    ec.ed = ed;
    ec.buffer = b;
    return ec;
}

static void buf_append_raw(Ed *ed, Buffer *b, const u8 *bytes, u64 len)
{
    EditCtx ec;

    if (b == NULL || b->tb == NULL || len == 0U)
        return;
    ec = buf_raw_edit_ctx(ed, b);
    (void)yew_edit_insert(&ec, BYTEOFF(yew_textbuf_len(b->tb)), bytes, len);
}

static void buf_append_str(Ed *ed, Buffer *b, const char *s)
{
    buf_append_raw(ed, b, (const u8 *)s, (u64)strlen(s));
}

/* ------------------------------------------------------------------ */
/* Mode (a): streaming into a job buffer (§4)                         */
/* ------------------------------------------------------------------ */

void yew_job_buffer_append(Ed *ed, YewJob *j, const u8 *bytes, u64 len,
                           bool is_err)
{
    bool at_end = false;
    Win *w;

    (void)is_err; /* stderr attr spans arrive with the theme in Sprint 41 */
    if (ed == NULL || j == NULL || j->buf == NULL || len == 0U)
        return;
    w = ed->win != NULL && ed->win->buf == j->buf ? ed->win : NULL;
    if (w != NULL && w->cs.curs.len != 0U) {
        const Cursor *cur = &w->cs.curs.data[w->cs.primary];

        /* Follow-tail moves the cursor only when it was already at the end
         * and nothing is selected.  Yanking the view to the bottom while
         * someone reads the middle is the classic complaint. */
        at_end = cur->pos.v >= yew_textbuf_len(j->buf->tb) &&
                 w->cs.active == YEW_MC_ACTIVE_NONE;
    }
    buf_append_raw(ed, j->buf, bytes, len);
    if (at_end && j->follow_tail && w != NULL) {
        Cursor *cur = &w->cs.curs.data[w->cs.primary];

        cur->pos = BYTEOFF(yew_textbuf_len(j->buf->tb));
        cur->anchor = cur->pos;
        ed->cursor_follow_pending = true;
    }
    if (w != NULL)
        yew_ed_damage_document(ed);
}

static void job_footer(Ed *ed, YewJob *j, char *out, size_t n)
{
    char elapsed[32];

    (void)ed;

    fmt_elapsed(elapsed, sizeof(elapsed), j->end_ms - j->start_ms);
    switch (j->state) {
    case YEW_JOB_EXECFAIL:
        (void)snprintf(out, n, "[cannot run %s: %s]", j->label,
                       strerror(j->exec_errno));
        break;
    case YEW_JOB_SIGNALED:
        (void)snprintf(out, n, "[killed by %s after %s]",
                       yew_signame(j->termsig), elapsed);
        break;
    case YEW_JOB_TIMEOUT:
        (void)snprintf(out, n, "[timed out after %s]", elapsed);
        break;
    case YEW_JOB_CANCELLED:
        (void)snprintf(out, n, "[cancelled after %s]", elapsed);
        break;
    case YEW_JOB_EXITED:
    default:
        (void)snprintf(out, n, "[exit %d in %s]", j->exit_code, elapsed);
        break;
    }
}

/* Mode (c): the collected output lands in one transaction at the mark. */
static void job_insert_collected(Ed *ed, YewJob *j)
{
    EditCtx ec;
    Buffer *b;
    ByteOff at;
    bool needs_nl;

    if (ed->win == NULL || ed->win->buf == NULL)
        return;
    b = ed->win->buf;
    if (j->state != YEW_JOB_EXITED || j->exit_code != 0) {
        char foot[256];

        job_footer(ed, j, foot, sizeof(foot));
        yew_msg(ed, YEW_MSG_WARN, "%s %s", j->label, foot);
        return;
    }
    if (j->collect.len == 0U) {
        yew_msg(ed, YEW_MSG_INFO, "%s — no output", j->label);
        return;
    }
    at = BYTEOFF(yew_mark_pos(b->marks, j->at).v);
    ec = yew_ed_edit_ctx(ed);
    yew_undo_begin(&ec, YEW_TXN_EXTERNAL);
    (void)yew_edit_insert(&ec, at, j->collect.data, (u64)j->collect.len);
    /* A trailing newline iff we inserted at a line start and the output
     * lacks one, so `:r !date` on an empty line does not glue the next
     * line onto it. */
    needs_nl = j->collect.data[j->collect.len - 1U] != (u8)'\n' &&
               (at.v == 0U || at.v == yew_textbuf_line_start(
                                          b->tb,
                                          yew_textbuf_line_of(b->tb, at)).v);
    if (needs_nl) {
        ByteOff end = BYTEOFF(at.v + (u64)j->collect.len);

        (void)yew_edit_insert(&ec, end, (const u8 *)"\n", 1U);
    }
    yew_undo_end(&ec);
    yew_ed_finish_edit(ed, &ec);
    yew_mark_del(b->marks, j->at);
    j->has_mark = false;
    yew_ed_damage_document(ed);
    yew_msg(ed, YEW_MSG_INFO, "%s — %llu bytes read", j->label,
            (unsigned long long)j->collect.len);
}

void yew_job_finish(Ed *ed, YewJob *j)
{
    char foot[256];

    if (ed == NULL || j == NULL)
        return;
    /* The filter drives its own job and owns the outcome, rollback
     * included; touching buffers here would fight it. */
    if (j->synchronous)
        return;
    if (j->sink == YEW_SINK_COLLECT && j->has_mark) {
        job_insert_collected(ed, j);
        return;
    }
    if (j->buf == NULL)
        return;
    job_footer(ed, j, foot, sizeof(foot));
    /* "Command produced no output": a full-screen empty buffer after every
     * :!make is the most-hated behavior of every editor that skips this. */
    if (j->bytes_out + j->bytes_err == 0U) {
        if (ed->win != NULL && ed->win->buf == j->buf)
            (void)yew_ed_show_buffer(ed, &ed->buffer);
        yew_msg(ed, YEW_MSG_INFO, ":!%s — no output (exit %d)",
                j->label, j->exit_code);
        yew_ws_scratch_drop(ed, j->buf);
        j->buf = NULL;
        return;
    }
    if (yew_textbuf_len(j->buf->tb) != 0U) {
        u64 end = yew_textbuf_len(j->buf->tb);
        u8 last = 0U;
        TextIter it;
        const u8 *chunk = NULL;
        size_t chunk_len = 0U;

        if (yew_textiter_begin(&it, j->buf->tb, BYTEOFF(end - 1U)) &&
            yew_textiter_chunk(&it, j->buf->tb, &chunk, &chunk_len) &&
            chunk_len != 0U)
            last = chunk[0];
        if (last != (u8)'\n')
            buf_append_str(ed, j->buf, "\n");
    }
    buf_append_str(ed, j->buf, foot);
    buf_append_str(ed, j->buf, "\n");
    yew_msg(ed, j->state == YEW_JOB_EXITED && j->exit_code == 0 ?
                    YEW_MSG_INFO : YEW_MSG_WARN,
            ":!%s %s", j->label, foot);
    yew_ed_damage_document(ed);
}

u32 yew_shell_run(Ed *ed, const char *cmdline, bool focus, char *err,
                  size_t errsz)
{
    YewJobSpec spec = {0};
    char name[192];
    u32 id;
    YewJob *j;
    Buffer *buf;

    if (ed == NULL || cmdline == NULL)
        return 0U;
    spec.cmdline = cmdline;
    spec.sink = YEW_SINK_BUFFER;
    id = yew_job_spawn(ed, &spec, err, errsz);
    if (id == 0U)
        return 0U;
    j = yew_job_find(ed, id);
    (void)snprintf(name, sizeof(name), "*job:%u %s*", (unsigned)id,
                   cmdline);
    buf = yew_ws_scratch_new(ed, name, YEW_BUF_NOUNDO);
    if (buf == NULL) {
        (void)yew_job_signal(ed, id, SIGTERM);
        (void)snprintf(err, errsz, "cannot create job buffer");
        return 0U;
    }
    j->buf = buf;
    if (focus)
        (void)yew_ed_show_buffer(ed, buf);
    return id;
}

/* ------------------------------------------------------------------ */
/* Mode (c): read output at the cursor (§6)                           */
/* ------------------------------------------------------------------ */

u32 yew_shell_read(Ed *ed, const char *cmdline, char *err, size_t errsz)
{
    YewJobSpec spec = {0};
    u32 id;
    YewJob *j;
    Buffer *b;
    const Cursor *cur;

    if (ed == NULL || cmdline == NULL || ed->win == NULL ||
        ed->win->buf == NULL || ed->win->cs.curs.len == 0U)
        return 0U;
    b = ed->win->buf;
    spec.cmdline = cmdline;
    spec.sink = YEW_SINK_COLLECT;
    id = yew_job_spawn(ed, &spec, err, errsz);
    if (id == 0U)
        return 0U;
    j = yew_job_find(ed, id);
    cur = &ed->win->cs.curs.data[ed->win->cs.primary];
    /* RIGHT bias so text typed at the insertion point stays before the
     * arriving output, and so editing elsewhere keeps the point correct. */
    j->at = yew_mark_add(b->marks, cur->pos, YEW_BIAS_RIGHT);
    j->has_mark = true;
    return id;
}

/* ------------------------------------------------------------------ */
/* Mode (b): the synchronous region filter (§5)                       */
/* ------------------------------------------------------------------ */

/* The filter's restricted loop.  NOT a nested event loop: terminal input
 * is buffered as typeahead and replayed afterwards, never dispatched —
 * dispatching would re-enter the editor with a half-applied transaction. */
static YewFilterResult filter_drive(Ed *ed, YewJob *j, Bytebuf *typeahead)
{
    for (;;) {
        struct pollfd pfd[8];
        u32 n = 0U;
        int tty_slot;
        int sig_slot;
        i64 now = yew_now_ms();
        i64 deadline = yew_job_deadline(ed, now);
        int rc;
        bool winch = false;
        bool cont = false;
        bool chld = false;

        /*
         * Exit only when the child is reaped AND its output has reached
         * EOF.  Breaking on reap alone silently truncated large filter
         * output — `cat` over 3.7 MB lost a few hundred KB, and the
         * filter still reported success, which is data loss wearing a
         * success message.  The timeout path still forces termination,
         * so a stuck writer cannot hang this loop.
         */
        if (!yew_job_pending(j))
            break;
        yew_job_collect_fds(ed, pfd, &n);
        tty_slot = -1;
        if (ed->tty_ready) {
            pfd[n].fd = ed->tty.rfd;
            pfd[n].events = POLLIN;
            pfd[n].revents = 0;
            tty_slot = (int)n++;
        }
        sig_slot = -1;
        if (ed->tty_ready) {
            pfd[n].fd = yew_tty_signal_fd(&ed->tty);
            pfd[n].events = POLLIN;
            pfd[n].revents = 0;
            sig_slot = (int)n++;
        }
        /* Cap the wait: once the child's pipes hit EOF there is nothing
         * left in the poll set, and without a tty there is no SIGCHLD
         * pipe here either — sleeping the full timeout would report a
         * finished `sort` as YEW_FILT_TIMEOUT.  A short tick keeps exit
         * detection prompt; this loop is synchronous, so it costs
         * nothing the user can perceive. */
        if (deadline < 0 || deadline > YEW_FILTER_TICK_MS)
            deadline = YEW_FILTER_TICK_MS;
        rc = poll(pfd, (nfds_t)n, (int)deadline);
        if (rc < 0 && errno != EINTR)
            return YEW_FILT_SPAWN;
        now = yew_now_ms();
        yew_job_pump(ed, pfd, n);
        if (sig_slot >= 0 && (pfd[sig_slot].revents & POLLIN) != 0) {
            yew_tty_drain_signals(&ed->tty, &winch, &cont, &chld);
            if (chld)
                yew_job_reap(ed);
            /* SIGWINCH is honored — resize and repaint — and the filter
             * keeps running.  A resize is not a reason to lose the run. */
            if (winch || cont)
                yew_ed_resize(ed, cont);
        }
        if (tty_slot >= 0 && (pfd[tty_slot].revents & POLLIN) != 0) {
            u8 bytes[1024];
            ssize_t got = read(ed->tty.rfd, bytes, sizeof(bytes));
            ssize_t i;

            for (i = 0; i < got; i++) {
                /* The only keys inspected are cancel keys, and they are
                 * consumed rather than replayed. */
                if (bytes[i] == 0x03U || bytes[i] == 0x1BU) {
                    (void)yew_job_signal(ed, j->id, SIGTERM);
                    j->state = YEW_JOB_CANCELLED;
                    continue;
                }
                bytebuf_push_u8(typeahead, bytes[i]);
            }
        }
        /* Reap before tick: a child that exits right as the deadline
         * lands must be recorded as EXITED, not lose the race and be
         * reported as a timeout it actually beat. */
        yew_job_reap(ed);
        yew_job_tick(ed, now);
    }
    switch (j->state) {
    case YEW_JOB_TIMEOUT:
        return YEW_FILT_TIMEOUT;
    case YEW_JOB_CANCELLED:
        return YEW_FILT_CANCELLED;
    case YEW_JOB_EXECFAIL:
        return YEW_FILT_SPAWN;
    case YEW_JOB_SIGNALED:
        return YEW_FILT_CANCELLED;
    case YEW_JOB_EXITED:
    default:
        break;
    }
    return j->exit_code == 0 ? YEW_FILT_OK : YEW_FILT_NONZERO;
}

YewFilterResult yew_shell_filter(Ed *ed, Win *w, Span region,
                                 const char *cmdline, Bytebuf *stderr_out)
{
    YewJobSpec spec = {0};
    Bytebuf typeahead;
    YewFilterResult result;
    u32 id;
    YewJob *j;
    EditCtx ec;
    u64 before_lines;
    u64 after_lines;
    char err[256];
    bool own_transaction;

    if (ed == NULL || w == NULL || w->buf == NULL || cmdline == NULL)
        return YEW_FILT_SPAWN;
    spec.cmdline = cmdline;
    spec.sink = YEW_SINK_COLLECT;
    spec.in_buf = w->buf->tb;
    spec.in_span = region;
    spec.timeout_ms = YEW_FILTER_TIMEOUT_MS;
    id = yew_job_spawn(ed, &spec, err, sizeof(err));
    if (id == 0U) {
        yew_msg(ed, YEW_MSG_ERROR, "%s", err);
        return YEW_FILT_SPAWN;
    }
    j = yew_job_find(ed, id);
    j->synchronous = true;
    bytebuf_init(&typeahead);
    result = filter_drive(ed, j, &typeahead);

    if (result == YEW_FILT_SPAWN && j->state == YEW_JOB_EXECFAIL)
        yew_msg(ed, YEW_MSG_ERROR, "cannot run %s: %s", cmdline,
                strerror(j->exec_errno));
    if (stderr_out != NULL && j->bytes_err != 0U)
        bytebuf_append(stderr_out, j->collect.data, j->collect.len);

    before_lines = yew_textbuf_line_count(w->buf->tb);
    if (result == YEW_FILT_OK) {
        ec = yew_ed_edit_ctx_for(ed, w);
        /* One transaction wrapping delete+insert, so a single undo
         * restores the original text exactly and the journal sees one
         * commit: a crash mid-filter recovers to pre- or post-, never
         * half. */
        own_transaction = ec.undo->depth == 0U;
        if (own_transaction)
            yew_undo_begin(&ec, YEW_TXN_FILTER);
        (void)yew_edit_delete(&ec, region);
        if (j->collect.len != 0U)
            (void)yew_edit_insert(&ec, BYTEOFF(region.lo), j->collect.data,
                                  (u64)j->collect.len);
        if (own_transaction)
            yew_undo_end(&ec);
        yew_ed_finish_edit(ed, &ec);
        after_lines = yew_textbuf_line_count(w->buf->tb);
        if (j->collect.len == 0U)
            /* Not rolled back — grep legitimately matches nothing — but
             * announced, because a silently vanished selection reads as
             * data loss. */
            yew_msg(ed, YEW_MSG_WARN,
                    "filter produced no output; region deleted (undo restores)");
        else
            yew_msg(ed, YEW_MSG_INFO, "filter: %llu → %llu lines",
                    (unsigned long long)before_lines,
                    (unsigned long long)after_lines);
        yew_ed_damage_document(ed);
    } else if (result == YEW_FILT_NONZERO) {
        yew_msg(ed, YEW_MSG_WARN, "filter: exit %d; buffer unchanged",
                j->exit_code);
    } else if (result == YEW_FILT_TIMEOUT) {
        yew_msg(ed, YEW_MSG_WARN, "filter timed out after %.1fs",
                (double)YEW_FILTER_TIMEOUT_MS / 1000.0);
    } else if (result == YEW_FILT_CANCELLED) {
        yew_msg(ed, YEW_MSG_WARN, "filter cancelled; buffer unchanged");
    }

    /* Typed-ahead bytes were never dispatched; replay them now. */
    if (typeahead.len != 0U && ed->probe_seeded)
        yew_input_feed(&ed->in, typeahead.data, typeahead.len);
    bytebuf_free(&typeahead);
    return result;
}

/* ------------------------------------------------------------------ */
/* The *jobs* table (§8)                                              */
/* ------------------------------------------------------------------ */

static void jobs_table_render(Ed *ed, Buffer *b)
{
    Bytebuf out;
    EditCtx ec;
    u64 old_len;
    u32 i;

    bytebuf_init(&out);
    bytebuf_printf(&out, " id  state     code  elapsed    bytes  command\n");
    /* Newest first: the job you just started is the one you are looking
     * for. */
    for (i = ed->jobs.len; i-- > 0U;) {
        const YewJob *j = &ed->jobs.v[i];
        char elapsed[32];
        char code[16];
        i64 end = j->state == YEW_JOB_RUNNING ? yew_now_ms() : j->end_ms;

        if (j->internal)
            continue;

        fmt_elapsed(elapsed, sizeof(elapsed), end - j->start_ms);
        if (j->state == YEW_JOB_RUNNING)
            (void)snprintf(code, sizeof(code), "-");
        else if (j->state == YEW_JOB_SIGNALED)
            (void)snprintf(code, sizeof(code), "%s",
                           yew_signame(j->termsig));
        else
            (void)snprintf(code, sizeof(code), "%d", j->exit_code);
        bytebuf_printf(&out, "%3u  %-9s %4s  %7s  %7llu  %s\n",
                       (unsigned)j->id, yew_job_state_name(j->state), code,
                       elapsed,
                       (unsigned long long)(j->bytes_out + j->bytes_err),
                       j->label);
    }
    /* Replace wholesale through the edit chokepoint.  The table is derived
     * state, but its syntax cache, symbol index, marks, and visible cursor
     * still have to observe the replacement. */
    ec = buf_raw_edit_ctx(ed, b);
    old_len = yew_textbuf_len(b->tb);
    if (old_len != 0U)
        (void)yew_edit_delete(&ec, (Span){0U, old_len});
    if (out.len != 0U)
        (void)yew_edit_insert(&ec, BYTEOFF(0U), out.data, (u64)out.len);
    bytebuf_free(&out);
}

void yew_jobs_table_refresh(Ed *ed)
{
    Buffer *b;

    if (ed == NULL)
        return;
    b = yew_ws_scratch_find(ed, YEW_JOBS_TABLE_NAME);
    if (b == NULL)
        return;
    jobs_table_render(ed, b);
    if (ed->win != NULL && ed->win->buf == b) {
        yew_vp_clamp(ed->win);
        yew_ed_damage_document(ed);
    }
}

Buffer *yew_jobs_table_open(Ed *ed)
{
    Buffer *b;

    if (ed == NULL)
        return NULL;
    b = yew_ws_scratch_find(ed, YEW_JOBS_TABLE_NAME);
    if (b == NULL)
        b = yew_ws_scratch_new(ed, YEW_JOBS_TABLE_NAME, YEW_BUF_NOUNDO);
    if (b == NULL)
        return NULL;
    jobs_table_render(ed, b);
    (void)yew_ed_show_buffer(ed, b);
    return b;
}

u32 yew_jobs_table_row_id(Ed *ed)
{
    Buffer *b;
    LineNo line;
    Span span;
    TextIter it;
    const u8 *chunk = NULL;
    size_t len = 0U;
    unsigned id = 0U;
    char row[32];
    size_t n = 0U;

    if (ed == NULL || ed->win == NULL)
        return 0U;
    b = ed->win->buf;
    if (b == NULL || b->name == NULL ||
        strcmp(b->name, YEW_JOBS_TABLE_NAME) != 0 ||
        ed->win->cs.curs.len == 0U)
        return 0U;
    line = yew_textbuf_line_of(b->tb,
                               ed->win->cs.curs.data[ed->win->cs.primary].pos);
    if (line.v == 0U)
        return 0U; /* the header row addresses no job */
    span = yew_textbuf_line_span(b->tb, line);
    if (!yew_textiter_begin(&it, b->tb, BYTEOFF(span.lo)))
        return 0U;
    while (n + 1U < sizeof(row) &&
           yew_textiter_chunk(&it, b->tb, &chunk, &len) && len != 0U) {
        size_t take = len;
        size_t k;

        if (take > sizeof(row) - 1U - n)
            take = sizeof(row) - 1U - n;
        for (k = 0U; k < take; k++)
            row[n++] = (char)chunk[k];
        break;
    }
    row[n] = '\0';
    if (sscanf(row, "%u", &id) != 1)
        return 0U;
    return (u32)id;
}
