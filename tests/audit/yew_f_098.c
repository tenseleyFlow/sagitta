/*
 * YEW-F-098 — Sprint 2's Unicode unit-test floor was never reached.
 *
 * Correct behavior: the four Sprint 2 unit sources define and explicitly
 * register at least 45 tests, as required by s02 DoD 14.
 *
 * Baseline failure: the files define 32 tests (the original Sprint 2 landing
 * had 30).  Their runtime assertion count is strong, but the named diagnostic
 * granularity promised by the contract is absent.
 */
#include "audit.h"

#include <stdio.h>
#include <string.h>

static bool registry_has(const char *name)
{
    FILE *file = fopen("tests/unit/registry.c", "rb");
    char line[512];
    char needle[192];

    if (file == NULL)
        return false;
    (void)snprintf(needle, sizeof(needle), "T(%s)", name);
    while (fgets(line, sizeof(line), file) != NULL) {
        if (strstr(line, needle) != NULL) {
            (void)fclose(file);
            return true;
        }
    }
    (void)fclose(file);
    return false;
}

static bool count_file(const char *path, size_t *count)
{
    FILE *file = fopen(path, "rb");
    char line[512];

    if (file == NULL)
        return false;
    while (fgets(line, sizeof(line), file) != NULL) {
        char name[160];

        if (sscanf(line, "void test_%159[a-zA-Z0-9_]", name) == 1) {
            if (!registry_has(name)) {
                (void)fclose(file);
                return false;
            }
            (*count)++;
        }
    }
    (void)fclose(file);
    return true;
}

bool test_yew_f_098(char *why, size_t why_cap)
{
    static const char *const files[] = {
        "tests/unit/test_utf8.c",
        "tests/unit/test_grapheme.c",
        "tests/unit/test_width.c",
        "tests/unit/test_gbtest.c"
    };
    size_t count = 0u;
    size_t i;

    for (i = 0u; i < sizeof(files) / sizeof(files[0]); i++) {
        if (!count_file(files[i], &count)) {
            (void)snprintf(why, why_cap,
                           "Unicode test definition is missing or unregistered");
            return false;
        }
    }
    if (count < 45u) {
        (void)snprintf(why, why_cap,
                       "four Unicode unit files register %lu of 45 tests",
                       (unsigned long)count);
        return false;
    }
    return true;
}
