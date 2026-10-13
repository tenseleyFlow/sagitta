/*
 * YEW-F-113 — F10 contracts require superseded syntax staging/state gates.
 *
 * Correct behavior: source sprint contracts and closeout use the embedded
 * frame state delivered by Sprint 41.5 and test later delivered surfaces
 * instead of their historical landing guards.
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

bool test_yew_f_113(char *why, size_t why_cap)
{
    static const char *const s39 =
        ".docs/sprints/08-highlighting/s39-syntax-engine.md";
    static const char *const s40 =
        ".docs/sprints/08-highlighting/s40-syntax-def-format.md";
    static const char *const s41 =
        ".docs/sprints/08-highlighting/s41-langpack-1-themes.md";
    static const char *const s42 =
        ".docs/sprints/08-highlighting/s42-langpack-2.md";
    static const char *const closeout =
        ".docs/audits/closeout-F10-syntax.md";

    if (!file_has(".docs/plan/00-decisions.md",
                  "Syntax state closeout amendment S59-A24") ||
        !file_has(s39, "Sprint 41.5 superseded the 40-byte state tuple") ||
        !file_has(s39, "Sprints 40–42.5 delivered the staged syntax surfaces") ||
        !file_has(s40,
                  "Sprints 41–42.5 delivered the staged theme, embedding") ||
        !file_has(s41,
                  "Sprint 41.5 replaced the flat `SynState.def` invariant") ||
        !file_has(s42,
                  "Sprint 41.5 replaced the flat `SynState.def` invariant") ||
        !file_has(closeout,
                  "Later binding Sprints 41.5 and 42.5 supersede")) {
        (void)snprintf(why, why_cap,
                       "F10 contract still requires pre-embed staging/state gates");
        return false;
    }
    return true;
}
