/*
 * YEW-F-141 — the sanitizer profile continues after UBSan diagnostics.
 * Correct behavior: SAN=1 exports a UBSan policy that stops at the first
 * runtime violation, so a diagnostic cannot be followed by a green target.
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

bool test_yew_f_141(char *why, size_t why_cap)
{
    bool policy = file_has("Makefile", "UBSAN_OPTIONS");
    bool halt = file_has("Makefile", "halt_on_error=1");

    if (policy && halt)
        return true;
    (void)snprintf(why, why_cap,
                   "SAN=1 does not make UBSan diagnostics fatal");
    return false;
}
