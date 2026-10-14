/*
 * YEW-F-139 — audit fixture path formatting trips GCC sanitizer restrict.
 * Correct behavior: a sanitizer-profile warning-clean build must not format
 * one F007Files subobject from another subobject of the same aggregate.
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

bool test_yew_f_139(char *why, size_t why_cap)
{
    bool same_aggregate_format = file_has(
        "tests/audit/yew_f_007.c",
        "\"%s/f%d.txt\", files->dir, i);");

    if (!same_aggregate_format)
        return true;
    (void)snprintf(why, why_cap,
                   "F007 path format aliases one aggregate under GCC");
    return false;
}
