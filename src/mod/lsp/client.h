#ifndef YEW_MOD_LSP_CLIENT_H
#define YEW_MOD_LSP_CLIENT_H

#include <stdbool.h>

#include "mod/lsp/jsonrpc.h"
#include "mod/lsp/sync.h"
#include "util/base.h"
#include "util/buf.h"
#include "util/vec.h"

typedef struct Buffer Buffer;
typedef struct Ed Ed;
typedef struct LspClient LspClient;

typedef enum {
    YEW_LSP_SPAWNING = 0,
    YEW_LSP_INITIALIZING,
    YEW_LSP_READY,
    YEW_LSP_SHUTTING_DOWN,
    YEW_LSP_DEAD
} LspState;

enum {
    YEW_LSPC_COMPLETION = 1U << 0,
    YEW_LSPC_HOVER = 1U << 1,
    YEW_LSPC_SIGNATURE = 1U << 2,
    YEW_LSPC_DEFINITION = 1U << 3,
    YEW_LSPC_DECLARATION = 1U << 4,
    YEW_LSPC_TYPE_DEFINITION = 1U << 5,
    YEW_LSPC_IMPLEMENTATION = 1U << 6,
    YEW_LSPC_REFERENCES = 1U << 7,
    YEW_LSPC_DOCUMENT_HIGHLIGHT = 1U << 8,
    YEW_LSPC_DOCUMENT_SYMBOL = 1U << 9,
    YEW_LSPC_RENAME = 1U << 10,
    YEW_LSPC_WORKSPACE_SYMBOL = 1U << 11
};

typedef struct LspServerCfg {
    const char *id;
    const char *lang;
    const char *cmd;
    const char *const *args;
    const char *const *roots;
    const char *init_options;
    i32 init_timeout_ms;
} LspServerCfg;

typedef struct LspCaps {
    u32 bits;
    u8 sync_kind;
    bool resolve_completion;
    char trigger_chars[16];
    char sig_trigger[8];
    bool save_supported;
    bool save_include_text;
} LspCaps;

typedef struct LspServer LspServer;

VEC_DECL(VecLspDoc, LspDoc);
VEC_DECL(VecU32Lsp, u32);

struct LspServer {
    u32 id;
    const LspServerCfg *cfg;
    char *root;
    u32 job;
    RpcConn rpc;
    u8 state;
    LspCaps caps;
    u8 pos_enc;
    VecU32Lsp docs;
    VecLspDoc docv;
    u32 restarts;
    i64 first_restart_ms;
    i64 next_try_ms;
    Bytebuf stderr_tail;
    bool rpc_live;
    bool gave_up;
    bool exit_sent;
    u32 missing_warned;
    Ed *owner;
    u64 dropped_stale;
    u64 shadow_request;
    u32 shadow_buf_id;
    u32 shadow_seq;
};

/* The defaults are immutable and may be replaced by compiled init.fl data. */
const LspServerCfg *yew_lsp_default_cfg(const char *lang);

/* Returns a canonical heap path, or NULL when either input is invalid. */
char *yew_lsp_resolve_root(const LspServerCfg *cfg, const char *buffer_path,
                           const char *workspace_root);

void yew_lsp_uri_of_path(Bytebuf *out, const u8 *path, u32 n);
bool yew_lsp_path_of_uri(Bytebuf *out, const u8 *uri, u32 n);

void yew_lsp_caps_parse(LspCaps *out, const JsonValue *initialize_result,
                        u8 *pos_enc, bool *unknown_encoding);
bool yew_lsp_has(const LspServer *s, u32 cap);
void yew_lsp_initialize_params(Bytebuf *out, const LspServerCfg *cfg,
                               const char *root, i64 process_id);

/* Pure lifecycle transition helpers; tests inject the monotonic clock. */
void yew_lsp_server_init(LspServer *s, u32 id,
                         const LspServerCfg *cfg, char *root);
void yew_lsp_server_dispose(LspServer *s);
bool yew_lsp_server_initialized(LspServer *s, const JsonValue *result);
bool yew_lsp_server_crashed(LspServer *s, i64 now_ms,
                            const char *last_stderr, Bytebuf *message);
void yew_lsp_server_restart_reset(LspServer *s);

LspClient *yew_lsp_client_new(void);
void yew_lsp_client_free(Ed *ed);
/* Refreshes the host-owned init.fl server table after config replacement. */
void yew_lsp_client_refresh_config(Ed *ed);
const LspServerCfg *yew_lsp_client_cfg(Ed *ed, const char *lang);
bool yew_lsp_client_start(Ed *ed, Buffer *b);
/* Compiled config entry point used by init.fl overrides and protocol tests. */
bool yew_lsp_client_start_cfg(Ed *ed, Buffer *b,
                              const LspServerCfg *cfg);
void yew_lsp_client_stop(Ed *ed, LspServer *s, bool graceful);
void yew_lsp_client_pump(Ed *ed);
void yew_lsp_client_close_buffer(Ed *ed, Buffer *b);
bool yew_lsp_client_restart(Ed *ed, Buffer *b);

/* Central response gate used by the transport and focused race tests. */
bool yew_lsp_dispatch_response(LspServer *s, const JsonValue *msg);
/* Real notification/request dispatcher; also the fuzz protocol seam. */
void yew_lsp_server_dispatch_value(LspServer *s, const JsonValue *msg);

LspDoc *yew_lsp_doc_for_buffer(Ed *ed, const Buffer *b);
LspServer *yew_lsp_server_for_doc(Ed *ed, const LspDoc *doc);
LspServer *yew_lsp_server_by_id(Ed *ed, u32 id);
LspDoc *yew_lsp_doc_find(const Ed *ed, u32 buf_id,
                         const LspServer **server);
bool yew_lsp_server_pos_enc(const Ed *ed, u32 server, u8 *out);

/* No 1.0 server-pushed file operations, formatting, code actions, inlay
 * hints, semantic tokens, or arbitrary URI schemes. */

#endif
