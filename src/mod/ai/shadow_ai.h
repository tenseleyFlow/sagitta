#ifndef YEW_MOD_AI_SHADOW_AI_H
#define YEW_MOD_AI_SHADOW_AI_H

#include <stdbool.h>

#include "edit/job.h"
#include "edit/shadow.h"
#include "mod/ai/backend.h"
#include "mod/ai/context.h"
#include "mod/ai/http.h"
#include "mod/ai/stream.h"
#include "util/arena.h"
#include "util/base.h"
#include "util/buf.h"

typedef struct AiBackendEntry AiBackendEntry;
typedef struct Ed Ed;

typedef struct AiCall {
    Ed *ed;
    AiBackendEntry *entry;
    AiBackend backend;
    AiCtx context;
    AiAdapterState adapter;
    AiStream stream;
    Arena arena;
    Bytebuf raw;
    Bytebuf text;
    Bytebuf body;
    Bytebuf response;
    Bytebuf curl_config;
    Bytebuf curl_err;
    AiErr error;
    HttpConn *conn;
    u32 job;
    u32 seq;
    u32 buf_id;
    u64 buf_gen;
    ByteOff pos;
    u32 delivered;
    u32 ntokens;
    u16 status;
    i64 retry_after_ms;
    i64 t_armed;
    i64 t_sent;
    i64 t_first_token;
    i64 t_done;
    bool active;
    bool live;
    bool dirty;
    bool transport_done;
    bool terminal;
    bool failed;
    bool abort_pending;
    bool counted_delivery;
} AiCall;

typedef bool (*AiWorkspaceAllowedFn)(Ed *ed, const char *root);
typedef bool (*AiPathExcludedFn)(Ed *ed, const char *path);

void yew_ai_workspace_policy_set(AiWorkspaceAllowedFn allowed);
void yew_ai_path_policy_set(AiPathExcludedFn excluded);

void yew_ai_shadow_init(Ed *ed);
void yew_ai_shadow_pump(Ed *ed);
i64 yew_ai_shadow_deadline(const Ed *ed, i64 now_ms);
void yew_ai_shadow_free(Ed *ed);
void yew_ai_call_abort(Ed *ed, AiCall *call, AiErrKind why);

void yew_ai_shadow_accept_note(Ed *ed, u32 seq, u8 kind, u64 bytes);
void yew_ai_shadow_dismiss_note(Ed *ed, u32 seq);

#endif
