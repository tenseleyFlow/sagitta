/*
 * YEW-F-117 — the live-clangd harness treats path aliases as documents.
 *
 * Correct behavior: requests use the attached LspDoc URI, exactly like the
 * production feature and rename paths, and the fixture root is canonicalized
 * once so requests, responses, and expected relative paths all name the same
 * document even when /tmp resolves through /private/tmp on macOS.
 */
#include "audit.h"

#include <stdio.h>
#include <string.h>

bool test_yew_f_117(char *why, size_t why_cap)
{
    char source[65536];
    FILE *file = fopen("tests/lsp/test_clangd_live.c", "rb");
    const char *begin;
    const char *end;
    size_t len;
    bool owns_doc;
    bool uses_doc_uri;
    bool rebuilds_uri;
    bool canonicalizes_root;

    if (file == NULL)
        return false;
    len = fread(source, 1U, sizeof(source) - 1U, file);
    if (ferror(file) || fclose(file) != 0)
        return false;
    source[len] = '\0';
    begin = strstr(source, "static void position_params(");
    end = begin == NULL ? NULL :
          strstr(begin, "static const char *relative_path(");
    if (begin == NULL || end == NULL)
        return false;
    owns_doc = strstr(begin, "const LspDoc *doc") != NULL &&
               strstr(begin, "const LspDoc *doc") < end;
    uses_doc_uri = strstr(begin, "yew_jsonw_cstr(&writer, doc->uri)") != NULL &&
                   strstr(begin, "yew_jsonw_cstr(&writer, doc->uri)") < end;
    rebuilds_uri = strstr(begin, "yew_lsp_uri_of_path") != NULL &&
                   strstr(begin, "yew_lsp_uri_of_path") < end;
    canonicalizes_root = strstr(source, "realpath(root, canonical_root)") !=
                         NULL;
    if (!owns_doc || !uses_doc_uri || rebuilds_uri || !canonicalizes_root) {
        (void)snprintf(why, why_cap,
                       "live LSP path identity: doc=%u uses_doc=%u "
                       "rebuilds=%u canonical_root=%u",
                       owns_doc ? 1U : 0U, uses_doc_uri ? 1U : 0U,
                       rebuilds_uri ? 1U : 0U,
                       canonicalizes_root ? 1U : 0U);
    }
    return owns_doc && uses_doc_uri && !rebuilds_uri && canonicalizes_root;
}
