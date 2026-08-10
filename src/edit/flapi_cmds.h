#ifndef YEW_EDIT_FLAPI_CMDS_H
#define YEW_EDIT_FLAPI_CMDS_H

/* Registry commands required by Sprint 34's Fletch editor bindings. */

#include "edit/cmd.h"

CmdStatus yew_flapi_cmd_win_split(CmdCtx *cx);
CmdStatus yew_flapi_cmd_win_focus(CmdCtx *cx);
CmdStatus yew_flapi_cmd_cursor_set_many(CmdCtx *cx);
CmdStatus yew_flapi_cmd_cursor_move(CmdCtx *cx);
CmdStatus yew_flapi_cmd_span_yank(CmdCtx *cx);
CmdStatus yew_flapi_cmd_reg_set(CmdCtx *cx);
CmdStatus yew_flapi_reg_write(Ed *ed, u8 name, const u8 *bytes,
                              u32 len, bool append);

#endif /* YEW_EDIT_FLAPI_CMDS_H */
