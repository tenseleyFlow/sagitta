/*
 * YEW-F-089 — the enforced 256-character-class cap has no firing test.
 *
 * Correct behavior: the unit suite accepts the exact 256-class boundary and
 * rejects class 257 with the documented message and an in-range offset.
 */
#include "audit.h"

#include <stdio.h>
#include <string.h>

bool test_yew_f_089(char *why, size_t why_cap)
{
    FILE *file = fopen("tests/unit/test_re_parse.c", "rb");
    char line[1024];

    if (file == NULL)
        return false;
    while (fgets(line, sizeof(line), file) != NULL) {
        if (strstr(line, "test_re_character_class_limit") != NULL) {
            (void)fclose(file);
            return true;
        }
    }
    (void)fclose(file);
    (void)snprintf(why, why_cap,
                   "no deterministic character-class limit regression");
    return false;
}
