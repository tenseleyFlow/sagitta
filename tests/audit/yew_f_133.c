/*
 * YEW-F-133 — the theme unit escapes a disjoint BUILD tree.
 * Correct behavior: temporary test data lives under the runner's TMPDIR,
 * which UNIT_RUN pins to the selected BUILD directory.
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

bool test_yew_f_133(char *why, size_t why_cap)
{
    bool uses_tmp = file_has(
        "tests/unit/test_theme.c",
        "%s/yew-theme-oversize.XXXXXX");
    bool stale = file_has(
        "tests/unit/test_theme.c",
        "build/theme-oversize.XXXXXX");

    if (uses_tmp && !stale)
        return true;
    (void)snprintf(why, why_cap,
                   "theme unit writes outside the selected BUILD tree");
    return false;
}
