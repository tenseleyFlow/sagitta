/*
 * YEW-F-106 — F07 contracts require superseded UI staging guards.
 *
 * Correct behavior: a formal decision amendment, source sprint contracts,
 * and their closeout distinguish surfaces delivered by later owning sprints
 * from the few intentional post-1.0 boundaries that remain.
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

bool test_yew_f_106(char *why, size_t why_cap)
{
    static const char *const s22 =
        ".docs/sprints/05-ui-workspace/s22-layout-panes.md";
    static const char *const s23 =
        ".docs/sprints/05-ui-workspace/s23-tabs.md";
    static const char *const s24 =
        ".docs/sprints/05-ui-workspace/s24-tab-groups.md";
    static const char *const s25 =
        ".docs/sprints/05-ui-workspace/s25-workspace-state.md";
    static const char *const s26 =
        ".docs/sprints/05-ui-workspace/s26-fuzzy-finder.md";
    static const char *const s27 =
        ".docs/sprints/05-ui-workspace/s27-mouse-and-feel.md";
    static const char *const closeout =
        ".docs/audits/closeout-F07-ui.md";

    if (!file_has(".docs/plan/00-decisions.md",
                  "UI/workspace closeout amendment S59-A17") ||
        file_has(s23, "`YEW_BUG` naming Sprint 24") ||
        file_has(s25, "tests/unit/test_state_differential.c") ||
        file_has(s26, "hard-error naming their sprint") ||
        file_has(s27, "right-click on a pane does") ||
        file_has(s27,
                 "Dragging a tab into a *pane* to open it there") ||
        file_has(closeout,
                 "`ed.tab.*` invocations hard-error naming Sprint 23") ||
        file_has(closeout,
                 "negative `YEW_REGION_TAB` payload reaching the router") ||
        file_has(closeout,
                 "`ed.group.from_dir` hard-errors naming Sprint 53") ||
        file_has(closeout, "tests/unit/test_state_differential.c") ||
        file_has(closeout,
                 "Deferred pickers hard-error naming Sprints 47, 55, 52, 38") ||
        file_has(closeout, "document context menu (post-1.0") ||
        !file_has(s22,
                  "Sprint 23 delivered the `ed.tab.*` command surface") ||
        !file_has(s23,
                  "Negative `YEW_REGION_TAB` payloads now identify") ||
        !file_has(s24, "Sprint 53 delivered `ed.group.from_dir`") ||
        !file_has(s25,
                  "Sprint 36 retired the hand-written differential") ||
        !file_has(s26,
                  "Later owning sprints delivered all four picker surfaces") ||
        !file_has(s27,
                  "Later owning sprints delivered document context menus") ||
        !file_has(closeout,
                  "Later owning sprints delivered the staged UI surfaces")) {
        (void)snprintf(why, why_cap,
                       "F07 contract still requires superseded UI guards");
        return false;
    }
    return true;
}
