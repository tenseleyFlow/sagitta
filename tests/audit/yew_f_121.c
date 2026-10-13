/*
 * YEW-F-121 — F13 contracts retain invalid clock/format/width scans.
 *
 * Correct behavior: tokenized call scans enforce injected clocks, structural
 * argv tests enforce byte-exact paths, and byte-length calls remain distinct
 * from Unicode display-width ownership.
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

bool test_yew_f_121(char *why, size_t why_cap)
{
    static const char *const s51 =
        ".docs/sprints/11-fuss-git/s51-git-layer.md";
    static const char *const s52 =
        ".docs/sprints/11-fuss-git/s52-fuss-mode.md";
    static const char *const closeout =
        ".docs/audits/closeout-F13-git.md";

    if (!file_has(".docs/plan/00-decisions.md",
                  "Git/FUSS ownership closeout amendment S59-A31") ||
        !file_has(s51, "tokenized call scan established by YEW-F-018") ||
        !file_has(s51, "structural argv control established by YEW-F-020") ||
        !file_has(s52, "byte lengths are permitted") ||
        !file_has(closeout, "tokenized clock-call scan") ||
        !file_has(closeout, "permits byte-length operations")) {
        (void)snprintf(why, why_cap,
                       "F13 contract retains invalid Git/FUSS scans");
        return false;
    }
    return true;
}
