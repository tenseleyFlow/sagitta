#ifndef SAG_UI_STATUSLINE_H
#define SAG_UI_STATUSLINE_H

#include <stddef.h>

#include "edit/mode.h"
#include "term/grid.h"
#include "util/base.h"

typedef struct Ed Ed;
typedef struct Win Win;

typedef struct SagUiStyle {
    SagColor chip_fg;
    SagColor chip_bg;
    SagColor row_fg;
    SagColor row_bg;
    u16 attrs;
} SagUiStyle;

typedef struct StatuslineText {
    char chip[16];
    size_t chip_len;
    char recording[32];
    size_t recording_len;
    char *body;
    size_t body_cap;
    size_t body_len;
    u16 chip_cells;
    u16 recording_cells;
    u16 body_cells;
    size_t warn_at;
    size_t warn_len;
} StatuslineText;

SagUiStyle sag_statusline_mode_style(Mode mode);
void sag_statusline_build(const Ed *ed, Win *w, u16 cols,
                          StatuslineText *out);
void sag_statusline_text_free(StatuslineText *text);
void sag_statusline_draw(Ed *ed, Win *w);

#endif
