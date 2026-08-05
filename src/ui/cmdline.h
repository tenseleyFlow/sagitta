#ifndef SAG_UI_CMDLINE_H
#define SAG_UI_CMDLINE_H

#include <stdbool.h>
#include <stddef.h>

#include "term/input.h"
#include "text/cursor.h"
#include "ui/cmdcomp.h"
#include "ui/cmdhist.h"
#include "ui/cmdparse.h"
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
    CompMenu menu;
    Arena comp_arena;
    /* §4: the cached candidate set `comp_arena` backs. */
    CompFilter filter;
    HistCur hist;
    CmdErr err;
    u16 scroll;

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

CmdStatus sag_cmdline_cmd_hist_prev(CmdCtx *cx);
CmdStatus sag_cmdline_cmd_hist_next(CmdCtx *cx);
CmdStatus sag_cmdline_cmd_complete_next(CmdCtx *cx);
CmdStatus sag_cmdline_cmd_complete_prev(CmdCtx *cx);
CmdStatus sag_cmdline_cmd_insert_register(CmdCtx *cx);
CmdStatus sag_cmdline_cmd_literal_next(CmdCtx *cx);
CmdStatus sag_cmdline_cmd_accept(CmdCtx *cx);
CmdStatus sag_cmdline_cmd_cancel(CmdCtx *cx);
CmdStatus sag_cmdline_cmd_delete_word_prev(CmdCtx *cx);
CmdStatus sag_cmdline_cmd_delete_to_home(CmdCtx *cx);
CmdStatus sag_cmdline_cmd_delete_to_end(CmdCtx *cx);

#endif
