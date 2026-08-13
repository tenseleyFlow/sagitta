#ifndef YEW_MOD_LSP_JSONRPC_H
#define YEW_MOD_LSP_JSONRPC_H

#include <stdbool.h>

#include "mod/lsp/json.h"
#include "util/base.h"
#include "util/buf.h"

typedef struct Ed Ed;

enum {
    YEW_RPC_MAX_HDRS = 32,
    YEW_RPC_MAX_HDRLINE = 4096,
    YEW_RPC_MAX_PENDING = 256,
    YEW_RPC_PENDING_SLOTS = 512
};

#define YEW_RPC_MAX_BODY (64u * 1024u * 1024u)
#define YEW_RPC_TIMEOUT_MS 10000
#define YEW_RPC_INIT_TIMEOUT_MS 60000

typedef enum {
    YEW_RPCRX_HDR = 0,
    YEW_RPCRX_BODY,
    YEW_RPCRX_DEAD
} RpcRxState;

typedef void (*RpcValueFn)(void *ctx, const JsonValue *msg);

typedef struct RpcRx {
    u8 state;
    bool pending_cr;
    Bytebuf line;
    Bytebuf body;
    u64 want;
    bool have_len;
    u16 nhdr;
    u64 bytes_in;
    u64 msgs_in;
    char err[96];
    RpcValueFn on_value;
    void *value_ctx;
} RpcRx;

typedef void (*RpcMsgFn)(void *ctx, const u8 *body, u64 n);

void yew_rpcrx_init(RpcRx *rx);
void yew_rpcrx_set_handler(RpcRx *rx, RpcValueFn on_value, void *ctx);
u32 yew_rpcrx_feed(RpcRx *rx, const u8 *b, u64 n, RpcMsgFn on_msg,
                   void *ctx);
void yew_rpcrx_free(RpcRx *rx);

typedef struct RpcTx {
    Bytebuf pending;
    u64 sent;
} RpcTx;

void yew_rpctx_init(RpcTx *tx);
void yew_rpctx_send(RpcTx *tx, const u8 *body, u64 n);
void yew_rpctx_consume(RpcTx *tx, u64 n);
void yew_rpctx_free(RpcTx *tx);

typedef enum {
    YEW_RPC_REQUEST = 0,
    YEW_RPC_RESPONSE,
    YEW_RPC_ERROR,
    YEW_RPC_NOTIFY,
    YEW_RPC_SRV_REQUEST,
    YEW_RPC_MALFORMED
} RpcMsgKind;

RpcMsgKind yew_rpc_classify(const JsonValue *msg);
bool yew_rpc_id_u64(const JsonValue *id, u64 *out);

typedef struct RpcPending {
    u64 id;
    u32 method;
    u64 gen;
    u32 buf_id;
    i64 sent_ms;
    i64 deadline_ms;
    void (*cb)(Ed *ed, void *ctx, const JsonValue *result,
               const JsonValue *error);
    void *ctx;
} RpcPending;

typedef enum {
    YEW_RPC_ERR_PARSE = 0,
    YEW_RPC_ERR_INVALID_REQUEST,
    YEW_RPC_ERR_METHOD_NOT_FOUND,
    YEW_RPC_ERR_INVALID_PARAMS,
    YEW_RPC_ERR_INTERNAL,
    YEW_RPC_ERR_SILENT,
    YEW_RPC_ERR_REQUEST_FAILED,
    YEW_RPC_ERR_SERVER_RESERVED,
    YEW_RPC_ERR_OTHER
} RpcErrorAction;

RpcErrorAction yew_rpc_error_action(i64 code);

typedef struct RpcPendingSlot {
    RpcPending pending;
    u8 state;
} RpcPendingSlot;

typedef struct RpcConn {
    RpcRx rx;
    RpcTx tx;
    RpcPendingSlot slots[YEW_RPC_PENDING_SLOTS];
    u64 next_id;
    u16 npending;
    u8 malformed;
    RpcValueFn on_value;
    void *value_ctx;
} RpcConn;

void yew_rpc_conn_init(RpcConn *c);
void yew_rpc_conn_free(RpcConn *c);
void yew_rpc_set_handler(RpcConn *c, RpcValueFn on_value, void *ctx);
bool yew_rpc_feed_stdout(void *owner, const u8 *bytes, u64 n);
bool yew_rpc_finish_stdout(void *owner);
u64 yew_rpc_tx_view(void *owner, const u8 **bytes);
void yew_rpc_tx_consume(void *owner, u64 n);
void yew_rpc_destroy(void *owner);
u64 yew_rpc_call(RpcConn *c, const char *method, const u8 *params,
                 u32 nparams, const RpcPending *p);
void yew_rpc_notify(RpcConn *c, const char *method, const u8 *params,
                    u32 nparams);
void yew_rpc_reply(RpcConn *c, const JsonValue *id, const u8 *result,
                   u32 nresult);
void yew_rpc_reply_error(RpcConn *c, const JsonValue *id, i32 code,
                         const char *msg);
void yew_rpc_cancel(RpcConn *c, u64 id);

/* Dispatches a response/error to its pending callback.  The parsed tree is
 * owned by the caller and is valid only for the duration of the callback. */
bool yew_rpc_dispatch(RpcConn *c, Ed *ed, const JsonValue *msg);
u32 yew_rpc_sweep(RpcConn *c, Ed *ed, i64 now_ms);
i64 yew_rpc_deadline(const RpcConn *c);
i64 yew_rpc_job_deadline(const void *owner);
void yew_rpc_job_tick(void *owner, Ed *ed, i64 now_ms);
const RpcPending *yew_rpc_pending(const RpcConn *c, u64 id);

#endif
