#include "ui/draw.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include "edit/ed.h"
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

static void draw_span(Grid *grid, const TextBuf *tb, Span span,
                      u16 row, const Win *w, CCol left)
{
    SagColor color = default_color();
    ByteOff start = sag_ccol_to_off(tb, span, left, SAG_VP_TABWIDTH);
    CCol logical = sag_off_to_ccol(tb, span, start, SAG_VP_TABWIDTH);
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
            cells = sag_tab_cells(logical, SAG_VP_TABWIDTH);
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

    if (w->cs.curs.len == 0U || (size_t)w->cs.primary >= w->cs.curs.len) {
        sag_grid_cursor(&ed->grid, 0U, 0U, false);
        return;
    }
    cursor = &w->cs.curs.data[w->cs.primary];
    line = sag_textbuf_line_of(tb, cursor->pos);
    sub = w->vp.wrap ? sag_vp_cursor_subrow(w) : 0U;
    if (!sag_vp_row_of_line(w, line, sub, &row) || w->rect.w == 0U) {
        sag_grid_cursor(&ed->grid, 0U, 0U, false);
        return;
    }
    span = w->vp.wrap ? sag_wrap_row(w, line, sub) :
                        line_content_span(tb, line);
    col = sag_off_to_ccol(tb, span, cursor->pos, SAG_VP_TABWIDTH);
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
    if (ed->msg.active)
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
    sag_draw_cursor(ed, w);
}
