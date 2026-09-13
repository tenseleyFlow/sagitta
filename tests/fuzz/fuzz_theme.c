#include "fuzzlib.h"

#include <stdio.h>

#include "syn/theme.h"
#include "util/arena.h"

static bool fail_theme(char *why, size_t why_cap, const char *message,
                       size_t index)
{
    (void)snprintf(why, why_cap, "%s at slot %zu", message, index);
    return false;
}

static bool color_valid(YewColor color)
{
    if (color.tag > YEW_COLOR_RGB)
        return false;
    if (color.tag == YEW_COLOR_DEFAULT)
        return color.r == 0U && color.g == 0U && color.b == 0U;
    if (color.tag == YEW_COLOR_INDEXED)
        return color.g == 0U && color.b == 0U;
    return true;
}

static bool entry_valid(const ThemeEnt *entry)
{
    return color_valid(entry->fg) && color_valid(entry->bg) &&
           (entry->attrs & (u16)~YEW_CELL_ATTR_MASK) == 0U;
}

static bool check_theme(const u8 *data, size_t len,
                        char *why, size_t why_cap)
{
    Arena arena;
    DiagCtx dc;
    Theme theme;
    bool accepted;
    u32 rendition;
    u32 attr;
    u32 role;

    arena_init(&arena);
    fl_diag_init(&dc, &arena);
    yew_theme_init(&theme);
    accepted = yew_theme_compile(&theme, data, len, "<fuzz-theme>", &dc);
    if (!accepted) {
        bool unchanged = theme.name == NULL && theme.ui == NULL &&
                         theme.nui == 0U;
        bool diagnosed = dc.nerrors != 0U;

        yew_theme_free(&theme);
        arena_free_all(&arena);
        if (!unchanged) {
            (void)snprintf(why, why_cap,
                           "rejected theme changed destination");
            return false;
        }
        if (!diagnosed) {
            (void)snprintf(why, why_cap,
                           "rejected theme emitted no error diagnostic");
            return false;
        }
        return true;
    }
    if (dc.nerrors != 0U || theme.name == NULL ||
        theme.kind > (u8)YEW_THEME_LIGHT ||
        (theme.explicit_attrs >> YEW_ATTR__COUNT) != 0U) {
        yew_theme_free(&theme);
        arena_free_all(&arena);
        (void)snprintf(why, why_cap,
                       "accepted theme has invalid top-level state");
        return false;
    }
    for (rendition = 0U; rendition < YEW_THEME_RENDITION_COUNT;
         rendition++) {
        const ThemeEnt *table = yew_theme_table(
            &theme, (YewThemeRendition)rendition);

        if (table == NULL) {
            yew_theme_free(&theme);
            arena_free_all(&arena);
            return fail_theme(why, why_cap,
                              "accepted theme omitted rendition", rendition);
        }
        for (attr = 0U; attr < YEW_ATTR__COUNT; attr++) {
            if (!entry_valid(&table[attr])) {
                yew_theme_free(&theme);
                arena_free_all(&arena);
                return fail_theme(why, why_cap,
                                  "accepted theme left invalid attr", attr);
            }
        }
    }
    for (role = 0U; role < theme.nui; role++) {
        if (theme.ui[role].name == NULL) {
            yew_theme_free(&theme);
            arena_free_all(&arena);
            return fail_theme(why, why_cap,
                              "accepted theme left unnamed UI role", role);
        }
        for (rendition = 0U; rendition < YEW_THEME_RENDITION_COUNT;
             rendition++) {
            if (!entry_valid(&theme.ui[role].tab[rendition])) {
                yew_theme_free(&theme);
                arena_free_all(&arena);
                return fail_theme(why, why_cap,
                                  "accepted theme left invalid UI role", role);
            }
        }
    }
    for (rendition = 0U; rendition < 3U; rendition++) {
        for (attr = 0U; attr < YEW_THEME_UL_COUNT; attr++) {
            if (!color_valid(theme.ul[rendition][attr])) {
                yew_theme_free(&theme);
                arena_free_all(&arena);
                return fail_theme(why, why_cap,
                                  "accepted theme left invalid underline",
                                  attr);
            }
        }
    }
    yew_theme_free(&theme);
    arena_free_all(&arena);
    return true;
}

int main(int argc, char **argv)
{
    return yew_fuzz_main(argc, argv, "fuzz_theme", "runtime/themes",
                         check_theme);
}
