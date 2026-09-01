#define _POSIX_C_SOURCE 200809L

#include "harness.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "syn/theme.h"
#include "util/arena.h"

typedef struct ThemeFix {
    Arena arena;
    DiagCtx dc;
    u32 ndiag;
    char message[256];
} ThemeFix;

static void theme_sink(void *ctx, FlDiagLevel level, FlSpan sp,
                       const char *msg, const char *rendered)
{
    ThemeFix *f = ctx;

    (void)level;
    (void)sp;
    (void)rendered;
    if (f->ndiag++ == 0U)
        (void)snprintf(f->message, sizeof(f->message), "%s", msg);
}

static void theme_fix_open(ThemeFix *f)
{
    (void)memset(f, 0, sizeof(*f));
    arena_init(&f->arena);
    fl_diag_init(&f->dc, &f->arena);
    fl_diag_set_sink(&f->dc, theme_sink, f);
}

static void theme_fix_close(ThemeFix *f)
{
    arena_free_all(&f->arena);
}

static char *theme_read(const char *path, size_t *len)
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

void test_theme_defaults_compile_with_full_attr_coverage(void)
{
    static const char *const paths[] = {
        "runtime/themes/quiver-dark.fl", "runtime/themes/quiver-light.fl"
    };
    static const char *const ui_roles[] = {
        "lsp.hover_range", "lsp.highlight_read", "lsp.highlight_write",
        "diag.error", "diag.warn", "diag.info", "diag.hint",
        "diag.unnecessary", "diag.deprecated", "git.blame",
        "git.blame.stale", "git.sign.add", "git.sign.mod",
        "git.sign.del", "git.sign.conflict", "git.sign.unknown",
        "git.diff.add", "git.diff.del", "git.diff.mod",
        "git.diff.filler", "git.diff.intra.add", "git.diff.intra.del",
        "tab.bar", "tab.active", "tab.inactive", "tab.modified",
        "tab.orphan", "tab.add"
    };
    ThemeFix f;
    u32 file;

    theme_fix_open(&f);
    for (file = 0U; file < YEW_ARRAY_LEN(paths); file++) {
        Theme theme;
        char *src;
        size_t len;
        u32 attr;

        yew_theme_init(&theme);
        src = theme_read(paths[file], &len);
        YEW_ASSERT(yew_theme_compile(&theme, (const u8 *)src, len,
                                     paths[file], &f.dc));
        YEW_ASSERT_EQ_U64(f.ndiag, 0U);
        YEW_ASSERT_EQ_STR(yew_theme_name(&theme),
                          file == 0U ? "quiver-dark" : "quiver-light");
        YEW_ASSERT_EQ_U64(yew_theme_kind(&theme),
                          file == 0U ? YEW_THEME_DARK : YEW_THEME_LIGHT);
        for (attr = 0U; attr < YEW_ATTR__COUNT; attr++)
            YEW_ASSERT(yew_theme_attr_explicit(&theme, (SynAttr)attr));
        for (attr = 0U; attr < YEW_ARRAY_LEN(ui_roles); attr++)
            YEW_ASSERT_NOT_NULL(yew_theme_ui(&theme, ui_roles[attr],
                                              YEW_THEME_TRUECOLOR));
        YEW_ASSERT_EQ_U64(theme.explicit_attrs,
                          (UINT64_C(1) << YEW_ATTR__COUNT) - UINT64_C(1));
        YEW_ASSERT_EQ_U64(yew_theme_table(&theme, YEW_THEME_TRUECOLOR)
                              [YEW_ATTR_TEXT].fg.tag, YEW_COLOR_RGB);
        YEW_ASSERT_EQ_U64(yew_theme_table(&theme, YEW_THEME_MONO)
                              [YEW_ATTR_ERROR].attrs, YEW_ATTR_REVERSE);
        YEW_ASSERT((yew_theme_table(&theme, YEW_THEME_TRUECOLOR)
                        [YEW_ATTR_ERROR].attrs &
                    (YEW_ATTR_UNDERCURL | YEW_CELL_UL_MASK)) ==
                   (YEW_ATTR_UNDERCURL | YEW_CELL_UL_ERROR));
        YEW_ASSERT((yew_theme_table(&theme, YEW_THEME_TRUECOLOR)
                        [YEW_ATTR_WARNING].attrs &
                    (YEW_ATTR_UNDERCURL | YEW_CELL_UL_MASK)) ==
                   (YEW_ATTR_UNDERCURL | YEW_CELL_UL_WARN));
        yew_theme_free(&theme);
        free(src);
    }
    theme_fix_close(&f);
}

void test_theme_partial_fallback_and_colour_forms(void)
{
    static const char src[] =
        "{ theme: 1, name: \"partial\", kind: \"dark\",\n"
        "  palette: { base: \"#123456\", alias: \"@base\" },\n"
        "  ui: { ul: { error: \"#ff0000\", warn: { idx: 3 }, info: \"none\" } },\n"
        "  attrs: {\n"
        "    text: { fg: \"@alias\", bg: \"none\", mono: \"plain\" },\n"
        "    keyword: { fg: { idx: 200 }, c256: 201, c16: 5,\n"
        "               bold: true, ul: \"info\", mono: \"italic\" },\n"
        "  },\n"
        "}";
    ThemeFix f;
    Theme theme;
    const ThemeEnt *tc;
    const ThemeEnt *c256;
    const ThemeEnt *c16;

    theme_fix_open(&f);
    yew_theme_init(&theme);
    YEW_ASSERT(yew_theme_compile(&theme, (const u8 *)src, strlen(src),
                                 "partial.fl", &f.dc));
    YEW_ASSERT_EQ_U64(f.ndiag, 0U);
    tc = yew_theme_table(&theme, YEW_THEME_TRUECOLOR);
    c256 = yew_theme_table(&theme, YEW_THEME_256);
    c16 = yew_theme_table(&theme, YEW_THEME_16);
    YEW_ASSERT_EQ_U64(tc[YEW_ATTR_TEXT].fg.tag, YEW_COLOR_RGB);
    YEW_ASSERT_EQ_U64(tc[YEW_ATTR_TEXT].fg.r, 0x12U);
    YEW_ASSERT_EQ_U64(tc[YEW_ATTR_TEXT].fg.g, 0x34U);
    YEW_ASSERT_EQ_U64(tc[YEW_ATTR_TEXT].fg.b, 0x56U);
    YEW_ASSERT_EQ_U64(tc[YEW_ATTR_TEXT].bg.tag, YEW_COLOR_DEFAULT);
    YEW_ASSERT_EQ_MEM(&tc[YEW_ATTR_KEYWORD_CONTROL], &tc[YEW_ATTR_KEYWORD],
                      sizeof(ThemeEnt));
    YEW_ASSERT_EQ_MEM(&tc[YEW_ATTR_MOTION_UNIT], &tc[YEW_ATTR_KEYWORD],
                      sizeof(ThemeEnt));
    YEW_ASSERT_EQ_U64(tc[YEW_ATTR_KEYWORD].fg.tag, YEW_COLOR_INDEXED);
    YEW_ASSERT_EQ_U64(tc[YEW_ATTR_KEYWORD].fg.r, 200U);
    YEW_ASSERT_EQ_U64(c256[YEW_ATTR_KEYWORD].fg.r, 201U);
    YEW_ASSERT_EQ_U64(c16[YEW_ATTR_KEYWORD].fg.r, 5U);
    YEW_ASSERT((tc[YEW_ATTR_KEYWORD].attrs & YEW_ATTR_BOLD) != 0U);
    YEW_ASSERT_EQ_U64(tc[YEW_ATTR_KEYWORD].attrs & YEW_CELL_UL_MASK,
                      YEW_CELL_UL_INFO);
    YEW_ASSERT_EQ_U64(yew_theme_table(&theme, YEW_THEME_MONO)
                          [YEW_ATTR_KEYWORD].attrs, YEW_ATTR_ITALIC);
    YEW_ASSERT_EQ_U64(yew_theme_underline(&theme, YEW_THEME_256,
                                          YEW_THEME_UL_WARN).tag,
                      YEW_COLOR_INDEXED);
    YEW_ASSERT_EQ_U64(yew_theme_underline(&theme, YEW_THEME_256,
                                          YEW_THEME_UL_WARN).r, 3U);
    YEW_ASSERT_EQ_U64(yew_theme_underline(&theme, YEW_THEME_TRUECOLOR,
                                          YEW_THEME_UL_INFO).tag,
                      YEW_COLOR_DEFAULT);
    yew_theme_free(&theme);
    theme_fix_close(&f);
}

void test_theme_invalid_input_is_diagnostic_and_transactional(void)
{
    static const char good[] =
        "{ theme: 1, name: \"kept\", kind: \"light\", palette: {},\n"
        "  ui: { ul: { error: \"#ff0000\", warn: \"#ffff00\", info: \"#00fF00\" } },\n"
        "  attrs: { text: { fg: \"none\", mono: \"plain\" } } }";
    static const char bad[] =
        "{ theme: 1, name: \"bad\", kind: \"dark\", palette: {},\n"
        "  ui: { ul: { error: \"#zz0000\", warn: \"none\", info: \"none\" } },\n"
        "  attrs: {}, mystery: true }";
    ThemeFix f;
    Theme theme;

    theme_fix_open(&f);
    yew_theme_init(&theme);
    YEW_ASSERT(yew_theme_compile(&theme, (const u8 *)good, strlen(good),
                                 "good.fl", &f.dc));
    YEW_ASSERT(!yew_theme_compile(&theme, (const u8 *)bad, strlen(bad),
                                  "bad.fl", &f.dc));
    YEW_ASSERT_EQ_STR(yew_theme_name(&theme), "kept");
    YEW_ASSERT(f.ndiag >= 2U);
    YEW_ASSERT(strstr(f.message, "unknown theme key") != NULL);
    yew_theme_free(&theme);
    theme_fix_close(&f);
}

void test_theme_oversize_file_reports_the_size_limit(void)
{
    char root[] = "build/theme-oversize.XXXXXX";
    char dir[256];
    char path[256];
    u8 block[4096];
    ThemeFix f;
    Theme theme;
    FILE *file;
    u32 i;
    int n;

    YEW_ASSERT_NOT_NULL(mkdtemp(root));
    n = snprintf(dir, sizeof(dir), "%s/themes", root);
    YEW_ASSERT(n > 0 && (size_t)n < sizeof(dir));
    YEW_ASSERT_EQ_I64(mkdir(dir, 0700), 0);
    n = snprintf(path, sizeof(path), "%s/oversize.fl", dir);
    YEW_ASSERT(n > 0 && (size_t)n < sizeof(path));
    file = fopen(path, "wb");
    YEW_ASSERT_NOT_NULL(file);
    (void)memset(block, 'x', sizeof(block));
    for (i = 0U; i < 257U; i++)
        YEW_ASSERT_EQ_U64(fwrite(block, 1U, sizeof(block), file),
                          sizeof(block));
    YEW_ASSERT_EQ_I64(fclose(file), 0);

    theme_fix_open(&f);
    yew_theme_init(&theme);
    errno = 0;
    YEW_ASSERT(!yew_theme_select(&theme, "oversize", root, &f.dc));
    YEW_ASSERT_EQ_U64(f.ndiag, 1U);
    YEW_ASSERT(strstr(f.message, "exceeds the 1 MiB limit") != NULL);
    YEW_ASSERT(strstr(f.message, "Success") == NULL);
    YEW_ASSERT_NULL(yew_theme_name(&theme));
    yew_theme_free(&theme);
    theme_fix_close(&f);

    YEW_ASSERT_EQ_I64(unlink(path), 0);
    YEW_ASSERT_EQ_I64(rmdir(dir), 0);
    YEW_ASSERT_EQ_I64(rmdir(root), 0);
}
