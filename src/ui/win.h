#ifndef SAG_UI_WIN_H
#define SAG_UI_WIN_H

#include "edit/motion.h"
#include "edit/jumplist.h"
#include "edit/multicursor.h"
#include "edit/select.h"
#include "text/coords.h"
#include "ui/gutter.h"
#include "ui/viewport.h"
#include "unicode/coords.h"
#include "util/base.h"

typedef struct Buffer Buffer;

/* Screen-cell geometry.  Sprint 22 moves this unchanged to layout.h. */
typedef struct Rect {
    u16 x;
    u16 y;
    u16 w;
    u16 h;
} Rect;

typedef struct Win {
    Buffer *buf;
    CursorSet cs;
    HState h;
    Viewport vp;
    /* Navigation history of this VIEW; two panes on one file keep
     * separate ones (Sprint 21 §5). */
    JumpList jumps;
    WrapCache wrap_cache;
    Rect rect;
    NumStyle number_style;
    u16 gutter_width;
    CCol wrap_goal;
    bool wrap_goal_valid;
} Win;

/* Sprint 14 compatibility names; new code uses the sag_vp_* API. */
void sag_win_follow_cursor(Win *w);
LineNo sag_win_view_top(const Win *w);
bool sag_win_view_row(const Win *w, LineNo line, u16 *row);

#endif
