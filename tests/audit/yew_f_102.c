/*
 * YEW-F-102 — F04 contracts require superseded modal staging guards.
 *
 * Correct behavior: a formal decision amendment, source sprint contracts,
 * and their closeout distinguish historical hard errors from the current
 * modes, syntax provider, and multi-cursor expansion delivered by later
 * owning sprints, without erasing intentional 1.0 boundary guards.
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

bool test_yew_f_102(char *why, size_t why_cap)
{
    static const char *const s14 =
        ".docs/sprints/03-modal-core/s14-modes-L-I.md";
    static const char *const s16 =
        ".docs/sprints/03-modal-core/s16-modes-W-B.md";
    static const char *const s17 =
        ".docs/sprints/03-modal-core/s17-mode-H-multicursor.md";
    static const char *const closeout =
        ".docs/audits/closeout-F04-modal.md";

    if (!file_has(".docs/plan/00-decisions.md",
                  "Modal closeout amendment S59-A13") ||
        file_has(s14, "W/B/H/E/F messages its sprint number") ||
        file_has(closeout, "W/B/H/E/F messages its sprint number") ||
        file_has(s16, "has a NULL `enclosing`") ||
        file_has(closeout, "has a NULL `enclosing`") ||
        file_has(s16, "hard-errors naming Sprint 40") ||
        file_has(closeout, "hard-errors naming Sprint 40") ||
        file_has(s16, "multi-cursor expand hard-errors naming Sprint 17") ||
        file_has(closeout,
                 "multi-cursor expand hard-errors naming Sprint 17") ||
        !file_has(s14, "Later owning sprints delivered W/B/H/E/F") ||
        !file_has(s16, "Sprint 40 delivered the syntax provider") ||
        !file_has(s17, "Intentional 1.0 boundaries fail loudly") ||
        !file_has(closeout, "Intentional 1.0 boundaries fail loudly")) {
        (void)snprintf(why, why_cap,
                       "F04 contract still requires superseded modal guards");
        return false;
    }
    return true;
}
