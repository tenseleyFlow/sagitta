/*
 * YEW-F-026 — PTY cases have no XFAIL/XPASS state.
 *
 * Correct behavior: Sprint 58 section 3 and F15 q1 require PtyCase.xfail_id
 * and a golden match for an expected failure to be a hard XPASS.
 *
 * Baseline failure: PtyCase has no xfail_id and the runner has no XFAIL or
 * XPASS classification path.
 */
#include "audit.h"

#include <stdio.h>
#include <string.h>

static bool file_contains(const char *path, const char *needle)
{
    char line[4096];
    FILE *file = fopen(path, "rb");
    bool found = false;

    if (file == NULL)
        return false;
    while (fgets(line, sizeof(line), file) != NULL)
        if (strstr(line, needle) != NULL)
            found = true;
    if (ferror(file) || fclose(file) != 0)
        return false;
    return found;
}

bool test_yew_f_026(char *why, size_t why_cap)
{
    bool field = file_contains("tests/pty/harness.h", "xfail_id");
    bool xfail = file_contains("tests/pty/runner.c", "XFAIL");
    bool xpass = file_contains("tests/pty/runner.c", "XPASS");

    if (!field || !xfail || !xpass)
        (void)snprintf(why, why_cap,
                       "PtyCase.xfail_id=%u runner XFAIL=%u XPASS=%u",
                       field ? 1U : 0U, xfail ? 1U : 0U,
                       xpass ? 1U : 0U);
    return field && xfail && xpass;
}
