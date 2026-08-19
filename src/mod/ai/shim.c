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
