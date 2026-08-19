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

static bool intern_is(const Interner *in, u32 id, const char *word)
{
    const char *bytes = yew_intern_str(in, id);
    size_t len = yew_intern_len(in, id);
    size_t want = strlen(word);

    return bytes != NULL && len == want && memcmp(bytes, word, want) == 0;
}

static bool backend_call(const Interner *in, const FlNode *node)
{
    const FlNode *callee;
    const FlNode *object;

    if (node == NULL || node->kind != (u8)FL_A_CALL)
        return false;
    callee = node->as.call.callee;
    if (callee == NULL || callee->kind != (u8)FL_A_FIELD ||
        !intern_is(in, callee->as.field.name, "backend"))
        return false;
    object = callee->as.field.obj;
    return object != NULL && object->kind == (u8)FL_A_IDENT &&
           intern_is(in, object->as.ident.name, "ai");
}

static bool validate_node(Arena *arena, DiagCtx *dc, const Interner *in,
                          const FlNode *node);

static bool validate_nodes(Arena *arena, DiagCtx *dc, const Interner *in,
                           FlNode *const *nodes, u32 n)
{
    u32 i;
    bool valid = true;

    for (i = 0U; i < n; i++)
        if (!validate_node(arena, dc, in, nodes[i]))
            valid = false;
    return valid;
}

static bool validate_node(Arena *arena, DiagCtx *dc, const Interner *in,
                          const FlNode *node)
{
    bool valid = true;

    if (node == NULL)
        return true;
    if (backend_call(in, node) && node->as.call.nargs >= 2U &&
        node->as.call.args[1] != NULL &&
        node->as.call.args[1]->kind == (u8)FL_A_MAP &&
        !yew_ai_config_validate_backend(arena, dc, in,
                                        node->as.call.args[1], NULL))
        valid = false;
    switch ((FlAstKind)node->kind) {
    case FL_A_LET:
        return validate_node(arena, dc, in, node->as.let.init) && valid;
    case FL_A_ASSIGN:
        if (!validate_node(arena, dc, in, node->as.assign.tgt)) valid = false;
        if (!validate_node(arena, dc, in, node->as.assign.val)) valid = false;
        return valid;
    case FL_A_FN:
        return validate_node(arena, dc, in, node->as.fn.body) && valid;
    case FL_A_MACRO:
        return validate_node(arena, dc, in, node->as.macro.body) && valid;
    case FL_A_IF:
        if (!validate_node(arena, dc, in, node->as.ifs.cond)) valid = false;
        if (!validate_node(arena, dc, in, node->as.ifs.then)) valid = false;
        if (!validate_node(arena, dc, in, node->as.ifs.els)) valid = false;
        return valid;
    case FL_A_WHILE:
        if (!validate_node(arena, dc, in, node->as.whiles.cond)) valid = false;
        if (!validate_node(arena, dc, in, node->as.whiles.body)) valid = false;
        return valid;
    case FL_A_FOR:
        if (!validate_node(arena, dc, in, node->as.fors.iter)) valid = false;
        if (!validate_node(arena, dc, in, node->as.fors.body)) valid = false;
        return valid;
    case FL_A_RETURN:
        return validate_node(arena, dc, in, node->as.ret.value) && valid;
    case FL_A_EDIT:
        return validate_node(arena, dc, in, node->as.edit.body) && valid;
    case FL_A_TRY:
        if (!validate_node(arena, dc, in, node->as.trys.body)) valid = false;
        if (!validate_node(arena, dc, in, node->as.trys.handler)) valid = false;
        return valid;
    case FL_A_EXPR_STMT:
        return validate_node(arena, dc, in, node->as.expr_stmt.expr) && valid;
    case FL_A_BLOCK:
    case FL_A_LIST:
    case FL_A_MOTION_BLOCK:
        return validate_nodes(arena, dc, in, node->as.list.items,
                              node->as.list.n) && valid;
    case FL_A_BINOP:
        if (!validate_node(arena, dc, in, node->as.bin.l)) valid = false;
        if (!validate_node(arena, dc, in, node->as.bin.r)) valid = false;
        return valid;
    case FL_A_UNOP:
        return validate_node(arena, dc, in, node->as.un.operand) && valid;
    case FL_A_CALL:
        if (!validate_node(arena, dc, in, node->as.call.callee)) valid = false;
        if (!validate_nodes(arena, dc, in, node->as.call.args,
                            node->as.call.nargs)) valid = false;
        return valid;
    case FL_A_INDEX:
        if (!validate_node(arena, dc, in, node->as.index.obj)) valid = false;
        if (!validate_node(arena, dc, in, node->as.index.idx)) valid = false;
        return valid;
    case FL_A_FIELD:
        return validate_node(arena, dc, in, node->as.field.obj) && valid;
    case FL_A_MAP:
        if (!validate_nodes(arena, dc, in, node->as.map.keys,
                            node->as.map.n)) valid = false;
        if (!validate_nodes(arena, dc, in, node->as.map.vals,
                            node->as.map.n)) valid = false;
        return valid;
    case FL_A_FN_EXPR:
        return validate_node(arena, dc, in, node->as.fn.body) && valid;
    case FL_A_MOTION:
        return validate_nodes(arena, dc, in, node->as.motion.inner,
                              node->as.motion.ninner) && valid;
    case FL_A_IMPORT:
    case FL_A_BREAK:
    case FL_A_CONTINUE:
    case FL_A_IDENT:
    case FL_A_LIT:
    case FL_A_KIND__N:
        return valid;
    }
    return valid;
}

bool yew_ai_config_validate_program(Arena *arena, DiagCtx *dc,
                                    const Interner *in,
                                    const FlProgram *program)
{
    if (arena == NULL || dc == NULL || in == NULL || program == NULL)
        return false;
    return validate_nodes(arena, dc, in, program->stmts, program->n);
}
