#include "edit/cmd.h"

#include <ctype.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "edit/edit_cmds.h"
#include "edit/jumplist.h"
#include "edit/pane_cmds.h"
#include "edit/search_cmds.h"
#include "edit/file_cmds.h"
#include "edit/shell_cmds.h"
#include "edit/sel_actions.h"
#include "ui/cmdline.h"
#include "util/arena.h"
#include "util/intern.h"
#include "util/log.h"

typedef struct {
    Arena arena;
    Interner names;
    CmdEntry *entries;
    size_t len;
    size_t cap;
    CmdRecordTap record_tap;
    bool initialized;
} CmdRegistry;

static CmdRegistry registry;

static CmdStatus cmd_nop(CmdCtx *cx)
{
    (void)cx;
    return SAG_CMD_OK;
}

static CmdStatus deferred_unreachable(CmdCtx *cx)
{
    (void)cx;
    SAG_BUG("deferred command reached its implementation");
}

#define DEFER(name_, arity_, flags_, sprint_, help_)                           \
    {                                                                          \
        name_, deferred_unreachable, arity_, (flags_) | SAG_CMD_DEFERRED,      \
            "Sprint " #sprint_ ": " help_                                    \
    }

static const CmdDesc builtins[] = {
    {"ed.nop", cmd_nop, SAG_ARITY_NONE, 0U, "Do nothing"},
    {"ed.quit", sag_file_cmd_quit, SAG_ARITY_NONE, 0U,
     "Quit, prompting when the buffer is dirty"},
    {"ed.quit_force", sag_file_cmd_quit_force, SAG_ARITY_NONE, 0U,
     "Quit without discarding the recovery journal"},
    {"ed.suspend", sag_file_cmd_suspend, SAG_ARITY_NONE, 0U,
     "Suspend the editor and restore the terminal"},
    {"ed.redraw", sag_file_cmd_redraw, SAG_ARITY_NONE, 0U,
     "Redraw the complete display"},
    DEFER("ed.repeat", SAG_ARITY_NONE, SAG_CMD_RECORDABLE, 35,
          "repeat the last command"),

    {"ed.move.buf.home", sag_edit_cmd_move_buf_home, SAG_ARITY_NONE,
     SAG_CMD_REPEATABLE | SAG_CMD_RECORDABLE | SAG_CMD_NEEDS_WIN,
     "Move to the start of the buffer"},
    {"ed.move.buf.end", sag_edit_cmd_move_buf_end, SAG_ARITY_NONE,
     SAG_CMD_TAKES_COUNT | SAG_CMD_RECORDABLE | SAG_CMD_NEEDS_WIN,
     "Move to the end of the buffer or a counted line"},
    {"ed.move.line.home", sag_edit_cmd_move_line_home, SAG_ARITY_NONE,
     SAG_CMD_REPEATABLE | SAG_CMD_RECORDABLE | SAG_CMD_NEEDS_WIN,
     "Move to the start of the line"},
    {"ed.move.line.end", sag_edit_cmd_move_line_end, SAG_ARITY_NONE,
     SAG_CMD_REPEATABLE | SAG_CMD_RECORDABLE | SAG_CMD_NEEDS_WIN,
     "Move to the end of the line"},
    {"ed.move.line.up", sag_edit_cmd_move_line_up, SAG_ARITY_NONE,
     SAG_CMD_REPEATABLE | SAG_CMD_RECORDABLE | SAG_CMD_NEEDS_WIN,
     "Move one line up"},
    {"ed.move.line.down", sag_edit_cmd_move_line_down, SAG_ARITY_NONE,
     SAG_CMD_REPEATABLE | SAG_CMD_RECORDABLE | SAG_CMD_NEEDS_WIN,
     "Move one line down"},
    {"ed.move.line.first_nonblank", sag_edit_cmd_move_line_first_nonblank,
     SAG_ARITY_NONE,
     SAG_CMD_REPEATABLE | SAG_CMD_RECORDABLE | SAG_CMD_NEEDS_WIN,
     "Move to the first nonblank grapheme"},
    {"ed.move.line.last_nonblank", sag_edit_cmd_move_line_last_nonblank,
     SAG_ARITY_NONE,
     SAG_CMD_REPEATABLE | SAG_CMD_RECORDABLE | SAG_CMD_NEEDS_WIN,
     "Move to the last nonblank grapheme"},
    {"ed.move.line.half_page_up", sag_edit_cmd_move_line_half_page_up,
     SAG_ARITY_NONE,
     SAG_CMD_REPEATABLE | SAG_CMD_RECORDABLE | SAG_CMD_NEEDS_WIN,
     "Move half a viewport up"},
    {"ed.move.line.half_page_down", sag_edit_cmd_move_line_half_page_down,
     SAG_ARITY_NONE,
     SAG_CMD_REPEATABLE | SAG_CMD_RECORDABLE | SAG_CMD_NEEDS_WIN,
     "Move half a viewport down"},
    {"ed.move.unit.next", sag_edit_cmd_move_unit_next, SAG_ARITY_NONE,
     SAG_CMD_REPEATABLE | SAG_CMD_RECORDABLE | SAG_CMD_NEEDS_WIN,
     "Move to the next unit"},
    {"ed.move.unit.prev", sag_edit_cmd_move_unit_prev, SAG_ARITY_NONE,
     SAG_CMD_REPEATABLE | SAG_CMD_RECORDABLE | SAG_CMD_NEEDS_WIN,
     "Move to the previous unit"},
    {"ed.move.unit.home", sag_edit_cmd_move_unit_home, SAG_ARITY_NONE,
     SAG_CMD_REPEATABLE | SAG_CMD_RECORDABLE | SAG_CMD_NEEDS_WIN,
     "Move to the start of the current unit"},
    {"ed.move.unit.end", sag_edit_cmd_move_unit_end, SAG_ARITY_NONE,
     SAG_CMD_REPEATABLE | SAG_CMD_RECORDABLE | SAG_CMD_NEEDS_WIN,
     "Move to the end of the current unit"},
    {"ed.move.unit.next_alt", sag_edit_cmd_move_unit_next_alt,
     SAG_ARITY_NONE,
     SAG_CMD_REPEATABLE | SAG_CMD_RECORDABLE | SAG_CMD_NEEDS_WIN,
     "Move to the next alternate unit"},
    {"ed.move.unit.prev_alt", sag_edit_cmd_move_unit_prev_alt,
     SAG_ARITY_NONE,
     SAG_CMD_REPEATABLE | SAG_CMD_RECORDABLE | SAG_CMD_NEEDS_WIN,
     "Move to the previous alternate unit"},
    {"ed.move.unit.home_alt", sag_edit_cmd_move_unit_home_alt,
     SAG_ARITY_NONE,
     SAG_CMD_REPEATABLE | SAG_CMD_RECORDABLE | SAG_CMD_NEEDS_WIN,
     "Move to the alternate unit start"},
    {"ed.move.unit.end_alt", sag_edit_cmd_move_unit_end_alt,
     SAG_ARITY_NONE,
     SAG_CMD_REPEATABLE | SAG_CMD_RECORDABLE | SAG_CMD_NEEDS_WIN,
     "Move to the alternate unit end"},
    {"ed.move.block.match_prev", sag_edit_cmd_move_block_match_prev,
     SAG_ARITY_NONE,
     SAG_CMD_REPEATABLE | SAG_CMD_RECORDABLE | SAG_CMD_NEEDS_WIN,
     "Move to the enclosing opening delimiter"},
    {"ed.move.block.match_next", sag_edit_cmd_move_block_match_next,
     SAG_ARITY_NONE,
     SAG_CMD_REPEATABLE | SAG_CMD_RECORDABLE | SAG_CMD_NEEDS_WIN,
     "Move to the enclosing closing delimiter"},
    {"ed.move.word.sub_prev", sag_edit_cmd_move_word_sub_prev,
     SAG_ARITY_NONE,
     SAG_CMD_REPEATABLE | SAG_CMD_RECORDABLE | SAG_CMD_NEEDS_WIN,
     "Move to the previous subword"},
    {"ed.move.word.sub_next", sag_edit_cmd_move_word_sub_next,
     SAG_ARITY_NONE,
     SAG_CMD_REPEATABLE | SAG_CMD_RECORDABLE | SAG_CMD_NEEDS_WIN,
     "Move to the next subword"},
    {"ed.move.char.prev", sag_edit_cmd_move_char_prev, SAG_ARITY_NONE,
     SAG_CMD_REPEATABLE | SAG_CMD_RECORDABLE | SAG_CMD_NEEDS_WIN,
     "Move one grapheme left"},
    {"ed.move.char.next", sag_edit_cmd_move_char_next, SAG_ARITY_NONE,
     SAG_CMD_REPEATABLE | SAG_CMD_RECORDABLE | SAG_CMD_NEEDS_WIN,
     "Move one grapheme right"},
    {"ed.move.char.left", sag_edit_cmd_move_char_prev, SAG_ARITY_NONE,
     SAG_CMD_REPEATABLE | SAG_CMD_RECORDABLE | SAG_CMD_NEEDS_WIN,
     "Alias for moving one grapheme left"},
    {"ed.move.char.right", sag_edit_cmd_move_char_next, SAG_ARITY_NONE,
     SAG_CMD_REPEATABLE | SAG_CMD_RECORDABLE | SAG_CMD_NEEDS_WIN,
     "Alias for moving one grapheme right"},

    {"ed.edit.insert.text", sag_edit_cmd_insert_text, SAG_ARITY_STR,
     SAG_CMD_RECORDABLE | SAG_CMD_NEEDS_WIN | SAG_CMD_CHANGES_BUFFER,
     "Insert UTF-8 text at the cursor"},
    {"ed.edit.insert.newline", sag_edit_cmd_insert_newline, SAG_ARITY_NONE,
     SAG_CMD_RECORDABLE | SAG_CMD_NEEDS_WIN | SAG_CMD_CHANGES_BUFFER,
     "Insert the buffer's native line ending"},
    {"ed.edit.insert.tab", sag_edit_cmd_insert_tab, SAG_ARITY_NONE,
     SAG_CMD_RECORDABLE | SAG_CMD_NEEDS_WIN | SAG_CMD_CHANGES_BUFFER,
     "Insert a literal tab"},
    {"ed.edit.insert.after", sag_edit_cmd_insert_after, SAG_ARITY_NONE,
     SAG_CMD_RECORDABLE | SAG_CMD_NEEDS_WIN,
     "Enter insert mode after the current grapheme"},
    {"ed.edit.line.open_below", sag_edit_cmd_open_below, SAG_ARITY_NONE,
     SAG_CMD_RECORDABLE | SAG_CMD_NEEDS_WIN | SAG_CMD_CHANGES_BUFFER,
     "Open a new line below and enter insert mode"},
    {"ed.edit.line.open_above", sag_edit_cmd_open_above, SAG_ARITY_NONE,
     SAG_CMD_RECORDABLE | SAG_CMD_NEEDS_WIN | SAG_CMD_CHANGES_BUFFER,
     "Open a new line above and enter insert mode"},
    {"ed.edit.delete.grapheme_left", sag_edit_cmd_delete_grapheme_left,
     SAG_ARITY_NONE,
     SAG_CMD_REPEATABLE | SAG_CMD_RECORDABLE | SAG_CMD_NEEDS_WIN |
         SAG_CMD_CHANGES_BUFFER,
     "Delete the grapheme left of the cursor"},
    {"ed.edit.delete.grapheme", sag_edit_cmd_delete_grapheme,
     SAG_ARITY_NONE,
     SAG_CMD_REPEATABLE | SAG_CMD_RECORDABLE | SAG_CMD_NEEDS_WIN |
         SAG_CMD_CHANGES_BUFFER,
     "Delete the grapheme at the cursor"},
    {"ed.edit.line.delete", sag_edit_cmd_delete_line, SAG_ARITY_NONE,
     SAG_CMD_REPEATABLE | SAG_CMD_RECORDABLE | SAG_CMD_NEEDS_WIN |
         SAG_CMD_CHANGES_BUFFER,
     "Delete the current logical line"},
    {"ed.edit.delete.prev", sag_edit_cmd_delete_grapheme_left,
     SAG_ARITY_NONE,
     SAG_CMD_REPEATABLE | SAG_CMD_RECORDABLE | SAG_CMD_NEEDS_WIN |
         SAG_CMD_CHANGES_BUFFER,
     "Alias for deleting the previous grapheme"},
    {"ed.edit.delete.next", sag_edit_cmd_delete_grapheme, SAG_ARITY_NONE,
     SAG_CMD_REPEATABLE | SAG_CMD_RECORDABLE | SAG_CMD_NEEDS_WIN |
         SAG_CMD_CHANGES_BUFFER,
     "Alias for deleting the next grapheme"},
    {"ed.edit.undo", sag_edit_cmd_undo, SAG_ARITY_NONE,
     SAG_CMD_REPEATABLE | SAG_CMD_RECORDABLE | SAG_CMD_NEEDS_WIN,
     "Undo the last edit transaction"},
    {"ed.edit.redo", sag_edit_cmd_redo, SAG_ARITY_NONE,
     SAG_CMD_REPEATABLE | SAG_CMD_RECORDABLE | SAG_CMD_NEEDS_WIN,
     "Redo the last undone transaction"},
    {"ed.edit.undo_barrier", sag_edit_cmd_undo_barrier, SAG_ARITY_NONE,
     SAG_CMD_NEEDS_WIN, "Close the active insert transaction"},
    {"ed.mode.enter", sag_edit_cmd_mode_enter, SAG_ARITY_STR,
     SAG_CMD_RECORDABLE | SAG_CMD_INTERNAL,
     "Enter L/W/B/I/H/E; F Sprint 52"},
    {"ed.mode.escape", sag_edit_cmd_mode_escape, SAG_ARITY_NONE,
     SAG_CMD_RECORDABLE | SAG_CMD_INTERNAL, "Return to line mode"},
    {"ed.sel.expand", sag_edit_cmd_sel_unit_expand, SAG_ARITY_NONE,
     SAG_CMD_REPEATABLE | SAG_CMD_RECORDABLE | SAG_CMD_NEEDS_WIN,
     "Expand the selection to the next structural unit"},
    {"ed.sel.contract", sag_edit_cmd_sel_unit_contract, SAG_ARITY_NONE,
     SAG_CMD_REPEATABLE | SAG_CMD_RECORDABLE | SAG_CMD_NEEDS_WIN,
     "Contract the selection to the previous structural unit"},
    {"ed.sel.unit.expand", sag_edit_cmd_sel_unit_expand, SAG_ARITY_NONE,
     SAG_CMD_REPEATABLE | SAG_CMD_RECORDABLE | SAG_CMD_NEEDS_WIN,
     "Expand to the next structural unit"},
    {"ed.sel.unit.contract", sag_edit_cmd_sel_unit_contract,
     SAG_ARITY_NONE,
     SAG_CMD_REPEATABLE | SAG_CMD_RECORDABLE | SAG_CMD_NEEDS_WIN,
     "Contract to the previous structural unit"},
    {"ed.sel.kind", sag_edit_cmd_sel_kind, SAG_ARITY_STR,
     SAG_CMD_RECORDABLE | SAG_CMD_NEEDS_WIN,
     "Choose character, line, or rectangular selection geometry"},
    {"ed.sel.swap_ends", sag_edit_cmd_sel_swap_ends, SAG_ARITY_NONE,
     SAG_CMD_RECORDABLE | SAG_CMD_NEEDS_WIN,
     "Exchange the active and anchored ends of each selection"},
    {"ed.sel.yank", sag_sel_cmd_yank, SAG_ARITY_NONE,
     SAG_CMD_RECORDABLE | SAG_CMD_NEEDS_WIN | SAG_CMD_MULTI_AGGREGATE,
     "Yank the active selections"},
    {"ed.sel.delete", sag_sel_cmd_delete, SAG_ARITY_NONE,
     SAG_CMD_RECORDABLE | SAG_CMD_NEEDS_WIN | SAG_CMD_CHANGES_BUFFER |
         SAG_CMD_MULTI_AGGREGATE,
     "Delete the active selections"},
    {"ed.sel.change", sag_sel_cmd_change, SAG_ARITY_NONE,
     SAG_CMD_RECORDABLE | SAG_CMD_NEEDS_WIN | SAG_CMD_CHANGES_BUFFER |
         SAG_CMD_MULTI_AGGREGATE,
     "Change the active selections"},
    {"ed.sel.case_upper", sag_sel_cmd_case_upper, SAG_ARITY_NONE,
     SAG_CMD_RECORDABLE | SAG_CMD_NEEDS_WIN | SAG_CMD_CHANGES_BUFFER |
         SAG_CMD_MULTI_AGGREGATE,
     "Uppercase the active selections"},
    {"ed.sel.case_lower", sag_sel_cmd_case_lower, SAG_ARITY_NONE,
     SAG_CMD_RECORDABLE | SAG_CMD_NEEDS_WIN | SAG_CMD_CHANGES_BUFFER |
         SAG_CMD_MULTI_AGGREGATE,
     "Lowercase the active selections"},
    {"ed.sel.case_toggle", sag_sel_cmd_case_toggle, SAG_ARITY_NONE,
     SAG_CMD_RECORDABLE | SAG_CMD_NEEDS_WIN | SAG_CMD_CHANGES_BUFFER |
         SAG_CMD_MULTI_AGGREGATE,
     "Toggle case in the active selections"},
    {"ed.sel.indent", sag_sel_cmd_indent, SAG_ARITY_NONE,
     SAG_CMD_RECORDABLE | SAG_CMD_NEEDS_WIN | SAG_CMD_CHANGES_BUFFER |
         SAG_CMD_MULTI_AGGREGATE,
     "Indent lines covered by the selection"},
    {"ed.sel.dedent", sag_sel_cmd_dedent, SAG_ARITY_NONE,
     SAG_CMD_RECORDABLE | SAG_CMD_NEEDS_WIN | SAG_CMD_CHANGES_BUFFER |
         SAG_CMD_MULTI_AGGREGATE,
     "Dedent lines covered by the selection"},
    {"ed.sel.shift_left", sag_sel_cmd_shift_left, SAG_ARITY_NONE,
     SAG_CMD_RECORDABLE | SAG_CMD_NEEDS_WIN | SAG_CMD_CHANGES_BUFFER |
         SAG_CMD_MULTI_AGGREGATE,
     "Shift selected text left"},
    {"ed.sel.shift_right", sag_sel_cmd_shift_right, SAG_ARITY_NONE,
     SAG_CMD_RECORDABLE | SAG_CMD_NEEDS_WIN | SAG_CMD_CHANGES_BUFFER |
         SAG_CMD_MULTI_AGGREGATE,
     "Shift selected text right"},
    {"ed.sel.join", sag_sel_cmd_join, SAG_ARITY_NONE,
     SAG_CMD_RECORDABLE | SAG_CMD_NEEDS_WIN | SAG_CMD_CHANGES_BUFFER |
         SAG_CMD_MULTI_AGGREGATE,
     "Join lines covered by the selection"},
    {"ed.sel.replace_char", sag_sel_cmd_replace_char, SAG_ARITY_STR,
     SAG_CMD_RECORDABLE | SAG_CMD_NEEDS_WIN | SAG_CMD_CHANGES_BUFFER |
         SAG_CMD_MULTI_AGGREGATE | SAG_CMD_CAPTURES_TEXT,
     "Replace selected graphemes with one grapheme"},
    {"ed.edit.rect.insert", sag_sel_cmd_rect_insert, SAG_ARITY_NONE,
     SAG_CMD_RECORDABLE | SAG_CMD_NEEDS_WIN | SAG_CMD_CHANGES_BUFFER |
         SAG_CMD_MULTI_AGGREGATE,
     "Insert at the left edge of a rectangular selection"},
    {"ed.edit.rect.append", sag_sel_cmd_rect_append, SAG_ARITY_NONE,
     SAG_CMD_RECORDABLE | SAG_CMD_NEEDS_WIN | SAG_CMD_CHANGES_BUFFER |
         SAG_CMD_MULTI_AGGREGATE,
     "Insert at the right edge of a rectangular selection"},
    {"ed.cursor.lift.lines", sag_edit_cmd_cursor_lift_lines, SAG_ARITY_NONE,
     SAG_CMD_RECORDABLE | SAG_CMD_NEEDS_WIN,
     "Lift a selection to one cursor per line"},
    {"ed.cursor.lift.matches", sag_edit_cmd_cursor_lift_matches,
     SAG_ARITY_OPT_STR, SAG_CMD_RECORDABLE | SAG_CMD_NEEDS_WIN,
     "Lift literal matches in the selection to cursors"},
    {"ed.cursor.lift.ends", sag_edit_cmd_cursor_lift_ends, SAG_ARITY_NONE,
     SAG_CMD_RECORDABLE | SAG_CMD_NEEDS_WIN,
     "Lift both ends of each selection to cursors"},
    {"ed.cursor.add.above", sag_edit_cmd_cursor_add_above, SAG_ARITY_NONE,
     SAG_CMD_RECORDABLE | SAG_CMD_NEEDS_WIN,
     "Add a cursor on the preceding line"},
    {"ed.cursor.add.below", sag_edit_cmd_cursor_add_below, SAG_ARITY_NONE,
     SAG_CMD_RECORDABLE | SAG_CMD_NEEDS_WIN,
     "Add a cursor on the following line"},
    {"ed.cursor.drop", sag_edit_cmd_cursor_drop, SAG_ARITY_NONE,
     SAG_CMD_RECORDABLE | SAG_CMD_NEEDS_WIN,
     "Drop the most recently added cursor"},
    {"ed.cursor.collapse", sag_edit_cmd_cursor_collapse, SAG_ARITY_NONE,
     SAG_CMD_RECORDABLE | SAG_CMD_NEEDS_WIN,
     "Keep only the primary cursor"},
    {"ed.view.center", sag_edit_cmd_view_center, SAG_ARITY_NONE,
     SAG_CMD_RECORDABLE | SAG_CMD_NEEDS_WIN, "Center the cursor line"},
    {"ed.view.top", sag_edit_cmd_view_top, SAG_ARITY_NONE,
     SAG_CMD_RECORDABLE | SAG_CMD_NEEDS_WIN,
     "Place the cursor line at the top"},
    {"ed.view.bottom", sag_edit_cmd_view_bottom, SAG_ARITY_NONE,
     SAG_CMD_RECORDABLE | SAG_CMD_NEEDS_WIN,
     "Place the cursor line at the bottom"},
    {"ed.view.scroll.up", sag_edit_cmd_view_scroll_up, SAG_ARITY_NONE,
     SAG_CMD_REPEATABLE | SAG_CMD_RECORDABLE | SAG_CMD_NEEDS_WIN,
     "Scroll the active view up one display row"},
    {"ed.view.scroll.down", sag_edit_cmd_view_scroll_down, SAG_ARITY_NONE,
     SAG_CMD_REPEATABLE | SAG_CMD_RECORDABLE | SAG_CMD_NEEDS_WIN,
     "Scroll the active view down one display row"},
    {"ed.view.up", sag_edit_cmd_view_scroll_up, SAG_ARITY_NONE,
     SAG_CMD_REPEATABLE | SAG_CMD_RECORDABLE | SAG_CMD_NEEDS_WIN,
     "Alias for scrolling the active view up"},
    {"ed.view.down", sag_edit_cmd_view_scroll_down, SAG_ARITY_NONE,
     SAG_CMD_REPEATABLE | SAG_CMD_RECORDABLE | SAG_CMD_NEEDS_WIN,
     "Alias for scrolling the active view down"},
    {"ed.view.page_up", sag_edit_cmd_view_page_up, SAG_ARITY_NONE,
     SAG_CMD_REPEATABLE | SAG_CMD_RECORDABLE | SAG_CMD_NEEDS_WIN,
     "Move one viewport up"},
    {"ed.view.page_down", sag_edit_cmd_view_page_down, SAG_ARITY_NONE,
     SAG_CMD_REPEATABLE | SAG_CMD_RECORDABLE | SAG_CMD_NEEDS_WIN,
     "Move one viewport down"},
    {"ed.view.half_page_up", sag_edit_cmd_view_half_page_up,
     SAG_ARITY_NONE,
     SAG_CMD_REPEATABLE | SAG_CMD_RECORDABLE | SAG_CMD_NEEDS_WIN,
     "Scroll half a viewport up"},
    {"ed.view.half_page_down", sag_edit_cmd_view_half_page_down,
     SAG_ARITY_NONE,
     SAG_CMD_REPEATABLE | SAG_CMD_RECORDABLE | SAG_CMD_NEEDS_WIN,
     "Scroll half a viewport down"},
    {"ed.view.goto_line", sag_edit_cmd_view_goto_line, SAG_ARITY_NONE,
     SAG_CMD_TAKES_COUNT | SAG_CMD_RECORDABLE | SAG_CMD_NEEDS_WIN,
     "Go to a counted line and center it"},
    {"ed.view.toggle_wrap", sag_edit_cmd_view_toggle_wrap, SAG_ARITY_NONE,
     SAG_CMD_RECORDABLE | SAG_CMD_NEEDS_WIN, "Toggle line wrapping"},
    {"ed.view.number_style", sag_edit_cmd_view_number_style, SAG_ARITY_STR,
     SAG_CMD_RECORDABLE | SAG_CMD_NEEDS_WIN,
     "Set line numbers to none, abs, rel, or hybrid"},
    {"ed.ui.message_expand", sag_edit_cmd_message_expand, SAG_ARITY_NONE,
     SAG_CMD_PROMPTS, "Expand the current message"},
    {"ed.ui.cancel", sag_edit_cmd_ui_cancel, SAG_ARITY_NONE, 0U,
     "Cancel the active prompt or message overlay"},

    {"ed.cmdline.hist_prev", sag_cmdline_cmd_hist_prev, SAG_ARITY_NONE,
     SAG_CMD_NEEDS_WIN | SAG_CMD_INTERNAL,
     "Find the previous matching command-line history entry"},
    {"ed.cmdline.hist_next", sag_cmdline_cmd_hist_next, SAG_ARITY_NONE,
     SAG_CMD_NEEDS_WIN | SAG_CMD_INTERNAL,
     "Find the next matching command-line history entry"},
    {"ed.cmdline.complete_next", sag_cmdline_cmd_complete_next,
     SAG_ARITY_NONE, SAG_CMD_NEEDS_WIN | SAG_CMD_INTERNAL,
     "Open or advance command-line completion"},
    {"ed.cmdline.complete_prev", sag_cmdline_cmd_complete_prev,
     SAG_ARITY_NONE, SAG_CMD_NEEDS_WIN | SAG_CMD_INTERNAL,
     "Open or reverse command-line completion"},
    {"ed.cmdline.insert_register", sag_cmdline_cmd_insert_register,
     SAG_ARITY_STR,
     SAG_CMD_NEEDS_WIN | SAG_CMD_CAPTURES_TEXT | SAG_CMD_INTERNAL,
     "Insert one named register into the command line"},
    {"ed.cmdline.literal_next", sag_cmdline_cmd_literal_next,
     SAG_ARITY_STR,
     SAG_CMD_NEEDS_WIN | SAG_CMD_CAPTURES_TEXT | SAG_CMD_INTERNAL,
     "Insert the next text-producing key literally"},
    {"ed.cmdline.accept", sag_cmdline_cmd_accept, SAG_ARITY_NONE,
     SAG_CMD_NEEDS_WIN | SAG_CMD_PROMPTS | SAG_CMD_INTERNAL,
     "Accept the command line"},
    {"ed.cmdline.cancel", sag_cmdline_cmd_cancel, SAG_ARITY_NONE,
     SAG_CMD_NEEDS_WIN | SAG_CMD_PROMPTS | SAG_CMD_INTERNAL,
     "Cancel the command line or menu"},
    {"ed.del.word_prev", sag_cmdline_cmd_delete_word_prev, SAG_ARITY_NONE,
     SAG_CMD_NEEDS_WIN | SAG_CMD_CHANGES_BUFFER | SAG_CMD_INTERNAL,
     "Delete to the previous word boundary"},
    {"ed.del.to_home", sag_cmdline_cmd_delete_to_home, SAG_ARITY_NONE,
     SAG_CMD_NEEDS_WIN | SAG_CMD_CHANGES_BUFFER | SAG_CMD_INTERNAL,
     "Delete from the cursor to line start"},
    {"ed.del.to_end", sag_cmdline_cmd_delete_to_end, SAG_ARITY_NONE,
     SAG_CMD_NEEDS_WIN | SAG_CMD_CHANGES_BUFFER | SAG_CMD_INTERNAL,
     "Delete from the cursor to line end"},

    DEFER("ed.file.open", SAG_ARITY_STR, SAG_CMD_PROMPTS, 23,
          "open a file"),
    {"ed.file.write", sag_file_cmd_write, SAG_ARITY_OPT_STR,
     SAG_CMD_NEEDS_WIN, "Write the active buffer, optionally to a path"},
    {"ed.file.write_quit", sag_file_cmd_write_quit, SAG_ARITY_OPT_STR,
     SAG_CMD_NEEDS_WIN, "Write the active buffer and quit"},
    {"ed.file.save", sag_file_cmd_save_current, SAG_ARITY_NONE,
     SAG_CMD_NEEDS_WIN, "Atomically save the active file"},
    {"ed.file.new", sag_file_cmd_new, SAG_ARITY_OPT_STR, 0U,
     "Create an empty buffer, optionally naming its file"},
    {"ed.file.reload", sag_file_cmd_reload, SAG_ARITY_NONE,
     SAG_CMD_NEEDS_WIN, "Reload the active file from disk"},
    DEFER("ed.file.close", SAG_ARITY_NONE, SAG_CMD_NEEDS_WIN, 23,
          "close the active file"),
    DEFER("ed.buf.next", SAG_ARITY_NONE, SAG_CMD_REPEATABLE, 23,
          "activate the next buffer"),
    DEFER("ed.buf.prev", SAG_ARITY_NONE, SAG_CMD_REPEATABLE, 23,
          "activate the previous buffer"),
    DEFER("ed.tab.goto", SAG_ARITY_INT, SAG_CMD_TAKES_COUNT, 23,
          "activate a numbered tab"),
    DEFER("ed.tab.new", SAG_ARITY_NONE, 0U, 23, "create a tab"),
    DEFER("ed.tab.close", SAG_ARITY_NONE, 0U, 23, "close the active tab"),
    DEFER("ed.group.next", SAG_ARITY_NONE, SAG_CMD_REPEATABLE, 24,
          "activate the next tab group"),
    DEFER("ed.group.prev", SAG_ARITY_NONE, SAG_CMD_REPEATABLE, 24,
          "activate the previous tab group"),
    {"ed.pane.split_h", sag_pane_cmd_split_h, SAG_ARITY_NONE,
     SAG_CMD_NEEDS_WIN, "Split the focused pane side by side"},
    {"ed.pane.split_v", sag_pane_cmd_split_v, SAG_ARITY_NONE,
     SAG_CMD_NEEDS_WIN, "Split the focused pane top and bottom"},
    {"ed.pane.close", sag_pane_cmd_close, SAG_ARITY_NONE,
     SAG_CMD_NEEDS_WIN, "Close the focused pane"},
    {"ed.pane.focus_left", sag_pane_cmd_focus_left, SAG_ARITY_NONE,
     SAG_CMD_NEEDS_WIN, "Focus the pane to the left"},
    {"ed.pane.focus_right", sag_pane_cmd_focus_right, SAG_ARITY_NONE,
     SAG_CMD_NEEDS_WIN, "Focus the pane to the right"},
    {"ed.pane.focus_up", sag_pane_cmd_focus_up, SAG_ARITY_NONE,
     SAG_CMD_NEEDS_WIN, "Focus the pane above"},
    {"ed.pane.focus_down", sag_pane_cmd_focus_down, SAG_ARITY_NONE,
     SAG_CMD_NEEDS_WIN, "Focus the pane below"},
    {"ed.pane.focus_next", sag_pane_cmd_focus_next, SAG_ARITY_NONE,
     SAG_CMD_NEEDS_WIN, "Focus the next pane in tree order"},
    {"ed.pane.grow", sag_pane_cmd_grow, SAG_ARITY_OPT_INT,
     SAG_CMD_TAKES_COUNT | SAG_CMD_NEEDS_WIN, "Grow the focused pane"},
    {"ed.pane.shrink", sag_pane_cmd_shrink, SAG_ARITY_OPT_INT,
     SAG_CMD_TAKES_COUNT | SAG_CMD_NEEDS_WIN, "Shrink the focused pane"},
    DEFER("ed.win.next", SAG_ARITY_NONE, SAG_CMD_REPEATABLE, 22,
          "focus the next window"),
    DEFER("ed.win.prev", SAG_ARITY_NONE, SAG_CMD_REPEATABLE, 22,
          "focus the previous window"),

    {"ed.search.open", sag_search_cmd_open, SAG_ARITY_OPT_STR,
     SAG_CMD_PROMPTS | SAG_CMD_NEEDS_WIN, "Open incremental search"},
    {"ed.search.open_back", sag_search_cmd_open_back, SAG_ARITY_OPT_STR,
     SAG_CMD_PROMPTS | SAG_CMD_NEEDS_WIN,
     "Open incremental search, backwards"},
    {"ed.search.next", sag_search_cmd_next, SAG_ARITY_OPT_INT,
     SAG_CMD_TAKES_COUNT | SAG_CMD_RECORDABLE | SAG_CMD_NEEDS_WIN,
     "Move to the next search match"},
    {"ed.search.prev", sag_search_cmd_prev, SAG_ARITY_OPT_INT,
     SAG_CMD_TAKES_COUNT | SAG_CMD_RECORDABLE | SAG_CMD_NEEDS_WIN,
     "Move to the previous search match"},
    {"ed.search.word_next", sag_search_cmd_word_next, SAG_ARITY_NONE,
     SAG_CMD_RECORDABLE | SAG_CMD_NEEDS_WIN,
     "Search forward for the word under the cursor"},
    {"ed.search.word_prev", sag_search_cmd_word_prev, SAG_ARITY_NONE,
     SAG_CMD_RECORDABLE | SAG_CMD_NEEDS_WIN,
     "Search backward for the word under the cursor"},
    {"ed.mark.set", sag_mark_cmd_set, SAG_ARITY_STR,
     SAG_CMD_CAPTURES_TEXT | SAG_CMD_NEEDS_WIN,
     "Set a named mark at the cursor"},
    {"ed.mark.jump", sag_mark_cmd_jump, SAG_ARITY_STR,
     SAG_CMD_CAPTURES_TEXT | SAG_CMD_NEEDS_WIN,
     "Jump to a named mark"},
    {"ed.search.clear_highlight", sag_search_cmd_clear_highlight,
     SAG_ARITY_NONE, SAG_CMD_NEEDS_WIN, "Clear match highlighting"},
    {"ed.jump.back", sag_jump_cmd_back, SAG_ARITY_OPT_INT,
     SAG_CMD_TAKES_COUNT | SAG_CMD_NEEDS_WIN,
     "Jump to an older position in this window's history"},
    {"ed.jump.fwd", sag_jump_cmd_fwd, SAG_ARITY_OPT_INT,
     SAG_CMD_TAKES_COUNT | SAG_CMD_NEEDS_WIN,
     "Jump to a newer position in this window's history"},
    {"ed.jump.list", sag_jump_cmd_list, SAG_ARITY_NONE, SAG_CMD_NEEDS_WIN,
     "Show this window's jumplist"},
    {"ed.change.older", sag_change_cmd_older, SAG_ARITY_OPT_INT,
     SAG_CMD_TAKES_COUNT | SAG_CMD_NEEDS_WIN,
     "Jump to an older change position in this buffer"},
    {"ed.change.newer", sag_change_cmd_newer, SAG_ARITY_OPT_INT,
     SAG_CMD_TAKES_COUNT | SAG_CMD_NEEDS_WIN,
     "Jump to a newer change position in this buffer"},
    {"ed.search.replace", sag_search_cmd_replace, SAG_ARITY_STR,
     SAG_CMD_CHANGES_BUFFER | SAG_CMD_NEEDS_WIN,
     "Substitute matches of a pattern in a line range"},
    {"ed.search.global", sag_search_cmd_global, SAG_ARITY_STR, 0U,
     "Rejected: :g is Fletch's query API in Sprint 34"},
    DEFER("ed.macro.record", SAG_ARITY_OPT_STR, SAG_CMD_PROMPTS, 35,
          "record a command macro"),
    DEFER("ed.macro.replay", SAG_ARITY_OPT_STR,
          SAG_CMD_REPEATABLE | SAG_CMD_RECORDABLE, 35,
          "replay a command macro"),
    {"ed.shell.run", sag_shell_cmd_run, SAG_ARITY_STR,
     SAG_CMD_RECORDABLE, "Run a shell command, streaming its output"},
    {"ed.shell.run_bg", sag_shell_cmd_run_bg, SAG_ARITY_STR,
     SAG_CMD_RECORDABLE, "Run a shell command without stealing focus"},
    {"ed.shell.read", sag_shell_cmd_read, SAG_ARITY_STR,
     SAG_CMD_RECORDABLE | SAG_CMD_NEEDS_WIN | SAG_CMD_CHANGES_BUFFER,
     "Insert a shell command's output at the cursor"},
    {"ed.shell.filter", sag_shell_cmd_filter, SAG_ARITY_STR,
     SAG_CMD_RECORDABLE | SAG_CMD_NEEDS_WIN | SAG_CMD_CHANGES_BUFFER,
     "Pipe a region through a shell command and replace it"},
    {"ed.shell.term", sag_shell_cmd_term, SAG_ARITY_NONE, 0U,
     "Interactive terminals are not a 1.0 feature"},
    {"ed.job.list", sag_job_cmd_list, SAG_ARITY_NONE, 0U,
     "Open the job table"},
    {"ed.job.kill", sag_job_cmd_kill, SAG_ARITY_OPT_INT,
     SAG_CMD_TAKES_COUNT, "Terminate a job's process group"},
    {"ed.job.kill_force", sag_job_cmd_kill_force, SAG_ARITY_OPT_INT,
     SAG_CMD_TAKES_COUNT, "Kill a job's process group"},
    {"ed.job.jump", sag_job_cmd_jump, SAG_ARITY_OPT_INT,
     SAG_CMD_TAKES_COUNT, "Focus a job's output buffer"},
    {"ed.job.clear_finished", sag_job_cmd_clear_finished, SAG_ARITY_NONE,
     0U, "Drop every finished job and its output buffer"},
    {"ed.job.rerun", sag_job_cmd_rerun, SAG_ARITY_OPT_INT,
     SAG_CMD_TAKES_COUNT, "Run a job's command line again"},
    DEFER("ed.git.stage", SAG_ARITY_NONE, SAG_CMD_NEEDS_WIN, 52,
          "stage the selected path"),
    DEFER("ed.lsp.goto", SAG_ARITY_STR, SAG_CMD_NEEDS_WIN, 47,
          "go to an LSP location"),
    DEFER("ed.ai.open", SAG_ARITY_NONE, SAG_CMD_PROMPTS, 49,
          "open the AI prompt"),
    DEFER("ed.plug.reload", SAG_ARITY_OPT_STR, 0U, 54,
          "reload a plugin")
};

#undef DEFER

typedef struct {
    const char *name;
    const char *argspec;
    u8 range_policy;
    const char *abbrev;
} BuiltinMeta;

static const BuiltinMeta builtin_meta[] = {
    {"ed.quit", "", SAG_RP_FORBID, "q"},
    {"ed.redraw", "", SAG_RP_FORBID, "redraw"},
    {"ed.edit.line.delete", "", SAG_RP_LINE, "d"},
    {"ed.file.open", "f", SAG_RP_FORBID, "e"},
    {"ed.file.write", "f", SAG_RP_FORBID, "w"},
    {"ed.file.write_quit", "f", SAG_RP_FORBID, "wq"},
    {"ed.file.new", "f", SAG_RP_FORBID, "new"},
    {"ed.file.reload", "", SAG_RP_FORBID, "reload"},
    {"ed.file.close", "", SAG_RP_FORBID, "close"},
    {"ed.search.open", "s", SAG_RP_FORBID, "search"},
    /* The substitution body is ONE opaque string; s18's tokenizer must
     * not try to understand `/` inside a regex. */
    {"ed.search.replace", "s", SAG_RP_OPT, "s"},
    {"ed.search.global", "s", SAG_RP_OPT, "g"},
    {"ed.mark.set", "s", SAG_RP_FORBID, "mark"},
    /* :! carries an arbitrary command line, so its argspec is one string
     * and the range decides run-vs-filter (§5). */
    {"ed.shell.run", "s", SAG_RP_OPT, NULL},
    {"ed.shell.read", "s", SAG_RP_FORBID, NULL},
    {"ed.shell.filter", "s", SAG_RP_REQUIRED, NULL},
    {"ed.job.list", "", SAG_RP_FORBID, "jobs"},
    {"ed.job.kill", "", SAG_RP_FORBID, NULL},
    {"ed.shell.term", "", SAG_RP_FORBID, "term"},
};

static bool word_in(const char *word, size_t len, const char *const *words,
                    size_t n)
{
    size_t i;

    for (i = 0; i < n; i++) {
        if (strlen(words[i]) == len && memcmp(words[i], word, len) == 0)
            return true;
    }
    return false;
}

static bool command_name_valid(const char *name)
{
    static const char *const app_verbs[] = {
        "quit", "quit_force", "suspend", "redraw", "repeat", "nop"};
    static const char *const domains[] = {
        "move", "edit", "mode", "sel", "cursor", "view", "ui",
        "file", "buf", "tab", "group", "pane", "win", "reg",
        "search", "macro", "job", "git", "lsp", "ai", "plug",
        "cmdline", "del", "shell",
        /* Sprint 21 */
        "jump", "change", "mark"};
    static const char *const verbs[] = {
        "home", "end", "next", "prev", "up", "down", "left", "right",
        "goto", "insert", "delete", "replace", "change", "yank", "paste", "toggle",
        "open", "close", "save", "new", "enter", "leave", "grow",
        "shrink", "expand", "contract", "list", "reload", "cancel",
        "text", "undo", "redo", "escape", "add", "above", "below", "center",
        "message_expand", "split_h", "split_v", "record", "replay", "stage",
        "first_nonblank", "last_nonblank", "half_page_up", "half_page_down",
        "page_up", "page_down", "after", "newline", "tab",
        "grapheme_left", "grapheme", "undo_barrier", "open_above",
        "open_below", "top", "bottom", "goto_line", "toggle_wrap",
        "number_style", "next_alt", "prev_alt", "home_alt", "end_alt",
        "match_prev", "match_next", "sub_prev", "sub_next", "kind",
        "swap_ends", "lines", "matches", "ends", "drop", "collapse",
        "case_upper", "case_lower", "case_toggle", "indent", "dedent",
        "shift_left", "shift_right", "join", "replace_char", "append",
        "write", "write_quit", "hist_prev", "hist_next",
        "complete_next", "complete_prev", "insert_register",
        "literal_next", "accept", "word_prev", "to_home", "to_end",
        /* Sprint 19 */
        "run", "run_bg", "read", "filter", "term", "kill", "kill_force",
        "jump", "clear_finished", "rerun",
        /* Sprint 21 */
        "back", "fwd", "older", "newer", "global",
        "open_back", "word_next", "clear_highlight", "set",
        /* Sprint 22 */
        "split_h", "split_v", "focus_left", "focus_right", "focus_up",
        "focus_down", "focus_next"};
    const char *segments[4];
    size_t lengths[4];
    const char *p;
    size_t n = 0;

    if (name == NULL)
        return false;
    p = name;
    for (;;) {
        const char *start = p;
        size_t len;

        if (n == SAG_ARRAY_LEN(segments))
            return false;
        while (*p != '\0' && *p != '.')
            p++;
        len = (size_t)(p - start);
        if (len == 0U || len > 16U || start[0] < 'a' || start[0] > 'z')
            return false;
        {
            size_t i;
            for (i = 1; i < len; i++) {
                if (!((start[i] >= 'a' && start[i] <= 'z') ||
                      (start[i] >= '0' && start[i] <= '9') ||
                      start[i] == '_'))
                    return false;
            }
        }
        segments[n] = start;
        lengths[n++] = len;
        if (*p == '\0')
            break;
        p++;
    }
    if (lengths[0] != 2U || memcmp(segments[0], "ed", 2U) != 0)
        return false;
    if (n == 2U)
        return word_in(segments[1], lengths[1], app_verbs,
                       SAG_ARRAY_LEN(app_verbs));
    if ((n != 3U && n != 4U) ||
        !word_in(segments[1], lengths[1], domains, SAG_ARRAY_LEN(domains)))
        return false;
    return word_in(segments[n - 1U], lengths[n - 1U], verbs,
                   SAG_ARRAY_LEN(verbs));
}

static bool help_names_sprint(const char *help)
{
    const char *p;

    if (help == NULL)
        return false;
    p = strstr(help, "Sprint ");
    if (p == NULL)
        return false;
    p += strlen("Sprint ");
    return isdigit((unsigned char)*p) != 0;
}

static void desc_validate(const CmdDesc *d)
{
    const u32 known_flags = SAG_CMD_REPEATABLE | SAG_CMD_TAKES_COUNT |
                            SAG_CMD_RECORDABLE | SAG_CMD_NEEDS_WIN |
                            SAG_CMD_CHANGES_BUFFER | SAG_CMD_PROMPTS |
                            SAG_CMD_DEFERRED | SAG_CMD_MULTI_AGGREGATE |
                            SAG_CMD_CAPTURES_TEXT | SAG_CMD_INTERNAL;

    if (d == NULL)
        SAG_BUG("sag_cmd_register: NULL descriptor");
    if (!command_name_valid(d->name))
        SAG_BUG("invalid command name: %s", d->name ? d->name : "(null)");
    if (d->fn == NULL)
        SAG_BUG("command %s has no implementation", d->name);
    if (d->arity > SAG_ARITY_OPT_STR)
        SAG_BUG("command %s has invalid arity %u", d->name,
                (unsigned)d->arity);
    if ((d->flags & ~known_flags) != 0U)
        SAG_BUG("command %s has unknown flags", d->name);
    if ((d->flags & SAG_CMD_REPEATABLE) != 0U &&
        (d->flags & SAG_CMD_TAKES_COUNT) != 0U)
        SAG_BUG("command %s is both REPEATABLE and TAKES_COUNT", d->name);
    if ((d->flags & SAG_CMD_CAPTURES_TEXT) != 0U &&
        d->arity != SAG_ARITY_STR)
        SAG_BUG("command %s captures text without string arity", d->name);
    if (d->help == NULL || d->help[0] == '\0')
        SAG_BUG("command %s has empty help", d->name);
    if ((d->flags & SAG_CMD_DEFERRED) != 0U &&
        !help_names_sprint(d->help))
        SAG_BUG("deferred command %s help does not name its sprint", d->name);
}

static const char *default_argspec(const CmdDesc *d)
{
    return d->arity == SAG_ARITY_NONE ? "" : "s";
}

static void entry_validate(const CmdEntry *entry)
{
    const char *p;

    if (entry == NULL)
        SAG_BUG("sag_cmd_register_entry: NULL entry");
    desc_validate(&entry->cmd);
    if (entry->range_policy > SAG_RP_REQUIRED)
        SAG_BUG("command %s has invalid range policy", entry->cmd.name);
    p = entry->argspec == NULL ? default_argspec(&entry->cmd) :
                                entry->argspec;
    while (*p != '\0') {
        if (*p != 'f' && *p != 'b' && *p != 'o' && *p != 'v' &&
            *p != 's' && !(*p == '*' && p[1] == '\0'))
            SAG_BUG("command %s has invalid argspec", entry->cmd.name);
        p++;
    }
    if (entry->abbrev != NULL) {
        if (entry->abbrev[0] == '\0')
            SAG_BUG("command %s has empty abbreviation", entry->cmd.name);
        for (p = entry->abbrev; *p != '\0'; p++) {
            if (!(isalnum((unsigned char)*p) || *p == '_'))
                SAG_BUG("command %s has invalid abbreviation",
                        entry->cmd.name);
        }
    }
}

static CmdId register_entry(const CmdEntry *entry)
{
    CmdEntry copy;
    const CmdDesc *d;
    u32 id;

    entry_validate(entry);
    d = &entry->cmd;
    if (strmap_has(&registry.names.map, d->name, strlen(d->name)))
        SAG_BUG("duplicate command registration: %s", d->name);
    if (registry.len == UINT32_MAX)
        SAG_BUG("command registry overflow");
    if (registry.len == registry.cap) {
        size_t cap = registry.cap ? registry.cap * 2U : 64U;

        if (cap < registry.cap)
            SAG_BUG("command registry allocation overflow");
        registry.entries = sag_xreallocarray(registry.entries, cap,
                                             sizeof(*registry.entries));
        registry.cap = cap;
    }
    id = sag_intern_cstr(&registry.names, d->name);
    if ((size_t)id != registry.len + 1U)
        SAG_BUG("command registry and interner order diverged");
    copy = *entry;
    copy.cmd = *d;
    copy.cmd.name = sag_intern_str(&registry.names, id);
    copy.cmd.help = arena_strdup(&registry.arena, d->help);
    copy.argspec = arena_strdup(
        &registry.arena,
        entry->argspec == NULL ? default_argspec(d) : entry->argspec);
    copy.abbrev = entry->abbrev == NULL ? NULL :
                  arena_strdup(&registry.arena, entry->abbrev);
    registry.entries[registry.len++] = copy;
    return (CmdId){id};
}

static CmdId register_desc(const CmdDesc *d)
{
    CmdEntry entry;

    if (d == NULL)
        SAG_BUG("sag_cmd_register: NULL descriptor");
    entry = (CmdEntry){*d, NULL, SAG_RP_FORBID, NULL};
    return register_entry(&entry);
}

static void install_builtin_meta(void)
{
    size_t i;

    for (i = 0U; i < SAG_ARRAY_LEN(builtin_meta); i++) {
        const BuiltinMeta *meta = &builtin_meta[i];
        void *found = strmap_get(&registry.names.map, meta->name,
                                 strlen(meta->name));
        u32 id = (u32)(uintptr_t)found;
        CmdEntry *entry;

        if (id == 0U || (size_t)id > registry.len)
            SAG_BUG("metadata names missing command: %s", meta->name);
        entry = &registry.entries[id - 1U];
        entry->argspec = arena_strdup(&registry.arena, meta->argspec);
        entry->range_policy = meta->range_policy;
        entry->abbrev = meta->abbrev == NULL ? NULL :
                        arena_strdup(&registry.arena, meta->abbrev);
        entry_validate(entry);
    }
}

void sag_cmd_init(void)
{
    size_t i;

    if (registry.initialized)
        return;
    arena_init(&registry.arena);
    interner_init(&registry.names, &registry.arena);
    registry.initialized = true;
    for (i = 0; i < SAG_ARRAY_LEN(builtins); i++)
        (void)register_desc(&builtins[i]);
    install_builtin_meta();
}

void sag_cmd_shutdown(void)
{
    if (!registry.initialized)
        return;
    interner_free(&registry.names);
    arena_free_all(&registry.arena);
    free(registry.entries);
    registry = (CmdRegistry){0};
}

CmdId sag_cmd_register(const CmdDesc *d)
{
    sag_cmd_init();
    return register_desc(d);
}

CmdId sag_cmd_register_entry(const CmdEntry *entry)
{
    sag_cmd_init();
    return register_entry(entry);
}

CmdId sag_cmd_lookup(const char *name, u32 len)
{
    void *found;

    sag_cmd_init();
    if (name == NULL)
        return SAG_CMD_NONE;
    found = strmap_get(&registry.names.map, name, len);
    return (CmdId){(u32)(uintptr_t)found};
}

const CmdDesc *sag_cmd_desc(CmdId id)
{
    sag_cmd_init();
    if (id.v == 0U || (size_t)id.v > registry.len)
        return NULL;
    return &registry.entries[id.v - 1U].cmd;
}

const CmdEntry *sag_cmd_entry(CmdId id)
{
    sag_cmd_init();
    if (id.v == 0U || (size_t)id.v > registry.len)
        return NULL;
    return &registry.entries[id.v - 1U];
}

static bool args_valid(const CmdDesc *d, const CmdCtx *cx)
{
    if (cx->source < SAG_SRC_KEY || cx->source > SAG_SRC_TEST)
        return false;
    if (cx->count == 0U)
        return false;
    if (cx->sarg == NULL && cx->sarg_len != 0U)
        return false;
    switch ((CmdArity)d->arity) {
    case SAG_ARITY_NONE:
        return cx->iarg == 0 && cx->sarg == NULL && cx->sarg_len == 0U;
    case SAG_ARITY_INT:
    case SAG_ARITY_OPT_INT:
        return cx->sarg == NULL && cx->sarg_len == 0U;
    case SAG_ARITY_STR:
        return cx->iarg == 0 && cx->sarg != NULL;
    case SAG_ARITY_OPT_STR:
        return cx->iarg == 0;
    }
    return false;
}

static CmdStatus command_fail(const CmdDesc *d, const char *reason,
                              CmdStatus status)
{
    sag_log(SAG_LOG_ERROR, "command failed: %s: %s", d->name, reason);
    return status;
}

static const char *deferred_sprint(const CmdDesc *d, char *number,
                                   size_t number_size)
{
    const char *sprint = strstr(d->help, "Sprint ");
    size_t n = 0;

    sprint += strlen("Sprint ");
    while (isdigit((unsigned char)sprint[n]) && n + 1U < number_size) {
        number[n] = sprint[n];
        n++;
    }
    number[n] = '\0';
    return number;
}

static CmdStatus command_deferred(const CmdDesc *d)
{
    char number[16];
    const char *sprint = deferred_sprint(d, number, sizeof(number));

    sag_log(SAG_LOG_ERROR,
            "command not implemented yet: %s lands in Sprint %s", d->name,
            sprint);
    return SAG_CMD_ERR_DEFERRED;
}

CmdStatus sag_cmd_prepare(CmdId id, CmdCtx *cx, const CmdDesc **out)
{
    const CmdDesc *d = sag_cmd_desc(id);

    if (out != NULL)
        *out = NULL;
    if (d == NULL || cx == NULL || out == NULL)
        return SAG_CMD_ERR_ARG;
    if ((d->flags & SAG_CMD_NEEDS_WIN) != 0U && cx->win == NULL)
        return command_fail(d, "no window", SAG_CMD_ERR_STATE);
    if ((d->flags & SAG_CMD_INTERNAL) != 0U &&
        cx->source == SAG_SRC_CMDLINE)
        return command_fail(d, "internal E command", SAG_CMD_ERR_ARG);
    if ((d->flags & SAG_CMD_DEFERRED) != 0U)
        return command_deferred(d);
    if (!args_valid(d, cx))
        return command_fail(d, "invalid arguments", SAG_CMD_ERR_ARG);
    if (registry.record_tap != NULL &&
        (d->flags & SAG_CMD_RECORDABLE) != 0U)
        registry.record_tap(id, cx);
    *out = d;
    return SAG_CMD_OK;
}

CmdStatus sag_cmd_invoke(CmdId id, CmdCtx *cx)
{
    const CmdDesc *d;
    CmdStatus status;
    u32 n;
    u32 i;

    status = sag_cmd_prepare(id, cx, &d);
    if (status != SAG_CMD_OK)
        return status;
    n = (d->flags & SAG_CMD_REPEATABLE) != 0U ? cx->count : 1U;
    for (i = 0; i < n && status == SAG_CMD_OK; i++)
        status = d->fn(cx);
    return status;
}

u32 sag_cmd_count(void)
{
    sag_cmd_init();
    return (u32)registry.len;
}

const CmdDesc *sag_cmd_at(u32 i)
{
    sag_cmd_init();
    if ((size_t)i >= registry.len)
        return NULL;
    return &registry.entries[i].cmd;
}

void sag_cmd_set_record_tap(CmdRecordTap tap)
{
    sag_cmd_init();
    registry.record_tap = tap;
}
