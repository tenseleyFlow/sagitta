#include "mod/ai/config.h"

#include <stdio.h>
#include <string.h>

#include "util/log.h"

void yew_ai_config_render_credential_diag(Bytebuf *out, const DiagCtx *dc,
                                          FlSpan span, const char *word)
{
    char message[128];

    if (out == NULL)
        return;
    (void)snprintf(message, sizeof(message),
                   "'%s' may not hold a literal API key",
                   word == NULL ? "" : word);
    fl_diag_render(out, dc, FL_DIAG_ERROR, span, message);
    bytebuf_append(out,
        (const u8 *)"      = init.fl is pasted into bug reports and "
                    "committed to dotfile repos.\n",
        strlen("      = init.fl is pasted into bug reports and "
               "committed to dotfile repos.\n"));
    bytebuf_append(out,
        (const u8 *)"      = use key_env: \"ANTHROPIC_API_KEY\", or "
                    "key_cmd: [\"pass\",\"show\",\"…\"]\n",
        strlen("      = use key_env: \"ANTHROPIC_API_KEY\", or "
               "key_cmd: [\"pass\",\"show\",\"…\"]\n"));
}

static void emit_credential_diag(DiagCtx *dc, FlSpan span,
                                 const char *word)
{
    char message[128];
    Bytebuf rendered;

    if (dc == NULL || dc->muted)
        return;
    (void)snprintf(message, sizeof(message),
                   "'%s' may not hold a literal API key", word);
    dc->nerrors++;
    bytebuf_init(&rendered);
    yew_ai_config_render_credential_diag(&rendered, dc, span, word);
    bytebuf_push_u8(&rendered, (u8)'\0');
    if (dc->sink != NULL)
        dc->sink(dc->sink_ctx, FL_DIAG_ERROR, span, message,
                 (const char *)rendered.data);
    else
        yew_log(YEW_LOG_ERROR, "%s", (const char *)rendered.data);
    bytebuf_free(&rendered);
}

static bool key_is(const Interner *in, const FlNode *key,
                   const char *word)
{
    const char *bytes;
    size_t len;
    size_t want;

    if (in == NULL || key == NULL || key->kind != (u8)FL_A_LIT ||
        key->as.lit.lit != (u8)FL_L_STR)
        return false;
    bytes = yew_intern_str(in, key->as.lit.v.str_id);
    len = yew_intern_len(in, key->as.lit.v.str_id);
    want = strlen(word);
    return bytes != NULL && len == want && memcmp(bytes, word, want) == 0;
}

static bool forbidden_key(const Interner *in, const FlNode *key,
                          const char **word)
{
    static const char *const forbidden[] = {
        "key", "api_key", "token", "password", "secret"
    };
    u32 i;

    for (i = 0U; i < YEW_ARRAY_LEN(forbidden); i++) {
        if (key_is(in, key, forbidden[i])) {
            *word = forbidden[i];
            return true;
        }
    }
    return false;
}

bool yew_ai_config_validate_backend(Arena *arena, DiagCtx *dc,
                                    const Interner *in,
                                    const FlNode *backend,
                                    HttpUrl *parsed_url)
{
    u32 i;
    bool valid = true;

    if (arena == NULL || dc == NULL || in == NULL || backend == NULL ||
        backend->kind != (u8)FL_A_MAP)
        return false;
    for (i = 0U; i < backend->as.map.n; i++) {
        const FlNode *key = backend->as.map.keys[i];
        const FlNode *value = backend->as.map.vals[i];
        const char *word = NULL;

        if (forbidden_key(in, key, &word)) {
            emit_credential_diag(dc, key->sp, word);
            valid = false;
            continue;
        }
        if (key_is(in, key, "url") && value != NULL &&
            value->kind == (u8)FL_A_LIT &&
            value->as.lit.lit == (u8)FL_L_STR) {
            const char *url = yew_intern_str(in, value->as.lit.v.str_id);
            size_t url_len = yew_intern_len(in, value->as.lit.v.str_id);
            HttpUrl ignored_url;
            HttpUrl *destination = parsed_url == NULL ? &ignored_url :
                                   parsed_url;
            char err[256];

            /* Embedded NULs are not URLs, even though the interner supports
             * them byte-exactly.  Avoid handing a truncated value to the
             * C-string URL parser. */
            if (url == NULL || strlen(url) != url_len ||
                !yew_http_url_parse(arena, url, destination, err,
                                    sizeof(err))) {
                if (url == NULL || strlen(url) != url_len)
                    (void)snprintf(err, sizeof(err), "bad url");
                fl_diag_emit(dc, FL_DIAG_ERROR, value->sp, "%s", err);
                valid = false;
            }
        }
    }
    return valid;
}
