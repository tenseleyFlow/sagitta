/*
 * YEW-F-111 — F09 contracts require superseded recorder staging guards.
 *
 * Correct behavior: source sprint contracts and closeout distinguish later
 * delivered macro UX, Fletch highlighting, and package integration from the
 * permanent recorder boundaries.
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

bool test_yew_f_111(char *why, size_t why_cap)
{
    static const char *const s35 =
        ".docs/sprints/07-fletch-editor/s35-recorder-roundtrip.md";
    static const char *const s38 =
        ".docs/sprints/07-fletch-editor/s38-macro-ux.md";
    static const char *const closeout =
        ".docs/audits/closeout-F09-recorder.md";

    if (!file_has(".docs/plan/00-decisions.md",
                  "Recorder closeout amendment S59-A22") ||
        file_has(s35, "hard-error naming Sprint 38.") ||
        file_has(s38,
                 "deferred surfaces hard-error naming Sprints 41 and 55.") ||
        file_has(closeout, "hard-error naming Sprint 38.") ||
        file_has(closeout,
                 "deferred surfaces hard-error naming Sprints 41 and 55.") ||
        !file_has(s35,
                  "Sprint 38 delivered the browser, edit flow, and library") ||
        !file_has(s38,
                  "Sprints 41 and 55 delivered the deferred integrations") ||
        !file_has(closeout,
                  "Later owning sprints delivered the recorder-adjacent surfaces")) {
        (void)snprintf(why, why_cap,
                       "F09 contract still requires superseded recorder guards");
        return false;
    }
    return true;
}
