/*
 * YEW-F-122 — F14 retains superseded package/docs staging requirements.
 *
 * Correct behavior: closeout tests Sprint 55's shipped package commands,
 * F022's honest negative isolation disclaimer, and Sprint 59's assembled
 * plugin manual while retaining the permanent package non-goals.
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

bool test_yew_f_122(char *why, size_t why_cap)
{
    static const char *const s54 =
        ".docs/sprints/12-plugins/s54-plugin-system.md";
    static const char *const s55 =
        ".docs/sprints/12-plugins/s55-pkg-and-presets.md";
    static const char *const closeout =
        ".docs/audits/closeout-F14-plugins.md";

    if (!file_has(".docs/plan/00-decisions.md",
                  "Plugin closeout amendment S59-A32") ||
        !file_has(s54, "Sprint 55 delivered `yew pkg`") ||
        !file_has(s54, "negative `sandbox` disclaimer") ||
        !file_has(s55, "Sprint 59 delivered the assembled plugin man pages") ||
        !file_has(closeout,
                  "Sprint 55 supersedes the original package staging guard") ||
        !file_has(closeout, "negative `sandbox` disclaimer") ||
        !file_has(closeout, "Sprint 59's assembled plugin man page")) {
        (void)snprintf(why, why_cap,
                       "F14 retains superseded package/docs staging rules");
        return false;
    }
    return true;
}
