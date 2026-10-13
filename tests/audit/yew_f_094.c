/*
 * YEW-F-094 — the regex golden-table guard permits fewer than 150 rows.
 *
 * Correct behavior: the executable floor preserves at least the contracted
 * 150 golden rows, rather than allowing the current table to erode to 130.
 */
#include "audit.h"

#include <stdio.h>
#include <string.h>

bool test_yew_f_094(char *why, size_t why_cap)
{
    FILE *file = fopen("tests/unit/test_re_match.c", "rb");
    char line[1024];

    if (file == NULL)
        return false;
    while (fgets(line, sizeof(line), file) != NULL) {
        if (strstr(line, "YEW_ARRAY_LEN(rows) >= 150U") != NULL) {
            (void)fclose(file);
            return true;
        }
    }
    (void)fclose(file);
    (void)snprintf(why, why_cap,
                   "regex golden-table floor remains below 150 rows");
    return false;
}
