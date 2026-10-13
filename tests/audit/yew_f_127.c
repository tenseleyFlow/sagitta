/*
 * YEW-F-127 — F15 retains landing-day hard errors for shipped surfaces.
 * Correct behavior: closeout tests the later owners that shipped each path.
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

bool test_yew_f_127(char *why, size_t why_cap)
{
    bool amended = file_has(
        ".docs/plan/00-decisions.md",
        "Foundation staging closeout amendment S59-A35");
    bool s00 = file_has(
        ".docs/sprints/00-scaffolding/s00-repo-and-foundations.md",
        "shipped startup paths");
    bool s01 = file_has(
        ".docs/sprints/00-scaffolding/s01-test-infrastructure.md",
        "current targets run their shipped owners");
    bool s37 = file_has(
        ".docs/sprints/07-fletch-editor/s37-headless-batch.md",
        "shipped `--replay` and `--grant` owners");
    bool closeout = file_has(
        ".docs/audits/closeout-F15-ci.md",
        "shipped file/TUI, batch, and clean paths");

    if (amended && s00 && s01 && s37 && closeout)
        return true;
    (void)snprintf(why, why_cap,
                   "F15 retains landing-day hard errors for shipped paths");
    return false;
}
