#ifndef YEW_EDIT_EDIT_CMDS_H
#define YEW_EDIT_EDIT_CMDS_H

#include "edit/cmd.h"

CmdStatus yew_edit_cmd_move_buf_home(CmdCtx *cx);
CmdStatus yew_edit_cmd_move_buf_end(CmdCtx *cx);
CmdStatus yew_edit_cmd_move_line_home(CmdCtx *cx);
CmdStatus yew_edit_cmd_move_line_end(CmdCtx *cx);
CmdStatus yew_edit_cmd_move_line_up(CmdCtx *cx);
CmdStatus yew_edit_cmd_move_line_down(CmdCtx *cx);
CmdStatus yew_edit_cmd_move_line_first_nonblank(CmdCtx *cx);
CmdStatus yew_edit_cmd_move_line_last_nonblank(CmdCtx *cx);
CmdStatus yew_edit_cmd_move_line_half_page_up(CmdCtx *cx);
CmdStatus yew_edit_cmd_move_line_half_page_down(CmdCtx *cx);
CmdStatus yew_edit_cmd_view_page_up(CmdCtx *cx);
CmdStatus yew_edit_cmd_view_page_down(CmdCtx *cx);
CmdStatus yew_edit_cmd_view_scroll_up(CmdCtx *cx);
CmdStatus yew_edit_cmd_view_scroll_down(CmdCtx *cx);
CmdStatus yew_edit_cmd_view_half_page_up(CmdCtx *cx);
CmdStatus yew_edit_cmd_view_half_page_down(CmdCtx *cx);
CmdStatus yew_edit_cmd_view_center(CmdCtx *cx);
CmdStatus yew_edit_cmd_view_top(CmdCtx *cx);
CmdStatus yew_edit_cmd_view_bottom(CmdCtx *cx);
CmdStatus yew_edit_cmd_view_goto_line(CmdCtx *cx);
CmdStatus yew_edit_cmd_view_toggle_wrap(CmdCtx *cx);
CmdStatus yew_edit_cmd_view_number_style(CmdCtx *cx);
CmdStatus yew_edit_cmd_message_expand(CmdCtx *cx);
CmdStatus yew_edit_cmd_ui_cancel(CmdCtx *cx);
CmdStatus yew_edit_cmd_move_char_prev(CmdCtx *cx);
CmdStatus yew_edit_cmd_move_char_next(CmdCtx *cx);
CmdStatus yew_edit_cmd_move_unit_next(CmdCtx *cx);
CmdStatus yew_edit_cmd_move_unit_prev(CmdCtx *cx);
CmdStatus yew_edit_cmd_move_unit_home(CmdCtx *cx);
CmdStatus yew_edit_cmd_move_unit_end(CmdCtx *cx);
CmdStatus yew_edit_cmd_move_unit_next_alt(CmdCtx *cx);
CmdStatus yew_edit_cmd_move_unit_prev_alt(CmdCtx *cx);
CmdStatus yew_edit_cmd_move_unit_home_alt(CmdCtx *cx);
CmdStatus yew_edit_cmd_move_unit_end_alt(CmdCtx *cx);
CmdStatus yew_edit_cmd_move_unit_up(CmdCtx *cx);
CmdStatus yew_edit_cmd_move_unit_down(CmdCtx *cx);
CmdStatus yew_edit_cmd_move_unit_up_alt(CmdCtx *cx);
CmdStatus yew_edit_cmd_move_unit_down_alt(CmdCtx *cx);
CmdStatus yew_edit_cmd_delete_unit(CmdCtx *cx);
CmdStatus yew_edit_cmd_move_block_match_prev(CmdCtx *cx);
CmdStatus yew_edit_cmd_move_block_match_next(CmdCtx *cx);
CmdStatus yew_edit_cmd_move_word_sub_prev(CmdCtx *cx);
CmdStatus yew_edit_cmd_move_word_sub_next(CmdCtx *cx);
CmdStatus yew_edit_cmd_sel_unit_expand(CmdCtx *cx);
CmdStatus yew_edit_cmd_sel_unit_contract(CmdCtx *cx);
CmdStatus yew_edit_cmd_sel_kind(CmdCtx *cx);
CmdStatus yew_edit_cmd_sel_swap_ends(CmdCtx *cx);
CmdStatus yew_edit_cmd_cursor_lift_lines(CmdCtx *cx);
CmdStatus yew_edit_cmd_cursor_lift_matches(CmdCtx *cx);
CmdStatus yew_edit_cmd_cursor_lift_ends(CmdCtx *cx);
CmdStatus yew_edit_cmd_cursor_add_above(CmdCtx *cx);
CmdStatus yew_edit_cmd_cursor_add_below(CmdCtx *cx);
CmdStatus yew_edit_cmd_cursor_drop(CmdCtx *cx);
CmdStatus yew_edit_cmd_cursor_collapse(CmdCtx *cx);

CmdStatus yew_edit_cmd_insert_text(CmdCtx *cx);
CmdStatus yew_edit_cmd_insert_newline(CmdCtx *cx);
CmdStatus yew_edit_cmd_insert_tab(CmdCtx *cx);
CmdStatus yew_edit_cmd_insert_after(CmdCtx *cx);
CmdStatus yew_edit_cmd_open_below(CmdCtx *cx);
CmdStatus yew_edit_cmd_open_above(CmdCtx *cx);
CmdStatus yew_edit_cmd_delete_grapheme_left(CmdCtx *cx);
CmdStatus yew_edit_cmd_delete_grapheme(CmdCtx *cx);
CmdStatus yew_edit_cmd_delete_line(CmdCtx *cx);
CmdStatus yew_edit_cmd_undo(CmdCtx *cx);
CmdStatus yew_edit_cmd_redo(CmdCtx *cx);
CmdStatus yew_edit_cmd_undo_barrier(CmdCtx *cx);

CmdStatus yew_edit_cmd_mode_enter(CmdCtx *cx);
CmdStatus yew_edit_cmd_mode_escape(CmdCtx *cx);
CmdStatus yew_edit_cmd_insert_at(CmdCtx *cx);
CmdStatus yew_edit_cmd_delete_span(CmdCtx *cx);
CmdStatus yew_edit_cmd_replace_span(CmdCtx *cx);
CmdStatus yew_edit_cmd_cursor_set(CmdCtx *cx);

#endif
