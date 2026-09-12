/*
 * YEW-F-017 — interactive rebase bypasses the Git verb/environment boundary.
 *
 * Correct behavior: Sprints 51, 52, and Sprint 58 F13 require every Git
 * invocation to select a row from the static verb table and inherit the
 * forced/sanitized Git environment. Interactive rebase may override the two
 * editor variables for terminal handover, but no other policy row.
 *
 * Baseline failure: fuss_rebase_sync constructs a direct `git rebase` job.
 * There is no `rebase` descriptor, and its private environment omits
 * GIT_TERMINAL_PROMPT, GIT_FLUSH, and the trace-variable removals.
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
    const char *hit;
    bool descriptor;
    bool direct_job;
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
    if (table == NULL || table_end == NULL || sync == NULL || sync_end == NULL)
        return false;
    hit = strstr(table, "\"rebase\"");
    descriptor = hit != NULL && hit < table_end;
    hit = strstr(sync, "yew_job_run_sync");
    direct_job = hit != NULL && hit < sync_end;
    hit = strstr(sync, "GIT_TERMINAL_PROMPT=0");
    prompt = hit != NULL && hit < sync_end;
    hit = strstr(sync, "GIT_FLUSH=1");
    flush = hit != NULL && hit < sync_end;
    hit = strstr(sync, "GIT_TRACE");
    trace = hit != NULL && hit < sync_end;
    if (!descriptor || direct_job || !prompt || !flush || !trace) {
        (void)snprintf(why, why_cap,
                       "rebase descriptor=%u direct_job=%u prompt=%u flush=%u trace_policy=%u",
                       descriptor ? 1U : 0U, direct_job ? 1U : 0U,
                       prompt ? 1U : 0U, flush ? 1U : 0U,
                       trace ? 1U : 0U);
    }
    return descriptor && !direct_job && prompt && flush && trace;
}
