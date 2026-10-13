/*
 * YEW-F-099 — F02 contracts require superseded terminal staging behavior.
 *
 * Correct behavior: a formal decision amendment, source sprint contracts,
 * and their closeout distinguish historical staging guards from the current
 * terminal/renderer evidence supplied by later owning sprints.
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

bool test_yew_f_099(char *why, size_t why_cap)
{
    static const char *const s03 =
        ".docs/sprints/01-terminal/s03-tty-lifecycle.md";
    static const char *const s05 =
        ".docs/sprints/01-terminal/s05-grid-and-render.md";
    static const char *const s06 =
        ".docs/sprints/01-terminal/s06-pty-test-harness.md";
    static const char *const closeout =
        ".docs/audits/closeout-F02-terminal.md";

    if (!file_has(".docs/plan/00-decisions.md",
                  "Terminal closeout amendment S59-A10") ||
        file_has(s03, "exits 1 naming Sprint 37") ||
        file_has(closeout, "exits 1 naming Sprint 37") ||
        file_has(s05, "grep -rn '2026' src/") ||
        file_has(closeout, "grep -rn '2026' src/") ||
        file_has(s06, "contains exactly these keys") ||
        file_has(s06, "`yew <file>` still hard-errors") ||
        file_has(s06, "`yew foo.txt` under the pty: exit 1") ||
        file_has(closeout, "envp key set matches §2 exactly") ||
        !file_has(s06, "later owning sprints extend this set")) {
        (void)snprintf(why, why_cap,
                       "F02 contract still requires superseded staging behavior");
        return false;
    }
    return true;
}
