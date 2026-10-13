/*
 * YEW-F-101 — F03 closeout retains obsolete mutation-ownership spellings.
 *
 * Correct behavior: a formal decision amendment, source sprint contracts,
 * and their closeout distinguish editable document mutation from initial
 * hydration/internal projections and name the private raw register setter.
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

bool test_yew_f_101(char *why, size_t why_cap)
{
    static const char *const s10 =
        ".docs/sprints/02-text-engine/s10-undo-tree.md";
    static const char *const s12 =
        ".docs/sprints/02-text-engine/s12-registers-clipboard.md";
    static const char *const closeout =
        ".docs/audits/closeout-F03-text.md";

    if (!file_has(".docs/plan/00-decisions.md",
                  "Text-engine ownership closeout amendment S59-A12") ||
        !file_has(s10, "initial hydration and internal scratch projections") ||
        !file_has(s12, "The raw storage setter is private") ||
        !file_has(closeout,
                  "initial hydration and internal scratch projections") ||
        !file_has(closeout, "private raw register setter")) {
        (void)snprintf(why, why_cap,
                       "F03 contract retains obsolete mutation ownership");
        return false;
    }
    return true;
}
