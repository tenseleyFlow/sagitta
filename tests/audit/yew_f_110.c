/*
 * YEW-F-110 — F08 contracts treat historical inventories as current totals.
 *
 * Correct behavior: closeout preserves each sprint's freeze-time snapshot
 * while testing the amended/current kind and native inventories separately.
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

bool test_yew_f_110(char *why, size_t why_cap)
{
    static const char *const s28 =
        ".docs/sprints/06-fletch-core/s28-fletch-design-freeze.md";
    static const char *const s30 =
        ".docs/sprints/06-fletch-core/s30-values-vm.md";
    static const char *const s31 =
        ".docs/sprints/06-fletch-core/s31-stdlib.md";
    static const char *const s33 =
        ".docs/sprints/06-fletch-core/s33-fletch-conformance-perf.md";
    static const char *const closeout =
        ".docs/audits/closeout-F08-fletch.md";

    if (!file_has(".docs/plan/00-decisions.md",
                  "Fletch inventory closeout amendment S59-A21") ||
        file_has(s28, "§16 empty.") ||
        file_has(s28, "Closed error-kind set (11 kinds)") ||
        file_has(s30, "§9's stated kind count reads 12") ||
        file_has(s31, "prints **117** names in deterministic order") ||
        file_has(s33, "kinds 12/12") ||
        file_has(s33, "12 error kinds") ||
        file_has(closeout, "§16 empty.") ||
        file_has(closeout, "Closed error-kind set (11 kinds)") ||
        file_has(closeout, "§9's stated kind count reads 12") ||
        file_has(closeout, "prints **117** names in deterministic order") ||
        file_has(closeout, "kinds 12/12") ||
        file_has(closeout, "12 error kinds") ||
        !file_has(s28,
                  "Freeze-time snapshot was 11 kinds with an empty §16") ||
        !file_has(s31,
                  "Sprint 31 core modules contribute exactly 117") ||
        !file_has(s33,
                  "current amended inventory is 13 kinds") ||
        !file_has(closeout,
                  "Freeze-time and current Fletch inventories are distinguished")) {
        (void)snprintf(why, why_cap,
                       "F08 contract treats historical inventories as current");
        return false;
    }
    return true;
}
