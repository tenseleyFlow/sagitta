#include "ui/draw.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "edit/ed.h"
#include "term/grid.h"
#include "ui/win.h"
#include "unicode/coords.h"
#include "util/base.h"
#include "util/log.h"

enum { SAG_DRAW_TAB_WIDTH = 4 };

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

static u64 line_content_end(const TextBuf *tb, Span line)
{
    if (line.lo == line.hi || text_byte_at(tb, line.hi - 1U) != '\n')
        return line.hi;
    if (line.hi - line.lo >= 2U &&
        text_byte_at(tb, line.hi - 2U) == '\r')
        return line.hi - 2U;
    return line.hi - 1U;
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
        memcpy(dst + (size_t)copied, bytes, (size_t)take);
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

static void draw_line(Grid *grid, const TextBuf *tb, Span line,
                      u16 row, u16 x, u16 width)
{
    SagColor color = default_color();
    u64 pos = line.lo;
    u64 end = line_content_end(tb, line);
    u16 col = x;
    u16 right;

    if ((u32)x + width > UINT16_MAX)
        right = UINT16_MAX;
    else
        right = (u16)(x + width);
    if (right > grid->cols)
        right = grid->cols;

    while (pos < end && col < right) {
        ByteOff next_off = sag_grapheme_next_boundary(tb, BYTEOFF(pos));
        u64 next = next_off.v;
        u64 n;
        u8 local[64];
        u8 *cluster = local;

        if (next <= pos || next > end)
            SAG_BUG("draw: invalid grapheme boundary");
        n = next - pos;
        if (n > sizeof(local)) {
            if (n > (u64)SIZE_MAX)
                SAG_BUG("draw: cluster exceeds address space");
            cluster = sag_xmalloc((size_t)n);
        }
        text_copy(tb, pos, next, cluster);
        if (n == 1U && cluster[0] == '\t') {
            u16 relative = (u16)(col - x);
            u16 spaces = (u16)(SAG_DRAW_TAB_WIDTH -
                               relative % SAG_DRAW_TAB_WIDTH);
            u32 stop = (u32)col + spaces;

            if (stop > right)
                stop = right;
            col = put_spaces(grid, row, col, (u16)stop);
        } else {
            col = sag_grid_put(grid, row, col, cluster, (size_t)n,
                               color, color, 0U);
        }
        if (cluster != local)
            free(cluster);
        pos = next;
    }
    if (col < right)
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
        u64 line_no;

        if (row32 >= grid->rows)
            break;
        if (w->vp.top.v > UINT64_MAX - screen_row)
            line_no = UINT64_MAX;
        else
            line_no = w->vp.top.v + screen_row;
        if (line_no < line_count) {
            Span line = sag_textbuf_line_span(tb, LINENO(line_no));

            draw_line(grid, tb, line, (u16)row32, w->rect.x, w->rect.w);
        } else {
            u32 right32 = (u32)w->rect.x + w->rect.w;
            u16 right = right32 > grid->cols ? grid->cols : (u16)right32;

            if (w->rect.x < right)
                (void)put_spaces(grid, (u16)row32, w->rect.x, right);
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
    u64 relative_row;
    u32 screen_row;
    u64 screen_col;

    if (w->cs.curs.len == 0U || (size_t)w->cs.primary >= w->cs.curs.len) {
        sag_grid_cursor(&ed->grid, 0U, 0U, false);
        return;
    }
    cursor = &w->cs.curs.data[w->cs.primary];
    line = sag_textbuf_line_of(tb, cursor->pos);
    if (line.v < w->vp.top.v) {
        sag_grid_cursor(&ed->grid, 0U, 0U, false);
        return;
    }
    relative_row = line.v - w->vp.top.v;
    if (relative_row >= w->rect.h) {
        sag_grid_cursor(&ed->grid, 0U, 0U, false);
        return;
    }
    span = sag_textbuf_line_span(tb, line);
    col = sag_off_to_ccol(tb, span, cursor->pos, SAG_DRAW_TAB_WIDTH);
    screen_row = (u32)w->rect.y + (u32)relative_row;
    screen_col = (u64)w->rect.x + col.v;
    if (screen_row >= ed->grid.rows || w->rect.w == 0U) {
        sag_grid_cursor(&ed->grid, 0U, 0U, false);
        return;
    }
    if (screen_col >= (u64)w->rect.x + w->rect.w)
        screen_col = (u64)w->rect.x + w->rect.w - 1U;
    sag_grid_cursor(&ed->grid, (u16)screen_row,
                    screen_col > UINT16_MAX ? UINT16_MAX : (u16)screen_col,
                    true);
}

void sag_draw_footer(Ed *ed, Win *w)
{
    Grid *grid = &ed->grid;
    const Cursor *cursor;
    const char *path;
    char footer[1024];
    int written;
    size_t footer_len;
    u16 row;
    u16 col;
    SagColor color = default_color();

    if (grid->rows == 0U || grid->cols == 0U)
        return;
    row = (u16)(grid->rows - 1U);
    if (ed->msg.active) {
        written = snprintf(footer, sizeof(footer), "%s", ed->msg.text);
    } else {
        LineNo line;
        Span span;
        CCol ccol;

        if (w->cs.curs.len == 0U ||
            (size_t)w->cs.primary >= w->cs.curs.len)
            SAG_BUG("draw footer: missing primary cursor");
        cursor = &w->cs.curs.data[w->cs.primary];
        if (ed->mode < SAG_MODE_L || ed->mode >= SAG_MODE__N)
            SAG_BUG("draw footer: invalid editor mode");
        line = sag_textbuf_line_of(w->buf->tb, cursor->pos);
        span = sag_textbuf_line_span(w->buf->tb, line);
        ccol = sag_off_to_ccol(w->buf->tb, span, cursor->pos,
                               SAG_DRAW_TAB_WIDTH);
        path = w->buf->path == NULL ? "[no name]" : w->buf->path;
        written = snprintf(footer, sizeof(footer), "[%s] %s%s %llu:%llu",
                           sag_modes[ed->mode].name, path,
                           sag_buf_dirty(w->buf) ? "*" : "",
                           (unsigned long long)(line.v + 1U),
                           (unsigned long long)(ccol.v + 1U));
    }
    if (written < 0)
        SAG_BUG("draw footer: formatting failed");
    footer_len = (size_t)written;
    if (footer_len >= sizeof(footer))
        footer_len = sizeof(footer) - 1U;
    col = sag_grid_puts(grid, row, 0U, (const u8 *)footer,
                        footer_len, color, color, 0U);
    if (col < grid->cols)
        (void)put_spaces(grid, row, col, grid->cols);
}

void sag_draw_win(Ed *ed, Win *w)
{
    if (ed == NULL || w == NULL || w->buf == NULL || w->buf->tb == NULL)
        SAG_BUG("draw: missing editor window");
    sag_draw_document_rows(ed, w, 0U, w->rect.h);
    sag_draw_footer(ed, w);
    sag_draw_cursor(ed, w);
}
