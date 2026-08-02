#ifndef SAG_EDIT_EDIT_CMDS_H
#define SAG_EDIT_EDIT_CMDS_H

#include "edit/cmd.h"

CmdStatus sag_edit_cmd_move_buf_home(CmdCtx *cx);
CmdStatus sag_edit_cmd_move_buf_end(CmdCtx *cx);
CmdStatus sag_edit_cmd_move_line_home(CmdCtx *cx);
CmdStatus sag_edit_cmd_move_line_end(CmdCtx *cx);
CmdStatus sag_edit_cmd_move_line_up(CmdCtx *cx);
CmdStatus sag_edit_cmd_move_line_down(CmdCtx *cx);
CmdStatus sag_edit_cmd_move_line_first_nonblank(CmdCtx *cx);
CmdStatus sag_edit_cmd_move_line_last_nonblank(CmdCtx *cx);
CmdStatus sag_edit_cmd_move_line_half_page_up(CmdCtx *cx);
CmdStatus sag_edit_cmd_move_line_half_page_down(CmdCtx *cx);
CmdStatus sag_edit_cmd_view_page_up(CmdCtx *cx);
CmdStatus sag_edit_cmd_view_page_down(CmdCtx *cx);
CmdStatus sag_edit_cmd_view_scroll_up(CmdCtx *cx);
CmdStatus sag_edit_cmd_view_scroll_down(CmdCtx *cx);
CmdStatus sag_edit_cmd_view_half_page_up(CmdCtx *cx);
CmdStatus sag_edit_cmd_view_half_page_down(CmdCtx *cx);
CmdStatus sag_edit_cmd_view_center(CmdCtx *cx);
CmdStatus sag_edit_cmd_view_top(CmdCtx *cx);
CmdStatus sag_edit_cmd_view_bottom(CmdCtx *cx);
CmdStatus sag_edit_cmd_view_goto_line(CmdCtx *cx);
CmdStatus sag_edit_cmd_view_toggle_wrap(CmdCtx *cx);
CmdStatus sag_edit_cmd_view_number_style(CmdCtx *cx);
CmdStatus sag_edit_cmd_message_expand(CmdCtx *cx);
CmdStatus sag_edit_cmd_ui_cancel(CmdCtx *cx);
CmdStatus sag_edit_cmd_move_char_prev(CmdCtx *cx);
CmdStatus sag_edit_cmd_move_char_next(CmdCtx *cx);

CmdStatus sag_edit_cmd_insert_text(CmdCtx *cx);
CmdStatus sag_edit_cmd_insert_newline(CmdCtx *cx);
CmdStatus sag_edit_cmd_insert_tab(CmdCtx *cx);
CmdStatus sag_edit_cmd_insert_after(CmdCtx *cx);
CmdStatus sag_edit_cmd_open_below(CmdCtx *cx);
CmdStatus sag_edit_cmd_open_above(CmdCtx *cx);
CmdStatus sag_edit_cmd_delete_grapheme_left(CmdCtx *cx);
CmdStatus sag_edit_cmd_delete_grapheme(CmdCtx *cx);
CmdStatus sag_edit_cmd_delete_line(CmdCtx *cx);
CmdStatus sag_edit_cmd_undo(CmdCtx *cx);
CmdStatus sag_edit_cmd_redo(CmdCtx *cx);
CmdStatus sag_edit_cmd_undo_barrier(CmdCtx *cx);

CmdStatus sag_edit_cmd_mode_enter(CmdCtx *cx);
CmdStatus sag_edit_cmd_mode_escape(CmdCtx *cx);

#endif
