/*
 * YEW-F-022 — plugin trust wording gate rejects its required warning.
 *
 * Correct behavior: Sprint 54 section 7 and Sprint 58 F14 q6 require the
 * user-facing plugin guide to contain no "sandbox" wording while plug.h
 * must plainly state that plugins have no memory or resource isolation.
 *
 * Baseline failure: the author guide quotes plug.h verbatim, including the
 * forbidden word.  The warning is honest; the literal release gate and the
 * quote-verbatim requirement cannot both pass.
 */
#include "audit.h"

#include <stdio.h>
#include <string.h>

static bool file_contains(const char *path, const char *needle, bool *found)
{
    char line[4096];
    FILE *file = fopen(path, "rb");

    if (file == NULL)
        return false;
    *found = false;
    while (fgets(line, sizeof(line), file) != NULL) {
        if (strstr(line, needle) != NULL)
            *found = true;
    }
    if (ferror(file) || fclose(file) != 0)
        return false;
    return true;
}

bool test_yew_f_022(char *why, size_t why_cap)
{
    bool user_sandbox;
    bool memory_warning;
    bool resource_warning;

    if (!file_contains("docs/plugins-authoring.md", "sandbox",
                       &user_sandbox) ||
        !file_contains("src/mod/plug/plug.h", "no memory isolation",
                       &memory_warning) ||
        !file_contains("src/mod/plug/plug.h", "no resource",
                       &resource_warning))
        return false;
    if (user_sandbox || !memory_warning || !resource_warning)
        (void)snprintf(why, why_cap,
                       "user sandbox=%u; plug.h memory=%u resource=%u",
                       user_sandbox ? 1U : 0U, memory_warning ? 1U : 0U,
                       resource_warning ? 1U : 0U);
    return !user_sandbox && memory_warning && resource_warning;
}
