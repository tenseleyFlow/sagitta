/*
 * YEW-F-107 — F07 contracts retain impossible ownership token greps.
 *
 * Correct behavior: closeout gates test the actual ownership boundaries:
 * Unicode measurement stays centralized, workspace code delegates atomic
 * replacement, and one event-loop branch dispatches mouse events.
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

bool test_yew_f_107(char *why, size_t why_cap)
{
    static const char *const s22 =
        ".docs/sprints/05-ui-workspace/s22-layout-panes.md";
    static const char *const s25 =
        ".docs/sprints/05-ui-workspace/s25-workspace-state.md";
    static const char *const s27 =
        ".docs/sprints/05-ui-workspace/s27-mouse-and-feel.md";
    static const char *const closeout =
        ".docs/audits/closeout-F07-ui.md";

    if (!file_has(".docs/plan/00-decisions.md",
                  "UI/workspace ownership closeout amendment S59-A18") ||
        file_has(s22, "grep -rn \"wcwidth\\|width\" src/ui/") ||
        file_has(s25, "grep -rn 'rename\\|fsync' src/ws/") ||
        file_has(s27, "'YEW_EV_MOUSE\\|mouse' src/") ||
        file_has(closeout, "grep -rn \"wcwidth\\|width\" src/ui/") ||
        file_has(closeout, "grep -rn 'rename\\|fsync' src/ws/") ||
        file_has(closeout, "'YEW_EV_MOUSE\\|mouse' src/") ||
        !file_has(s22,
                  "Unicode measurement and coordinate conversion remain centralized") ||
        !file_has(s25,
                  "Direct `rename()` and `fsync()` calls remain absent from `src/ws/`") ||
        !file_has(s27, "Mouse-event dispatch remains singular") ||
        !file_has(closeout,
                  "Unicode measurement and coordinate conversion remain centralized") ||
        !file_has(closeout,
                  "Direct `rename()` and `fsync()` calls remain absent from `src/ws/`") ||
        !file_has(closeout, "Mouse-event dispatch remains singular")) {
        (void)snprintf(why, why_cap,
                       "F07 contract retains impossible ownership greps");
        return false;
    }
    return true;
}
