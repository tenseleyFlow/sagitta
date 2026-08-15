/* Sprint 46: mutated initialize results, responses, and diagnostics. */
#include "fuzzlib.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "edit/ed.h"
#include "mod/lsp/client.h"
#include "mod/lsp/diag.h"
#include "mod/lsp/json.h"
#include "mod/lsp/jsonrpc.h"
#include "util/arena.h"
#include "util/buf.h"

enum {
    LSP_FUZZ_LINES = 16,
    LSP_FUZZ_DIAG_MAX = 4096,
    LSP_FUZZ_STRING_MAX = 64 * 1024
};

static bool fail(char *why, size_t cap, const char *message)
{
    (void)snprintf(why, cap, "%s", message);
    return false;
}

static bool valid_state(u8 state)
{
    return state == YEW_LSP_SPAWNING ||
           state == YEW_LSP_INITIALIZING || state == YEW_LSP_READY ||
           state == YEW_LSP_SHUTTING_DOWN || state == YEW_LSP_DEAD;
}

static bool fuzz_initialize(const JsonValue *input,
                            char *why, size_t why_cap)
{
    static const char *const roots[] = {".git", NULL};
    static const LspServerCfg cfg = {
        .id = "fuzz-lsp",
        .lang = "c",
        .cmd = "fuzz-lsp",
        .args = NULL,
        .roots = roots,
        .init_options = NULL,
        .init_timeout_ms = YEW_RPC_INIT_TIMEOUT_MS
    };
    LspCaps caps;
    LspServer server;
    bool unknown = false;
    u8 encoding = 0U;
    char *root = malloc(2U);

    if (root == NULL)
        return fail(why, why_cap, "cannot allocate server root");
    (void)memcpy(root, "/", 2U);
    yew_lsp_caps_parse(&caps, input, &encoding, &unknown);
    if ((encoding != YEW_POSENC_UTF8 && encoding != YEW_POSENC_UTF16) ||
        caps.sync_kind > 2U) {
        free(root);
        return fail(why, why_cap, "capability parser returned invalid state");
    }
    yew_lsp_server_init(&server, 1U, &cfg, root);
    server.state = YEW_LSP_INITIALIZING;
    (void)yew_lsp_server_initialized(&server, input);
    if (!valid_state(server.state)) {
        yew_lsp_server_dispose(&server);
        return fail(why, why_cap, "initialize left invalid LSP state");
    }
    if (server.state == YEW_LSP_READY && server.caps.sync_kind == 0U) {
        yew_lsp_server_dispose(&server);
        return fail(why, why_cap, "server ready without document sync");
    }
    yew_lsp_server_dispose(&server);
    return true;
}

typedef struct ResponseSeen {
    u32 calls;
} ResponseSeen;

static void response_seen(Ed *ed, void *ctx, const JsonValue *result,
                          const JsonValue *error)
{
    ResponseSeen *seen = ctx;

    (void)ed;
    if (result != NULL || error != NULL)
        seen->calls++;
}

static bool fuzz_response(const JsonValue *input,
                          char *why, size_t why_cap)
{
    RpcConn rpc;
    RpcPending pending;
    ResponseSeen seen = {0U};
    Bytebuf encoded;
    JsonW writer;
    Arena arena;
    JsonErr err;
    JsonValue *response;
    Ed ed;
    u64 id;
    bool ok = false;

    yew_ed_init(&ed);
    yew_rpc_conn_init(&rpc);
    bytebuf_init(&encoded);
    arena_init(&arena);
    (void)memset(&pending, 0, sizeof(pending));
    pending.cb = response_seen;
    pending.ctx = &seen;
    id = yew_rpc_call(&rpc, "fuzz/response", NULL, 0U, &pending);
    if (id == 0U)
        goto done;
    yew_jsonw_init(&writer, &encoded);
    yew_jsonw_obj(&writer);
    yew_jsonw_key(&writer, "jsonrpc");
    yew_jsonw_cstr(&writer, "2.0");
    yew_jsonw_key(&writer, "id");
    yew_jsonw_int(&writer, (i64)id);
    yew_jsonw_key(&writer, "result");
    yew_jsonw_value(&writer, input);
    yew_jsonw_obj_end(&writer);
    response = yew_json_parse(&arena, encoded.data, encoded.len, &err);
    if (response == NULL || !yew_rpc_dispatch(&rpc, &ed, response))
        goto done;
    /* The raw mutation also exercises classification/unknown-id rejection. */
    (void)yew_rpc_dispatch(&rpc, &ed, input);
    if (seen.calls != 1U || rpc.npending != 0U)
        goto done;
    ok = true;
done:
    arena_free_all(&arena);
    bytebuf_free(&encoded);
    yew_rpc_conn_free(&rpc);
    yew_ed_free(&ed);
    if (!ok)
        return fail(why, why_cap, "response was not dispatched exactly once");
    return true;
}

static void jsonw_position(JsonW *w, i64 line, i64 character)
{
    yew_jsonw_obj(w);
    yew_jsonw_key(w, "line");
    yew_jsonw_int(w, line);
    yew_jsonw_key(w, "character");
    yew_jsonw_int(w, character);
    yew_jsonw_obj_end(w);
}

static bool build_mutated_diagnostics(const u8 *data, size_t len,
                                      Bytebuf *json)
{
    JsonW w;
    size_t at = 0U;
    u32 count = len == 0U ? 1U : 1U + data[0] % 32U;
    u32 i;

    yew_jsonw_init(&w, json);
    yew_jsonw_arr(&w);
    for (i = 0U; i < count; i++) {
        i64 sl = len == 0U ? 0 : (i64)(i8)data[at++ % len];
        i64 sc = len == 0U ? 0 : (i64)data[at++ % len] * 1024;
        i64 el = len == 0U ? 0 : (i64)(i8)data[at++ % len];
        i64 ec = len == 0U ? 0 : (i64)data[at++ % len] * 1024;
        size_t msg_at = len == 0U ? 0U : at % len;
        size_t msg_len = len == 0U ? 0U : len - msg_at;

        yew_jsonw_obj(&w);
        yew_jsonw_key(&w, "range");
        yew_jsonw_obj(&w);
        yew_jsonw_key(&w, "start");
        jsonw_position(&w, sl, sc);
        yew_jsonw_key(&w, "end");
        jsonw_position(&w, el, ec);
        yew_jsonw_obj_end(&w);
        yew_jsonw_key(&w, "severity");
        yew_jsonw_int(&w, len == 0U ? 1 : data[at++ % len]);
        yew_jsonw_key(&w, "message");
        yew_jsonw_str(&w, data + msg_at, (u32)msg_len);
        yew_jsonw_obj_end(&w);
    }
    yew_jsonw_arr_end(&w);
    return json->len <= YEW_JSON_MAX_BYTES;
}

static bool fuzz_diagnostics(const u8 *data, size_t len,
                             const JsonValue *input,
                             char *why, size_t why_cap)
{
    static const u8 line[] = "0123456789abcdef\n";
    Bytebuf text;
    Bytebuf json;
    Arena arena;
    JsonErr err;
    JsonValue *made;
    Ed ed;
    u32 i;
    bool ok = false;

    bytebuf_init(&text);
    bytebuf_init(&json);
    for (i = 0U; i < LSP_FUZZ_LINES; i++)
        bytebuf_append(&text, line, sizeof(line) - 1U);
    if (!build_mutated_diagnostics(data, len, &json))
        goto done_buffers;
    arena_init(&arena);
    made = yew_json_parse(&arena, json.data, json.len, &err);
    if (made == NULL || made->kind != YEW_JS_ARR)
        goto done_arena;
    yew_ed_init(&ed);
    if (!yew_ed_open_memory(&ed, text.data, text.len, "fuzz_lsp.c"))
        goto done_ed;
    yew_diag_replace(&ed, &ed.buffer, 1U, made, 1);
    if (input != NULL && input->kind == YEW_JS_ARR)
        yew_diag_replace(&ed, &ed.buffer, 1U, input, 2);
    if (ed.buffer.diag != NULL) {
        DiagStore *store = ed.buffer.diag;
        u64 text_len = yew_textbuf_len(ed.buffer.tb);

        if (store->d.len > LSP_FUZZ_DIAG_MAX)
            goto done_ed;
        for (i = 0U; i < store->d.len; i++) {
            const Diagnostic *diag = &store->d.data[i];

            if (diag->cache.lo > diag->cache.hi ||
                diag->cache.hi > text_len || diag->sev < YEW_DIAG_ERROR ||
                diag->sev > YEW_DIAG_HINT || diag->message == NULL ||
                strlen(diag->message) > LSP_FUZZ_STRING_MAX)
                goto done_ed;
        }
    }
    ok = true;
done_ed:
    yew_ed_free(&ed);
done_arena:
    arena_free_all(&arena);
done_buffers:
    bytebuf_free(&json);
    bytebuf_free(&text);
    if (!ok)
        return fail(why, why_cap,
                    "diagnostic handler violated bounded store invariant");
    return true;
}

static bool check_lsp_message(const u8 *data, size_t len,
                              char *why, size_t why_cap)
{
    static const u8 fallback[] = "{}";
    const u8 *bytes = len == 0U ? fallback : data;
    size_t nbytes = len == 0U ? sizeof(fallback) - 1U : len;
    Arena arena;
    JsonErr err;
    JsonValue *input;
    bool ok;

    arena_init(&arena);
    input = yew_json_parse(&arena, bytes, nbytes, &err);
    if (input == NULL) {
        arena_free_all(&arena);
        return fuzz_diagnostics(data, len, NULL, why, why_cap);
    }
    ok = fuzz_initialize(input, why, why_cap) &&
         fuzz_response(input, why, why_cap) &&
         fuzz_diagnostics(data, len, input, why, why_cap);
    arena_free_all(&arena);
    return ok;
}

int main(int argc, char **argv)
{
    return yew_fuzz_main(argc, argv, "fuzz_lsp_msg", NULL,
                         check_lsp_message);
}
