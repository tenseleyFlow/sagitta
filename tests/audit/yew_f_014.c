/*
 * YEW-F-014 — stripped LSP completion bypasses the module hard error.
 *
 * Correct behavior: Sprints 45, 47, and 58 F11 require every ed.lsp.*
 * command in a MODULES="" build to return the exact yew_mod_require error.
 *
 * Baseline failure: yew_lsp_complete is an intentional exception in
 * shim.c.  It reports INFO and opens core index completion, while the module
 * boundary test explicitly skips ed.lsp.complete.
 */
#include "audit.h"

#include <stdio.h>
#include <string.h>

bool test_yew_f_014(char *why, size_t why_cap)
{
    char source[16384];
    FILE *file = fopen("src/mod/lsp/shim.c", "rb");
    const char *begin;
    const char *end;
    size_t len;
    bool requires;
    bool falls_back;

    if (file == NULL)
        return false;
    len = fread(source, 1U, sizeof(source) - 1U, file);
    if (ferror(file) || fclose(file) != 0)
        return false;
    source[len] = '\0';
    begin = strstr(source, "bool yew_lsp_complete(");
    end = begin == NULL ? NULL : strstr(begin, "bool yew_lsp_hover(");
    if (begin == NULL || end == NULL)
        return false;
    requires = strstr(begin, "require_lsp(ed)") != NULL &&
               strstr(begin, "require_lsp(ed)") < end;
    falls_back = strstr(begin, "yew_compl_open_source") != NULL &&
                 strstr(begin, "yew_compl_open_source") < end;
    if (!requires || falls_back) {
        (void)snprintf(why, why_cap,
                       "MODULES=empty completion shim: requires=%u index_fallback=%u",
                       requires ? 1U : 0U, falls_back ? 1U : 0U);
    }
    return requires && !falls_back;
}
