#define _POSIX_C_SOURCE 200809L

#include "harness.h"

#include <stdio.h>
#include <stdlib.h>

#include "syn/theme.h"
#include "util/arena.h"

typedef struct FussThemeFix {
    Arena arena;
    DiagCtx dc;
    u32 ndiag;
} FussThemeFix;

static void fuss_theme_sink(void *ctx, FlDiagLevel level, FlSpan sp,
                            const char *msg, const char *rendered)
{
    FussThemeFix *fix = ctx;

    (void)level;
    (void)sp;
    (void)msg;
    (void)rendered;
    fix->ndiag++;
}

static char *fuss_theme_read(const char *path, size_t *len)
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

void test_fuss_theme_roles_are_shipped(void)
{
    static const char *const paths[] = {
        "runtime/themes/quiver-dark.fl", "runtime/themes/quiver-light.fl"
    };
    static const char *const roles[] = {
        "mode.git", "git.staged", "git.modified", "git.untracked",
        "git.incoming", "git.conflict", "git.ignored", "git.blame",
        "git.blame.stale", "git.sign.add", "git.sign.mod",
        "git.sign.del", "git.sign.conflict", "git.sign.unknown",
        "git.diff.add", "git.diff.del", "git.diff.mod",
        "git.diff.filler", "git.diff.intra.add", "git.diff.intra.del"
    };
    FussThemeFix fix = {0};
    u32 file;

    arena_init(&fix.arena);
    fl_diag_init(&fix.dc, &fix.arena);
    fl_diag_set_sink(&fix.dc, fuss_theme_sink, &fix);
    for (file = 0U; file < YEW_ARRAY_LEN(paths); file++) {
        Theme theme;
        const ThemeEnt *mode;
        const ThemeEnt *conflict;
        char *src;
        size_t len;
        u32 role;

        yew_theme_init(&theme);
        src = fuss_theme_read(paths[file], &len);
        YEW_ASSERT(yew_theme_compile(&theme, (const u8 *)src, len,
                                     paths[file], &fix.dc));
        YEW_ASSERT_EQ_U64(fix.ndiag, 0U);
        for (role = 0U; role < YEW_ARRAY_LEN(roles); role++)
            YEW_ASSERT_NOT_NULL(yew_theme_ui(&theme, roles[role],
                                              YEW_THEME_TRUECOLOR));
        /* Keep the pre-S52 spelling available to user themes/configuration. */
        YEW_ASSERT_NOT_NULL(yew_theme_ui(&theme, "mode.fuss",
                                          YEW_THEME_TRUECOLOR));
        mode = yew_theme_ui(&theme, "mode.git", YEW_THEME_TRUECOLOR);
        conflict = yew_theme_ui(&theme, "git.conflict",
                                YEW_THEME_TRUECOLOR);
        YEW_ASSERT_EQ_U64(mode->bg.tag, YEW_COLOR_RGB);
        YEW_ASSERT_EQ_U64(conflict->fg.tag, YEW_COLOR_RGB);
        YEW_ASSERT((conflict->attrs & YEW_ATTR_BOLD) != 0U);
        yew_theme_free(&theme);
        free(src);
    }
    arena_free_all(&fix.arena);
}
