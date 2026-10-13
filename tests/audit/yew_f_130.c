/*
 * YEW-F-130 — Sprint 1's landing-day timing is applied to today's suite.
 * Correct behavior: retain that historical measurement and use current CI
 * budgets for the expanded aggregate.
 */
#include "audit.h"

#include <stdio.h>
#include <string.h>

static bool file_has(const char *path, const char *needle)
{
    FILE *file = fopen(path, "rb");
    char line[2048];

    if (file == NULL)
        return false;
    while (fgets(line, sizeof(line), file) != NULL) {
        if (strstr(line, needle) != NULL) {
            (void)fclose(file);
            return true;
        }
    }
    (void)fclose(file);
    return false;
}

bool test_yew_f_130(char *why, size_t why_cap)
{
    bool amended = file_has(
        ".docs/plan/00-decisions.md",
        "Foundation timing closeout amendment S59-A38");
    bool sprint = file_has(
        ".docs/sprints/00-scaffolding/s01-test-infrastructure.md",
        "landing-day unit+smoke budget");
    bool closeout = file_has(
        ".docs/audits/closeout-F15-ci.md",
        "current suite follows its CI job budget");

    if (amended && sprint && closeout)
        return true;
    (void)snprintf(why, why_cap,
                   "Sprint 1's 10-second budget still covers all make test");
    return false;
}
