#ifndef YEW_MOD_AI_AI_INT_H
#define YEW_MOD_AI_AI_INT_H

#include "mod/ai/backend_curl.h"
#include "mod/ai/http.h"
#include "mod/ai/key.h"
#include "mod/ai/registry.h"

struct AiState {
    AiKeyCache keys;
    HttpState *http;
    AiBackendRegistry backends;
    AiCurlProbe curl;
};

#endif
