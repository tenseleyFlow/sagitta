/*
 * YEW-F-112 — F09 contracts require an unreachable macrolib collision.
 *
 * Correct behavior: closeout proves the canonical namespace is injective and
 * reviews the retained defensive rejection path; it does not require a
 * filesystem fixture that the namespace rules make impossible.
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

bool test_yew_f_112(char *why, size_t why_cap)
{
    static const char *const s38 =
        ".docs/sprints/07-fletch-editor/s38-macro-ux.md";
    static const char *const closeout =
        ".docs/audits/closeout-F09-recorder.md";

    if (!file_has(".docs/plan/00-decisions.md",
                  "Macrolib namespace closeout amendment S59-A23") ||
        file_has(s38, "all eight naming/collision rows tested") ||
        file_has(closeout, "all eight naming/collision rows tested") ||
        !file_has(s38, "The seven reachable namespace rows are tested") ||
        !file_has(closeout,
                  "Namespace injectivity replaces the impossible collision fixture")) {
        (void)snprintf(why, why_cap,
                       "F09 contract requires unreachable macrolib collision");
        return false;
    }
    return true;
}
