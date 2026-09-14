/*
 * YEW-F-017 — interactive rebase bypasses the Git verb/environment boundary.
 *
 * Correct behavior: Sprints 51, 52, and Sprint 58 F13 require every Git
 * invocation to select a row from the static verb table and inherit the
 * forced/sanitized Git environment. Interactive rebase may override the two
 * editor variables for terminal handover, but no other policy row.
 *
 * Baseline failure: fuss_rebase_sync constructed a direct `git rebase` job.
 * The remediation must delegate to the Git terminal runner; its behavioral
 * unit test pins the complete argv and environment rather than trusting this
 * source-structure control alone.
 */
#include "audit.h"

#include <stdio.h>
#include <string.h>

static bool read_source(const char *path, char *buf, size_t cap)
{
    FILE *file = fopen(path, "rb");
    size_t len;

    if (file == NULL || cap == 0U)
        return false;
    len = fread(buf, 1U, cap - 1U, file);
    if (ferror(file) || fclose(file) != 0)
        return false;
    buf[len] = '\0';
    return true;
}

bool test_yew_f_017(char *why, size_t why_cap)
{
    char git[100000];
    char fuss[200000];
    const char *table;
    const char *table_end;
    const char *sync;
    const char *sync_end;
    const char *runner;
    const char *runner_end;
    const char *hit;
    bool descriptor;
    bool direct_job;
    bool delegated;
    bool canonical_argv;
    bool canonical_env;
    bool sync_job;
    bool prompt;
    bool flush;
    bool trace;

    if (!read_source("src/mod/git/git.c", git, sizeof(git)) ||
        !read_source("src/mod/git/fussmode.c", fuss, sizeof(fuss)))
        return false;
    table = strstr(git, "static const GitVerb git_verbs[]");
    table_end = table == NULL ? NULL : strstr(table, "#undef GIT_READ");
    sync = strstr(fuss, "static CmdStatus fuss_rebase_sync(");
    sync_end = sync == NULL ? NULL : strstr(sync, "static void fuss_prompt_done");
    runner = strstr(git, "bool yew_git_run_terminal(");
    runner_end = runner == NULL ? NULL :
                 strstr(runner, "typedef struct GitCallbackOwner");
    if (table == NULL || table_end == NULL || sync == NULL ||
        sync_end == NULL || runner == NULL || runner_end == NULL)
        return false;
    hit = strstr(table, "\"rebase\"");
    descriptor = hit != NULL && hit < table_end;
    hit = strstr(sync, "yew_job_run_sync");
    direct_job = hit != NULL && hit < sync_end;
    hit = strstr(sync, "yew_git_run_terminal");
    delegated = hit != NULL && hit < sync_end;
    hit = strstr(runner, "git_build_argv");
    canonical_argv = hit != NULL && hit < runner_end;
    hit = strstr(runner, "git_env_build");
    canonical_env = hit != NULL && hit < runner_end;
    hit = strstr(runner, "yew_job_run_sync");
    sync_job = hit != NULL && hit < runner_end;
    prompt = strstr(git, "GIT_TERMINAL_PROMPT=0") != NULL;
    flush = strstr(git, "GIT_FLUSH=1") != NULL;
    trace = strstr(git, "GIT_TRACE2") != NULL &&
            strstr(git, "GIT_TRACE_PACKET") != NULL;
    if (!descriptor || direct_job || !delegated || !canonical_argv ||
        !canonical_env || !sync_job || !prompt || !flush || !trace) {
        (void)snprintf(why, why_cap,
                       "rebase descriptor=%u direct_job=%u delegated=%u argv=%u env=%u sync=%u prompt=%u flush=%u trace=%u",
                       descriptor ? 1U : 0U, direct_job ? 1U : 0U,
                       delegated ? 1U : 0U, canonical_argv ? 1U : 0U,
                       canonical_env ? 1U : 0U, sync_job ? 1U : 0U,
                       prompt ? 1U : 0U, flush ? 1U : 0U,
                       trace ? 1U : 0U);
    }
    return descriptor && !direct_job && delegated && canonical_argv &&
           canonical_env && sync_job && prompt && flush && trace;
}
