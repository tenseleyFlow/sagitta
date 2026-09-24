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

static u32 term_public_jobs(const Ed *ed)
{
    u32 i;
    u32 n = 0U;

    for (i = 0U; i < ed->jobs.len; i++)
        n += ed->jobs.v[i].internal ? 0U : 1U;
    return n;
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

    /* The editor deliberately uses the user's shell.  Delegate the signal
     * fixture to POSIX sh so this remains exact when $SHELL is fish. */
    YEW_ASSERT_EQ_I64(
        term_run_line(&f, ":!!exec /bin/sh -c 'kill -TERM $$'"),
        YEW_CMD_OK);
    YEW_ASSERT_NOT_NULL(strstr(f.ed.msg.text, "killed by signal"));
    YEW_ASSERT_EQ_U64(f.ed.jobs.len, 0U);

    /* The whole line after the second bang is the command, verbatim:
     * quoting and redirection reach the shell exactly as `:!` does. */
    YEW_ASSERT_EQ_I64(term_run_line(&f, ":!!test \"a b\" = \"a b\""),
                      YEW_CMD_OK);
    YEW_ASSERT_NOT_NULL(strstr(f.ed.msg.text, "exited 0"));

    /* And ordinary `:!` is still the asynchronous, captured form: one
     * user-visible job (Sprint 57.27's shell session, when on, adds its
     * own hidden internal one). */
    YEW_ASSERT_EQ_I64(term_run_line(&f, ":!true"), YEW_CMD_OK);
    YEW_ASSERT_EQ_U64(term_public_jobs(&f.ed), 1U);
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

/* Reads `path` whole into `out` (NUL-terminated); false when absent. */
static bool term_slurp(const char *path, char *out, size_t cap)
{
    FILE *fp = fopen(path, "rb");
    size_t n;

    if (fp == NULL)
        return false;
    n = fread(out, 1U, cap - 1U, fp);
    out[n] = '\0';
    (void)fclose(fp);
    return true;
}

/*
 * Sprint 57.31 §3: the argv route runs the words it is given -- no shell
 * parses them -- and, like `:!!`, a child handed the terminal keeps the
 * user's pagers, where a captured job still gets PAGER=cat.
 */
void test_shell_term_argv_runs_words_and_keeps_the_pager(void)
{
    TermFix f;
    YewJobWait wait;
    char err[256];
    char out[PATH_MAX];
    char got[256];
    char *saved = getenv("PAGER") == NULL ? NULL :
                  strdup(getenv("PAGER"));
    char *argv[6];
    Arena a;
    char **env;
    size_t i;
    bool cat = false;
    int n;

    term_fix_make(&f);
    n = snprintf(out, sizeof(out), "%s/out", f.base);
    YEW_ASSERT(n > 0 && (size_t)n < sizeof(out));
    YEW_ASSERT_EQ_I64(setenv("PAGER", "my-pager -R", 1), 0);
    argv[0] = (char *)"/bin/sh";
    argv[1] = (char *)"-c";
    argv[2] = (char *)"printf '%s|%s|%s' \"$#\" \"$1\" \"$PAGER\" > \"$2\"";
    argv[3] = (char *)"sh";
    argv[4] = (char *)"a b;$(touch nope)";
    argv[5] = NULL;
    /* $2 is the output path: pass it as the second word. */
    {
        char *full[7];

        full[0] = argv[0];
        full[1] = argv[1];
        full[2] = argv[2];
        full[3] = argv[3];
        full[4] = argv[4];
        full[5] = out;
        full[6] = NULL;
        YEW_ASSERT(yew_shell_term_argv(&f.ed, full, &wait, err,
                                       sizeof(err)));
    }
    YEW_ASSERT_EQ_I64(wait.state, YEW_JOB_EXITED);
    YEW_ASSERT_EQ_I64(wait.exit_code, 0);
    YEW_ASSERT(term_slurp(out, got, sizeof(got)));
    YEW_ASSERT_EQ_STR(got, "2|a b;$(touch nope)|my-pager -R");
    YEW_ASSERT_EQ_I64(unlink(out), 0);

    /* A captured job's environment still forces the pager off. */
    arena_init(&a);
    env = yew_job_env(&f.ed, &a);
    for (i = 0U; env != NULL && env[i] != NULL; i++) {
        if (strcmp(env[i], "PAGER=cat") == 0)
            cat = true;
        YEW_ASSERT(strcmp(env[i], "PAGER=my-pager -R") != 0);
    }
    YEW_ASSERT(cat);
    arena_free_all(&a);

    /* Headless: refused by name, nothing runs. */
    f.ed.headless = true;
    YEW_ASSERT(!yew_shell_term_argv(&f.ed, argv, &wait, err, sizeof(err)));
    YEW_ASSERT_NOT_NULL(strstr(err, "--batch"));
    f.ed.headless = false;
    YEW_ASSERT(!yew_shell_term_argv(&f.ed, NULL, &wait, err, sizeof(err)));

    if (saved != NULL) {
        YEW_ASSERT_EQ_I64(setenv("PAGER", saved, 1), 0);
        free(saved);
    } else {
        YEW_ASSERT_EQ_I64(unsetenv("PAGER"), 0);
    }
    term_fix_free(&f);
}
