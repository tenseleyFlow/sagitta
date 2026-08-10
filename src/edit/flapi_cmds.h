#ifndef SAG_EDIT_FLAPI_CMDS_H
#define SAG_EDIT_FLAPI_CMDS_H

/* Registry commands required by Sprint 34's Fletch editor bindings. */

#include "edit/cmd.h"

CmdStatus sag_flapi_cmd_win_split(CmdCtx *cx);
CmdStatus sag_flapi_cmd_win_focus(CmdCtx *cx);
CmdStatus sag_flapi_cmd_cursor_set_many(CmdCtx *cx);
CmdStatus sag_flapi_cmd_cursor_move(CmdCtx *cx);
CmdStatus sag_flapi_cmd_span_yank(CmdCtx *cx);

#endif /* SAG_EDIT_FLAPI_CMDS_H */
