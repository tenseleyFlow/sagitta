/*
 * YEW-F-025 — script tests have no XFAIL/XPASS state.
 *
 * Correct behavior: Sprint 58 section 3 and F15 q1 require a script-test
 * XFAIL marker tied to YEW-F IDs, with a passing expected failure reported
 * as a hard XPASS.
 *
 * Baseline failure: the script runner classifies only PASS, FAIL and SKIP;
 * it neither parses an XFAIL marker nor emits XPASS.
 */
#include "audit.h"

#include <stdio.h>
#include <string.h>

static bool source_contains(const char *path, const char *needle)
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

bool test_yew_f_025(char *why, size_t why_cap)
{
    bool xfail = source_contains("tests/script/runner.c", "XFAIL");
    bool xpass = source_contains("tests/script/runner.c", "XPASS");

    if (!xfail || !xpass)
        (void)snprintf(why, why_cap, "script runner XFAIL=%u XPASS=%u",
                       xfail ? 1U : 0U, xpass ? 1U : 0U);
    return xfail && xpass;
}
