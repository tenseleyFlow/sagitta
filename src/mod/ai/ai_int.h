#ifndef YEW_MOD_AI_AI_INT_H
#define YEW_MOD_AI_AI_INT_H

#include "mod/ai/backend_curl.h"
#include "mod/ai/http.h"
#include "mod/ai/key.h"
#include "mod/ai/redact.h"
#include "mod/ai/registry.h"
#include "mod/ai/shadow_ai.h"
#include "mod/ai/stats.h"
#include "util/buf.h"

typedef struct AiCommandCall AiCommandCall;

struct AiState {
    AiKeyCache keys;
    HttpState *http;
    AiBackendRegistry backends;
    AiCurlProbe curl;
    Bytebuf log;
    AiCommandCall *command_call;
    AiStatsState *stats;
    AiRedactPolicy *redact;
    AiPathPolicy *paths;
    AiCall call;
    u32 retired_jobs[YEW_JOB_MAX];
    u32 nretired_jobs;
    i64 last_deliver_ms;
    u32 suggestion_seq;
    char suggestion_backend[128];
    bool curl_probe_messaged;
    bool curl_backends_waiting;
    bool redact_elide_messaged;
    bool redact_off_messaged;
    bool workspace_prompted;
    AiWsGrant session_workspace_grant;
    /* Last completed non-cancelled result, retained for the status badge. */
    AiErrKind last_error;
    bool have_last_error;
};

void yew_ai_command_pump(Ed *ed);
void yew_ai_command_cancel(Ed *ed);

#endif
