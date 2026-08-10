#ifndef YEW_UI_VIEWPORT_H
#define YEW_UI_VIEWPORT_H

#include <stddef.h>

#include "text/coords.h"
#include "text/piece.h"
#include "unicode/coords.h"
#include "util/base.h"

typedef struct Win Win;

enum {
    YEW_VP_TABWIDTH = 4,
    YEW_VP_WRAP_SLACK = 64
};

typedef struct Viewport {
    LineNo top;
    u32 top_sub;
    CCol left;
    u16 rows;
    u16 cols;
    u8 scrolloff;
    u8 sidescrolloff;
    bool wrap;
} Viewport;

typedef struct WrapCache {
    u32 *rows;
    size_t len;
    size_t cap;
    LineNo first;
    u16 cols;
    u32 tabwidth;
    u64 generation;
    bool valid;
    Span *spans;
    size_t spans_len;
    size_t spans_cap;
    LineNo spans_line;
    u32 spans_first;
    u16 spans_cols;
    u64 spans_generation;
    bool spans_valid;
} WrapCache;

void yew_vp_init(Win *w);
void yew_vp_free(Win *w);
void yew_vp_invalidate(Win *w);
void yew_vp_invalidate_from(Win *w, LineNo line);

u32 yew_wrap_rows(Win *w, LineNo line);
Span yew_wrap_row(Win *w, LineNo line, u32 sub);

bool yew_vp_row_of_line(Win *w, LineNo line, u32 sub, u16 *row);
bool yew_vp_line_of_row(Win *w, u16 row, LineNo *line, u32 *sub);
u16 yew_vp_gridx_of_ccol(const Win *w, CCol col);
CCol yew_vp_ccol_of_gridx(const Win *w, u16 grid_x);
u32 yew_vp_cursor_subrow(Win *w);

void yew_vp_follow(Win *w);
void yew_vp_scroll(Win *w, i32 rows);
void yew_vp_push_cursor(Win *w);
void yew_vp_page(Win *w, i32 pages);
void yew_vp_center(Win *w);
void yew_vp_top(Win *w);
void yew_vp_bottom(Win *w);
void yew_vp_clamp(Win *w);
bool yew_vp_move_display(Win *w, i32 rows);
/* Pure counterparts used by the line unit engine.  They perform no cache,
 * cursor, or viewport writes. */
CCol yew_vp_display_col(const Win *w, ByteOff pos);
ByteOff yew_vp_display_target(const Win *w, ByteOff pos, i32 rows);
LineNo yew_vp_last_visible_line(Win *w);

#endif
