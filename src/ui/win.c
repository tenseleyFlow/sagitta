#include "ui/win.h"

#include "edit/ed.h"

#include "ui/viewport.h"
#include "util/log.h"

LineNo sag_win_view_top(const Win *w)
{
    if (w == NULL)
        SAG_BUG("window viewport: missing window");
    return w->vp.top;
}

bool sag_win_view_row(const Win *w, LineNo line, u16 *row)
{
    if (w == NULL || row == NULL)
        SAG_BUG("window viewport row: missing argument");
    return sag_vp_row_of_line((Win *)w, line, 0U, row);
}

void sag_win_follow_cursor(Win *w)
{
    sag_vp_follow(w);
}

/*
 * Sprint 22 §7: a clicked CELL to a cursor position.
 *
 * The whole conversion goes through src/unicode/ — sag_vp_ccol_of_gridx
 * for the cell column and sag_ccol_to_off for the byte offset — because
 * that is the only place that knows a character can be two cells wide.
 * Doing the arithmetic here instead is the desync bug the layout/draw
 * split exists to prevent: a double-width glyph earlier on the line
 * would shift every click after it.
 */
void sag_win_click_to_cursor(Win *w, u16 grid_x, u16 grid_y)
{
    LineNo line;
    u32 sub = 0U;
    Span span;
    CCol col;
    Cursor *c;

    if (w == NULL || w->buf == NULL || w->buf->tb == NULL)
        return;
    if (grid_y < w->rect.y || grid_y >= (u32)w->rect.y + w->rect.h)
        return;
    if (!sag_vp_line_of_row(w, (u16)(grid_y - w->rect.y), &line, &sub))
        return;
    span = sag_textbuf_line_span(w->buf->tb, line);
    col = sag_vp_ccol_of_gridx(w, grid_x);
    if (w->cs.curs.len == 0U)
        return;
    c = &w->cs.curs.data[w->cs.primary];
    c->pos = sag_ccol_to_off(w->buf->tb, span, col,
                             w->buf->tabwidth == 0U ? SAG_VP_TABWIDTH
                                                    : w->buf->tabwidth);
    c->anchor = c->pos;
    c->goal_col = (GCol){0U};
}
