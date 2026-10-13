/*
 * YEW-F-135 — governance fixtures inherit global Git commit signing.
 * Correct behavior: every temporary repository that commits disables signing
 * locally, so a developer's Git policy cannot invoke pinentry or fail CI.
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

bool test_yew_f_135(char *why, size_t why_cap)
{
    bool priority = file_has(
        "scripts/tests/check-priority.test.sh",
        "config commit.gpgsign false");
    bool closeouts = file_has(
        "scripts/tests/check-closeouts.test.sh",
        "config commit.gpgsign false");

    if (priority && closeouts)
        return true;
    (void)snprintf(why, why_cap,
                   "governance fixtures inherit global Git signing");
    return false;
}
