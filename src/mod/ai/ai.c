#include "mod/ai/ai.h"

#include <stdlib.h>

#include "edit/ed.h"
#include "mod/ai/key.h"
#include "ui/message.h"
#include "util/base.h"

/* Ownership root for Sprint 48 and the following AI sprints.  Concrete
 * transport objects remain private to the module; expanding this structure
 * cannot make Ed or stripped builds depend on their representation. */
struct AiState {
    AiKeyCache keys;
    /* Backend registry, curl probe/cache, pool and cooldown ownership lands
     * here without widening Ed when those Sprint 48 components integrate. */
};

void yew_ai_state_init(Ed *ed)
{
    AiState *state;

    if (ed == NULL || ed->ai != NULL)
        return;
    state = yew_xcalloc(1U, sizeof(*state));
    yew_ai_key_cache_init(&state->keys);
    ed->ai = state;
}

void yew_ai_state_free(Ed *ed)
{
    AiState *state;

    if (ed == NULL || ed->ai == NULL)
        return;
    state = ed->ai;
    ed->ai = NULL;
    yew_ai_key_cache_drop(&state->keys);
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
