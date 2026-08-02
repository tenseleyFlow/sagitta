#ifndef SAG_UI_WIN_H
#define SAG_UI_WIN_H

#include "edit/multicursor.h"
#include "text/coords.h"
#include "util/base.h"

typedef struct Buffer Buffer;

/* Screen-cell geometry.  Sprint 22 moves this unchanged to layout.h. */
typedef struct Rect {
    u16 x;
    u16 y;
    u16 w;
    u16 h;
} Rect;

/* Sprint 15 replaces this placeholder with the scrolling viewport. */
typedef struct Viewport {
    LineNo top;
    u16 rows;
    u16 cols;
} Viewport;

typedef struct Win {
    Buffer *buf;
    CursorSet cs;
    Viewport vp;
    Rect rect;
} Win;

void sag_win_follow_cursor(Win *w);
LineNo sag_win_view_top(const Win *w);
bool sag_win_view_row(const Win *w, LineNo line, u16 *row);

#endif
