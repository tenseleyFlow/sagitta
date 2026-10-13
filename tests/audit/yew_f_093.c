/*
 * YEW-F-093 — restored F06 contracts describe superseded mechanisms.
 *
 * Correct behavior: a formal decision amendment, source sprint contracts,
 * and their closeout all describe the mechanisms the shipped engine uses.
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

bool test_yew_f_093(char *why, size_t why_cap)
{
    static const char *const s20 =
        ".docs/sprints/04-execute-search/s20-regex-engine.md";
    static const char *const s21 =
        ".docs/sprints/04-execute-search/s21-search-replace.md";
    static const char *const closeout = ".docs/audits/closeout-F06-regex.md";

    if (file_has(s20, "yew_re_window_fill") ||
        file_has(s20, "sparse_has") ||
        file_has(s20, "forward+reverse DFA spans") ||
        file_has(closeout, "yew_re_window_fill") ||
        file_has(closeout, "sparse_has") ||
        file_has(closeout, "forward+reverse DFA spans") ||
        file_has(s21, "`:g` hard-errors naming Sprint 34") ||
        file_has(closeout, "`:g` hard-errors naming Sprint 34") ||
        !file_has(".docs/plan/00-decisions.md",
                  "Sprint 20 conformance amendment")) {
        (void)snprintf(why, why_cap,
                       "F06 contract still names superseded mechanisms");
        return false;
    }
    return true;
}
