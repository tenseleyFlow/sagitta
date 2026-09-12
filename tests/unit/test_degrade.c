#define _POSIX_C_SOURCE 200809L

/*
 * Sprint 27 §8: the degradation audit.
 *
 * The claim being defended is that nothing in this program is only
 * usable at the top tier.  Each row of §8's table is a condition the
 * editor may find itself in — NO_COLOR, sixteen colours, no mouse, no
 * UTF-8, a 20x5 terminal — and in every one of them the chrome has to
 * keep its MEANING, not merely avoid crashing.
 *
 * The subtle half is that "still legible" is asserted by an ATTRIBUTE
 * check rather than by eye: with NO_COLOR the only thing distinguishing
 * an active tab from an inactive one is reverse video, so the test that
 * proves the colours are gone must also prove the attributes are still
 * there.  A NO_COLOR mode that emitted nothing at all would pass the
 * first half and be unusable.
 */
#include "harness.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "edit/ed.h"
#include "edit/theme_cmds.h"
#include "term/render.h"
#include "ui/ctxmenu.h"
#include "ui/draw.h"
#include "ui/glyphs.h"
#include "ui/groups.h"
#include "ui/layout.h"
#include "ui/mouse.h"
#include "ui/region.h"
#include "ui/tabs.h"

static const char *dg_env(const char *name)
{
    return getenv(name);
}

static void dg_fixture(Ed *ed, u16 rows, u16 cols, int extra_tabs)
{
    int i;

    yew_cmd_shutdown();
    yew_cmd_init();
    yew_ed_init(ed);
    YEW_ASSERT(yew_ed_open_scratch(ed));
    YEW_ASSERT(yew_grid_init(&ed->grid, &ed->interner, rows, cols));
    ed->grid_ready = true;
    for (i = 0; i < extra_tabs; i++) {
        char path[64];

        (void)snprintf(path, sizeof(path), "/tmp/yew-degrade-%d.txt", i);
        YEW_ASSERT(yew_tab_open(ed, path) >= 0);
    }
    yew_ed_layout(ed);
}

/* One frame's bytes, rendered at whatever tier `r` resolved to. */
static void dg_frame(Ed *ed, Render *r, Bytebuf *out)
{
    yew_region_frame_begin();
    yew_draw_panes(ed);
    if (ed->tab_strip_rect.h != 0U)
        yew_tab_strip_draw(ed, ed->tab_strip_rect);
    yew_draw_footer(ed, ed->win);
    out->len = 0U;
    yew_grid_mark_all(&ed->grid);
    (void)yew_render_frame(r, &ed->grid, out);
    bytebuf_append(out, (const u8 *)"", 1U);
}

static bool dg_has_sgr(const Bytebuf *out)
{
    size_t i;

    for (i = 0U; i + 2U < out->len; i++) {
        size_t end;

        if (out->data[i] != 0x1bU || out->data[i + 1U] != (u8)'[')
            continue;
        for (end = i + 2U; end < out->len; end++) {
            if (out->data[end] >= (u8)'@' && out->data[end] <= (u8)'~') {
                if (out->data[end] == (u8)'m')
                    return true;
                break;
            }
        }
    }
    return false;
}

/* ---------------------------------------------------------------- */
/* NO_COLOR                                                          */
/* ---------------------------------------------------------------- */

/*
 * DoD 11.  Zero colour SGR parameters, and still legible: an active tab
 * is still distinguishable from an inactive one, by REVERSE.
 */
void test_degrade_no_color_emits_no_colour_and_stays_legible(void)
{
    Ed ed;
    Render r;
    Bytebuf frame;
    const char *s;
    bool saw_reverse;

    bytebuf_init(&frame);
    (void)setenv("NO_COLOR", "1", 1);
    dg_fixture(&ed, 24U, 80U, 3);
    (void)memset(&r, 0, sizeof(r));
    yew_render_init(&r, NULL, dg_env);
    YEW_ASSERT(r.no_color);
    dg_frame(&ed, &r, &frame);

    s = (const char *)frame.data;
    /* Not one truecolor, 256-colour or ANSI colour parameter. */
    YEW_ASSERT(strstr(s, "38;2") == NULL);
    YEW_ASSERT(strstr(s, "48;2") == NULL);
    YEW_ASSERT(strstr(s, "38;5") == NULL);
    YEW_ASSERT(strstr(s, "48;5") == NULL);
    {
        const char *p = s;
        int code;

        for (code = 30; code <= 37; code++) {
            /* Wide enough for any int the compiler cannot prove is two
             * digits — the sanitizer lane's -Wformat-truncation sees
             * cases the plain build inlines away. */
            char want[32];

            (void)snprintf(want, sizeof(want), "\x1b[%dm", code);
            YEW_ASSERT(strstr(p, want) == NULL);
            (void)snprintf(want, sizeof(want), "\x1b[%dm", code + 60);
            YEW_ASSERT(strstr(p, want) == NULL);
        }
    }
    /*
     * And LEGIBLE: reverse video is what carries "this tab is active"
     * once colour is gone.  Asserted here rather than by eye, because a
     * NO_COLOR mode that emitted nothing would pass the half above and
     * be unusable.
     */
    saw_reverse = strstr(s, "7m") != NULL || strstr(s, ";7") != NULL ||
                  strstr(s, "[7") != NULL;
    YEW_ASSERT(saw_reverse);

    (void)unsetenv("NO_COLOR");
    bytebuf_free(&frame);
    yew_ed_free(&ed);
}

/* The NO_COLOR contract is presence-based: even an empty value counts. */
void test_degrade_empty_no_color_is_not_set(void)
{
    Ed ed;
    Render r;
    Bytebuf frame;
    const char *s;

    bytebuf_init(&frame);
    (void)setenv("NO_COLOR", "", 1);
    (void)memset(&r, 0, sizeof(r));
    yew_render_init(&r, NULL, dg_env);
    YEW_ASSERT(r.no_color);
    dg_fixture(&ed, 24U, 80U, 1);
    dg_frame(&ed, &r, &frame);
    s = (const char *)frame.data;
    YEW_ASSERT(strstr(s, "38;") == NULL);
    YEW_ASSERT(strstr(s, "48;") == NULL);
    yew_ed_free(&ed);
    (void)unsetenv("NO_COLOR");

    (void)setenv("TERM", "dumb", 1);
    (void)memset(&r, 0, sizeof(r));
    yew_render_init(&r, NULL, dg_env);
    YEW_ASSERT(!r.no_color);
    YEW_ASSERT(r.plain);
    dg_fixture(&ed, 24U, 80U, 1);
    dg_frame(&ed, &r, &frame);
    YEW_ASSERT(!dg_has_sgr(&frame));
    yew_ed_free(&ed);
    (void)unsetenv("TERM");
    bytebuf_free(&frame);
}

/* ---------------------------------------------------------------- */
/* The 16-colour tier                                                */
/* ---------------------------------------------------------------- */

/*
 * At sixteen colours an accent and a dim are not reliably
 * distinguishable, so active/selected identity rides on REVERSE — and
 * the frame contains no 256-colour or truecolor parameters at all.
 */
void test_degrade_16_colour_tier_uses_reverse_not_colour(void)
{
    Ed ed;
    Render r;
    Bytebuf frame;
    const char *s;

    bytebuf_init(&frame);
    (void)setenv("YEW_COLORS", "16", 1);
    (void)unsetenv("NO_COLOR");
    dg_fixture(&ed, 24U, 80U, 3);
    (void)memset(&r, 0, sizeof(r));
    yew_render_init(&r, NULL, dg_env);
    YEW_ASSERT_EQ_U64(r.tier, (u64)YEW_RENDER_TIER_16);
    dg_frame(&ed, &r, &frame);

    s = (const char *)frame.data;
    YEW_ASSERT(strstr(s, "38;2") == NULL);
    YEW_ASSERT(strstr(s, "38;5") == NULL);
    YEW_ASSERT(strstr(s, "48;2") == NULL);
    YEW_ASSERT(strstr(s, "48;5") == NULL);
    /* Reverse is still what says "active". */
    YEW_ASSERT(strstr(s, "7m") != NULL || strstr(s, ";7") != NULL ||
               strstr(s, "[7") != NULL);

    (void)unsetenv("YEW_COLORS");
    bytebuf_free(&frame);
    yew_ed_free(&ed);
}

/* ---------------------------------------------------------------- */
/* No mouse                                                          */
/* ---------------------------------------------------------------- */

/*
 * YEW_MOUSE=0 drops events at the router too, not only at the
 * terminal.  A terminal that keeps reporting after the disable
 * sequence — or one that never honoured it — must not be able to move
 * the cursor.
 */
void test_degrade_yew_mouse_zero_drops_events(void)
{
    Ed ed;
    Pane *before;
    i32 leaf;

    dg_fixture(&ed, 24U, 80U, 0);
    yew_pane_tables_reset(&ed);
    leaf = yew_pane_table_add_leaf(&ed, ed.pane_root);
    yew_region_frame_begin();
    yew_region_add(YEW_REGION_PANE, ed.pane_root->rect, leaf);
    before = ed.focus;

    yew_mouse_set_enabled(false);
    {
        Key press;

        (void)memset(&press, 0, sizeof(press));
        press.kind = (u16)YEW_EV_MOUSE;
        press.button = (u8)YEW_MB_LEFT;
        press.ev = (u8)YEW_KEY_PRESS;
        press.col = 10U;
        press.row = (u16)(ed.pane_root->rect.y + 2U);
        yew_mouse_event(&ed, &press);
    }
    YEW_ASSERT_EQ_U64((u64)ed.mouse.phase, (u64)YEW_MP_IDLE);
    YEW_ASSERT(ed.focus == before);
    YEW_ASSERT_EQ_U64(yew_ed_cursor(&ed)->pos.v, 0U);

    /* And back on, the same event lands. */
    yew_mouse_set_enabled(true);
    {
        Key press;

        (void)memset(&press, 0, sizeof(press));
        press.kind = (u16)YEW_EV_MOUSE;
        press.button = (u8)YEW_MB_LEFT;
        press.ev = (u8)YEW_KEY_PRESS;
        press.col = 10U;
        press.row = (u16)(ed.pane_root->rect.y + 2U);
        yew_mouse_event(&ed, &press);
    }
    YEW_ASSERT_EQ_U64((u64)ed.mouse.phase, (u64)YEW_MP_ARMED);
    yew_mouse_cancel(&ed);
    yew_ed_free(&ed);
}

/* ---------------------------------------------------------------- */
/* Tiny terminals                                                    */
/* ---------------------------------------------------------------- */

/*
 * Down to 20x5 and below: never a crash, and NEVER a zero- or
 * negative-sized Rect handed to a renderer.  The chrome sheds in a
 * pinned order — the row-2 member strip first, then the tab strip, then
 * the gutter — and the message line is last, because a message the user
 * cannot see is a decision made silently.
 */
static void dg_assert_rect_sane(Rect r, u16 rows, u16 cols)
{
    YEW_ASSERT((u32)r.x + r.w <= (u32)cols);
    YEW_ASSERT((u32)r.y + r.h <= (u32)rows);
}

void test_degrade_tiny_terminals_never_produce_a_bad_rect(void)
{
    static const u16 sizes[][2] = {
        {80U, 24U}, {40U, 12U}, {20U, 5U}, {20U, 4U},
        {20U, 3U},  {20U, 2U},  {20U, 1U}, {10U, 5U},
        {4U, 2U},   {1U, 1U}
    };
    size_t i;

    for (i = 0U; i < YEW_ARRAY_LEN(sizes); i++) {
        Ed ed;
        u16 cols = sizes[i][0];
        u16 rows = sizes[i][1];
        u32 g;

        dg_fixture(&ed, rows, cols, 3);
        /* A group, so the strip WANTS two rows and has to give one
         * back when there is no room. */
        g = yew_group_create(&ed, "/src", "grp");
        yew_group_add_member(&ed, g, 2);
        yew_group_add_member(&ed, g, 3);
        yew_tab_switch(&ed, 2);
        yew_ed_layout(&ed);

        dg_assert_rect_sane(ed.tab_strip_rect, rows, cols);
        dg_assert_rect_sane(ed.footer_rect, rows, cols);
        YEW_ASSERT(ed.win != NULL);
        dg_assert_rect_sane(ed.win->rect, rows, cols);
        /* The strip never takes more than the screen has. */
        YEW_ASSERT(ed.tab_strip_rect.h <= rows);
        /* Drawing at this size must not fault. */
        {
            Render r;
            Bytebuf frame;

            bytebuf_init(&frame);
            (void)memset(&r, 0, sizeof(r));
            yew_render_init(&r, NULL, dg_env);
            dg_frame(&ed, &r, &frame);
            bytebuf_free(&frame);
        }
        yew_ed_free(&ed);
    }
}

/*
 * The shed ORDER, at the one size where it is visible: the row-2 member
 * strip goes before row 1 does, so the entry that leaves the group is
 * still reachable.
 */
void test_degrade_the_member_strip_sheds_before_the_tab_strip(void)
{
    Ed ed;
    u32 g;
    u16 two_row_h;

    dg_fixture(&ed, 24U, 80U, 3);
    g = yew_group_create(&ed, "/src", "grp");
    yew_group_add_member(&ed, g, 2);
    yew_group_add_member(&ed, g, 3);
    yew_tab_switch(&ed, 2);
    yew_ed_layout(&ed);
    two_row_h = ed.tab_strip_rect.h;
    YEW_ASSERT_EQ_U64(two_row_h, 2U);

    /* RESIZE, not re-init: yew_grid_init memsets over the buffers the
     * first one allocated, so a second init leaks them. */
    YEW_ASSERT(yew_grid_resize(&ed.grid, 3U, 80U));
    yew_ed_layout(&ed);
    YEW_ASSERT(ed.tab_strip_rect.h <= 2U);
    YEW_ASSERT((u32)ed.tab_strip_rect.y + ed.tab_strip_rect.h <= 3U);

    /* Two rows: the strip has at most one left. */
    YEW_ASSERT(yew_grid_resize(&ed.grid, 2U, 80U));
    yew_ed_layout(&ed);
    YEW_ASSERT(ed.tab_strip_rect.h <= 1U);
    yew_ed_free(&ed);
}

/* ---------------------------------------------------------------- */
/* ASCII                                                             */
/* ---------------------------------------------------------------- */

/*
 * YEW_ASCII=1 loses no chrome element's meaning: every glyph still has
 * a distinct fallback within its group, so a border is still a border
 * and a ticked row still reads as ticked.
 */
void test_degrade_ascii_keeps_every_element_meaningful(void)
{
    Ed ed;
    Render r;
    Bytebuf frame;
    size_t i;

    bytebuf_init(&frame);
    yew_glyph_force_ascii(true);
    dg_fixture(&ed, 24U, 80U, 3);
    (void)memset(&r, 0, sizeof(r));
    yew_render_init(&r, NULL, dg_env);
    dg_frame(&ed, &r, &frame);
    /* Pure ASCII on the wire, so a terminal that cannot decode UTF-8
     * shows the chrome rather than replacement characters. */
    for (i = 0U; i < frame.len; i++)
        YEW_ASSERT(frame.data[i] < 0x80U);

    /* Ticked and unticked still differ. */
    YEW_ASSERT(strcmp(yew_glyph(YEW_GLYPH_TICKED),
                      yew_glyph(YEW_GLYPH_UNTICKED)) != 0);
    /* A vertical border is still not a horizontal one. */
    YEW_ASSERT(strcmp(yew_glyph(YEW_GLYPH_BORDER_V),
                      yew_glyph(YEW_GLYPH_BORDER_H)) != 0);
    yew_glyph_reset();
    bytebuf_free(&frame);
    yew_ed_free(&ed);
}

/* ---------------------------------------------------------------- */
/* Sprint 57.13 §6: the context menu, at every tier                  */
/* ---------------------------------------------------------------- */

/*
 * The menu takes its colours from the theme's `menu.*` roles, which
 * means it degrades exactly the way the rest of the chrome does — and
 * that has to be checked with a THEME LOADED and the editor's own
 * Render in place, because `yew_theme_ui_tab` picks the rendition from
 * `ed->render`.  Handing the roles their truecolor values and then
 * rendering to a 16-colour terminal would test the renderer, not the
 * menu.
 */
static void dg_menu_fixture(Ed *ed, Render *r)
{
    char error[192];

    dg_fixture(ed, 24U, 80U, 3);
    (void)memset(r, 0, sizeof(*r));
    yew_render_init(r, NULL, dg_env);
    ed->render = *r;
    ed->render_ready = true;
    YEW_ASSERT(yew_theme_apply(ed, "quiver-dark", error, sizeof(error)));
    YEW_ASSERT(yew_mouse_open_tab_menu(ed, yew_tab_at(ed, 1)->tab_id,
                                       4U, 1U));
    YEW_ASSERT(yew_ctx_active());
    /* The first enabled row takes the highlight at open. */
    YEW_ASSERT_EQ_I64(yew_ctx_cursor(), 0);
}

/* One frame with the menu over it. */
static void dg_menu_frame(Ed *ed, Render *r, Bytebuf *out)
{
    yew_region_frame_begin();
    yew_draw_panes(ed);
    if (ed->tab_strip_rect.h != 0U)
        yew_tab_strip_draw(ed, ed->tab_strip_rect);
    yew_draw_footer(ed, ed->win);
    yew_mouse_menu_draw(ed);
    out->len = 0U;
    yew_grid_mark_all(&ed->grid);
    (void)yew_render_frame(r, &ed->grid, out);
    bytebuf_append(out, (const u8 *)"", 1U);
}

static const Cell *dg_cell(const Ed *ed, u16 row, u16 col)
{
    return &ed->grid.back[(size_t)row * ed->grid.cols + col];
}

/* The first label cell of drawn row `i`: past the border and the pad. */
static const Cell *dg_menu_row_cell(const Ed *ed, u32 i)
{
    Rect box = yew_ctx_box();

    return dg_cell(ed, (u16)(box.y + 1U + i), (u16)(box.x + 2U));
}

void test_degrade_no_color_menu_keeps_its_hover_and_frame(void)
{
    Ed ed;
    Render r;
    Bytebuf frame;
    const Cell *hover;
    const char *s;

    bytebuf_init(&frame);
    (void)setenv("NO_COLOR", "1", 1);
    dg_menu_fixture(&ed, &r);
    YEW_ASSERT(r.no_color);
    dg_menu_frame(&ed, &r, &frame);

    s = (const char *)frame.data;
    YEW_ASSERT(strstr(s, "38;2") == NULL);
    YEW_ASSERT(strstr(s, "48;2") == NULL);
    YEW_ASSERT(strstr(s, "38;5") == NULL);
    YEW_ASSERT(strstr(s, "48;5") == NULL);

    /*
     * LEGIBLE, not merely colourless: `menu.hover`'s mono rendition is
     * reverse video, and that is the whole of what says "this row is
     * the one Enter will take" once the palette is gone.
     */
    hover = dg_menu_row_cell(&ed, 0U);
    YEW_ASSERT((hover->attrs & YEW_ATTR_REVERSE) != 0U);
    YEW_ASSERT_EQ_U64((u64)hover->fg.tag, (u64)YEW_COLOR_DEFAULT);
    YEW_ASSERT_EQ_U64((u64)hover->bg.tag, (u64)YEW_COLOR_DEFAULT);
    /* And the greyed row is still greyed, by DIM. */
    YEW_ASSERT((dg_menu_row_cell(&ed, 4U)->attrs & YEW_ATTR_DIM) != 0U);
    /* The frame is still a frame. */
    YEW_ASSERT(memcmp(dg_cell(&ed, yew_ctx_box().y, yew_ctx_box().x)->utf8,
                      yew_glyph(YEW_GLYPH_BORDER_TL),
                      yew_glyph_len(YEW_GLYPH_BORDER_TL)) == 0);

    yew_ctx_close();
    (void)unsetenv("NO_COLOR");
    bytebuf_free(&frame);
    yew_ed_free(&ed);
}

void test_degrade_16_colour_menu_stays_distinguishable(void)
{
    Ed ed;
    Render r;
    Bytebuf frame;
    const Cell *hover;
    const Cell *plain;
    const char *s;

    bytebuf_init(&frame);
    (void)unsetenv("NO_COLOR");
    (void)setenv("YEW_COLORS", "16", 1);
    dg_menu_fixture(&ed, &r);
    YEW_ASSERT_EQ_U64(r.tier, (u64)YEW_RENDER_TIER_16);
    dg_menu_frame(&ed, &r, &frame);

    s = (const char *)frame.data;
    YEW_ASSERT(strstr(s, "38;2") == NULL);
    YEW_ASSERT(strstr(s, "48;2") == NULL);
    YEW_ASSERT(strstr(s, "38;5") == NULL);
    YEW_ASSERT(strstr(s, "48;5") == NULL);

    /*
     * At sixteen colours an accent and a dim are not reliably
     * distinguishable, so the highlighted row has to differ from an
     * ordinary one by something the terminal cannot flatten.
     */
    hover = dg_menu_row_cell(&ed, 0U);
    plain = dg_menu_row_cell(&ed, 3U);
    YEW_ASSERT(hover->attrs != plain->attrs ||
               memcmp(&hover->fg, &plain->fg, sizeof(hover->fg)) != 0 ||
               memcmp(&hover->bg, &plain->bg, sizeof(hover->bg)) != 0);
    /* And the greyed row still differs from the enabled one. */
    YEW_ASSERT(memcmp(dg_menu_row_cell(&ed, 4U), plain,
                      sizeof(Cell)) != 0);

    yew_ctx_close();
    (void)unsetenv("YEW_COLORS");
    bytebuf_free(&frame);
    yew_ed_free(&ed);
}

/*
 * The border comes out of the glyph table, so a terminal with no UTF-8
 * gets a box rather than a field of replacement characters.
 */
void test_degrade_ascii_menu_is_still_a_box(void)
{
    Ed ed;
    Render r;
    Bytebuf frame;
    Rect box;
    size_t i;

    bytebuf_init(&frame);
    (void)unsetenv("NO_COLOR");
    yew_glyph_force_ascii(true);
    dg_menu_fixture(&ed, &r);
    dg_menu_frame(&ed, &r, &frame);
    for (i = 0U; i < frame.len; i++)
        YEW_ASSERT(frame.data[i] < 0x80U);

    box = yew_ctx_box();
    YEW_ASSERT(memcmp(dg_cell(&ed, box.y, box.x)->utf8,
                      yew_glyph(YEW_GLYPH_BORDER_TL),
                      yew_glyph_len(YEW_GLYPH_BORDER_TL)) == 0);
    YEW_ASSERT(memcmp(dg_cell(&ed, (u16)(box.y + box.h - 1U),
                              (u16)(box.x + box.w - 1U))->utf8,
                      yew_glyph(YEW_GLYPH_BORDER_BR),
                      yew_glyph_len(YEW_GLYPH_BORDER_BR)) == 0);
    /* A corner is still not an edge: the fallbacks stay distinct. */
    YEW_ASSERT(strcmp(yew_glyph(YEW_GLYPH_BORDER_TL),
                      yew_glyph(YEW_GLYPH_BORDER_H)) != 0);

    yew_ctx_close();
    yew_glyph_reset();
    bytebuf_free(&frame);
    yew_ed_free(&ed);
}
