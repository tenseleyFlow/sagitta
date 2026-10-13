/*
 * YEW-F-126 — Sprint 54's stripped-plugin diagnostic diverges from the
 * shared module boundary that owns every excisable feature.
 *
 * Correct behavior: the binding decision, source sprint, and F14 closeout
 * identify yew_mod_require's current clear diagnostic as the byte-exact
 * stripped-plugin contract.
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

bool test_yew_f_126(char *why, size_t why_cap)
{
    bool amended = file_has(
        ".docs/plan/00-decisions.md",
        "Plugin module-diagnostic closeout amendment S59-A34");
    bool sprint_owns_shared = file_has(
        ".docs/sprints/12-plugins/s54-plugin-system.md",
        "this build has no plugins module; rebuild with");
    bool closeout_owns_shared = file_has(
        ".docs/audits/closeout-F14-plugins.md",
        "shared module diagnostic byte-exact");

    if (amended && sprint_owns_shared && closeout_owns_shared)
        return true;
    (void)snprintf(why, why_cap,
                   "Sprint 54 retains a divergent stripped-plugin message");
    return false;
}
