/* Sprint 43: ghost rows overlay document rows without moving the viewport. */
#include "harness.h"

#include <string.h>

#include "edit/ed.h"
#include "edit/shadow.h"
#include "term/grid.h"
#include "ui/draw.h"
#include "ui/region.h"
#include "ui/shadowdraw.h"

static void shadow_layout_fixture(Ed *ed, const u8 *bytes, size_t len,
                                  u64 cursor, Rect rect)
{
    Cursor *primary;

    yew_ed_init(ed);
    YEW_ASSERT(yew_ed_open_memory(ed, bytes, len, "shadow-layout"));
    ed->win->rect = rect;
    ed->win->gutter_width = rect.x;
    ed->win->vp.rows = rect.h;
    ed->win->vp.cols = rect.w;
    primary = &ed->win->cs.curs.data[ed->win->cs.primary];
    primary->pos = BYTEOFF(cursor);
    primary->anchor = BYTEOFF(cursor);
}

static void shadow_layout_suggestion(Win *win, const u8 *text, u32 len,
                                     u8 provider)
{
    win->shadow.live = true;
    win->shadow.sug.prov = provider;
    win->shadow.sug.buf_id = win->buf->id;
    win->shadow.sug.buf_gen = win->buf->tb->gen;
    win->shadow.sug.pos =
        win->cs.curs.data[win->cs.primary].pos;
    win->shadow.sug.text = text;
    win->shadow.sug.len = len;
}

static const Cell *shadow_cell(const Ed *ed, u16 row, u16 col)
{
    return &ed->grid.back[(size_t)row * ed->grid.cols + col];
}

void test_shadow_layout_geometry_is_exact_and_viewport_is_unchanged(void)
{
    static const u8 document[] = "abc\nx\ny\nz\n";
    static const u8 ghost[] = "one\ntwo\nthree";
    Ed ed;
    ShadowLayout layout;
    LineNo top;
    u32 top_sub;

    shadow_layout_fixture(&ed, document, sizeof(document) - 1U, 3U,
                          (Rect){6U, 2U, 10U, 5U});
    shadow_layout_suggestion(ed.win, ghost, sizeof(ghost) - 1U,
                             YEW_SHADOW_INDEX);
    top = ed.win->vp.top;
    top_sub = ed.win->vp.top_sub;
    yew_shadow_layout(ed.win, &ed.win->shadow, &layout);

    YEW_ASSERT_EQ_U64(layout.inline_run.x, 9U);
    YEW_ASSERT_EQ_U64(layout.inline_run.y, 2U);
    YEW_ASSERT_EQ_U64(layout.inline_run.w, 7U);
    YEW_ASSERT_EQ_U64(layout.inline_run.h, 1U);
    YEW_ASSERT_EQ_U64(layout.vrows.x, 6U);
    YEW_ASSERT_EQ_U64(layout.vrows.y, 3U);
    YEW_ASSERT_EQ_U64(layout.vrows.w, 10U);
    YEW_ASSERT_EQ_U64(layout.vrows.h, 2U);
    YEW_ASSERT_EQ_U64(layout.nlines, 3U);
    YEW_ASSERT(!layout.clipped);
    YEW_ASSERT_EQ_U64(ed.win->vp.top.v, top.v);
    YEW_ASSERT_EQ_U64(ed.win->vp.top_sub, top_sub);
    yew_ed_free(&ed);
}

void test_shadow_layout_clamps_forty_lines_to_eight(void)
{
    static const u8 document[] = "abc\n";
    u8 ghost[79U];
    Ed ed;
    ShadowLayout layout;
    u32 i;

    for (i = 0U; i < 40U; i++) {
        ghost[i * 2U] = 'x';
        if (i + 1U < 40U)
            ghost[i * 2U + 1U] = '\n';
    }
    shadow_layout_fixture(&ed, document, sizeof(document) - 1U, 3U,
                          (Rect){6U, 0U, 20U, 12U});
    shadow_layout_suggestion(ed.win, ghost, sizeof(ghost),
                             YEW_SHADOW_AI);
    yew_shadow_layout(ed.win, &ed.win->shadow, &layout);

    YEW_ASSERT_EQ_U64(layout.nlines, YEW_SHADOW_MAX_LINES);
    YEW_ASSERT(layout.clipped);
    YEW_ASSERT_EQ_U64(layout.inline_run.y, 0U);
    YEW_ASSERT_EQ_U64(layout.vrows.y, 1U);
    YEW_ASSERT_EQ_U64(layout.vrows.h, YEW_SHADOW_MAX_LINES - 1U);
    yew_ed_free(&ed);
}

void test_shadow_layout_last_visible_row_and_inline_only_clip(void)
{
    static const u8 document[] = "a\nb\nc\nd\ne\n";
    static const u8 ghost[] = "one\ntwo\nthree";
    Ed ed;
    ShadowLayout layout;

    shadow_layout_fixture(&ed, document, sizeof(document) - 1U, 8U,
                          (Rect){4U, 0U, 12U, 5U});
    shadow_layout_suggestion(ed.win, ghost, sizeof(ghost) - 1U,
                             YEW_SHADOW_LSP);
    yew_shadow_layout(ed.win, &ed.win->shadow, &layout);
    YEW_ASSERT_EQ_U64(layout.nlines, 1U);
    YEW_ASSERT(layout.clipped);
    YEW_ASSERT_EQ_U64(layout.inline_run.y, 4U);
    YEW_ASSERT_EQ_U64(layout.vrows.h, 0U);

    ed.win->cs.curs.data[0].pos = BYTEOFF(0U);
    ed.win->cs.curs.data[0].anchor = BYTEOFF(0U);
    ed.win->shadow.max_lines = 1U;
    yew_shadow_layout(ed.win, &ed.win->shadow, &layout);
    YEW_ASSERT_EQ_U64(layout.nlines, 1U);
    YEW_ASSERT(layout.clipped);
    YEW_ASSERT_EQ_U64(layout.vrows.h, 0U);
    yew_ed_free(&ed);
}

void test_shadow_draw_overlays_exact_damage_blocks_hits_and_clips(void)
{
    static const u8 document[] = "abc\nREAL1\nREAL2\nREAL3\n";
    static const u8 ghost[] = "one\ntwo\nthree\nfour\nfive";
    static const u8 ellipsis[] = "\xE2\x80\xA6";
    Ed ed;
    ShadowLayout layout;
    LineNo top;
    u32 top_sub;
    u16 row;

    shadow_layout_fixture(&ed, document, sizeof(document) - 1U, 3U,
                          (Rect){6U, 1U, 10U, 4U});
    YEW_ASSERT(yew_grid_init(&ed.grid, &ed.interner, 6U, 16U));
    ed.grid_ready = true;
    yew_draw_document_rows(&ed, ed.win, 0U, ed.win->rect.h);
    yew_grid_flip(&ed.grid);
    shadow_layout_suggestion(ed.win, ghost, sizeof(ghost) - 1U,
                             YEW_SHADOW_LSP);
    top = ed.win->vp.top;
    top_sub = ed.win->vp.top_sub;
    yew_region_frame_begin();
    yew_shadow_layout(ed.win, &ed.win->shadow, &layout);
    yew_shadow_draw(&ed, ed.win, &layout, &ed.grid);

    YEW_ASSERT_EQ_U64(layout.nlines, 4U);
    YEW_ASSERT(layout.clipped);
    YEW_ASSERT_EQ_U64(shadow_cell(&ed, 1U, 9U)->utf8[0], (u8)'o');
    YEW_ASSERT_EQ_U64(shadow_cell(&ed, 2U, 6U)->utf8[0], (u8)'t');
    YEW_ASSERT_EQ_U64(shadow_cell(&ed, 3U, 6U)->utf8[0], (u8)'t');
    YEW_ASSERT_EQ_U64(shadow_cell(&ed, 4U, 6U)->utf8[0], (u8)'f');
    YEW_ASSERT_EQ_MEM(shadow_cell(&ed, 4U, 15U)->utf8, ellipsis,
                      sizeof(ellipsis) - 1U);
    for (row = 1U; row < 5U; row++) {
        const Cell *glyph = shadow_cell(&ed, row, 1U);

        YEW_ASSERT_EQ_U64(glyph->utf8[0], (u8)'l');
        YEW_ASSERT((glyph->attrs & YEW_ATTR_DIM) != 0U);
        YEW_ASSERT((glyph->attrs & YEW_ATTR_ITALIC) != 0U);
        YEW_ASSERT((glyph->attrs & YEW_ATTR_UNDERLINE) == 0U);
    }
    YEW_ASSERT_EQ_U64(ed.grid.dmg_lo, 1U);
    YEW_ASSERT_EQ_U64(ed.grid.dmg_hi, 5U);
    YEW_ASSERT_EQ_U64(yew_region_count(), 2U);
    YEW_ASSERT_EQ_U64(yew_region_hit(10U, 1U).kind, YEW_REGION_BLOCK);
    YEW_ASSERT_EQ_U64(yew_region_hit(7U, 3U).kind, YEW_REGION_BLOCK);
    YEW_ASSERT_EQ_U64(ed.win->vp.top.v, top.v);
    YEW_ASSERT_EQ_U64(ed.win->vp.top_sub, top_sub);
    YEW_ASSERT_EQ_U64(ed.win->shadow.draw_row, 0U);
    YEW_ASSERT_EQ_U64(ed.win->shadow.vrows, 4U);
    yew_ed_free(&ed);
}

void test_shadow_draw_provenance_attrs_and_glyphs_are_distinct(void)
{
    static const u8 document[] = "abc\n";
    static const u8 ghost[] = "x";
    static const u8 glyphs[YEW_SHADOW_NPROV] = {'s', 'l', 'a'};
    static const u16 attrs[YEW_SHADOW_NPROV] = {
        YEW_ATTR_DIM,
        YEW_ATTR_DIM | YEW_ATTR_ITALIC,
        YEW_ATTR_DIM | YEW_ATTR_ITALIC | YEW_ATTR_UNDERLINE,
    };
    u32 prov;

    for (prov = 0U; prov < (u32)YEW_SHADOW_NPROV; prov++) {
        Ed ed;
        ShadowLayout layout;
        const Cell *text;
        const Cell *glyph;

        shadow_layout_fixture(&ed, document, sizeof(document) - 1U, 3U,
                              (Rect){4U, 0U, 8U, 2U});
        YEW_ASSERT(yew_grid_init(&ed.grid, &ed.interner, 2U, 12U));
        ed.grid_ready = true;
        shadow_layout_suggestion(ed.win, ghost, sizeof(ghost) - 1U,
                                 (u8)prov);
        yew_region_frame_begin();
        yew_shadow_layout(ed.win, &ed.win->shadow, &layout);
        yew_shadow_draw(&ed, ed.win, &layout, &ed.grid);
        text = shadow_cell(&ed, 0U, 7U);
        glyph = shadow_cell(&ed, 0U, 1U);
        YEW_ASSERT_EQ_U64(text->utf8[0], (u8)'x');
        YEW_ASSERT_EQ_U64(glyph->utf8[0], glyphs[prov]);
        YEW_ASSERT_EQ_U64(text->attrs & attrs[prov], attrs[prov]);
        YEW_ASSERT_EQ_U64(glyph->attrs & attrs[prov], attrs[prov]);
        yew_ed_free(&ed);
    }
}
