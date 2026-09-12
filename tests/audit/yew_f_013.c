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

static bool fixture_marks_heuristic(const char *path)
{
    char bytes[8192];
    FILE *file = fopen(path, "rb");
    size_t len;

    if (file == NULL)
        return false;
    len = fread(bytes, 1U, sizeof(bytes) - 1U, file);
    if (ferror(file) || fclose(file) != 0)
        return false;
    bytes[len] = '\0';
    return strstr(bytes, "known-wrong") != NULL &&
           strstr(bytes, "value") != NULL &&
           strstr(bytes, "heuristic") != NULL;
}

bool test_yew_f_013(char *why, size_t why_cap)
{
    bool javascript = fixture_marks_heuristic(
        "tests/syn/javascript/01-kitchen.js");
    bool typescript = fixture_marks_heuristic(
        "tests/syn/javascript/10-kitchen.ts");

    if (!javascript || !typescript) {
        (void)snprintf(why, why_cap,
                       "fixture comments naming value-flag heuristic: js=%u ts=%u",
                       javascript ? 1U : 0U, typescript ? 1U : 0U);
    }
    return javascript && typescript;
}
