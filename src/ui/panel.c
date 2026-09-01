#include "ui/panel.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include "edit/ed.h"
#include "edit/theme_cmds.h"
#include "term/grid.h"
#include "ui/glyphs.h"
#include "ui/region.h"
#include "ui/win.h"
#include "unicode/grapheme.h"
#include "unicode/utf8.h"
#include "unicode/width.h"

enum { PANEL_TABWIDTH = 4 };

static const BindRow panel_keys[] = {
    {"<up>", "ed.ui.panel.move", -1, NULL},
    {"<down>", "ed.ui.panel.move", 1, NULL},
    {"<pgup>", "ed.ui.panel.move", -2, NULL},
    {"<pgdn>", "ed.ui.panel.move", 2, NULL},
};

static char *panel_strdup(const char *s)
{
    size_t n;
    char *copy;

    if (s == NULL)
        return NULL;
    n = strlen(s);
    copy = yew_xmalloc(n + 1U);
    (void)memcpy(copy, s, n + 1U);
    return copy;
}

static bool panel_space(const u8 *s, size_t n)
{
    u32 cp;

    if (n == 1U && (s[0] == ' ' || s[0] == '\t'))
        return true;
    (void)yew_utf8_decode(s, n, &cp);
    return cp == 0x00A0U || cp == 0x1680U ||
           (cp >= 0x2000U && cp <= 0x200AU) || cp == 0x202FU ||
           cp == 0x205FU || cp == 0x3000U;
}

static u16 panel_cluster_width(const u8 *s, size_t n, u16 col)
{
    int width;

    if (n == 1U && s[0] == '\t')
        return (u16)(PANEL_TABWIDTH - col % PANEL_TABWIDTH);
    width = yew_cluster_width(s, n);
    return width > 0 ? (u16)width : 0U;
}

static bool panel_fence_line(const u8 *body, u32 lo, u32 hi)
{
    u32 len = hi - lo;

    if (len != 0U && body[hi - 1U] == '\r')
        len--;
    return len == 3U && memcmp(body + lo, "```", 3U) == 0;
}

static void panel_push_line(Panel *p, u32 lo, u32 hi, u16 width)
{
    u32 start = lo;

    if (lo == hi) {
        Vec_Span_push(&p->rows, ((Span){lo, hi}));
        return;
    }
    while (start < hi) {
        u32 pos = start;
        u32 break_at = UINT32_MAX;
        u32 resume_at = UINT32_MAX;
        u16 cells = 0U;

        while (pos < hi) {
            size_t nextz = yew_gb_next_bytes(p->body, p->len, pos);
            u32 next;
            u16 cluster_w;

            if (nextz <= pos || nextz > hi)
                nextz = hi;
            next = (u32)nextz;
            cluster_w = panel_cluster_width(p->body + pos, next - pos,
                                            cells);
            if ((u32)cells + cluster_w > width) {
                if (panel_space(p->body + pos, next - pos)) {
                    break_at = pos;
                    resume_at = next;
                }
                break;
            }
            cells = (u16)(cells + cluster_w);
            if (panel_space(p->body + pos, next - pos)) {
                break_at = pos;
                resume_at = next;
            }
            pos = next;
        }
        if (pos == hi) {
            while (pos > start) {
                size_t prev = yew_gb_prev_bytes(p->body, start, pos);

                if (prev >= pos || !panel_space(p->body + prev, pos - prev))
                    break;
                pos = (u32)prev;
            }
            Vec_Span_push(&p->rows, ((Span){start, pos}));
            break;
        }
        if (break_at != UINT32_MAX && break_at > start) {
            Vec_Span_push(&p->rows, ((Span){start, break_at}));
            start = resume_at;
            while (start < hi) {
                size_t next = yew_gb_next_bytes(p->body, p->len, start);

                if (next <= start || next > hi ||
                    !panel_space(p->body + start, next - start))
                    break;
                start = (u32)next;
            }
        } else {
            if (pos == start) {
                size_t next = yew_gb_next_bytes(p->body, p->len, start);

                pos = next > start && next <= hi ? (u32)next : hi;
            }
            Vec_Span_push(&p->rows, ((Span){start, pos}));
            start = pos;
        }
    }
}

static void panel_wrap(Panel *p, u16 width)
{
    u32 at = 0U;

    p->rows.len = 0U;
    if (p->len == 0U) {
        Vec_Span_push(&p->rows, ((Span){0U, 0U}));
    } else {
        while (at < p->len) {
            u32 end = at;

            while (end < p->len && p->body[end] != '\n')
                end++;
            if (!panel_fence_line(p->body, at, end))
                panel_push_line(p, at, end, width);
            at = end < p->len ? end + 1U : end;
        }
        if (p->body[p->len - 1U] == '\n')
            Vec_Span_push(&p->rows, ((Span){p->len, p->len}));
    }
    p->nrows = p->rows.len > UINT32_MAX ? UINT32_MAX : (u32)p->rows.len;
}

static Rect panel_area(const Ed *ed, const Panel *p)
{
    Rect area = {0U, 0U, 0U, 0U};
    u32 right;
    u32 bottom;

    if (ed == NULL)
        return area;
    if (p != NULL && p->has_area) {
        area = p->area;
        if (area.x >= ed->grid.cols || area.y >= ed->grid.rows)
            return (Rect){0U, 0U, 0U, 0U};
        right = (u32)area.x + area.w;
        bottom = (u32)area.y + area.h;
        if (right > ed->grid.cols)
            area.w = (u16)(ed->grid.cols - area.x);
        if (bottom > ed->grid.rows)
            area.h = (u16)(ed->grid.rows - area.y);
        return area;
    }
    if (ed->win != NULL && ed->win->rect.w != 0U &&
        ed->win->rect.h != 0U)
        return ed->win->rect;
    area = (Rect){0U, 0U, ed->grid.cols, ed->grid.rows};
    return area;
}

static u16 panel_row_width(const Panel *p, Span row)
{
    int width;

    if (row.hi <= row.lo)
        return 0U;
    width = yew_str_width(p->body + row.lo, (size_t)(row.hi - row.lo),
                          PANEL_TABWIDTH);
    if (width <= 0)
        return 0U;
    return width > UINT16_MAX ? UINT16_MAX : (u16)width;
}

static bool panel_geometry(Ed *ed, Panel *p)
{
    Rect area = panel_area(ed, p);
    u16 content_cap;
    u16 widest = 0U;
    u16 height_cap;
    u16 desired_h;
    u16 above;
    u16 below;
    u16 room;
    u16 x;
    u16 y;
    u16 anchor_y;
    bool use_below;
    size_t i;
    u32 right;
    u32 bottom;

    if (area.w < 3U || area.h < 3U)
        return false;
    content_cap = YEW_PANEL_MAX_W;
    if (p->max_w != 0U && p->max_w < content_cap)
        content_cap = p->max_w;
    if (content_cap > area.w - 2U)
        content_cap = (u16)(area.w - 2U);
    if (content_cap == 0U)
        return false;
    panel_wrap(p, content_cap);
    for (i = 0U; i < p->rows.len; i++) {
        u16 row_w = panel_row_width(p, p->rows.data[i]);

        if (row_w > widest)
            widest = row_w;
    }
    if (widest == 0U)
        widest = 1U;
    if (widest > content_cap)
        widest = content_cap;
    height_cap = YEW_PANEL_MAX_H;
    if (p->max_h != 0U && p->max_h < height_cap)
        height_cap = p->max_h;
    if (height_cap < 3U)
        return false;
    desired_h = p->nrows >= UINT16_MAX - 2U ? UINT16_MAX :
                (u16)(p->nrows + 2U);
    if (desired_h > height_cap)
        desired_h = height_cap;
    if (desired_h > area.h)
        desired_h = area.h;

    right = (u32)area.x + area.w;
    bottom = (u32)area.y + area.h;
    if (p->place == YEW_PANEL_CENTER) {
        u16 panel_w = (u16)(widest + 2U);

        x = (u16)(area.x + (area.w - panel_w) / 2U);
        y = (u16)(area.y + (area.h - desired_h) / 2U);
        p->rect = (Rect){x, y, panel_w, desired_h};
        if (p->scroll + (u32)(p->rect.h - 2U) > p->nrows) {
            u32 last = p->nrows > p->rect.h - 2U ?
                       p->nrows - (p->rect.h - 2U) : 0U;

            p->scroll = last > UINT16_MAX ? UINT16_MAX : (u16)last;
        }
        return true;
    }
    anchor_y = p->anchor_y;
    if (anchor_y < area.y)
        anchor_y = area.y;
    else if ((u32)anchor_y >= bottom)
        anchor_y = (u16)(bottom - 1U);
    above = (u16)(anchor_y - area.y);
    below = (u32)anchor_y + 1U >= bottom ? 0U :
            (u16)(bottom - ((u32)anchor_y + 1U));

    if (p->place == YEW_PANEL_ABOVE) {
        if (above >= desired_h)
            use_below = false;
        else if (below >= desired_h)
            use_below = true;
        else
            use_below = below > above;
    } else if (p->place == YEW_PANEL_BELOW) {
        if (below >= desired_h)
            use_below = true;
        else if (above >= desired_h)
            use_below = false;
        else
            use_below = below >= above;
    } else if (below >= desired_h) {
        use_below = true;
    } else if (above >= desired_h) {
        use_below = false;
    } else {
        use_below = below >= above;
    }
    room = use_below ? below : above;
    if (room < 3U)
        return false;
    if (desired_h > room)
        desired_h = room;
    y = use_below ? (u16)(anchor_y + 1U) :
                    (u16)(anchor_y - desired_h);

    x = p->anchor_x < area.x ? area.x : p->anchor_x;
    if ((u32)x + widest + 2U > right)
        x = right > (u32)widest + 2U ?
                (u16)(right - widest - 2U) : area.x;
    p->rect = (Rect){x, y, (u16)(widest + 2U), desired_h};
    if (p->scroll + (u32)(p->rect.h - 2U) > p->nrows) {
        u32 last = p->nrows > p->rect.h - 2U ?
                   p->nrows - (p->rect.h - 2U) : 0U;

        p->scroll = last > UINT16_MAX ? UINT16_MAX : (u16)last;
    }
    return true;
}

bool yew_panel_open(Ed *ed, Panel *p, const PanelSpec *spec)
{
    size_t i;

    if (p == NULL || spec == NULL || (spec->len != 0U && spec->body == NULL) ||
        spec->place > YEW_PANEL_CENTER)
        return false;
    yew_panel_close(ed, p);
    if (!yew_keymap_build(&p->keys, "panel", panel_keys,
                          (u32)YEW_ARRAY_LEN(panel_keys)))
        return false;
    if (spec->len != 0U) {
        p->body = yew_xmalloc(spec->len);
        (void)memcpy(p->body, spec->body, spec->len);
    }
    p->len = spec->len;
    p->title = panel_strdup(spec->title);
    p->role = panel_strdup(spec->role);
    p->anchor_x = spec->x;
    p->anchor_y = spec->y;
    p->place = spec->place;
    p->max_w = spec->max_w;
    p->max_h = spec->max_h;
    p->area = spec->area;
    p->has_area = spec->has_area;
    if (spec->emph != NULL) {
        for (i = 0U; i < spec->emph->len; i++)
            Vec_Span_push(&p->emph, spec->emph->data[i]);
    }
    p->open = true;
    if (!panel_geometry(ed, p)) {
        yew_panel_close(ed, p);
        return false;
    }
    if (ed != NULL)
        ed->full_damage = true;
    return true;
}

bool yew_panel_mark(Panel *p, u32 buf_id, u64 buf_gen, Span span,
                    const char *role)
{
    char *copy;

    if (p == NULL || !p->open || buf_id == 0U || span.lo > span.hi ||
        role == NULL || role[0] == '\0')
        return false;
    copy = panel_strdup(role);
    yew_xfree(p->mark_role);
    p->mark_role = copy;
    p->mark = span;
    p->mark_buf_id = buf_id;
    p->mark_buf_gen = buf_gen;
    p->mark_live = span.lo != span.hi;
    return true;
}

void yew_panel_close(Ed *ed, Panel *p)
{
    if (p == NULL)
        return;
    yew_xfree(p->body);
    yew_xfree(p->title);
    yew_xfree(p->role);
    yew_xfree(p->mark_role);
    Vec_Span_free(&p->emph);
    Vec_Span_free(&p->rows);
    yew_keymap_free(&p->keys);
    (void)memset(p, 0, sizeof(*p));
    if (ed != NULL)
        ed->full_damage = true;
}

void yew_panel_resize(Ed *ed, Panel *p)
{
    if (p == NULL || !p->open)
        return;
    if (!panel_geometry(ed, p)) {
        yew_panel_close(ed, p);
        return;
    }
    if (ed != NULL)
        ed->full_damage = true;
}

static bool panel_scroll(Ed *ed, Panel *p, i64 action)
{
    u16 visible;
    u32 max_scroll;
    u32 next;
    u16 step;

    if (p == NULL || !p->open)
        return false;
    visible = p->rect.h > 2U ? (u16)(p->rect.h - 2U) : 0U;
    max_scroll = p->nrows > visible ? p->nrows - visible : 0U;
    if (max_scroll > UINT16_MAX)
        max_scroll = UINT16_MAX;
    step = visible == 0U ? 1U : visible;
    switch (action) {
    case -1:
        if (p->scroll != 0U)
            p->scroll--;
        break;
    case 1:
        if (p->scroll < max_scroll)
            p->scroll++;
        break;
    case -2:
        p->scroll = p->scroll > step ? (u16)(p->scroll - step) : 0U;
        break;
    case 2:
        next = (u32)p->scroll + step;
        p->scroll = (u16)(next < max_scroll ? next : max_scroll);
        break;
    default:
        return false;
    }
    if (ed != NULL)
        ed->full_damage = true;
    return true;
}

bool yew_panel_key(Ed *ed, Panel *p, const Key *k)
{
    const Binding *binding = NULL;
    KeyId id;
    KeyMatch match;

    if (p == NULL || !p->open || k == NULL || k->ev == YEW_KEY_RELEASE)
        return false;
    id = yew_keyid(*k);
    match = yew_keymap_lookup(&p->keys, &id, 1U, NULL, &binding);
    if (match != YEW_MATCH_FULL || binding == NULL) {
        yew_panel_close(ed, p);
        return false;
    }
    return panel_scroll(ed, p, binding->iarg);
}

CmdStatus yew_panel_cmd_move(CmdCtx *cx)
{
    if (cx == NULL || cx->ed == NULL || cx->win == NULL ||
        !panel_scroll(cx->ed, &cx->win->panel, cx->iarg))
        return YEW_CMD_ERR_STATE;
    return YEW_CMD_OK;
}

static Cell panel_style(const Ed *ed, const Panel *p, const Grid *g)
{
    const ThemeEnt *role = p->role == NULL ? NULL :
                           yew_theme_ui_tab(ed, p->role);
    Cell style = g->blank;

    if (role != NULL) {
        style.fg = role->fg;
        style.bg = role->bg;
        style.attrs = role->attrs;
    }
    return style;
}

static void panel_put(Grid *g, u16 row, u16 col, const char *glyph,
                      Cell style)
{
    (void)yew_grid_put(g, row, col, (const u8 *)glyph, strlen(glyph),
                       style.fg, style.bg, style.attrs);
}

static bool panel_emphasized(const Panel *p, u32 lo, u32 hi)
{
    size_t i;

    for (i = 0U; i < p->emph.len; i++)
        if (p->emph.data[i].lo < hi && p->emph.data[i].hi > lo)
            return true;
    return false;
}

static void panel_draw_border(const Panel *p, Grid *g, Cell style)
{
    u16 dx;
    u16 dy;
    u16 right = (u16)(p->rect.x + p->rect.w - 1U);
    u16 bottom = (u16)(p->rect.y + p->rect.h - 1U);

    panel_put(g, p->rect.y, p->rect.x,
              yew_glyph(YEW_GLYPH_BORDER_TL), style);
    panel_put(g, p->rect.y, right,
              yew_glyph(YEW_GLYPH_BORDER_TR), style);
    panel_put(g, bottom, p->rect.x,
              yew_glyph(YEW_GLYPH_BORDER_BL), style);
    panel_put(g, bottom, right,
              yew_glyph(YEW_GLYPH_BORDER_BR), style);
    for (dx = 1U; dx + 1U < p->rect.w; dx++) {
        panel_put(g, p->rect.y, (u16)(p->rect.x + dx),
                  yew_glyph(YEW_GLYPH_BORDER_H), style);
        panel_put(g, bottom, (u16)(p->rect.x + dx),
                  yew_glyph(YEW_GLYPH_BORDER_H), style);
    }
    for (dy = 1U; dy + 1U < p->rect.h; dy++) {
        panel_put(g, (u16)(p->rect.y + dy), p->rect.x,
                  yew_glyph(YEW_GLYPH_BORDER_V), style);
        panel_put(g, (u16)(p->rect.y + dy), right,
                  yew_glyph(YEW_GLYPH_BORDER_V), style);
    }
    if (p->title != NULL && p->rect.w > 4U) {
        size_t len = strlen(p->title);
        size_t keep = yew_str_clip((const u8 *)p->title, len,
                                   p->rect.w - 4U, NULL);

        panel_put(g, p->rect.y, (u16)(p->rect.x + 1U), " ", style);
        (void)yew_grid_puts(g, p->rect.y, (u16)(p->rect.x + 2U),
                            (const u8 *)p->title, keep, style.fg, style.bg,
                            style.attrs | YEW_ATTR_BOLD);
    }
    if (p->scroll != 0U)
        panel_put(g, (u16)(p->rect.y + 1U), right,
                  yew_glyph(YEW_GLYPH_SCROLL_UP), style);
    if (p->scroll + (u32)(p->rect.h - 2U) < p->nrows)
        panel_put(g, (u16)(bottom - 1U), right,
                  yew_glyph(YEW_GLYPH_DISCLOSE_OPEN), style);
}

void yew_panel_draw(Ed *ed, const Panel *p, Grid *g)
{
    Cell style;
    u16 row;
    u16 visible;
    u32 grid_right;
    u32 grid_bottom;

    if (p == NULL || g == NULL || !p->open || p->rect.w < 3U ||
        p->rect.h < 3U)
        return;
    grid_right = (u32)p->rect.x + p->rect.w;
    grid_bottom = (u32)p->rect.y + p->rect.h;
    if (grid_right > g->cols || grid_bottom > g->rows)
        return;
    style = panel_style(ed, p, g);
    for (row = 0U; row < p->rect.h; row++)
        yew_grid_fill(g, (u16)(p->rect.y + row), p->rect.x,
                      (u16)grid_right, style);
    panel_draw_border(p, g, style);
    visible = (u16)(p->rect.h - 2U);
    for (row = 0U; row < visible; row++) {
        u32 index = (u32)p->scroll + row;
        Span span;
        u32 at;
        u16 col;
        u16 limit = (u16)(p->rect.x + p->rect.w - 1U);
        bool have_base = false;

        if (index >= p->nrows)
            break;
        span = p->rows.data[index];
        at = (u32)span.lo;
        col = (u16)(p->rect.x + 1U);
        while (at < span.hi && col < limit) {
            size_t nextz = yew_gb_next_bytes(p->body, p->len, at);
            u32 next;
            u16 attrs = style.attrs;
            u16 cluster_w;
            u16 drawn;

            if (nextz <= at || nextz > span.hi)
                break;
            next = (u32)nextz;
            if (panel_emphasized(p, at, next))
                attrs |= YEW_ATTR_BOLD;
            if (next - at == 1U && p->body[at] == '\t') {
                u16 tab = panel_cluster_width(p->body + at, 1U,
                                              (u16)(col - p->rect.x - 1U));

                col = (u32)col + tab < limit ? (u16)(col + tab) : limit;
                at = next;
                have_base = false;
                continue;
            }
            cluster_w = panel_cluster_width(p->body + at, next - at,
                                            (u16)(col - p->rect.x - 1U));
            if (cluster_w == 0U) {
                if (have_base)
                    (void)yew_grid_put(g,
                        (u16)(p->rect.y + 1U + row), col,
                        p->body + at, next - at, style.fg, style.bg, attrs);
                at = next;
                continue;
            }
            if ((u32)col + cluster_w > limit)
                break;
            drawn = yew_grid_put(g, (u16)(p->rect.y + 1U + row), col,
                                 p->body + at, next - at, style.fg, style.bg,
                                 attrs);
            if (drawn <= col || drawn > limit)
                break;
            col = drawn;
            at = next;
            have_base = true;
        }
    }
    yew_region_add(YEW_REGION_BLOCK, p->rect, 0);
}
