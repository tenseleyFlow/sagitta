#include "harness.h"

#include <string.h>

#include "syn/theme.h"
#include "term/render.h"
#include "util/arena.h"

static const char *degrade_no_color;
static const char *degrade_term;

static const char *degrade_env(const char *name)
{
    if (strcmp(name, "NO_COLOR") == 0)
        return degrade_no_color;
    if (strcmp(name, "TERM") == 0)
        return degrade_term;
    return NULL;
}

void test_theme_degradation_ladder(void)
{
    static const char src[] =
        "{ theme: 1, name: \"degrade\", kind: \"dark\", palette: {},\n"
        "  ui: { ul: { error: \"#ff0000\", warn: \"#ffff00\", info: \"#0000ff\" } },\n"
        "  attrs: { text: { fg: \"#123456\", bg: \"#abcdef\",\n"
        "                         italic: true, mono: \"bold\" } } }";
    Arena arena;
    DiagCtx dc;
    Theme theme;
    const ThemeEnt *tab;

    arena_init(&arena);
    fl_diag_init(&dc, &arena);
    yew_theme_init(&theme);
    YEW_ASSERT(yew_theme_compile(&theme, (const u8 *)src, strlen(src),
                                 "degrade.fl", &dc));

    degrade_term = "xterm-256color";
    degrade_no_color = NULL;
    YEW_ASSERT_EQ_U64(yew_theme_rendition(YEW_RENDER_TIER_TRUECOLOR,
                                          degrade_env),
                      YEW_THEME_TRUECOLOR);
    YEW_ASSERT_EQ_U64(yew_theme_rendition(YEW_RENDER_TIER_256, degrade_env),
                      YEW_THEME_256);
    YEW_ASSERT_EQ_U64(yew_theme_rendition(YEW_RENDER_TIER_16, degrade_env),
                      YEW_THEME_16);

    degrade_no_color = "";
    YEW_ASSERT_EQ_U64(yew_theme_rendition(YEW_RENDER_TIER_TRUECOLOR,
                                          degrade_env), YEW_THEME_MONO);
    degrade_no_color = "0";
    YEW_ASSERT_EQ_U64(yew_theme_rendition(YEW_RENDER_TIER_TRUECOLOR,
                                          degrade_env), YEW_THEME_MONO);
    tab = yew_theme_table(&theme, YEW_THEME_MONO);
    YEW_ASSERT_EQ_U64(tab[YEW_ATTR_TEXT].fg.tag, YEW_COLOR_DEFAULT);
    YEW_ASSERT_EQ_U64(tab[YEW_ATTR_TEXT].bg.tag, YEW_COLOR_DEFAULT);
    YEW_ASSERT_EQ_U64(tab[YEW_ATTR_TEXT].attrs, YEW_ATTR_BOLD);

    degrade_no_color = NULL;
    degrade_term = "dumb";
    YEW_ASSERT_EQ_U64(yew_theme_rendition(YEW_RENDER_TIER_TRUECOLOR,
                                          degrade_env), YEW_THEME_DUMB);
    tab = yew_theme_table(&theme, YEW_THEME_DUMB);
    YEW_ASSERT_EQ_U64(tab[YEW_ATTR_TEXT].fg.tag, YEW_COLOR_DEFAULT);
    YEW_ASSERT_EQ_U64(tab[YEW_ATTR_TEXT].bg.tag, YEW_COLOR_DEFAULT);
    YEW_ASSERT_EQ_U64(tab[YEW_ATTR_TEXT].attrs, 0U);

    tab = yew_theme_table(&theme, YEW_THEME_16);
    YEW_ASSERT_EQ_U64(tab[YEW_ATTR_TEXT].fg.tag, YEW_COLOR_INDEXED);
    YEW_ASSERT(tab[YEW_ATTR_TEXT].fg.r < 16U);
    tab = yew_theme_table(&theme, YEW_THEME_256);
    YEW_ASSERT_EQ_U64(tab[YEW_ATTR_TEXT].fg.tag, YEW_COLOR_INDEXED);
    tab = yew_theme_table(&theme, YEW_THEME_TRUECOLOR);
    YEW_ASSERT_EQ_U64(tab[YEW_ATTR_TEXT].fg.tag, YEW_COLOR_RGB);

    yew_theme_free(&theme);
    arena_free_all(&arena);
}
