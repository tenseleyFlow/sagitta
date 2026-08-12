#ifndef YEW_EDIT_SHADOW_CMDS_H
#define YEW_EDIT_SHADOW_CMDS_H

#include "edit/cmd.h"

CmdStatus yew_shadow_cmd_accept_word(CmdCtx *cx);
CmdStatus yew_shadow_cmd_accept_word_alt(CmdCtx *cx);
CmdStatus yew_shadow_cmd_accept_line(CmdCtx *cx);
CmdStatus yew_shadow_cmd_accept_all(CmdCtx *cx);
CmdStatus yew_shadow_cmd_dismiss(CmdCtx *cx);

#endif
