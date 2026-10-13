/*
 * YEW-F-123 — F14 names the wrong Fletch import amendment.
 *
 * Correct behavior: the Sprint 55 contract and F14 closeout require binding
 * Amendment A3, whose fourth import-resolution row owns shipped runtime files.
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

bool test_yew_f_123(char *why, size_t why_cap)
{
    static const char *const s55 =
        ".docs/sprints/12-plugins/s55-pkg-and-presets.md";
    static const char *const closeout =
        ".docs/audits/closeout-F14-plugins.md";

    if (!file_has(".docs/plan/00-decisions.md",
                  "Plugin import closeout amendment S59-A33") ||
        !file_has(s55, "Binding Amendment A3 is present") ||
        !file_has(closeout, "binding Amendment A3 is present")) {
        (void)snprintf(why, why_cap,
                       "F14 names A2 instead of runtime-import Amendment A3");
        return false;
    }
    return true;
}
