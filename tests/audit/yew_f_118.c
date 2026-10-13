/*
 * YEW-F-118 — F12 contracts require superseded AI staging guards.
 *
 * Correct behavior: source sprint contracts and closeout preserve the
 * historical handoffs while testing Sprint 50's shipped enable, stats,
 * privacy, preset, and opt-in surfaces plus the permanent 1.0 non-goals.
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

bool test_yew_f_118(char *why, size_t why_cap)
{
    static const char *const s48 =
        ".docs/sprints/10-ai/s48-http-and-backends.md";
    static const char *const s49 =
        ".docs/sprints/10-ai/s49-ai-shadow.md";
    static const char *const closeout =
        ".docs/audits/closeout-F12-ai.md";

    if (!file_has(".docs/plan/00-decisions.md",
                  "AI closeout amendment S59-A28") ||
        !file_has(s48,
                  "Sprint 50 delivered the staged AI command surfaces") ||
        !file_has(s49,
                  "Sprint 50 delivered the opt-in and privacy surfaces") ||
        !file_has(closeout,
                  "Later binding Sprint 50 supersedes the original AI staging guards")) {
        (void)snprintf(why, why_cap,
                       "F12 contract still requires pre-delivery AI guards");
        return false;
    }
    return true;
}
