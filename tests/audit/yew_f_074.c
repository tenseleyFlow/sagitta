/*
 * YEW-F-074 — Darwin shipping binaries are not reproducible.
 *
 * Correct behavior: the Darwin shipping link disables the volatile Mach-O
 * LC_UUID before the stripped binary is hashed or signed.
 *
 * Baseline failure: every single-module profile produced different clean
 * rebuild hashes on arm64 macOS.  Stripping at the same path left only the
 * changing UUID and derived ad-hoc signature; adding -Wl,-no_uuid made the
 * stripped pair byte-identical.
 */
#include "audit.h"

#include <stdio.h>
#include <string.h>

bool test_yew_f_074(char *why, size_t why_cap)
{
    char line[4096];
    FILE *file = fopen("Makefile", "rb");
    bool disables_uuid = false;

    if (file == NULL)
        return false;
    while (fgets(line, sizeof(line), file) != NULL)
        if (strstr(line, "-no_uuid") != NULL)
            disables_uuid = true;
    if (ferror(file) || fclose(file) != 0)
        return false;
    if (!disables_uuid)
        (void)snprintf(why, why_cap,
                       "Darwin link retains volatile LC_UUID metadata");
    return disables_uuid;
}
