/*
 * YEW-F-131 — F15 retains a future size stub and superseded hard caps.
 * Correct behavior: Sprint 56 names the shipped size owner and Sprint 57/F15
 * follow the eight current executable budgets plus their amendment history.
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

bool test_yew_f_131(char *why, size_t why_cap)
{
    bool amended = file_has(
        ".docs/plan/00-decisions.md",
        "Size closeout amendment S59-A39");
    bool owner = file_has(
        ".docs/sprints/13-performance/s56-latency-gates.md",
        "runs its shipped Sprint 57 owner");
    bool full = file_has(
        ".docs/sprints/13-performance/s57-size-and-embedded.md",
        "2,310,144 bytes");
    bool minimal = file_has(
        ".docs/sprints/13-performance/s57-size-and-embedded.md",
        "1,818,624 bytes");
    bool closeout = file_has(
        ".docs/audits/closeout-F15-ci.md",
        "binding eight current caps");

    if (amended && owner && full && minimal && closeout)
        return true;
    (void)snprintf(why, why_cap,
                   "F15 retains a future size stub or superseded caps");
    return false;
}
