#ifndef SAG_EDIT_SEL_ACTIONS_H
#define SAG_EDIT_SEL_ACTIONS_H

#include "edit/cmd.h"

CmdStatus sag_sel_cmd_yank(CmdCtx *cx);
CmdStatus sag_sel_cmd_delete(CmdCtx *cx);
CmdStatus sag_sel_cmd_change(CmdCtx *cx);
CmdStatus sag_sel_cmd_case_upper(CmdCtx *cx);
CmdStatus sag_sel_cmd_case_lower(CmdCtx *cx);
CmdStatus sag_sel_cmd_case_toggle(CmdCtx *cx);
CmdStatus sag_sel_cmd_indent(CmdCtx *cx);
CmdStatus sag_sel_cmd_dedent(CmdCtx *cx);
CmdStatus sag_sel_cmd_shift_left(CmdCtx *cx);
CmdStatus sag_sel_cmd_shift_right(CmdCtx *cx);
CmdStatus sag_sel_cmd_join(CmdCtx *cx);
CmdStatus sag_sel_cmd_replace_char(CmdCtx *cx);
CmdStatus sag_sel_cmd_rect_insert(CmdCtx *cx);
CmdStatus sag_sel_cmd_rect_append(CmdCtx *cx);

#endif
