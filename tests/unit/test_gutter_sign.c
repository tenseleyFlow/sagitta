/* Sprint 43: sign kinds have fixed cells and deterministic priority. */
#include "harness.h"

#include <string.h>

#include "edit/ed.h"
#include "ui/gutter.h"

static const GutterSign sign_diag = {
    (const u8 *)"d", 1U, "shadow.index", YEW_ATTR_BOLD
};
static const GutterSign sign_git = {
    (const u8 *)"g", 1U, "shadow.index", YEW_ATTR_DIM
};
static const GutterSign sign_shadow = {
    (const u8 *)"s", 1U, "shadow.index", YEW_ATTR_UNDERLINE
};

static void gutter_sign_fixture(Ed *ed)
{
    yew_ed_init(ed);
    YEW_ASSERT(yew_ed_open_memory(ed, (const u8 *)"a\nb\nc\n", 6U,
                                  "gutter-sign"));
    YEW_ASSERT(yew_grid_init(&ed->grid, &ed->interner, 4U, 12U));
    ed->grid_ready = true;
    ed->win->rect = (Rect){6U, 0U, 6U, 4U};
    ed->win->gutter_width = 6U;
    ed->win->vp.rows = 4U;
    ed->win->vp.cols = 6U;
}

static u8 gutter_byte(const Ed *ed, u16 row, u16 col)
{
    return ed->grid.back[(size_t)row * ed->grid.cols + col].utf8[0];
}

void test_gutter_sign_kind_cells_and_priority_cover_all_combinations(void)
{
    u32 mask;

    for (mask = 1U; mask < 8U; mask++) {
        Ed ed;

        gutter_sign_fixture(&ed);
        if ((mask & 1U) != 0U)
            yew_gutter_sign_set(ed.win, LINENO(0U), YEW_SIGN_DIAG,
                                &sign_diag);
        if ((mask & 2U) != 0U)
            yew_gutter_sign_set(ed.win, LINENO(0U), YEW_SIGN_GIT,
                                &sign_git);
        if ((mask & 4U) != 0U)
            yew_gutter_sign_set(ed.win, LINENO(0U), YEW_SIGN_SHADOW,
                                &sign_shadow);
        yew_gutter_draw(&ed, ed.win, 0U, 1U);
        YEW_ASSERT_EQ_U64(gutter_byte(&ed, 0U, 0U),
                          (mask & 1U) != 0U ? (u8)'d' :
                          (mask & 2U) != 0U ? (u8)'g' : 0U);
        YEW_ASSERT_EQ_U64(gutter_byte(&ed, 0U, 1U),
                          (mask & 4U) != 0U ? (u8)'s' : 0U);
        yew_ed_free(&ed);
    }
}

void test_gutter_sign_rejects_wide_glyph_once_and_uses_ascii(void)
{
    static const u8 wide[] = "漢";
    GutterSign bad = {wide, (u8)(sizeof(wide) - 1U),
                      "shadow.ai", YEW_ATTR_DIM};
    Ed ed;

    gutter_sign_fixture(&ed);
    yew_test_capture_log();
    yew_gutter_sign_set(ed.win, LINENO(0U), YEW_SIGN_SHADOW, &bad);
    yew_gutter_sign_set(ed.win, LINENO(1U), YEW_SIGN_SHADOW, &bad);
    YEW_ASSERT_EQ_U64(yew_test_log_count(), 1U);
    YEW_ASSERT(yew_test_log_contains(YEW_LOG_WARN, "ASCII fallback"));
    yew_gutter_draw(&ed, ed.win, 0U, 2U);
    YEW_ASSERT_EQ_U64(gutter_byte(&ed, 0U, 1U), (u8)'s');
    YEW_ASSERT_EQ_U64(gutter_byte(&ed, 1U, 1U), (u8)'s');
    yew_ed_free(&ed);
}

void test_gutter_sign_clear_is_scrolled_range_exact(void)
{
    Ed ed;
    u32 line;

    gutter_sign_fixture(&ed);
    for (line = 0U; line < 4U; line++)
        yew_gutter_sign_set(ed.win, LINENO(line), YEW_SIGN_SHADOW,
                            &sign_shadow);
    yew_gutter_signs_clear(ed.win, LINENO(1U), LINENO(3U));
    yew_gutter_draw(&ed, ed.win, 0U, 4U);
    YEW_ASSERT_EQ_U64(gutter_byte(&ed, 0U, 1U), (u8)'s');
    YEW_ASSERT_EQ_U64(gutter_byte(&ed, 1U, 1U), 0U);
    YEW_ASSERT_EQ_U64(gutter_byte(&ed, 2U, 1U), 0U);
    YEW_ASSERT_EQ_U64(gutter_byte(&ed, 3U, 1U), (u8)'s');
    YEW_ASSERT_EQ_U64(ed.win->gutter_signs.len, 2U);
    yew_ed_free(&ed);
}
