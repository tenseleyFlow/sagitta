/*
 * YEW-F-103 — F04 contracts retain obsolete viewport/width grep gates.
 *
 * Correct behavior: a formal decision amendment, source sprint contracts,
 * and their closeout distinguish later viewport consumers from ownership of
 * viewport algorithms, and Unicode measurement from ordinary cell geometry.
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

bool test_yew_f_103(char *why, size_t why_cap)
{
    static const char *const s14 =
        ".docs/sprints/03-modal-core/s14-modes-L-I.md";
    static const char *const s15 =
        ".docs/sprints/03-modal-core/s15-viewport-statusline.md";
    static const char *const closeout =
        ".docs/audits/closeout-F04-modal.md";

    if (!file_has(".docs/plan/00-decisions.md",
                  "Modal ownership closeout amendment S59-A14") ||
        file_has(s14, "grep -rn 'vp\\.top\\|vp->top'") ||
        file_has(closeout, "grep -rn 'vp\\.top\\|vp->top'") ||
        file_has(s15, "grep -rn 'wcwidth\\|cells\\|width'") ||
        file_has(closeout, "grep -rn 'wcwidth\\|cells\\|width'") ||
        !file_has(s14, "Later owners legitimately consume viewport state") ||
        !file_has(s15,
                  "Unicode measurement and coordinate conversion remain centralized") ||
        !file_has(closeout,
                  "Later owners legitimately consume viewport state") ||
        !file_has(closeout,
                  "Unicode measurement and coordinate conversion remain centralized")) {
        (void)snprintf(why, why_cap,
                       "F04 contract retains obsolete viewport/width greps");
        return false;
    }
    return true;
}
