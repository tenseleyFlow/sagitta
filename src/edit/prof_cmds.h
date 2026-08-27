#ifndef YEW_EDIT_PROF_CMDS_H
#define YEW_EDIT_PROF_CMDS_H

#include "edit/cmd.h"

CmdStatus yew_prof_cmd_report(CmdCtx *cx);
CmdStatus yew_prof_cmd_reset(CmdCtx *cx);
CmdStatus yew_prof_cmd_dump(CmdCtx *cx);
CmdStatus yew_prof_cmd_mark(CmdCtx *cx);
CmdStatus yew_prof_cmd_frames(CmdCtx *cx);

#endif
