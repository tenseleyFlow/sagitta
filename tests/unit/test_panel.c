#include "harness.h"

#include <stdint.h>
#include <string.h>

#include "edit/ed.h"
#include "term/grid.h"
#include "term/input.h"
#include "ui/draw.h"
#include "ui/glyphs.h"
#include "ui/panel.h"
#include "ui/region.h"
#include "unicode/grapheme.h"
#include "unicode/width.h"

static void panel_fixture(Ed *ed, u16 cols, u16 rows)
{
    yew_ed_init(ed);
    YEW_ASSERT(yew_ed_open_memory(ed, (const u8 *)"", 0U, "panel"));
    ed->win->rect = (Rect){0U, 0U, cols, rows};
    ed->win->vp.cols = cols;
    ed->win->vp.rows = rows;
}

static PanelSpec panel_spec(const u8 *body, u32 len, u16 x, u16 y,
                            PanelPlace place)
{
    PanelSpec spec = {0};

    spec.body = body;
    spec.len = len;
    spec.x = x;
    spec.y = y;
    spec.place = (u8)place;
    spec.role = "ui.panel";
    return spec;
}

static Key panel_key(u32 code)
{
    Key key = {0};

    key.kind = YEW_EV_KEY;
    key.ev = YEW_KEY_PRESS;
    key.code = code;
    return key;
}

static bool rect_inside(Rect inner, Rect outer)
{
    return inner.x >= outer.x && inner.y >= outer.y &&
           (u32)inner.x + inner.w <= (u32)outer.x + outer.w &&
           (u32)inner.y + inner.h <= (u32)outer.y + outer.h;
}

static void assert_cell_glyph(const Cell *cell, const char *glyph)
{
    size_t len = strlen(glyph);

    YEW_ASSERT((cell->flags & CELL_INTERNED) == 0U);
    YEW_ASSERT(len <= sizeof(cell->utf8));
    YEW_ASSERT_EQ_MEM(cell->utf8, glyph, len);
}

static bool span_edge_is_cluster(const u8 *body, size_t len, u64 edge)
{
    size_t at = 0U;

    if (edge == 0U)
        return true;
    while (at < len) {
        at = yew_gb_next_bytes(body, len, at);
        if (at == edge)
            return true;
        if (at > edge)
            return false;
    }
    return edge == len;
}

static void assert_wrapped_clusters(const Panel *p, const u8 *body,
                                    size_t len)
{
    size_t i;
    u64 covered = 0U;
    u16 inner = p->rect.w >= 2U ? (u16)(p->rect.w - 2U) : 0U;

    YEW_ASSERT(p->rows.len != 0U);
    for (i = 0U; i < p->rows.len; i++) {
        Span row = p->rows.data[i];

        YEW_ASSERT_EQ_U64(row.lo, covered);
        YEW_ASSERT(row.hi > row.lo);
        YEW_ASSERT(row.hi <= len);
        YEW_ASSERT(span_edge_is_cluster(body, len, row.lo));
        YEW_ASSERT(span_edge_is_cluster(body, len, row.hi));
        YEW_ASSERT(yew_str_width(body + row.lo,
                                 (size_t)(row.hi - row.lo), 4U) <= inner);
        covered = row.hi;
    }
    YEW_ASSERT_EQ_U64(covered, len);
}

void test_panel_placement_resolves_all_fit_combinations_at_three_sizes(void)
{
    static const struct {
        u16 cols;
        u16 rows;
    } sizes[] = {{16U, 8U}, {40U, 12U}, {100U, 30U}};
    static const u8 short_body[] = "abc";
    static const u8 tall_body[] =
        "0\n1\n2\n3\n4\n5\n6\n7\n8\n9\n10\n11\n12\n13\n14\n15";
    size_t i;

    for (i = 0U; i < YEW_ARRAY_LEN(sizes); i++) {
        Ed ed;
        PanelSpec spec;
        Panel *p;
        u16 anchor;

        panel_fixture(&ed, sizes[i].cols, sizes[i].rows);
        p = &ed.win->panel;

        /* Both sides fit: the preferred side wins. */
        anchor = (u16)(sizes[i].rows / 2U);
        spec = panel_spec(short_body, sizeof(short_body) - 1U, 2U, anchor,
                          YEW_PANEL_BELOW);
        YEW_ASSERT(yew_panel_open(&ed, p, &spec));
        YEW_ASSERT(p->rect.y > anchor);
        YEW_ASSERT(rect_inside(p->rect, ed.win->rect));

        spec.y = 0U; /* Below fits, above does not. */
        YEW_ASSERT(yew_panel_open(&ed, p, &spec));
        YEW_ASSERT(p->rect.y > spec.y);
        YEW_ASSERT(rect_inside(p->rect, ed.win->rect));

        spec.y = (u16)(sizes[i].rows - 1U); /* Above only. */
        YEW_ASSERT(yew_panel_open(&ed, p, &spec));
        YEW_ASSERT((u32)p->rect.y + p->rect.h <= spec.y);
        YEW_ASSERT(rect_inside(p->rect, ed.win->rect));

        /* Neither side fits: use the roomier side and make it scroll. */
        spec = panel_spec(tall_body, sizeof(tall_body) - 1U, 2U,
                          (u16)((sizes[i].rows - 1U) / 2U),
                          YEW_PANEL_BELOW);
        YEW_ASSERT(yew_panel_open(&ed, p, &spec));
        YEW_ASSERT(p->rect.y > spec.y);
        YEW_ASSERT(p->nrows > (u32)(p->rect.h - 2U));
        YEW_ASSERT(rect_inside(p->rect, ed.win->rect));

        /* Neither side fits: room beats an above preference. */
        spec.place = YEW_PANEL_ABOVE;
        YEW_ASSERT(yew_panel_open(&ed, p, &spec));
        YEW_ASSERT(p->rect.y > spec.y);
        YEW_ASSERT(rect_inside(p->rect, ed.win->rect));

        /* And the reverse: room beats a below preference. */
        spec.place = YEW_PANEL_BELOW;
        spec.y = (u16)(sizes[i].rows / 2U);
        YEW_ASSERT(yew_panel_open(&ed, p, &spec));
        YEW_ASSERT((u32)p->rect.y + p->rect.h <= spec.y);
        YEW_ASSERT(rect_inside(p->rect, ed.win->rect));
        yew_ed_free(&ed);
    }
}

void test_panel_explicit_limits_stay_inside_caps_and_resize(void)
{
    u8 body[256];
    u32 len = 100U;
    u32 i;
    Ed ed;
    PanelSpec spec;
    Rect before;

    (void)memset(body, 'x', len);
    for (i = 0U; i < 30U; i++) {
        body[len++] = '\n';
        body[len++] = 'x';
    }
    panel_fixture(&ed, 120U, 40U);
    spec = panel_spec(body, len, 5U, 20U, YEW_PANEL_BELOW);
    spec.max_w = UINT16_MAX;
    spec.max_h = UINT16_MAX;
    YEW_ASSERT(yew_panel_open(&ed, &ed.win->panel, &spec));
    YEW_ASSERT_EQ_U64(ed.win->panel.rect.w, YEW_PANEL_MAX_W + 2U);
    YEW_ASSERT_EQ_U64(ed.win->panel.rect.h, YEW_PANEL_MAX_H);
    before = ed.win->panel.rect;

    /* The old anchor is now below the pane; resize must clamp it first. */
    ed.win->rect = (Rect){0U, 0U, 40U, 10U};
    yew_panel_resize(&ed, &ed.win->panel);
    YEW_ASSERT(ed.win->panel.open);
    YEW_ASSERT(rect_inside(ed.win->panel.rect, ed.win->rect));
    YEW_ASSERT_EQ_U64(ed.win->panel.rect.w, ed.win->rect.w);
    YEW_ASSERT(ed.win->panel.rect.x != before.x ||
               ed.win->panel.rect.y != before.y ||
               ed.win->panel.rect.w != before.w ||
               ed.win->panel.rect.h != before.h);
    yew_ed_free(&ed);
}

void test_panel_wraps_cjk_only_at_grapheme_boundaries(void)
{
    static const u8 body[] =
        "\xE4\xBD\xA0\xE5\xA5\xBD\xE4\xB8\x96\xE7\x95\x8C"
        "\xE4\xBD\xA0\xE5\xA5\xBD\xE4\xB8\x96\xE7\x95\x8C";
    Ed ed;
    PanelSpec spec;

    panel_fixture(&ed, 24U, 10U);
    spec = panel_spec(body, sizeof(body) - 1U, 1U, 1U, YEW_PANEL_BELOW);
    spec.max_w = 6U;
    YEW_ASSERT(yew_panel_open(&ed, &ed.win->panel, &spec));
    assert_wrapped_clusters(&ed.win->panel, body, sizeof(body) - 1U);
    YEW_ASSERT(ed.win->panel.rows.len > 1U);
    yew_ed_free(&ed);
}

void test_panel_wraps_zwj_family_only_at_grapheme_boundaries(void)
{
    static const u8 family[] =
        "\xF0\x9F\x91\xA8\xE2\x80\x8D\xF0\x9F\x91\xA9\xE2\x80\x8D"
        "\xF0\x9F\x91\xA7\xE2\x80\x8D\xF0\x9F\x91\xA6";
    u8 body[(sizeof(family) - 1U) * 4U];
    Ed ed;
    PanelSpec spec;
    size_t i;

    for (i = 0U; i < 4U; i++)
        (void)memcpy(body + i * (sizeof(family) - 1U), family,
                     sizeof(family) - 1U);
    panel_fixture(&ed, 20U, 10U);
    spec = panel_spec(body, sizeof(body), 1U, 1U, YEW_PANEL_BELOW);
    spec.max_w = 5U;
    YEW_ASSERT(yew_panel_open(&ed, &ed.win->panel, &spec));
    assert_wrapped_clusters(&ed.win->panel, body, sizeof(body));
    YEW_ASSERT(ed.win->panel.rows.len > 1U);
    yew_ed_free(&ed);
}

void test_panel_hard_breaks_two_hundred_byte_token(void)
{
    u8 body[200];
    Ed ed;
    PanelSpec spec;

    (void)memset(body, 'x', sizeof(body));
    panel_fixture(&ed, 30U, 10U);
    spec = panel_spec(body, sizeof(body), 1U, 1U, YEW_PANEL_BELOW);
    spec.max_w = 12U;
    YEW_ASSERT(yew_panel_open(&ed, &ed.win->panel, &spec));
    assert_wrapped_clusters(&ed.win->panel, body, sizeof(body));
    YEW_ASSERT(ed.win->panel.rows.len >= 17U);
    yew_ed_free(&ed);
}

void test_panel_wrap_prefers_the_latest_fitting_whitespace(void)
{
    static const u8 body[] = "ab cd ef";
    Ed ed;
    PanelSpec spec;

    panel_fixture(&ed, 20U, 8U);
    spec = panel_spec(body, sizeof(body) - 1U, 1U, 1U, YEW_PANEL_BELOW);
    spec.max_w = 5U;
    YEW_ASSERT(yew_panel_open(&ed, &ed.win->panel, &spec));
    YEW_ASSERT_EQ_U64(ed.win->panel.rows.len, 2U);
    YEW_ASSERT_EQ_U64(ed.win->panel.rows.data[0].lo, 0U);
    YEW_ASSERT_EQ_U64(ed.win->panel.rows.data[0].hi, 5U);
    YEW_ASSERT_EQ_U64(ed.win->panel.rows.data[1].lo, 6U);
    YEW_ASSERT_EQ_U64(ed.win->panel.rows.data[1].hi, 8U);
    yew_ed_free(&ed);
}

void test_panel_width_uses_cells_and_draw_stays_inside_rect(void)
{
    static const u8 body[] =
        "\xE4\xBD\xA0\xE5\xA5\xBD\xE4\xB8\x96"; /* six cells */
    Ed ed;
    PanelSpec spec;
    Grid grid;
    Cell before[20U * 8U];
    Rect rect;
    u16 x;
    u16 y;

    panel_fixture(&ed, 20U, 8U);
    spec = panel_spec(body, sizeof(body) - 1U, 3U, 1U, YEW_PANEL_BELOW);
    YEW_ASSERT(yew_panel_open(&ed, &ed.win->panel, &spec));
    rect = ed.win->panel.rect;
    YEW_ASSERT_EQ_U64(rect.w, 8U); /* 6 display cells plus two borders. */
    YEW_ASSERT(yew_grid_init(&grid, &ed.interner, 8U, 20U));
    (void)memcpy(before, grid.back, sizeof(before));
    yew_region_frame_begin();
    yew_panel_draw(&ed, &ed.win->panel, &grid);
    for (y = 0U; y < grid.rows; y++) {
        for (x = 0U; x < grid.cols; x++) {
            bool inside = x >= rect.x && y >= rect.y &&
                          x < (u32)rect.x + rect.w &&
                          y < (u32)rect.y + rect.h;
            size_t at = (size_t)y * grid.cols + x;

            if (!inside)
                YEW_ASSERT(yew_cell_eq(&grid.back[at], &before[at]));
        }
    }
    yew_grid_free(&grid);
    yew_ed_free(&ed);
}

void test_panel_oversized_cluster_never_draws_outside_rect(void)
{
    static const u8 body[] = {0x80U};
    static const u8 leading_combining[] = {0xCCU, 0x81U, 'x'};
    Ed ed;
    PanelSpec spec;
    Grid grid;
    Cell before[12U * 8U];
    Rect rect;
    u16 x;
    u16 y;

    panel_fixture(&ed, 12U, 8U);
    spec = panel_spec(body, sizeof(body), 2U, 1U, YEW_PANEL_BELOW);
    spec.max_w = 2U;
    YEW_ASSERT(yew_panel_open(&ed, &ed.win->panel, &spec));
    rect = ed.win->panel.rect;
    YEW_ASSERT(yew_grid_init(&grid, &ed.interner, 8U, 12U));
    (void)memcpy(before, grid.back, sizeof(before));
    yew_region_frame_begin();
    yew_panel_draw(&ed, &ed.win->panel, &grid);
    for (y = 0U; y < grid.rows; y++) {
        for (x = 0U; x < grid.cols; x++) {
            bool inside = x >= rect.x && y >= rect.y &&
                          x < (u32)rect.x + rect.w &&
                          y < (u32)rect.y + rect.h;
            size_t at = (size_t)y * grid.cols + x;

            if (!inside)
                YEW_ASSERT(yew_cell_eq(&grid.back[at], &before[at]));
        }
    }
    assert_cell_glyph(&grid.back[(size_t)(rect.y + 1U) * grid.cols +
                                 rect.x + rect.w - 1U],
                      yew_glyph(YEW_GLYPH_BORDER_V));

    spec = panel_spec(leading_combining, sizeof(leading_combining), 2U, 1U,
                      YEW_PANEL_BELOW);
    spec.max_w = 2U;
    YEW_ASSERT(yew_panel_open(&ed, &ed.win->panel, &spec));
    rect = ed.win->panel.rect;
    yew_grid_clear(&grid);
    yew_region_frame_begin();
    yew_panel_draw(&ed, &ed.win->panel, &grid);
    assert_cell_glyph(&grid.back[(size_t)(rect.y + 1U) * grid.cols + rect.x],
                      yew_glyph(YEW_GLYPH_BORDER_V));
    assert_cell_glyph(&grid.back[(size_t)(rect.y + 1U) * grid.cols +
                                 rect.x + 1U], "x");
    yew_grid_free(&grid);
    yew_ed_free(&ed);
}

void test_panel_chrome_title_emphasis_and_scroll_markers(void)
{
    static const u8 body[] = "zero\none\ntwo\nthree";
    Ed ed;
    PanelSpec spec;
    Vec_Span emph = {0};
    Grid grid;
    Rect rect;
    size_t at;
    Key key;

    panel_fixture(&ed, 24U, 10U);
    Vec_Span_push(&emph, ((Span){0U, 4U}));
    spec = panel_spec(body, sizeof(body) - 1U, 2U, 1U, YEW_PANEL_BELOW);
    spec.title = "T";
    spec.max_h = 4U;
    spec.emph = &emph;
    YEW_ASSERT(yew_panel_open(&ed, &ed.win->panel, &spec));
    Vec_Span_free(&emph);
    rect = ed.win->panel.rect;
    YEW_ASSERT(yew_grid_init(&grid, &ed.interner, 10U, 24U));
    yew_region_frame_begin();
    yew_panel_draw(&ed, &ed.win->panel, &grid);

    at = (size_t)rect.y * grid.cols + rect.x;
    assert_cell_glyph(&grid.back[at], yew_glyph(YEW_GLYPH_BORDER_TL));
    at = (size_t)rect.y * grid.cols + rect.x + 2U;
    assert_cell_glyph(&grid.back[at], "T");
    YEW_ASSERT((grid.back[at].attrs & YEW_ATTR_BOLD) != 0U);
    at = (size_t)(rect.y + 1U) * grid.cols + rect.x + 1U;
    assert_cell_glyph(&grid.back[at], "z");
    YEW_ASSERT((grid.back[at].attrs & YEW_ATTR_BOLD) != 0U);
    at = (size_t)(rect.y + rect.h - 2U) * grid.cols +
         rect.x + rect.w - 1U;
    assert_cell_glyph(&grid.back[at], yew_glyph(YEW_GLYPH_DISCLOSE_OPEN));

    key = panel_key(YEW_KEY_DOWN);
    YEW_ASSERT(yew_panel_key(&ed, &ed.win->panel, &key));
    yew_grid_clear(&grid);
    yew_region_frame_begin();
    yew_panel_draw(&ed, &ed.win->panel, &grid);
    at = (size_t)(rect.y + 1U) * grid.cols + rect.x + rect.w - 1U;
    assert_cell_glyph(&grid.back[at], yew_glyph(YEW_GLYPH_SCROLL_UP));
    at = (size_t)(rect.y + rect.h - 2U) * grid.cols +
         rect.x + rect.w - 1U;
    assert_cell_glyph(&grid.back[at], yew_glyph(YEW_GLYPH_DISCLOSE_OPEN));
    yew_grid_free(&grid);
    yew_ed_free(&ed);
}

void test_panel_scroll_keys_consume_and_clamp(void)
{
    static const u8 body[] = "0\n1\n2\n3\n4\n5\n6\n7\n8\n9";
    Ed ed;
    PanelSpec spec;
    Key key;
    u16 page;

    panel_fixture(&ed, 20U, 7U);
    spec = panel_spec(body, sizeof(body) - 1U, 1U, 1U, YEW_PANEL_BELOW);
    spec.max_h = 5U;
    YEW_ASSERT(yew_panel_open(&ed, &ed.win->panel, &spec));
    page = (u16)(ed.win->panel.rect.h - 2U);
    key = panel_key(YEW_KEY_DOWN);
    YEW_ASSERT(yew_panel_key(&ed, &ed.win->panel, &key));
    YEW_ASSERT_EQ_U64(ed.win->panel.scroll, 1U);
    key = panel_key(YEW_KEY_UP);
    YEW_ASSERT(yew_panel_key(&ed, &ed.win->panel, &key));
    YEW_ASSERT_EQ_U64(ed.win->panel.scroll, 0U);
    key = panel_key(YEW_KEY_PAGE_DOWN);
    YEW_ASSERT(yew_panel_key(&ed, &ed.win->panel, &key));
    YEW_ASSERT_EQ_U64(ed.win->panel.scroll, page);
    key = panel_key(YEW_KEY_PAGE_UP);
    YEW_ASSERT(yew_panel_key(&ed, &ed.win->panel, &key));
    YEW_ASSERT_EQ_U64(ed.win->panel.scroll, 0U);
    key = panel_key(YEW_KEY_UP);
    YEW_ASSERT(yew_panel_key(&ed, &ed.win->panel, &key));
    YEW_ASSERT_EQ_U64(ed.win->panel.scroll, 0U);
    yew_ed_free(&ed);
}

void test_panel_unknown_key_closes_and_returns_for_redispatch(void)
{
    static const u8 body[] = "body";
    Ed ed;
    PanelSpec spec;
    Key key;

    panel_fixture(&ed, 20U, 8U);
    spec = panel_spec(body, sizeof(body) - 1U, 1U, 1U, YEW_PANEL_BELOW);
    YEW_ASSERT(yew_panel_open(&ed, &ed.win->panel, &spec));
    key = panel_key('x');
    YEW_ASSERT(!yew_panel_key(&ed, &ed.win->panel, &key));
    YEW_ASSERT(!ed.win->panel.open);

    YEW_ASSERT(yew_panel_open(&ed, &ed.win->panel, &spec));
    key = panel_key(YEW_KEY_ESCAPE);
    YEW_ASSERT(!yew_panel_key(&ed, &ed.win->panel, &key));
    YEW_ASSERT(!ed.win->panel.open);
    yew_ed_free(&ed);
}

void test_panel_editor_spine_redispatches_printable_key_after_close(void)
{
    static const u8 body[] = "body";
    Ed ed;
    PanelSpec spec;
    Key key;
    TextIter it;
    const u8 *bytes;
    u64 available;

    panel_fixture(&ed, 20U, 8U);
    spec = panel_spec(body, sizeof(body) - 1U, 1U, 1U, YEW_PANEL_BELOW);
    YEW_ASSERT(yew_panel_open(&ed, &ed.win->panel, &spec));
    YEW_ASSERT_EQ_I64(yew_mode_enter(&ed, YEW_MODE_I), YEW_CMD_OK);
    key = panel_key('x');
    key.ntext = 1U;
    key.text[0] = (u8)'x';
    yew_ed_handle_key(&ed, key, 1);
    YEW_ASSERT(!ed.win->panel.open);
    YEW_ASSERT_EQ_U64(yew_textbuf_len(ed.win->buf->tb), 1U);
    YEW_ASSERT(yew_textiter_begin(&it, ed.win->buf->tb, BYTEOFF(0U)));
    YEW_ASSERT(yew_textiter_chunk(&it, ed.win->buf->tb, &bytes, &available));
    YEW_ASSERT(available >= 1U);
    YEW_ASSERT_EQ_U64(bytes[0], (u8)'x');
    yew_ed_free(&ed);
}

void test_panel_draw_registers_exact_block_rect(void)
{
    static const u8 body[] = "body";
    Ed ed;
    PanelSpec spec;
    Grid grid;
    Region hit;
    Rect rect;

    panel_fixture(&ed, 30U, 10U);
    spec = panel_spec(body, sizeof(body) - 1U, 5U, 2U, YEW_PANEL_BELOW);
    YEW_ASSERT(yew_panel_open(&ed, &ed.win->panel, &spec));
    rect = ed.win->panel.rect;
    YEW_ASSERT(yew_grid_init(&grid, &ed.interner, 10U, 30U));
    yew_region_frame_begin();
    yew_panel_draw(&ed, &ed.win->panel, &grid);
    YEW_ASSERT_EQ_U64(yew_region_count(), 1U);
    hit = yew_region_hit(rect.x, rect.y);
    YEW_ASSERT_EQ_I64(hit.kind, YEW_REGION_BLOCK);
    YEW_ASSERT_EQ_U64(hit.rect.x, rect.x);
    YEW_ASSERT_EQ_U64(hit.rect.y, rect.y);
    YEW_ASSERT_EQ_U64(hit.rect.w, rect.w);
    YEW_ASSERT_EQ_U64(hit.rect.h, rect.h);
    hit = yew_region_hit((u16)(rect.x + rect.w - 1U),
                         (u16)(rect.y + rect.h - 1U));
    YEW_ASSERT_EQ_I64(hit.kind, YEW_REGION_BLOCK);
    if (rect.x != 0U)
        YEW_ASSERT_EQ_I64(yew_region_hit((u16)(rect.x - 1U), rect.y).kind,
                          YEW_REGION_NONE);
    yew_grid_free(&grid);
    yew_ed_free(&ed);
}

void test_panel_second_open_replaces_existing_content(void)
{
    static const u8 first[] = "old\ncontent";
    static const u8 second[] = "new";
    Ed ed;
    PanelSpec spec;
    Rect old_rect;

    panel_fixture(&ed, 30U, 10U);
    spec = panel_spec(first, sizeof(first) - 1U, 1U, 1U,
                      YEW_PANEL_BELOW);
    YEW_ASSERT(yew_panel_open(&ed, &ed.win->panel, &spec));
    YEW_ASSERT_EQ_U64(ed.win->panel.nrows, 2U);
    old_rect = ed.win->panel.rect;

    spec = panel_spec(second, sizeof(second) - 1U, 20U, 8U,
                      YEW_PANEL_ABOVE);
    YEW_ASSERT(yew_panel_open(&ed, &ed.win->panel, &spec));
    YEW_ASSERT(ed.win->panel.open);
    YEW_ASSERT_EQ_U64(ed.win->panel.nrows, 1U);
    YEW_ASSERT_EQ_U64(ed.win->panel.rows.len, 1U);
    YEW_ASSERT_EQ_U64(ed.win->panel.rows.data[0].lo, 0U);
    YEW_ASSERT_EQ_U64(ed.win->panel.rows.data[0].hi, sizeof(second) - 1U);
    YEW_ASSERT(ed.win->panel.rect.x != old_rect.x ||
               ed.win->panel.rect.y != old_rect.y);
    YEW_ASSERT_EQ_U64(ed.win->panel.scroll, 0U);
    yew_ed_free(&ed);
}

void test_panel_drops_only_standalone_code_fence_lines(void)
{
    static const u8 body[] = "alpha\n```\nbeta\n```c\ngamma";
    static const Span want[] = {
        {0U, 5U}, {10U, 14U}, {15U, 19U}, {20U, 25U},
    };
    Ed ed;
    PanelSpec spec;
    size_t i;

    panel_fixture(&ed, 40U, 12U);
    spec = panel_spec(body, sizeof(body) - 1U, 1U, 1U, YEW_PANEL_BELOW);
    YEW_ASSERT(yew_panel_open(&ed, &ed.win->panel, &spec));
    YEW_ASSERT_EQ_U64(ed.win->panel.nrows, YEW_ARRAY_LEN(want));
    YEW_ASSERT_EQ_U64(ed.win->panel.rows.len, YEW_ARRAY_LEN(want));
    for (i = 0U; i < YEW_ARRAY_LEN(want); i++) {
        YEW_ASSERT_EQ_U64(ed.win->panel.rows.data[i].lo, want[i].lo);
        YEW_ASSERT_EQ_U64(ed.win->panel.rows.data[i].hi, want[i].hi);
    }
    yew_ed_free(&ed);
}

void test_panel_mark_is_owned_drawn_and_cleared_with_panel(void)
{
    static const u8 text[] = "alpha beta\n";
    static const u8 body[] = "hover";
    Ed ed;
    PanelSpec spec;
    Grid grid;
    size_t at;

    yew_ed_init(&ed);
    YEW_ASSERT(yew_ed_open_memory(&ed, text, sizeof(text) - 1U,
                                  "panel-mark.c"));
    ed.win->rect = (Rect){0U, 0U, 24U, 8U};
    ed.win->vp.cols = 24U;
    ed.win->vp.rows = 8U;
    spec = panel_spec(body, sizeof(body) - 1U, 1U, 1U,
                      YEW_PANEL_BELOW);
    YEW_ASSERT(yew_panel_open(&ed, &ed.win->panel, &spec));
    YEW_ASSERT(yew_panel_mark(&ed.win->panel, ed.win->buf->id,
                              ed.win->buf->tb->gen, (Span){6U, 10U},
                              "lsp.hover_range"));
    YEW_ASSERT_EQ_STR(ed.win->panel.mark_role, "lsp.hover_range");
    YEW_ASSERT(yew_grid_init(&grid, &ed.interner, 8U, 24U));
    ed.grid = grid;
    yew_draw_document_rows(&ed, ed.win, 0U, ed.win->rect.h);
    at = 6U;
    YEW_ASSERT((ed.grid.back[at].attrs & YEW_ATTR_UNDERLINE) != 0U);
    yew_panel_close(&ed, &ed.win->panel);
    YEW_ASSERT(!ed.win->panel.mark_live);
    YEW_ASSERT_NULL(ed.win->panel.mark_role);
    yew_grid_free(&ed.grid);
    (void)memset(&ed.grid, 0, sizeof(ed.grid));
    yew_ed_free(&ed);
}
