#include "harness.h"

#include <string.h>

#include "mod/lsp/jsonrpc.h"
#include "util/arena.h"

static JsonValue *parse(Arena *arena, const char *text)
{
    JsonErr err;
    JsonValue *value = yew_json_parse(arena, (const u8 *)text,
                                      (u64)strlen(text), &err);

    YEW_ASSERT_NOT_NULL(value);
    return value;
}

void test_jsonrpc_classification_table(void)
{
    static const struct {
        const char *json;
        RpcMsgKind kind;
    } cases[] = {
        {"{\"jsonrpc\":\"2.0\",\"method\":\"window/logMessage\"}",
         YEW_RPC_NOTIFY},
        {"{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"workspace/configuration\"}",
         YEW_RPC_SRV_REQUEST},
        {"{\"jsonrpc\":\"2.0\",\"id\":\"7\",\"method\":\"x\"}",
         YEW_RPC_SRV_REQUEST},
        {"{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":null}",
         YEW_RPC_RESPONSE},
        {"{\"jsonrpc\":\"2.0\",\"id\":\"7\",\"result\":{}}",
         YEW_RPC_RESPONSE},
        {"{\"jsonrpc\":\"2.0\",\"id\":1,\"error\":{\"code\":-1}}",
         YEW_RPC_ERROR},
        {"{\"jsonrpc\":\"2.0\",\"id\":null,\"error\":{\"code\":-32700}}",
         YEW_RPC_ERROR},
        {"{\"jsonrpc\":\"2.0\",\"id\":\"nope\",\"result\":null}",
         YEW_RPC_MALFORMED},
        {"{\"jsonrpc\":\"1.0\",\"id\":1,\"result\":null}",
         YEW_RPC_MALFORMED},
        {"{\"jsonrpc\":\"2.0\",\"id\":1}", YEW_RPC_MALFORMED},
        {"{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":0,\"error\":{}}",
         YEW_RPC_MALFORMED},
        {"[]", YEW_RPC_MALFORMED}
    };
    size_t i;

    for (i = 0U; i < YEW_ARRAY_LEN(cases); i++) {
        Arena arena;
        JsonValue *value;

        arena_init(&arena);
        value = parse(&arena, cases[i].json);
        YEW_ASSERT_EQ_U64(yew_rpc_classify(value), cases[i].kind);
        arena_free_all(&arena);
    }
}

void test_jsonrpc_id_shapes_and_errors(void)
{
    Arena arena;
    JsonValue *root;
    u64 id = 0U;
    static const struct {
        i64 code;
        RpcErrorAction action;
    } errors[] = {
        {-32700, YEW_RPC_ERR_PARSE},
        {-32600, YEW_RPC_ERR_INVALID_REQUEST},
        {-32601, YEW_RPC_ERR_METHOD_NOT_FOUND},
        {-32602, YEW_RPC_ERR_INVALID_PARAMS},
        {-32603, YEW_RPC_ERR_INTERNAL},
        {-32800, YEW_RPC_ERR_SILENT},
        {-32801, YEW_RPC_ERR_SILENT},
        {-32802, YEW_RPC_ERR_SILENT},
        {-32803, YEW_RPC_ERR_REQUEST_FAILED},
        {-32099, YEW_RPC_ERR_SERVER_RESERVED},
        {-32000, YEW_RPC_ERR_SERVER_RESERVED},
        {-31999, YEW_RPC_ERR_OTHER},
        {42, YEW_RPC_ERR_OTHER}
    };
    size_t i;

    arena_init(&arena);
    root = parse(&arena, "{\"id\":9223372036854775807,\"sid\":\"18446744073709551615\",\"bad\":\"18446744073709551616\"}");
    YEW_ASSERT(yew_rpc_id_u64(yew_json_get(root, "id"), &id));
    YEW_ASSERT_EQ_U64(id, INT64_MAX);
    YEW_ASSERT(yew_rpc_id_u64(yew_json_get(root, "sid"), &id));
    YEW_ASSERT_EQ_U64(id, UINT64_MAX);
    YEW_ASSERT(!yew_rpc_id_u64(yew_json_get(root, "bad"), &id));
    for (i = 0U; i < YEW_ARRAY_LEN(errors); i++)
        YEW_ASSERT_EQ_U64(yew_rpc_error_action(errors[i].code),
                          errors[i].action);
    arena_free_all(&arena);
}

typedef struct {
    u32 calls;
    bool got_result;
    bool got_error;
    i64 code;
    u64 gen;
} CallbackSeen;

static void record_callback(Ed *ed, void *ctx, const JsonValue *result,
                            const JsonValue *error)
{
    CallbackSeen *seen = ctx;
    (void)ed;

    seen->calls++;
    seen->got_result = result != NULL;
    seen->got_error = error != NULL;
    seen->code = yew_json_int(yew_json_get(error, "code"), 0);
}

static const u8 *tx_body(const RpcConn *c, size_t *len)
{
    size_t i;

    for (i = (size_t)c->tx.sent; i + 3U < c->tx.pending.len; i++) {
        if (memcmp(c->tx.pending.data + i, "\r\n\r\n", 4U) == 0) {
            *len = c->tx.pending.len - i - 4U;
            return c->tx.pending.data + i + 4U;
        }
    }
    *len = 0U;
    return NULL;
}

static void record_value(void *ctx, const JsonValue *msg)
{
    u32 *count = ctx;

    YEW_ASSERT_EQ_U64(yew_rpc_classify(msg), YEW_RPC_NOTIFY);
    (*count)++;
}

void test_jsonrpc_connection_delivers_parsed_values(void)
{
    static const char wire[] =
        "Content-Length: 46\r\n\r\n"
        "{\"jsonrpc\":\"2.0\",\"method\":\"window/logMessage\"}";
    RpcConn c;
    u32 count = 0U;
    size_t i;

    yew_rpc_conn_init(&c);
    yew_rpc_set_handler(&c, record_value, &count);
    for (i = 0U; i < sizeof wire - 1U; i++)
        YEW_ASSERT(yew_rpc_feed_stdout(&c, (const u8 *)wire + i, 1U));
    YEW_ASSERT_EQ_U64(count, 1U);
    YEW_ASSERT(yew_rpc_finish_stdout(&c));
    yew_rpc_conn_free(&c);
}

void test_jsonrpc_connection_kills_eight_malformed_messages(void)
{
    static const char wire[] =
        "Content-Length: 2\r\n\r\n{}";
    RpcConn c;
    u32 i;

    yew_rpc_conn_init(&c);
    for (i = 0U; i < 7U; i++)
        YEW_ASSERT(yew_rpc_feed_stdout(&c, (const u8 *)wire,
                                      sizeof wire - 1U));
    YEW_ASSERT_EQ_U64(c.malformed, 7U);
    YEW_ASSERT(!yew_rpc_feed_stdout(&c, (const u8 *)wire,
                                   sizeof wire - 1U));
    YEW_ASSERT_EQ_STR(c.rx.err, "too many malformed JSON-RPC messages");
    yew_rpc_conn_free(&c);
}

void test_jsonrpc_call_dispatch_and_string_echo(void)
{
    RpcConn c;
    RpcPending pending;
    CallbackSeen seen;
    Arena arena;
    JsonValue *response;
    const u8 *body;
    size_t n;
    u64 id;

    yew_rpc_conn_init(&c);
    (void)memset(&pending, 0, sizeof pending);
    (void)memset(&seen, 0, sizeof seen);
    pending.gen = 91U;
    pending.sent_ms = 100U;
    pending.cb = record_callback;
    pending.ctx = &seen;
    id = yew_rpc_call(&c, "textDocument/hover", (const u8 *)"{\"x\":1}",
                      7U, &pending);
    YEW_ASSERT_EQ_U64(id, 1U);
    YEW_ASSERT_EQ_U64(c.npending, 1U);
    YEW_ASSERT_EQ_I64(yew_rpc_pending(&c, id)->deadline_ms, 10100);
    YEW_ASSERT_EQ_U64(yew_rpc_pending(&c, id)->gen, 91U);
    body = tx_body(&c, &n);
    YEW_ASSERT_NOT_NULL(body);
    YEW_ASSERT_EQ_U64(n, sizeof(
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"textDocument/hover\",\"params\":{\"x\":1}}") - 1U);
    YEW_ASSERT_EQ_MEM(body,
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"textDocument/hover\",\"params\":{\"x\":1}}",
        n);

    arena_init(&arena);
    response = parse(&arena,
        "{\"jsonrpc\":\"2.0\",\"id\":\"1\",\"result\":{\"ok\":true}}");
    YEW_ASSERT(yew_rpc_dispatch(&c, NULL, response));
    YEW_ASSERT_EQ_U64(seen.calls, 1U);
    YEW_ASSERT(seen.got_result);
    YEW_ASSERT(!seen.got_error);
    YEW_ASSERT_EQ_U64(c.npending, 0U);
    YEW_ASSERT_NULL(yew_rpc_pending(&c, id));
    YEW_ASSERT(!yew_rpc_dispatch(&c, NULL, response));
    arena_free_all(&arena);
    yew_rpc_conn_free(&c);
}

void test_jsonrpc_pending_cap_timeout_and_cancel(void)
{
    RpcConn c;
    RpcPending pending;
    CallbackSeen seen;
    u64 id;
    u32 i;
    const u8 *body;
    size_t n;

    yew_rpc_conn_init(&c);
    (void)memset(&pending, 0, sizeof pending);
    (void)memset(&seen, 0, sizeof seen);
    pending.sent_ms = 50U;
    pending.deadline_ms = 75U;
    pending.cb = record_callback;
    pending.ctx = &seen;
    for (i = 0U; i < YEW_RPC_MAX_PENDING; i++)
        YEW_ASSERT(yew_rpc_call(&c, "x", NULL, 0U, &pending) != 0U);
    YEW_ASSERT_EQ_U64(c.npending, YEW_RPC_MAX_PENDING);
    YEW_ASSERT_EQ_I64(yew_rpc_deadline(&c), 75);
    YEW_ASSERT_EQ_U64(yew_rpc_call(&c, "overflow", NULL, 0U, &pending), 0U);
    YEW_ASSERT_EQ_U64(yew_rpc_sweep(&c, NULL, 74U), 0U);
    YEW_ASSERT_EQ_U64(yew_rpc_sweep(&c, NULL, 75U), YEW_RPC_MAX_PENDING);
    YEW_ASSERT_EQ_U64(seen.calls, YEW_RPC_MAX_PENDING);
    YEW_ASSERT(seen.got_error);
    YEW_ASSERT_EQ_I64(seen.code, -32001);
    YEW_ASSERT_EQ_U64(c.npending, 0U);
    YEW_ASSERT_EQ_I64(yew_rpc_deadline(&c), -1);

    c.tx.pending.len = 0U;
    c.tx.sent = 0U;
    id = yew_rpc_call(&c, "initialize", NULL, 0U, NULL);
    YEW_ASSERT_EQ_I64(yew_rpc_pending(&c, id)->deadline_ms, 60000);
    c.tx.pending.len = 0U;
    c.tx.sent = 0U;
    yew_rpc_cancel(&c, id);
    body = tx_body(&c, &n);
    YEW_ASSERT_NOT_NULL(body);
    YEW_ASSERT_EQ_U64(n, sizeof(
        "{\"jsonrpc\":\"2.0\",\"method\":\"$/cancelRequest\",\"params\":{\"id\":257}}") - 1U);
    YEW_ASSERT_EQ_MEM(body,
        "{\"jsonrpc\":\"2.0\",\"method\":\"$/cancelRequest\",\"params\":{\"id\":257}}",
        n);
    YEW_ASSERT_NOT_NULL(yew_rpc_pending(&c, id));
    c.tx.pending.len = 0U;
    c.tx.sent = 0U;
    yew_rpc_cancel(&c, UINT64_C(999999));
    YEW_ASSERT_EQ_U64(c.tx.pending.len, 0U);
    yew_rpc_conn_free(&c);
}

void test_jsonrpc_reply_shapes(void)
{
    RpcConn c;
    Arena arena;
    JsonValue *id;
    const u8 *body;
    size_t n;

    yew_rpc_conn_init(&c);
    arena_init(&arena);
    id = parse(&arena, "\"server-7\"");
    yew_rpc_reply(&c, id, (const u8 *)"{\"ok\":true}", 11U);
    body = tx_body(&c, &n);
    YEW_ASSERT_EQ_U64(n, sizeof(
        "{\"jsonrpc\":\"2.0\",\"id\":\"server-7\",\"result\":{\"ok\":true}}") - 1U);
    YEW_ASSERT_EQ_MEM(body,
        "{\"jsonrpc\":\"2.0\",\"id\":\"server-7\",\"result\":{\"ok\":true}}",
        n);
    c.tx.pending.len = 0U;
    c.tx.sent = 0U;
    yew_rpc_reply_error(&c, id, -32601, "not \"there\"");
    body = tx_body(&c, &n);
    YEW_ASSERT_EQ_U64(n, sizeof(
        "{\"jsonrpc\":\"2.0\",\"id\":\"server-7\",\"error\":{\"code\":-32601,\"message\":\"not \\\"there\\\"\"}}") - 1U);
    YEW_ASSERT_EQ_MEM(body,
        "{\"jsonrpc\":\"2.0\",\"id\":\"server-7\",\"error\":{\"code\":-32601,\"message\":\"not \\\"there\\\"\"}}",
        n);
    arena_free_all(&arena);
    yew_rpc_conn_free(&c);
}
