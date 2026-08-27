#ifndef YEW_UI_SHADOWDRAW_H
#define YEW_UI_SHADOWDRAW_H

#include <stdbool.h>

#include "ui/layout.h"
#include "util/base.h"

typedef struct Ed Ed;
typedef struct Grid Grid;
typedef struct Shadow Shadow;
typedef struct Win Win;

#define YEW_SHADOW_MAX_LINES 8U

typedef struct ShadowLayout {
    Rect inline_run;
    Rect vrows;
    u64 logical_col;
    u16 nlines;
    bool clipped;
} ShadowLayout;

void yew_shadow_layout(const Win *win, const Shadow *shadow,
                       ShadowLayout *out);
void yew_shadow_draw(Ed *ed, Win *win, const ShadowLayout *layout,
                     Grid *grid);
void yew_shadow_draw_panes(Ed *ed);

#endif
