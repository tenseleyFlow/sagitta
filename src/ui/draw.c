#include "ui/draw.h"

#include "ui/grouppicker.h"

#include "ui/tabs.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include "edit/ed.h"
#include "edit/select.h"
#include "term/grid.h"
#include "ui/gutter.h"
#include "ui/message.h"
#include "ui/statusline.h"
#include "ui/viewport.h"
#include "ui/win.h"
#include "unicode/coords.h"
#include "unicode/width.h"
#include "util/log.h"

static SagColor default_color(void)
{
    SagColor color = {0U, 0U, 0U, 0U};

    return color;
}

static u8 text_byte_at(const TextBuf *tb, u64 off)
{
    TextIter it;
    const u8 *bytes;
    u64 len;

    if (!sag_textiter_begin(&it, tb, BYTEOFF(off)) ||
        !sag_textiter_chunk(&it, tb, &bytes, &len) || len == 0U)
        SAG_BUG("draw: cannot read buffer byte");
    return bytes[0];
}

static Span line_content_span(const TextBuf *tb, LineNo line)
{
    Span span = sag_textbuf_line_span(tb, line);

    if (span.lo == span.hi || text_byte_at(tb, span.hi - 1U) != '\n')
        return span;
    span.hi--;
    if (span.hi > span.lo && text_byte_at(tb, span.hi - 1U) == '\r')
        span.hi--;
    return span;
}

static void text_copy(const TextBuf *tb, u64 lo, u64 hi, u8 *dst)
{
    TextIter it;
    u64 copied = 0U;

    if (lo == hi)
        return;
    if (!sag_textiter_begin(&it, tb, BYTEOFF(lo)))
        SAG_BUG("draw: cannot begin buffer span");
    while (lo + copied < hi) {
        const u8 *bytes;
        u64 len;
        u64 remain = hi - lo - copied;
        u64 take;

        if (!sag_textiter_chunk(&it, tb, &bytes, &len) || len == 0U)
            SAG_BUG("draw: buffer span ended early");
        take = len < remain ? len : remain;
        if (take > (u64)SIZE_MAX)
            SAG_BUG("draw: cluster exceeds address space");
        (void)memcpy(dst + (size_t)copied, bytes, (size_t)take);
        copied += take;
        if (copied < hi - lo && !sag_textiter_advance(&it, tb))
            SAG_BUG("draw: buffer span ended early");
    }
}

static u16 put_spaces(Grid *grid, u16 row, u16 col, u16 end)
{
    if (col < end)
        sag_grid_fill(grid, row, col, end, grid->blank);
    return end;
}

static u16 row_right(const Grid *grid, const Win *w)
{
    u32 right = (u32)w->rect.x + w->rect.w;

    return right > grid->cols ? grid->cols : (u16)right;
}

static u32 draw_tabwidth(const Win *w)
{
    return w->buf->tabwidth == 0U ? SAG_VP_TABWIDTH : w->buf->tabwidth;
}

static u64 signature_mix(u64 hash, u64 value)
{
    hash ^= value;
    hash *= UINT64_C(1099511628211);
    return hash;
}

static u64 cursor_overlay_signature(const Ed *ed, const Win *w)
{
    u64 hash = UINT64_C(1469598103934665603);
    size_t i;

    hash = signature_mix(hash, w->cs.curs.len);
    hash = signature_mix(hash, w->cs.primary);
    hash = signature_mix(hash, (u64)ed->mode);
    hash = signature_mix(hash, (u64)ed->render.tier);
    hash = signature_mix(hash, draw_tabwidth(w));
    hash = signature_mix(hash, w->vp.top.v);
    hash = signature_mix(hash, w->vp.top_sub);
    hash = signature_mix(hash, w->vp.left.v);
    hash = signature_mix(hash, w->vp.wrap ? 1U : 0U);
    hash = signature_mix(hash, w->rect.x);
    hash = signature_mix(hash, w->rect.y);
    hash = signature_mix(hash, w->rect.w);
    hash = signature_mix(hash, w->rect.h);
    for (i = 0U; i < w->cs.curs.len; i++) {
        if (i == (size_t)w->cs.primary)
            continue;
        hash = signature_mix(hash, w->cs.curs.data[i].pos.v);
        hash = signature_mix(hash, w->cs.curs.data[i].anchor.v);
    }
    return hash;
}

static void redraw_primary_selection_change(Ed *ed, Win *w,
                                            const Cursor *cursor)
{
    LineNo first;
    LineNo last;
    LineNo old_pos;
    LineNo old_anchor;
    LineNo new_pos;
    LineNo new_anchor;
    u16 lo = w->rect.h;
    u16 hi = 0U;
    u16 row;

    old_pos = sag_textbuf_line_of(w->buf->tb,
                                  BYTEOFF(ed->grid.cursor_overlay_primary_pos));
    old_anchor = sag_textbuf_line_of(w->buf->tb,
                                     BYTEOFF(ed->grid.cursor_overlay_primary_anchor));
    new_pos = sag_textbuf_line_of(w->buf->tb, cursor->pos);
    new_anchor = sag_textbuf_line_of(w->buf->tb, cursor->anchor);
    first = old_pos.v < old_anchor.v ? old_pos : old_anchor;
    if (new_pos.v < first.v)
        first = new_pos;
    if (new_anchor.v < first.v)
        first = new_anchor;
    last = old_pos.v > old_anchor.v ? old_pos : old_anchor;
    if (new_pos.v > last.v)
        last = new_pos;
    if (new_anchor.v > last.v)
        last = new_anchor;
    for (row = 0U; row < w->rect.h; row++) {
        LineNo line;
        u32 sub;

        if (!sag_vp_line_of_row(w, row, &line, &sub) || line.v < first.v ||
            line.v > last.v)
            continue;
        if (lo == w->rect.h)
            lo = row;
        hi = (u16)(row + 1U);
    }
    if (lo < hi)
        sag_draw_document_rows(ed, w, lo, hi);
}

static u16 grid_x(const Win *w, CCol col)
{
    u64 left = w->vp.wrap ? 0U : w->vp.left.v;
    u64 relative = col.v > left ? col.v - left : 0U;
    u64 x = (u64)w->rect.x + w->gutter_width + relative;

    return x > UINT16_MAX ? UINT16_MAX : (u16)x;
}

static void overlay_span(Grid *grid, const Win *w, u16 row,
                         Span displayed, Span selected, const Cell *style,
                         u8 fields)
{
    TextBuf *tb = w->buf->tb;
    u64 lo = selected.lo > displayed.lo ? selected.lo : displayed.lo;
    u64 hi = selected.hi < displayed.hi ? selected.hi : displayed.hi;
    CCol c0;
    CCol c1;
    u16 x0;
    u16 x1;
    u16 right;

    if (lo >= hi)
        return;
    c0 = sag_off_to_ccol(tb, displayed, BYTEOFF(lo), draw_tabwidth(w));
    c1 = sag_off_to_ccol(tb, displayed, BYTEOFF(hi), draw_tabwidth(w));
    x0 = grid_x(w, c0);
    x1 = grid_x(w, c1);
    right = row_right(grid, w);
    if (x0 < (u16)(w->rect.x + w->gutter_width))
        x0 = (u16)(w->rect.x + w->gutter_width);
    if (x1 > right)
        x1 = right;
    sag_grid_overlay(grid, row, x0, x1, style, fields);
}

static void draw_selection_rows(Ed *ed, Win *w, u16 lo, u16 hi)
{
    static const SagColor selected_bg = {
        SAG_COLOR_RGB, 52U, 72U, 108U
    };
    Cell style = ed->grid.blank;
    size_t i;

    if (ed->mode != SAG_MODE_H)
        return;
    style.bg = selected_bg;
    for (i = 0U; i < w->cs.curs.len; i++) {
        const Cursor *cursor = &w->cs.curs.data[i];
        SagSelSpanVec rect_spans = {0};
        LineNo rect_first = {0U};
        Span selected = {0U, 0U};
        u16 screen_row;

        if (cursor->pos.v == cursor->anchor.v)
            continue;
        if (w->h.kind == SAG_SEL_RECT) {
            LineNo pos_line = sag_textbuf_line_of(w->buf->tb, cursor->pos);
            LineNo anchor_line = sag_textbuf_line_of(w->buf->tb,
                                                     cursor->anchor);

            rect_first = pos_line.v < anchor_line.v ? pos_line : anchor_line;
            sag_sel_rect_spans(w, cursor, &rect_spans);
        } else {
            selected = sag_sel_span(w, cursor);
        }
        for (screen_row = lo;
             screen_row < hi && screen_row < w->rect.h; screen_row++) {
            LineNo line;
            u32 sub;
            Span displayed;

            if (!sag_vp_line_of_row(w, screen_row, &line, &sub))
                continue;
            displayed = w->vp.wrap ? sag_wrap_row(w, line, sub) :
                                     line_content_span(w->buf->tb, line);
            if (w->h.kind == SAG_SEL_RECT) {
                u64 index;

                if (line.v < rect_first.v)
                    continue;
                index = line.v - rect_first.v;
                if (index >= rect_spans.len)
                    continue;
                selected = rect_spans.data[index];
            }
            overlay_span(&ed->grid, w, (u16)(w->rect.y + screen_row),
                         displayed, selected, &style, SAG_OVERLAY_BG);
        }
        SagSelSpanVec_free(&rect_spans);
    }
}

static void draw_secondary_rows(Ed *ed, Win *w, u16 lo, u16 hi)
{
    Cell style = ed->grid.blank;
    size_t i;

    if (ed->render.tier == SAG_RENDER_TIER_16) {
        style.attrs = SAG_ATTR_REVERSE;
    } else {
        style.fg = (SagColor){SAG_COLOR_RGB, 250U, 252U, 255U};
        style.bg = (SagColor){SAG_COLOR_RGB, 104U, 139U, 214U};
    }
    for (i = 0U; i < w->cs.curs.len; i++) {
        const Cursor *cursor;
        LineNo line;
        u32 sub;
        u32 count;
        u16 screen_row;
        Span displayed;
        CCol col;
        u16 x;
        u8 fields;

        if (i == (size_t)w->cs.primary)
            continue;
        cursor = &w->cs.curs.data[i];
        line = sag_textbuf_line_of(w->buf->tb, cursor->pos);
        count = w->vp.wrap ? sag_wrap_rows(w, line) : 1U;
        for (sub = 0U; sub < count; sub++) {
            displayed = w->vp.wrap ? sag_wrap_row(w, line, sub) :
                                     line_content_span(w->buf->tb, line);
            if (cursor->pos.v >= displayed.lo &&
                (cursor->pos.v < displayed.hi || sub + 1U == count))
                break;
        }
        if (sub == count || !sag_vp_row_of_line(w, line, sub, &screen_row) ||
            screen_row < lo || screen_row >= hi)
            continue;
        col = sag_off_to_ccol(w->buf->tb, displayed, cursor->pos,
                              draw_tabwidth(w));
        x = grid_x(w, col);
        if ((u32)w->rect.y + screen_row >= ed->grid.rows ||
            x < (u16)(w->rect.x + w->gutter_width) ||
            x >= row_right(&ed->grid, w))
            continue;
        fields = ed->render.tier == SAG_RENDER_TIER_16 ? SAG_OVERLAY_ATTRS :
                 (u8)(SAG_OVERLAY_FG | SAG_OVERLAY_BG);
        sag_grid_overlay(&ed->grid, (u16)(w->rect.y + screen_row), x,
                         (u16)(x + 1U), &style, fields);
    }
}

static void draw_span(Grid *grid, const TextBuf *tb, Span span,
                      u16 row, const Win *w, CCol left)
{
    SagColor color = default_color();
    u32 tabwidth = draw_tabwidth(w);
    ByteOff start = sag_ccol_to_off(tb, span, left, tabwidth);
    CCol logical = sag_off_to_ccol(tb, span, start, tabwidth);
    u64 pos = start.v;
    u16 col = w->rect.x;
    u16 right = row_right(grid, w);

    while (pos < span.hi && col < right) {
        ByteOff next_off = sag_grapheme_next_boundary(tb, BYTEOFF(pos));
        u64 next = next_off.v;
        u64 n;
        u64 cells;
        u8 local[64];
        u8 *cluster = local;

        if (next <= pos || next > span.hi)
            SAG_BUG("draw: invalid grapheme boundary");
        n = next - pos;
        if (n > sizeof(local)) {
            if (n > (u64)SIZE_MAX)
                SAG_BUG("draw: cluster exceeds address space");
            cluster = sag_xmalloc((size_t)n);
        }
        text_copy(tb, pos, next, cluster);
        if (n == 1U && cluster[0] == '\t') {
            cells = sag_tab_cells(logical, tabwidth);
        } else {
            int measured = sag_cluster_width(cluster, (size_t)n);

            cells = measured > 0 ? (u64)measured : 0U;
        }

        if (logical.v >= left.v) {
            u64 relative = logical.v - left.v;
            u64 target64 = (u64)w->rect.x + relative;
            u16 target = target64 > right ? right : (u16)target64;

            col = put_spaces(grid, row, col, target);
            if (n == 1U && cluster[0] == '\t') {
                u64 stop64 = (u64)col + cells;
                u16 stop = stop64 > right ? right : (u16)stop64;

                col = put_spaces(grid, row, col, stop);
            } else {
                col = sag_grid_put(grid, row, col, cluster, (size_t)n,
                                   color, color, 0U);
            }
        }
        logical.v = cells > UINT64_MAX - logical.v ? UINT64_MAX :
                                                          logical.v + cells;
        if (cluster != local)
            free(cluster);
        pos = next;
    }
    (void)put_spaces(grid, row, col, right);
}

void sag_draw_document_rows(Ed *ed, Win *w, u16 lo, u16 hi)
{
    Grid *grid = &ed->grid;
    TextBuf *tb = w->buf->tb;
    u64 line_count = sag_textbuf_line_count(tb);
    u16 screen_row;

    if (hi > w->rect.h)
        hi = w->rect.h;
    if (lo > hi)
        lo = hi;
    for (screen_row = lo; screen_row < hi; screen_row++) {
        u32 row32 = (u32)w->rect.y + screen_row;
        LineNo line;
        u32 sub;

        if (row32 >= grid->rows)
            break;
        if (sag_vp_line_of_row(w, screen_row, &line, &sub) &&
            line.v < line_count) {
            Span span = w->vp.wrap ? sag_wrap_row(w, line, sub) :
                                     line_content_span(tb, line);

            draw_span(grid, tb, span, (u16)row32, w,
                      w->vp.wrap ? (CCol){0U} : w->vp.left);
        } else {
            (void)put_spaces(grid, (u16)row32, w->rect.x,
                             row_right(grid, w));
        }
    }
    sag_gutter_draw(ed, w, lo, hi);
    draw_selection_rows(ed, w, lo, hi);
    draw_secondary_rows(ed, w, lo, hi);
    if (lo == 0U && hi == w->rect.h) {
        grid->cursor_overlay_signature = cursor_overlay_signature(ed, w);
        grid->cursor_overlay_valid = true;
        if (w->cs.curs.len != 0U &&
            (size_t)w->cs.primary < w->cs.curs.len) {
            grid->cursor_overlay_primary_pos =
                w->cs.curs.data[w->cs.primary].pos.v;
            grid->cursor_overlay_primary_anchor =
                w->cs.curs.data[w->cs.primary].anchor.v;
            grid->cursor_overlay_primary_valid = true;
        } else {
            grid->cursor_overlay_primary_valid = false;
        }
    }
}

void sag_draw_cursor(Ed *ed, Win *w)
{
    const Cursor *cursor;
    TextBuf *tb = w->buf->tb;
    LineNo line;
    Span span;
    CCol col;
    u32 sub;
    u16 row;
    u16 screen_col;

    sag_grid_cursor_shape(&ed->grid, ed->mode == SAG_MODE_I ?
                          SAG_CURSOR_BAR : SAG_CURSOR_BLOCK);
    if (!ed->grid.cursor_overlay_valid ||
        ed->grid.cursor_overlay_signature != cursor_overlay_signature(ed, w))
        sag_draw_document_rows(ed, w, 0U, w->rect.h);
    if (w->cs.curs.len == 0U || (size_t)w->cs.primary >= w->cs.curs.len) {
        sag_grid_cursor(&ed->grid, 0U, 0U, false);
        return;
    }
    cursor = &w->cs.curs.data[w->cs.primary];
    if (ed->mode == SAG_MODE_H && ed->grid.cursor_overlay_primary_valid &&
        (ed->grid.cursor_overlay_primary_pos != cursor->pos.v ||
         ed->grid.cursor_overlay_primary_anchor != cursor->anchor.v))
        redraw_primary_selection_change(ed, w, cursor);
    ed->grid.cursor_overlay_primary_pos = cursor->pos.v;
    ed->grid.cursor_overlay_primary_anchor = cursor->anchor.v;
    ed->grid.cursor_overlay_primary_valid = true;
    line = sag_textbuf_line_of(tb, cursor->pos);
    sub = w->vp.wrap ? sag_vp_cursor_subrow(w) : 0U;
    if (!sag_vp_row_of_line(w, line, sub, &row) || w->rect.w == 0U) {
        sag_grid_cursor(&ed->grid, 0U, 0U, false);
        return;
    }
    span = w->vp.wrap ? sag_wrap_row(w, line, sub) :
                        line_content_span(tb, line);
    col = sag_off_to_ccol(tb, span, cursor->pos, draw_tabwidth(w));
    screen_col = sag_vp_gridx_of_ccol(w, col);
    if ((u32)w->rect.y + row >= ed->grid.rows ||
        screen_col < w->rect.x || screen_col >= row_right(&ed->grid, w)) {
        sag_grid_cursor(&ed->grid, 0U, 0U, false);
        return;
    }
    sag_grid_cursor(&ed->grid, (u16)(w->rect.y + row), screen_col, true);
}

void sag_draw_footer(Ed *ed, Win *w)
{
    if (ed->footer_rect.h == 0U)
        return;
    if (ed->cmdline.active)
        sag_cmdline_draw(ed, ed->footer_rect);
    else if (ed->msg.active)
        sag_message_draw(ed, w);
    else
        sag_statusline_draw(ed, w);
}

void sag_draw_win(Ed *ed, Win *w)
{
    if (ed == NULL || w == NULL || w->buf == NULL || w->buf->tb == NULL)
        SAG_BUG("draw: missing editor window");
    sag_draw_document_rows(ed, w, 0U, w->rect.h);
    sag_draw_footer(ed, w);
    if (!ed->cmdline.active)
        sag_draw_cursor(ed, w);
}

/* ---------------------------------------------------------------- */
/* Sprint 22 §7: panes, borders, dim-inactive                       */
/* ---------------------------------------------------------------- */

/*
 * Border glyphs.  Each split node owns its own border row or column and
 * draws it; leaves never draw borders.  That is what makes it
 * impossible for two adjacent leaves to double-draw a shared edge —
 * there is exactly one node responsible for each border cell.
 */
static const u8 BORDER_V[] = {0xE2U, 0x94U, 0x82U};       /* │ */
static const u8 BORDER_H[] = {0xE2U, 0x94U, 0x80U};       /* ─ */
static const u8 BORDER_CROSS[] = {0xE2U, 0x94U, 0xBCU};   /* ┼ */
static const u8 BORDER_TEE_R[] = {0xE2U, 0x94U, 0x9CU};   /* ├ */
static const u8 BORDER_TEE_L[] = {0xE2U, 0x94U, 0xA4U};   /* ┤ */

typedef struct PaneDrawCtx {
    Ed *ed;
    bool focused_subtree;
} PaneDrawCtx;

static SagColor border_color(bool active)
{
    /* Accent for the pane with focus, dim for the rest.  Exact palette
     * is theme data in a later sprint; the distinction is what this
     * sprint owes. */
    if (active)
        return (SagColor){SAG_COLOR_RGB, 231U, 125U, 36U};
    return (SagColor){SAG_COLOR_RGB, 90U, 90U, 90U};
}

/* Does the focused leaf live under this node? */
static bool subtree_has_focus(const Pane *p, const Pane *focus)
{
    if (p == NULL || focus == NULL)
        return false;
    if (p == focus)
        return true;
    if (p->is_leaf)
        return false;
    return subtree_has_focus(p->a, focus) ||
           subtree_has_focus(p->b, focus);
}

static void draw_pane_rec(Ed *ed, Pane *p)
{
    if (p == NULL || p->rect.w == 0U || p->rect.h == 0U)
        return; /* a collapsed subtree is skipped, never drawn */
    if (p->is_leaf) {
        i32 index;

        if (p->win == NULL || p->win->buf == NULL)
            return;
        sag_draw_document_rows(ed, p->win, 0U, p->win->rect.h);
        /*
         * Registered with the SAME rect that was drawn — the one-source
         * rule.  Registering the pane's full rect rather than the Win's
         * means a click in the gutter still focuses the pane.
         */
        index = sag_pane_table_add_leaf(ed, p);
        if (index >= 0)
            sag_region_add(SAG_REGION_PANE, p->rect, index);
        return;
    }
    draw_pane_rec(ed, p->a);
    draw_pane_rec(ed, p->b);
    /*
     * Draw the border after the children so a child that overran would
     * be visibly wrong here rather than invisibly wrong underneath.
     */
    {
        bool active = subtree_has_focus(p, ed->focus);
        SagColor fg = border_color(active);
        SagColor bg = (SagColor){SAG_COLOR_DEFAULT, 0U, 0U, 0U};
        u16 attrs = active ? 0U : SAG_ATTR_DIM;
        i32 index = sag_pane_table_add_split(ed, p);
        Rect border;

        if (p->dir == SAG_SPLIT_H) {
            u16 col = (u16)(p->a->rect.x + p->a->rect.w);
            u16 row;

            if (p->a->rect.w == 0U || p->b->rect.w == 0U)
                return; /* collapsed: no border to own */
            for (row = p->rect.y; row < p->rect.y + p->rect.h; row++)
                (void)sag_grid_put(&ed->grid, row, col, BORDER_V,
                                   sizeof(BORDER_V), fg, bg, attrs);
            border = (Rect){col, p->rect.y, 1U, p->rect.h};
        } else {
            u16 row = (u16)(p->a->rect.y + p->a->rect.h);
            u16 col;

            if (p->a->rect.h == 0U || p->b->rect.h == 0U)
                return;
            for (col = p->rect.x; col < p->rect.x + p->rect.w; col++)
                (void)sag_grid_put(&ed->grid, row, col, BORDER_H,
                                   sizeof(BORDER_H), fg, bg, attrs);
            border = (Rect){p->rect.x, row, p->rect.w, 1U};
        }
        if (index >= 0)
            sag_region_add(SAG_REGION_PANE_BORDER, border, index);
    }
}

/*
 * A crossing is a cell where a border of one axis meets a border of the
 * other.  It is drawn last, from the tree, so neither owner has to know
 * about the other.
 */
static void draw_crossings_rec(Ed *ed, Pane *p)
{
    if (p == NULL || p->is_leaf || p->rect.w == 0U || p->rect.h == 0U)
        return;
    draw_crossings_rec(ed, p->a);
    draw_crossings_rec(ed, p->b);
    if (p->dir == SAG_SPLIT_H && p->a->rect.w != 0U &&
        p->b->rect.w != 0U) {
        u16 col = (u16)(p->a->rect.x + p->a->rect.w);
        Pane *side[2];
        u32 i;

        side[0] = p->a;
        side[1] = p->b;
        for (i = 0U; i < 2U; i++) {
            u16 row;
            bool active;
            bool from_left = i == 0U;
            bool both;
            const u8 *glyph;
            size_t glyph_n;

            /* A vertical split immediately inside either child puts a
             * horizontal border against this column. */
            if (side[i]->is_leaf || side[i]->dir != SAG_SPLIT_V ||
                side[i]->a->rect.h == 0U || side[i]->b->rect.h == 0U)
                continue;
            row = (u16)(side[i]->a->rect.y + side[i]->a->rect.h);
            /*
             * The glyph says which way the horizontal line actually
             * runs.  A `┼` where the line stops at the column draws a
             * stub pointing at nothing — the joint has to match the
             * geometry, so a line arriving only from the right is `├`.
             */
            both = !side[1U - i]->is_leaf &&
                   side[1U - i]->dir == SAG_SPLIT_V &&
                   side[1U - i]->a->rect.h != 0U &&
                   side[1U - i]->b->rect.h != 0U &&
                   (u16)(side[1U - i]->a->rect.y +
                         side[1U - i]->a->rect.h) == row;
            if (both) {
                glyph = BORDER_CROSS;
                glyph_n = sizeof(BORDER_CROSS);
            } else if (from_left) {
                glyph = BORDER_TEE_L;
                glyph_n = sizeof(BORDER_TEE_L);
            } else {
                glyph = BORDER_TEE_R;
                glyph_n = sizeof(BORDER_TEE_R);
            }
            active = subtree_has_focus(p, ed->focus);
            (void)sag_grid_put(&ed->grid, row, col, glyph, glyph_n,
                               border_color(active),
                               (SagColor){SAG_COLOR_DEFAULT, 0U, 0U, 0U},
                               active ? 0U : SAG_ATTR_DIM);
        }
    }
}

void sag_draw_panes(Ed *ed)
{
    if (ed == NULL || ed->pane_root == NULL)
        return;
    /*
     * Frame begin CLEARS the region table, and it happens here rather
     * than after drawing so an early return below leaves an empty table
     * rather than a stale one.
     */
    sag_region_frame_begin();
    sag_pane_tables_reset(ed);
    draw_pane_rec(ed, ed->pane_root);
    draw_crossings_rec(ed, ed->pane_root);
    /* After the panes, so a strip span shadows the document beneath it
     * on overlap — last added wins. */
    sag_tab_strip_draw(ed, ed->tab_strip_rect);
    /* Last of all: the picker is modal, so its BLOCK region must shadow
     * every span drawn beneath it (last added wins). */
    sag_gp_draw(ed);
}

bool sag_draw_pane_is_focused(const Ed *ed, const Win *w)
{
    return ed != NULL && ed->focus != NULL && ed->focus->win == w;
}
