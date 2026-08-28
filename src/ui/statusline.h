#ifndef YEW_UI_STATUSLINE_H
#define YEW_UI_STATUSLINE_H

#include <stddef.h>

#include "edit/mode.h"
#include "term/grid.h"
#include "util/base.h"

typedef struct Ed Ed;
typedef struct Win Win;

enum { YEW_STATUSLINE_INLINE_CAP = 4096 };

typedef struct YewUiStyle {
    YewColor chip_fg;
    YewColor chip_bg;
    YewColor row_fg;
    YewColor row_bg;
    u16 attrs;
} YewUiStyle;

typedef struct StatuslineText {
    char chip[16];
    size_t chip_len;
    char recording[32];
    size_t recording_len;
    char body_inline[YEW_STATUSLINE_INLINE_CAP];
    char *body;
    size_t body_cap;
    size_t body_len;
    bool body_owned;
    u16 chip_cells;
    u16 recording_cells;
    u16 body_cells;
    size_t warn_at;
    size_t warn_len;
    size_t diag_error_at;
    size_t diag_error_len;
    size_t diag_warn_at;
    size_t diag_warn_len;
} StatuslineText;

YewUiStyle yew_statusline_mode_style(Mode mode);
void yew_statusline_build(const Ed *ed, Win *w, u16 cols,
                          StatuslineText *out);
void yew_statusline_text_free(StatuslineText *text);
void yew_statusline_draw(Ed *ed, Win *w);

#endif
