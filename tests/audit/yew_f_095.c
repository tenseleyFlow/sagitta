/*
 * YEW-F-095 — the F01 closeout requires delivered deferrals to be absent.
 *
 * Correct behavior: a formal decision amendment, the Sprint 2 contract, and
 * the F01 closeout distinguish the historical Sprint 2 staging boundary from
 * the word-break and simple-fold surfaces later delivered by Sprints 16/20.
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

bool test_yew_f_095(char *why, size_t why_cap)
{
    static const char *const s02 =
        ".docs/sprints/00-scaffolding/s02-unicode-foundation.md";
    static const char *const closeout =
        ".docs/audits/closeout-F01-unicode.md";

    if (!file_has("src/unicode/wordbreak.h", "YewWb yew_wb_prop") ||
        !file_has("src/search/regex_internal.h",
                  "yew_re_fold_partners")) {
        (void)snprintf(why, why_cap,
                       "later Unicode surfaces are not present to review");
        return false;
    }
    if (file_has(s02, "no `yew_wb_*`") ||
        file_has(closeout, "no `yew_wb_*`") ||
        !file_has(".docs/plan/00-decisions.md",
                  "Sprint 2 closeout amendment")) {
        (void)snprintf(why, why_cap,
                       "F01 still requires delivered deferrals to be absent");
        return false;
    }
    return true;
}
