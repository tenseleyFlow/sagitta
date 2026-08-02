#include "ui/win.h"

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
