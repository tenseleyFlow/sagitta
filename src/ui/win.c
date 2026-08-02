#include "ui/win.h"

#include "edit/ed.h"
#include "util/log.h"

LineNo sag_win_view_top(const Win *w)
{
    if (w == NULL)
        SAG_BUG("window viewport: missing window");
    return w->vp.top;
}

bool sag_win_view_row(const Win *w, LineNo line, u16 *row)
{
    u64 relative;

    if (w == NULL || row == NULL)
        SAG_BUG("window viewport row: missing argument");
    if (line.v < w->vp.top.v)
        return false;
    relative = line.v - w->vp.top.v;
    if (relative >= w->rect.h)
        return false;
    *row = (u16)relative;
    return true;
}

void sag_win_follow_cursor(Win *w)
{
    const Cursor *cursor;
    LineNo line;
    u64 bottom;

    if (w == NULL || w->buf == NULL || w->buf->tb == NULL)
        SAG_BUG("window follow: missing window buffer");
    if (w->cs.curs.len == 0U || (size_t)w->cs.primary >= w->cs.curs.len)
        SAG_BUG("window follow: missing primary cursor");

    cursor = &w->cs.curs.data[w->cs.primary];
    line = sag_textbuf_line_of(w->buf->tb, cursor->pos);
    if (w->vp.rows == 0U) {
        w->vp.top = line;
        return;
    }
    if (line.v < w->vp.top.v) {
        w->vp.top = line;
        return;
    }
    bottom = w->vp.top.v + (u64)w->vp.rows;
    if (bottom < w->vp.top.v)
        bottom = UINT64_MAX;
    if (line.v >= bottom)
        w->vp.top = LINENO(line.v - (u64)w->vp.rows + 1U);
}

void sag_ed_layout(Ed *ed)
{
    Win *w;
    u16 rows;

    if (ed == NULL || ed->win == NULL)
        SAG_BUG("editor layout: missing window");
    w = ed->win;
    rows = ed->grid.rows == 0U ? 0U : (u16)(ed->grid.rows - 1U);
    w->rect = (Rect){0U, 0U, ed->grid.cols, rows};
    w->vp.rows = rows;
    w->vp.cols = ed->grid.cols;
    sag_win_follow_cursor(w);
    ed->layout_dirty = false;
}
