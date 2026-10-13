/*
 * YEW-F-108 — F08 contracts require superseded Fletch staging guards.
 *
 * Correct behavior: a formal decision amendment, source sprint contracts,
 * and their closeout distinguish surfaces delivered by later owning sprints
 * from intentional boundaries that remain.
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

bool test_yew_f_108(char *why, size_t why_cap)
{
    static const char *const s30 =
        ".docs/sprints/06-fletch-core/s30-values-vm.md";
    static const char *const s31 =
        ".docs/sprints/06-fletch-core/s31-stdlib.md";
    static const char *const s32 =
        ".docs/sprints/06-fletch-core/s32-repl-and-errors.md";
    static const char *const s33 =
        ".docs/sprints/06-fletch-core/s33-fletch-conformance-perf.md";
    static const char *const s34 =
        ".docs/sprints/07-fletch-editor/s34-editor-api.md";
    static const char *const s36 =
        ".docs/sprints/07-fletch-editor/s36-config-system.md";
    static const char *const closeout =
        ".docs/audits/closeout-F08-fletch.md";

    if (!file_has(".docs/plan/00-decisions.md",
                  "Fletch closeout amendment S59-A19") ||
        file_has(s30, "`FL_OP_IMPORT` raises") ||
        file_has(s31, "`bind`, `set`, `on`, `buf`, `win` each raise") ||
        file_has(s32, "`yew --batch` → Sprint 37") ||
        file_has(s33, "`make test-fletch-roundtrip` hard-errors") ||
        file_has(s34, "Deferred surfaces hard-error naming their sprint") ||
        file_has(s36, "Differential test green: 500 generated states") ||
        file_has(s36, "Deferred surfaces hard-error naming their sprint") ||
        file_has(closeout, "`FL_OP_IMPORT` raises") ||
        file_has(closeout,
                 "`bind`, `set`, `on`, `buf`, `win` each raise") ||
        file_has(closeout, "`make test-fletch-roundtrip` hard-errors") ||
        file_has(closeout, "Differential test green: 500 generated states") ||
        !file_has(s30,
                  "Sprint 31 delivered import execution") ||
        !file_has(s31,
                  "Later owning sprints delivered the editor surfaces") ||
        !file_has(s36,
                  "Sprint 58 retired the legacy differential") ||
        !file_has(closeout,
                  "Later owning sprints delivered the staged Fletch surfaces")) {
        (void)snprintf(why, why_cap,
                       "F08 contract still requires superseded Fletch guards");
        return false;
    }
    return true;
}
