/* Sprint 19: the E-mode command surface over the shell layer.  Every
 * command here routes through the registry like any other (s13's
 * one-dispatch-surface doctrine), so :! is scriptable and recordable. */
#define _POSIX_C_SOURCE 200809L

#include "edit/shell_cmds.h"

#include <signal.h>
#include <stdio.h>
#include <string.h>

#include "edit/ed.h"
#include "edit/select.h"
#include "edit/shell.h"
#include "text/piece.h"
#include "ui/message.h"
#include "util/log.h"

/* The command text an invocation carries, or NULL. */
static const char *shell_arg(const CmdCtx *cx)
{
    if (cx->sarg != NULL && cx->sarg_len != 0U)
        return cx->sarg;
    if (cx->argv.n != 0U && cx->argv.v[0] != NULL &&
        cx->argv.v[0][0] != '\0')
        return cx->argv.v[0];
    return NULL;
}

/* Resolves a CmdRange to a byte span; SAG_RANGE_NONE means "no region". */
static bool range_span(CmdCtx *cx, Span *out)
{
    const TextBuf *tb;
    Win *w = cx->win;

    if (w == NULL || w->buf == NULL || w->buf->tb == NULL)
        return false;
    tb = w->buf->tb;
    switch (cx->range.kind) {
    case SAG_RANGE_BUFFER:
        out->lo = 0U;
        out->hi = sag_textbuf_len(tb);
        return true;
    case SAG_RANGE_LINES: {
        Span lo = sag_textbuf_line_span(tb, cx->range.lo);
        Span hi = sag_textbuf_line_span(tb, cx->range.hi);

        out->lo = lo.lo;
        out->hi = hi.hi;
        return true;
    }
    case SAG_RANGE_SELECTION: {
        const Cursor *c;

        if (w->cs.curs.len == 0U)
            return false;
        c = &w->cs.curs.data[w->cs.primary];
        *out = sag_sel_span(w, c);
        return out->hi > out->lo;
    }
    case SAG_RANGE_NONE:
    default:
        break;
    }
    return false;
}

CmdStatus sag_shell_cmd_run(CmdCtx *cx)
{
    const char *cmdline = shell_arg(cx);
    char err[256] = {0};
    Span region;

    if (cmdline == NULL) {
        sag_msg(cx->ed, SAG_MSG_ERROR, ":! needs a command");
        return SAG_CMD_ERR_ARG;
    }
    /* A range turns :! into the §5 filter: `:%!sort` and `:'<,'>!fmt` are
     * the same command with a region attached. */
    if (cx->range.given && range_span(cx, &region)) {
        SagFilterResult r = sag_shell_filter(cx->ed, cx->win, region,
                                             cmdline, NULL);

        return r == SAG_FILT_OK ? SAG_CMD_OK : SAG_CMD_ERR_STATE;
    }
    if (sag_shell_run(cx->ed, cmdline, true, err, sizeof(err)) == 0U) {
        sag_msg(cx->ed, SAG_MSG_ERROR, "%s", err);
        return SAG_CMD_ERR_STATE;
    }
    return SAG_CMD_OK;
}

CmdStatus sag_shell_cmd_run_bg(CmdCtx *cx)
{
    const char *cmdline = shell_arg(cx);
    char err[256] = {0};

    if (cmdline == NULL) {
        sag_msg(cx->ed, SAG_MSG_ERROR, ":!bg needs a command");
        return SAG_CMD_ERR_ARG;
    }
    /* Same as :! without stealing focus. */
    if (sag_shell_run(cx->ed, cmdline, false, err, sizeof(err)) == 0U) {
        sag_msg(cx->ed, SAG_MSG_ERROR, "%s", err);
        return SAG_CMD_ERR_STATE;
    }
    return SAG_CMD_OK;
}

CmdStatus sag_shell_cmd_read(CmdCtx *cx)
{
    const char *cmdline = shell_arg(cx);
    char err[256] = {0};

    if (cmdline == NULL) {
        sag_msg(cx->ed, SAG_MSG_ERROR, ":r ! needs a command");
        return SAG_CMD_ERR_ARG;
    }
    if (sag_shell_read(cx->ed, cmdline, err, sizeof(err)) == 0U) {
        sag_msg(cx->ed, SAG_MSG_ERROR, "%s", err);
        return SAG_CMD_ERR_STATE;
    }
    return SAG_CMD_OK;
}

CmdStatus sag_shell_cmd_filter(CmdCtx *cx)
{
    const char *cmdline = shell_arg(cx);
    Span region;
    SagFilterResult r;

    if (cmdline == NULL) {
        sag_msg(cx->ed, SAG_MSG_ERROR, "filter needs a command");
        return SAG_CMD_ERR_ARG;
    }
    if (!range_span(cx, &region)) {
        sag_msg(cx->ed, SAG_MSG_ERROR, "filter needs a range or selection");
        return SAG_CMD_ERR_ARG;
    }
    r = sag_shell_filter(cx->ed, cx->win, region, cmdline, NULL);
    return r == SAG_FILT_OK ? SAG_CMD_OK : SAG_CMD_ERR_STATE;
}

CmdStatus sag_job_cmd_list(CmdCtx *cx)
{
    if (sag_jobs_table_open(cx->ed) == NULL)
        return SAG_CMD_ERR_STATE;
    return SAG_CMD_OK;
}

/* The job a job-command addresses: an explicit count wins, else the
 * *jobs* row under the cursor. */
static u32 target_job(CmdCtx *cx)
{
    if (cx->count_given && cx->count != 0U)
        return cx->count;
    return sag_jobs_table_row_id(cx->ed);
}

static CmdStatus job_signal_cmd(CmdCtx *cx, int sig, const char *verb)
{
    u32 id = target_job(cx);

    if (id == 0U) {
        sag_msg(cx->ed, SAG_MSG_ERROR, "no job selected");
        return SAG_CMD_ERR_ARG;
    }
    if (!sag_job_signal(cx->ed, id, sig)) {
        sag_msg(cx->ed, SAG_MSG_WARN, "job %u is not running",
                (unsigned)id);
        return SAG_CMD_ERR_STATE;
    }
    sag_msg(cx->ed, SAG_MSG_INFO, "%s job %u", verb, (unsigned)id);
    sag_jobs_table_refresh(cx->ed);
    return SAG_CMD_OK;
}

CmdStatus sag_job_cmd_kill(CmdCtx *cx)
{
    return job_signal_cmd(cx, SIGTERM, "terminated");
}

CmdStatus sag_job_cmd_kill_force(CmdCtx *cx)
{
    return job_signal_cmd(cx, SIGKILL, "killed");
}

CmdStatus sag_job_cmd_jump(CmdCtx *cx)
{
    u32 id = target_job(cx);
    SagJob *j = sag_job_find(cx->ed, id);

    if (j == NULL || j->buf == NULL) {
        sag_msg(cx->ed, SAG_MSG_WARN, "job %u has no output buffer",
                (unsigned)id);
        return SAG_CMD_ERR_STATE;
    }
    return sag_ed_show_buffer(cx->ed, j->buf) ? SAG_CMD_OK
                                              : SAG_CMD_ERR_STATE;
}

CmdStatus sag_job_cmd_clear_finished(CmdCtx *cx)
{
    Ed *ed = cx->ed;
    u32 i = 0U;
    u32 removed = 0U;

    while (i < ed->jobs.len) {
        SagJob *j = &ed->jobs.v[i];

        if (j->state == SAG_JOB_RUNNING) {
            i++;
            continue;
        }
        /* Drop the output buffer with the job: keeping it would leave
         * scratch buffers nobody can reach from the table. */
        if (j->buf != NULL) {
            sag_ws_scratch_drop(ed, j->buf);
            j->buf = NULL;
        }
        sag_job_release(ed, j);
        removed++;
    }
    sag_msg(ed, SAG_MSG_INFO, "cleared %u finished job%s",
            (unsigned)removed, removed == 1U ? "" : "s");
    sag_jobs_table_refresh(ed);
    return SAG_CMD_OK;
}

CmdStatus sag_job_cmd_rerun(CmdCtx *cx)
{
    u32 id = target_job(cx);
    SagJob *j = sag_job_find(cx->ed, id);
    char cmdline[512];
    char err[256] = {0};

    if (j == NULL) {
        sag_msg(cx->ed, SAG_MSG_ERROR, "no job selected");
        return SAG_CMD_ERR_ARG;
    }
    (void)snprintf(cmdline, sizeof(cmdline), "%s", j->label);
    if (sag_shell_run(cx->ed, cmdline, false, err, sizeof(err)) == 0U) {
        sag_msg(cx->ed, SAG_MSG_ERROR, "%s", err);
        return SAG_CMD_ERR_STATE;
    }
    sag_jobs_table_refresh(cx->ed);
    return SAG_CMD_OK;
}

CmdStatus sag_shell_cmd_term(CmdCtx *cx)
{
    /* A permanent non-goal, not a sprint deferral: an interactive
     * pty-backed buffer is a different subsystem (terminal emulation,
     * resize propagation, CR/backspace/ANSI interpretation) and 1.0 does
     * not ship one. */
    sag_msg(cx->ed, SAG_MSG_ERROR,
            "interactive terminals are not a 1.0 feature (jobs are: see :!)");
    return SAG_CMD_ERR_STATE;
}
