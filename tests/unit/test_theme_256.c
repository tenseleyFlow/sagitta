#include "harness.h"

#include <stdio.h>
#include <stdlib.h>

#include "syn/theme.h"
#include "term/render.h"
#include "util/arena.h"

static char *theme256_read(const char *path, size_t *len)
{
    FILE *file = fopen(path, "rb");
    char *src;
    long end;

    YEW_ASSERT_NOT_NULL(file);
    YEW_ASSERT_EQ_I64(fseek(file, 0L, SEEK_END), 0);
    end = ftell(file);
    YEW_ASSERT(end >= 0L);
    YEW_ASSERT_EQ_I64(fseek(file, 0L, SEEK_SET), 0);
    src = malloc((size_t)end + 1U);
    YEW_ASSERT_NOT_NULL(src);
    YEW_ASSERT_EQ_U64(fread(src, 1U, (size_t)end, file), (size_t)end);
    YEW_ASSERT_EQ_I64(fclose(file), 0);
    src[end] = '\0';
    *len = (size_t)end;
    return src;
}

void test_theme_all_108_documented_256_conversions(void)
{
    static const u8 expected[2][YEW_ATTR__COUNT] = {
        {252,176,176,73,176,180,180,173,173,173,173,114,114,73,212,114,
         241,245,180,75,75,75,75,252,168,252,168,180,212,180,176,73,247,
         247,247,168,173,75,73,252,252,114,176,241,114,168,203,180,238,
         176,73,173,75,238},
        {235,160,160,25,160,94,94,25,25,25,25,23,23,30,132,23,243,241,94,
         98,98,98,98,235,160,235,22,94,132,94,160,25,241,241,241,22,98,
         25,25,235,235,22,160,243,22,160,124,94,188,160,25,25,98,188}
    };
    static const char *const paths[2] = {
        "runtime/themes/quiver-dark.fl", "runtime/themes/quiver-light.fl"
    };
    u32 file;

    for (file = 0U; file < 2U; file++) {
        Arena arena;
        DiagCtx dc;
        Theme theme;
        const ThemeEnt *tc;
        char *src;
        size_t len;
        u32 attr;

        arena_init(&arena);
        fl_diag_init(&dc, &arena);
        yew_theme_init(&theme);
        src = theme256_read(paths[file], &len);
        YEW_ASSERT(yew_theme_compile(&theme, (const u8 *)src, len,
                                     paths[file], &dc));
        tc = yew_theme_table(&theme, YEW_THEME_TRUECOLOR);
        for (attr = 0U; attr < YEW_ATTR__COUNT; attr++) {
            YEW_ASSERT_EQ_U64(tc[attr].fg.tag, YEW_COLOR_RGB);
            YEW_ASSERT_EQ_U64(yew_rgb_to_256(tc[attr].fg.r, tc[attr].fg.g,
                                             tc[attr].fg.b),
                              expected[file][attr]);
            YEW_ASSERT_EQ_U64(yew_theme_table(&theme, YEW_THEME_256)
                                  [attr].fg.r, expected[file][attr]);
        }
        yew_theme_free(&theme);
        free(src);
        arena_free_all(&arena);
    }
}
