#include "edit/cmd.h"

#include <ctype.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "edit/bind.h"
#include "edit/edit_cmds.h"
#include "edit/jumplist.h"
#include "edit/opt.h"
#include "edit/pane_cmds.h"
#include "edit/search_cmds.h"
#include "edit/file_cmds.h"
#include "edit/flapi_cmds.h"
#include "edit/shell_cmds.h"
#include "edit/sel_actions.h"
#include "edit/ws_cmds.h"
#include "fl/flconf.h"
#include "fl/record.h"
#include "ui/pickers.h"
#include "ui/cmdline.h"
#include "ui/groupnav.h"
#include "ui/mouse.h"
#include "ui/macrobrowse.h"
#include "ui/grouppicker.h"
#include "ui/tabs.h"
#include "util/arena.h"
#include "util/intern.h"
#include "util/strmap.h"
#include "util/log.h"

typedef struct {
    Arena arena;
    Interner names;
    /* Sprint 34 §8: CMDWORD -> CmdId, built as commands register.  A
     * map rather than a scan because motion-block execution looks a
     * word up per WORD, inside the 1 us dispatch budget. */
    Strmap words;
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
            "Sprint " #sprint_ ": " help_, NULL                              \
    }

/*
 * A deferred command that is nonetheless RECORDABLE, and so must carry
 * its CMDWORD now: the word is what a macro records, and s35 cannot
 * add it later without changing what earlier recordings mean.
 */
#define DEFER_W(name_, arity_, flags_, sprint_, help_, word_)                  \
    {                                                                          \
        name_, deferred_unreachable, arity_, (flags_) | SAG_CMD_DEFERRED,      \
            "Sprint " #sprint_ ": " help_, word_                             \
    }

static const CmdDesc builtins[] = {
    {"ed.edit.insert.at", sag_edit_cmd_insert_at, SAG_ARITY_STR,
     SAG_CMD_CHANGES_BUFFER | SAG_CMD_RECORDABLE | SAG_CMD_NEEDS_WIN,
     "Insert bytes at an offset", "insert_at"},
    {"ed.edit.delete.span", sag_edit_cmd_delete_span, SAG_ARITY_NONE,
     SAG_CMD_CHANGES_BUFFER | SAG_CMD_RECORDABLE | SAG_CMD_NEEDS_WIN,
     "Delete the supplied span", "delete_span"},
    {"ed.edit.replace.span", sag_edit_cmd_replace_span, SAG_ARITY_STR,
     SAG_CMD_CHANGES_BUFFER | SAG_CMD_RECORDABLE | SAG_CMD_NEEDS_WIN,
     "Replace the supplied span", "replace_span"},
    {"ed.edit.delete.unit", sag_edit_cmd_delete_unit, SAG_ARITY_NONE,
     SAG_CMD_REPEATABLE | SAG_CMD_CHANGES_BUFFER | SAG_CMD_RECORDABLE |
         SAG_CMD_NEEDS_WIN,
     "Delete the current unit", "delete_unit"},
    {"ed.move.unit.up", sag_edit_cmd_move_unit_up, SAG_ARITY_NONE,
     SAG_CMD_RECORDABLE | SAG_CMD_NEEDS_WIN, "Move unit up", "unit_up"},
    {"ed.move.unit.down", sag_edit_cmd_move_unit_down, SAG_ARITY_NONE,
     SAG_CMD_RECORDABLE | SAG_CMD_NEEDS_WIN, "Move unit down", "unit_down"},
    {"ed.move.unit.up_alt", sag_edit_cmd_move_unit_up_alt, SAG_ARITY_NONE,
     SAG_CMD_RECORDABLE | SAG_CMD_NEEDS_WIN, "Move unit up (alternate)",
     "unit_up_alt"},
    {"ed.move.unit.down_alt", sag_edit_cmd_move_unit_down_alt, SAG_ARITY_NONE,
     SAG_CMD_RECORDABLE | SAG_CMD_NEEDS_WIN, "Move unit down (alternate)",
     "unit_down_alt"},
    {"ed.buf.open", sag_file_cmd_buf_open, SAG_ARITY_STR, 0U,
     "Open a buffer", NULL},
    {"ed.buf.close", sag_file_cmd_buf_close, SAG_ARITY_NONE,
     SAG_CMD_NEEDS_WIN, "Close the current buffer", NULL},
    {"ed.cursor.set", sag_edit_cmd_cursor_set, SAG_ARITY_NONE,
     SAG_CMD_NEEDS_WIN, "Set the primary cursor offset", NULL},
    {"ed.cursor.set_many", sag_flapi_cmd_cursor_set_many, SAG_ARITY_NONE,
     SAG_CMD_NEEDS_WIN | SAG_CMD_MULTI_AGGREGATE,
     "Replace a window's cursor set", NULL},
    {"ed.cursor.move", sag_flapi_cmd_cursor_move, SAG_ARITY_STR,
     SAG_CMD_REPEATABLE | SAG_CMD_NEEDS_WIN,
     "Move one cursor by a named unit and direction", NULL},
    {"ed.win.split", sag_flapi_cmd_win_split, SAG_ARITY_STR,
     SAG_CMD_NEEDS_WIN, "Split a window horizontally or vertically", NULL},
    {"ed.win.focus", sag_flapi_cmd_win_focus, SAG_ARITY_NONE,
     SAG_CMD_NEEDS_WIN, "Focus a specific window", NULL},
    {"ed.edit.yank", sag_flapi_cmd_span_yank, SAG_ARITY_OPT_STR,
     SAG_CMD_NEEDS_WIN, "Yank a supplied span into a register", NULL},
    {"ed.reg.set", sag_flapi_cmd_reg_set, SAG_ARITY_STR,
     SAG_CMD_INTERNAL, "Set macro source in a named register", NULL},
    {"ed.opt.get", sag_opt_cmd_get, SAG_ARITY_STR, 0U,
     "Read an editor option", NULL},
    {"ed.opt.set", sag_opt_cmd_set, SAG_ARITY_STR, 0U,
     "Set an editor option", NULL},
    {"ed.opt.set_many", sag_opt_cmdline_set, SAG_ARITY_NONE, 0U,
     "Set an editor option from E mode", NULL},
    {"ed.fl.eval", sag_fl_cmd_eval, SAG_ARITY_STR, 0U,
     "Evaluate Fletch in the persistent editor runtime", NULL},
    {"ed.fl.closure", sag_bind_closure_cmd, SAG_ARITY_INT,
     SAG_CMD_INTERNAL, "Invoke a Fletch closure bound to a key", NULL},
    {"ed.config.reload", sag_config_cmd_reload, SAG_ARITY_NONE, 0U,
     "Reload the runtime, user, and workspace configuration", NULL},
    {"ed.config.edit", sag_config_cmd_edit, SAG_ARITY_NONE, 0U,
     "Open the user init.fl", NULL},
    {"ed.map", sag_bind_cmd_map, SAG_ARITY_NONE, 0U,
     "List configured bindings for the current mode", NULL},
    {"ed.nop", cmd_nop, SAG_ARITY_NONE, 0U, "Do nothing", NULL},
    {"ed.quit", sag_file_cmd_quit, SAG_ARITY_NONE, 0U,
     "Quit, prompting when the buffer is dirty", NULL},
    {"ed.quit_force", sag_file_cmd_quit_force, SAG_ARITY_NONE, 0U,
     "Quit without discarding the recovery journal", NULL},
    {"ed.suspend", sag_file_cmd_suspend, SAG_ARITY_NONE, 0U,
     "Suspend the editor and restore the terminal", NULL},
    {"ed.redraw", sag_file_cmd_redraw, SAG_ARITY_NONE, 0U,
     "Redraw the complete display", NULL},
    {"ed.repeat", sag_record_cmd_repeat, SAG_ARITY_NONE, 0U,
     "Unavailable until resolved command arguments are retained", NULL},

    {"ed.move.buf.home", sag_edit_cmd_move_buf_home, SAG_ARITY_NONE,
     SAG_CMD_REPEATABLE | SAG_CMD_RECORDABLE | SAG_CMD_NEEDS_WIN,
     "Move to the start of the buffer", "buf_home"},
    {"ed.move.buf.end", sag_edit_cmd_move_buf_end, SAG_ARITY_NONE,
     SAG_CMD_TAKES_COUNT | SAG_CMD_RECORDABLE | SAG_CMD_NEEDS_WIN,
     "Move to the end of the buffer or a counted line", "buf_end"},
    {"ed.move.line.home", sag_edit_cmd_move_line_home, SAG_ARITY_NONE,
     SAG_CMD_REPEATABLE | SAG_CMD_RECORDABLE | SAG_CMD_NEEDS_WIN,
     "Move to the start of the line", "line_home"},
    {"ed.move.line.end", sag_edit_cmd_move_line_end, SAG_ARITY_NONE,
     SAG_CMD_REPEATABLE | SAG_CMD_RECORDABLE | SAG_CMD_NEEDS_WIN,
     "Move to the end of the line", "line_end"},
    {"ed.move.line.up", sag_edit_cmd_move_line_up, SAG_ARITY_NONE,
     SAG_CMD_REPEATABLE | SAG_CMD_RECORDABLE | SAG_CMD_NEEDS_WIN,
     "Move one line up", "up"},
    {"ed.move.line.down", sag_edit_cmd_move_line_down, SAG_ARITY_NONE,
     SAG_CMD_REPEATABLE | SAG_CMD_RECORDABLE | SAG_CMD_NEEDS_WIN,
     "Move one line down", "down"},
    {"ed.move.line.first_nonblank", sag_edit_cmd_move_line_first_nonblank,
     SAG_ARITY_NONE,
     SAG_CMD_REPEATABLE | SAG_CMD_RECORDABLE | SAG_CMD_NEEDS_WIN,
     "Move to the first nonblank grapheme", "home_text"},
    {"ed.move.line.last_nonblank", sag_edit_cmd_move_line_last_nonblank,
     SAG_ARITY_NONE,
     SAG_CMD_REPEATABLE | SAG_CMD_RECORDABLE | SAG_CMD_NEEDS_WIN,
     "Move to the last nonblank grapheme", "end_text"},
    {"ed.move.line.half_page_up", sag_edit_cmd_move_line_half_page_up,
     SAG_ARITY_NONE,
     SAG_CMD_REPEATABLE | SAG_CMD_RECORDABLE | SAG_CMD_NEEDS_WIN,
     "Move half a viewport up", "half_up"},
    {"ed.move.line.half_page_down", sag_edit_cmd_move_line_half_page_down,
     SAG_ARITY_NONE,
     SAG_CMD_REPEATABLE | SAG_CMD_RECORDABLE | SAG_CMD_NEEDS_WIN,
     "Move half a viewport down", "half_down"},
    {"ed.move.unit.next", sag_edit_cmd_move_unit_next, SAG_ARITY_NONE,
     SAG_CMD_REPEATABLE | SAG_CMD_RECORDABLE | SAG_CMD_NEEDS_WIN,
     "Move to the next unit", "unit_next"},
    {"ed.move.unit.prev", sag_edit_cmd_move_unit_prev, SAG_ARITY_NONE,
     SAG_CMD_REPEATABLE | SAG_CMD_RECORDABLE | SAG_CMD_NEEDS_WIN,
     "Move to the previous unit", "unit_prev"},
    {"ed.move.unit.home", sag_edit_cmd_move_unit_home, SAG_ARITY_NONE,
     SAG_CMD_REPEATABLE | SAG_CMD_RECORDABLE | SAG_CMD_NEEDS_WIN,
     "Move to the start of the current unit", "unit_home"},
    {"ed.move.unit.end", sag_edit_cmd_move_unit_end, SAG_ARITY_NONE,
     SAG_CMD_REPEATABLE | SAG_CMD_RECORDABLE | SAG_CMD_NEEDS_WIN,
     "Move to the end of the current unit", "unit_end"},
    {"ed.move.unit.next_alt", sag_edit_cmd_move_unit_next_alt,
     SAG_ARITY_NONE,
     SAG_CMD_REPEATABLE | SAG_CMD_RECORDABLE | SAG_CMD_NEEDS_WIN,
     "Move to the next alternate unit", "unit_next_alt"},
    {"ed.move.unit.prev_alt", sag_edit_cmd_move_unit_prev_alt,
     SAG_ARITY_NONE,
     SAG_CMD_REPEATABLE | SAG_CMD_RECORDABLE | SAG_CMD_NEEDS_WIN,
     "Move to the previous alternate unit", "unit_prev_alt"},
    {"ed.move.unit.home_alt", sag_edit_cmd_move_unit_home_alt,
     SAG_ARITY_NONE,
     SAG_CMD_REPEATABLE | SAG_CMD_RECORDABLE | SAG_CMD_NEEDS_WIN,
     "Move to the alternate unit start", "unit_home_alt"},
    {"ed.move.unit.end_alt", sag_edit_cmd_move_unit_end_alt,
     SAG_ARITY_NONE,
     SAG_CMD_REPEATABLE | SAG_CMD_RECORDABLE | SAG_CMD_NEEDS_WIN,
     "Move to the alternate unit end", "unit_end_alt"},
    {"ed.move.block.match_prev", sag_edit_cmd_move_block_match_prev,
     SAG_ARITY_NONE,
     SAG_CMD_REPEATABLE | SAG_CMD_RECORDABLE | SAG_CMD_NEEDS_WIN,
     "Move to the enclosing opening delimiter", "match_prev"},
    {"ed.move.block.match_next", sag_edit_cmd_move_block_match_next,
     SAG_ARITY_NONE,
     SAG_CMD_REPEATABLE | SAG_CMD_RECORDABLE | SAG_CMD_NEEDS_WIN,
     "Move to the enclosing closing delimiter", "match_next"},
    {"ed.move.word.sub_prev", sag_edit_cmd_move_word_sub_prev,
     SAG_ARITY_NONE,
     SAG_CMD_REPEATABLE | SAG_CMD_RECORDABLE | SAG_CMD_NEEDS_WIN,
     "Move to the previous subword", "subword_prev"},
    {"ed.move.word.sub_next", sag_edit_cmd_move_word_sub_next,
     SAG_ARITY_NONE,
     SAG_CMD_REPEATABLE | SAG_CMD_RECORDABLE | SAG_CMD_NEEDS_WIN,
     "Move to the next subword", "subword_next"},
    {"ed.move.char.prev", sag_edit_cmd_move_char_prev, SAG_ARITY_NONE,
     SAG_CMD_REPEATABLE | SAG_CMD_RECORDABLE | SAG_CMD_NEEDS_WIN,
     "Move one grapheme left", "char_prev"},
    {"ed.move.char.next", sag_edit_cmd_move_char_next, SAG_ARITY_NONE,
     SAG_CMD_REPEATABLE | SAG_CMD_RECORDABLE | SAG_CMD_NEEDS_WIN,
     "Move one grapheme right", "char_next"},
    {"ed.move.char.left", sag_edit_cmd_move_char_prev, SAG_ARITY_NONE,
     SAG_CMD_REPEATABLE | SAG_CMD_RECORDABLE | SAG_CMD_NEEDS_WIN,
     "Alias for moving one grapheme left", "char_left"},
    {"ed.move.char.right", sag_edit_cmd_move_char_next, SAG_ARITY_NONE,
     SAG_CMD_REPEATABLE | SAG_CMD_RECORDABLE | SAG_CMD_NEEDS_WIN,
     "Alias for moving one grapheme right", "char_right"},

    {"ed.edit.insert.text", sag_edit_cmd_insert_text, SAG_ARITY_STR,
     SAG_CMD_RECORDABLE | SAG_CMD_NEEDS_WIN | SAG_CMD_CHANGES_BUFFER,
     "Insert UTF-8 text at the cursor", "insert"},
    {"ed.edit.insert.newline", sag_edit_cmd_insert_newline, SAG_ARITY_NONE,
     SAG_CMD_RECORDABLE | SAG_CMD_NEEDS_WIN | SAG_CMD_CHANGES_BUFFER,
     "Insert the buffer's native line ending", "newline"},
    {"ed.edit.insert.tab", sag_edit_cmd_insert_tab, SAG_ARITY_NONE,
     SAG_CMD_RECORDABLE | SAG_CMD_NEEDS_WIN | SAG_CMD_CHANGES_BUFFER,
     "Insert a literal tab", "tab"},
    {"ed.edit.insert.after", sag_edit_cmd_insert_after, SAG_ARITY_NONE,
     SAG_CMD_RECORDABLE | SAG_CMD_NEEDS_WIN,
     "Enter insert mode after the current grapheme", "append"},
    {"ed.edit.line.open_below", sag_edit_cmd_open_below, SAG_ARITY_NONE,
     SAG_CMD_RECORDABLE | SAG_CMD_NEEDS_WIN | SAG_CMD_CHANGES_BUFFER,
     "Open a new line below and enter insert mode", "open_below"},
    {"ed.edit.line.open_above", sag_edit_cmd_open_above, SAG_ARITY_NONE,
     SAG_CMD_RECORDABLE | SAG_CMD_NEEDS_WIN | SAG_CMD_CHANGES_BUFFER,
     "Open a new line above and enter insert mode", "open_above"},
    {"ed.edit.delete.grapheme_left", sag_edit_cmd_delete_grapheme_left,
     SAG_ARITY_NONE,
     SAG_CMD_REPEATABLE | SAG_CMD_RECORDABLE | SAG_CMD_NEEDS_WIN |
         SAG_CMD_CHANGES_BUFFER,
     "Delete the grapheme left of the cursor", "backspace"},
    {"ed.edit.delete.grapheme", sag_edit_cmd_delete_grapheme,
     SAG_ARITY_NONE,
     SAG_CMD_REPEATABLE | SAG_CMD_RECORDABLE | SAG_CMD_NEEDS_WIN |
         SAG_CMD_CHANGES_BUFFER,
     "Delete the grapheme at the cursor", "del_char"},
    {"ed.edit.line.delete", sag_edit_cmd_delete_line, SAG_ARITY_NONE,
     SAG_CMD_REPEATABLE | SAG_CMD_RECORDABLE | SAG_CMD_NEEDS_WIN |
         SAG_CMD_CHANGES_BUFFER,
     "Delete the current logical line", "del_line"},
    {"ed.edit.delete.prev", sag_edit_cmd_delete_grapheme_left,
     SAG_ARITY_NONE,
     SAG_CMD_REPEATABLE | SAG_CMD_RECORDABLE | SAG_CMD_NEEDS_WIN |
         SAG_CMD_CHANGES_BUFFER,
     "Alias for deleting the previous grapheme", "del_prev"},
    {"ed.edit.delete.next", sag_edit_cmd_delete_grapheme, SAG_ARITY_NONE,
     SAG_CMD_REPEATABLE | SAG_CMD_RECORDABLE | SAG_CMD_NEEDS_WIN |
         SAG_CMD_CHANGES_BUFFER,
     "Alias for deleting the next grapheme", "del_next"},
    {"ed.edit.undo", sag_edit_cmd_undo, SAG_ARITY_NONE,
     SAG_CMD_REPEATABLE | SAG_CMD_RECORDABLE | SAG_CMD_NEEDS_WIN,
     "Undo the last edit transaction", "undo"},
    {"ed.edit.redo", sag_edit_cmd_redo, SAG_ARITY_NONE,
     SAG_CMD_REPEATABLE | SAG_CMD_RECORDABLE | SAG_CMD_NEEDS_WIN,
     "Redo the last undone transaction", "redo"},
    {"ed.edit.undo_barrier", sag_edit_cmd_undo_barrier, SAG_ARITY_NONE,
     SAG_CMD_NEEDS_WIN, "Close the active insert transaction", NULL},
    {"ed.mode.enter", sag_edit_cmd_mode_enter, SAG_ARITY_STR,
     SAG_CMD_RECORDABLE | SAG_CMD_INTERNAL,
     "Enter L/W/B/I/H/E; F Sprint 52", "mode"},
    {"ed.mode.escape", sag_edit_cmd_mode_escape, SAG_ARITY_NONE,
     SAG_CMD_RECORDABLE | SAG_CMD_INTERNAL, "Return to line mode", "escape"},
    {"ed.sel.expand", sag_edit_cmd_sel_unit_expand, SAG_ARITY_NONE,
     SAG_CMD_REPEATABLE | SAG_CMD_RECORDABLE | SAG_CMD_NEEDS_WIN,
     "Expand the selection to the next structural unit", "sel_expand"},
    {"ed.sel.contract", sag_edit_cmd_sel_unit_contract, SAG_ARITY_NONE,
     SAG_CMD_REPEATABLE | SAG_CMD_RECORDABLE | SAG_CMD_NEEDS_WIN,
     "Contract the selection to the previous structural unit", "sel_contract"},
    {"ed.sel.unit.expand", sag_edit_cmd_sel_unit_expand, SAG_ARITY_NONE,
     SAG_CMD_REPEATABLE | SAG_CMD_RECORDABLE | SAG_CMD_NEEDS_WIN,
     "Expand to the next structural unit", "unit_expand"},
    {"ed.sel.unit.contract", sag_edit_cmd_sel_unit_contract,
     SAG_ARITY_NONE,
     SAG_CMD_REPEATABLE | SAG_CMD_RECORDABLE | SAG_CMD_NEEDS_WIN,
     "Contract to the previous structural unit", "unit_contract"},
    {"ed.sel.kind", sag_edit_cmd_sel_kind, SAG_ARITY_STR,
     SAG_CMD_RECORDABLE | SAG_CMD_NEEDS_WIN,
     "Choose character, line, or rectangular selection geometry", "sel_kind"},
    {"ed.sel.swap_ends", sag_edit_cmd_sel_swap_ends, SAG_ARITY_NONE,
     SAG_CMD_RECORDABLE | SAG_CMD_NEEDS_WIN,
     "Exchange the active and anchored ends of each selection", "swap_ends"},
    {"ed.sel.yank", sag_sel_cmd_yank, SAG_ARITY_NONE,
     SAG_CMD_RECORDABLE | SAG_CMD_NEEDS_WIN | SAG_CMD_MULTI_AGGREGATE,
     "Yank the active selections", "yank"},
    {"ed.sel.delete", sag_sel_cmd_delete, SAG_ARITY_NONE,
     SAG_CMD_RECORDABLE | SAG_CMD_NEEDS_WIN | SAG_CMD_CHANGES_BUFFER |
         SAG_CMD_MULTI_AGGREGATE,
     "Delete the active selections", "sel_delete"},
    {"ed.sel.change", sag_sel_cmd_change, SAG_ARITY_NONE,
     SAG_CMD_RECORDABLE | SAG_CMD_NEEDS_WIN | SAG_CMD_CHANGES_BUFFER |
         SAG_CMD_MULTI_AGGREGATE,
     "Change the active selections", "change"},
    {"ed.sel.case_upper", sag_sel_cmd_case_upper, SAG_ARITY_NONE,
     SAG_CMD_RECORDABLE | SAG_CMD_NEEDS_WIN | SAG_CMD_CHANGES_BUFFER |
         SAG_CMD_MULTI_AGGREGATE,
     "Uppercase the active selections", "upper"},
    {"ed.sel.case_lower", sag_sel_cmd_case_lower, SAG_ARITY_NONE,
     SAG_CMD_RECORDABLE | SAG_CMD_NEEDS_WIN | SAG_CMD_CHANGES_BUFFER |
         SAG_CMD_MULTI_AGGREGATE,
     "Lowercase the active selections", "lower"},
    {"ed.sel.case_toggle", sag_sel_cmd_case_toggle, SAG_ARITY_NONE,
     SAG_CMD_RECORDABLE | SAG_CMD_NEEDS_WIN | SAG_CMD_CHANGES_BUFFER |
         SAG_CMD_MULTI_AGGREGATE,
     "Toggle case in the active selections", "case_toggle"},
    {"ed.sel.indent", sag_sel_cmd_indent, SAG_ARITY_NONE,
     SAG_CMD_RECORDABLE | SAG_CMD_NEEDS_WIN | SAG_CMD_CHANGES_BUFFER |
         SAG_CMD_MULTI_AGGREGATE,
     "Indent lines covered by the selection", "indent"},
    {"ed.sel.dedent", sag_sel_cmd_dedent, SAG_ARITY_NONE,
     SAG_CMD_RECORDABLE | SAG_CMD_NEEDS_WIN | SAG_CMD_CHANGES_BUFFER |
         SAG_CMD_MULTI_AGGREGATE,
     "Dedent lines covered by the selection", "dedent"},
    {"ed.sel.shift_left", sag_sel_cmd_shift_left, SAG_ARITY_NONE,
     SAG_CMD_RECORDABLE | SAG_CMD_NEEDS_WIN | SAG_CMD_CHANGES_BUFFER |
         SAG_CMD_MULTI_AGGREGATE,
     "Shift selected text left", "shift_left"},
    {"ed.sel.shift_right", sag_sel_cmd_shift_right, SAG_ARITY_NONE,
     SAG_CMD_RECORDABLE | SAG_CMD_NEEDS_WIN | SAG_CMD_CHANGES_BUFFER |
         SAG_CMD_MULTI_AGGREGATE,
     "Shift selected text right", "shift_right"},
    {"ed.sel.join", sag_sel_cmd_join, SAG_ARITY_NONE,
     SAG_CMD_RECORDABLE | SAG_CMD_NEEDS_WIN | SAG_CMD_CHANGES_BUFFER |
         SAG_CMD_MULTI_AGGREGATE,
     "Join lines covered by the selection", "join"},
    {"ed.sel.replace_char", sag_sel_cmd_replace_char, SAG_ARITY_STR,
     SAG_CMD_RECORDABLE | SAG_CMD_NEEDS_WIN | SAG_CMD_CHANGES_BUFFER |
         SAG_CMD_MULTI_AGGREGATE | SAG_CMD_CAPTURES_TEXT,
     "Replace selected graphemes with one grapheme", "replace_char"},
    {"ed.edit.rect.insert", sag_sel_cmd_rect_insert, SAG_ARITY_NONE,
     SAG_CMD_RECORDABLE | SAG_CMD_NEEDS_WIN | SAG_CMD_CHANGES_BUFFER |
         SAG_CMD_MULTI_AGGREGATE,
     "Insert at the left edge of a rectangular selection", "rect_insert"},
    {"ed.edit.rect.append", sag_sel_cmd_rect_append, SAG_ARITY_NONE,
     SAG_CMD_RECORDABLE | SAG_CMD_NEEDS_WIN | SAG_CMD_CHANGES_BUFFER |
         SAG_CMD_MULTI_AGGREGATE,
     "Insert at the right edge of a rectangular selection", "rect_append"},
    {"ed.cursor.lift.lines", sag_edit_cmd_cursor_lift_lines, SAG_ARITY_NONE,
     SAG_CMD_RECORDABLE | SAG_CMD_NEEDS_WIN,
     "Lift a selection to one cursor per line", "lift_lines"},
    {"ed.cursor.lift.matches", sag_edit_cmd_cursor_lift_matches,
     SAG_ARITY_OPT_STR, SAG_CMD_RECORDABLE | SAG_CMD_NEEDS_WIN,
     "Lift literal matches in the selection to cursors", "lift_matches"},
    {"ed.cursor.lift.ends", sag_edit_cmd_cursor_lift_ends, SAG_ARITY_NONE,
     SAG_CMD_RECORDABLE | SAG_CMD_NEEDS_WIN,
     "Lift both ends of each selection to cursors", "lift_ends"},
    {"ed.cursor.add.above", sag_edit_cmd_cursor_add_above, SAG_ARITY_NONE,
     SAG_CMD_RECORDABLE | SAG_CMD_NEEDS_WIN,
     "Add a cursor on the preceding line", "cursor_above"},
    {"ed.cursor.add.below", sag_edit_cmd_cursor_add_below, SAG_ARITY_NONE,
     SAG_CMD_RECORDABLE | SAG_CMD_NEEDS_WIN,
     "Add a cursor on the following line", "cursor_below"},
    {"ed.cursor.drop", sag_edit_cmd_cursor_drop, SAG_ARITY_NONE,
     SAG_CMD_RECORDABLE | SAG_CMD_NEEDS_WIN,
     "Drop the most recently added cursor", "cursor_drop"},
    {"ed.cursor.collapse", sag_edit_cmd_cursor_collapse, SAG_ARITY_NONE,
     SAG_CMD_RECORDABLE | SAG_CMD_NEEDS_WIN,
     "Keep only the primary cursor", "collapse"},
    {"ed.view.center", sag_edit_cmd_view_center, SAG_ARITY_NONE,
     SAG_CMD_RECORDABLE | SAG_CMD_NEEDS_WIN, "Center the cursor line", "center"},
    {"ed.view.top", sag_edit_cmd_view_top, SAG_ARITY_NONE,
     SAG_CMD_RECORDABLE | SAG_CMD_NEEDS_WIN,
     "Place the cursor line at the top", "view_top"},
    {"ed.view.bottom", sag_edit_cmd_view_bottom, SAG_ARITY_NONE,
     SAG_CMD_RECORDABLE | SAG_CMD_NEEDS_WIN,
     "Place the cursor line at the bottom", "view_bottom"},
    {"ed.view.scroll.up", sag_edit_cmd_view_scroll_up, SAG_ARITY_NONE,
     SAG_CMD_REPEATABLE | SAG_CMD_RECORDABLE | SAG_CMD_NEEDS_WIN,
     "Scroll the active view up one display row", "scroll_up"},
    {"ed.view.scroll.down", sag_edit_cmd_view_scroll_down, SAG_ARITY_NONE,
     SAG_CMD_REPEATABLE | SAG_CMD_RECORDABLE | SAG_CMD_NEEDS_WIN,
     "Scroll the active view down one display row", "scroll_down"},
    {"ed.view.up", sag_edit_cmd_view_scroll_up, SAG_ARITY_NONE,
     SAG_CMD_REPEATABLE | SAG_CMD_RECORDABLE | SAG_CMD_NEEDS_WIN,
     "Alias for scrolling the active view up", "view_up"},
    {"ed.view.down", sag_edit_cmd_view_scroll_down, SAG_ARITY_NONE,
     SAG_CMD_REPEATABLE | SAG_CMD_RECORDABLE | SAG_CMD_NEEDS_WIN,
     "Alias for scrolling the active view down", "view_down"},
    {"ed.view.page_up", sag_edit_cmd_view_page_up, SAG_ARITY_NONE,
     SAG_CMD_REPEATABLE | SAG_CMD_RECORDABLE | SAG_CMD_NEEDS_WIN,
     "Move one viewport up", "page_up"},
    {"ed.view.page_down", sag_edit_cmd_view_page_down, SAG_ARITY_NONE,
     SAG_CMD_REPEATABLE | SAG_CMD_RECORDABLE | SAG_CMD_NEEDS_WIN,
     "Move one viewport down", "page_down"},
    {"ed.view.half_page_up", sag_edit_cmd_view_half_page_up,
     SAG_ARITY_NONE,
     SAG_CMD_REPEATABLE | SAG_CMD_RECORDABLE | SAG_CMD_NEEDS_WIN,
     "Scroll half a viewport up", "view_half_up"},
    {"ed.view.half_page_down", sag_edit_cmd_view_half_page_down,
     SAG_ARITY_NONE,
     SAG_CMD_REPEATABLE | SAG_CMD_RECORDABLE | SAG_CMD_NEEDS_WIN,
     "Scroll half a viewport down", "view_half_down"},
    {"ed.view.goto_line", sag_edit_cmd_view_goto_line, SAG_ARITY_NONE,
     SAG_CMD_TAKES_COUNT | SAG_CMD_RECORDABLE | SAG_CMD_NEEDS_WIN,
     "Go to a counted line and center it", "goto_line"},
    {"ed.view.toggle_wrap", sag_edit_cmd_view_toggle_wrap, SAG_ARITY_NONE,
     SAG_CMD_RECORDABLE | SAG_CMD_NEEDS_WIN, "Toggle line wrapping", "toggle_wrap"},
    {"ed.view.number_style", sag_edit_cmd_view_number_style, SAG_ARITY_STR,
     SAG_CMD_RECORDABLE | SAG_CMD_NEEDS_WIN,
     "Set line numbers to none, abs, rel, or hybrid", "number_style"},
    {"ed.ui.message_expand", sag_edit_cmd_message_expand, SAG_ARITY_NONE,
     SAG_CMD_PROMPTS, "Expand the current message", NULL},
    {"ed.ui.cancel", sag_edit_cmd_ui_cancel, SAG_ARITY_NONE, 0U,
     "Cancel the active prompt or message overlay", NULL},

    {"ed.cmdline.hist_prev", sag_cmdline_cmd_hist_prev, SAG_ARITY_NONE,
     SAG_CMD_NEEDS_WIN | SAG_CMD_INTERNAL,
     "Find the previous matching command-line history entry", NULL},
    {"ed.cmdline.hist_next", sag_cmdline_cmd_hist_next, SAG_ARITY_NONE,
     SAG_CMD_NEEDS_WIN | SAG_CMD_INTERNAL,
     "Find the next matching command-line history entry", NULL},
    {"ed.cmdline.complete_next", sag_cmdline_cmd_complete_next,
     SAG_ARITY_NONE, SAG_CMD_NEEDS_WIN | SAG_CMD_INTERNAL,
     "Open or advance command-line completion", NULL},
    {"ed.cmdline.complete_prev", sag_cmdline_cmd_complete_prev,
     SAG_ARITY_NONE, SAG_CMD_NEEDS_WIN | SAG_CMD_INTERNAL,
     "Open or reverse command-line completion", NULL},
    {"ed.cmdline.insert_register", sag_cmdline_cmd_insert_register,
     SAG_ARITY_STR,
     SAG_CMD_NEEDS_WIN | SAG_CMD_CAPTURES_TEXT | SAG_CMD_INTERNAL,
     "Insert one named register into the command line", NULL},
    {"ed.cmdline.literal_next", sag_cmdline_cmd_literal_next,
     SAG_ARITY_STR,
     SAG_CMD_NEEDS_WIN | SAG_CMD_CAPTURES_TEXT | SAG_CMD_INTERNAL,
     "Insert the next text-producing key literally", NULL},
    {"ed.cmdline.ghost.accept", sag_cmdline_cmd_ghost_accept,
     SAG_ARITY_NONE, SAG_CMD_NEEDS_WIN | SAG_CMD_INTERNAL,
     "Accept the inline suggestion, or move one grapheme right", NULL},
    /* Sprint 18.5 §10.  complete_next/prev stay as the names the keymap
     * and the goldens already use; these are the same behaviours under
     * the menu's own namespace, plus the two the old menu could not do. */
    {"ed.cmdline.menu.next", sag_cmdline_cmd_complete_next, SAG_ARITY_NONE,
     SAG_CMD_NEEDS_WIN | SAG_CMD_INTERNAL, "Select the next menu row", NULL},
    {"ed.cmdline.menu.prev", sag_cmdline_cmd_complete_prev, SAG_ARITY_NONE,
     SAG_CMD_NEEDS_WIN | SAG_CMD_INTERNAL,
     "Select the previous menu row", NULL},
    {"ed.cmdline.menu.page_next", sag_cmdline_cmd_menu_page_next,
     SAG_ARITY_NONE, SAG_CMD_NEEDS_WIN | SAG_CMD_INTERNAL,
     "Move one visible page down the menu", NULL},
    {"ed.cmdline.menu.page_prev", sag_cmdline_cmd_menu_page_prev,
     SAG_ARITY_NONE, SAG_CMD_NEEDS_WIN | SAG_CMD_INTERNAL,
     "Move one visible page up the menu", NULL},
    {"ed.cmdline.menu.accept", sag_cmdline_cmd_menu_accept, SAG_ARITY_NONE,
     SAG_CMD_NEEDS_WIN | SAG_CMD_INTERNAL,
     "Accept the selected menu row", NULL},
    {"ed.cmdline.menu.dismiss", sag_cmdline_cmd_menu_dismiss,
     SAG_ARITY_NONE, SAG_CMD_NEEDS_WIN | SAG_CMD_INTERNAL,
     "Close the menu without accepting", NULL},
    {"ed.cmdline.accept", sag_cmdline_cmd_accept, SAG_ARITY_NONE,
     SAG_CMD_NEEDS_WIN | SAG_CMD_PROMPTS | SAG_CMD_INTERNAL,
     "Accept the command line", NULL},
    {"ed.cmdline.cancel", sag_cmdline_cmd_cancel, SAG_ARITY_NONE,
     SAG_CMD_NEEDS_WIN | SAG_CMD_PROMPTS | SAG_CMD_INTERNAL,
     "Cancel the command line or menu", NULL},
    {"ed.del.word_prev", sag_cmdline_cmd_delete_word_prev, SAG_ARITY_NONE,
     SAG_CMD_NEEDS_WIN | SAG_CMD_CHANGES_BUFFER | SAG_CMD_INTERNAL,
     "Delete to the previous word boundary", NULL},
    {"ed.del.to_home", sag_cmdline_cmd_delete_to_home, SAG_ARITY_NONE,
     SAG_CMD_NEEDS_WIN | SAG_CMD_CHANGES_BUFFER | SAG_CMD_INTERNAL,
     "Delete from the cursor to line start", NULL},
    {"ed.del.to_end", sag_cmdline_cmd_delete_to_end, SAG_ARITY_NONE,
     SAG_CMD_NEEDS_WIN | SAG_CMD_CHANGES_BUFFER | SAG_CMD_INTERNAL,
     "Delete from the cursor to line end", NULL},

    DEFER("ed.file.open", SAG_ARITY_STR, SAG_CMD_PROMPTS, 23,
          "open a file"),
    {"ed.file.write", sag_file_cmd_write, SAG_ARITY_OPT_STR,
     SAG_CMD_NEEDS_WIN, "Write the active buffer, optionally to a path", NULL},
    {"ed.file.write_quit", sag_file_cmd_write_quit, SAG_ARITY_OPT_STR,
     SAG_CMD_NEEDS_WIN, "Write the active buffer and quit", NULL},
    {"ed.file.save", sag_file_cmd_save_current, SAG_ARITY_NONE,
     SAG_CMD_NEEDS_WIN, "Atomically save the active file", NULL},
    {"ed.file.new", sag_file_cmd_new, SAG_ARITY_OPT_STR, 0U,
     "Create an empty buffer, optionally naming its file", NULL},
    {"ed.file.reload", sag_file_cmd_reload, SAG_ARITY_NONE,
     SAG_CMD_NEEDS_WIN, "Reload the active file from disk", NULL},
    DEFER("ed.file.close", SAG_ARITY_NONE, SAG_CMD_NEEDS_WIN, 23,
          "close the active file"),
    DEFER("ed.buf.next", SAG_ARITY_NONE, SAG_CMD_REPEATABLE, 23,
          "activate the next buffer"),
    DEFER("ed.buf.prev", SAG_ARITY_NONE, SAG_CMD_REPEATABLE, 23,
          "activate the previous buffer"),
    {"ed.tab.goto", sag_tab_cmd_goto, SAG_ARITY_OPT_INT,
     SAG_CMD_TAKES_COUNT, "Activate a numbered tab (0 = tab 10)", NULL},
    {"ed.tab.new", sag_tab_cmd_new, SAG_ARITY_NONE, 0U,
     "Open an untitled tab", NULL},
    {"ed.tab.open", sag_tab_cmd_open, SAG_ARITY_STR, 0U,
     "Open a path in a new tab", NULL},
    {"ed.tab.close", sag_tab_cmd_close, SAG_ARITY_NONE, 0U,
     "Close the active tab", NULL},
    {"ed.tab.next", sag_tab_cmd_next, SAG_ARITY_NONE,
     SAG_CMD_REPEATABLE, "Activate the next tab", NULL},
    {"ed.tab.prev", sag_tab_cmd_prev, SAG_ARITY_NONE,
     SAG_CMD_REPEATABLE, "Activate the previous tab", NULL},
    {"ed.tab.move", sag_tab_cmd_move, SAG_ARITY_OPT_INT,
     SAG_CMD_TAKES_COUNT, "Move the active tab to position N", NULL},
    /* Sprint 27 §5: the tab context menu's rows.  Commands, because
     * invariant 9 requires a keyboard path for every menu row. */
    {"ed.tab.close_others", sag_tab_cmd_close_others, SAG_ARITY_NONE, 0U,
     "Close every tab but the active one", NULL},
    {"ed.tab.copy_path", sag_tab_cmd_copy_path, SAG_ARITY_NONE, 0U,
     "Copy the active tab's canonical path to the clipboard", NULL},
    /* Sprint 24 §6: the continuous line.  next/prev walk EVERY open
     * file — members of the active group first, then the row-1 entry
     * beside it — so left/right never dead-ends inside a group. */
    {"ed.file.next", sag_file_cmd_next, SAG_ARITY_NONE,
     SAG_CMD_REPEATABLE, "Walk to the next open file", NULL},
    {"ed.file.prev", sag_file_cmd_prev, SAG_ARITY_NONE,
     SAG_CMD_REPEATABLE, "Walk to the previous open file", NULL},
    {"ed.group.enter", sag_group_cmd_enter, SAG_ARITY_NONE, 0U,
     "Enter a tab group, resuming where you left it", NULL},
    {"ed.group.leave", sag_group_cmd_leave, SAG_ARITY_NONE, 0U,
     "Leave the active tab group", NULL},
    {"ed.group.dissolve", sag_group_cmd_dissolve, SAG_ARITY_NONE, 0U,
     "Dissolve the active group; its tabs stay open", NULL},
    {"ed.group.remove_tab", sag_group_cmd_remove_tab, SAG_ARITY_NONE, 0U,
     "Remove the active tab from its group", NULL},
    /* Sprint 27 §5/§9. */
    {"ed.ui.context_menu", sag_ui_cmd_context_menu, SAG_ARITY_NONE, 0U,
     "Open the context menu for the focused tab or group", NULL},
    {"ed.mouse.enable", sag_mouse_cmd_enable, SAG_ARITY_NONE, 0U,
     "Turn mouse reporting on for this session", NULL},
    {"ed.mouse.disable", sag_mouse_cmd_disable, SAG_ARITY_NONE, 0U,
     "Turn mouse reporting off for this session", NULL},
    /* Sprint 27 §8: the keyboard twin of dropping a tab into a group. */
    {"ed.group.add_tab", sag_group_cmd_add_tab, SAG_ARITY_STR, 0U,
     "Add the active tab to the named group", NULL},
    {"ed.group.rename", sag_group_cmd_rename, SAG_ARITY_OPT_STR,
     SAG_CMD_PROMPTS, "Rename the active tab group", NULL},
    {"ed.group.new", sag_gp_cmd_new, SAG_ARITY_OPT_STR, SAG_CMD_PROMPTS,
     "Assemble a new tab group", NULL},
    {"ed.group.edit", sag_gp_cmd_edit, SAG_ARITY_NONE, SAG_CMD_PROMPTS,
     "Edit the active group's membership", NULL},
    DEFER("ed.group.from_dir", SAG_ARITY_STR, 0U, 53,
          "open a directory as a tab group (F-mode)"),
    DEFER("ed.group.next", SAG_ARITY_NONE, SAG_CMD_REPEATABLE, 24,
          "activate the next tab group"),
    DEFER("ed.group.prev", SAG_ARITY_NONE, SAG_CMD_REPEATABLE, 24,
          "activate the previous tab group"),
    /* Sprint 25 §9: workspace state. */
    {"ed.ws.save_state", sag_ws_cmd_save_state, SAG_ARITY_NONE, 0U,
     "Write this workspace's state now, without waiting for the debounce", NULL},
    {"ed.ws.restore_state", sag_ws_cmd_restore_state, SAG_ARITY_NONE, 0U,
     "Open what this workspace's saved state names, alongside what is open", NULL},
    {"ed.ws.info", sag_ws_cmd_info, SAG_ARITY_NONE, 0U,
     "Report the workspace key, state directory, path record and lock owner", NULL},
    {"ed.ws.forget", sag_ws_cmd_forget, SAG_ARITY_NONE, 0U,
     "Delete this workspace's state directory, after confirming", NULL},
    /*
     * v1 is FROZEN and there is no v2, so there is nothing to migrate
     * TO.  The name exists and hard-errors rather than being absent and
     * reading as "no such command" (invariant 3); the first sprint that
     * needs v2 builds the framework and takes this over.
     */
    DEFER("ed.ws.migrate", SAG_ARITY_NONE, 0U, 25,
          "migrate workspace state to a newer schema (no v2 exists)"),
    /*
     * Sprint 18.5 ranks command NAMES and declared abbreviations.  The
     * full palette -- a picker that also matches help text -- stays
     * Sprint 38, so the name exists and hard-errors rather than being
     * absent and reading as "no such command" (invariant 3).
     */
    /* Sprint 26 §6: the three instances. */
    {"ed.find.file", sag_find_cmd_file, SAG_ARITY_NONE, SAG_CMD_PROMPTS,
     "Find a file in the workspace by fuzzy name", NULL},
    {"ed.find.buffer", sag_find_cmd_buffer, SAG_ARITY_NONE, SAG_CMD_PROMPTS,
     "Switch to an open tab by fuzzy name", NULL},
    {"ed.undo.branches", sag_undo_cmd_branches, SAG_ARITY_NONE,
     SAG_CMD_NEEDS_WIN | SAG_CMD_PROMPTS,
     "Pick an undo state from the branch tree", NULL},
    /*
     * Sprint 26 §9 defers these two, and they must EXIST to say so:
     * absent, they read to the user as "no such command" rather than
     * "not yet" (invariant 3).  Both are PickerSpec values over §5's
     * widget when their sprint arrives — no new machinery.
     */
    DEFER("ed.find.symbol", SAG_ARITY_NONE, 0U, 47,
          "pick a symbol from the LSP workspace index"),
    DEFER("ed.find.command", SAG_ARITY_NONE, 0U, 38,
          "open the command palette"),
    {"ed.pane.split_h", sag_pane_cmd_split_h, SAG_ARITY_NONE,
     SAG_CMD_NEEDS_WIN, "Split the focused pane side by side", NULL},
    {"ed.pane.split_v", sag_pane_cmd_split_v, SAG_ARITY_NONE,
     SAG_CMD_NEEDS_WIN, "Split the focused pane top and bottom", NULL},
    {"ed.pane.close", sag_pane_cmd_close, SAG_ARITY_NONE,
     SAG_CMD_NEEDS_WIN, "Close the focused pane", NULL},
    {"ed.pane.focus_left", sag_pane_cmd_focus_left, SAG_ARITY_NONE,
     SAG_CMD_NEEDS_WIN, "Focus the pane to the left", NULL},
    {"ed.pane.focus_right", sag_pane_cmd_focus_right, SAG_ARITY_NONE,
     SAG_CMD_NEEDS_WIN, "Focus the pane to the right", NULL},
    {"ed.pane.focus_up", sag_pane_cmd_focus_up, SAG_ARITY_NONE,
     SAG_CMD_NEEDS_WIN, "Focus the pane above", NULL},
    {"ed.pane.focus_down", sag_pane_cmd_focus_down, SAG_ARITY_NONE,
     SAG_CMD_NEEDS_WIN, "Focus the pane below", NULL},
    {"ed.pane.focus_next", sag_pane_cmd_focus_next, SAG_ARITY_NONE,
     SAG_CMD_NEEDS_WIN, "Focus the next pane in tree order", NULL},
    {"ed.pane.grow", sag_pane_cmd_grow, SAG_ARITY_OPT_INT,
     SAG_CMD_TAKES_COUNT | SAG_CMD_NEEDS_WIN, "Grow the focused pane", NULL},
    {"ed.pane.shrink", sag_pane_cmd_shrink, SAG_ARITY_OPT_INT,
     SAG_CMD_TAKES_COUNT | SAG_CMD_NEEDS_WIN, "Shrink the focused pane", NULL},
    DEFER("ed.win.next", SAG_ARITY_NONE, SAG_CMD_REPEATABLE, 22,
          "focus the next window"),
    DEFER("ed.win.prev", SAG_ARITY_NONE, SAG_CMD_REPEATABLE, 22,
          "focus the previous window"),

    {"ed.search.open", sag_search_cmd_open, SAG_ARITY_OPT_STR,
     SAG_CMD_PROMPTS | SAG_CMD_NEEDS_WIN, "Open incremental search", NULL},
    {"ed.search.open_back", sag_search_cmd_open_back, SAG_ARITY_OPT_STR,
     SAG_CMD_PROMPTS | SAG_CMD_NEEDS_WIN,
     "Open incremental search, backwards", NULL},
    {"ed.search.next", sag_search_cmd_next, SAG_ARITY_OPT_INT,
     SAG_CMD_TAKES_COUNT | SAG_CMD_RECORDABLE | SAG_CMD_NEEDS_WIN,
     "Move to the next search match", "search_next"},
    {"ed.search.prev", sag_search_cmd_prev, SAG_ARITY_OPT_INT,
     SAG_CMD_TAKES_COUNT | SAG_CMD_RECORDABLE | SAG_CMD_NEEDS_WIN,
     "Move to the previous search match", "search_prev"},
    {"ed.search.word_next", sag_search_cmd_word_next, SAG_ARITY_NONE,
     SAG_CMD_RECORDABLE | SAG_CMD_NEEDS_WIN,
     "Search forward for the word under the cursor", "word_next"},
    {"ed.search.word_prev", sag_search_cmd_word_prev, SAG_ARITY_NONE,
     SAG_CMD_RECORDABLE | SAG_CMD_NEEDS_WIN,
     "Search backward for the word under the cursor", "word_prev"},
    {"ed.mark.set", sag_mark_cmd_set, SAG_ARITY_STR,
     SAG_CMD_CAPTURES_TEXT | SAG_CMD_NEEDS_WIN,
     "Set a named mark at the cursor", NULL},
    {"ed.mark.jump", sag_mark_cmd_jump, SAG_ARITY_STR,
     SAG_CMD_CAPTURES_TEXT | SAG_CMD_NEEDS_WIN,
     "Jump to a named mark", NULL},
    {"ed.search.clear_highlight", sag_search_cmd_clear_highlight,
     SAG_ARITY_NONE, SAG_CMD_NEEDS_WIN, "Clear match highlighting", NULL},
    {"ed.jump.back", sag_jump_cmd_back, SAG_ARITY_OPT_INT,
     SAG_CMD_TAKES_COUNT | SAG_CMD_NEEDS_WIN,
     "Jump to an older position in this window's history", NULL},
    {"ed.jump.fwd", sag_jump_cmd_fwd, SAG_ARITY_OPT_INT,
     SAG_CMD_TAKES_COUNT | SAG_CMD_NEEDS_WIN,
     "Jump to a newer position in this window's history", NULL},
    {"ed.jump.list", sag_jump_cmd_list, SAG_ARITY_NONE, SAG_CMD_NEEDS_WIN,
     "Show this window's jumplist", NULL},
    {"ed.change.older", sag_change_cmd_older, SAG_ARITY_OPT_INT,
     SAG_CMD_TAKES_COUNT | SAG_CMD_NEEDS_WIN,
     "Jump to an older change position in this buffer", NULL},
    {"ed.change.newer", sag_change_cmd_newer, SAG_ARITY_OPT_INT,
     SAG_CMD_TAKES_COUNT | SAG_CMD_NEEDS_WIN,
     "Jump to a newer change position in this buffer", NULL},
    {"ed.search.replace", sag_search_cmd_replace, SAG_ARITY_STR,
     SAG_CMD_CHANGES_BUFFER | SAG_CMD_NEEDS_WIN,
     "Substitute matches of a pattern in a line range", NULL},
    {"ed.search.global", sag_search_cmd_global, SAG_ARITY_STR, 0U,
     "Rejected: :g is Fletch's query API in Sprint 34", NULL},
    {"ed.macro.record", sag_record_cmd_record, SAG_ARITY_OPT_STR,
     SAG_CMD_PROMPTS, "Record a command macro", NULL},
    {"ed.macro.stop", sag_record_cmd_stop, SAG_ARITY_NONE, 0U,
     "Stop recording a command macro", NULL},
    {"ed.macro.replay", sag_record_cmd_replay, SAG_ARITY_STR,
     SAG_CMD_REPEATABLE | SAG_CMD_RECORDABLE | SAG_CMD_CAPTURES_TEXT,
     "Replay a command macro",
     "replay"},
    {"ed.macro.replay_last", sag_record_cmd_replay_last, SAG_ARITY_NONE,
     SAG_CMD_REPEATABLE | SAG_CMD_RECORDABLE,
     "Replay the last command macro", "replay_last"},
    {"ed.macro.list", sag_macro_cmd_list, SAG_ARITY_NONE, 0U,
     "List registers containing macros", NULL},
    {"ed.macro.edit", sag_macro_cmd_edit, SAG_ARITY_STR, 0U,
     "Edit a macro register as Fletch source", NULL},
    {"ed.macro.name", sag_macro_cmd_name, SAG_ARITY_STR, 0U,
     "Promote a macro register into the library", NULL},
    {"ed.macro.reload", sag_macro_cmd_reload, SAG_ARITY_NONE, 0U,
     "Reload the macro library", NULL},
    {"ed.shell.run", sag_shell_cmd_run, SAG_ARITY_STR,
     SAG_CMD_RECORDABLE, "Run a shell command, streaming its output", "shell_run"},
    {"ed.shell.run_bg", sag_shell_cmd_run_bg, SAG_ARITY_STR,
     SAG_CMD_RECORDABLE, "Run a shell command without stealing focus", "shell_bg"},
    {"ed.shell.read", sag_shell_cmd_read, SAG_ARITY_STR,
     SAG_CMD_RECORDABLE | SAG_CMD_NEEDS_WIN | SAG_CMD_CHANGES_BUFFER,
     "Insert a shell command's output at the cursor", "shell_read"},
    {"ed.shell.filter", sag_shell_cmd_filter, SAG_ARITY_STR,
     SAG_CMD_RECORDABLE | SAG_CMD_NEEDS_WIN | SAG_CMD_CHANGES_BUFFER,
     "Pipe a region through a shell command and replace it", "filter"},
    {"ed.shell.term", sag_shell_cmd_term, SAG_ARITY_NONE, 0U,
     "Interactive terminals are not a 1.0 feature", NULL},
    {"ed.job.list", sag_job_cmd_list, SAG_ARITY_NONE, 0U,
     "Open the job table", NULL},
    {"ed.job.kill", sag_job_cmd_kill, SAG_ARITY_OPT_INT,
     SAG_CMD_TAKES_COUNT, "Terminate a job's process group", NULL},
    {"ed.job.kill_force", sag_job_cmd_kill_force, SAG_ARITY_OPT_INT,
     SAG_CMD_TAKES_COUNT, "Kill a job's process group", NULL},
    {"ed.job.jump", sag_job_cmd_jump, SAG_ARITY_OPT_INT,
     SAG_CMD_TAKES_COUNT, "Focus a job's output buffer", NULL},
    {"ed.job.clear_finished", sag_job_cmd_clear_finished, SAG_ARITY_NONE,
     0U, "Drop every finished job and its output buffer", NULL},
    {"ed.job.rerun", sag_job_cmd_rerun, SAG_ARITY_OPT_INT,
     SAG_CMD_TAKES_COUNT, "Run a job's command line again", NULL},
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
    {"ed.tab.new", "", SAG_RP_FORBID, "tabnew"},
    {"ed.tab.open", "f", SAG_RP_FORBID, "tabedit"},
    {"ed.tab.close", "", SAG_RP_FORBID, "tabclose"},
    {"ed.group.new", "s", SAG_RP_FORBID, "gnew"},
    {"ed.group.edit", "", SAG_RP_FORBID, "gedit"},
    {"ed.group.dissolve", "", SAG_RP_FORBID, "gdissolve"},
    {"ed.group.rename", "s", SAG_RP_OPT, "grename"},
    {"ed.group.add_tab", "s", SAG_RP_FORBID, "gadd"},
    {"ed.tab.close_others", "", SAG_RP_FORBID, "tabonly"},
    {"ed.tab.copy_path", "", SAG_RP_FORBID, "copypath"},
    {"ed.group.remove_tab", "", SAG_RP_FORBID, "gremove"},
    {"ed.group.enter", "", SAG_RP_FORBID, "genter"},
    {"ed.group.leave", "", SAG_RP_FORBID, "gleave"},
    {"ed.ws.save_state", "", SAG_RP_FORBID, "wssave"},
    {"ed.ws.restore_state", "", SAG_RP_FORBID, "wsrestore"},
    {"ed.ws.info", "", SAG_RP_FORBID, "wsinfo"},
    {"ed.ws.forget", "", SAG_RP_FORBID, "wsforget"},
    {"ed.find.file", "", SAG_RP_FORBID, "find"},
    {"ed.find.buffer", "", SAG_RP_FORBID, "buffers"},
    {"ed.undo.branches", "", SAG_RP_FORBID, "undolist"},
    /* The substitution body is ONE opaque string; s18's tokenizer must
     * not try to understand `/` inside a regex. */
    {"ed.search.replace", "s", SAG_RP_OPT, "s"},
    {"ed.search.global", "s", SAG_RP_OPT, "g"},
    {"ed.fl.eval", "s", SAG_RP_FORBID, "fl"},
    {"ed.opt.set_many", "ov", SAG_RP_FORBID, "set"},
    {"ed.mark.set", "s", SAG_RP_FORBID, "mark"},
    /* :! carries an arbitrary command line, so its argspec is one string
     * and the range decides run-vs-filter (§5). */
    {"ed.shell.run", "s", SAG_RP_OPT, NULL},
    {"ed.shell.read", "s", SAG_RP_FORBID, NULL},
    {"ed.shell.filter", "s", SAG_RP_REQUIRED, NULL},
    {"ed.macro.list", "", SAG_RP_FORBID, "macros"},
    {"ed.macro.edit", "s", SAG_RP_FORBID, NULL},
    {"ed.macro.name", "ss", SAG_RP_FORBID, NULL},
    {"ed.macro.reload", "", SAG_RP_FORBID, NULL},
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
        "quit", "quit_force", "suspend", "redraw", "repeat", "nop",
        "map"};
    static const char *const domains[] = {
        "move", "edit", "mode", "sel", "cursor", "view", "ui",
        "file", "buf", "tab", "group", "pane", "win", "reg",
        "search", "macro", "job", "git", "lsp", "ai", "plug",
        "cmdline", "del", "shell", "opt", "fl", "config",
        /* Sprint 21 */
        "jump", "change", "mark",
        /* Sprint 18.5: the palette itself is Sprint 38's, but the name has
         * to exist now so it can hard-error naming it (invariant 3). */
        "find",
        /* Sprint 25 */
        "ws",
        /* Sprint 26: the undo branch picker closes s10 §11's deferral. */
        "undo",
        /* Sprint 27 §9: the runtime mouse toggle.  The option model that
         * PERSISTS it is Sprint 36. */
        "mouse"};
    static const char *const verbs[] = {
        "home", "end", "next", "prev", "up", "down", "left", "right",
        "goto", "insert", "delete", "replace", "change", "yank", "paste", "toggle",
        "open", "close", "save", "new", "enter", "leave", "grow",
        "shrink", "expand", "contract", "list", "reload", "cancel",
        "text", "undo", "redo", "escape", "add", "above", "below", "center",
        "message_expand", "split_h", "split_v", "record", "stop", "name",
        "replay", "replay_last", "stage", "map",
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
        /* Sprint 18.5 */
        "page_next", "page_prev", "dismiss", "command",
        /* Sprint 19 */
        "run", "run_bg", "read", "filter", "term", "kill", "kill_force",
        "jump", "clear_finished", "rerun",
        /* Sprint 21 */
        "back", "fwd", "older", "newer", "global",
        "open_back", "word_next", "clear_highlight", "set",
        /* Sprint 22 */
        "split_h", "split_v", "focus_left", "focus_right", "focus_up",
        "focus_down", "focus_next",
        /* Sprint 23 */
        "move",
        /* Sprint 24 */
        "dissolve", "remove_tab", "from_dir", "edit",
        /* Sprint 25 */
        "save_state", "restore_state", "info", "forget", "migrate",
        /* Sprint 26 */
        "file", "buffer", "branches", "symbol",
        /* Sprint 27 */
        "close_others", "copy_path", "rename", "context_menu", "add_tab",
        "enable", "disable", "at", "span", "unit", "up_alt", "down_alt",
        "get", "eval", "set_many", "split", "focus", "closure",
        "reload"};
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

/*
 * Sprint 34 §3: the CMDWORD rules, enforced where a command is BORN.
 *
 * Sprint 35's round-trip law says a recorded macro and a hand-typed
 * motion block are indistinguishable, which requires word -> command
 * to be a bijection over the recordable set.  A law checked in the
 * recorder is a law that a new command silently breaks; checked here,
 * adding a recordable command without a word does not build.
 */
static void word_validate(const CmdDesc *d)
{
    const char *w = d->word;
    size_t n;

    if ((d->flags & SAG_CMD_RECORDABLE) != 0U && w == NULL)
        SAG_BUG("recordable command %s has no CMDWORD", d->name);
    if (w == NULL)
        return;
    if ((d->flags & SAG_CMD_RECORDABLE) == 0U)
        SAG_BUG("command %s has a CMDWORD but is not recordable", d->name);
    n = strlen(w);
    if (n == 0U || n > 16U)
        SAG_BUG("command %s has a CMDWORD of bad length", d->name);
    if (w[0] < 'a' || w[0] > 'z')
        SAG_BUG("command %s CMDWORD must start with a letter", d->name);
    {
        size_t i;

        for (i = 1U; i < n; i++) {
            char c = w[i];

            if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                  c == '_'))
                SAG_BUG("command %s has an invalid CMDWORD", d->name);
        }
    }
}

static void desc_validate(const CmdDesc *d)
{
    const u32 known_flags = SAG_CMD_REPEATABLE | SAG_CMD_TAKES_COUNT |
                            SAG_CMD_RECORDABLE | SAG_CMD_NEEDS_WIN |
                            SAG_CMD_CHANGES_BUFFER | SAG_CMD_PROMPTS |
                            SAG_CMD_DEFERRED | SAG_CMD_MULTI_AGGREGATE |
                            SAG_CMD_CAPTURES_TEXT | SAG_CMD_INTERNAL |
                            SAG_CMD_INTERACTIVE;

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
    word_validate(d);
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
    if ((copy.cmd.flags & SAG_CMD_PROMPTS) != 0U)
        copy.cmd.flags |= SAG_CMD_INTERACTIVE;
    copy.cmd.name = sag_intern_str(&registry.names, id);
    copy.cmd.help = arena_strdup(&registry.arena, d->help);
    copy.argspec = arena_strdup(
        &registry.arena,
        entry->argspec == NULL ? default_argspec(d) : entry->argspec);
    copy.abbrev = entry->abbrev == NULL ? NULL :
                  arena_strdup(&registry.arena, entry->abbrev);
    registry.entries[registry.len++] = copy;
    if (copy.cmd.word != NULL) {
        size_t wlen = strlen(copy.cmd.word);

        if (strmap_has(&registry.words, copy.cmd.word, wlen))
            SAG_BUG("duplicate CMDWORD '%s' on %s", copy.cmd.word,
                    d->name);
        copy.cmd.word = arena_strdup(&registry.arena, copy.cmd.word);
        registry.entries[registry.len - 1U].cmd.word = copy.cmd.word;
        (void)strmap_put(&registry.words, copy.cmd.word, wlen,
                         (void *)(uintptr_t)id);
    }
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
    strmap_init(&registry.words);
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
    strmap_free(&registry.words);
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

CmdId sag_cmd_by_word(const char *word, u32 len)
{
    void *found;

    sag_cmd_init();
    if (word == NULL || len == 0U)
        return SAG_CMD_NONE;
    found = strmap_get(&registry.words, word, len);
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
        return cx->sarg == NULL && cx->sarg_len == 0U;
    case SAG_ARITY_INT:
    case SAG_ARITY_OPT_INT:
        return cx->sarg == NULL && cx->sarg_len == 0U;
    case SAG_ARITY_STR:
        return cx->sarg != NULL;
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
        (d->flags & SAG_CMD_RECORDABLE) != 0U) {
        CmdStatus recordable = sag_record_preflight(id, cx);

        if (recordable != SAG_CMD_OK)
            return recordable;
        registry.record_tap(id, cx);
    }
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
