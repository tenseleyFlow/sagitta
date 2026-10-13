/*
 * YEW-F-119 — F12 contracts retain invalid AI ownership scans.
 *
 * Correct behavior: the TLS gate excludes implementations while permitting
 * the required error/routing surface, and the stats gate permits its public
 * API and lifecycle owner while excluding prompt and transport data paths.
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

bool test_yew_f_119(char *why, size_t why_cap)
{
    static const char *const s48 =
        ".docs/sprints/10-ai/s48-http-and-backends.md";
    static const char *const s49 =
        ".docs/sprints/10-ai/s49-ai-shadow.md";
    static const char *const closeout =
        ".docs/audits/closeout-F12-ai.md";

    if (!file_has(".docs/plan/00-decisions.md",
                  "AI ownership closeout amendment S59-A29") ||
        !file_has(s48,
                  "scans TLS implementation symbols") ||
        !file_has(s49,
                  "stats API and lifecycle owner are permitted") ||
        !file_has(closeout,
                  "Current closeout bans TLS implementation symbols") ||
        !file_has(closeout,
                  "stats API and lifecycle owner while excluding prompt")) {
        (void)snprintf(why, why_cap,
                       "F12 contract retains invalid AI ownership scans");
        return false;
    }
    return true;
}
