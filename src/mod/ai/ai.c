#include "mod/ai/ai.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "edit/ed.h"
#include "edit/option.h"
#include "fl/flconf.h"
#include "mod/ai/ai_int.h"
#include "mod/ai/key.h"
#include "mod/ai/redact.h"
#include "mod/ai/stats.h"
#include "mod/ai/shadow_ai.h"
#include "ui/message.h"
#include "util/base.h"

static bool ai_redact_policy_check(Ed *ed, const AiCtx *context,
                                   RedactHit *hit)
{
    return ed != NULL && ed->ai != NULL &&
           yew_ai_redact_scan(ed->ai->redact, context, hit);
}

static bool ai_path_policy_check(Ed *ed, const char *path)
{
    return ed != NULL && ed->ai != NULL &&
           yew_ai_path_excluded(ed->ai->paths, path, NULL);
}

static AiWsGrant ai_workspace_resolve(Ed *ed, bool notify)
{
    AiWsGrant grant;
    OptVal value;

    if (ed == NULL || ed->ai == NULL)
        return YEW_AI_WS_DENY;
    grant = yew_ai_workspace_grant(ed);
    if (grant != YEW_AI_WS_UNSET)
        return grant;
    if (yew_opt_get(ed, NULL, NULL, "ai.default_workspace", 20U, &value) &&
        value.type == (u8)YEW_OPT_ENUM && value.as.str.s != NULL) {
        if (value.as.str.len == 5U &&
            memcmp(value.as.str.s, "allow", 5U) == 0)
            return YEW_AI_WS_ALLOW;
        if (value.as.str.len == 4U &&
            memcmp(value.as.str.s, "deny", 4U) == 0)
            return YEW_AI_WS_DENY;
    }
    if (notify && !ed->ai->workspace_prompted) {
        yew_msg(ed, YEW_MSG_INFO,
                "AI is off in this workspace; :ai enable grants it");
        ed->ai->workspace_prompted = true;
    }
    return YEW_AI_WS_DENY;
}

static bool ai_workspace_policy_check(Ed *ed, const char *root)
{
    (void)root;
    return yew_ai_workspace_allowed(ed);
}

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
    state->redact = yew_ai_redact_policy_new(NULL, 0U, false, NULL);
    state->paths = yew_ai_path_policy_new(NULL, 0U, false, NULL);
    if (state->redact == NULL || state->paths == NULL)
        YEW_BUG("failed to install shipped AI privacy policy");
    yew_ai_redact_hook_set(ai_redact_policy_check);
    yew_ai_path_policy_set(ai_path_policy_check);
    yew_ai_workspace_policy_set(ai_workspace_policy_check);
    state->last_deliver_ms = -1;
    bytebuf_append(&state->log, "AI transport log\n", 17U);
}

void yew_ai_state_free(Ed *ed)
{
    AiState *state;

    if (ed == NULL || ed->ai == NULL)
        return;
    state = ed->ai;
    yew_ai_optin_cancel(&state->optin);
    yew_ai_shadow_free(ed);
    yew_ai_command_cancel(ed);
    yew_ai_curl_probe_free(&state->curl);
    yew_ai_registry_drop(&state->backends);
    yew_http_state_free(state->http);
    yew_ai_key_cache_drop(&state->keys);
    yew_ai_stats_free(ed, state->stats);
    yew_ai_redact_policy_free(state->redact);
    yew_ai_path_policy_free(state->paths);
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

static bool ai_bool(const Ed *ed, const char *name, bool fallback)
{
    OptVal v;
    size_t n = strlen(name);
    return yew_opt_get((Ed *)ed, NULL, NULL, name, (u32)n, &v) &&
                   v.type == (u8)YEW_OPT_BOOL ? v.as.b : fallback;
}

static i64 ai_int(const Ed *ed, const char *name, i64 fallback)
{
    OptVal v;
    size_t n = strlen(name);
    return yew_opt_get((Ed *)ed, NULL, NULL, name, (u32)n, &v) &&
                   v.type == (u8)YEW_OPT_INT ? v.as.i : fallback;
}

static bool ai_badge_on(const Ed *ed)
{
    OptVal v;
    const char *s;
    if (!yew_opt_get((Ed *)ed, NULL, NULL, "ai.badge", 8U, &v) ||
        v.type != (u8)YEW_OPT_ENUM)
        return true;
    s = v.as.str.s;
    return s == NULL || v.as.str.len != 3U || memcmp(s, "off", 3U) != 0;
}

bool yew_ai_status_badge(const Ed *ed, char *out, size_t outsz, u8 *priority)
{
    const AiBackendEntry *entry;
    const AiCall *call;
    const char *host;
    bool remote;
    bool active;
    const char *state = "";
    i64 max;

    if (out == NULL || outsz == 0U || priority == NULL || ed == NULL ||
        ed->ai == NULL || !ai_bool(ed, "ai.enable", false))
        return false;
    if (ai_workspace_resolve((Ed *)ed, false) != YEW_AI_WS_ALLOW)
        return false;
    call = &ed->ai->call;
    entry = call->active ? call->entry : yew_ai_backend_selected(ed);
    if (entry == NULL)
        return false;
    remote = !entry->backend.url.loopback;
    if (!ai_badge_on(ed))
        return false;
    active = call->active;
    if (active)
        state = "~";
    else if (ed->ai->have_last_error)
        state = "!";
    host = entry->backend.url.host == NULL ? "" : entry->backend.url.host;
    max = ai_int(ed, "ai.badge_host_max", 20);
    if (max < 1)
        max = 1;
    *priority = remote ? 2U : 5U;
    if (!remote) {
        (void)snprintf(out, outsz, "[AI%s]", state);
        return true;
    }
    if ((i64)strlen(host) <= max) {
        (void)snprintf(out, outsz, "[AI->%s%s]", host, state);
    } else {
        size_t keep = (size_t)(max > 1 ? max - 1 : 0);
        size_t n = strlen(host);
        if (keep > n)
            keep = n;
        (void)snprintf(out, outsz, "[AI->%s%s%s]", max > 1 ? "…" : "",
                       host + n - keep, state);
    }
    return true;
}

void yew_ai_status_note(Ed *ed, AiErrKind kind)
{
    if (ed == NULL || ed->ai == NULL || kind == YEW_AI_OK ||
        kind == YEW_AI_ERR_CANCELLED)
        return;
    ed->ai->last_error = kind;
    ed->ai->have_last_error = true;
}

void yew_ai_status_clear(Ed *ed)
{
    if (ed == NULL || ed->ai == NULL)
        return;
    ed->ai->last_error = YEW_AI_OK;
    ed->ai->have_last_error = false;
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

bool yew_ai_backend_name_is_remote(const Ed *ed, const char *name, u32 len)
{
    const AiBackendEntry *entry;
    char selected[128];
    if (ed == NULL || ed->ai == NULL || name == NULL || len == 0U ||
        len >= sizeof(selected))
        return false;
    (void)memcpy(selected, name, len);
    selected[len] = '\0';
    entry = yew_ai_registry_find(&ed->ai->backends, selected);
    return entry != NULL && !entry->backend.url.loopback;
}

AiWsGrant yew_ai_workspace_grant(Ed *ed)
{
    if (ed == NULL || ed->ai == NULL)
        return YEW_AI_WS_UNSET;
    if (ed->ai->session_workspace_grant != YEW_AI_WS_UNSET)
        return ed->ai->session_workspace_grant;
    return yew_config_ai_workspace_grant(ed);
}

bool yew_ai_workspace_allowed(Ed *ed)
{
    return ai_workspace_resolve(ed, true) == YEW_AI_WS_ALLOW;
}

void yew_ai_workspace_set(Ed *ed, AiWsGrant grant)
{
    if (ed == NULL || ed->ai == NULL)
        return;
    ed->ai->session_workspace_grant = YEW_AI_WS_UNSET;
    if (!yew_config_ai_workspace_set(ed, grant))
        yew_msg(ed, YEW_MSG_ERROR, "could not write the AI workspace grant");
}

void yew_ai_workspace_session_set(Ed *ed, AiWsGrant grant)
{
    if (ed == NULL || ed->ai == NULL)
        return;
    ed->ai->session_workspace_grant = grant;
    ed->ai->workspace_prompted = false;
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
