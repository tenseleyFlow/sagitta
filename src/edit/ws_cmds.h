#ifndef YEW_EDIT_WS_CMDS_H
#define YEW_EDIT_WS_CMDS_H

/*
 * Sprint 25 §9: the workspace-state commands.
 *
 * save_state and restore_state exist so the feature is reachable from
 * the keyboard and from a test script without waiting out a 2 s
 * debounce or restarting the editor (modal-paradigm-first: everything
 * has a name).  info and forget exist because a hashed directory under
 * $XDG_STATE_HOME is otherwise unauditable — a user who wants to know
 * what we are keeping, or to stop us keeping it, must not have to read
 * the source to find out where it is.
 */

#include "edit/cmd.h"

typedef struct Ed Ed;

/*
 * The forget confirmation.  A separate prompt rather than a reuse of
 * YEW_PROMPT_*: those all answer "what about these unsaved bytes", and
 * this one answers "delete a cache", which must never share a keystroke
 * with them.
 */
typedef struct WsPrompt {
    bool active;
} WsPrompt;

/* True when the key was consumed by the prompt. */
bool yew_ws_prompt_key(Ed *ed, u8 answer);

CmdStatus yew_ws_cmd_save_state(CmdCtx *cx);
CmdStatus yew_ws_cmd_restore_state(CmdCtx *cx);
CmdStatus yew_ws_cmd_info(CmdCtx *cx);
CmdStatus yew_ws_cmd_forget(CmdCtx *cx);
CmdStatus yew_ws_cmd_migrate(CmdCtx *cx);

/*
 * Removes a workspace state directory.  Refuses any path that is not
 * under .../workspaces/, which is the belt to the braces of only ever
 * being handed one computed by yew_ws_key.
 */
bool yew_ws_forget_dir(const char *dir);

#endif
