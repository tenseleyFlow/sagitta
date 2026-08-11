#include "ui/gutter.h"

#include <stdio.h>
#include <string.h>

#include "edit/ed.h"
#include "edit/theme_cmds.h"
#include "syn/theme.h"
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

u16 yew_gutter_width_for(u64 line_count, NumStyle style)
{
    u16 digits;

    (void)style;
    digits = decimal_digits(line_count == 0U ? 1U : line_count);
    if (digits < 3U)
        digits = 3U;
    return (u16)(YEW_GUTTER_SIGN_COLS + digits + 1U);
}

u16 yew_gutter_width(const Win *w)
{
    if (w == NULL || w->buf == NULL || w->buf->tb == NULL)
        YEW_BUG("gutter width: missing window buffer");
    return yew_gutter_width_for(yew_textbuf_line_count(w->buf->tb),
                                w->number_style);
}

size_t yew_gutter_number(char *dst, size_t cap, NumStyle style,
                         LineNo line, LineNo cursor_line,
                         bool continuation)
{
    u64 value;
    int written;

    if (dst == NULL || cap == 0U)
        return 0U;
    dst[0] = '\0';
    if (continuation || style == YEW_NUM_NONE)
        return 0U;
    switch (style) {
    case YEW_NUM_ABS:
        value = line.v + 1U;
        break;
    case YEW_NUM_REL:
        value = line.v >= cursor_line.v ? line.v - cursor_line.v :
                                          cursor_line.v - line.v;
        break;
    case YEW_NUM_HYBRID:
        value = line.v == cursor_line.v ? line.v + 1U :
                (line.v > cursor_line.v ? line.v - cursor_line.v :
                                          cursor_line.v - line.v);
        break;
    case YEW_NUM_NONE:
        return 0U;
    default:
        YEW_BUG("gutter number: invalid number style");
    }
    written = snprintf(dst, cap, "%llu", (unsigned long long)value);
    if (written < 0)
        YEW_BUG("gutter number: formatting failed");
    if ((size_t)written >= cap)
        return cap - 1U;
    return (size_t)written;
}

void yew_gutter_draw(Ed *ed, Win *w, u16 lo, u16 hi)
{
    Grid *grid;
    const Cursor *cursor;
    LineNo cursor_line;
    u16 width;
    u16 number_width;
    u16 row;
    const ThemeEnt *gutter;
    const ThemeEnt *gutter_cursor;
    Cell gutter_blank;

    if (ed == NULL || w == NULL || w->buf == NULL || w->buf->tb == NULL)
        YEW_BUG("gutter draw: missing editor window");
    grid = &ed->grid;
    width = w->gutter_width;
    if (width > grid->cols)
        width = grid->cols;
    if (width == 0U || lo >= hi)
        return;
    if (w->cs.curs.len == 0U || (size_t)w->cs.primary >= w->cs.curs.len)
        YEW_BUG("gutter draw: missing primary cursor");
    cursor = &w->cs.curs.data[w->cs.primary];
    cursor_line = yew_textbuf_line_of(w->buf->tb, cursor->pos);
    gutter = yew_theme_ui_tab(ed, "gutter");
    gutter_cursor = yew_theme_ui_tab(ed, "gutter.cursor");
    gutter_blank = grid->blank;
    if (gutter != NULL) {
        if (gutter->fg.tag != YEW_COLOR_DEFAULT)
            gutter_blank.fg = gutter->fg;
        if (gutter->bg.tag != YEW_COLOR_DEFAULT)
            gutter_blank.bg = gutter->bg;
        gutter_blank.attrs = gutter->attrs;
    }
    number_width = width > YEW_GUTTER_SIGN_COLS + 1U ?
                   (u16)(width - YEW_GUTTER_SIGN_COLS - 1U) : 0U;
    if (hi > w->rect.h)
        hi = w->rect.h;
    for (row = lo; row < hi; row++) {
        u32 grid_row32 = (u32)w->rect.y + row;
        LineNo line;
        u32 sub;

        if (grid_row32 >= grid->rows)
            break;
        yew_grid_fill(grid, (u16)grid_row32, 0U, width, gutter_blank);
        if (number_width != 0U &&
            yew_vp_line_of_row(w, row, &line, &sub)) {
            char number[32];
            size_t len = yew_gutter_number(number, sizeof(number),
                                           w->number_style, line,
                                           cursor_line, sub != 0U);

            if (len > number_width)
                len = number_width;
            if (len != 0U) {
                const ThemeEnt *number_style =
                    line.v == cursor_line.v && sub == 0U &&
                            gutter_cursor != NULL ? gutter_cursor : gutter;
                YewColor fg = gutter_blank.fg;
                YewColor bg = gutter_blank.bg;
                u16 attrs = gutter_blank.attrs;
                u16 col = (u16)(YEW_GUTTER_SIGN_COLS +
                                number_width - (u16)len);

                if (number_style != NULL) {
                    if (number_style->fg.tag != YEW_COLOR_DEFAULT)
                        fg = number_style->fg;
                    if (number_style->bg.tag != YEW_COLOR_DEFAULT)
                        bg = number_style->bg;
                    attrs = number_style->attrs;
                }
                (void)yew_grid_puts(grid, (u16)grid_row32, col,
                                    (const u8 *)number, len, fg, bg, attrs);
            }
        }
    }
}
