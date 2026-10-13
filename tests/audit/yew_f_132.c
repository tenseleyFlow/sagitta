/*
 * YEW-F-132 — reserved --batch-strict promises a Sprint 59 delivery.
 * Correct behavior: keep the hard error but name the honest 1.0 boundary,
 * because Sprint 59 explicitly decided that the option is not for 1.0.
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

bool test_yew_f_132(char *why, size_t why_cap)
{
    bool amended = file_has(
        ".docs/plan/00-decisions.md",
        "Permanent non-goal diagnostic amendment S59-A40");
    bool product = file_has(
        "src/args.c",
        "option '%s' is not available in yew 1.0");
    bool unit = file_has(
        "tests/unit/test_args.c",
        "option '--batch-strict' is not available in yew 1.0");
    bool manual = file_has(
        "docs/batch.md",
        "`--batch-strict` is not available in yew 1.0");
    bool audit = file_has(
        ".docs/audits/audit-15-ci.md",
        "`--batch-strict` is not available in yew 1.0");

    if (amended && product && unit && manual && audit)
        return true;
    (void)snprintf(why, why_cap,
                   "--batch-strict still promises Sprint 59 delivery");
    return false;
}
