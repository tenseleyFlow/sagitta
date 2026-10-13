/*
 * YEW-F-125 — a mixed Fletch API unit links a FUSS-only test seam when the
 * FUSS module is stripped.
 *
 * Correct behavior: the core query assertions remain in every unit build,
 * while the synthetic Git snapshot and active-FUSS assertions are compiled
 * only when YEW_WITH_FUSS is true.
 */
#include "audit.h"

#include <stdio.h>
#include <string.h>

bool test_yew_f_125(char *why, size_t why_cap)
{
    FILE *file = fopen("tests/unit/test_fl_api_complete.c", "rb");
    char line[2048];
    bool in_test = false;
    bool guarded = false;
    bool saw_seam = false;

    if (file == NULL) {
        (void)snprintf(why, why_cap, "cannot read Fletch API unit source");
        return false;
    }
    while (fgets(line, sizeof(line), file) != NULL) {
        if (strstr(line,
                   "test_fl_api_tutor_state_queries_report_mode_and_undo_depth")
            != NULL)
            in_test = true;
        if (!in_test)
            continue;
        if (strstr(line, "#if YEW_WITH_FUSS") != NULL)
            guarded = true;
        if (strstr(line, "yew_git_test_snapshot_mut") != NULL) {
            saw_seam = true;
            if (!guarded) {
                (void)fclose(file);
                (void)snprintf(
                    why, why_cap,
                    "Fletch API unit links FUSS seam outside module guard");
                return false;
            }
        }
        if (strstr(line, "#endif /* YEW_WITH_FUSS */") != NULL)
            guarded = false;
        if (saw_seam && strstr(line, "api_close(&f);") != NULL)
            break;
    }
    (void)fclose(file);
    if (saw_seam)
        return true;
    (void)snprintf(why, why_cap, "Fletch API FUSS witness is missing");
    return false;
}
