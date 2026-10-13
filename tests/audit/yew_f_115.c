/*
 * YEW-F-115 — F11 contracts require superseded LSP staging guards.
 *
 * Correct behavior: source sprint contracts and closeout preserve the
 * historical handoffs while testing the shadow, lifecycle, documentation,
 * and feature surfaces delivered by their later owning sprints.
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

bool test_yew_f_115(char *why, size_t why_cap)
{
    static const char *const s43 =
        ".docs/sprints/09-completion-lsp/s43-shadow-text.md";
    static const char *const s44 =
        ".docs/sprints/09-completion-lsp/s44-symbol-completion.md";
    static const char *const s45 =
        ".docs/sprints/09-completion-lsp/s45-json-jsonrpc.md";
    static const char *const s46 =
        ".docs/sprints/09-completion-lsp/s46-lsp-client-core.md";
    static const char *const closeout =
        ".docs/audits/closeout-F11-lsp.md";

    if (!file_has(".docs/plan/00-decisions.md",
                  "Completion/LSP closeout amendment S59-A26") ||
        !file_has(s43,
                  "Sprints 44, 47, and 49 delivered the staged shadow providers") ||
        !file_has(s44,
                  "Sprint 47 delivered the LSP documentation handover") ||
        !file_has(s45,
                  "Sprint 46 delivered full-build server startup") ||
        !file_has(s46,
                  "Sprint 47 delivered all nine feature commands") ||
        !file_has(closeout,
                  "Later binding Sprints 44, 46, 47, and 49 supersede")) {
        (void)snprintf(why, why_cap,
                       "F11 contract still requires pre-delivery LSP guards");
        return false;
    }
    return true;
}
