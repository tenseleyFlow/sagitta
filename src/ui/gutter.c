#include "ui/gutter.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "edit/ed.h"
#include "edit/theme_cmds.h"
#include "syn/theme.h"
#include "term/grid.h"
#include "ui/viewport.h"
#include "ui/win.h"
#include "unicode/width.h"
#include "util/log.h"

static u8 invalid_sign_warned;

static const u8 *sign_fallback(SignKind kind, const char *role)
{
    static const u8 glyphs[YEW_SIGN_NKIND] = {'!', '+', 's'};
    static const u8 diag_error = 'E';
    static const u8 diag_warn = 'W';
    static const u8 diag_info = 'i';
    static const u8 diag_hint = 'h';

    if (kind >= YEW_SIGN_NKIND)
        YEW_BUG("gutter sign: invalid kind");
    if (kind == YEW_SIGN_DIAG && role != NULL) {
        if (strcmp(role, "diag.error") == 0)
            return &diag_error;
        if (strcmp(role, "diag.warn") == 0)
            return &diag_warn;
        if (strcmp(role, "diag.info") == 0)
            return &diag_info;
        if (strcmp(role, "diag.hint") == 0)
            return &diag_hint;
    }
    return &glyphs[kind];
}

static u32 sign_lower_bound(const GutterSigns *signs, LineNo line)
{
    u32 lo = 0U;
    u32 hi = signs->len;

    while (lo < hi) {
        u32 mid = lo + (hi - lo) / 2U;

        if (signs->v[mid].line.v < line.v)
            lo = mid + 1U;
        else
            hi = mid;
    }
    return lo;
}

static GutterSignLine *sign_line(Win *w, LineNo line, bool create)
{
    GutterSigns *signs = &w->gutter_signs;
    u32 at = sign_lower_bound(signs, line);

    if (at < signs->len && signs->v[at].line.v == line.v)
        return &signs->v[at];
    if (!create)
        return NULL;
    if (signs->len == signs->cap) {
        u32 cap = signs->cap == 0U ? 8U : signs->cap * 2U;

        if (cap < signs->cap)
            YEW_BUG("gutter sign table overflow");
        signs->v = yew_xreallocarray(signs->v, cap, sizeof(*signs->v));
        signs->cap = cap;
    }
    if (at < signs->len)
        (void)memmove(signs->v + at + 1U, signs->v + at,
                      (signs->len - at) * sizeof(*signs->v));
    (void)memset(&signs->v[at], 0, sizeof(signs->v[at]));
    signs->v[at].line = line;
    signs->len++;
    return &signs->v[at];
}

void yew_gutter_sign_set(Win *w, LineNo line, SignKind kind,
                         const GutterSign *sign)
{
    GutterSignLine *entry;
    u8 bit;

    if (w == NULL || kind >= YEW_SIGN_NKIND)
        YEW_BUG("gutter sign set: invalid argument");
    bit = (u8)(1U << kind);
    entry = sign_line(w, line, sign != NULL);
    if (entry == NULL)
        return;
    if (sign == NULL) {
        entry->mask &= (u8)~bit;
        return;
    }
    entry->sign[kind] = *sign;
    if (sign->glyph == NULL || sign->nbytes == 0U ||
        yew_cluster_width(sign->glyph, sign->nbytes) != 1) {
        entry->sign[kind].glyph = sign_fallback(kind, sign->role);
        entry->sign[kind].nbytes = 1U;
        if ((invalid_sign_warned & bit) == 0U) {
            yew_log(YEW_LOG_WARN,
                    "gutter sign kind %u is not one cell; using ASCII fallback",
                    (unsigned int)kind);
            invalid_sign_warned |= bit;
        }
    }
    entry->mask |= bit;
}

void yew_gutter_signs_clear(Win *w, LineNo lo, LineNo hi)
{
    GutterSigns *signs;
    u32 first;
    u32 last;

    if (w == NULL || lo.v >= hi.v)
        return;
    signs = &w->gutter_signs;
    first = sign_lower_bound(signs, lo);
    last = sign_lower_bound(signs, hi);
    if (first >= last)
        return;
    if (last < signs->len)
        (void)memmove(signs->v + first, signs->v + last,
                      (signs->len - last) * sizeof(*signs->v));
    signs->len -= last - first;
}

void yew_gutter_sign_clear_kind(Win *w, LineNo lo, LineNo hi,
                                SignKind kind)
{
    GutterSigns *signs;
    u32 at;
    u8 bit;

    if (w == NULL || lo.v >= hi.v || kind >= YEW_SIGN_NKIND)
        return;
    signs = &w->gutter_signs;
    at = sign_lower_bound(signs, lo);
    bit = (u8)(1U << kind);
    while (at < signs->len && signs->v[at].line.v < hi.v) {
        signs->v[at].mask &= (u8)~bit;
        if (signs->v[at].mask != 0U) {
            at++;
            continue;
        }
        if (at + 1U < signs->len)
            (void)memmove(signs->v + at, signs->v + at + 1U,
                          (signs->len - at - 1U) * sizeof(*signs->v));
        signs->len--;
    }
}

void yew_gutter_signs_free(Win *w)
{
    if (w == NULL)
        return;
    yew_xfree(w->gutter_signs.v);
    (void)memset(&w->gutter_signs, 0, sizeof(w->gutter_signs));
}

static const GutterSign *sign_pick(const Win *w, LineNo line,
                                   SignKind kind)
{
    const GutterSigns *signs = &w->gutter_signs;
    u32 at = sign_lower_bound(signs, line);
    u8 bit = (u8)(1U << kind);

    if (at >= signs->len || signs->v[at].line.v != line.v ||
        (signs->v[at].mask & bit) == 0U)
        return NULL;
    return &signs->v[at].sign[kind];
}

static void draw_sign(Ed *ed, Grid *grid, u16 row, u16 col,
                      const GutterSign *sign, Cell base)
{
    const ThemeEnt *role;

    if (sign == NULL || col >= grid->cols)
        return;
    role = sign->role == NULL ? NULL : yew_theme_ui_tab(ed, sign->role);
    if (role != NULL) {
        if (role->fg.tag != YEW_COLOR_DEFAULT)
            base.fg = role->fg;
        if (role->bg.tag != YEW_COLOR_DEFAULT)
            base.bg = role->bg;
        base.attrs |= role->attrs;
    }
    base.attrs |= sign->attrs;
    (void)yew_grid_put(grid, row, col, sign->glyph, sign->nbytes,
                       base.fg, base.bg, base.attrs);
}

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
    u16 grid_col;
    u16 number_width;
    u16 row;
    const ThemeEnt *gutter;
    const ThemeEnt *gutter_cursor;
    Cell gutter_blank;

    if (ed == NULL || w == NULL || w->buf == NULL || w->buf->tb == NULL)
        YEW_BUG("gutter draw: missing editor window");
    grid = &ed->grid;
    width = w->gutter_width;
    grid_col = w->rect.x >= width ? (u16)(w->rect.x - width) : 0U;
    if (grid_col >= grid->cols)
        return;
    if (width > (u16)(grid->cols - grid_col))
        width = (u16)(grid->cols - grid_col);
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
        yew_grid_fill(grid, (u16)grid_row32, grid_col,
                      (u16)(grid_col + width), gutter_blank);
        if (yew_vp_line_of_row(w, row, &line, &sub)) {
            const GutterSign *diag = sign_pick(w, line, YEW_SIGN_DIAG);
            const GutterSign *git = sign_pick(w, line, YEW_SIGN_GIT);
            const GutterSign *shadow = sign_pick(w, line,
                                                 YEW_SIGN_SHADOW);

            draw_sign(ed, grid, (u16)grid_row32, grid_col,
                      diag == NULL ? git : diag, gutter_blank);
            if (width > 1U) {
                /* Diagnostics own the first cell.  A simultaneous Git
                 * hunk remains visible in the second; transient shadow
                 * decoration uses that cell only when Git does not need it. */
                draw_sign(ed, grid, (u16)grid_row32,
                          (u16)(grid_col + 1U),
                          diag != NULL && git != NULL ? git : shadow,
                          gutter_blank);
            }
        }
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
                u16 col = (u16)(grid_col + YEW_GUTTER_SIGN_COLS +
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
