/*
 * YEW-F-134 — plugin containment test assumes /etc/passwd is canonical.
 * Correct behavior: compare the diagnostic with a resolved fixture-owned
 * escape target so libc feature macros and FHS aliases are irrelevant.
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

bool test_yew_f_134(char *why, size_t why_cap)
{
    bool resolves = file_has(
        "tests/unit/test_plug_manifest.c",
        "yew_xrealpath(outside)");
    bool compares = file_has(
        "tests/unit/test_plug_manifest.c",
        "strstr(f.diag, resolved)");
    bool contained = !file_has(
        "tests/unit/test_plug_manifest.c",
        "symlink(\"/etc/passwd\"");

    if (resolves && compares && contained)
        return true;
    (void)snprintf(why, why_cap,
                   "plugin containment unit assumes a canonical FHS path");
    return false;
}
