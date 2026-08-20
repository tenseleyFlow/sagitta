#include "mod/ai/ai.h"

#include <stdio.h>
#include <stdlib.h>

#include "edit/ed.h"
#include "mod/ai/ai_int.h"
#include "mod/ai/key.h"
#include "ui/message.h"
#include "util/base.h"

static bool backend_prepare(void *ctx, const char *url_text, HttpUrl *url,
                            u8 transport, void **endpoint, char *err,
                            size_t errsz)
{
    Ed *ed = ctx;
    AiErr failure;

    (void)url_text;
    *endpoint = NULL;
    if (transport != (u8)YEW_AI_TR_HTTP)
        return true;
    if (yew_http_register_endpoint(ed, url, &failure))
        return true;
    if (err != NULL && errsz != 0U)
        (void)snprintf(err, errsz, "%s", failure.msg);
    return false;
}

/* Ownership root for Sprint 48 and the following AI sprints.  Concrete
 * transport objects remain private to the module; expanding this structure
 * cannot make Ed or stripped builds depend on their representation. */
void yew_ai_state_init(Ed *ed)
{
    AiState *state;

    if (ed == NULL || ed->ai != NULL)
        return;
    state = yew_xcalloc(1U, sizeof(*state));
    yew_ai_key_cache_init(&state->keys);
    state->http = yew_http_state_new();
    ed->ai = state;
    yew_ai_registry_init(&state->backends, backend_prepare, NULL, ed);
    yew_ai_curl_probe_init(&state->curl);
}

void yew_ai_state_free(Ed *ed)
{
    AiState *state;

    if (ed == NULL || ed->ai == NULL)
        return;
    state = ed->ai;
    yew_ai_curl_probe_free(&state->curl);
    yew_ai_registry_drop(&state->backends);
    yew_http_state_free(state->http);
    yew_ai_key_cache_drop(&state->keys);
    ed->ai = NULL;
    free(state);
}

bool yew_ai_state_ready(const Ed *ed)
{
    return ed != NULL && ed->ai != NULL;
}

void yew_ai_state_key_cache_enable(Ed *ed, bool enabled)
{
    if (ed == NULL || ed->ai == NULL)
        return;
    yew_ai_key_cache_enable(&ed->ai->keys, enabled);
}

bool yew_ai_state_key_cache_enabled(const Ed *ed)
{
    return ed != NULL && ed->ai != NULL && ed->ai->keys.enabled;
}

bool yew_ai_backend_define(Ed *ed, const FlStr *name, const FlMap *config,
                           char *err, size_t errsz)
{
    if (ed == NULL || ed->ai == NULL) {
        if (err != NULL && errsz != 0U)
            (void)snprintf(err, errsz, "AI state is unavailable");
        return false;
    }
    return yew_ai_registry_put(&ed->ai->backends, name, config, err, errsz);
}

u32 yew_ai_backend_count(const Ed *ed)
{
    return ed == NULL || ed->ai == NULL ? 0U :
           yew_ai_registry_count(&ed->ai->backends);
}

const AiBackendEntry *yew_ai_backend_at(const Ed *ed, u32 index)
{
    return ed == NULL || ed->ai == NULL ? NULL :
           yew_ai_registry_at(&ed->ai->backends, index);
}

void yew_ai_collect_fds(Ed *ed, struct pollfd *pfd, u32 *n)
{
    yew_http_collect_fds(ed, pfd, n);
}

void yew_ai_pump(Ed *ed, const struct pollfd *pfd, u32 n)
{
    yew_http_pump(ed, pfd, n);
}

i64 yew_ai_deadline(const Ed *ed, i64 now_ms)
{
    return yew_http_deadline(ed, now_ms);
}

CmdStatus yew_ai_cmd_off(CmdCtx *cx)
{
    if (cx == NULL || cx->ed == NULL)
        return YEW_CMD_ERR_STATE;
    yew_msg(cx->ed, YEW_MSG_INFO,
            "AI is off; :ai enable turns it on (Sprint 50)");
    return YEW_CMD_ERR_STATE;
}

CmdStatus yew_ai_cmd_require(CmdCtx *cx)
{
    /* An enabled build reaches this only if a descriptor is wired wrong. */
    return yew_ai_cmd_off(cx);
}
