#ifndef YEW_UI_CMDLINE_H
#define YEW_UI_CMDLINE_H

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
    YEW_PROMPT_CMD,
    YEW_PROMPT_SEARCH_F,
    YEW_PROMPT_SEARCH_B,
    YEW_PROMPT_INPUT
} YewPromptKind;

/* `text` is valid only for the duration of the callback.  The callback runs
 * after the prompt has closed and the editor mode has been restored. */
typedef void (*YewCmdlineInputDone)(Ed *ed, bool accepted,
                                    const u8 *text, size_t len, void *ctx);

typedef struct CmdLine {
    YewPromptKind kind;
    bool active;
    /* Monotonic identity for one prompt lifetime.  The CmdLine storage is
     * embedded in Ed and reused, so pointer identity cannot distinguish a
     * prompt that a command replaced while Enter was being handled. */
    u64 generation;
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
    YewCmdlineInputDone input_done;
    void *input_ctx;
} CmdLine;

void yew_cmdline_open(Ed *ed, YewPromptKind kind, const char *seed);
void yew_cmdline_open_input(Ed *ed, const char *seed,
                            YewCmdlineInputDone done, void *ctx);
void yew_cmdline_close(Ed *ed, bool accepted);
void yew_cmdline_dispose(Ed *ed);
bool yew_cmdline_key(Ed *ed, const Key *key);
void yew_cmdline_paste(Ed *ed, const u8 *bytes, size_t len);
void yew_cmdline_draw(Ed *ed, Rect rect);
Win *yew_cmdline_target(Ed *ed);
void yew_cmdline_sync(Ed *ed);
/* Appends the prompt's current text to `out`. */
void yew_cmdline_text(Ed *ed, Bytebuf *out);
void yew_cmdline_edited(Ed *ed);

/*
 * Continue a sliced completion scan on the idle path; true while more
 * remains.  The loop calls this beside yew_picker_tick, after input is
 * drained, for the reason given there.
 *
 * `scanning` is the same condition the tick acts on, exposed so
 * yew_loop_deadline can refuse to sleep on exactly it.  Use it rather
 * than re-deriving the test — the two drifting apart is a busy loop in
 * one direction and a stalled menu in the other.
 */
bool yew_cmdline_comp_tick(Ed *ed);
bool yew_cmdline_comp_scanning(const Ed *ed);

CmdStatus yew_cmdline_cmd_hist_prev(CmdCtx *cx);
CmdStatus yew_cmdline_cmd_hist_next(CmdCtx *cx);
CmdStatus yew_cmdline_cmd_complete_next(CmdCtx *cx);
CmdStatus yew_cmdline_cmd_complete_prev(CmdCtx *cx);
CmdStatus yew_cmdline_cmd_insert_register(CmdCtx *cx);
CmdStatus yew_cmdline_cmd_literal_next(CmdCtx *cx);
/* §8: click selects, a second click on the same row accepts. */
bool yew_cmdline_menu_click(Ed *ed, i32 row);
bool yew_cmdline_menu_scroll(Ed *ed, i32 delta);

CmdStatus yew_cmdline_cmd_menu_page_next(CmdCtx *cx);
CmdStatus yew_cmdline_cmd_menu_page_prev(CmdCtx *cx);
CmdStatus yew_cmdline_cmd_menu_accept(CmdCtx *cx);
CmdStatus yew_cmdline_cmd_menu_dismiss(CmdCtx *cx);
CmdStatus yew_cmdline_cmd_ghost_accept(CmdCtx *cx);
CmdStatus yew_cmdline_cmd_accept(CmdCtx *cx);
CmdStatus yew_cmdline_cmd_cancel(CmdCtx *cx);
CmdStatus yew_cmdline_cmd_delete_word_prev(CmdCtx *cx);
CmdStatus yew_cmdline_cmd_delete_to_home(CmdCtx *cx);
CmdStatus yew_cmdline_cmd_delete_to_end(CmdCtx *cx);

#endif
