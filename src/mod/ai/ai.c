#include "mod/ai/ai.h"

#include <stdio.h>
#include <stdlib.h>

#include "edit/ed.h"
#include "edit/option.h"
#include "mod/ai/ai_int.h"
#include "mod/ai/key.h"
#include "mod/ai/stats.h"
#include "mod/ai/shadow_ai.h"
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
    bytebuf_init(&state->log);
    state->stats = yew_ai_stats_new();
    state->last_deliver_ms = -1;
    bytebuf_append(&state->log, "AI transport log\n", 17U);
}

void yew_ai_state_free(Ed *ed)
{
    AiState *state;

    if (ed == NULL || ed->ai == NULL)
        return;
    state = ed->ai;
    yew_ai_shadow_free(ed);
    yew_ai_command_cancel(ed);
    yew_ai_curl_probe_free(&state->curl);
    yew_ai_registry_drop(&state->backends);
    yew_http_state_free(state->http);
    yew_ai_key_cache_drop(&state->keys);
    yew_ai_stats_free(ed, state->stats);
    bytebuf_free(&state->log);
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
    bool ok;

    if (ed == NULL || ed->ai == NULL) {
        if (err != NULL && errsz != 0U)
            (void)snprintf(err, errsz, "AI state is unavailable");
        return false;
    }
    if (ed->ai->command_call != NULL || ed->ai->call.active) {
        if (err != NULL && errsz != 0U)
            (void)snprintf(
                err, errsz,
                "cannot change AI backends while a request is running");
        return false;
    }
    ok = yew_ai_registry_put(&ed->ai->backends, name, config, err, errsz);
    if (ok)
        yew_ai_key_cache_reload(&ed->ai->keys);
    return ok;
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

const AiBackendEntry *yew_ai_backend_selected(const Ed *ed)
{
    OptVal value;

    if (ed == NULL || ed->ai == NULL ||
        !yew_opt_get((Ed *)ed, NULL, NULL, "ai.backend", 10U, &value) ||
        value.type != (u8)YEW_OPT_STR || value.as.str.len == 0U)
        return NULL;
    return yew_ai_registry_find(&ed->ai->backends, value.as.str.s);
}

void yew_ai_collect_fds(Ed *ed, struct pollfd *pfd, u32 *n)
{
    yew_http_collect_fds(ed, pfd, n);
}

void yew_ai_pump(Ed *ed, const struct pollfd *pfd, u32 n)
{
    yew_http_pump(ed, pfd, n);
    yew_ai_command_pump(ed);
    yew_ai_shadow_pump(ed);
    yew_ai_stats_pump(ed, ed == NULL ? 0 : ed->now_ms);
}

i64 yew_ai_deadline(const Ed *ed, i64 now_ms)
{
    i64 http = yew_http_deadline(ed, now_ms);
    i64 shadow = yew_ai_shadow_deadline(ed, now_ms);

    if (http < 0)
        return shadow;
    if (shadow < 0)
        return http;
    return http < shadow ? http : shadow;
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

CmdStatus yew_ai_cmd_open(CmdCtx *cx)
{
    if (cx == NULL || cx->ed == NULL)
        return YEW_CMD_ERR_STATE;
    yew_msg(cx->ed, YEW_MSG_INFO,
            "AI prompt UI is not a 1.0 feature; AI completions use ghost text");
    return YEW_CMD_ERR_STATE;
}
