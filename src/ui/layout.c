#include "ui/layout.h"

#include "edit/ed.h"
#include "ui/gutter.h"
#include "ui/viewport.h"
#include "ui/win.h"
#include "util/log.h"

void sag_layout(Ed *ed)
{
    Win *w;
    u16 content_rows;
    u16 gutter;
    u16 old_cols;
    u16 old_gutter;

    if (ed == NULL || ed->win == NULL)
        SAG_BUG("editor layout: missing window");
    w = ed->win;
    old_cols = w->vp.cols;
    old_gutter = w->gutter_width;

    ed->footer_rect = (Rect){0U, 0U, 0U, 0U};
    if (ed->grid.rows >= 2U) {
        content_rows = (u16)(ed->grid.rows - 1U);
        ed->footer_rect = (Rect){0U, content_rows, ed->grid.cols, 1U};
    } else {
        content_rows = ed->grid.rows;
    }

    gutter = ed->grid.cols < 20U ? 0U : sag_gutter_width(w);
    if (gutter >= ed->grid.cols)
        gutter = 0U;
    w->gutter_width = gutter;
    w->rect = (Rect){gutter, 0U, (u16)(ed->grid.cols - gutter),
                     content_rows};
    w->vp.rows = content_rows;
    w->vp.cols = w->rect.w;

    if (old_cols != w->vp.cols) {
        sag_vp_invalidate(w);
        ed->full_damage = true;
        ed->footer_dirty = true;
    }
    if (old_gutter != gutter) {
        ed->full_damage = true;
        ed->footer_dirty = true;
    }
    sag_vp_clamp(w);
    sag_vp_follow(w);
    ed->layout_dirty = false;
}

void sag_ed_layout(Ed *ed)
{
    sag_layout(ed);
}
