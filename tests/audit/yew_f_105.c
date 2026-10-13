/*
 * YEW-F-105 — F05 contracts retain impossible Unicode/pipe grep gates.
 *
 * Correct behavior: a formal decision amendment, source sprint contracts,
 * and their closeout test ownership: Unicode conversion stays centralized,
 * and all pipe creation uses the one portable close-on-exec helper.
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

bool test_yew_f_105(char *why, size_t why_cap)
{
    static const char *const s18 =
        ".docs/sprints/04-execute-search/s18-cmdline.md";
    static const char *const s19 =
        ".docs/sprints/04-execute-search/s19-shell-execution.md";
    static const char *const closeout =
        ".docs/audits/closeout-F05-exec.md";

    if (!file_has(".docs/plan/00-decisions.md",
                  "Execute ownership closeout amendment S59-A16") ||
        file_has(s18, "grep -rn 'utf8_\\|grapheme\\|width\\|wcwidth'") ||
        file_has(closeout,
                 "grep -rn 'utf8_\\|grapheme\\|width\\|wcwidth'") ||
        file_has(s19, "grep -rn 'pipe('") ||
        file_has(closeout, "grep -rn 'pipe('") ||
        !file_has(s18,
                  "Unicode measurement and coordinate conversion remain centralized") ||
        !file_has(closeout,
                  "Unicode measurement and coordinate conversion remain centralized") ||
        !file_has(s19, "All production pipe creation routes through") ||
        !file_has(s19, "portable macOS fallback") ||
        !file_has(closeout, "All production pipe creation routes through")) {
        (void)snprintf(why, why_cap,
                       "F05 contract retains impossible Unicode/pipe greps");
        return false;
    }
    return true;
}
