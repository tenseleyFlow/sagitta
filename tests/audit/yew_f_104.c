/*
 * YEW-F-104 — F05 contracts require superseded execute staging guards.
 *
 * Correct behavior: a formal decision amendment, source sprint contracts,
 * and their closeout distinguish surfaces delivered by later owning sprints
 * from the :g and terminal-emulator permanent non-goals.
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

bool test_yew_f_104(char *why, size_t why_cap)
{
    static const char *const s18 =
        ".docs/sprints/04-execute-search/s18-cmdline.md";
    static const char *const s19 =
        ".docs/sprints/04-execute-search/s19-shell-execution.md";
    static const char *const closeout =
        ".docs/audits/closeout-F05-exec.md";

    if (!file_has(".docs/plan/00-decisions.md",
                  "Execute closeout amendment S59-A15") ||
        file_has(s18, "Deferred surfaces (`:!`, `:s`, `:g`, `:fl`") ||
        file_has(closeout,
                 "Deferred surfaces (`:!`, `:s`, `:g`, `:fl`") ||
        file_has(s19, "`:s`, `/re/` still hard-error naming Sprints") ||
        file_has(closeout, "`:s` and `/re/` still name Sprints 20/21") ||
        !file_has(s18, "Later owning sprints delivered `:!`, `:s`, `:fl`") ||
        !file_has(s18, "`:g` remains a permanent non-goal") ||
        !file_has(s19, "Sprint 21 delivered `:s` and `/re/`") ||
        !file_has(s19, "`ed.shell.term` still rejects terminal emulation") ||
        !file_has(closeout,
                  "Later owning sprints delivered the execute surfaces")) {
        (void)snprintf(why, why_cap,
                       "F05 contract still requires superseded execute guards");
        return false;
    }
    return true;
}
