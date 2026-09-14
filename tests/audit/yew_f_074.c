/*
 * YEW-F-074 — Darwin shipping binaries are not reproducible.
 *
 * Correct behavior: the Darwin shipping link retains its required Mach-O
 * LC_UUID and ad-hoc signature while deriving both reproducibly.
 *
 * Regression: size-tools.test.sh exercises shipping and development profiles;
 * this audit keeps the production linker rule in inventory.  -no_uuid is not
 * a valid substitute because current dyld refuses to launch such a binary.
 */
#include "audit.h"

#include <stdio.h>
#include <string.h>

bool test_yew_f_074(char *why, size_t why_cap)
{
    char line[4096];
    FILE *file = fopen("Makefile", "rb");
    bool reproducible_link = false;

    if (file == NULL)
        return false;
    while (fgets(line, sizeof(line), file) != NULL)
        if (strstr(line, "LDFLAGS += -Wl,-reproducible") != NULL)
            reproducible_link = true;
    if (ferror(file) || fclose(file) != 0)
        return false;
    if (!reproducible_link)
        (void)snprintf(why, why_cap,
                       "Darwin shipping link omits reproducible mode");
    return reproducible_link;
}
