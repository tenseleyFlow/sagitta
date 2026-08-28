#include "mod/lsp/jsonrpc.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "unicode/utf8.h"
#include "util/arena.h"

enum { RPC_SLOT_EMPTY = 0, RPC_SLOT_USED, RPC_SLOT_TOMB };

#define APPEND_LIT(buf, lit) bytebuf_append((buf), (lit), sizeof(lit) - 1U)

static void conn_on_value(void *ctx, const JsonValue *value);

static void rpc_dead(RpcRx *rx, const char *msg)
{
    rx->state = YEW_RPCRX_DEAD;
    (void)snprintf(rx->err, sizeof rx->err, "%s", msg);
}

static bool ascii_ieq(const u8 *s, size_t n, const char *lit)
{
    size_t i;

    if (strlen(lit) != n)
        return false;
    for (i = 0U; i < n; i++) {
        u8 a = s[i];
        u8 b = (u8)lit[i];

        if (a >= (u8)'A' && a <= (u8)'Z')
            a = (u8)(a + ((u8)'a' - (u8)'A'));
        if (b >= (u8)'A' && b <= (u8)'Z')
            b = (u8)(b + ((u8)'a' - (u8)'A'));
        if (a != b)
            return false;
    }
    return true;
}

static bool parse_length(RpcRx *rx, const u8 *s, size_t n)
{
    u64 value = 0U;
    size_t i;
    char text[48];
    size_t shown;

    for (i = 0U; i < n && (s[i] == (u8)' ' || s[i] == (u8)'\t'); i++)
        ;
    s += i;
    n -= i;
    shown = n < sizeof text - 1U ? n : sizeof text - 1U;
    if (n == 0U)
        goto bad;
    for (i = 0U; i < n; i++) {
        u8 digit = s[i];

        if (digit < (u8)'0' || digit > (u8)'9')
            goto bad;
        if (value > (YEW_RPC_MAX_BODY - (u64)(digit - (u8)'0')) / 10U)
            goto bad;
        value = value * 10U + (u64)(digit - (u8)'0');
    }
    rx->want = value;
    rx->have_len = true;
    return true;

bad:
    (void)memcpy(text, s, shown);
    text[shown] = '\0';
    rx->state = YEW_RPCRX_DEAD;
    (void)snprintf(rx->err, sizeof rx->err, "bad Content-Length '%s'", text);
    return false;
}

static bool finish_header_line(RpcRx *rx)
{
    size_t colon;

    if (rx->line.len == 0U) {
        if (!rx->have_len) {
            rpc_dead(rx, "frame has no Content-Length");
            return false;
        }
        rx->state = YEW_RPCRX_BODY;
        rx->body.len = 0U;
        return true;
    }
    rx->nhdr++;
    if (rx->nhdr > YEW_RPC_MAX_HDRS) {
        rpc_dead(rx, "too many headers");
        return false;
    }
    for (colon = 0U; colon < rx->line.len; colon++) {
        if (rx->line.data[colon] == (u8)':')
            break;
    }
    if (colon < rx->line.len &&
        ascii_ieq(rx->line.data, colon, "Content-Length")) {
        if (!parse_length(rx, rx->line.data + colon + 1U,
                          rx->line.len - colon - 1U))
            return false;
    }
    rx->line.len = 0U;
    return true;
}

static bool deliver_body(RpcRx *rx, RpcMsgFn on_msg, void *ctx)
{
    Arena arena;
    JsonErr err;
    JsonValue *root;
    bool killed;
    size_t bad = yew_utf8_validate(rx->body.data, rx->body.len);

    if (bad != rx->body.len) {
        rx->state = YEW_RPCRX_DEAD;
        (void)snprintf(rx->err, sizeof rx->err,
                       "body is not UTF-8 at offset %zu", bad);
        return false;
    }
    arena_init(&arena);
    root = yew_json_parse(&arena, rx->body.data, (u64)rx->body.len, &err);
    if (root == NULL || root->kind != YEW_JS_OBJ) {
        arena_free_all(&arena);
        rpc_dead(rx, "frame is not a JSON-RPC object");
        return false;
    }
    if (rx->on_value != NULL)
        rx->on_value(rx->value_ctx, root);
    if (on_msg != NULL)
        on_msg(ctx, rx->body.data, (u64)rx->body.len);
    killed = rx->state == YEW_RPCRX_DEAD;
    arena_free_all(&arena);
    rx->msgs_in++;
    if (killed)
        return false;
    rx->state = YEW_RPCRX_HDR;
    rx->line.len = 0U;
    rx->body.len = 0U;
    rx->want = 0U;
    rx->have_len = false;
    rx->nhdr = 0U;
    return true;
}

void yew_rpcrx_init(RpcRx *rx)
{
    (void)memset(rx, 0, sizeof *rx);
    rx->state = YEW_RPCRX_HDR;
    bytebuf_init(&rx->line);
    bytebuf_init(&rx->body);
}

void yew_rpcrx_set_handler(RpcRx *rx, RpcValueFn on_value, void *ctx)
{
    if (rx == NULL)
        return;
    rx->on_value = on_value;
    rx->value_ctx = ctx;
}

u32 yew_rpcrx_feed(RpcRx *rx, const u8 *b, u64 n, RpcMsgFn on_msg,
                   void *ctx)
{
    u64 at = 0U;
    u32 delivered = 0U;

    if (rx == NULL || rx->state == YEW_RPCRX_DEAD || (b == NULL && n != 0U))
        return 0U;
    rx->bytes_in += n;
    while (at < n && rx->state != YEW_RPCRX_DEAD) {
        if (rx->state == YEW_RPCRX_BODY) {
            u64 need = rx->want - (u64)rx->body.len;
            u64 take = n - at < need ? n - at : need;

            bytebuf_append(&rx->body, b + at, (size_t)take);
            at += take;
            if ((u64)rx->body.len == rx->want) {
                if (!deliver_body(rx, on_msg, ctx))
                    break;
                delivered++;
            }
            continue;
        }
        if (rx->pending_cr) {
            rx->pending_cr = false;
            if (b[at++] != (u8)'\n') {
                rpc_dead(rx, "header line must end with CRLF");
                break;
            }
            if (!finish_header_line(rx))
                break;
            if (rx->state == YEW_RPCRX_BODY && rx->want == 0U) {
                if (!deliver_body(rx, on_msg, ctx))
                    break;
                delivered++;
            }
            continue;
        }
        if (b[at] == (u8)'\r') {
            rx->pending_cr = true;
            at++;
        } else {
            bytebuf_push_u8(&rx->line, b[at++]);
            if (rx->line.len > YEW_RPC_MAX_HDRLINE) {
                rpc_dead(rx, "header line too long");
                break;
            }
        }
    }
    return delivered;
}

void yew_rpcrx_free(RpcRx *rx)
{
    if (rx == NULL)
        return;
    bytebuf_free(&rx->line);
    bytebuf_free(&rx->body);
    (void)memset(rx, 0, sizeof *rx);
}

void yew_rpctx_init(RpcTx *tx)
{
    bytebuf_init(&tx->pending);
    tx->sent = 0U;
}

void yew_rpctx_send(RpcTx *tx, const u8 *body, u64 n)
{
    if (n > YEW_RPC_MAX_BODY)
        return;
    bytebuf_printf(&tx->pending, "Content-Length: %llu\r\n\r\n",
                   (unsigned long long)n);
    bytebuf_append(&tx->pending, body, (size_t)n);
}

void yew_rpctx_consume(RpcTx *tx, u64 n)
{
    u64 left = (u64)tx->pending.len - tx->sent;

    if (n > left)
        n = left;
    tx->sent += n;
    if (tx->sent == (u64)tx->pending.len) {
        tx->pending.len = 0U;
        tx->sent = 0U;
    }
}

void yew_rpctx_free(RpcTx *tx)
{
    bytebuf_free(&tx->pending);
    tx->sent = 0U;
}

bool yew_rpc_id_u64(const JsonValue *id, u64 *out)
{
    u64 value = 0U;
    u32 i;

    if (id == NULL || out == NULL)
        return false;
    if (id->kind == YEW_JS_INT) {
        if (id->i < 0)
            return false;
        *out = (u64)id->i;
        return true;
    }
    if (id->kind != YEW_JS_STR || id->s.len == 0U)
        return false;
    for (i = 0U; i < id->s.len; i++) {
        u8 digit = id->s.p[i];

        if (digit < (u8)'0' || digit > (u8)'9' ||
            value > (UINT64_MAX - (u64)(digit - (u8)'0')) / 10U)
            return false;
        value = value * 10U + (u64)(digit - (u8)'0');
    }
    *out = value;
    return true;
}

RpcMsgKind yew_rpc_classify(const JsonValue *msg)
{
    const JsonValue *version;
    const JsonValue *id;
    const JsonValue *method;
    const JsonValue *result;
    const JsonValue *error;
    u64 parsed_id;

    if (msg == NULL || msg->kind != YEW_JS_OBJ)
        return YEW_RPC_MALFORMED;
    version = yew_json_get(msg, "jsonrpc");
    id = yew_json_get(msg, "id");
    method = yew_json_get(msg, "method");
    result = yew_json_get(msg, "result");
    error = yew_json_get(msg, "error");
    if (!yew_json_streq(version, "2.0"))
        return YEW_RPC_MALFORMED;
    if (method != NULL) {
        if (method->kind != YEW_JS_STR || result != NULL || error != NULL)
            return YEW_RPC_MALFORMED;
        if (id == NULL)
            return YEW_RPC_NOTIFY;
        return yew_rpc_id_u64(id, &parsed_id) ? YEW_RPC_SRV_REQUEST
                                               : YEW_RPC_MALFORMED;
    }
    if (id == NULL || (result == NULL) == (error == NULL))
        return YEW_RPC_MALFORMED;
    if (id->kind == YEW_JS_NULL && error != NULL)
        return YEW_RPC_ERROR;
    if (!yew_rpc_id_u64(id, &parsed_id))
        return YEW_RPC_MALFORMED;
    return error != NULL ? YEW_RPC_ERROR : YEW_RPC_RESPONSE;
}

RpcErrorAction yew_rpc_error_action(i64 code)
{
    switch (code) {
    case -32700: return YEW_RPC_ERR_PARSE;
    case -32600: return YEW_RPC_ERR_INVALID_REQUEST;
    case -32601: return YEW_RPC_ERR_METHOD_NOT_FOUND;
    case -32602: return YEW_RPC_ERR_INVALID_PARAMS;
    case -32603: return YEW_RPC_ERR_INTERNAL;
    case -32800:
    case -32801:
    case -32802: return YEW_RPC_ERR_SILENT;
    case -32803: return YEW_RPC_ERR_REQUEST_FAILED;
    default:
        if (code >= -32099 && code <= -32000)
            return YEW_RPC_ERR_SERVER_RESERVED;
        return YEW_RPC_ERR_OTHER;
    }
}

static size_t slot_start(u64 id)
{
    return (size_t)((id * UINT64_C(11400714819323198485)) &
                    (YEW_RPC_PENDING_SLOTS - 1U));
}

static RpcPendingSlot *find_slot(RpcConn *c, u64 id, bool inserting)
{
    size_t at = slot_start(id);
    RpcPendingSlot *tomb = NULL;
    size_t n;

    for (n = 0U; n < YEW_RPC_PENDING_SLOTS; n++) {
        RpcPendingSlot *slot = &c->slots[at];

        if (slot->state == RPC_SLOT_EMPTY)
            return inserting ? (tomb != NULL ? tomb : slot) : NULL;
        if (slot->state == RPC_SLOT_TOMB) {
            if (tomb == NULL)
                tomb = slot;
        } else if (slot->pending.id == id) {
            return slot;
        }
        at = (at + 1U) & (YEW_RPC_PENDING_SLOTS - 1U);
    }
    return tomb;
}

static const RpcPendingSlot *find_slot_const(const RpcConn *c, u64 id)
{
    size_t at = slot_start(id);
    size_t n;

    for (n = 0U; n < YEW_RPC_PENDING_SLOTS; n++) {
        const RpcPendingSlot *slot = &c->slots[at];

        if (slot->state == RPC_SLOT_EMPTY)
            return NULL;
        if (slot->state == RPC_SLOT_USED && slot->pending.id == id)
            return slot;
        at = (at + 1U) & (YEW_RPC_PENDING_SLOTS - 1U);
    }
    return NULL;
}

void yew_rpc_conn_init(RpcConn *c)
{
    (void)memset(c, 0, sizeof *c);
    yew_rpcrx_init(&c->rx);
    yew_rpcrx_set_handler(&c->rx, conn_on_value, c);
    yew_rpctx_init(&c->tx);
    c->next_id = 1U;
}

void yew_rpc_conn_free(RpcConn *c)
{
    size_t i;

    if (c == NULL)
        return;
    for (i = 0U; i < YEW_RPC_PENDING_SLOTS; i++) {
        RpcPendingSlot *slot = &c->slots[i];

        if (slot->state == RPC_SLOT_USED && slot->pending.release != NULL)
            slot->pending.release(slot->pending.ctx);
    }
    yew_rpcrx_free(&c->rx);
    yew_rpctx_free(&c->tx);
    (void)memset(c, 0, sizeof *c);
}

void yew_rpc_set_handler(RpcConn *c, RpcValueFn on_value, void *ctx)
{
    if (c == NULL)
        return;
    c->on_value = on_value;
    c->value_ctx = ctx;
}

static void conn_on_value(void *ctx, const JsonValue *value)
{
    RpcConn *c = ctx;

    if (yew_rpc_classify(value) == YEW_RPC_MALFORMED) {
        if (c->malformed < UINT8_MAX)
            c->malformed++;
        if (c->malformed >= 8U)
            rpc_dead(&c->rx, "too many malformed JSON-RPC messages");
    }
    if (c->on_value != NULL)
        c->on_value(c->value_ctx, value);
}

bool yew_rpc_feed_stdout(void *owner, const u8 *bytes, u64 n)
{
    RpcConn *c = owner;

    if (c == NULL)
        return false;
    (void)yew_rpcrx_feed(&c->rx, bytes, n, NULL, NULL);
    return c->rx.state != YEW_RPCRX_DEAD;
}

bool yew_rpc_finish_stdout(void *owner)
{
    RpcConn *c = owner;

    if (c == NULL)
        return false;
    if (c->rx.state == YEW_RPCRX_DEAD)
        return false;
    if (c->rx.state == YEW_RPCRX_BODY || c->rx.pending_cr ||
        c->rx.line.len != 0U || c->rx.have_len) {
        rpc_dead(&c->rx, "unexpected EOF in JSON-RPC frame");
        return false;
    }
    return true;
}

u64 yew_rpc_tx_view(void *owner, const u8 **bytes)
{
    RpcConn *c = owner;

    if (bytes != NULL)
        *bytes = NULL;
    if (c == NULL || bytes == NULL || c->tx.sent > (u64)c->tx.pending.len)
        return 0U;
    if (c->tx.sent == (u64)c->tx.pending.len)
        return 0U;
    *bytes = c->tx.pending.data + (size_t)c->tx.sent;
    return (u64)c->tx.pending.len - c->tx.sent;
}

void yew_rpc_tx_consume(void *owner, u64 n)
{
    RpcConn *c = owner;

    if (c != NULL)
        yew_rpctx_consume(&c->tx, n);
}

void yew_rpc_destroy(void *owner)
{
    RpcConn *c = owner;

    if (c != NULL) {
        yew_rpc_conn_free(c);
        yew_xfree(c);
    }
}

static void append_method(Bytebuf *body, const char *method)
{
    JsonW w;

    yew_jsonw_init(&w, body);
    yew_jsonw_cstr(&w, method);
}

u64 yew_rpc_call(RpcConn *c, const char *method, const u8 *params,
                 u32 nparams, const RpcPending *p)
{
    Bytebuf body;
    RpcPendingSlot *slot;
    RpcPending copy;
    u64 id;

    if (c == NULL || method == NULL || c->npending >= YEW_RPC_MAX_PENDING ||
        c->next_id == 0U || c->next_id > (u64)INT64_MAX)
        return 0U;
    id = c->next_id++;
    slot = find_slot(c, id, true);
    if (slot == NULL)
        return 0U;
    (void)memset(&copy, 0, sizeof copy);
    if (p != NULL)
        copy = *p;
    copy.id = id;
    if (copy.deadline_ms <= copy.sent_ms) {
        i64 timeout = strcmp(method, "initialize") == 0
                          ? YEW_RPC_INIT_TIMEOUT_MS
                          : YEW_RPC_TIMEOUT_MS;
        copy.deadline_ms = copy.sent_ms + timeout;
    }
    slot->pending = copy;
    slot->state = RPC_SLOT_USED;
    c->npending++;

    bytebuf_init(&body);
    APPEND_LIT(&body, "{\"jsonrpc\":\"2.0\",\"id\":");
    bytebuf_printf(&body, "%llu,\"method\":", (unsigned long long)id);
    append_method(&body, method);
    APPEND_LIT(&body, ",\"params\":");
    if (params != NULL && nparams != 0U)
        bytebuf_append(&body, params, nparams);
    else
        APPEND_LIT(&body, "null");
    bytebuf_push_u8(&body, (u8)'}');
    yew_rpctx_send(&c->tx, body.data, (u64)body.len);
    bytebuf_free(&body);
    return id;
}

void yew_rpc_notify(RpcConn *c, const char *method, const u8 *params,
                    u32 nparams)
{
    Bytebuf body;

    if (c == NULL || method == NULL)
        return;
    bytebuf_init(&body);
    APPEND_LIT(&body, "{\"jsonrpc\":\"2.0\",\"method\":");
    append_method(&body, method);
    APPEND_LIT(&body, ",\"params\":");
    if (params != NULL && nparams != 0U)
        bytebuf_append(&body, params, nparams);
    else
        APPEND_LIT(&body, "null");
    bytebuf_push_u8(&body, (u8)'}');
    yew_rpctx_send(&c->tx, body.data, (u64)body.len);
    bytebuf_free(&body);
}

static void append_id(Bytebuf *body, const JsonValue *id)
{
    JsonW w;

    yew_jsonw_init(&w, body);
    yew_jsonw_value(&w, id);
}

void yew_rpc_reply(RpcConn *c, const JsonValue *id, const u8 *result,
                   u32 nresult)
{
    Bytebuf body;

    if (c == NULL || id == NULL)
        return;
    bytebuf_init(&body);
    APPEND_LIT(&body, "{\"jsonrpc\":\"2.0\",\"id\":");
    append_id(&body, id);
    APPEND_LIT(&body, ",\"result\":");
    if (result != NULL && nresult != 0U)
        bytebuf_append(&body, result, nresult);
    else
        APPEND_LIT(&body, "null");
    bytebuf_push_u8(&body, (u8)'}');
    yew_rpctx_send(&c->tx, body.data, (u64)body.len);
    bytebuf_free(&body);
}

void yew_rpc_reply_error(RpcConn *c, const JsonValue *id, i32 code,
                         const char *msg)
{
    Bytebuf body;

    if (c == NULL || id == NULL || msg == NULL)
        return;
    bytebuf_init(&body);
    APPEND_LIT(&body, "{\"jsonrpc\":\"2.0\",\"id\":");
    append_id(&body, id);
    bytebuf_printf(&body, ",\"error\":{\"code\":%ld,\"message\":",
                   (long)code);
    append_method(&body, msg);
    APPEND_LIT(&body, "}}");
    yew_rpctx_send(&c->tx, body.data, (u64)body.len);
    bytebuf_free(&body);
}

void yew_rpc_cancel(RpcConn *c, u64 id)
{
    Bytebuf params;

    if (c == NULL || find_slot(c, id, false) == NULL)
        return;
    bytebuf_init(&params);
    bytebuf_printf(&params, "{\"id\":%llu}", (unsigned long long)id);
    yew_rpc_notify(c, "$/cancelRequest", params.data, (u32)params.len);
    bytebuf_free(&params);
}

const RpcPending *yew_rpc_pending(const RpcConn *c, u64 id)
{
    const RpcPendingSlot *slot;

    if (c == NULL)
        return NULL;
    slot = find_slot_const(c, id);
    return slot != NULL ? &slot->pending : NULL;
}

bool yew_rpc_dispatch(RpcConn *c, Ed *ed, const JsonValue *msg)
{
    RpcMsgKind kind = yew_rpc_classify(msg);
    const JsonValue *id;
    const JsonValue *result;
    const JsonValue *error;
    RpcPendingSlot *slot;
    RpcPending pending;
    u64 number;

    if (c == NULL || (kind != YEW_RPC_RESPONSE && kind != YEW_RPC_ERROR))
        return false;
    id = yew_json_get(msg, "id");
    if (!yew_rpc_id_u64(id, &number))
        return false;
    slot = find_slot(c, number, false);
    if (slot == NULL || slot->state != RPC_SLOT_USED)
        return false;
    pending = slot->pending;
    (void)memset(&slot->pending, 0, sizeof(slot->pending));
    slot->state = RPC_SLOT_TOMB;
    c->npending--;
    result = yew_json_get(msg, "result");
    error = yew_json_get(msg, "error");
    if (pending.cb != NULL)
        pending.cb(ed, pending.ctx, result, error);
    if (pending.release != NULL)
        pending.release(pending.ctx);
    return true;
}

bool yew_rpc_drop(RpcConn *c, u64 id)
{
    RpcPendingSlot *slot;
    RpcPending pending;

    if (c == NULL)
        return false;
    slot = find_slot(c, id, false);
    if (slot == NULL || slot->state != RPC_SLOT_USED)
        return false;
    pending = slot->pending;
    (void)memset(&slot->pending, 0, sizeof(slot->pending));
    slot->state = RPC_SLOT_TOMB;
    c->npending--;
    if (pending.release != NULL)
        pending.release(pending.ctx);
    return true;
}

u32 yew_rpc_sweep(RpcConn *c, Ed *ed, i64 now_ms)
{
    JsonValue code;
    JsonValue message;
    JsonMember members[2];
    JsonValue error;
    u32 expired = 0U;
    size_t i;

    if (c == NULL)
        return 0U;
    (void)memset(&code, 0, sizeof code);
    code.kind = YEW_JS_INT;
    code.i = -32001;
    (void)memset(&message, 0, sizeof message);
    message.kind = YEW_JS_STR;
    message.s.p = (const u8 *)"timed out";
    message.s.len = 9U;
    members[0].key = (const u8 *)"code";
    members[0].klen = 4U;
    members[0].val = &code;
    members[1].key = (const u8 *)"message";
    members[1].klen = 7U;
    members[1].val = &message;
    (void)memset(&error, 0, sizeof error);
    error.kind = YEW_JS_OBJ;
    error.obj.m = members;
    error.obj.n = 2U;

    for (i = 0U; i < YEW_RPC_PENDING_SLOTS; i++) {
        RpcPendingSlot *slot = &c->slots[i];
        RpcPending pending;

        if (slot->state != RPC_SLOT_USED || slot->pending.deadline_ms < 0 ||
            now_ms < slot->pending.deadline_ms)
            continue;
        pending = slot->pending;
        (void)memset(&slot->pending, 0, sizeof(slot->pending));
        slot->state = RPC_SLOT_TOMB;
        c->npending--;
        expired++;
        if (pending.cb != NULL)
            pending.cb(ed, pending.ctx, NULL, &error);
        if (pending.release != NULL)
            pending.release(pending.ctx);
    }
    return expired;
}

i64 yew_rpc_deadline(const RpcConn *c)
{
    i64 deadline = -1;
    size_t i;

    if (c == NULL)
        return -1;
    for (i = 0U; i < YEW_RPC_PENDING_SLOTS; i++) {
        const RpcPendingSlot *slot = &c->slots[i];

        if (slot->state == RPC_SLOT_USED && slot->pending.deadline_ms >= 0 &&
            (deadline < 0 || slot->pending.deadline_ms < deadline))
            deadline = slot->pending.deadline_ms;
    }
    return deadline;
}

i64 yew_rpc_job_deadline(const void *owner)
{
    return yew_rpc_deadline(owner);
}

void yew_rpc_job_tick(void *owner, Ed *ed, i64 now_ms)
{
    (void)yew_rpc_sweep(owner, ed, now_ms);
}
