#include "fuzzlib.h"

#include <stdio.h>
#include <string.h>

#include "mod/lsp/json.h"
#include "mod/lsp/jsonrpc.h"
#include "util/arena.h"
#include "util/buf.h"

typedef struct {
    u64 delivered;
    u64 hash;
    bool valid;
    char *why;
    size_t why_cap;
} Delivered;

typedef struct {
    u8 state;
    bool pending_cr;
    bool have_len;
    u16 nhdr;
    u64 want;
    u64 delivered;
    u64 hash;
    size_t line_len;
    size_t body_len;
    u64 line_hash;
    u64 body_hash;
} RxResult;

static u64 hash_bytes(u64 hash, const u8 *bytes, size_t len)
{
    size_t i;

    for (i = 0U; i < len; i++) {
        hash ^= bytes[i];
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

static void receive_body(void *ctx, const u8 *body, u64 n)
{
    Delivered *got = ctx;
    Arena arena;
    JsonErr err;
    JsonValue *root;

    if (!got->valid)
        return;
    if (n > YEW_RPC_MAX_BODY || n > (u64)SIZE_MAX) {
        (void)snprintf(got->why, got->why_cap,
                       "framer delivered an oversized body");
        got->valid = false;
        return;
    }
    arena_init(&arena);
    root = yew_json_parse(&arena, body, n, &err);
    if (root == NULL || root->kind != YEW_JS_OBJ) {
        (void)snprintf(got->why, got->why_cap,
                       "framer delivered a non-object JSON body");
        got->valid = false;
        arena_free_all(&arena);
        return;
    }
    arena_free_all(&arena);
    got->hash = hash_bytes(got->hash, body, (size_t)n);
    got->hash ^= n;
    got->hash *= UINT64_C(1099511628211);
    got->delivered++;
}

static size_t next_chunk(const u8 *data, size_t len, size_t at,
                         unsigned schedule)
{
    size_t chunk;

    if (schedule == 0U)
        return len - at;
    if (schedule == 1U)
        return 1U;
    chunk = 1U + (size_t)(data[at] & 31U);
    if (chunk > len - at)
        chunk = len - at;
    return chunk;
}

static bool run_stream(const u8 *data, size_t len, unsigned schedule,
                       RxResult *result, char *why, size_t why_cap)
{
    RpcRx rx;
    Delivered got;
    size_t at = 0U;

    (void)memset(&got, 0, sizeof got);
    got.hash = UINT64_C(1469598103934665603);
    got.valid = true;
    got.why = why;
    got.why_cap = why_cap;
    yew_rpcrx_init(&rx);
    while (at < len && rx.state != YEW_RPCRX_DEAD) {
        size_t chunk = next_chunk(data, len, at, schedule);

        (void)yew_rpcrx_feed(&rx, data + at, (u64)chunk,
                             receive_body, &got);
        at += chunk;
    }
    if (!got.valid) {
        yew_rpcrx_free(&rx);
        return false;
    }
    if (rx.state == YEW_RPCRX_DEAD && rx.err[0] == '\0') {
        (void)snprintf(why, why_cap,
                       "dead framing state has no diagnostic");
        yew_rpcrx_free(&rx);
        return false;
    }

    result->state = rx.state;
    result->pending_cr = rx.pending_cr;
    result->have_len = rx.have_len;
    result->nhdr = rx.nhdr;
    result->want = rx.want;
    result->delivered = got.delivered;
    result->hash = got.hash;
    result->line_len = rx.line.len;
    result->body_len = rx.body.len;
    result->line_hash = hash_bytes(UINT64_C(1469598103934665603),
                                   rx.line.data, rx.line.len);
    result->body_hash = hash_bytes(UINT64_C(1469598103934665603),
                                   rx.body.data, rx.body.len);
    yew_rpcrx_free(&rx);
    return true;
}

static bool same_result(const RxResult *a, const RxResult *b)
{
    return a->state == b->state &&
           a->pending_cr == b->pending_cr &&
           a->have_len == b->have_len &&
           a->nhdr == b->nhdr &&
           a->want == b->want &&
           a->delivered == b->delivered &&
           a->hash == b->hash &&
           a->line_len == b->line_len &&
           a->body_len == b->body_len &&
           a->line_hash == b->line_hash &&
           a->body_hash == b->body_hash;
}

static bool check_jsonrpc(const u8 *data, size_t len,
                          char *why, size_t why_cap)
{
    RxResult whole;
    RxResult bytewise;
    RxResult varied;

    if (!run_stream(data, len, 0U, &whole, why, why_cap) ||
        !run_stream(data, len, 1U, &bytewise, why, why_cap) ||
        !run_stream(data, len, 2U, &varied, why, why_cap))
        return false;
    if (!same_result(&whole, &bytewise) ||
        !same_result(&whole, &varied)) {
        (void)snprintf(why, why_cap,
                       "framing result depends on input chunk boundaries");
        return false;
    }
    return true;
}

static bool check_valid_frame(const u8 *data, size_t len,
                              char *why, size_t why_cap)
{
    Bytebuf body;
    JsonW writer;
    RpcTx tx;
    RxResult whole;
    RxResult bytewise;
    RxResult varied;
    bool ok;

    bytebuf_init(&body);
    yew_jsonw_init(&writer, &body);
    yew_jsonw_obj(&writer);
    yew_jsonw_key(&writer, "data");
    yew_jsonw_str(&writer, data, (u32)len);
    yew_jsonw_obj_end(&writer);
    yew_rpctx_init(&tx);
    yew_rpctx_send(&tx, body.data, (u64)body.len);

    ok = run_stream(tx.pending.data, tx.pending.len, 0U,
                    &whole, why, why_cap) &&
         run_stream(tx.pending.data, tx.pending.len, 1U,
                    &bytewise, why, why_cap) &&
         run_stream(tx.pending.data, tx.pending.len, 2U,
                    &varied, why, why_cap);
    if (ok && (!same_result(&whole, &bytewise) ||
               !same_result(&whole, &varied))) {
        (void)snprintf(why, why_cap,
                       "valid frame depends on input chunk boundaries");
        ok = false;
    }
    if (ok && (whole.state != YEW_RPCRX_HDR ||
               whole.delivered != 1U || whole.body_len != 0U)) {
        (void)snprintf(why, why_cap,
                       "valid frame was not delivered exactly once");
        ok = false;
    }
    yew_rpctx_free(&tx);
    bytebuf_free(&body);
    return ok;
}

static bool check_input(const u8 *data, size_t len,
                        char *why, size_t why_cap)
{
    return check_jsonrpc(data, len, why, why_cap) &&
           check_valid_frame(data, len, why, why_cap);
}

int main(int argc, char **argv)
{
    return yew_fuzz_main(argc, argv, "fuzz_jsonrpc", NULL,
                         check_input);
}
