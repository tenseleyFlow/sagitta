/*
 * Sprint 18.5 §5.  See menu.h for what this widget owns and why it is
 * not part of cmdline.c.
 */
#include "ui/menu.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "edit/ed.h"
#include "ui/region.h"
#include "unicode/grapheme.h"
#include "unicode/width.h"
#include "util/log.h"

enum {
    SAG_MENU_DEFAULT_ROWS = 5,
    /* Under this many columns the detail text is dropped rather than
     * overlapping the label. */
    SAG_MENU_DETAIL_MIN_COLS = 40
};

void sag_menu_init(Menu *m, const MenuSpec *spec)
{
    if (m == NULL)
        return;
    (void)memset(m, 0, sizeof(*m));
    if (spec != NULL)
        m->spec = *spec;
    m->sel = -1;
}

void sag_menu_free(Menu *m)
{
    MenuSpec spec;

    if (m == NULL)
        return;
    spec = m->spec;
    Vec_CompItem_free(&m->items);
    free(m->stem);
    sag_menu_init(m, &spec);
}

static void menu_scroll_to_selection(Menu *m, u16 rows)
{
    if (rows == 0U || m->sel < 0) {
        m->top = 0U;
        return;
    }
    if ((u32)m->sel < m->top)
        m->top = (u32)m->sel;
    else if ((u32)m->sel >= m->top + rows)
        m->top = (u32)m->sel - rows + 1U;
    if (m->top + rows > m->items.len)
        m->top = m->items.len > rows ? (u32)(m->items.len - rows) : 0U;
}

void sag_menu_reset(Menu *m, Vec_CompItem items, u32 total, Span replace)
{
    char *held = NULL;
    size_t i;

    if (m == NULL)
        return;
    /*
     * Remember WHAT was selected, not WHERE.  The new ranking may put it
     * anywhere, and an index carried across lands on a different row.
     */
    if (m->sel >= 0 && (size_t)m->sel < m->items.len &&
        m->items.data[m->sel].text != NULL) {
        size_t n = strlen(m->items.data[m->sel].text) + 1U;

        held = sag_xmalloc(n);
        (void)memcpy(held, m->items.data[m->sel].text, n);
    }
    Vec_CompItem_free(&m->items);
    m->items = items;
    m->total = total;
    m->replace = replace;
    m->sel = -1;
    m->top = 0U;
    if (held != NULL) {
        for (i = 0U; i < m->items.len; i++) {
            if (m->items.data[i].text != NULL &&
                strcmp(m->items.data[i].text, held) == 0) {
                m->sel = (i32)i;
                break;
            }
        }
        free(held);
    }
    /*
     * The held item left the filtered set.  Falling to row 0 would leave
     * a selection the user never made, and §6's Enter rule would then
     * accept it instead of executing the line.
     */
    if (m->sel < 0)
        m->explicit_sel = false;
}

u16 sag_menu_rows(const Menu *m, u16 height)
{
    u16 want;

    if (m == NULL || height == 0U || m->items.len == 0U)
        return 0U;
    want = m->spec.max_rows == 0U ? (u16)SAG_MENU_DEFAULT_ROWS
                                  : m->spec.max_rows;
    if (want > height)
        want = height;
    if ((u64)want > m->items.len)
        want = (u16)m->items.len;
    return want;
}

bool sag_menu_move(Menu *m, i32 delta, bool page)
{
    i32 count;
    i32 next;

    if (m == NULL || m->items.len == 0U)
        return false;
    count = m->items.len > (size_t)INT32_MAX ? INT32_MAX
                                             : (i32)m->items.len;
    if (page) {
        u16 rows = m->spec.max_rows == 0U ? (u16)SAG_MENU_DEFAULT_ROWS
                                          : m->spec.max_rows;

        delta *= (i32)(rows == 0U ? 1U : rows);
    }
    if (m->sel < 0) {
        /* The first move enters the list from whichever end it came
         * from, so S-Tab out of nothing lands on the last row. */
        next = delta >= 0 ? 0 : count - 1;
    } else {
        next = m->sel + delta;
        if (next < 0)
            next = m->spec.wrap ? count - 1 : 0;
        else if (next >= count)
            next = m->spec.wrap ? 0 : count - 1;
    }
    m->sel = next;
    m->explicit_sel = true;
    return true;
}

const CompItem *sag_menu_selected(const Menu *m)
{
    if (m == NULL || m->sel < 0 || (size_t)m->sel >= m->items.len)
        return NULL;
    return &m->items.data[m->sel];
}

void sag_menu_dismiss(Menu *m)
{
    if (m == NULL)
        return;
    Vec_CompItem_free(&m->items);
    m->sel = -1;
    m->explicit_sel = false;
    m->top = 0U;
    m->total = 0U;
    m->scanning = false;
}

static Cell styled_blank(const SagUiStyle *style)
{
    Cell cell = {0};

    cell.fg = style->row_fg;
    cell.bg = style->row_bg;
    cell.attrs = style->attrs;
    cell.w = 1U;
    return cell;
}

/*
 * Overlay the matched bytes in the accent style.
 *
 * FzMatch positions are BYTE offsets, and a matched byte can sit inside
 * a multi-byte cluster -- ASCII folding means a non-ASCII pattern matches
 * its own bytes one at a time.  So the walk is by grapheme cluster, and a
 * cluster is highlighted when ANY of its bytes matched.  Colouring
 * per-byte would put the accent on the wrong columns the moment a CJK or
 * emoji name is in the list, which is exactly what §5 pins a golden for.
 *
 * Every width question goes to src/unicode/ (DoD 4).
 */
static void highlight_match(Grid *grid, u16 row, u16 col0, u16 right,
                            const char *text, const FzMatch *m,
                            const SagUiStyle *style)
{
    size_t len;
    size_t at = 0U;
    u16 col = col0;
    Cell accent;

    if (m->n_pos == 0U || text == NULL)
        return;
    len = strlen(text);
    accent = styled_blank(style);
    accent.attrs |= SAG_ATTR_BOLD | SAG_ATTR_UNDERLINE;
    while (at < len && col < right) {
        size_t next = sag_gb_next_bytes((const u8 *)text, len, at);
        int measured;
        u16 cells;
        bool hit = false;
        u16 i;

        if (next <= at)
            break;
        measured = sag_cluster_width((const u8 *)text + at, next - at);
        cells = measured > 0 ? (u16)measured : 0U;
        for (i = 0U; i < m->n_pos; i++) {
            if ((size_t)m->pos[i] >= at && (size_t)m->pos[i] < next) {
                hit = true;
                break;
            }
        }
        if (hit && cells != 0U) {
            u32 end = (u32)col + cells;

            sag_grid_overlay(grid, row, col,
                             end > right ? right : (u16)end, &accent,
                             SAG_OVERLAY_ATTRS);
        }
        col = (u32)col + cells > (u32)right ? right : (u16)(col + cells);
        at = next;
    }
}

void sag_menu_draw(Ed *ed, Menu *m, Rect area, const SagUiStyle *style)
{
    u16 rows;
    u16 first_row;
    u16 right;
    u16 i;

    if (ed == NULL || m == NULL || style == NULL)
        return;
    rows = sag_menu_rows(m, area.h);
    if (rows == 0U)
        return;
    right = (u32)area.x + area.w > ed->grid.cols ? ed->grid.cols
                                                 : (u16)(area.x + area.w);
    menu_scroll_to_selection(m, rows);
    first_row = (u16)(area.y + area.h - rows);
    /* One inert block under the whole list, added FIRST so the per-row
     * regions added after it win the overlap (last-added-wins). */
    sag_region_add(SAG_REGION_BLOCK,
                   (Rect){area.x, first_row, area.w, rows}, 0);
    for (i = 0U; i < rows; i++) {
        size_t index = m->top + i;
        u16 row = (u16)(first_row + i);
        const CompItem *item;
        SagUiStyle row_style = *style;
        Rect row_rect = {area.x, row, area.w, 1U};
        char label[512];
        char footer[64];
        u16 footer_cells;
        u16 col;
        bool selected;

        if (index >= m->items.len)
            break;
        item = &m->items.data[index];
        selected = (i32)index == m->sel;
        if (selected) {
            SagColor swap = row_style.row_fg;

            row_style.row_fg = row_style.row_bg;
            row_style.row_bg = swap;
            row_style.attrs |= SAG_ATTR_BOLD;
        } else if (item->deferred) {
            /* The command exists but hard-errors naming its sprint, and
             * its detail column already reads "Sprint 23: ...".  Dim is
             * the marker; a second glyph would only repeat the detail. */
            row_style.attrs |= SAG_ATTR_DIM;
        }
        sag_grid_fill(&ed->grid, row, area.x, right, styled_blank(&row_style));
        (void)snprintf(label, sizeof(label), "%c %s",
                       selected ? '>' : ' ', item->text);
        col = sag_grid_puts(&ed->grid, row, area.x, (const u8 *)label,
                            strlen(label), row_style.row_fg,
                            row_style.row_bg, row_style.attrs);
        /* The label starts two cells in, past the selection marker. */
        highlight_match(&ed->grid, row, (u16)(area.x + 2U), right,
                        item->text, &item->m, &row_style);
        /*
         * The footer shares the last row with that row's detail, so its
         * cells are reserved BEFORE the detail is drawn.  Drawing it
         * afterwards overwrites the tail of the detail text instead --
         * `…optionally to a path1/34`, which a golden caught.
         */
        footer[0] = '\0';
        footer_cells = 0U;
        if (i + 1U == rows) {
            size_t n;
            int width;

            if (m->scanning)
                (void)snprintf(footer, sizeof(footer), "scanning\xE2\x80\xA6");
            else if (m->total > m->items.len)
                (void)snprintf(footer, sizeof(footer), "%u+ of %u",
                               (unsigned)m->items.len, (unsigned)m->total);
            else if (m->sel >= 0)
                (void)snprintf(footer, sizeof(footer), "%u/%u",
                               (unsigned)(m->sel + 1), (unsigned)m->total);
            n = strlen(footer);
            width = n == 0U ? 0 : sag_str_width((const u8 *)footer, n, 1U);
            footer_cells = width > 0 ? (u16)width : 0U;
        }
        if (item->detail != NULL && area.w >= SAG_MENU_DETAIL_MIN_COLS) {
            u16 at = (u16)(area.x + m->spec.detail_col);
            u16 limit = footer_cells != 0U && right > footer_cells + 1U
                            ? (u16)(right - footer_cells - 1U)
                            : right;

            if (at < col + 1U)
                at = (u16)(col + 1U);
            if (at < limit) {
                size_t keep = sag_str_clip((const u8 *)item->detail,
                                           strlen(item->detail),
                                           (int)(limit - at), NULL);

                (void)sag_grid_puts(&ed->grid, row, at,
                                    (const u8 *)item->detail, keep,
                                    row_style.row_fg, row_style.row_bg,
                                    row_style.attrs);
            }
        }
        sag_region_add(SAG_REGION_MENU_ROW, row_rect, (i32)index);
        if (footer_cells != 0U && footer_cells < right)
            (void)sag_grid_puts(&ed->grid, row,
                                (u16)(right - footer_cells),
                                (const u8 *)footer, strlen(footer),
                                row_style.row_fg, row_style.row_bg,
                                row_style.attrs);
    }
}
