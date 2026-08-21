#include "mod/ai/ai.h"

#include <stdlib.h>

#include "edit/ed.h"
#include "mod/mods.h"
#include "ui/message.h"
#include "util/base.h"

/* Preserve the same ownership and lifecycle shape without linking any AI
 * transport implementation into a stripped build. */
struct AiState {
    bool ready;
    bool key_cache_enabled;
};

void yew_ai_state_init(Ed *ed)
{
    if (ed == NULL || ed->ai != NULL)
        return;
    ed->ai = yew_xcalloc(1U, sizeof(*ed->ai));
    ed->ai->ready = true;
    ed->ai->key_cache_enabled = true;
}

void yew_ai_state_free(Ed *ed)
{
    if (ed == NULL)
        return;
    free(ed->ai);
    ed->ai = NULL;
}

bool yew_ai_state_ready(const Ed *ed)
{
    return ed != NULL && ed->ai != NULL && ed->ai->ready;
}

void yew_ai_state_key_cache_enable(Ed *ed, bool enabled)
{
    if (ed != NULL && ed->ai != NULL)
        ed->ai->key_cache_enabled = enabled;
}

bool yew_ai_state_key_cache_enabled(const Ed *ed)
{
    return ed != NULL && ed->ai != NULL && ed->ai->key_cache_enabled;
}

void yew_ai_policy_options_changed(Ed *ed)
{
    (void)ed;
}

void yew_ai_redact_option_changed(Ed *ed)
{
    (void)ed;
}

bool yew_ai_status_badge(const Ed *ed, char *out, size_t outsz, u8 *priority)
{
    (void)ed;
    if (out != NULL && outsz != 0U)
        out[0] = '\0';
    if (priority != NULL)
        *priority = 5U;
    return false;
}

void yew_ai_status_note(Ed *ed, AiErrKind kind)
{
    (void)ed;
    (void)kind;
}

void yew_ai_status_clear(Ed *ed)
{
    (void)ed;
}

bool yew_ai_backend_define(Ed *ed, const FlStr *name, const FlMap *config,
                           char *err, size_t errsz)
{
    (void)ed;
    (void)name;
    (void)config;
    return yew_mod_require(YEW_MOD_AI, err, errsz);
}

u32 yew_ai_backend_count(const Ed *ed)
{
    (void)ed;
    return 0U;
}

const AiBackendEntry *yew_ai_backend_at(const Ed *ed, u32 index)
{
    (void)ed;
    (void)index;
    return NULL;
}

const AiBackendEntry *yew_ai_backend_selected(const Ed *ed)
{
    (void)ed;
    return NULL;
}

bool yew_ai_backend_name_is_remote(const Ed *ed, const char *name, u32 len)
{
    (void)ed;
    (void)name;
    (void)len;
    return false;
}

AiWsGrant yew_ai_workspace_grant(Ed *ed)
{
    (void)ed;
    return YEW_AI_WS_DENY;
}

bool yew_ai_workspace_allowed(Ed *ed)
{
    (void)ed;
    return false;
}

void yew_ai_workspace_set(Ed *ed, AiWsGrant grant)
{
    (void)ed;
    (void)grant;
}

void yew_ai_workspace_session_set(Ed *ed, AiWsGrant grant)
{
    (void)ed;
    (void)grant;
}

const char *yew_ai_path_exclusion(Ed *ed, const char *path)
{
    (void)ed;
    (void)path;
    return NULL;
}

void yew_ai_block_offer(Ed *ed, u32 buf_id, u32 line_1based)
{
    (void)ed;
    (void)buf_id;
    (void)line_1based;
}

bool yew_ai_block_prompt_key(Ed *ed, u8 answer)
{
    (void)ed;
    (void)answer;
    return false;
}

bool yew_ai_buffer_session_ignored(const Ed *ed, u32 buf_id)
{
    (void)ed;
    (void)buf_id;
    return false;
}

void yew_ai_collect_fds(Ed *ed, struct pollfd *pfd, u32 *n)
{
    (void)ed;
    (void)pfd;
    (void)n;
}

void yew_ai_pump(Ed *ed, const struct pollfd *pfd, u32 n)
{
    (void)ed;
    (void)pfd;
    (void)n;
}

i64 yew_ai_deadline(const Ed *ed, i64 now_ms)
{
    (void)ed;
    (void)now_ms;
    return -1;
}

void yew_ai_shadow_init(Ed *ed)
{
    (void)ed;
}

void yew_ai_shadow_accept_note(Ed *ed, u32 seq, u8 kind, u64 bytes)
{
    (void)ed;
    (void)seq;
    (void)kind;
    (void)bytes;
}

void yew_ai_shadow_dismiss_note(Ed *ed, u32 seq)
{
    (void)ed;
    (void)seq;
}

static CmdStatus require_ai(CmdCtx *cx)
{
    char err[160];

    if (cx == NULL || cx->ed == NULL)
        return YEW_CMD_ERR_STATE;
    if (!yew_mod_require(YEW_MOD_AI, err, sizeof(err))) {
        yew_msg(cx->ed, YEW_MSG_ERROR, "%s", err);
        return YEW_CMD_ERR_STATE;
    }
    return YEW_CMD_OK;
}

CmdStatus yew_ai_cmd_off(CmdCtx *cx)
{
    return require_ai(cx);
}

CmdStatus yew_ai_cmd_require(CmdCtx *cx)
{
    return require_ai(cx);
}

CmdStatus yew_ai_cmd_backends(CmdCtx *cx)
{
    return require_ai(cx);
}

CmdStatus yew_ai_cmd_models(CmdCtx *cx)
{
    return require_ai(cx);
}

CmdStatus yew_ai_cmd_ping(CmdCtx *cx)
{
    return require_ai(cx);
}

CmdStatus yew_ai_cmd_log(CmdCtx *cx)
{
    return require_ai(cx);
}

CmdStatus yew_ai_cmd_reload(CmdCtx *cx)
{
    return require_ai(cx);
}

CmdStatus yew_ai_cmd_stats(CmdCtx *cx)
{
    return require_ai(cx);
}

CmdStatus yew_ai_cmd_open(CmdCtx *cx)
{
    return require_ai(cx);
}

CmdStatus yew_ai_cmd_enable(CmdCtx *cx)
{
    return require_ai(cx);
}

CmdStatus yew_ai_cmd_disable(CmdCtx *cx)
{
    return require_ai(cx);
}

CmdStatus yew_ai_cmd_forget(CmdCtx *cx)
{
    return require_ai(cx);
}

CmdStatus yew_ai_cmd_privacy(CmdCtx *cx)
{
    return require_ai(cx);
}

CmdStatus yew_ai_cmd_preset(CmdCtx *cx)
{
    return require_ai(cx);
}

CmdStatus yew_ai_cmd_status(CmdCtx *cx)
{
    return require_ai(cx);
}
