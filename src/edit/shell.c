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

#define SAG_JOBS_TABLE_NAME "*jobs*"

/* Filter poll tick: bounds how long the restricted loop sleeps before
 * checking for child exit.  See filter_drive. */
#define SAG_FILTER_TICK_MS 20

/* Our own table rather than strsignal(): its text is locale-dependent and
 * would leak into pty goldens, breaking deterministic rendering. */
static const char *sag_signame(int sig)
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

static void fmt_elapsed(char *out, size_t n, i64 ms)
{
    (void)snprintf(out, n, "%.2fs", (double)ms / 1000.0);
}

/* Appends straight through the edit chokepoint so marks and cursors track,
 * but outside any undo transaction: a job buffer is SAG_BUF_NOUNDO. */
static void buf_append_raw(Ed *ed, Buffer *b, const u8 *bytes, u64 len)
{
    EditCtx ec = {0};
    Win *w = ed->win != NULL && ed->win->buf == b ? ed->win : NULL;

    if (b == NULL || b->tb == NULL || len == 0U)
        return;
    ec.tb = b->tb;
    ec.marks = b->marks;
    ec.cset = w != NULL ? &w->cs : NULL;
    ec.undo = NULL; /* SAG_BUF_NOUNDO: no ops recorded, nothing journaled */
    ec.jrnl = NULL;
    ec.meta = &b->meta;
    (void)sag_edit_insert(&ec, BYTEOFF(sag_textbuf_len(b->tb)), bytes, len);
}

static void buf_append_str(Ed *ed, Buffer *b, const char *s)
{
    buf_append_raw(ed, b, (const u8 *)s, (u64)strlen(s));
}

/* ------------------------------------------------------------------ */
/* Mode (a): streaming into a job buffer (§4)                         */
/* ------------------------------------------------------------------ */

void sag_job_buffer_append(Ed *ed, SagJob *j, const u8 *bytes, u64 len,
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
        at_end = cur->pos.v >= sag_textbuf_len(j->buf->tb) &&
                 w->cs.active == SAG_MC_ACTIVE_NONE;
    }
    buf_append_raw(ed, j->buf, bytes, len);
    if (at_end && j->follow_tail && w != NULL) {
        Cursor *cur = &w->cs.curs.data[w->cs.primary];

        cur->pos = BYTEOFF(sag_textbuf_len(j->buf->tb));
        cur->anchor = cur->pos;
        ed->cursor_follow_pending = true;
    }
    if (w != NULL)
        sag_ed_damage_document(ed);
}

static void job_footer(Ed *ed, SagJob *j, char *out, size_t n)
{
    char elapsed[32];

    (void)ed;

    fmt_elapsed(elapsed, sizeof(elapsed), j->end_ms - j->start_ms);
    switch (j->state) {
    case SAG_JOB_EXECFAIL:
        (void)snprintf(out, n, "[cannot run %s: %s]", j->label,
                       strerror(j->exec_errno));
        break;
    case SAG_JOB_SIGNALED:
        (void)snprintf(out, n, "[killed by %s after %s]",
                       sag_signame(j->termsig), elapsed);
        break;
    case SAG_JOB_TIMEOUT:
        (void)snprintf(out, n, "[timed out after %s]", elapsed);
        break;
    case SAG_JOB_CANCELLED:
        (void)snprintf(out, n, "[cancelled after %s]", elapsed);
        break;
    case SAG_JOB_EXITED:
    default:
        (void)snprintf(out, n, "[exit %d in %s]", j->exit_code, elapsed);
        break;
    }
}

/* Mode (c): the collected output lands in one transaction at the mark. */
static void job_insert_collected(Ed *ed, SagJob *j)
{
    EditCtx ec;
    Buffer *b;
    ByteOff at;
    bool needs_nl;

    if (ed->win == NULL || ed->win->buf == NULL)
        return;
    b = ed->win->buf;
    if (j->state != SAG_JOB_EXITED || j->exit_code != 0) {
        char foot[256];

        job_footer(ed, j, foot, sizeof(foot));
        sag_msg(ed, SAG_MSG_WARN, "%s %s", j->label, foot);
        return;
    }
    if (j->collect.len == 0U) {
        sag_msg(ed, SAG_MSG_INFO, "%s — no output", j->label);
        return;
    }
    at = BYTEOFF(sag_mark_pos(b->marks, j->at).v);
    ec = sag_ed_edit_ctx(ed);
    sag_undo_begin(&ec, SAG_TXN_EXTERNAL);
    (void)sag_edit_insert(&ec, at, j->collect.data, (u64)j->collect.len);
    /* A trailing newline iff we inserted at a line start and the output
     * lacks one, so `:r !date` on an empty line does not glue the next
     * line onto it. */
    needs_nl = j->collect.data[j->collect.len - 1U] != (u8)'\n' &&
               (at.v == 0U || at.v == sag_textbuf_line_start(
                                          b->tb,
                                          sag_textbuf_line_of(b->tb, at)).v);
    if (needs_nl) {
        ByteOff end = BYTEOFF(at.v + (u64)j->collect.len);

        (void)sag_edit_insert(&ec, end, (const u8 *)"\n", 1U);
    }
    sag_undo_end(&ec);
    sag_ed_finish_edit(ed, &ec);
    sag_mark_del(b->marks, j->at);
    j->has_mark = false;
    sag_ed_damage_document(ed);
    sag_msg(ed, SAG_MSG_INFO, "%s — %llu bytes read", j->label,
            (unsigned long long)j->collect.len);
}

void sag_job_finish(Ed *ed, SagJob *j)
{
    char foot[256];

    if (ed == NULL || j == NULL)
        return;
    /* The filter drives its own job and owns the outcome, rollback
     * included; touching buffers here would fight it. */
    if (j->synchronous)
        return;
    if (j->sink == SAG_SINK_COLLECT && j->has_mark) {
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
            (void)sag_ed_show_buffer(ed, &ed->buffer);
        sag_msg(ed, SAG_MSG_INFO, ":!%s — no output (exit %d)",
                j->label, j->exit_code);
        sag_ws_scratch_drop(ed, j->buf);
        j->buf = NULL;
        return;
    }
    if (sag_textbuf_len(j->buf->tb) != 0U) {
        u64 end = sag_textbuf_len(j->buf->tb);
        u8 last = 0U;
        TextIter it;
        const u8 *chunk = NULL;
        size_t chunk_len = 0U;

        if (sag_textiter_begin(&it, j->buf->tb, BYTEOFF(end - 1U)) &&
            sag_textiter_chunk(&it, j->buf->tb, &chunk, &chunk_len) &&
            chunk_len != 0U)
            last = chunk[0];
        if (last != (u8)'\n')
            buf_append_str(ed, j->buf, "\n");
    }
    buf_append_str(ed, j->buf, foot);
    buf_append_str(ed, j->buf, "\n");
    sag_msg(ed, j->state == SAG_JOB_EXITED && j->exit_code == 0 ?
                    SAG_MSG_INFO : SAG_MSG_WARN,
            ":!%s %s", j->label, foot);
    sag_ed_damage_document(ed);
}

u32 sag_shell_run(Ed *ed, const char *cmdline, bool focus, char *err,
                  size_t errsz)
{
    SagJobSpec spec = {0};
    char name[192];
    u32 id;
    SagJob *j;
    Buffer *buf;

    if (ed == NULL || cmdline == NULL)
        return 0U;
    spec.cmdline = cmdline;
    spec.sink = SAG_SINK_BUFFER;
    id = sag_job_spawn(ed, &spec, err, errsz);
    if (id == 0U)
        return 0U;
    j = sag_job_find(ed, id);
    (void)snprintf(name, sizeof(name), "*job:%u %s*", (unsigned)id,
                   cmdline);
    buf = sag_ws_scratch_new(ed, name, SAG_BUF_NOUNDO);
    if (buf == NULL) {
        (void)sag_job_signal(ed, id, SIGTERM);
        (void)snprintf(err, errsz, "cannot create job buffer");
        return 0U;
    }
    j->buf = buf;
    if (focus)
        (void)sag_ed_show_buffer(ed, buf);
    return id;
}

/* ------------------------------------------------------------------ */
/* Mode (c): read output at the cursor (§6)                           */
/* ------------------------------------------------------------------ */

u32 sag_shell_read(Ed *ed, const char *cmdline, char *err, size_t errsz)
{
    SagJobSpec spec = {0};
    u32 id;
    SagJob *j;
    Buffer *b;
    const Cursor *cur;

    if (ed == NULL || cmdline == NULL || ed->win == NULL ||
        ed->win->buf == NULL || ed->win->cs.curs.len == 0U)
        return 0U;
    b = ed->win->buf;
    spec.cmdline = cmdline;
    spec.sink = SAG_SINK_COLLECT;
    id = sag_job_spawn(ed, &spec, err, errsz);
    if (id == 0U)
        return 0U;
    j = sag_job_find(ed, id);
    cur = &ed->win->cs.curs.data[ed->win->cs.primary];
    /* RIGHT bias so text typed at the insertion point stays before the
     * arriving output, and so editing elsewhere keeps the point correct. */
    j->at = sag_mark_add(b->marks, cur->pos, SAG_BIAS_RIGHT);
    j->has_mark = true;
    return id;
}

/* ------------------------------------------------------------------ */
/* Mode (b): the synchronous region filter (§5)                       */
/* ------------------------------------------------------------------ */

/* The filter's restricted loop.  NOT a nested event loop: terminal input
 * is buffered as typeahead and replayed afterwards, never dispatched —
 * dispatching would re-enter the editor with a half-applied transaction. */
static SagFilterResult filter_drive(Ed *ed, SagJob *j, Bytebuf *typeahead)
{
    for (;;) {
        struct pollfd pfd[8];
        u32 n = 0U;
        int tty_slot;
        int sig_slot;
        i64 now = sag_now_ms();
        i64 deadline = sag_job_deadline(ed, now);
        int rc;
        bool winch = false;
        bool cont = false;
        bool chld = false;

        if (j->state != SAG_JOB_RUNNING && j->reaped)
            break;
        sag_job_collect_fds(ed, pfd, &n);
        tty_slot = -1;
        if (ed->tty_ready) {
            pfd[n].fd = ed->tty.rfd;
            pfd[n].events = POLLIN;
            pfd[n].revents = 0;
            tty_slot = (int)n++;
        }
        sig_slot = -1;
        if (ed->tty_ready) {
            pfd[n].fd = sag_tty_signal_fd(&ed->tty);
            pfd[n].events = POLLIN;
            pfd[n].revents = 0;
            sig_slot = (int)n++;
        }
        /* Cap the wait: once the child's pipes hit EOF there is nothing
         * left in the poll set, and without a tty there is no SIGCHLD
         * pipe here either — sleeping the full timeout would report a
         * finished `sort` as SAG_FILT_TIMEOUT.  A short tick keeps exit
         * detection prompt; this loop is synchronous, so it costs
         * nothing the user can perceive. */
        if (deadline < 0 || deadline > SAG_FILTER_TICK_MS)
            deadline = SAG_FILTER_TICK_MS;
        rc = poll(pfd, (nfds_t)n, (int)deadline);
        if (rc < 0 && errno != EINTR)
            return SAG_FILT_SPAWN;
        now = sag_now_ms();
        sag_job_pump(ed, pfd, n);
        if (sig_slot >= 0 && (pfd[sig_slot].revents & POLLIN) != 0) {
            sag_tty_drain_signals(&ed->tty, &winch, &cont, &chld);
            if (chld)
                sag_job_reap(ed);
            /* SIGWINCH is honored — resize and repaint — and the filter
             * keeps running.  A resize is not a reason to lose the run. */
            if (winch || cont)
                sag_ed_resize(ed, cont);
        }
        if (tty_slot >= 0 && (pfd[tty_slot].revents & POLLIN) != 0) {
            u8 bytes[1024];
            ssize_t got = read(ed->tty.rfd, bytes, sizeof(bytes));
            ssize_t i;

            for (i = 0; i < got; i++) {
                /* The only keys inspected are cancel keys, and they are
                 * consumed rather than replayed. */
                if (bytes[i] == 0x03U || bytes[i] == 0x1BU) {
                    (void)sag_job_signal(ed, j->id, SIGTERM);
                    j->state = SAG_JOB_CANCELLED;
                    continue;
                }
                bytebuf_push_u8(typeahead, bytes[i]);
            }
        }
        /* Reap before tick: a child that exits right as the deadline
         * lands must be recorded as EXITED, not lose the race and be
         * reported as a timeout it actually beat. */
        sag_job_reap(ed);
        sag_job_tick(ed, now);
    }
    switch (j->state) {
    case SAG_JOB_TIMEOUT:
        return SAG_FILT_TIMEOUT;
    case SAG_JOB_CANCELLED:
        return SAG_FILT_CANCELLED;
    case SAG_JOB_EXECFAIL:
        return SAG_FILT_SPAWN;
    case SAG_JOB_SIGNALED:
        return SAG_FILT_CANCELLED;
    case SAG_JOB_EXITED:
    default:
        break;
    }
    return j->exit_code == 0 ? SAG_FILT_OK : SAG_FILT_NONZERO;
}

SagFilterResult sag_shell_filter(Ed *ed, Win *w, Span region,
                                 const char *cmdline, Bytebuf *stderr_out)
{
    SagJobSpec spec = {0};
    Bytebuf typeahead;
    SagFilterResult result;
    u32 id;
    SagJob *j;
    EditCtx ec;
    u64 before_lines;
    u64 after_lines;
    char err[256];

    if (ed == NULL || w == NULL || w->buf == NULL || cmdline == NULL)
        return SAG_FILT_SPAWN;
    spec.cmdline = cmdline;
    spec.sink = SAG_SINK_COLLECT;
    spec.in_buf = w->buf->tb;
    spec.in_span = region;
    spec.timeout_ms = SAG_FILTER_TIMEOUT_MS;
    id = sag_job_spawn(ed, &spec, err, sizeof(err));
    if (id == 0U) {
        sag_msg(ed, SAG_MSG_ERROR, "%s", err);
        return SAG_FILT_SPAWN;
    }
    j = sag_job_find(ed, id);
    j->synchronous = true;
    bytebuf_init(&typeahead);
    result = filter_drive(ed, j, &typeahead);

    if (result == SAG_FILT_SPAWN && j->state == SAG_JOB_EXECFAIL)
        sag_msg(ed, SAG_MSG_ERROR, "cannot run %s: %s", cmdline,
                strerror(j->exec_errno));
    if (stderr_out != NULL && j->bytes_err != 0U)
        bytebuf_append(stderr_out, j->collect.data, j->collect.len);

    before_lines = sag_textbuf_line_count(w->buf->tb);
    if (result == SAG_FILT_OK) {
        ec = sag_ed_edit_ctx_for(ed, w);
        /* One transaction wrapping delete+insert, so a single undo
         * restores the original text exactly and the journal sees one
         * commit: a crash mid-filter recovers to pre- or post-, never
         * half. */
        sag_undo_begin(&ec, SAG_TXN_FILTER);
        (void)sag_edit_delete(&ec, region);
        if (j->collect.len != 0U)
            (void)sag_edit_insert(&ec, BYTEOFF(region.lo), j->collect.data,
                                  (u64)j->collect.len);
        sag_undo_end(&ec);
        sag_ed_finish_edit(ed, &ec);
        after_lines = sag_textbuf_line_count(w->buf->tb);
        if (j->collect.len == 0U)
            /* Not rolled back — grep legitimately matches nothing — but
             * announced, because a silently vanished selection reads as
             * data loss. */
            sag_msg(ed, SAG_MSG_WARN,
                    "filter produced no output; region deleted (undo restores)");
        else
            sag_msg(ed, SAG_MSG_INFO, "filter: %llu → %llu lines",
                    (unsigned long long)before_lines,
                    (unsigned long long)after_lines);
        sag_ed_damage_document(ed);
    } else if (result == SAG_FILT_NONZERO) {
        sag_msg(ed, SAG_MSG_WARN, "filter: exit %d; buffer unchanged",
                j->exit_code);
    } else if (result == SAG_FILT_TIMEOUT) {
        sag_msg(ed, SAG_MSG_WARN, "filter timed out after %.1fs",
                (double)SAG_FILTER_TIMEOUT_MS / 1000.0);
    } else if (result == SAG_FILT_CANCELLED) {
        sag_msg(ed, SAG_MSG_WARN, "filter cancelled; buffer unchanged");
    }

    /* Typed-ahead bytes were never dispatched; replay them now. */
    if (typeahead.len != 0U && ed->probe_seeded)
        sag_input_feed(&ed->in, typeahead.data, typeahead.len);
    bytebuf_free(&typeahead);
    return result;
}

/* ------------------------------------------------------------------ */
/* The *jobs* table (§8)                                              */
/* ------------------------------------------------------------------ */

static void jobs_table_render(Ed *ed, Buffer *b)
{
    Bytebuf out;
    u32 i;

    bytebuf_init(&out);
    bytebuf_printf(&out, " id  state     code  elapsed    bytes  command\n");
    /* Newest first: the job you just started is the one you are looking
     * for. */
    for (i = ed->jobs.len; i-- > 0U;) {
        const SagJob *j = &ed->jobs.v[i];
        char elapsed[32];
        char code[16];
        i64 end = j->state == SAG_JOB_RUNNING ? sag_now_ms() : j->end_ms;

        fmt_elapsed(elapsed, sizeof(elapsed), end - j->start_ms);
        if (j->state == SAG_JOB_RUNNING)
            (void)snprintf(code, sizeof(code), "-");
        else if (j->state == SAG_JOB_SIGNALED)
            (void)snprintf(code, sizeof(code), "%s",
                           sag_signame(j->termsig));
        else
            (void)snprintf(code, sizeof(code), "%d", j->exit_code);
        bytebuf_printf(&out, "%3u  %-9s %4s  %7s  %7llu  %s\n",
                       (unsigned)j->id, sag_job_state_name(j->state), code,
                       elapsed,
                       (unsigned long long)(j->bytes_out + j->bytes_err),
                       j->label);
    }
    /* Replace wholesale: the table is derived state, so diffing it would
     * buy nothing but a chance to disagree with the job list. */
    sag_textbuf_free(b->tb);
    b->tb = sag_textbuf_from_bytes(out.data, out.len);
    bytebuf_free(&out);
}

void sag_jobs_table_refresh(Ed *ed)
{
    Buffer *b;

    if (ed == NULL)
        return;
    b = sag_ws_scratch_find(ed, SAG_JOBS_TABLE_NAME);
    if (b == NULL)
        return;
    jobs_table_render(ed, b);
    if (ed->win != NULL && ed->win->buf == b) {
        sag_vp_clamp(ed->win);
        sag_ed_damage_document(ed);
    }
}

Buffer *sag_jobs_table_open(Ed *ed)
{
    Buffer *b;

    if (ed == NULL)
        return NULL;
    b = sag_ws_scratch_find(ed, SAG_JOBS_TABLE_NAME);
    if (b == NULL)
        b = sag_ws_scratch_new(ed, SAG_JOBS_TABLE_NAME, SAG_BUF_NOUNDO);
    if (b == NULL)
        return NULL;
    jobs_table_render(ed, b);
    (void)sag_ed_show_buffer(ed, b);
    return b;
}

u32 sag_jobs_table_row_id(Ed *ed)
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
        strcmp(b->name, SAG_JOBS_TABLE_NAME) != 0 ||
        ed->win->cs.curs.len == 0U)
        return 0U;
    line = sag_textbuf_line_of(b->tb,
                               ed->win->cs.curs.data[ed->win->cs.primary].pos);
    if (line.v == 0U)
        return 0U; /* the header row addresses no job */
    span = sag_textbuf_line_span(b->tb, line);
    if (!sag_textiter_begin(&it, b->tb, BYTEOFF(span.lo)))
        return 0U;
    while (n + 1U < sizeof(row) &&
           sag_textiter_chunk(&it, b->tb, &chunk, &len) && len != 0U) {
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
