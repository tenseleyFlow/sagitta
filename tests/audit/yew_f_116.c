/*
 * YEW-F-116 — F11 contracts retain invalid determinism source scans.
 *
 * Correct behavior: symbol determinism scans the files owned by the symbol
 * index with token boundaries, and the JSON formatter scan admits the
 * precision-bearing conversion required by the implementation.
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

bool test_yew_f_116(char *why, size_t why_cap)
{
    static const char *const s44 =
        ".docs/sprints/09-completion-lsp/s44-symbol-completion.md";
    static const char *const s45 =
        ".docs/sprints/09-completion-lsp/s45-json-jsonrpc.md";
    static const char *const closeout =
        ".docs/audits/closeout-F11-lsp.md";

    if (!file_has(".docs/plan/00-decisions.md",
                  "Completion/LSP ownership closeout amendment S59-A27") ||
        !file_has(s44,
                  "symbol-index sources rather than all workspace owners") ||
        !file_has(s45,
                  "precision-bearing conversion specifiers") ||
        !file_has(closeout,
                  "Current closeout scopes symbol determinism") ||
        !file_has(closeout,
                  "JSON real-format gate accepts precision-bearing")) {
        (void)snprintf(why, why_cap,
                       "F11 contract retains invalid determinism scans");
        return false;
    }
    return true;
}
