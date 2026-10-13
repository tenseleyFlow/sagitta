/*
 * YEW-F-129 — F15 literal greps reject legitimate prose and fixture data.
 * Correct behavior: semantic controls reject code, not comments or data.
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

bool test_yew_f_129(char *why, size_t why_cap)
{
    bool amended = file_has(
        ".docs/plan/00-decisions.md",
        "Foundation scan closeout amendment S59-A37");
    bool s00 = file_has(
        ".docs/sprints/00-scaffolding/s00-repo-and-foundations.md",
        "token-aware ban control");
    bool s01 = file_has(
        ".docs/sprints/00-scaffolding/s01-test-infrastructure.md",
        "semantic registration control");
    bool closeout = file_has(
        ".docs/audits/closeout-F15-ci.md",
        "semantic ban and registration controls");

    if (amended && s00 && s01 && closeout)
        return true;
    (void)snprintf(why, why_cap,
                   "F15 still uses raw prose/data-matching grep gates");
    return false;
}
