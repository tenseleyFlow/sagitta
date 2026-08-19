#ifndef YEW_MOD_AI_AI_H
#define YEW_MOD_AI_AI_H

#include <stdbool.h>

#include "edit/cmd.h"

typedef struct Ed Ed;

/* Sprint 48's editor-facing module boundary.  The stripped-module shim
 * implements the same command surface, so AI commands remain discoverable
 * and explain how to obtain the module instead of disappearing. */
CmdStatus yew_ai_cmd_off(CmdCtx *cx);
CmdStatus yew_ai_cmd_require(CmdCtx *cx);

/* Module-neutral, Ed-owned lifecycle.  Both the enabled implementation and
 * stripped shim provide it, keeping compile-time module checks out of core. */
void yew_ai_state_init(Ed *ed);
void yew_ai_state_free(Ed *ed);
bool yew_ai_state_ready(const Ed *ed);
void yew_ai_state_key_cache_enable(Ed *ed, bool enabled);
bool yew_ai_state_key_cache_enabled(const Ed *ed);

#endif
