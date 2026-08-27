#include "ui/shadowdraw.h"

#include <string.h>

#include "edit/ed.h"
#include "edit/shadow.h"
#include "edit/theme_cmds.h"
#include "syn/theme.h"
#include "term/grid.h"
#include "ui/gutter.h"
#include "ui/region.h"
#include "ui/viewport.h"
#include "ui/win.h"
#include "unicode/coords.h"
#include "unicode/width.h"
#include "util/log.h"

static u8 shadow_byte_at(const TextBuf *tb, u64 off)
{
    TextIter iter;
    const u8 *bytes;
    u64 len;

    if (!yew_textiter_begin(&iter, tb, BYTEOFF(off)) ||
        !yew_textiter_chunk(&iter, tb, &bytes, &len) || len == 0U)
        YEW_BUG("shadow layout: cannot read buffer byte");
    return bytes[0];
}

static Span shadow_line_content(const TextBuf *tb, LineNo line)
{
    Span span = yew_textbuf_line_span(tb, line);

    if (span.lo == span.hi || shadow_byte_at(tb, span.hi - 1U) != '\n')
        return span;
    span.hi--;
    if (span.hi > span.lo && shadow_byte_at(tb, span.hi - 1U) == '\r')
        span.hi--;
    return span;
}

static u16 shadow_line_count(const ShadowSug *suggestion)
{
    u32 at;
    u16 lines = 1U;

    for (at = suggestion->consumed; at < suggestion->len; at++)
        if (suggestion->text[at] == '\n' && lines != UINT16_MAX)
            lines++;
    return lines;
}

void yew_shadow_layout(const Win *win, const Shadow *shadow,
                       ShadowLayout *out)
{
    Win *mutable_win;
    const Cursor *cursor;
    TextBuf *tb;
    LineNo line;
    Span span;
    CCol col;
    u32 sub;
    u16 row;
    u16 x;
    u16 right;
    u16 lines;
    u16 max_lines;
    u16 available;

    if (out == NULL)
        YEW_BUG("shadow layout: missing output");
    (void)memset(out, 0, sizeof(*out));
    if (win == NULL || shadow == NULL || !shadow->live ||
        shadow->sug.text == NULL || shadow->sug.consumed >= shadow->sug.len ||
        win->buf == NULL || win->buf->tb == NULL || win->rect.w == 0U ||
        win->rect.h == 0U || win->cs.curs.len == 0U ||
        win->cs.primary >= win->cs.curs.len)
        return;
    mutable_win = (Win *)win;
    cursor = &win->cs.curs.data[win->cs.primary];
    tb = win->buf->tb;
    line = yew_textbuf_line_of(tb, cursor->pos);
    sub = win->vp.wrap ? yew_vp_cursor_subrow(mutable_win) : 0U;
    if (!yew_vp_row_of_line(mutable_win, line, sub, &row))
        return;
    span = win->vp.wrap ? yew_wrap_row(mutable_win, line, sub) :
                          shadow_line_content(tb, line);
    col = yew_off_to_ccol(tb, span, cursor->pos,
                          win->buf->tabwidth == 0U ? YEW_VP_TABWIDTH :
                                                   win->buf->tabwidth);
    x = yew_vp_gridx_of_ccol(win, col);
    right = (u32)win->rect.x + win->rect.w > UINT16_MAX ? UINT16_MAX :
            (u16)(win->rect.x + win->rect.w);
    if (x < win->rect.x || x >= right || row >= win->rect.h)
        return;
    lines = shadow_line_count(&shadow->sug);
    if (lines > 1U && cursor->pos.v != shadow_line_content(tb, line).hi)
        return;
    max_lines = shadow->max_lines == 0U ? YEW_SHADOW_MAX_LINES :
                                         shadow->max_lines;
    if (max_lines > YEW_SHADOW_MAX_LINES)
        max_lines = YEW_SHADOW_MAX_LINES;
    available = (u16)(win->rect.h - row);
    out->nlines = lines < max_lines ? lines : max_lines;
    if (out->nlines > available)
        out->nlines = available;
    if (out->nlines == 0U)
        return;
    out->clipped = lines > out->nlines;
    out->logical_col = col.v;
    out->inline_run = (Rect){x, (u16)(win->rect.y + row),
                             (u16)(right - x), 1U};
    if (out->nlines > 1U)
        out->vrows = (Rect){win->rect.x,
                            (u16)(win->rect.y + row + 1U), win->rect.w,
                            (u16)(out->nlines - 1U)};
}

static void shadow_style(const Ed *ed, const ShadowSug *suggestion,
                         Cell *style, u8 *glyph)
{
    static const char *const roles[YEW_SHADOW_NPROV] = {
        "shadow.index", "shadow.lsp", "shadow.ai",
    };
    static const u8 glyphs[YEW_SHADOW_NPROV] = {'s', 'l', 'a'};
    static const u16 attrs[YEW_SHADOW_NPROV] = {
        YEW_ATTR_DIM,
        YEW_ATTR_DIM | YEW_ATTR_ITALIC,
        YEW_ATTR_DIM | YEW_ATTR_ITALIC | YEW_ATTR_UNDERLINE,
    };
    const ThemeEnt *theme;
    u8 prov = suggestion->prov;

    if (prov >= (u8)YEW_SHADOW_NPROV)
        YEW_BUG("shadow draw: invalid provider");
    *glyph = glyphs[prov];
    style->attrs |= attrs[prov];
    theme = yew_theme_ui_tab(ed, roles[prov]);
    if (theme == NULL)
        return;
    if (theme->fg.tag != YEW_COLOR_DEFAULT)
        style->fg = theme->fg;
    if (theme->bg.tag != YEW_COLOR_DEFAULT)
        style->bg = theme->bg;
    style->attrs |= theme->attrs;
}

static u16 shadow_puts(Grid *grid, u16 row, u16 col, u16 right,
                       const u8 *text, size_t len, Cell style,
                       CCol logical, u32 tabwidth)
{
    static const u8 space = ' ';
    size_t at = 0U;

    if (right > grid->cols)
        right = grid->cols;
    while (at < len && col < right) {
        size_t run = at;

        while (run < len && text[run] != '\t')
            run++;
        if (run != at) {
            u16 before = col;

            col = yew_grid_puts(grid, row, col, text + at, run - at,
                                style.fg, style.bg, style.attrs);
            logical.v += (u64)(col - before);
            at = run;
            continue;
        }
        {
            u32 cells = yew_tab_cells(logical, tabwidth);

            logical.v += cells;
            while (cells != 0U && col < right) {
                col = yew_grid_put(grid, row, col, &space, 1U,
                                   style.fg, style.bg, style.attrs);
                cells--;
            }
        }
        at++;
    }
    return col;
}

static size_t shadow_clip(const u8 *text, size_t len, u16 max_cells,
                          CCol logical, u32 tabwidth, u16 *out_cells)
{
    size_t at = 0U;
    u16 cells = 0U;

    while (at < len && cells < max_cells) {
        size_t run = at;
        size_t run_len;
        int run_cells = 0;
        size_t keep;

        while (run < len && text[run] != '\t')
            run++;
        if (run != at) {
            run_len = run - at;
            keep = yew_str_clip(text + at, run_len,
                                (int)(max_cells - cells), &run_cells);
            at += keep;
            cells = (u16)(cells + (u16)run_cells);
            logical.v += (u64)run_cells;
            if (keep != run_len)
                break;
            continue;
        }
        {
            u32 tab_cells = yew_tab_cells(logical, tabwidth);
            u16 visible = tab_cells > (u32)(max_cells - cells)
                              ? (u16)(max_cells - cells)
                              : (u16)tab_cells;

            cells = (u16)(cells + visible);
            logical.v += tab_cells;
            at++;
            if (visible != tab_cells)
                break;
        }
    }
    *out_cells = cells;
    return at;
}

static void shadow_damage(Grid *grid, u16 row, u16 lo, u16 hi)
{
    yew_grid_overlay(grid, row, lo, hi, &grid->blank, 0U);
}

static void shadow_blank_cells(Grid *grid, u16 row, u16 lo, u16 hi)
{
    Cell *cells;
    u16 col;

    if (row >= grid->rows || lo >= hi || lo >= grid->cols)
        return;
    if (hi > grid->cols)
        hi = grid->cols;
    cells = grid->back + (size_t)row * grid->cols;
    for (col = lo; col < hi; col++)
        cells[col] = grid->blank;
}

static void shadow_shift_inline(Grid *grid, Rect rect, u16 cells_right)
{
    Cell *row;
    u16 right;
    u16 width;
    u16 keep;
    u16 moved_end;

    if (rect.y >= grid->rows || rect.x >= grid->cols || rect.w == 0U ||
        cells_right == 0U)
        return;
    right = (u32)rect.x + rect.w > grid->cols ? grid->cols :
                                                    (u16)(rect.x + rect.w);
    width = (u16)(right - rect.x);
    row = grid->back + (size_t)rect.y * grid->cols;
    if (cells_right >= width) {
        shadow_blank_cells(grid, rect.y, rect.x, right);
        shadow_damage(grid, rect.y, rect.x, right);
        return;
    }
    keep = (u16)(width - cells_right);
    /* Clipping may not strand a wide head at the pane edge. */
    if (keep != 0U && row[rect.x + keep - 1U].w == 2U)
        keep--;
    if (keep != 0U)
        (void)memmove(row + rect.x + cells_right, row + rect.x,
                      (size_t)keep * sizeof(*row));
    moved_end = (u16)(rect.x + cells_right + keep);
    shadow_blank_cells(grid, rect.y, rect.x,
                       (u16)(rect.x + cells_right));
    shadow_blank_cells(grid, rect.y, moved_end, right);
    shadow_damage(grid, rect.y, rect.x, right);
}

static void shadow_shift_rows(Grid *grid, const Win *win,
                              const ShadowLayout *layout)
{
    u16 count;
    u16 top;
    u16 bottom;
    u16 left;
    u16 right;
    u16 dst;

    if (layout->nlines <= 1U)
        return;
    count = (u16)(layout->nlines - 1U);
    top = (u16)(layout->inline_run.y + 1U);
    bottom = (u32)win->rect.y + win->rect.h > grid->rows ? grid->rows :
                                                        (u16)(win->rect.y +
                                                              win->rect.h);
    left = win->rect.x >= win->gutter_width
               ? (u16)(win->rect.x - win->gutter_width)
               : 0U;
    right = (u32)win->rect.x + win->rect.w > grid->cols ? grid->cols :
                                                        (u16)(win->rect.x +
                                                              win->rect.w);
    if (top >= bottom || left >= right)
        return;
    dst = bottom;
    while (dst > (u16)(top + count)) {
        Cell *to;
        const Cell *from;

        dst--;
        to = grid->back + (size_t)dst * grid->cols + left;
        from = grid->back + (size_t)(dst - count) * grid->cols + left;
        (void)memmove(to, from, (size_t)(right - left) * sizeof(*to));
        shadow_damage(grid, dst, left, right);
    }
    for (dst = top; dst < (u16)(top + count); dst++) {
        shadow_blank_cells(grid, dst, left, right);
        shadow_damage(grid, dst, left, right);
    }
}

static u16 shadow_draw_line(Grid *grid, Rect rect, const u8 *text,
                            size_t len, bool ellipsis, Cell style,
                            bool compose_inline, CCol logical,
                            u32 tabwidth)
{
    static const u8 dots[] = "\xE2\x80\xA6";
    u16 text_cells;
    size_t keep;
    u16 total_cells;

    if (rect.w == 0U || rect.h == 0U || rect.y >= grid->rows ||
        rect.x >= grid->cols)
        return 0U;
    text_cells = ellipsis && rect.w != 0U ? (u16)(rect.w - 1U) : rect.w;
    keep = shadow_clip(text, len, text_cells, logical, tabwidth,
                       &total_cells);
    if (ellipsis)
        total_cells++;
    if (compose_inline)
        shadow_shift_inline(grid, rect, total_cells);
    (void)shadow_puts(grid, rect.y, rect.x,
                      (u16)(rect.x + rect.w), text, keep, style,
                      logical, tabwidth);
    if (ellipsis)
        (void)yew_grid_put(grid, rect.y,
                           (u16)(rect.x + rect.w - 1U), dots,
                           sizeof(dots) - 1U, style.fg, style.bg,
                           style.attrs);
    return total_cells;
}

void yew_shadow_draw(Ed *ed, Win *win, const ShadowLayout *layout,
                     Grid *grid)
{
    Shadow *shadow;
    const u8 *text;
    u32 len;
    u32 at;
    u16 line;
    Cell style;
    u8 glyph;
    u16 gutter_col;
    u32 tabwidth;

    if (ed == NULL || win == NULL || layout == NULL || grid == NULL ||
        layout->nlines == 0U)
        return;
    shadow = &win->shadow;
    if (win->compl.open && shadow->live)
        YEW_BUG("shadow draw: completion menu and ghost are both open");
    if (!shadow->live || shadow->suppressed)
        return;
    text = shadow->sug.text + shadow->sug.consumed;
    len = shadow->sug.len - shadow->sug.consumed;
    style = grid->blank;
    shadow_style(ed, &shadow->sug, &style, &glyph);
    tabwidth = win->buf->tabwidth == 0U ? YEW_VP_TABWIDTH :
                                         win->buf->tabwidth;
    shadow_shift_rows(grid, win, layout);
    at = 0U;
    for (line = 0U; line < layout->nlines; line++) {
        u32 end = at;
        Rect row = line == 0U ? layout->inline_run :
                   (Rect){layout->vrows.x,
                          (u16)(layout->vrows.y + line - 1U),
                          layout->vrows.w, 1U};
        bool last = line + 1U == layout->nlines;

        while (end < len && text[end] != '\n')
            end++;
        if (end > at && text[end - 1U] == '\r')
            end--;
        (void)shadow_draw_line(grid, row, text + at, end - at,
                              last && layout->clipped, style,
                              line == 0U,
                              (CCol){line == 0U ? layout->logical_col : 0U},
                              tabwidth);
        if (end < len && text[end] == '\r')
            end++;
        at = end < len && text[end] == '\n' ? end + 1U : end;
    }
    if (win->gutter_width >= YEW_GUTTER_SIGN_COLS && win->rect.x >= 1U) {
        gutter_col = (u16)(win->rect.x - win->gutter_width + 1U);
        for (line = 0U; line < layout->nlines; line++)
            (void)yew_grid_put(grid,
                               (u16)(layout->inline_run.y + line),
                               gutter_col, &glyph, 1U, style.fg, style.bg,
                               style.attrs);
    }
    yew_region_add(YEW_REGION_BLOCK, layout->inline_run, 0);
    yew_region_add(YEW_REGION_BLOCK, layout->vrows, 0);
    shadow->draw_row = (u16)(layout->inline_run.y - win->rect.y);
    shadow->vrows = layout->nlines;
}

static void shadow_draw_rec(Ed *ed, Pane *pane)
{
    ShadowLayout layout;

    if (pane == NULL || pane->rect.w == 0U || pane->rect.h == 0U)
        return;
    if (!pane->is_leaf) {
        shadow_draw_rec(ed, pane->a);
        shadow_draw_rec(ed, pane->b);
        return;
    }
    if (pane->win == NULL)
        return;
    /* Retire the previous frame's row transform even when dismissal made
     * this frame ghost-free.  Input keeps using the old geometry until
     * this draw pass replaces the cells and region table together. */
    pane->win->shadow.vrows = 0U;
    if (!pane->win->shadow.live)
        return;
    yew_shadow_layout(pane->win, &pane->win->shadow, &layout);
    yew_shadow_draw(ed, pane->win, &layout, &ed->grid);
}

void yew_shadow_draw_panes(Ed *ed)
{
    if (ed != NULL)
        shadow_draw_rec(ed, ed->pane_root);
}
