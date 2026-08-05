#ifndef SAG_UI_WIN_H
#define SAG_UI_WIN_H

#include "edit/motion.h"
#include "edit/jumplist.h"
/* Sprint 22 moved Rect here, as this file said it would. */
#include "ui/layout.h"
#include "search/overlay.h"
#include "edit/multicursor.h"
#include "edit/select.h"
#include "text/coords.h"
#include "ui/gutter.h"
#include "ui/viewport.h"
#include "unicode/coords.h"
#include "util/base.h"

typedef struct Buffer Buffer;

typedef struct Win {
    Buffer *buf;
    CursorSet cs;
    HState h;
    Viewport vp;
    /* Navigation history of this VIEW; two panes on one file keep
     * separate ones (Sprint 21 §5). */
    JumpList jumps;
    /* Sprint 21 $3: match highlighting for THIS view. */
    MatchOverlay overlay;
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

/* Sprint 22 §7: click-to-focus lands the cursor on the clicked
 * grapheme.  Conversions go through src/unicode/, never here. */
void sag_win_click_to_cursor(Win *w, u16 grid_x, u16 grid_y);

#endif
