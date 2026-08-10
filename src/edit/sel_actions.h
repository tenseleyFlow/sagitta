#ifndef YEW_EDIT_SEL_ACTIONS_H
#define YEW_EDIT_SEL_ACTIONS_H

#include "edit/cmd.h"

CmdStatus yew_sel_cmd_yank(CmdCtx *cx);
CmdStatus yew_sel_cmd_delete(CmdCtx *cx);
CmdStatus yew_sel_cmd_change(CmdCtx *cx);
CmdStatus yew_sel_cmd_case_upper(CmdCtx *cx);
CmdStatus yew_sel_cmd_case_lower(CmdCtx *cx);
CmdStatus yew_sel_cmd_case_toggle(CmdCtx *cx);
CmdStatus yew_sel_cmd_indent(CmdCtx *cx);
CmdStatus yew_sel_cmd_dedent(CmdCtx *cx);
CmdStatus yew_sel_cmd_shift_left(CmdCtx *cx);
CmdStatus yew_sel_cmd_shift_right(CmdCtx *cx);
CmdStatus yew_sel_cmd_join(CmdCtx *cx);
CmdStatus yew_sel_cmd_replace_char(CmdCtx *cx);
CmdStatus yew_sel_cmd_rect_insert(CmdCtx *cx);
CmdStatus yew_sel_cmd_rect_append(CmdCtx *cx);

#endif
