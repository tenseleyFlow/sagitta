#include "edit/cmd.h"

#include <ctype.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "edit/bind.h"
#include "edit/buf.h"
#include "edit/edit_cmds.h"
#include "edit/jumplist.h"
#include "edit/lsp_cmds.h"
#include "edit/opt.h"
#include "edit/pane_cmds.h"
#include "edit/search_cmds.h"
#include "edit/shadow_cmds.h"
#include "edit/file_cmds.h"
#include "edit/flapi_cmds.h"
#include "edit/shell_cmds.h"
#include "edit/theme_cmds.h"
#include "edit/sel_actions.h"
#include "edit/ws_cmds.h"
#include "fl/flconf.h"
#include "fl/record.h"
#include "mod/ai/ai.h"
#include "mod/git/fussmode.h"
#include "mod/git/git.h"
#include "mod/git/editor.h"
#include "mod/lsp/lsp.h"
#include "ui/pickers.h"
#include "ui/cmdline.h"
#include "ui/complmenu.h"
#include "ui/groupnav.h"
#include "ui/mouse.h"
#include "ui/macrobrowse.h"
#include "ui/message.h"
#include "ui/win.h"
#include "ui/grouppicker.h"
#include "ui/groupfromdir.h"
#include "ui/tabs.h"
#include "syn/defs.h"
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
    return YEW_CMD_OK;
}

static CmdStatus cmd_syn_status(CmdCtx *cx)
{
    Buffer *b;
    char status[160];

    if (cx == NULL || cx->ed == NULL || cx->win == NULL ||
        cx->win->buf == NULL || cx->win->buf->tb == NULL)
        return YEW_CMD_ERR_STATE;
    b = cx->win->buf;
    yew_syn_status(&b->syn, yew_textbuf_line_count(b->tb), status,
                   sizeof(status));
    yew_msg(cx->ed, YEW_MSG_INFO, "%s", status);
    return YEW_CMD_OK;
}

static CmdStatus cmd_syn_set(CmdCtx *cx)
{
    Buffer *b;
    SynEngine *engine;
    const SynLangDesc *desc;
    const char *next_lang;
    u32 lang;

    if (cx == NULL || cx->win == NULL || cx->win->buf == NULL ||
        cx->win->buf->tb == NULL || cx->sarg == NULL)
        return YEW_CMD_ERR_STATE;
    b = cx->win->buf;
    if ((cx->sarg_len == 4U && memcmp(cx->sarg, "none", 4U) == 0) ||
        (cx->sarg_len == 4U && memcmp(cx->sarg, "text", 4U) == 0)) {
        if (b->lang != NULL)
            yew_lsp_buffer_close(cx->ed, b);
        yew_syn_attach(&b->syn, YEW_LANG_NONE, b->tb);
        b->lang = NULL;
        return YEW_CMD_OK;
    }
    {
        char name[96];

        if (cx->sarg_len == 0U || cx->sarg_len >= sizeof(name))
            return YEW_CMD_ERR_ARG;
        (void)memcpy(name, cx->sarg, cx->sarg_len);
        name[cx->sarg_len] = '\0';
        lang = yew_syn_lang_named(name);
        if (lang == YEW_LANG_NONE) {
            u32 sprint = strcmp(name, "c") == 0 || strcmp(name, "fletch") == 0 ||
                                 strcmp(name, "sh") == 0 ||
                                 strcmp(name, "make") == 0 ||
                                 strcmp(name, "markdown") == 0
                             ? 41U
                             : 42U;

            yew_msg(cx->ed, YEW_MSG_ERROR,
                    "no definition for '%s' — Sprint %u", name, sprint);
            return YEW_CMD_ERR_ARG;
        }
    }
    engine = yew_syn_engine_for(lang);
    if (engine == NULL)
        return YEW_CMD_ERR_IO;
    yew_syn_attach(&b->syn, lang, b->tb);
    yew_syn_buf_bind(&b->syn, engine);
    desc = yew_syn_lang_desc(lang);
    next_lang = desc == NULL ? NULL :
                strcmp(desc->name, "fortran-fixed") == 0 ?
                "fortran(fixed)" : desc->name;
    if ((b->lang == NULL) != (next_lang == NULL) ||
        (b->lang != NULL && strcmp(b->lang, next_lang) != 0)) {
        yew_lsp_buffer_close(cx->ed, b);
        b->lang = next_lang;
        if (next_lang != NULL)
            yew_lsp_buffer_open(cx->ed, b);
    } else {
        b->lang = next_lang;
    }
    return YEW_CMD_OK;
}

static CmdStatus deferred_unreachable(CmdCtx *cx)
{
    (void)cx;
    YEW_BUG("deferred command reached its implementation");
}

#define DEFER(name_, arity_, flags_, sprint_, help_)                           \
    {                                                                          \
        name_, deferred_unreachable, arity_, (flags_) | YEW_CMD_DEFERRED,      \
            "Sprint " #sprint_ ": " help_, NULL                              \
    }

/*
 * A deferred command that is nonetheless RECORDABLE, and so must carry
 * its CMDWORD now: the word is what a macro records, and s35 cannot
 * add it later without changing what earlier recordings mean.
 */
#define DEFER_W(name_, arity_, flags_, sprint_, help_, word_)                  \
    {                                                                          \
        name_, deferred_unreachable, arity_, (flags_) | YEW_CMD_DEFERRED,      \
            "Sprint " #sprint_ ": " help_, word_                             \
    }

#ifndef YEW_WITH_LSP
#define YEW_WITH_LSP 0
#endif

#if YEW_WITH_LSP
#define LSP_DEFER(name_, arity_, flags_, sprint_, help_)                       \
    DEFER(name_, arity_, flags_, sprint_, help_)
#else
#define LSP_DEFER(name_, arity_, flags_, sprint_, help_)                       \
    {                                                                          \
        name_, yew_lsp_cmd_require, arity_, flags_,                            \
            "LSP module unavailable: " help_, NULL                            \
    }
#endif

#ifndef YEW_WITH_AI
#define YEW_WITH_AI 0
#endif

#if YEW_WITH_AI
#define AI_DEFER(name_, sprint_, help_)                                       \
    DEFER(name_, YEW_ARITY_NONE, 0U, sprint_, help_)
#else
#define AI_DEFER(name_, sprint_, help_)                                       \
    {                                                                          \
        name_, yew_ai_cmd_require, YEW_ARITY_NONE, 0U,                        \
            "AI module unavailable: " help_, NULL                            \
    }
#endif

#ifndef YEW_WITH_FUSS
#define YEW_WITH_FUSS 0
#endif

static const CmdDesc builtins[] = {
    {"ed.edit.insert.at", yew_edit_cmd_insert_at, YEW_ARITY_STR,
     YEW_CMD_CHANGES_BUFFER | YEW_CMD_RECORDABLE | YEW_CMD_NEEDS_WIN,
     "Insert bytes at an offset", "insert_at"},
    {"ed.edit.delete.span", yew_edit_cmd_delete_span, YEW_ARITY_NONE,
     YEW_CMD_CHANGES_BUFFER | YEW_CMD_RECORDABLE | YEW_CMD_NEEDS_WIN,
     "Delete the supplied span", "delete_span"},
    {"ed.edit.replace.span", yew_edit_cmd_replace_span, YEW_ARITY_STR,
     YEW_CMD_CHANGES_BUFFER | YEW_CMD_RECORDABLE | YEW_CMD_NEEDS_WIN,
     "Replace the supplied span", "replace_span"},
    {"ed.edit.delete.unit", yew_edit_cmd_delete_unit, YEW_ARITY_NONE,
     YEW_CMD_REPEATABLE | YEW_CMD_CHANGES_BUFFER | YEW_CMD_RECORDABLE |
         YEW_CMD_NEEDS_WIN,
     "Delete the current unit", "delete_unit"},
    {"ed.move.unit.up", yew_edit_cmd_move_unit_up, YEW_ARITY_NONE,
     YEW_CMD_RECORDABLE | YEW_CMD_NEEDS_WIN, "Move unit up", "unit_up"},
    {"ed.move.unit.down", yew_edit_cmd_move_unit_down, YEW_ARITY_NONE,
     YEW_CMD_RECORDABLE | YEW_CMD_NEEDS_WIN, "Move unit down", "unit_down"},
    {"ed.move.unit.up_alt", yew_edit_cmd_move_unit_up_alt, YEW_ARITY_NONE,
     YEW_CMD_RECORDABLE | YEW_CMD_NEEDS_WIN, "Move unit up (alternate)",
     "unit_up_alt"},
    {"ed.move.unit.down_alt", yew_edit_cmd_move_unit_down_alt, YEW_ARITY_NONE,
     YEW_CMD_RECORDABLE | YEW_CMD_NEEDS_WIN, "Move unit down (alternate)",
     "unit_down_alt"},
    {"ed.buf.open", yew_file_cmd_buf_open, YEW_ARITY_STR, 0U,
     "Open a buffer", NULL},
    {"ed.buf.close", yew_file_cmd_buf_close, YEW_ARITY_NONE,
     YEW_CMD_NEEDS_WIN, "Close the current buffer", NULL},
    {"ed.cursor.set", yew_edit_cmd_cursor_set, YEW_ARITY_NONE,
     YEW_CMD_NEEDS_WIN, "Set the primary cursor offset", NULL},
    {"ed.cursor.set_many", yew_flapi_cmd_cursor_set_many, YEW_ARITY_NONE,
     YEW_CMD_NEEDS_WIN | YEW_CMD_MULTI_AGGREGATE,
     "Replace a window's cursor set", NULL},
    {"ed.cursor.move", yew_flapi_cmd_cursor_move, YEW_ARITY_STR,
     YEW_CMD_REPEATABLE | YEW_CMD_NEEDS_WIN,
     "Move one cursor by a named unit and direction", NULL},
    {"ed.win.split", yew_flapi_cmd_win_split, YEW_ARITY_STR,
     YEW_CMD_NEEDS_WIN, "Split a window horizontally or vertically", NULL},
    {"ed.win.focus", yew_flapi_cmd_win_focus, YEW_ARITY_NONE,
     YEW_CMD_NEEDS_WIN, "Focus a specific window", NULL},
    {"ed.edit.yank", yew_flapi_cmd_span_yank, YEW_ARITY_OPT_STR,
     YEW_CMD_NEEDS_WIN, "Yank a supplied span into a register", NULL},
    {"ed.reg.set", yew_flapi_cmd_reg_set, YEW_ARITY_STR,
     YEW_CMD_INTERNAL, "Set macro source in a named register", NULL},
    {"ed.opt.get", yew_opt_cmd_get, YEW_ARITY_STR, 0U,
     "Read an editor option", NULL},
    {"ed.opt.set", yew_opt_cmd_set, YEW_ARITY_STR, 0U,
     "Set an editor option", NULL},
    {"ed.opt.set_many", yew_opt_cmdline_set, YEW_ARITY_NONE, 0U,
     "Set an editor option from E mode", NULL},
    {"ed.fl.eval", yew_fl_cmd_eval, YEW_ARITY_STR, 0U,
     "Evaluate Fletch in the persistent editor runtime", NULL},
    {"ed.fl.closure", yew_bind_closure_cmd, YEW_ARITY_INT,
     YEW_CMD_INTERNAL, "Invoke a Fletch closure bound to a key", NULL},
    {"ed.config.reload", yew_config_cmd_reload, YEW_ARITY_NONE, 0U,
     "Reload the runtime, user, and workspace configuration", NULL},
    {"ed.config.edit", yew_config_cmd_edit, YEW_ARITY_NONE, 0U,
     "Open the user init.fl", NULL},
    {"ed.map", yew_bind_cmd_map, YEW_ARITY_NONE, 0U,
     "List configured bindings for the current mode", NULL},
    {"ed.nop", cmd_nop, YEW_ARITY_NONE, 0U, "Do nothing", NULL},
    {"ed.syn.status", cmd_syn_status, YEW_ARITY_NONE, YEW_CMD_NEEDS_WIN,
     "Report incremental syntax highlighting progress", NULL},
    {"ed.syn.set", cmd_syn_set, YEW_ARITY_STR, YEW_CMD_NEEDS_WIN,
     "Set the current buffer's syntax language", NULL},
    {"ed.theme.set", yew_theme_cmd_set, YEW_ARITY_STR, 0U,
     "Load and select a syntax theme", NULL},
    {"ed.theme.toggle", yew_theme_cmd_toggle, YEW_ARITY_NONE, 0U,
     "Toggle between the last dark and light themes", NULL},
    {"ed.shadow.accept_word", yew_shadow_cmd_accept_word, YEW_ARITY_NONE,
     YEW_CMD_RECORDABLE | YEW_CMD_NEEDS_WIN | YEW_CMD_CHANGES_BUFFER,
     "Accept the next suggested unit", "shadow_word"},
    {"ed.shadow.accept_word_alt", yew_shadow_cmd_accept_word_alt,
     YEW_ARITY_NONE,
     YEW_CMD_RECORDABLE | YEW_CMD_NEEDS_WIN | YEW_CMD_CHANGES_BUFFER,
     "Accept the next alternate suggested unit", "shadow_word_alt"},
    {"ed.shadow.accept_line", yew_shadow_cmd_accept_line, YEW_ARITY_NONE,
     YEW_CMD_RECORDABLE | YEW_CMD_NEEDS_WIN | YEW_CMD_CHANGES_BUFFER,
     "Accept the next suggested line", "shadow_line"},
    {"ed.shadow.accept_all", yew_shadow_cmd_accept_all, YEW_ARITY_NONE,
     YEW_CMD_RECORDABLE | YEW_CMD_NEEDS_WIN | YEW_CMD_CHANGES_BUFFER,
     "Accept the complete suggestion", "shadow_all"},
    {"ed.shadow.dismiss", yew_shadow_cmd_dismiss, YEW_ARITY_NONE,
     YEW_CMD_NEEDS_WIN, "Dismiss the current suggestion", NULL},
    {"ed.shadow.next", yew_shadow_cmd_next, YEW_ARITY_NONE,
     YEW_CMD_NEEDS_WIN, "Show the next provider suggestion", NULL},
    {"ed.shadow.prev", yew_shadow_cmd_prev, YEW_ARITY_NONE,
     YEW_CMD_NEEDS_WIN, "Show the previous provider suggestion", NULL},
    {"ed.shadow.toggle", yew_shadow_cmd_toggle, YEW_ARITY_NONE, 0U,
     "Toggle passive shadow suggestions", NULL},
    {"ed.shadow.stats", yew_shadow_cmd_stats, YEW_ARITY_NONE, 0U,
     "Report shadow provider and delivery statistics", NULL},
    {"ed.compl.open", yew_compl_cmd_open, YEW_ARITY_NONE,
     YEW_CMD_NEEDS_WIN | YEW_CMD_PROMPTS,
     "Open symbol completion for the current stem", NULL},
    {"ed.compl.next", yew_compl_cmd_next, YEW_ARITY_NONE,
     YEW_CMD_NEEDS_WIN, "Select the next completion", NULL},
    {"ed.compl.prev", yew_compl_cmd_prev, YEW_ARITY_NONE,
     YEW_CMD_NEEDS_WIN, "Select the previous completion", NULL},
    {"ed.compl.page_next", yew_compl_cmd_page_next, YEW_ARITY_NONE,
     YEW_CMD_NEEDS_WIN, "Page forward through completions", NULL},
    {"ed.compl.page_prev", yew_compl_cmd_page_prev, YEW_ARITY_NONE,
     YEW_CMD_NEEDS_WIN, "Page backward through completions", NULL},
    {"ed.compl.accept", yew_compl_cmd_accept, YEW_ARITY_NONE,
     YEW_CMD_NEEDS_WIN | YEW_CMD_CHANGES_BUFFER,
     "Accept the selected completion", NULL},
    {"ed.compl.cancel", yew_compl_cmd_cancel, YEW_ARITY_NONE,
     YEW_CMD_NEEDS_WIN, "Close completion without editing", NULL},
    {"ed.compl.doc_toggle", yew_compl_cmd_doc_toggle, YEW_ARITY_NONE,
     YEW_CMD_NEEDS_WIN, "Toggle completion documentation", NULL},
    {"ed.compl.stats", yew_compl_cmd_stats, YEW_ARITY_NONE, 0U,
     "Report symbol completion index statistics", NULL},
    {"ed.compl.reindex", yew_compl_cmd_reindex, YEW_ARITY_NONE, 0U,
     "Rebuild the workspace symbol index", NULL},
    {"ed.quit", yew_file_cmd_quit, YEW_ARITY_NONE, 0U,
     "Quit, prompting when the buffer is dirty", NULL},
    {"ed.quit_force", yew_file_cmd_quit_force, YEW_ARITY_NONE, 0U,
     "Quit without discarding the recovery journal", NULL},
    {"ed.suspend", yew_file_cmd_suspend, YEW_ARITY_NONE, 0U,
     "Suspend the editor and restore the terminal", NULL},
    {"ed.redraw", yew_file_cmd_redraw, YEW_ARITY_NONE, 0U,
     "Redraw the complete display", NULL},
    {"ed.repeat", yew_record_cmd_repeat, YEW_ARITY_NONE, 0U,
     "Unavailable until resolved command arguments are retained", NULL},

    {"ed.move.buf.home", yew_edit_cmd_move_buf_home, YEW_ARITY_NONE,
     YEW_CMD_REPEATABLE | YEW_CMD_RECORDABLE | YEW_CMD_NEEDS_WIN,
     "Move to the start of the buffer", "buf_home"},
    {"ed.move.buf.end", yew_edit_cmd_move_buf_end, YEW_ARITY_NONE,
     YEW_CMD_TAKES_COUNT | YEW_CMD_RECORDABLE | YEW_CMD_NEEDS_WIN,
     "Move to the end of the buffer or a counted line", "buf_end"},
    {"ed.move.line.home", yew_edit_cmd_move_line_home, YEW_ARITY_NONE,
     YEW_CMD_REPEATABLE | YEW_CMD_RECORDABLE | YEW_CMD_NEEDS_WIN,
     "Move to the start of the line", "line_home"},
    {"ed.move.line.end", yew_edit_cmd_move_line_end, YEW_ARITY_NONE,
     YEW_CMD_REPEATABLE | YEW_CMD_RECORDABLE | YEW_CMD_NEEDS_WIN,
     "Move to the end of the line", "line_end"},
    {"ed.move.line.up", yew_edit_cmd_move_line_up, YEW_ARITY_NONE,
     YEW_CMD_REPEATABLE | YEW_CMD_RECORDABLE | YEW_CMD_NEEDS_WIN,
     "Move one line up", "up"},
    {"ed.move.line.down", yew_edit_cmd_move_line_down, YEW_ARITY_NONE,
     YEW_CMD_REPEATABLE | YEW_CMD_RECORDABLE | YEW_CMD_NEEDS_WIN,
     "Move one line down", "down"},
    {"ed.move.line.first_nonblank", yew_edit_cmd_move_line_first_nonblank,
     YEW_ARITY_NONE,
     YEW_CMD_REPEATABLE | YEW_CMD_RECORDABLE | YEW_CMD_NEEDS_WIN,
     "Move to the first nonblank grapheme", "home_text"},
    {"ed.move.line.last_nonblank", yew_edit_cmd_move_line_last_nonblank,
     YEW_ARITY_NONE,
     YEW_CMD_REPEATABLE | YEW_CMD_RECORDABLE | YEW_CMD_NEEDS_WIN,
     "Move to the last nonblank grapheme", "end_text"},
    {"ed.move.line.half_page_up", yew_edit_cmd_move_line_half_page_up,
     YEW_ARITY_NONE,
     YEW_CMD_REPEATABLE | YEW_CMD_RECORDABLE | YEW_CMD_NEEDS_WIN,
     "Move half a viewport up", "half_up"},
    {"ed.move.line.half_page_down", yew_edit_cmd_move_line_half_page_down,
     YEW_ARITY_NONE,
     YEW_CMD_REPEATABLE | YEW_CMD_RECORDABLE | YEW_CMD_NEEDS_WIN,
     "Move half a viewport down", "half_down"},
    {"ed.move.unit.next", yew_edit_cmd_move_unit_next, YEW_ARITY_NONE,
     YEW_CMD_REPEATABLE | YEW_CMD_RECORDABLE | YEW_CMD_NEEDS_WIN,
     "Move to the next unit", "unit_next"},
    {"ed.move.unit.prev", yew_edit_cmd_move_unit_prev, YEW_ARITY_NONE,
     YEW_CMD_REPEATABLE | YEW_CMD_RECORDABLE | YEW_CMD_NEEDS_WIN,
     "Move to the previous unit", "unit_prev"},
    {"ed.move.unit.home", yew_edit_cmd_move_unit_home, YEW_ARITY_NONE,
     YEW_CMD_REPEATABLE | YEW_CMD_RECORDABLE | YEW_CMD_NEEDS_WIN,
     "Move to the start of the current unit", "unit_home"},
    {"ed.move.unit.end", yew_edit_cmd_move_unit_end, YEW_ARITY_NONE,
     YEW_CMD_REPEATABLE | YEW_CMD_RECORDABLE | YEW_CMD_NEEDS_WIN,
     "Move to the end of the current unit", "unit_end"},
    {"ed.move.unit.next_alt", yew_edit_cmd_move_unit_next_alt,
     YEW_ARITY_NONE,
     YEW_CMD_REPEATABLE | YEW_CMD_RECORDABLE | YEW_CMD_NEEDS_WIN,
     "Move to the next alternate unit", "unit_next_alt"},
    {"ed.move.unit.prev_alt", yew_edit_cmd_move_unit_prev_alt,
     YEW_ARITY_NONE,
     YEW_CMD_REPEATABLE | YEW_CMD_RECORDABLE | YEW_CMD_NEEDS_WIN,
     "Move to the previous alternate unit", "unit_prev_alt"},
    {"ed.move.unit.home_alt", yew_edit_cmd_move_unit_home_alt,
     YEW_ARITY_NONE,
     YEW_CMD_REPEATABLE | YEW_CMD_RECORDABLE | YEW_CMD_NEEDS_WIN,
     "Move to the alternate unit start", "unit_home_alt"},
    {"ed.move.unit.end_alt", yew_edit_cmd_move_unit_end_alt,
     YEW_ARITY_NONE,
     YEW_CMD_REPEATABLE | YEW_CMD_RECORDABLE | YEW_CMD_NEEDS_WIN,
     "Move to the alternate unit end", "unit_end_alt"},
    {"ed.move.block.match_prev", yew_edit_cmd_move_block_match_prev,
     YEW_ARITY_NONE,
     YEW_CMD_REPEATABLE | YEW_CMD_RECORDABLE | YEW_CMD_NEEDS_WIN,
     "Move to the enclosing opening delimiter", "match_prev"},
    {"ed.move.block.match_next", yew_edit_cmd_move_block_match_next,
     YEW_ARITY_NONE,
     YEW_CMD_REPEATABLE | YEW_CMD_RECORDABLE | YEW_CMD_NEEDS_WIN,
     "Move to the enclosing closing delimiter", "match_next"},
    {"ed.move.word.sub_prev", yew_edit_cmd_move_word_sub_prev,
     YEW_ARITY_NONE,
     YEW_CMD_REPEATABLE | YEW_CMD_RECORDABLE | YEW_CMD_NEEDS_WIN,
     "Move to the previous subword", "subword_prev"},
    {"ed.move.word.sub_next", yew_edit_cmd_move_word_sub_next,
     YEW_ARITY_NONE,
     YEW_CMD_REPEATABLE | YEW_CMD_RECORDABLE | YEW_CMD_NEEDS_WIN,
     "Move to the next subword", "subword_next"},
    {"ed.move.char.prev", yew_edit_cmd_move_char_prev, YEW_ARITY_NONE,
     YEW_CMD_REPEATABLE | YEW_CMD_RECORDABLE | YEW_CMD_NEEDS_WIN,
     "Move one grapheme left", "char_prev"},
    {"ed.move.char.next", yew_edit_cmd_move_char_next, YEW_ARITY_NONE,
     YEW_CMD_REPEATABLE | YEW_CMD_RECORDABLE | YEW_CMD_NEEDS_WIN,
     "Move one grapheme right", "char_next"},
    {"ed.move.char.left", yew_edit_cmd_move_char_prev, YEW_ARITY_NONE,
     YEW_CMD_REPEATABLE | YEW_CMD_RECORDABLE | YEW_CMD_NEEDS_WIN,
     "Alias for moving one grapheme left", "char_left"},
    {"ed.move.char.right", yew_edit_cmd_move_char_next, YEW_ARITY_NONE,
     YEW_CMD_REPEATABLE | YEW_CMD_RECORDABLE | YEW_CMD_NEEDS_WIN,
     "Alias for moving one grapheme right", "char_right"},

    {"ed.edit.insert.text", yew_edit_cmd_insert_text, YEW_ARITY_STR,
     YEW_CMD_RECORDABLE | YEW_CMD_NEEDS_WIN | YEW_CMD_CHANGES_BUFFER,
     "Insert UTF-8 text at the cursor", "insert"},
    {"ed.edit.insert.newline", yew_edit_cmd_insert_newline, YEW_ARITY_NONE,
     YEW_CMD_RECORDABLE | YEW_CMD_NEEDS_WIN | YEW_CMD_CHANGES_BUFFER,
     "Insert the buffer's native line ending", "newline"},
    {"ed.edit.insert.tab", yew_edit_cmd_insert_tab, YEW_ARITY_NONE,
     YEW_CMD_RECORDABLE | YEW_CMD_NEEDS_WIN | YEW_CMD_CHANGES_BUFFER,
     "Insert a literal tab", "tab"},
    {"ed.edit.insert.after", yew_edit_cmd_insert_after, YEW_ARITY_NONE,
     YEW_CMD_RECORDABLE | YEW_CMD_NEEDS_WIN,
     "Enter insert mode after the current grapheme", "append"},
    {"ed.edit.line.open_below", yew_edit_cmd_open_below, YEW_ARITY_NONE,
     YEW_CMD_RECORDABLE | YEW_CMD_NEEDS_WIN | YEW_CMD_CHANGES_BUFFER,
     "Open a new line below and enter insert mode", "open_below"},
    {"ed.edit.line.open_above", yew_edit_cmd_open_above, YEW_ARITY_NONE,
     YEW_CMD_RECORDABLE | YEW_CMD_NEEDS_WIN | YEW_CMD_CHANGES_BUFFER,
     "Open a new line above and enter insert mode", "open_above"},
    {"ed.edit.delete.grapheme_left", yew_edit_cmd_delete_grapheme_left,
     YEW_ARITY_NONE,
     YEW_CMD_REPEATABLE | YEW_CMD_RECORDABLE | YEW_CMD_NEEDS_WIN |
         YEW_CMD_CHANGES_BUFFER,
     "Delete the grapheme left of the cursor", "backspace"},
    {"ed.edit.delete.grapheme", yew_edit_cmd_delete_grapheme,
     YEW_ARITY_NONE,
     YEW_CMD_REPEATABLE | YEW_CMD_RECORDABLE | YEW_CMD_NEEDS_WIN |
         YEW_CMD_CHANGES_BUFFER,
     "Delete the grapheme at the cursor", "del_char"},
    {"ed.edit.line.delete", yew_edit_cmd_delete_line, YEW_ARITY_NONE,
     YEW_CMD_REPEATABLE | YEW_CMD_RECORDABLE | YEW_CMD_NEEDS_WIN |
         YEW_CMD_CHANGES_BUFFER,
     "Delete the current logical line", "del_line"},
    {"ed.edit.delete.prev", yew_edit_cmd_delete_grapheme_left,
     YEW_ARITY_NONE,
     YEW_CMD_REPEATABLE | YEW_CMD_RECORDABLE | YEW_CMD_NEEDS_WIN |
         YEW_CMD_CHANGES_BUFFER,
     "Alias for deleting the previous grapheme", "del_prev"},
    {"ed.edit.delete.next", yew_edit_cmd_delete_grapheme, YEW_ARITY_NONE,
     YEW_CMD_REPEATABLE | YEW_CMD_RECORDABLE | YEW_CMD_NEEDS_WIN |
         YEW_CMD_CHANGES_BUFFER,
     "Alias for deleting the next grapheme", "del_next"},
    {"ed.edit.undo", yew_edit_cmd_undo, YEW_ARITY_NONE,
     YEW_CMD_REPEATABLE | YEW_CMD_RECORDABLE | YEW_CMD_NEEDS_WIN,
     "Undo the last edit transaction", "undo"},
    {"ed.edit.redo", yew_edit_cmd_redo, YEW_ARITY_NONE,
     YEW_CMD_REPEATABLE | YEW_CMD_RECORDABLE | YEW_CMD_NEEDS_WIN,
     "Redo the last undone transaction", "redo"},
    {"ed.edit.undo_barrier", yew_edit_cmd_undo_barrier, YEW_ARITY_NONE,
     YEW_CMD_NEEDS_WIN, "Close the active insert transaction", NULL},
    {"ed.mode.enter", yew_edit_cmd_mode_enter, YEW_ARITY_STR,
     YEW_CMD_RECORDABLE | YEW_CMD_INTERNAL,
     "Enter L/W/B/I/H/E; F Sprint 52", "mode"},
    {"ed.mode.escape", yew_edit_cmd_mode_escape, YEW_ARITY_NONE,
     YEW_CMD_RECORDABLE | YEW_CMD_INTERNAL, "Return to line mode", "escape"},
    {"ed.sel.expand", yew_edit_cmd_sel_unit_expand, YEW_ARITY_NONE,
     YEW_CMD_REPEATABLE | YEW_CMD_RECORDABLE | YEW_CMD_NEEDS_WIN,
     "Expand the selection to the next structural unit", "sel_expand"},
    {"ed.sel.contract", yew_edit_cmd_sel_unit_contract, YEW_ARITY_NONE,
     YEW_CMD_REPEATABLE | YEW_CMD_RECORDABLE | YEW_CMD_NEEDS_WIN,
     "Contract the selection to the previous structural unit", "sel_contract"},
    {"ed.sel.unit.expand", yew_edit_cmd_sel_unit_expand, YEW_ARITY_NONE,
     YEW_CMD_REPEATABLE | YEW_CMD_RECORDABLE | YEW_CMD_NEEDS_WIN,
     "Expand to the next structural unit", "unit_expand"},
    {"ed.sel.unit.contract", yew_edit_cmd_sel_unit_contract,
     YEW_ARITY_NONE,
     YEW_CMD_REPEATABLE | YEW_CMD_RECORDABLE | YEW_CMD_NEEDS_WIN,
     "Contract to the previous structural unit", "unit_contract"},
    {"ed.sel.kind", yew_edit_cmd_sel_kind, YEW_ARITY_STR,
     YEW_CMD_RECORDABLE | YEW_CMD_NEEDS_WIN,
     "Choose character, line, or rectangular selection geometry", "sel_kind"},
    {"ed.sel.swap_ends", yew_edit_cmd_sel_swap_ends, YEW_ARITY_NONE,
     YEW_CMD_RECORDABLE | YEW_CMD_NEEDS_WIN,
     "Exchange the active and anchored ends of each selection", "swap_ends"},
    {"ed.sel.yank", yew_sel_cmd_yank, YEW_ARITY_NONE,
     YEW_CMD_RECORDABLE | YEW_CMD_NEEDS_WIN | YEW_CMD_MULTI_AGGREGATE,
     "Yank the active selections", "yank"},
    {"ed.sel.delete", yew_sel_cmd_delete, YEW_ARITY_NONE,
     YEW_CMD_RECORDABLE | YEW_CMD_NEEDS_WIN | YEW_CMD_CHANGES_BUFFER |
         YEW_CMD_MULTI_AGGREGATE,
     "Delete the active selections", "sel_delete"},
    {"ed.sel.change", yew_sel_cmd_change, YEW_ARITY_NONE,
     YEW_CMD_RECORDABLE | YEW_CMD_NEEDS_WIN | YEW_CMD_CHANGES_BUFFER |
         YEW_CMD_MULTI_AGGREGATE,
     "Change the active selections", "change"},
    {"ed.sel.case_upper", yew_sel_cmd_case_upper, YEW_ARITY_NONE,
     YEW_CMD_RECORDABLE | YEW_CMD_NEEDS_WIN | YEW_CMD_CHANGES_BUFFER |
         YEW_CMD_MULTI_AGGREGATE,
     "Uppercase the active selections", "upper"},
    {"ed.sel.case_lower", yew_sel_cmd_case_lower, YEW_ARITY_NONE,
     YEW_CMD_RECORDABLE | YEW_CMD_NEEDS_WIN | YEW_CMD_CHANGES_BUFFER |
         YEW_CMD_MULTI_AGGREGATE,
     "Lowercase the active selections", "lower"},
    {"ed.sel.case_toggle", yew_sel_cmd_case_toggle, YEW_ARITY_NONE,
     YEW_CMD_RECORDABLE | YEW_CMD_NEEDS_WIN | YEW_CMD_CHANGES_BUFFER |
         YEW_CMD_MULTI_AGGREGATE,
     "Toggle case in the active selections", "case_toggle"},
    {"ed.sel.indent", yew_sel_cmd_indent, YEW_ARITY_NONE,
     YEW_CMD_RECORDABLE | YEW_CMD_NEEDS_WIN | YEW_CMD_CHANGES_BUFFER |
         YEW_CMD_MULTI_AGGREGATE,
     "Indent lines covered by the selection", "indent"},
    {"ed.sel.dedent", yew_sel_cmd_dedent, YEW_ARITY_NONE,
     YEW_CMD_RECORDABLE | YEW_CMD_NEEDS_WIN | YEW_CMD_CHANGES_BUFFER |
         YEW_CMD_MULTI_AGGREGATE,
     "Dedent lines covered by the selection", "dedent"},
    {"ed.sel.shift_left", yew_sel_cmd_shift_left, YEW_ARITY_NONE,
     YEW_CMD_RECORDABLE | YEW_CMD_NEEDS_WIN | YEW_CMD_CHANGES_BUFFER |
         YEW_CMD_MULTI_AGGREGATE,
     "Shift selected text left", "shift_left"},
    {"ed.sel.shift_right", yew_sel_cmd_shift_right, YEW_ARITY_NONE,
     YEW_CMD_RECORDABLE | YEW_CMD_NEEDS_WIN | YEW_CMD_CHANGES_BUFFER |
         YEW_CMD_MULTI_AGGREGATE,
     "Shift selected text right", "shift_right"},
    {"ed.sel.join", yew_sel_cmd_join, YEW_ARITY_NONE,
     YEW_CMD_RECORDABLE | YEW_CMD_NEEDS_WIN | YEW_CMD_CHANGES_BUFFER |
         YEW_CMD_MULTI_AGGREGATE,
     "Join lines covered by the selection", "join"},
    {"ed.sel.replace_char", yew_sel_cmd_replace_char, YEW_ARITY_STR,
     YEW_CMD_RECORDABLE | YEW_CMD_NEEDS_WIN | YEW_CMD_CHANGES_BUFFER |
         YEW_CMD_MULTI_AGGREGATE | YEW_CMD_CAPTURES_TEXT,
     "Replace selected graphemes with one grapheme", "replace_char"},
    {"ed.edit.rect.insert", yew_sel_cmd_rect_insert, YEW_ARITY_NONE,
     YEW_CMD_RECORDABLE | YEW_CMD_NEEDS_WIN | YEW_CMD_CHANGES_BUFFER |
         YEW_CMD_MULTI_AGGREGATE,
     "Insert at the left edge of a rectangular selection", "rect_insert"},
    {"ed.edit.rect.append", yew_sel_cmd_rect_append, YEW_ARITY_NONE,
     YEW_CMD_RECORDABLE | YEW_CMD_NEEDS_WIN | YEW_CMD_CHANGES_BUFFER |
         YEW_CMD_MULTI_AGGREGATE,
     "Insert at the right edge of a rectangular selection", "rect_append"},
    {"ed.cursor.lift.lines", yew_edit_cmd_cursor_lift_lines, YEW_ARITY_NONE,
     YEW_CMD_RECORDABLE | YEW_CMD_NEEDS_WIN,
     "Lift a selection to one cursor per line", "lift_lines"},
    {"ed.cursor.lift.matches", yew_edit_cmd_cursor_lift_matches,
     YEW_ARITY_OPT_STR, YEW_CMD_RECORDABLE | YEW_CMD_NEEDS_WIN,
     "Lift literal matches in the selection to cursors", "lift_matches"},
    {"ed.cursor.lift.ends", yew_edit_cmd_cursor_lift_ends, YEW_ARITY_NONE,
     YEW_CMD_RECORDABLE | YEW_CMD_NEEDS_WIN,
     "Lift both ends of each selection to cursors", "lift_ends"},
    {"ed.cursor.add.above", yew_edit_cmd_cursor_add_above, YEW_ARITY_NONE,
     YEW_CMD_RECORDABLE | YEW_CMD_NEEDS_WIN,
     "Add a cursor on the preceding line", "cursor_above"},
    {"ed.cursor.add.below", yew_edit_cmd_cursor_add_below, YEW_ARITY_NONE,
     YEW_CMD_RECORDABLE | YEW_CMD_NEEDS_WIN,
     "Add a cursor on the following line", "cursor_below"},
    {"ed.cursor.drop", yew_edit_cmd_cursor_drop, YEW_ARITY_NONE,
     YEW_CMD_RECORDABLE | YEW_CMD_NEEDS_WIN,
     "Drop the most recently added cursor", "cursor_drop"},
    {"ed.cursor.collapse", yew_edit_cmd_cursor_collapse, YEW_ARITY_NONE,
     YEW_CMD_RECORDABLE | YEW_CMD_NEEDS_WIN,
     "Keep only the primary cursor", "collapse"},
    {"ed.view.center", yew_edit_cmd_view_center, YEW_ARITY_NONE,
     YEW_CMD_RECORDABLE | YEW_CMD_NEEDS_WIN, "Center the cursor line", "center"},
    {"ed.view.top", yew_edit_cmd_view_top, YEW_ARITY_NONE,
     YEW_CMD_RECORDABLE | YEW_CMD_NEEDS_WIN,
     "Place the cursor line at the top", "view_top"},
    {"ed.view.bottom", yew_edit_cmd_view_bottom, YEW_ARITY_NONE,
     YEW_CMD_RECORDABLE | YEW_CMD_NEEDS_WIN,
     "Place the cursor line at the bottom", "view_bottom"},
    {"ed.view.scroll.up", yew_edit_cmd_view_scroll_up, YEW_ARITY_NONE,
     YEW_CMD_REPEATABLE | YEW_CMD_RECORDABLE | YEW_CMD_NEEDS_WIN,
     "Scroll the active view up one display row", "scroll_up"},
    {"ed.view.scroll.down", yew_edit_cmd_view_scroll_down, YEW_ARITY_NONE,
     YEW_CMD_REPEATABLE | YEW_CMD_RECORDABLE | YEW_CMD_NEEDS_WIN,
     "Scroll the active view down one display row", "scroll_down"},
    {"ed.view.up", yew_edit_cmd_view_scroll_up, YEW_ARITY_NONE,
     YEW_CMD_REPEATABLE | YEW_CMD_RECORDABLE | YEW_CMD_NEEDS_WIN,
     "Alias for scrolling the active view up", "view_up"},
    {"ed.view.down", yew_edit_cmd_view_scroll_down, YEW_ARITY_NONE,
     YEW_CMD_REPEATABLE | YEW_CMD_RECORDABLE | YEW_CMD_NEEDS_WIN,
     "Alias for scrolling the active view down", "view_down"},
    {"ed.view.page_up", yew_edit_cmd_view_page_up, YEW_ARITY_NONE,
     YEW_CMD_REPEATABLE | YEW_CMD_RECORDABLE | YEW_CMD_NEEDS_WIN,
     "Move one viewport up", "page_up"},
    {"ed.view.page_down", yew_edit_cmd_view_page_down, YEW_ARITY_NONE,
     YEW_CMD_REPEATABLE | YEW_CMD_RECORDABLE | YEW_CMD_NEEDS_WIN,
     "Move one viewport down", "page_down"},
    {"ed.view.half_page_up", yew_edit_cmd_view_half_page_up,
     YEW_ARITY_NONE,
     YEW_CMD_REPEATABLE | YEW_CMD_RECORDABLE | YEW_CMD_NEEDS_WIN,
     "Scroll half a viewport up", "view_half_up"},
    {"ed.view.half_page_down", yew_edit_cmd_view_half_page_down,
     YEW_ARITY_NONE,
     YEW_CMD_REPEATABLE | YEW_CMD_RECORDABLE | YEW_CMD_NEEDS_WIN,
     "Scroll half a viewport down", "view_half_down"},
    {"ed.view.goto_line", yew_edit_cmd_view_goto_line, YEW_ARITY_NONE,
     YEW_CMD_TAKES_COUNT | YEW_CMD_RECORDABLE | YEW_CMD_NEEDS_WIN,
     "Go to a counted line and center it", "goto_line"},
    {"ed.view.toggle_wrap", yew_edit_cmd_view_toggle_wrap, YEW_ARITY_NONE,
     YEW_CMD_RECORDABLE | YEW_CMD_NEEDS_WIN, "Toggle line wrapping", "toggle_wrap"},
    {"ed.view.number_style", yew_edit_cmd_view_number_style, YEW_ARITY_STR,
     YEW_CMD_RECORDABLE | YEW_CMD_NEEDS_WIN,
     "Set line numbers to none, abs, rel, or hybrid", "number_style"},
    {"ed.ui.message_expand", yew_edit_cmd_message_expand, YEW_ARITY_NONE,
     YEW_CMD_PROMPTS, "Expand the current message", NULL},
    {"ed.ui.cancel", yew_edit_cmd_ui_cancel, YEW_ARITY_NONE, 0U,
     "Cancel the active prompt or message overlay", NULL},
    {"ed.ui.panel.move", yew_panel_cmd_move, YEW_ARITY_INT,
     YEW_CMD_NEEDS_WIN | YEW_CMD_INTERNAL,
     "Scroll the active transient panel", NULL},

    {"ed.cmdline.hist_prev", yew_cmdline_cmd_hist_prev, YEW_ARITY_NONE,
     YEW_CMD_NEEDS_WIN | YEW_CMD_INTERNAL,
     "Find the previous matching command-line history entry", NULL},
    {"ed.cmdline.hist_next", yew_cmdline_cmd_hist_next, YEW_ARITY_NONE,
     YEW_CMD_NEEDS_WIN | YEW_CMD_INTERNAL,
     "Find the next matching command-line history entry", NULL},
    {"ed.cmdline.complete_next", yew_cmdline_cmd_complete_next,
     YEW_ARITY_NONE, YEW_CMD_NEEDS_WIN | YEW_CMD_INTERNAL,
     "Open or advance command-line completion", NULL},
    {"ed.cmdline.complete_prev", yew_cmdline_cmd_complete_prev,
     YEW_ARITY_NONE, YEW_CMD_NEEDS_WIN | YEW_CMD_INTERNAL,
     "Open or reverse command-line completion", NULL},
    {"ed.cmdline.insert_register", yew_cmdline_cmd_insert_register,
     YEW_ARITY_STR,
     YEW_CMD_NEEDS_WIN | YEW_CMD_CAPTURES_TEXT | YEW_CMD_INTERNAL,
     "Insert one named register into the command line", NULL},
    {"ed.cmdline.literal_next", yew_cmdline_cmd_literal_next,
     YEW_ARITY_STR,
     YEW_CMD_NEEDS_WIN | YEW_CMD_CAPTURES_TEXT | YEW_CMD_INTERNAL,
     "Insert the next text-producing key literally", NULL},
    {"ed.cmdline.ghost.accept", yew_cmdline_cmd_ghost_accept,
     YEW_ARITY_NONE, YEW_CMD_NEEDS_WIN | YEW_CMD_INTERNAL,
     "Accept the inline suggestion, or move one grapheme right", NULL},
    /* Sprint 18.5 §10.  complete_next/prev stay as the names the keymap
     * and the goldens already use; these are the same behaviours under
     * the menu's own namespace, plus the two the old menu could not do. */
    {"ed.cmdline.menu.next", yew_cmdline_cmd_complete_next, YEW_ARITY_NONE,
     YEW_CMD_NEEDS_WIN | YEW_CMD_INTERNAL, "Select the next menu row", NULL},
    {"ed.cmdline.menu.prev", yew_cmdline_cmd_complete_prev, YEW_ARITY_NONE,
     YEW_CMD_NEEDS_WIN | YEW_CMD_INTERNAL,
     "Select the previous menu row", NULL},
    {"ed.cmdline.menu.page_next", yew_cmdline_cmd_menu_page_next,
     YEW_ARITY_NONE, YEW_CMD_NEEDS_WIN | YEW_CMD_INTERNAL,
     "Move one visible page down the menu", NULL},
    {"ed.cmdline.menu.page_prev", yew_cmdline_cmd_menu_page_prev,
     YEW_ARITY_NONE, YEW_CMD_NEEDS_WIN | YEW_CMD_INTERNAL,
     "Move one visible page up the menu", NULL},
    {"ed.cmdline.menu.accept", yew_cmdline_cmd_menu_accept, YEW_ARITY_NONE,
     YEW_CMD_NEEDS_WIN | YEW_CMD_INTERNAL,
     "Accept the selected menu row", NULL},
    {"ed.cmdline.menu.dismiss", yew_cmdline_cmd_menu_dismiss,
     YEW_ARITY_NONE, YEW_CMD_NEEDS_WIN | YEW_CMD_INTERNAL,
     "Close the menu without accepting", NULL},
    {"ed.cmdline.accept", yew_cmdline_cmd_accept, YEW_ARITY_NONE,
     YEW_CMD_NEEDS_WIN | YEW_CMD_PROMPTS | YEW_CMD_INTERNAL,
     "Accept the command line", NULL},
    {"ed.cmdline.cancel", yew_cmdline_cmd_cancel, YEW_ARITY_NONE,
     YEW_CMD_NEEDS_WIN | YEW_CMD_PROMPTS | YEW_CMD_INTERNAL,
     "Cancel the command line or menu", NULL},
    {"ed.del.word_prev", yew_cmdline_cmd_delete_word_prev, YEW_ARITY_NONE,
     YEW_CMD_NEEDS_WIN | YEW_CMD_CHANGES_BUFFER | YEW_CMD_INTERNAL,
     "Delete to the previous word boundary", NULL},
    {"ed.del.to_home", yew_cmdline_cmd_delete_to_home, YEW_ARITY_NONE,
     YEW_CMD_NEEDS_WIN | YEW_CMD_CHANGES_BUFFER | YEW_CMD_INTERNAL,
     "Delete from the cursor to line start", NULL},
    {"ed.del.to_end", yew_cmdline_cmd_delete_to_end, YEW_ARITY_NONE,
     YEW_CMD_NEEDS_WIN | YEW_CMD_CHANGES_BUFFER | YEW_CMD_INTERNAL,
     "Delete from the cursor to line end", NULL},

    DEFER("ed.file.open", YEW_ARITY_STR, YEW_CMD_PROMPTS, 23,
          "open a file"),
    {"ed.file.write", yew_file_cmd_write, YEW_ARITY_OPT_STR,
     YEW_CMD_NEEDS_WIN, "Write the active buffer, optionally to a path", NULL},
    {"ed.file.write_quit", yew_file_cmd_write_quit, YEW_ARITY_OPT_STR,
     YEW_CMD_NEEDS_WIN, "Write the active buffer and quit", NULL},
    {"ed.file.save", yew_file_cmd_save_current, YEW_ARITY_NONE,
     YEW_CMD_NEEDS_WIN, "Atomically save the active file", NULL},
    {"ed.file.new", yew_file_cmd_new, YEW_ARITY_OPT_STR, 0U,
     "Create an empty buffer, optionally naming its file", NULL},
    {"ed.file.reload", yew_file_cmd_reload, YEW_ARITY_NONE,
     YEW_CMD_NEEDS_WIN, "Reload the active file from disk", NULL},
    DEFER("ed.file.close", YEW_ARITY_NONE, YEW_CMD_NEEDS_WIN, 23,
          "close the active file"),
    DEFER("ed.buf.next", YEW_ARITY_NONE, YEW_CMD_REPEATABLE, 23,
          "activate the next buffer"),
    DEFER("ed.buf.prev", YEW_ARITY_NONE, YEW_CMD_REPEATABLE, 23,
          "activate the previous buffer"),
    {"ed.tab.goto", yew_tab_cmd_goto, YEW_ARITY_OPT_INT,
     YEW_CMD_TAKES_COUNT, "Activate a numbered tab (0 = tab 10)", NULL},
    {"ed.tab.new", yew_tab_cmd_new, YEW_ARITY_NONE, 0U,
     "Open an untitled tab", NULL},
    {"ed.tab.open", yew_tab_cmd_open, YEW_ARITY_STR, 0U,
     "Open a path in a new tab", NULL},
    {"ed.tab.close", yew_tab_cmd_close, YEW_ARITY_NONE, 0U,
     "Close the active tab", NULL},
    {"ed.tab.next", yew_tab_cmd_next, YEW_ARITY_NONE,
     YEW_CMD_REPEATABLE, "Activate the next tab", NULL},
    {"ed.tab.prev", yew_tab_cmd_prev, YEW_ARITY_NONE,
     YEW_CMD_REPEATABLE, "Activate the previous tab", NULL},
    {"ed.tab.move", yew_tab_cmd_move, YEW_ARITY_OPT_INT,
     YEW_CMD_TAKES_COUNT, "Move the active tab to position N", NULL},
    /* Sprint 27 §5: the tab context menu's rows.  Commands, because
     * invariant 9 requires a keyboard path for every menu row. */
    {"ed.tab.close_others", yew_tab_cmd_close_others, YEW_ARITY_NONE, 0U,
     "Close every tab but the active one", NULL},
    {"ed.tab.copy_path", yew_tab_cmd_copy_path, YEW_ARITY_NONE, 0U,
     "Copy the active tab's canonical path to the clipboard", NULL},
    /* Sprint 24 §6: the continuous line.  next/prev walk EVERY open
     * file — members of the active group first, then the row-1 entry
     * beside it — so left/right never dead-ends inside a group. */
    {"ed.file.next", yew_file_cmd_next, YEW_ARITY_NONE,
     YEW_CMD_REPEATABLE, "Walk to the next open file", NULL},
    {"ed.file.prev", yew_file_cmd_prev, YEW_ARITY_NONE,
     YEW_CMD_REPEATABLE, "Walk to the previous open file", NULL},
    {"ed.group.enter", yew_group_cmd_enter, YEW_ARITY_NONE, 0U,
     "Enter a tab group, resuming where you left it", NULL},
    {"ed.group.leave", yew_group_cmd_leave, YEW_ARITY_NONE, 0U,
     "Leave the active tab group", NULL},
    {"ed.group.dissolve", yew_group_cmd_dissolve, YEW_ARITY_NONE, 0U,
     "Dissolve the active group; its tabs stay open", NULL},
    {"ed.group.remove_tab", yew_group_cmd_remove_tab, YEW_ARITY_NONE, 0U,
     "Remove the active tab from its group", NULL},
    /* Sprint 27 §5/§9. */
    {"ed.ui.context_menu", yew_ui_cmd_context_menu, YEW_ARITY_NONE, 0U,
     "Open the context menu for the focused tab or group", NULL},
    {"ed.mouse.enable", yew_mouse_cmd_enable, YEW_ARITY_NONE, 0U,
     "Turn mouse reporting on for this session", NULL},
    {"ed.mouse.disable", yew_mouse_cmd_disable, YEW_ARITY_NONE, 0U,
     "Turn mouse reporting off for this session", NULL},
    /* Sprint 27 §8: the keyboard twin of dropping a tab into a group. */
    {"ed.group.add_tab", yew_group_cmd_add_tab, YEW_ARITY_STR, 0U,
     "Add the active tab to the named group", NULL},
    {"ed.group.rename", yew_group_cmd_rename, YEW_ARITY_OPT_STR,
     YEW_CMD_PROMPTS, "Rename the active tab group", NULL},
    {"ed.group.new", yew_gp_cmd_new, YEW_ARITY_OPT_STR, YEW_CMD_PROMPTS,
     "Assemble a new tab group", NULL},
    {"ed.group.edit", yew_gp_cmd_edit, YEW_ARITY_NONE, YEW_CMD_PROMPTS,
     "Edit the active group's membership", NULL},
    {"ed.group.from_dir", yew_group_cmd_from_dir, YEW_ARITY_OPT_STR,
     YEW_CMD_RECORDABLE, "open a directory as a tab group (F-mode)", "from_dir"},
    DEFER("ed.group.next", YEW_ARITY_NONE, YEW_CMD_REPEATABLE, 24,
          "activate the next tab group"),
    DEFER("ed.group.prev", YEW_ARITY_NONE, YEW_CMD_REPEATABLE, 24,
          "activate the previous tab group"),
    /* Sprint 25 §9: workspace state. */
    {"ed.ws.save_state", yew_ws_cmd_save_state, YEW_ARITY_NONE, 0U,
     "Write this workspace's state now, without waiting for the debounce", NULL},
    {"ed.ws.restore_state", yew_ws_cmd_restore_state, YEW_ARITY_NONE, 0U,
     "Open what this workspace's saved state names, alongside what is open", NULL},
    {"ed.ws.info", yew_ws_cmd_info, YEW_ARITY_NONE, 0U,
     "Report the workspace key, state directory, path record and lock owner", NULL},
    {"ed.ws.forget", yew_ws_cmd_forget, YEW_ARITY_NONE, 0U,
     "Delete this workspace's state directory, after confirming", NULL},
    /*
     * v1 is FROZEN and there is no v2, so there is nothing to migrate
     * TO.  The name exists and hard-errors rather than being absent and
     * reading as "no such command" (invariant 3); the first sprint that
     * needs v2 builds the framework and takes this over.
     */
    DEFER("ed.ws.migrate", YEW_ARITY_NONE, 0U, 25,
          "migrate workspace state to a newer schema (no v2 exists)"),
    /*
     * Sprint 18.5 ranks command NAMES and declared abbreviations.  The
     * full palette -- a picker that also matches help text -- stays
     * Sprint 38, so the name exists and hard-errors rather than being
     * absent and reading as "no such command" (invariant 3).
     */
    /* Sprint 26 §6: the three instances. */
    {"ed.find.file", yew_find_cmd_file, YEW_ARITY_NONE, YEW_CMD_PROMPTS,
     "Find a file in the workspace by fuzzy name", NULL},
    {"ed.find.buffer", yew_find_cmd_buffer, YEW_ARITY_NONE, YEW_CMD_PROMPTS,
     "Switch to an open tab by fuzzy name", NULL},
    {"ed.undo.branches", yew_undo_cmd_branches, YEW_ARITY_NONE,
     YEW_CMD_NEEDS_WIN | YEW_CMD_PROMPTS,
     "Pick an undo state from the branch tree", NULL},
    /*
     * Sprint 26 §9 defers these two, and they must EXIST to say so:
     * absent, they read to the user as "no such command" rather than
     * "not yet" (invariant 3).  Both are PickerSpec values over §5's
     * widget when their sprint arrives — no new machinery.
     */
    DEFER("ed.find.symbol", YEW_ARITY_NONE, 0U, 47,
          "pick a symbol from the LSP workspace index"),
    DEFER("ed.find.command", YEW_ARITY_NONE, 0U, 38,
          "open the command palette"),
    {"ed.pane.split_h", yew_pane_cmd_split_h, YEW_ARITY_NONE,
     YEW_CMD_NEEDS_WIN, "Split the focused pane side by side", NULL},
    {"ed.pane.split_v", yew_pane_cmd_split_v, YEW_ARITY_NONE,
     YEW_CMD_NEEDS_WIN, "Split the focused pane top and bottom", NULL},
    {"ed.pane.close", yew_pane_cmd_close, YEW_ARITY_NONE,
     YEW_CMD_NEEDS_WIN, "Close the focused pane", NULL},
    {"ed.pane.focus_left", yew_pane_cmd_focus_left, YEW_ARITY_NONE,
     YEW_CMD_NEEDS_WIN, "Focus the pane to the left", NULL},
    {"ed.pane.focus_right", yew_pane_cmd_focus_right, YEW_ARITY_NONE,
     YEW_CMD_NEEDS_WIN, "Focus the pane to the right", NULL},
    {"ed.pane.focus_up", yew_pane_cmd_focus_up, YEW_ARITY_NONE,
     YEW_CMD_NEEDS_WIN, "Focus the pane above", NULL},
    {"ed.pane.focus_down", yew_pane_cmd_focus_down, YEW_ARITY_NONE,
     YEW_CMD_NEEDS_WIN, "Focus the pane below", NULL},
    {"ed.pane.focus_next", yew_pane_cmd_focus_next, YEW_ARITY_NONE,
     YEW_CMD_NEEDS_WIN, "Focus the next pane in tree order", NULL},
    {"ed.pane.grow", yew_pane_cmd_grow, YEW_ARITY_OPT_INT,
     YEW_CMD_TAKES_COUNT | YEW_CMD_NEEDS_WIN, "Grow the focused pane", NULL},
    {"ed.pane.shrink", yew_pane_cmd_shrink, YEW_ARITY_OPT_INT,
     YEW_CMD_TAKES_COUNT | YEW_CMD_NEEDS_WIN, "Shrink the focused pane", NULL},
    DEFER("ed.win.next", YEW_ARITY_NONE, YEW_CMD_REPEATABLE, 22,
          "focus the next window"),
    DEFER("ed.win.prev", YEW_ARITY_NONE, YEW_CMD_REPEATABLE, 22,
          "focus the previous window"),

    {"ed.search.open", yew_search_cmd_open, YEW_ARITY_OPT_STR,
     YEW_CMD_PROMPTS | YEW_CMD_NEEDS_WIN, "Open incremental search", NULL},
    {"ed.search.open_back", yew_search_cmd_open_back, YEW_ARITY_OPT_STR,
     YEW_CMD_PROMPTS | YEW_CMD_NEEDS_WIN,
     "Open incremental search, backwards", NULL},
    {"ed.search.next", yew_search_cmd_next, YEW_ARITY_OPT_INT,
     YEW_CMD_TAKES_COUNT | YEW_CMD_RECORDABLE | YEW_CMD_NEEDS_WIN,
     "Move to the next search match", "search_next"},
    {"ed.search.prev", yew_search_cmd_prev, YEW_ARITY_OPT_INT,
     YEW_CMD_TAKES_COUNT | YEW_CMD_RECORDABLE | YEW_CMD_NEEDS_WIN,
     "Move to the previous search match", "search_prev"},
    {"ed.search.word_next", yew_search_cmd_word_next, YEW_ARITY_NONE,
     YEW_CMD_RECORDABLE | YEW_CMD_NEEDS_WIN,
     "Search forward for the word under the cursor", "word_next"},
    {"ed.search.word_prev", yew_search_cmd_word_prev, YEW_ARITY_NONE,
     YEW_CMD_RECORDABLE | YEW_CMD_NEEDS_WIN,
     "Search backward for the word under the cursor", "word_prev"},
    {"ed.mark.set", yew_mark_cmd_set, YEW_ARITY_STR,
     YEW_CMD_CAPTURES_TEXT | YEW_CMD_NEEDS_WIN,
     "Set a named mark at the cursor", NULL},
    {"ed.mark.jump", yew_mark_cmd_jump, YEW_ARITY_STR,
     YEW_CMD_CAPTURES_TEXT | YEW_CMD_NEEDS_WIN,
     "Jump to a named mark", NULL},
    {"ed.search.clear_highlight", yew_search_cmd_clear_highlight,
     YEW_ARITY_NONE, YEW_CMD_NEEDS_WIN, "Clear match highlighting", NULL},
    {"ed.jump.back", yew_jump_cmd_back, YEW_ARITY_OPT_INT,
     YEW_CMD_TAKES_COUNT | YEW_CMD_NEEDS_WIN,
     "Jump to an older position in this window's history", NULL},
    {"ed.jump.fwd", yew_jump_cmd_fwd, YEW_ARITY_OPT_INT,
     YEW_CMD_TAKES_COUNT | YEW_CMD_NEEDS_WIN,
     "Jump to a newer position in this window's history", NULL},
    {"ed.jump.list", yew_jump_cmd_list, YEW_ARITY_NONE, YEW_CMD_NEEDS_WIN,
     "Show this window's jumplist", NULL},
    {"ed.change.older", yew_change_cmd_older, YEW_ARITY_OPT_INT,
     YEW_CMD_TAKES_COUNT | YEW_CMD_NEEDS_WIN,
     "Jump to an older change position in this buffer", NULL},
    {"ed.change.newer", yew_change_cmd_newer, YEW_ARITY_OPT_INT,
     YEW_CMD_TAKES_COUNT | YEW_CMD_NEEDS_WIN,
     "Jump to a newer change position in this buffer", NULL},
    {"ed.search.replace", yew_search_cmd_replace, YEW_ARITY_STR,
     YEW_CMD_CHANGES_BUFFER | YEW_CMD_NEEDS_WIN,
     "Substitute matches of a pattern in a line range", NULL},
    {"ed.search.global", yew_search_cmd_global, YEW_ARITY_STR, 0U,
     "Rejected: :g is Fletch's query API in Sprint 34", NULL},
    {"ed.macro.record", yew_record_cmd_record, YEW_ARITY_OPT_STR,
     YEW_CMD_PROMPTS, "Record a command macro", NULL},
    {"ed.macro.stop", yew_record_cmd_stop, YEW_ARITY_NONE, 0U,
     "Stop recording a command macro", NULL},
    {"ed.macro.replay", yew_record_cmd_replay, YEW_ARITY_STR,
     YEW_CMD_REPEATABLE | YEW_CMD_RECORDABLE | YEW_CMD_CAPTURES_TEXT,
     "Replay a command macro",
     "replay"},
    {"ed.macro.replay_last", yew_record_cmd_replay_last, YEW_ARITY_NONE,
     YEW_CMD_REPEATABLE | YEW_CMD_RECORDABLE,
     "Replay the last command macro", "replay_last"},
    {"ed.macro.list", yew_macro_cmd_list, YEW_ARITY_NONE, 0U,
     "List registers containing macros", NULL},
    {"ed.macro.edit", yew_macro_cmd_edit, YEW_ARITY_STR, 0U,
     "Edit a macro register as Fletch source", NULL},
    {"ed.macro.name", yew_macro_cmd_name, YEW_ARITY_STR, 0U,
     "Promote a macro register into the library", NULL},
    {"ed.macro.reload", yew_macro_cmd_reload, YEW_ARITY_NONE, 0U,
     "Reload the macro library", NULL},
    {"ed.shell.run", yew_shell_cmd_run, YEW_ARITY_STR,
     YEW_CMD_RECORDABLE, "Run a shell command, streaming its output", "shell_run"},
    {"ed.shell.run_bg", yew_shell_cmd_run_bg, YEW_ARITY_STR,
     YEW_CMD_RECORDABLE, "Run a shell command without stealing focus", "shell_bg"},
    {"ed.shell.read", yew_shell_cmd_read, YEW_ARITY_STR,
     YEW_CMD_RECORDABLE | YEW_CMD_NEEDS_WIN | YEW_CMD_CHANGES_BUFFER,
     "Insert a shell command's output at the cursor", "shell_read"},
    {"ed.shell.filter", yew_shell_cmd_filter, YEW_ARITY_STR,
     YEW_CMD_RECORDABLE | YEW_CMD_NEEDS_WIN | YEW_CMD_CHANGES_BUFFER,
     "Pipe a region through a shell command and replace it", "filter"},
    {"ed.shell.term", yew_shell_cmd_term, YEW_ARITY_NONE, 0U,
     "Interactive terminals are not a 1.0 feature", NULL},
    {"ed.job.list", yew_job_cmd_list, YEW_ARITY_NONE, 0U,
     "Open the job table", NULL},
    {"ed.job.kill", yew_job_cmd_kill, YEW_ARITY_OPT_INT,
     YEW_CMD_TAKES_COUNT, "Terminate a job's process group", NULL},
    {"ed.job.kill_force", yew_job_cmd_kill_force, YEW_ARITY_OPT_INT,
     YEW_CMD_TAKES_COUNT, "Kill a job's process group", NULL},
    {"ed.job.jump", yew_job_cmd_jump, YEW_ARITY_OPT_INT,
     YEW_CMD_TAKES_COUNT, "Focus a job's output buffer", NULL},
    {"ed.job.clear_finished", yew_job_cmd_clear_finished, YEW_ARITY_NONE,
     0U, "Drop every finished job and its output buffer", NULL},
    {"ed.job.rerun", yew_job_cmd_rerun, YEW_ARITY_OPT_INT,
     YEW_CMD_TAKES_COUNT, "Run a job's command line again", NULL},
    {"ed.lsp.info", yew_lsp_cmd_info, YEW_ARITY_NONE, 0U,
     "Show LSP module and server status", NULL},
    {"ed.lsp.log", yew_lsp_cmd_log, YEW_ARITY_NONE, 0U,
     "Open the LSP server log", NULL},
    {"ed.lsp.restart", yew_lsp_cmd_start, YEW_ARITY_NONE, YEW_CMD_NEEDS_WIN,
     "Restart the server for the current buffer", NULL},
    {"ed.lsp.stop", yew_lsp_cmd_stop, YEW_ARITY_NONE, YEW_CMD_NEEDS_WIN,
     "Stop the server for the current buffer", NULL},
    {"ed.lsp.diagnostics", yew_lsp_cmd_diagnostics, YEW_ARITY_NONE,
     YEW_CMD_NEEDS_WIN, "Open the diagnostic list", NULL},
    {"ed.lsp.diag_next", yew_lsp_cmd_diag_next, YEW_ARITY_NONE,
     YEW_CMD_NEEDS_WIN, "Jump to the next diagnostic", NULL},
    {"ed.lsp.diag_prev", yew_lsp_cmd_diag_prev, YEW_ARITY_NONE,
     YEW_CMD_NEEDS_WIN, "Jump to the previous diagnostic", NULL},
    {"ed.lsp.goto_def", yew_lsp_cmd_goto_def, YEW_ARITY_NONE,
     YEW_CMD_NEEDS_WIN, "Go to the definition under the cursor", NULL},
    {"ed.lsp.goto_decl", yew_lsp_cmd_goto_decl, YEW_ARITY_NONE,
     YEW_CMD_NEEDS_WIN, "Go to the declaration under the cursor", NULL},
    {"ed.lsp.goto_type", yew_lsp_cmd_goto_type, YEW_ARITY_NONE,
     YEW_CMD_NEEDS_WIN, "Go to the type definition under the cursor", NULL},
    {"ed.lsp.goto_impl", yew_lsp_cmd_goto_impl, YEW_ARITY_NONE,
     YEW_CMD_NEEDS_WIN, "Go to the implementation under the cursor", NULL},
    {"ed.lsp.references", yew_lsp_cmd_references, YEW_ARITY_NONE,
     YEW_CMD_NEEDS_WIN | YEW_CMD_PROMPTS,
     "List references to the symbol under the cursor", NULL},
    {"ed.lsp.hover", yew_lsp_cmd_hover, YEW_ARITY_NONE,
     YEW_CMD_NEEDS_WIN | YEW_CMD_PROMPTS, "Show hover information", NULL},
    {"ed.lsp.rename", yew_lsp_cmd_rename, YEW_ARITY_NONE,
     YEW_CMD_NEEDS_WIN | YEW_CMD_PROMPTS | YEW_CMD_CHANGES_BUFFER |
         YEW_CMD_MULTI_AGGREGATE,
     "Rename the symbol under the cursor", NULL},
    {"ed.lsp.symbols", yew_lsp_cmd_symbols, YEW_ARITY_NONE,
     YEW_CMD_NEEDS_WIN | YEW_CMD_PROMPTS, "List document symbols", NULL},
    {"ed.lsp.signature", yew_lsp_cmd_signature, YEW_ARITY_NONE,
     YEW_CMD_NEEDS_WIN | YEW_CMD_PROMPTS, "Show signature help", NULL},
    {"ed.lsp.complete", yew_lsp_cmd_complete, YEW_ARITY_NONE,
     YEW_CMD_NEEDS_WIN | YEW_CMD_PROMPTS, "Request LSP completion", NULL},
    {"ed.ai.backends", yew_ai_cmd_backends, YEW_ARITY_NONE, 0U,
     "List configured AI backends and transport state", NULL},
    {"ed.ai.models", yew_ai_cmd_models, YEW_ARITY_NONE, 0U,
     "List models supported by the selected AI backend", NULL},
    {"ed.ai.ping", yew_ai_cmd_ping, YEW_ARITY_NONE, 0U,
     "Probe the selected AI backend", NULL},
    {"ed.ai.log", yew_ai_cmd_log, YEW_ARITY_NONE, 0U,
     "Open the AI transport log", NULL},
    {"ed.ai.reload", yew_ai_cmd_reload, YEW_ARITY_NONE, 0U,
     "Reload AI backends and clear cached credentials", NULL},
    {"ed.ai.enable", yew_ai_cmd_enable, YEW_ARITY_NONE, YEW_CMD_PROMPTS,
     "Enable AI after the privacy disclosure", NULL},
    {"ed.ai.disable", yew_ai_cmd_disable, YEW_ARITY_NONE, 0U,
     "Disable AI without deleting backend definitions", NULL},
    {"ed.ai.forget", yew_ai_cmd_forget, YEW_ARITY_NONE, 0U,
     "Remove this workspace's AI grant", NULL},
    {"ed.ai.privacy", yew_ai_cmd_privacy, YEW_ARITY_NONE,
     YEW_CMD_NEEDS_WIN, "Open the AI privacy page", NULL},
    {"ed.ai.preset", yew_ai_cmd_preset, YEW_ARITY_STR, 0U,
     "Load the local or cloud AI preset", NULL},
    {"ed.ai.status", yew_ai_cmd_status, YEW_ARITY_NONE, 0U,
     "Show the effective AI privacy gates", NULL},
    {"ed.ai.stats", yew_ai_cmd_stats, YEW_ARITY_NONE, 0U,
     "Show local AI request statistics", NULL},
    {"ed.git.info", yew_git_cmd_info, YEW_ARITY_NONE, 0U,
     "Show repository, branch, state, and snapshot age", NULL},
    {"ed.git.refresh", yew_git_cmd_refresh, YEW_ARITY_NONE,
     YEW_CMD_RECORDABLE, "Force an asynchronous Git snapshot refresh",
     "git_refresh"},
    {"ed.git.log", yew_git_cmd_log, YEW_ARITY_NONE, 0U,
     "Populate the pinned Git log record list", NULL},
    {"ed.git.init", yew_fuss_cmd_init, YEW_ARITY_NONE,
     YEW_CMD_RECORDABLE, "Initialize the workspace repository", "git_init"},
    {"ed.git.mode.leave", yew_fuss_cmd_leave, YEW_ARITY_NONE,
     YEW_CMD_RECORDABLE, "Leave F mode", "git_leave"},
    {"ed.git.tree.all", yew_fuss_cmd_tree_all, YEW_ARITY_NONE,
     YEW_CMD_RECORDABLE, "Toggle the all-files F-mode tree", "git_tree_all"},
    {"ed.git.tree.hidden", yew_fuss_cmd_tree_hidden, YEW_ARITY_NONE,
     YEW_CMD_RECORDABLE, "Toggle hidden and ignored files", "git_hidden"},
    {"ed.git.nav.prev", yew_fuss_cmd_nav_prev, YEW_ARITY_NONE,
     YEW_CMD_REPEATABLE | YEW_CMD_RECORDABLE,
     "Move to the previous F-mode entry", "git_prev"},
    {"ed.git.nav.next", yew_fuss_cmd_nav_next, YEW_ARITY_NONE,
     YEW_CMD_REPEATABLE | YEW_CMD_RECORDABLE,
     "Move to the next F-mode entry", "git_next"},
    {"ed.git.nav.parent", yew_fuss_cmd_nav_parent, YEW_ARITY_NONE,
     YEW_CMD_RECORDABLE, "Move to the parent F-mode entry", "git_parent"},
    {"ed.git.nav.enter", yew_fuss_cmd_nav_enter, YEW_ARITY_NONE,
     YEW_CMD_RECORDABLE, "Enter the selected F-mode directory", "git_enter"},
    {"ed.git.nav.toggle", yew_fuss_cmd_nav_toggle, YEW_ARITY_NONE,
     YEW_CMD_RECORDABLE, "Toggle the selected F-mode directory", "git_toggle"},
    {"ed.git.nav.row_prev", yew_fuss_cmd_nav_row_prev, YEW_ARITY_NONE,
     YEW_CMD_REPEATABLE | YEW_CMD_RECORDABLE,
     "Move to the previous raw F-mode row", "git_row_prev"},
    {"ed.git.nav.row_next", yew_fuss_cmd_nav_row_next, YEW_ARITY_NONE,
     YEW_CMD_REPEATABLE | YEW_CMD_RECORDABLE,
     "Move to the next raw F-mode row", "git_row_next"},
    {"ed.git.jump.arm", yew_fuss_cmd_jump_arm, YEW_ARITY_NONE,
     YEW_CMD_RECORDABLE, "Arm F-mode fuzzy jump", "git_jump"},
    {"ed.git.stage", yew_fuss_cmd_stage, YEW_ARITY_OPT_STR,
     YEW_CMD_NEEDS_WIN | YEW_CMD_RECORDABLE, "Stage the selected path",
     "git_stage"},
    {"ed.git.unstage", yew_fuss_cmd_unstage, YEW_ARITY_OPT_STR,
     YEW_CMD_NEEDS_WIN | YEW_CMD_RECORDABLE, "Unstage the selected path",
     "git_unstage"},
    {"ed.git.stage.all", yew_fuss_cmd_stage_all, YEW_ARITY_NONE,
     YEW_CMD_RECORDABLE, "Stage all paths", "git_stage_all"},
    {"ed.git.unstage.all", yew_fuss_cmd_unstage_all, YEW_ARITY_NONE,
     YEW_CMD_RECORDABLE, "Unstage all paths", "git_unstage_all"},
    {"ed.git.commit", yew_fuss_cmd_commit, YEW_ARITY_NONE,
     YEW_CMD_PROMPTS | YEW_CMD_RECORDABLE, "Commit staged changes",
     "git_commit"},
    {"ed.git.commit.amend", yew_fuss_cmd_commit_amend, YEW_ARITY_NONE,
     YEW_CMD_PROMPTS | YEW_CMD_RECORDABLE, "Amend the current commit",
     "git_amend"},
    {"ed.git.push", yew_fuss_cmd_push, YEW_ARITY_NONE,
     YEW_CMD_PROMPTS | YEW_CMD_RECORDABLE, "Push the current branch",
     "git_push"},
    {"ed.git.push.force", yew_fuss_cmd_push_force, YEW_ARITY_NONE,
     YEW_CMD_PROMPTS | YEW_CMD_RECORDABLE,
     "Force-push after typed confirmation", "git_force_push"},
    {"ed.git.pull", yew_fuss_cmd_pull, YEW_ARITY_NONE,
     YEW_CMD_RECORDABLE, "Pull the current branch", "git_pull"},
    {"ed.git.fetch", yew_fuss_cmd_fetch, YEW_ARITY_NONE,
     YEW_CMD_RECORDABLE, "Fetch remotes", "git_fetch"},
    {"ed.git.diff", yew_fuss_cmd_diff, YEW_ARITY_OPT_STR,
     YEW_CMD_NEEDS_WIN | YEW_CMD_RECORDABLE, "Open the selected path's diff",
     "git_diff"},
    {"ed.git.status", yew_fuss_cmd_status, YEW_ARITY_NONE,
     YEW_CMD_RECORDABLE, "Open Git status output", "git_status"},
    {"ed.git.blame", yew_fuss_cmd_blame, YEW_ARITY_OPT_STR,
     YEW_CMD_NEEDS_WIN | YEW_CMD_RECORDABLE, "Open blame for the selected path",
     "git_blame"},
    {"ed.git.history", yew_fuss_cmd_history, YEW_ARITY_NONE,
     YEW_CMD_RECORDABLE, "Open repository history", "git_history"},
    {"ed.git.reflog", yew_fuss_cmd_reflog, YEW_ARITY_NONE,
     YEW_CMD_RECORDABLE, "Open the reflog", "git_reflog"},
    {"ed.git.view", yew_fuss_cmd_view, YEW_ARITY_OPT_STR,
     YEW_CMD_NEEDS_WIN | YEW_CMD_RECORDABLE, "View the selected path",
     "git_view"},
    {"ed.git.branch.switch", yew_fuss_cmd_branch_switch, YEW_ARITY_NONE,
     YEW_CMD_PROMPTS | YEW_CMD_RECORDABLE, "Switch branches", "git_switch"},
    {"ed.git.branch.create", yew_fuss_cmd_branch_create, YEW_ARITY_OPT_STR,
     YEW_CMD_PROMPTS | YEW_CMD_RECORDABLE,
     "Create and switch to a branch", "git_branch_new"},
    {"ed.git.branch.delete", yew_fuss_cmd_branch_delete, YEW_ARITY_NONE,
     YEW_CMD_PROMPTS | YEW_CMD_RECORDABLE, "Delete a branch", "git_branch_del"},
    {"ed.git.merge", yew_fuss_cmd_merge, YEW_ARITY_NONE,
     YEW_CMD_PROMPTS | YEW_CMD_RECORDABLE, "Merge a branch", "git_merge"},
    {"ed.git.reset", yew_fuss_cmd_reset, YEW_ARITY_NONE,
     YEW_CMD_PROMPTS | YEW_CMD_RECORDABLE, "Reset to a commit", "git_reset"},
    {"ed.git.rebase.interactive", yew_fuss_cmd_rebase_interactive,
     YEW_ARITY_NONE, YEW_CMD_PROMPTS | YEW_CMD_RECORDABLE,
     "Start an interactive rebase", "git_rebase_i"},
    {"ed.git.rebase.continue", yew_fuss_cmd_rebase_continue, YEW_ARITY_NONE,
     YEW_CMD_RECORDABLE, "Continue a rebase", "git_rebase_cont"},
    {"ed.git.rebase.abort", yew_fuss_cmd_rebase_abort, YEW_ARITY_NONE,
     YEW_CMD_RECORDABLE, "Abort a rebase", "git_rebase_abort"},
    {"ed.git.cherry_pick", yew_fuss_cmd_cherry_pick, YEW_ARITY_NONE,
     YEW_CMD_PROMPTS | YEW_CMD_RECORDABLE, "Cherry-pick a commit", "git_pick"},
    {"ed.git.revert", yew_fuss_cmd_revert, YEW_ARITY_NONE,
     YEW_CMD_PROMPTS | YEW_CMD_RECORDABLE, "Revert a commit", "git_revert"},
    {"ed.git.stash.push", yew_fuss_cmd_stash_push, YEW_ARITY_OPT_STR,
     YEW_CMD_PROMPTS | YEW_CMD_RECORDABLE, "Stash worktree changes",
     "git_stash"},
    {"ed.git.stash.pop", yew_fuss_cmd_stash_pop, YEW_ARITY_NONE,
     YEW_CMD_PROMPTS | YEW_CMD_RECORDABLE, "Apply or pop a stash",
     "git_stash_pop"},
    {"ed.git.tag", yew_fuss_cmd_tag, YEW_ARITY_OPT_STR,
     YEW_CMD_PROMPTS | YEW_CMD_RECORDABLE, "Create a tag", "git_tag"},
    {"ed.git.discard", yew_fuss_cmd_discard, YEW_ARITY_OPT_STR,
     YEW_CMD_PROMPTS | YEW_CMD_RECORDABLE,
     "Discard the selected path's changes", "git_discard"},
    {"ed.git.file.delete", yew_fuss_cmd_file_delete, YEW_ARITY_OPT_STR,
     YEW_CMD_PROMPTS | YEW_CMD_RECORDABLE, "Delete the selected path",
     "git_file_del"},
    {"ed.git.file.rename", yew_fuss_cmd_file_rename, YEW_ARITY_OPT_STR,
     YEW_CMD_PROMPTS | YEW_CMD_RECORDABLE, "Rename the selected path",
     "git_file_rename"},
    {"ed.git.open", yew_fuss_cmd_open, YEW_ARITY_OPT_STR,
     YEW_CMD_NEEDS_WIN | YEW_CMD_RECORDABLE,
     "Open the selected path and leave F mode", "git_open"},
    {"ed.git.hunk.next", yew_git_cmd_hunk_next, YEW_ARITY_NONE,
     YEW_CMD_REPEATABLE | YEW_CMD_NEEDS_WIN, "Jump to next changed hunk", NULL},
    {"ed.git.hunk.prev", yew_git_cmd_hunk_prev, YEW_ARITY_NONE,
     YEW_CMD_REPEATABLE | YEW_CMD_NEEDS_WIN, "Jump to previous changed hunk", NULL},
    {"ed.git.hunk.first", yew_git_cmd_hunk_first, YEW_ARITY_NONE,
     YEW_CMD_NEEDS_WIN, "Jump to first changed hunk", NULL},
    {"ed.git.hunk.last", yew_git_cmd_hunk_last, YEW_ARITY_NONE,
     YEW_CMD_NEEDS_WIN, "Jump to last changed hunk", NULL},
    {"ed.git.hunk.stage", yew_git_cmd_hunk_stage, YEW_ARITY_NONE,
     YEW_CMD_NEEDS_WIN | YEW_CMD_MULTI_AGGREGATE,
     "Stage selected hunks", NULL},
    {"ed.git.hunk.unstage", yew_git_cmd_hunk_unstage, YEW_ARITY_NONE,
     YEW_CMD_NEEDS_WIN, "Explain hunk unstage scope", NULL},
    {"ed.git.hunk.discard", yew_git_cmd_hunk_discard, YEW_ARITY_NONE,
     YEW_CMD_NEEDS_WIN | YEW_CMD_CHANGES_BUFFER | YEW_CMD_MULTI_AGGREGATE,
     "Discard selected hunks into undo history", NULL},
    {"ed.git.blame.toggle", yew_git_cmd_blame_toggle, YEW_ARITY_NONE,
     YEW_CMD_NEEDS_WIN, "Toggle inline blame", NULL},
    {"ed.git.diff.view", yew_git_cmd_diff_view, YEW_ARITY_NONE,
     YEW_CMD_NEEDS_WIN | YEW_CMD_RECORDABLE, "Open editor diff view",
     "git_diff_view"},
    {"ed.git.conflict.next", yew_git_cmd_conflict_scope, YEW_ARITY_NONE,
     YEW_CMD_NEEDS_WIN, "Explain conflict resolution scope", NULL},
    {"ed.git.conflict.ours", yew_git_cmd_conflict_scope, YEW_ARITY_NONE,
     YEW_CMD_NEEDS_WIN, "Explain conflict resolution scope", NULL},
    {"ed.git.conflict.theirs", yew_git_cmd_conflict_scope, YEW_ARITY_NONE,
     YEW_CMD_NEEDS_WIN, "Explain conflict resolution scope", NULL},
    {"ed.ai.open", yew_ai_cmd_open, YEW_ARITY_NONE, 0U,
     "Explain the ghost-only AI surface", NULL},
    DEFER("ed.plug.reload", YEW_ARITY_OPT_STR, 0U, 54,
          "reload a plugin")
};

#undef DEFER
#undef LSP_DEFER
#undef AI_DEFER

typedef struct {
    const char *name;
    const char *argspec;
    u8 range_policy;
    const char *abbrev;
} BuiltinMeta;

static const BuiltinMeta builtin_meta[] = {
    {"ed.quit", "", YEW_RP_FORBID, "q"},
    {"ed.redraw", "", YEW_RP_FORBID, "redraw"},
    {"ed.edit.line.delete", "", YEW_RP_LINE, "d"},
    {"ed.file.open", "f", YEW_RP_FORBID, "e"},
    {"ed.file.write", "f", YEW_RP_FORBID, "w"},
    {"ed.file.write_quit", "f", YEW_RP_FORBID, "wq"},
    {"ed.file.new", "f", YEW_RP_FORBID, "new"},
    {"ed.file.reload", "", YEW_RP_FORBID, "reload"},
    {"ed.file.close", "", YEW_RP_FORBID, "close"},
    {"ed.search.open", "s", YEW_RP_FORBID, "search"},
    {"ed.tab.new", "", YEW_RP_FORBID, "tabnew"},
    {"ed.tab.open", "f", YEW_RP_FORBID, "tabedit"},
    {"ed.tab.close", "", YEW_RP_FORBID, "tabclose"},
    {"ed.group.new", "s", YEW_RP_FORBID, "gnew"},
    {"ed.group.edit", "", YEW_RP_FORBID, "gedit"},
    {"ed.group.dissolve", "", YEW_RP_FORBID, "gdissolve"},
    {"ed.group.rename", "s", YEW_RP_OPT, "grename"},
    {"ed.group.add_tab", "s", YEW_RP_FORBID, "gadd"},
    {"ed.tab.close_others", "", YEW_RP_FORBID, "tabonly"},
    {"ed.tab.copy_path", "", YEW_RP_FORBID, "copypath"},
    {"ed.group.remove_tab", "", YEW_RP_FORBID, "gremove"},
    {"ed.group.enter", "", YEW_RP_FORBID, "genter"},
    {"ed.group.leave", "", YEW_RP_FORBID, "gleave"},
    {"ed.ws.save_state", "", YEW_RP_FORBID, "wssave"},
    {"ed.ws.restore_state", "", YEW_RP_FORBID, "wsrestore"},
    {"ed.ws.info", "", YEW_RP_FORBID, "wsinfo"},
    {"ed.ws.forget", "", YEW_RP_FORBID, "wsforget"},
    {"ed.find.file", "", YEW_RP_FORBID, "find"},
    {"ed.find.buffer", "", YEW_RP_FORBID, "buffers"},
    {"ed.undo.branches", "", YEW_RP_FORBID, "undolist"},
    /* The substitution body is ONE opaque string; s18's tokenizer must
     * not try to understand `/` inside a regex. */
    {"ed.search.replace", "s", YEW_RP_OPT, "s"},
    {"ed.search.global", "s", YEW_RP_OPT, "g"},
    {"ed.fl.eval", "s", YEW_RP_FORBID, "fl"},
    {"ed.opt.set_many", "ov", YEW_RP_FORBID, "set"},
    {"ed.theme.set", "s", YEW_RP_FORBID, "theme"},
    {"ed.theme.toggle", "", YEW_RP_FORBID, NULL},
    {"ed.mark.set", "s", YEW_RP_FORBID, "mark"},
    /* :! carries an arbitrary command line, so its argspec is one string
     * and the range decides run-vs-filter (§5). */
    {"ed.shell.run", "s", YEW_RP_OPT, NULL},
    {"ed.shell.read", "s", YEW_RP_FORBID, NULL},
    {"ed.shell.filter", "s", YEW_RP_REQUIRED, NULL},
    {"ed.macro.list", "", YEW_RP_FORBID, "macros"},
    {"ed.macro.edit", "s", YEW_RP_FORBID, NULL},
    {"ed.macro.name", "ss", YEW_RP_FORBID, NULL},
    {"ed.macro.reload", "", YEW_RP_FORBID, NULL},
    {"ed.job.list", "", YEW_RP_FORBID, "jobs"},
    {"ed.job.kill", "", YEW_RP_FORBID, NULL},
    {"ed.shell.term", "", YEW_RP_FORBID, "term"},
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
        "cmdline", "del", "shell", "opt", "fl", "config", "syn",
        "theme", "shadow", "compl",
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
        "reload", "status", "stats", "accept_word", "accept_word_alt",
        "accept_line", "accept_all", "doc_toggle", "reindex",
        /* Sprint 45: the LSP module surface is registered before its
         * lifecycle and feature implementations land. */
        "log", "restart", "diagnostics", "diag_next", "diag_prev",
        "goto_def", "goto_decl", "goto_type", "goto_impl", "references",
        "hover", "symbols", "signature", "complete",
        /* Sprint 48: the AI command boundary is discoverable even when the
         * optional module is stripped. */
        "backends", "models", "ping",
        /* Sprint 50: explicit privacy and preset surfaces. */
        "privacy", "preset",
        /* Sprint 51: the complete Git command surface is registered before
         * the Sprint 52/53 viewers and mutating actions land. */
        "abort", "all", "amend", "arm", "blame", "cherry_pick",
        "commit", "continue", "create", "diff", "discard", "fetch",
        "first", "force", "hidden", "history", "init", "interactive",
        "last", "merge", "parent", "pop", "pull", "push", "reflog",
        "refresh", "reset", "revert", "row_next", "row_prev", "switch",
        "tag", "unstage", "view", "ours", "theirs"};
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

        if (n == YEW_ARRAY_LEN(segments))
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
                       YEW_ARRAY_LEN(app_verbs));
    if ((n != 3U && n != 4U) ||
        !word_in(segments[1], lengths[1], domains, YEW_ARRAY_LEN(domains)))
        return false;
    return word_in(segments[n - 1U], lengths[n - 1U], verbs,
                   YEW_ARRAY_LEN(verbs));
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

    if ((d->flags & YEW_CMD_RECORDABLE) != 0U && w == NULL)
        YEW_BUG("recordable command %s has no CMDWORD", d->name);
    if (w == NULL)
        return;
    if ((d->flags & YEW_CMD_RECORDABLE) == 0U)
        YEW_BUG("command %s has a CMDWORD but is not recordable", d->name);
    n = strlen(w);
    if (n == 0U || n > 16U)
        YEW_BUG("command %s has a CMDWORD of bad length", d->name);
    if (w[0] < 'a' || w[0] > 'z')
        YEW_BUG("command %s CMDWORD must start with a letter", d->name);
    {
        size_t i;

        for (i = 1U; i < n; i++) {
            char c = w[i];

            if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                  c == '_'))
                YEW_BUG("command %s has an invalid CMDWORD", d->name);
        }
    }
}

static void desc_validate(const CmdDesc *d)
{
    const u32 known_flags = YEW_CMD_REPEATABLE | YEW_CMD_TAKES_COUNT |
                            YEW_CMD_RECORDABLE | YEW_CMD_NEEDS_WIN |
                            YEW_CMD_CHANGES_BUFFER | YEW_CMD_PROMPTS |
                            YEW_CMD_DEFERRED | YEW_CMD_MULTI_AGGREGATE |
                            YEW_CMD_CAPTURES_TEXT | YEW_CMD_INTERNAL |
                            YEW_CMD_INTERACTIVE;

    if (d == NULL)
        YEW_BUG("yew_cmd_register: NULL descriptor");
    if (!command_name_valid(d->name))
        YEW_BUG("invalid command name: %s", d->name ? d->name : "(null)");
    if (d->fn == NULL)
        YEW_BUG("command %s has no implementation", d->name);
    if (d->arity > YEW_ARITY_OPT_STR)
        YEW_BUG("command %s has invalid arity %u", d->name,
                (unsigned)d->arity);
    if ((d->flags & ~known_flags) != 0U)
        YEW_BUG("command %s has unknown flags", d->name);
    if ((d->flags & YEW_CMD_REPEATABLE) != 0U &&
        (d->flags & YEW_CMD_TAKES_COUNT) != 0U)
        YEW_BUG("command %s is both REPEATABLE and TAKES_COUNT", d->name);
    if ((d->flags & YEW_CMD_CAPTURES_TEXT) != 0U &&
        d->arity != YEW_ARITY_STR)
        YEW_BUG("command %s captures text without string arity", d->name);
    if (d->help == NULL || d->help[0] == '\0')
        YEW_BUG("command %s has empty help", d->name);
    if ((d->flags & YEW_CMD_DEFERRED) != 0U &&
        !help_names_sprint(d->help))
        YEW_BUG("deferred command %s help does not name its sprint", d->name);
    word_validate(d);
}

static const char *default_argspec(const CmdDesc *d)
{
    return d->arity == YEW_ARITY_NONE ? "" : "s";
}

static void entry_validate(const CmdEntry *entry)
{
    const char *p;

    if (entry == NULL)
        YEW_BUG("yew_cmd_register_entry: NULL entry");
    desc_validate(&entry->cmd);
    if (entry->range_policy > YEW_RP_REQUIRED)
        YEW_BUG("command %s has invalid range policy", entry->cmd.name);
    p = entry->argspec == NULL ? default_argspec(&entry->cmd) :
                                entry->argspec;
    while (*p != '\0') {
        if (*p != 'f' && *p != 'b' && *p != 'o' && *p != 'v' &&
            *p != 's' && !(*p == '*' && p[1] == '\0'))
            YEW_BUG("command %s has invalid argspec", entry->cmd.name);
        p++;
    }
    if (entry->abbrev != NULL) {
        if (entry->abbrev[0] == '\0')
            YEW_BUG("command %s has empty abbreviation", entry->cmd.name);
        for (p = entry->abbrev; *p != '\0'; p++) {
            if (!(isalnum((unsigned char)*p) || *p == '_'))
                YEW_BUG("command %s has invalid abbreviation",
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
        YEW_BUG("duplicate command registration: %s", d->name);
    if (registry.len == UINT32_MAX)
        YEW_BUG("command registry overflow");
    if (registry.len == registry.cap) {
        size_t cap = registry.cap ? registry.cap * 2U : 64U;

        if (cap < registry.cap)
            YEW_BUG("command registry allocation overflow");
        registry.entries = yew_xreallocarray(registry.entries, cap,
                                             sizeof(*registry.entries));
        registry.cap = cap;
    }
    id = yew_intern_cstr(&registry.names, d->name);
    if ((size_t)id != registry.len + 1U)
        YEW_BUG("command registry and interner order diverged");
    copy = *entry;
    copy.cmd = *d;
    if ((copy.cmd.flags & YEW_CMD_PROMPTS) != 0U)
        copy.cmd.flags |= YEW_CMD_INTERACTIVE;
    copy.cmd.name = yew_intern_str(&registry.names, id);
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
            YEW_BUG("duplicate CMDWORD '%s' on %s", copy.cmd.word,
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
        YEW_BUG("yew_cmd_register: NULL descriptor");
    entry = (CmdEntry){*d, NULL, YEW_RP_FORBID, NULL};
    return register_entry(&entry);
}

static void install_builtin_meta(void)
{
    size_t i;

    for (i = 0U; i < YEW_ARRAY_LEN(builtin_meta); i++) {
        const BuiltinMeta *meta = &builtin_meta[i];
        void *found = strmap_get(&registry.names.map, meta->name,
                                 strlen(meta->name));
        u32 id = (u32)(uintptr_t)found;
        CmdEntry *entry;

        if (id == 0U || (size_t)id > registry.len)
            YEW_BUG("metadata names missing command: %s", meta->name);
        entry = &registry.entries[id - 1U];
        entry->argspec = arena_strdup(&registry.arena, meta->argspec);
        entry->range_policy = meta->range_policy;
        entry->abbrev = meta->abbrev == NULL ? NULL :
                        arena_strdup(&registry.arena, meta->abbrev);
        entry_validate(entry);
    }
}

void yew_cmd_init(void)
{
    size_t i;

    if (registry.initialized)
        return;
    arena_init(&registry.arena);
    interner_init(&registry.names, &registry.arena);
    strmap_init(&registry.words);
    registry.initialized = true;
    for (i = 0; i < YEW_ARRAY_LEN(builtins); i++)
        (void)register_desc(&builtins[i]);
    install_builtin_meta();
}

void yew_cmd_shutdown(void)
{
    if (!registry.initialized)
        return;
    interner_free(&registry.names);
    strmap_free(&registry.words);
    arena_free_all(&registry.arena);
    free(registry.entries);
    registry = (CmdRegistry){0};
}

CmdId yew_cmd_register(const CmdDesc *d)
{
    yew_cmd_init();
    return register_desc(d);
}

CmdId yew_cmd_register_entry(const CmdEntry *entry)
{
    yew_cmd_init();
    return register_entry(entry);
}

CmdId yew_cmd_lookup(const char *name, u32 len)
{
    void *found;

    yew_cmd_init();
    if (name == NULL)
        return YEW_CMD_NONE;
    found = strmap_get(&registry.names.map, name, len);
    return (CmdId){(u32)(uintptr_t)found};
}

CmdId yew_cmd_by_word(const char *word, u32 len)
{
    void *found;

    yew_cmd_init();
    if (word == NULL || len == 0U)
        return YEW_CMD_NONE;
    found = strmap_get(&registry.words, word, len);
    return (CmdId){(u32)(uintptr_t)found};
}

const CmdDesc *yew_cmd_desc(CmdId id)
{
    yew_cmd_init();
    if (id.v == 0U || (size_t)id.v > registry.len)
        return NULL;
    return &registry.entries[id.v - 1U].cmd;
}

const CmdEntry *yew_cmd_entry(CmdId id)
{
    yew_cmd_init();
    if (id.v == 0U || (size_t)id.v > registry.len)
        return NULL;
    return &registry.entries[id.v - 1U];
}

static bool args_valid(const CmdDesc *d, const CmdCtx *cx)
{
    if (cx->source < YEW_SRC_KEY || cx->source > YEW_SRC_TEST)
        return false;
    if (cx->count == 0U)
        return false;
    if (cx->sarg == NULL && cx->sarg_len != 0U)
        return false;
    switch ((CmdArity)d->arity) {
    case YEW_ARITY_NONE:
        return cx->sarg == NULL && cx->sarg_len == 0U;
    case YEW_ARITY_INT:
    case YEW_ARITY_OPT_INT:
        return cx->sarg == NULL && cx->sarg_len == 0U;
    case YEW_ARITY_STR:
        return cx->sarg != NULL;
    case YEW_ARITY_OPT_STR:
        return cx->iarg == 0;
    }
    return false;
}

static CmdStatus command_fail(const CmdDesc *d, const char *reason,
                              CmdStatus status)
{
    yew_log(YEW_LOG_ERROR, "command failed: %s: %s", d->name, reason);
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

static CmdStatus command_deferred(const CmdDesc *d, CmdCtx *cx)
{
    char number[16];
    const char *sprint = deferred_sprint(d, number, sizeof(number));

#if YEW_WITH_AI
    /* Sprint 48's off-by-default notice precedes even an AI command's
     * implementation-sprint refusal.  Keep the descriptor deferred so the
     * registry still enforces the no-stub invariant. */
    if (strncmp(d->name, "ed.ai.", 6U) == 0)
        (void)yew_ai_cmd_off(cx);
#else
    (void)cx;
#endif
#if !YEW_WITH_FUSS
    /* A stripped build explains how to obtain the module before it talks
     * about the later UI sprint.  Enabled builds retain the exact Sprint
     * 52/53 hard error until those implementations land. */
    if (strncmp(d->name, "ed.git.", 7U) == 0)
        return yew_git_cmd_require(cx);
#endif
    yew_log(YEW_LOG_ERROR,
            "command not implemented yet: %s lands in Sprint %s", d->name,
            sprint);
    return YEW_CMD_ERR_DEFERRED;
}

CmdStatus yew_cmd_prepare(CmdId id, CmdCtx *cx, const CmdDesc **out)
{
    const CmdDesc *d = yew_cmd_desc(id);

    if (out != NULL)
        *out = NULL;
    if (d == NULL || cx == NULL || out == NULL)
        return YEW_CMD_ERR_ARG;
    if ((d->flags & YEW_CMD_NEEDS_WIN) != 0U && cx->win == NULL)
        return command_fail(d, "no window", YEW_CMD_ERR_STATE);
    if ((d->flags & YEW_CMD_INTERNAL) != 0U &&
        cx->source == YEW_SRC_CMDLINE)
        return command_fail(d, "internal E command", YEW_CMD_ERR_ARG);
    if ((d->flags & YEW_CMD_DEFERRED) != 0U)
        return command_deferred(d, cx);
    if (!args_valid(d, cx))
        return command_fail(d, "invalid arguments", YEW_CMD_ERR_ARG);
    if (registry.record_tap != NULL &&
        (d->flags & YEW_CMD_RECORDABLE) != 0U) {
        CmdStatus recordable = yew_record_preflight(id, cx);

        if (recordable != YEW_CMD_OK)
            return recordable;
        registry.record_tap(id, cx);
    }
    *out = d;
    return YEW_CMD_OK;
}

CmdStatus yew_cmd_invoke(CmdId id, CmdCtx *cx)
{
    const CmdDesc *d;
    CmdStatus status;
    u32 n;
    u32 i;

    status = yew_cmd_prepare(id, cx, &d);
    if (status != YEW_CMD_OK)
        return status;
    n = (d->flags & YEW_CMD_REPEATABLE) != 0U ? cx->count : 1U;
    for (i = 0; i < n && status == YEW_CMD_OK; i++)
        status = d->fn(cx);
    return status;
}

u32 yew_cmd_count(void)
{
    yew_cmd_init();
    return (u32)registry.len;
}

const CmdDesc *yew_cmd_at(u32 i)
{
    yew_cmd_init();
    if ((size_t)i >= registry.len)
        return NULL;
    return &registry.entries[i].cmd;
}

void yew_cmd_set_record_tap(CmdRecordTap tap)
{
    yew_cmd_init();
    registry.record_tap = tap;
}
