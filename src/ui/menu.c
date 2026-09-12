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
    YEW_MENU_DEFAULT_ROWS = 5,
    /* Under this many columns the detail text is dropped rather than
     * overlapping the label. */
    YEW_MENU_DETAIL_MIN_COLS = 40
};

void yew_menu_init(Menu *m, const MenuSpec *spec)
{
    if (m == NULL)
        return;
    (void)memset(m, 0, sizeof(*m));
    if (spec != NULL)
        m->spec = *spec;
    m->sel = -1;
}

void yew_menu_free(Menu *m)
{
    MenuSpec spec;

    if (m == NULL)
        return;
    spec = m->spec;
    Vec_CompItem_free(&m->items);
    yew_xfree(m->stem);
    yew_xfree(m->held);
    yew_menu_init(m, &spec);
}

static void menu_hold(Menu *m)
{
    const CompItem *item = yew_menu_selected(m);

    yew_xfree(m->held);
    m->held = NULL;
    if (item != NULL && item->text != NULL) {
        size_t n = strlen(item->text) + 1U;

        m->held = yew_xmalloc(n);
        (void)memcpy(m->held, item->text, n);
    }
}

/* Rows the widget would draw in an area of its own maximum height --
 * what a move knows before the draw tells it what it actually got. */
static u16 menu_spec_rows(const Menu *m)
{
    return m->spec.max_rows == 0U ? (u16)YEW_MENU_DEFAULT_ROWS
                                  : m->spec.max_rows;
}

/*
 * Sprint 57.17 §3: how many of `rows` drawn rows carry candidates when
 * the window starts at `top`.
 *
 * ONE function, so the draw, the scroll clamp and the click regions
 * cannot disagree about which row is the tail.  Three copies of "is
 * there a tail?" is a click that selects the row the tail is covering.
 */
static u16 menu_candidate_rows_at(const Menu *m, u32 top, u16 rows)
{
    /* A one-row window spent on a tail would show no candidates at all,
     * and "scanning…" is a state rather than a count -- see menu.h. */
    if (rows < 2U || m->scanning)
        return rows;
    return (u64)top + rows >= m->items.len ? rows : (u16)(rows - 1U);
}

static void menu_scroll_to_selection(Menu *m, u16 rows)
{
    u32 sel;
    u32 max_top;
    u32 want;
    u16 cand;

    if (rows == 0U || m->sel < 0) {
        m->top = 0U;
        return;
    }
    sel = (u32)m->sel;
    max_top = m->items.len > rows ? (u32)(m->items.len - rows) : 0U;
    if (sel < m->top) {
        m->top = sel;
    } else {
        /*
         * Room for CANDIDATES, not for rows.  The last row becomes the
         * tail whenever anything is left below, so a scrolled window
         * holds one candidate fewer than it has rows -- and a move onto
         * what would be the tail therefore scrolls by one instead,
         * which is exactly §3's "moving down onto it scrolls by one and
         * keeps the tail".
         */
        want = sel + 1U >= rows ? sel + 1U - rows : 0U;
        cand = menu_candidate_rows_at(m, want, rows);
        if (cand < rows)
            want = sel + 1U >= cand ? sel + 1U - cand : 0U;
        if (want > max_top)
            want = max_top;
        if (m->top < want)
            m->top = want;
    }
    if (m->top > max_top)
        m->top = max_top;
}

void yew_menu_reset(Menu *m, Vec_CompItem items, u32 total, Span replace)
{
    size_t i;

    if (m == NULL)
        return;
    Vec_CompItem_free(&m->items);
    m->items = items;
    m->total = total;
    m->replace = replace;
    m->sel = -1;
    m->top = 0U;
    /*
     * Re-find WHAT was selected, not WHERE.  The new ranking may put it
     * anywhere, and an index carried across lands on a different row.
     * `held` is our own copy precisely because the items it came from
     * may already have been freed with their arena.
     */
    if (m->held != NULL) {
        for (i = 0U; i < m->items.len; i++) {
            if (m->items.data[i].text != NULL &&
                strcmp(m->items.data[i].text, m->held) == 0) {
                m->sel = (i32)i;
                break;
            }
        }
    }
    /*
     * The held item left the filtered set.  Falling to row 0 would leave
     * a selection the user never made, and §6's Enter rule would then
     * accept it instead of executing the line.
     */
    if (m->sel < 0) {
        m->explicit_sel = false;
        /* Focus over nothing is not focus: the arrows would have no row
         * to move and §6's Enter rule no choice to accept. */
        m->focus = false;
        yew_xfree(m->held);
        m->held = NULL;
    }
}

u16 yew_menu_rows(const Menu *m, u16 height)
{
    u16 want;

    if (m == NULL || height == 0U || m->items.len == 0U)
        return 0U;
    want = m->spec.max_rows == 0U ? (u16)YEW_MENU_DEFAULT_ROWS
                                  : m->spec.max_rows;
    if (want > height)
        want = height;
    if ((u64)want > m->items.len)
        want = (u16)m->items.len;
    return want;
}

u16 yew_menu_candidate_rows(const Menu *m, u16 height)
{
    u16 rows;

    if (m == NULL)
        return 0U;
    rows = yew_menu_rows(m, height);
    return rows == 0U ? 0U : menu_candidate_rows_at(m, m->top, rows);
}

u32 yew_menu_hidden(const Menu *m, u16 height)
{
    u16 rows;
    u16 cand;

    if (m == NULL)
        return 0U;
    rows = yew_menu_rows(m, height);
    cand = rows == 0U ? 0U : menu_candidate_rows_at(m, m->top, rows);
    if (rows == 0U || cand == rows)
        return 0U;
    return (u32)(m->items.len - m->top - cand);
}

bool yew_menu_move(Menu *m, i32 delta, bool page)
{
    i32 count;
    i32 next;

    if (m == NULL || m->items.len == 0U)
        return false;
    count = m->items.len > (size_t)INT32_MAX ? INT32_MAX
                                             : (i32)m->items.len;
    if (page) {
        u16 rows = m->spec.max_rows == 0U ? (u16)YEW_MENU_DEFAULT_ROWS
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
    menu_hold(m);
    /* Sprint 57.17 §3: the selection drags the window with it.  The
     * draw re-applies the same rule with the height it really got; this
     * is what makes `top` correct for a caller that asks BEFORE the
     * next paint. */
    menu_scroll_to_selection(m, menu_spec_rows(m));
    return true;
}

bool yew_menu_select(Menu *m, i32 index)
{
    if (m == NULL || index < 0 || (size_t)index >= m->items.len)
        return false;
    m->sel = index;
    m->explicit_sel = true;
    menu_hold(m);
    menu_scroll_to_selection(m, menu_spec_rows(m));
    return true;
}

bool yew_menu_focus(Menu *m)
{
    if (m == NULL || m->items.len == 0U)
        return false;
    if (m->sel < 0) {
        /* Enter at the best match, the row Tab lands on -- and the row
         * one more `<up>` then leaves the pager from. */
        m->sel = 0;
        m->explicit_sel = true;
        menu_hold(m);
        menu_scroll_to_selection(m, menu_spec_rows(m));
    }
    m->focus = true;
    return true;
}

void yew_menu_blur(Menu *m)
{
    if (m != NULL)
        m->focus = false;
}

void yew_menu_unselect(Menu *m)
{
    if (m == NULL)
        return;
    m->focus = false;
    m->sel = -1;
    m->explicit_sel = false;
    m->top = 0U;
    yew_xfree(m->held);
    m->held = NULL;
}

bool yew_menu_focused(const Menu *m)
{
    return m != NULL && m->focus && m->items.len != 0U && m->sel >= 0;
}

bool yew_menu_scroll(Menu *m, i32 delta, u16 height)
{
    u16 rows;
    i64 top;
    i64 max_top;

    if (m == NULL || m->items.len == 0U)
        return false;
    rows = yew_menu_rows(m, height);
    if (rows == 0U || m->items.len <= rows)
        return false;
    max_top = (i64)m->items.len - (i64)rows;
    top = (i64)m->top + delta;
    if (top < 0)
        top = 0;
    if (top > max_top)
        top = max_top;
    if ((u32)top == m->top)
        return false;
    m->top = (u32)top;
    return true;
}

const CompItem *yew_menu_selected(const Menu *m)
{
    if (m == NULL || m->sel < 0 || (size_t)m->sel >= m->items.len)
        return NULL;
    return &m->items.data[m->sel];
}

void yew_menu_dismiss(Menu *m)
{
    if (m == NULL)
        return;
    Vec_CompItem_free(&m->items);
    yew_xfree(m->held);
    m->held = NULL;
    m->sel = -1;
    m->explicit_sel = false;
    m->focus = false;
    m->top = 0U;
    m->total = 0U;
    m->scanning = false;
}

static Cell styled_blank(const YewUiStyle *style)
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
                            const YewUiStyle *style)
{
    size_t len;
    size_t at = 0U;
    u16 col = col0;
    Cell accent;

    if (m->n_pos == 0U || text == NULL)
        return;
    len = strlen(text);
    accent = styled_blank(style);
    accent.attrs |= YEW_ATTR_BOLD | YEW_ATTR_UNDERLINE;
    while (at < len && col < right) {
        size_t next = yew_gb_next_bytes((const u8 *)text, len, at);
        int measured;
        u16 cells;
        bool hit = false;
        u16 i;

        if (next <= at)
            break;
        measured = yew_cluster_width((const u8 *)text + at, next - at);
        cells = measured > 0 ? (u16)measured : 0U;
        for (i = 0U; i < m->n_pos; i++) {
            if ((size_t)m->pos[i] >= at && (size_t)m->pos[i] < next) {
                hit = true;
                break;
            }
        }
        if (hit && cells != 0U) {
            u32 end = (u32)col + cells;

            yew_grid_overlay(grid, row, col,
                             end > right ? right : (u16)end, &accent,
                             YEW_OVERLAY_ATTRS);
        }
        col = (u32)col + cells > (u32)right ? right : (u16)(col + cells);
        at = next;
    }
}

/*
 * Sprint 57.17 §3: the honest tail.
 *
 * `… and N more` rather than a silently truncated list.  It is drawn
 * where a candidate would be, in the row style but dimmed, indented to
 * the label column so it lines up with the names above it, and it
 * registers NO region: it names no candidate, so a click must fall
 * through to the inert block rather than select the row it covers.
 */
static void draw_tail(Ed *ed, u16 row, u16 x, u16 right, u32 hidden,
                      const YewUiStyle *style)
{
    YewUiStyle tail_style = *style;
    char text[64];

    tail_style.attrs |= YEW_ATTR_DIM;
    yew_grid_fill(&ed->grid, row, x, right, styled_blank(&tail_style));
    (void)snprintf(text, sizeof(text), "  \xE2\x80\xA6 and %u more",
                   (unsigned)hidden);
    (void)yew_grid_puts(&ed->grid, row, x, (const u8 *)text, strlen(text),
                        tail_style.row_fg, tail_style.row_bg,
                        tail_style.attrs);
}

void yew_menu_draw(Ed *ed, Menu *m, Rect area, const YewUiStyle *style)
{
    u16 rows;
    u16 cand_rows;
    u32 hidden;
    u16 first_row;
    u16 right;
    u16 i;

    if (ed == NULL || m == NULL || style == NULL)
        return;
    rows = yew_menu_rows(m, area.h);
    if (rows == 0U)
        return;
    right = (u32)area.x + area.w > ed->grid.cols ? ed->grid.cols
                                                 : (u16)(area.x + area.w);
    menu_scroll_to_selection(m, rows);
    cand_rows = menu_candidate_rows_at(m, m->top, rows);
    hidden = cand_rows == rows ? 0U
                               : (u32)(m->items.len - m->top - cand_rows);
    first_row = (u16)(area.y + area.h - rows);
    /* One inert block under the whole list, added FIRST so the per-row
     * regions added after it win the overlap (last-added-wins). */
    yew_region_add(YEW_REGION_BLOCK,
                   (Rect){area.x, first_row, area.w, rows}, 0);
    for (i = 0U; i < rows; i++) {
        size_t index = m->top + i;
        u16 row = (u16)(first_row + i);
        const CompItem *item;
        YewUiStyle row_style = *style;
        Rect row_rect = {area.x, row, area.w, 1U};
        char label[512];
        char footer[64];
        u16 footer_cells;
        u16 col;
        bool selected;

        if (i >= cand_rows) {
            /* The last row is the tail, and it is the whole row: no
             * label, no detail, no footer.  menu.h states the rule. */
            draw_tail(ed, row, area.x, right, hidden, style);
            break;
        }
        if (index >= m->items.len)
            break;
        item = &m->items.data[index];
        selected = (i32)index == m->sel;
        if (selected) {
            YewColor swap = row_style.row_fg;

            row_style.row_fg = row_style.row_bg;
            row_style.row_bg = swap;
            row_style.attrs |= YEW_ATTR_BOLD;
        } else if (item->deferred) {
            /* The command exists but hard-errors naming its sprint, and
             * its detail column already reads "Sprint 23: ...".  Dim is
             * the marker; a second glyph would only repeat the detail. */
            row_style.attrs |= YEW_ATTR_DIM;
        }
        yew_grid_fill(&ed->grid, row, area.x, right, styled_blank(&row_style));
        (void)snprintf(label, sizeof(label), "%c %s",
                       selected ? '>' : ' ', item->text);
        col = yew_grid_puts(&ed->grid, row, area.x, (const u8 *)label,
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
            width = n == 0U ? 0 : yew_str_width((const u8 *)footer, n, 1U);
            footer_cells = width > 0 ? (u16)width : 0U;
        }
        if (item->detail != NULL && area.w >= YEW_MENU_DETAIL_MIN_COLS) {
            u16 at = (u16)(area.x + m->spec.detail_col);
            u16 limit = footer_cells != 0U && right > footer_cells + 1U
                            ? (u16)(right - footer_cells - 1U)
                            : right;

            if (at < col + 1U)
                at = (u16)(col + 1U);
            if (at < limit) {
                size_t keep = yew_str_clip((const u8 *)item->detail,
                                           strlen(item->detail),
                                           (int)(limit - at), NULL);

                (void)yew_grid_puts(&ed->grid, row, at,
                                    (const u8 *)item->detail, keep,
                                    row_style.row_fg, row_style.row_bg,
                                    row_style.attrs);
            }
        }
        yew_region_add(YEW_REGION_MENU_ROW, row_rect, (i32)index);
        if (footer_cells != 0U && footer_cells < right)
            (void)yew_grid_puts(&ed->grid, row,
                                (u16)(right - footer_cells),
                                (const u8 *)footer, strlen(footer),
                                row_style.row_fg, row_style.row_bg,
                                row_style.attrs);
    }
}
