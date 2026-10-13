/*
 * YEW-F-114 — F10 contracts retain historical syntax ownership/inventories.
 *
 * Correct behavior: the color-ownership gate excludes the theme loader, the
 * original 17-definition cohort remains historical, and current closeout
 * follows Sprint 42.5's checked 48-definition manifest.
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

bool test_yew_f_114(char *why, size_t why_cap)
{
    static const char *const s39 =
        ".docs/sprints/08-highlighting/s39-syntax-engine.md";
    static const char *const s42 =
        ".docs/sprints/08-highlighting/s42-langpack-2.md";
    static const char *const closeout =
        ".docs/audits/closeout-F10-syntax.md";

    if (!file_has(".docs/plan/00-decisions.md",
                  "Syntax ownership and inventory closeout amendment S59-A25") ||
        !file_has(s39,
                  "Theme loading is the syntax subsystem's color-owning exception") ||
        !file_has(s42,
                  "The original 17-definition / 14-language cohort remains") ||
        !file_has(closeout,
                  "Current closeout uses the theme-excluding syntax color ban") ||
        !file_has(closeout,
                  "Sprint 42.5's manifest owns the current 48-definition inventory")) {
        (void)snprintf(why, why_cap,
                       "F10 contract retains historical ownership/inventory gates");
        return false;
    }
    return true;
}
