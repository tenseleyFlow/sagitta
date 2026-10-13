/*
 * YEW-F-120 — F13 contracts require superseded Git/FUSS staging and keys.
 *
 * Correct behavior: closeout tests Sprint 52/53's shipped commands and the
 * binding compact-FUSS action/type-jump model while retaining permanent
 * conflict-resolution and sub-hunk non-goals.
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

bool test_yew_f_120(char *why, size_t why_cap)
{
    static const char *const s51 =
        ".docs/sprints/11-fuss-git/s51-git-layer.md";
    static const char *const s52 =
        ".docs/sprints/11-fuss-git/s52-fuss-mode.md";
    static const char *const closeout =
        ".docs/audits/closeout-F13-git.md";

    if (!file_has(".docs/plan/00-decisions.md",
                  "Git/FUSS closeout amendment S59-A30") ||
        !file_has(s51, "Sprints 52 and 53 delivered the staged commands") ||
        !file_has(s52, "binding compact-FUSS action model") ||
        !file_has(s52, "Sprint 53 delivered directory groups") ||
        !file_has(closeout,
                  "Later binding Sprints 52, 53, and 57.5 supersede")) {
        (void)snprintf(why, why_cap,
                       "F13 contract retains superseded staging/key rules");
        return false;
    }
    return true;
}
