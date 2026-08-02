#include "ui/gutter.h"

#include <stdio.h>
#include <string.h>

#include "edit/ed.h"
#include "term/grid.h"
#include "ui/viewport.h"
#include "ui/win.h"
#include "util/log.h"

static u16 decimal_digits(u64 value)
{
    u16 digits = 1U;

    while (value >= 10U) {
        value /= 10U;
        digits++;
    }
    return digits;
}

u16 sag_gutter_width_for(u64 line_count, NumStyle style)
{
    u16 digits;

    (void)style;
    digits = decimal_digits(line_count == 0U ? 1U : line_count);
    if (digits < 3U)
        digits = 3U;
    return (u16)(SAG_GUTTER_SIGN_COLS + digits + 1U);
}

u16 sag_gutter_width(const Win *w)
{
    if (w == NULL || w->buf == NULL || w->buf->tb == NULL)
        SAG_BUG("gutter width: missing window buffer");
    return sag_gutter_width_for(sag_textbuf_line_count(w->buf->tb),
                                w->number_style);
}

size_t sag_gutter_number(char *dst, size_t cap, NumStyle style,
                         LineNo line, LineNo cursor_line,
                         bool continuation)
{
    u64 value;
    int written;

    if (dst == NULL || cap == 0U)
        return 0U;
    dst[0] = '\0';
    if (continuation || style == SAG_NUM_NONE)
        return 0U;
    switch (style) {
    case SAG_NUM_ABS:
        value = line.v + 1U;
        break;
    case SAG_NUM_REL:
        value = line.v >= cursor_line.v ? line.v - cursor_line.v :
                                          cursor_line.v - line.v;
        break;
    case SAG_NUM_HYBRID:
        value = line.v == cursor_line.v ? line.v + 1U :
                (line.v > cursor_line.v ? line.v - cursor_line.v :
                                          cursor_line.v - line.v);
        break;
    case SAG_NUM_NONE:
        return 0U;
    default:
        SAG_BUG("gutter number: invalid number style");
    }
    written = snprintf(dst, cap, "%llu", (unsigned long long)value);
    if (written < 0)
        SAG_BUG("gutter number: formatting failed");
    if ((size_t)written >= cap)
        return cap - 1U;
    return (size_t)written;
}

static SagColor default_color(void)
{
    SagColor color = {SAG_COLOR_DEFAULT, 0U, 0U, 0U};

    return color;
}

void sag_gutter_draw(Ed *ed, Win *w, u16 lo, u16 hi)
{
    Grid *grid;
    const Cursor *cursor;
    LineNo cursor_line;
    u16 width;
    u16 number_width;
    u16 row;
    SagColor color = default_color();

    if (ed == NULL || w == NULL || w->buf == NULL || w->buf->tb == NULL)
        SAG_BUG("gutter draw: missing editor window");
    grid = &ed->grid;
    width = w->gutter_width;
    if (width > grid->cols)
        width = grid->cols;
    if (width == 0U || lo >= hi)
        return;
    if (w->cs.curs.len == 0U || (size_t)w->cs.primary >= w->cs.curs.len)
        SAG_BUG("gutter draw: missing primary cursor");
    cursor = &w->cs.curs.data[w->cs.primary];
    cursor_line = sag_textbuf_line_of(w->buf->tb, cursor->pos);
    number_width = width > SAG_GUTTER_SIGN_COLS + 1U ?
                   (u16)(width - SAG_GUTTER_SIGN_COLS - 1U) : 0U;
    if (hi > w->rect.h)
        hi = w->rect.h;
    for (row = lo; row < hi; row++) {
        u32 grid_row32 = (u32)w->rect.y + row;
        LineNo line;
        u32 sub;

        if (grid_row32 >= grid->rows)
            break;
        sag_grid_fill(grid, (u16)grid_row32, 0U, width, grid->blank);
        if (number_width != 0U &&
            sag_vp_line_of_row(w, row, &line, &sub)) {
            char number[32];
            size_t len = sag_gutter_number(number, sizeof(number),
                                           w->number_style, line,
                                           cursor_line, sub != 0U);

            if (len > number_width)
                len = number_width;
            if (len != 0U) {
                u16 col = (u16)(SAG_GUTTER_SIGN_COLS +
                                number_width - (u16)len);

                (void)sag_grid_puts(grid, (u16)grid_row32, col,
                                    (const u8 *)number, len,
                                    color, color, 0U);
            }
        }
    }
}
