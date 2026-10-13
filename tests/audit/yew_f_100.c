/*
 * YEW-F-100 — F03 contracts require superseded text-engine staging guards.
 *
 * Correct behavior: a formal decision amendment, source sprint contracts,
 * and their closeout distinguish historical hard errors from the current
 * multi-cursor, undo-reason, and register surfaces delivered by later
 * owning sprints.
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

bool test_yew_f_100(char *why, size_t why_cap)
{
    static const char *const s09 =
        ".docs/sprints/02-text-engine/s09-cursors-and-coords.md";
    static const char *const s10 =
        ".docs/sprints/02-text-engine/s10-undo-tree.md";
    static const char *const s12 =
        ".docs/sprints/02-text-engine/s12-registers-clipboard.md";
    static const char *const closeout =
        ".docs/audits/closeout-F03-text.md";

    if (!file_has(".docs/plan/00-decisions.md",
                  "Text-engine closeout amendment S59-A11") ||
        file_has(s09, "Multi-cursor editing entry points hard-error") ||
        file_has(closeout,
                 "Multi-cursor editing entry points hard-error") ||
        file_has(s10, "hard-error gates in item 11") ||
        file_has(closeout, "hard-error gates in item 11") ||
        file_has(s10, "Deferred reasons hard-error naming their sprint") ||
        file_has(closeout,
                 "Deferred reasons hard-error naming their sprint") ||
        file_has(s12, "Blockwise production outside `tests/` hard-errors") ||
        file_has(closeout,
                 "Blockwise production outside `tests/` hard-errors") ||
        file_has(s12, "`/`, `:`, `#` read empty") ||
        file_has(closeout, "`/`, `:`, `#` read empty") ||
        !file_has(s09, "Sprint 17 delivered multi-cursor editing") ||
        !file_has(s10, "later owning sprints replace these gates") ||
        !file_has(s12,
                  "delivered register producers replace these staging guards")) {
        (void)snprintf(why, why_cap,
                       "F03 contract still requires superseded staging guards");
        return false;
    }
    return true;
}
