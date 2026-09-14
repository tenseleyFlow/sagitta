/*
 * YEW-F-013 — JS/TS known-wrong golden rows lost their explanation.
 *
 * Correct behavior: Sprint 42 and Sprint 58 F10 require the JavaScript and
 * TypeScript fixture rows for the `)`/`}` value-flag failures to carry a
 * comment naming the value-flag heuristic.  A future highlighting fix then
 * presents as the intentional removal of a documented known-wrong row.
 *
 * Baseline failure: the rows contain identifiers named `knownWrong`, but
 * neither fixture has a comment naming the heuristic.  The definition and a
 * unit-test function describe it elsewhere; the golden input itself does not.
 */
#include "audit.h"

#include <stdio.h>
#include <string.h>

static bool fixture_marks_heuristic(const char *path, size_t expected_rows)
{
    char bytes[8192];
    FILE *file = fopen(path, "rb");
    size_t len;
    size_t marked = 0U;
    char *line;

    if (file == NULL)
        return false;
    len = fread(bytes, 1U, sizeof(bytes) - 1U, file);
    if (ferror(file) || fclose(file) != 0)
        return false;
    bytes[len] = '\0';
    line = bytes;
    while (*line != '\0') {
        char *end = strchr(line, '\n');
        char saved = end == NULL ? '\0' : *end;
        const char *comment;

        if (end != NULL)
            *end = '\0';
        if (strstr(line, "/knownWrong") != NULL) {
            comment = strstr(line, "//");
            if (comment == NULL || strstr(comment, "YEW-F-013") == NULL ||
                strstr(comment, "known-wrong") == NULL ||
                strstr(comment, "value-flag") == NULL ||
                strstr(comment, "heuristic") == NULL)
                return false;
            marked++;
        }
        if (end == NULL)
            break;
        *end = saved;
        line = end + 1;
    }
    return marked == expected_rows;
}

bool test_yew_f_013(char *why, size_t why_cap)
{
    bool javascript = fixture_marks_heuristic(
        "tests/syn/javascript/01-kitchen.js", 2U);
    bool typescript = fixture_marks_heuristic(
        "tests/syn/javascript/10-kitchen.ts", 1U);

    if (!javascript || !typescript) {
        (void)snprintf(why, why_cap,
                       "fixture comments naming value-flag heuristic: js=%u ts=%u",
                       javascript ? 1U : 0U, typescript ? 1U : 0U);
    }
    return javascript && typescript;
}
