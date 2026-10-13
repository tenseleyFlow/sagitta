/*
 * YEW-F-109 — F08 contracts retain historical and impossible ownership gates.
 *
 * Correct behavior: closeout checks the parser arena boundary and Fletch GC
 * object ownership without rejecting later host-side Fletch modules that use
 * the project's checked allocation wrappers.
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

bool test_yew_f_109(char *why, size_t why_cap)
{
    static const char *const s29 =
        ".docs/sprints/06-fletch-core/s29-lexer-parser.md";
    static const char *const s30 =
        ".docs/sprints/06-fletch-core/s30-values-vm.md";
    static const char *const closeout =
        ".docs/audits/closeout-F08-fletch.md";

    if (!file_has(".docs/plan/00-decisions.md",
                  "Fletch ownership closeout amendment S59-A20") ||
        file_has(s29, "no directory other than `src/fl/`") ||
        file_has(s29, "grep -n 'malloc\\|calloc' src/fl/") ||
        file_has(s30,
                 "grep -rn 'malloc\\|calloc\\|realloc' src/fl/") ||
        file_has(closeout, "no directory other than `src/fl/`") ||
        file_has(closeout, "grep -n 'malloc\\|calloc' src/fl/") ||
        file_has(closeout,
                 "grep -rn 'malloc\\|calloc\\|realloc' src/fl/") ||
        !file_has(s29,
                  "Lexer and parser allocation remains caller-arena-owned") ||
        !file_has(s30,
                  "Every `FlObj` allocation remains GC-owned") ||
        !file_has(closeout,
                  "Lexer and parser allocation remains caller-arena-owned") ||
        !file_has(closeout,
                  "Every `FlObj` allocation remains GC-owned")) {
        (void)snprintf(why, why_cap,
                       "F08 contract retains historical ownership gates");
        return false;
    }
    return true;
}
