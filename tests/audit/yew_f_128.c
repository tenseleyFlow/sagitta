/*
 * YEW-F-128 — the renamed Sprint 0 contract requires yew to alias itself.
 * Correct behavior: build and install each own one yew executable path.
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

bool test_yew_f_128(char *why, size_t why_cap)
{
    bool amended = file_has(
        ".docs/plan/00-decisions.md",
        "Executable-name closeout amendment S59-A36");
    bool build = file_has(
        ".docs/sprints/00-scaffolding/s00-repo-and-foundations.md",
        "one `build/yew` executable");
    bool install = file_has(
        ".docs/sprints/00-scaffolding/s00-repo-and-foundations.md",
        "one `bin/yew` executable");
    bool closeout = file_has(
        ".docs/audits/closeout-F15-ci.md",
        "one `build/yew` executable");

    if (amended && build && install && closeout)
        return true;
    (void)snprintf(why, why_cap,
                   "Sprint 0 still requires a self-referential yew alias");
    return false;
}
