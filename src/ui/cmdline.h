#ifndef SAG_UI_CMDLINE_H
#define SAG_UI_CMDLINE_H

#include <stdbool.h>
#include <stddef.h>

#include "term/input.h"
#include "text/cursor.h"
#include "ui/cmdcomp.h"
#include "ui/cmdhist.h"
#include "ui/cmdparse.h"
#include "ui/menu.h"
#include "ui/win.h"

typedef struct Ed Ed;

typedef enum {
    SAG_PROMPT_CMD,
    SAG_PROMPT_SEARCH_F,
    SAG_PROMPT_SEARCH_B,
    SAG_PROMPT_INPUT
} SagPromptKind;

typedef struct CmdLine {
    SagPromptKind kind;
    bool active;
    TextBuf *buf;
    Cursor cur;
    Menu menu;
    Arena comp_arena;
    /* §4: the cached candidate set `comp_arena` backs. */
    CompFilter filter;
    HistCur hist;
    CmdErr err;
    /* §9: what the parser currently understands.  Empty when it
     * understands nothing -- silence, never a guess. */
    char hint[160];
    u16 scroll;

    /* §8: the menu row a click last selected, so a second click on the
     * same row accepts it.  -1 when none.  A row index rather than a
     * timer keeps it deterministic for the goldens. */
    i32 click_row;
    void *target;
    CmdHist *history;
    u8 return_mode;
    u32 comp_total;
    char *menu_stem;
    Span menu_original;
} CmdLine;

void sag_cmdline_open(Ed *ed, SagPromptKind kind, const char *seed);
void sag_cmdline_close(Ed *ed, bool accepted);
void sag_cmdline_dispose(Ed *ed);
bool sag_cmdline_key(Ed *ed, const Key *key);
void sag_cmdline_paste(Ed *ed, const u8 *bytes, size_t len);
void sag_cmdline_draw(Ed *ed, Rect rect);
Win *sag_cmdline_target(Ed *ed);
void sag_cmdline_sync(Ed *ed);
/* Appends the prompt's current text to `out`. */
void sag_cmdline_text(Ed *ed, Bytebuf *out);
void sag_cmdline_edited(Ed *ed);

/*
 * Continue a sliced completion scan on the idle path; true while more
 * remains.  The loop calls this beside sag_picker_tick, after input is
 * drained, for the reason given there.
 */
bool sag_cmdline_comp_tick(Ed *ed);

CmdStatus sag_cmdline_cmd_hist_prev(CmdCtx *cx);
CmdStatus sag_cmdline_cmd_hist_next(CmdCtx *cx);
CmdStatus sag_cmdline_cmd_complete_next(CmdCtx *cx);
CmdStatus sag_cmdline_cmd_complete_prev(CmdCtx *cx);
CmdStatus sag_cmdline_cmd_insert_register(CmdCtx *cx);
CmdStatus sag_cmdline_cmd_literal_next(CmdCtx *cx);
/* §8: click selects, a second click on the same row accepts. */
bool sag_cmdline_menu_click(Ed *ed, i32 row);
bool sag_cmdline_menu_scroll(Ed *ed, i32 delta);

CmdStatus sag_cmdline_cmd_menu_page_next(CmdCtx *cx);
CmdStatus sag_cmdline_cmd_menu_page_prev(CmdCtx *cx);
CmdStatus sag_cmdline_cmd_menu_accept(CmdCtx *cx);
CmdStatus sag_cmdline_cmd_menu_dismiss(CmdCtx *cx);
CmdStatus sag_cmdline_cmd_ghost_accept(CmdCtx *cx);
CmdStatus sag_cmdline_cmd_accept(CmdCtx *cx);
CmdStatus sag_cmdline_cmd_cancel(CmdCtx *cx);
CmdStatus sag_cmdline_cmd_delete_word_prev(CmdCtx *cx);
CmdStatus sag_cmdline_cmd_delete_to_home(CmdCtx *cx);
CmdStatus sag_cmdline_cmd_delete_to_end(CmdCtx *cx);

#endif
