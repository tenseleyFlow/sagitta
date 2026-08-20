#ifndef YEW_MOD_AI_AI_INT_H
#define YEW_MOD_AI_AI_INT_H

#include "mod/ai/backend_curl.h"
#include "mod/ai/http.h"
#include "mod/ai/key.h"
#include "mod/ai/registry.h"
#include "util/buf.h"

typedef struct AiCommandCall AiCommandCall;

struct AiState {
    AiKeyCache keys;
    HttpState *http;
    AiBackendRegistry backends;
    AiCurlProbe curl;
    Bytebuf log;
    AiCommandCall *command_call;
    bool curl_backends_waiting;
};

void yew_ai_command_pump(Ed *ed);
void yew_ai_command_cancel(Ed *ed);

#endif
