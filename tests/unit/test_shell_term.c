/*
 * Sprint 57.18 §4: `:!!cmd` routes one command through Sprint 19's
 * existing terminal handover, and `ed.shell.term` still refuses.
 *
 * The RESTORE half of §4 is not here: proving the terminal comes back
 * needs a real controlling terminal, so it lives in test_job_handover.c
 * beside the pty child that already owns that apparatus.  What is here
 * is the routing, the refusals, and the claim that ordinary `:!` did not
 * move.
 */
#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700

#include "harness.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "edit/ed.h"
#include "edit/shell.h"
#include "edit/shell_cmds.h"
#include "ui/cmdparse.h"

typedef struct TermFix {
    Ed ed;
    char base[PATH_MAX];
} TermFix;

static void term_fix_make(TermFix *f)
{
    const char *tmp = getenv("TMPDIR");
    int n;

    if (tmp == NULL || tmp[0] == '\0')
        tmp = "/tmp";
    n = snprintf(f->base, sizeof(f->base), "%s/yew-term-XXXXXX", tmp);
    YEW_ASSERT(n > 0 && (size_t)n < sizeof(f->base));
    YEW_ASSERT_NOT_NULL(mkdtemp(f->base));
    yew_cmd_shutdown();
    yew_cmd_init();
    yew_ed_init(&f->ed);
    YEW_ASSERT(yew_ed_open_scratch(&f->ed));
    YEW_ASSERT(yew_ed_set_workspace_root(&f->ed, f->base));
    yew_layout_compute(f->ed.pane_root, (Rect){0U, 0U, 80U, 24U});
}

static void term_fix_free(TermFix *f)
{
    yew_ed_free(&f->ed);
    yew_cmd_shutdown();
    YEW_ASSERT_EQ_I64(rmdir(f->base), 0);
}

/* Run one E-mode line through the real parse and dispatch, so the test
 * exercises the same route a typed `:!!` takes. */
static CmdStatus term_run_line(TermFix *f, const char *line)
{
    Arena scratch;
    CmdParse parsed;
    YewCmdInvoke invoke;
    CmdStatus status;

    arena_init(&scratch);
    if (!yew_cmd_parse(&f->ed, line, strlen(line), &scratch, &parsed)) {
        arena_free_all(&scratch);
        return YEW_CMD_ERR_ARG;
    }
    invoke = (YewCmdInvoke){parsed.range, parsed.argv, 0, parsed.bang,
                            f->ed.win};
    status = yew_ed_invoke_parsed(&f->ed, parsed.command, &invoke);
    arena_free_all(&scratch);
    return status;
}

/*
 * DoD 5: the refusal is still a refusal, and its wording now says what
 * is refused (emulation) and what is not (lending the real terminal).
 * A reader who finds only one of the two sentences must not conclude the
 * other one is a contradiction.
 */
void test_shell_term_refuses_and_names_the_route(void)
{
    TermFix f;

    term_fix_make(&f);
    YEW_ASSERT_EQ_I64(term_run_line(&f, ":shell.term"), YEW_CMD_ERR_STATE);
    YEW_ASSERT_NOT_NULL(strstr(f.ed.msg.text, "does not emulate"));
    YEW_ASSERT_NOT_NULL(strstr(f.ed.msg.text, ":!!"));
    YEW_ASSERT_EQ_I64(f.ed.msg.sev, YEW_MSG_ERROR);
    /* The refusal and the route are two different commands. */
    YEW_ASSERT(yew_cmd_lookup("ed.shell.term", 13U).v != 0U);
    YEW_ASSERT(yew_cmd_lookup("ed.shell.term_run", 17U).v != 0U);
    term_fix_free(&f);
}

/*
 * §4: `:!!cmd` runs the command with the terminal and reports its exit;
 * `:!cmd` is untouched and still streams into a job buffer.
 *
 * This process has no controlling terminal under the test runner, so
 * yew_job_run_sync skips the handover (ed.tty_ready is false) and runs
 * the child with the inherited stdio -- which is exactly the path whose
 * REPORTING is under test here.  The restore claim is asserted in
 * test_job_handover.c, against a real pty.
 */
void test_shell_term_run_reports_every_outcome(void)
{
    TermFix f;

    term_fix_make(&f);
    YEW_ASSERT_EQ_I64(term_run_line(&f, ":!!true"), YEW_CMD_OK);
    YEW_ASSERT_NOT_NULL(strstr(f.ed.msg.text, "exited 0"));
    YEW_ASSERT_EQ_I64(f.ed.msg.sev, YEW_MSG_INFO);
    /* Nothing entered the async job table: this child was waited for. */
    YEW_ASSERT_EQ_U64(f.ed.jobs.len, 0U);

    YEW_ASSERT_EQ_I64(term_run_line(&f, ":!!exit 23"), YEW_CMD_OK);
    YEW_ASSERT_NOT_NULL(strstr(f.ed.msg.text, "exited 23"));
    YEW_ASSERT_EQ_I64(f.ed.msg.sev, YEW_MSG_WARN);
    YEW_ASSERT_EQ_U64(f.ed.jobs.len, 0U);

    YEW_ASSERT_EQ_I64(term_run_line(&f, ":!!kill -TERM $$"), YEW_CMD_OK);
    YEW_ASSERT_NOT_NULL(strstr(f.ed.msg.text, "killed by signal"));
    YEW_ASSERT_EQ_U64(f.ed.jobs.len, 0U);

    /* The whole line after the second bang is the command, verbatim:
     * quoting and redirection reach the shell exactly as `:!` does. */
    YEW_ASSERT_EQ_I64(term_run_line(&f, ":!!test \"a b\" = \"a b\""),
                      YEW_CMD_OK);
    YEW_ASSERT_NOT_NULL(strstr(f.ed.msg.text, "exited 0"));

    /* And ordinary `:!` is still the asynchronous, captured form. */
    YEW_ASSERT_EQ_I64(term_run_line(&f, ":!true"), YEW_CMD_OK);
    YEW_ASSERT_EQ_U64(f.ed.jobs.len, 1U);
    term_fix_free(&f);
}

/*
 * §4's two refusals: a range has nothing to pipe through a child that
 * owns the screen, and --batch has no terminal to hand over.  Both are
 * named refusals rather than silent degradations into `:!` (invariant 3).
 */
void test_shell_term_run_refuses_a_range_and_batch(void)
{
    TermFix f;
    YewJobWait wait;
    char err[256];

    term_fix_make(&f);
    YEW_ASSERT_EQ_I64(term_run_line(&f, ":%!!sort"), YEW_CMD_ERR_ARG);
    YEW_ASSERT_NOT_NULL(strstr(f.ed.msg.text, "no range"));
    YEW_ASSERT_EQ_U64(f.ed.jobs.len, 0U);

    YEW_ASSERT_EQ_I64(term_run_line(&f, ":!!"), YEW_CMD_ERR_STATE);
    YEW_ASSERT_NOT_NULL(strstr(f.ed.msg.text, "needs a command"));

    f.ed.headless = true;
    YEW_ASSERT(!yew_shell_term_run(&f.ed, "true", &wait, err, sizeof(err)));
    YEW_ASSERT_NOT_NULL(strstr(err, "--batch"));
    YEW_ASSERT_NOT_NULL(strstr(err, "ed.shell.run"));
    YEW_ASSERT_EQ_I64(term_run_line(&f, ":!!true"), YEW_CMD_ERR_STATE);
    YEW_ASSERT_NOT_NULL(strstr(f.ed.msg.text, "--batch"));
    f.ed.headless = false;
    term_fix_free(&f);
}
