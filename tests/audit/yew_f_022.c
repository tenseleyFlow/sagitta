/*
 * YEW-F-022 — plugin trust wording gate rejects its required warning.
 *
 * Correct behavior: Sprint 54 section 7 requires the user-facing guide to
 * quote plug.h's warning verbatim.  The sole "sandbox" mention must negate
 * that claim while both copies plainly deny memory and resource isolation.
 *
 * Baseline failure: the author guide quotes plug.h verbatim, including the
 * forbidden word.  The warning is honest; the literal release gate and the
 * quote-verbatim requirement cannot both pass.
 */
#include "audit.h"

#include <stdio.h>
#include <string.h>

static bool file_occurrences(const char *path, const char *needle,
                             size_t *count)
{
    char line[4096];
    FILE *file = fopen(path, "rb");
    size_t needle_len = strlen(needle);

    if (file == NULL || needle_len == 0U)
        return false;
    *count = 0U;
    while (fgets(line, sizeof(line), file) != NULL) {
        const char *at = line;

        while ((at = strstr(at, needle)) != NULL) {
            (*count)++;
            at += needle_len;
        }
    }
    if (ferror(file) || fclose(file) != 0)
        return false;
    return true;
}

bool test_yew_f_022(char *why, size_t why_cap)
{
    size_t guide_sandbox;
    size_t guide_negative;
    size_t guide_memory;
    size_t guide_resource;
    size_t header_memory;
    size_t header_resource;
    bool valid;

    if (!file_occurrences("docs/plugins-authoring.md", "sandbox",
                          &guide_sandbox) ||
        !file_occurrences("docs/plugins-authoring.md",
                          "they do not create a sandbox", &guide_negative) ||
        !file_occurrences("docs/plugins-authoring.md", "no memory isolation",
                          &guide_memory) ||
        !file_occurrences("docs/plugins-authoring.md", "no resource",
                          &guide_resource) ||
        !file_occurrences("src/mod/plug/plug.h", "no memory isolation",
                          &header_memory) ||
        !file_occurrences("src/mod/plug/plug.h", "no resource",
                          &header_resource))
        return false;
    valid = guide_sandbox == 1U && guide_negative == 1U &&
            guide_memory == 1U && guide_resource == 1U &&
            header_memory == 1U && header_resource == 1U;
    if (!valid)
        (void)snprintf(why, why_cap,
                       "guide sandbox=%u negative=%u memory=%u resource=%u; "
                       "header memory=%u resource=%u",
                       (unsigned)guide_sandbox, (unsigned)guide_negative,
                       (unsigned)guide_memory, (unsigned)guide_resource,
                       (unsigned)header_memory, (unsigned)header_resource);
    return valid;
}
