#define _POSIX_C_SOURCE 200809L
#include "harness.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "edit/ed.h"
#include "edit/option.h"
#include "edit/theme_cmds.h"
#include "ui/picker.h"
#include "ui/region.h"
#include "util/runtime_asset.h"

static void setup(Ed *ed)
{
    yew_ed_init(ed);
    ed->clean = true;
    YEW_ASSERT(yew_ed_open_scratch(ed));
    YEW_ASSERT(yew_grid_init(&ed->grid, &ed->interner, 24U, 100U));
    ed->grid_ready = true;
    yew_ed_layout(ed);
}

static void open_picker(Ed *ed)
{
    CmdCtx cx = {0};

    cx.ed = ed;
    YEW_ASSERT_EQ_I64(yew_theme_cmd_pick(&cx), YEW_CMD_OK);
    YEW_ASSERT(yew_picker_active(ed));
    (void)yew_picker_tick(ed);
}

static i32 id(const char *name)
{
    ThemeNames names;
    u32 i;

    yew_theme_names(&names);
    for (i = 0U; i < names.n; i++) {
        if (strcmp(names.names[i], name) == 0)
            return (i32)i;
    }
    YEW_ASSERT(false);
    return -1;
}

static void key(Ed *ed, u32 code, u16 mods)
{
    Key k = {0};

    k.kind = YEW_EV_KEY;
    k.ev = YEW_KEY_PRESS;
    k.code = code;
    k.mods = mods;
    if (code < 127U && code >= 32U && mods == 0U) {
        k.ntext = 1U;
        k.text[0] = (u8)code;
    }
    YEW_ASSERT(yew_picker_key(ed, &k));
    while (yew_picker_tick(ed)) { }
}

static void option_is(Ed *ed, const char *name)
{
    OptVal v;

    YEW_ASSERT(yew_opt_get(ed, NULL, NULL, "theme", 5U, &v));
    YEW_ASSERT_EQ_U64(v.as.str.len, strlen(name));
    YEW_ASSERT_EQ_MEM(v.as.str.s, name, strlen(name));
}

void test_theme_picker_preview_cancel_is_transactional(void)
{
    Ed ed;
    Theme original;
    u32 i;

    setup(&ed);
    original = ed.theme;
    open_picker(&ed);
    YEW_ASSERT_EQ_STR(yew_theme_name(&ed.theme), "quiver-dark");
    yew_picker_select_payload(&ed, id("catppuccin-latte"));
    YEW_ASSERT_EQ_STR(yew_theme_name(&ed.theme), "catppuccin-latte");
    option_is(&ed, "quiver-dark");
    YEW_ASSERT_EQ_STR(ed.theme_last_light, "quiver-light");
    YEW_ASSERT_EQ_STR(ed.theme_last_dark, "quiver-dark");
    YEW_ASSERT_EQ_U64(yew_textbuf_len(ed.buffer.tb), 0U);
    key(&ed, YEW_KEY_ESCAPE, 0U);
    YEW_ASSERT(!yew_picker_active(&ed));
    YEW_ASSERT(ed.theme.name == original.name);
    YEW_ASSERT(ed.theme.ui == original.ui);
    YEW_ASSERT_EQ_MEM(ed.theme.tab, original.tab, sizeof(original.tab));
    for (i = 0U; i < 30U; i++) {
        open_picker(&ed);
        yew_picker_scroll(&ed, -1);
        YEW_ASSERT_EQ_STR(yew_theme_name(&ed.theme), "nord");
        yew_picker_close(&ed, false);
    }
    option_is(&ed, "quiver-dark");
    yew_ed_free(&ed);
}

void test_theme_picker_accept_keys_and_toggle_history(void)
{
    Ed ed;
    CmdCtx cx = {0};
    static const u32 codes[] = {' ', YEW_KEY_ENTER, 'v', 's'};
    u32 i;

    setup(&ed);
    cx.ed = &ed;
    for (i = 0U; i < YEW_ARRAY_LEN(codes); i++) {
        open_picker(&ed);
        yew_picker_select_payload(&ed, id("gruvbox-light"));
        key(&ed, codes[i], i >= 2U ? YEW_MOD_CTRL : 0U);
        if (i >= 2U) {
            YEW_ASSERT(yew_picker_active(&ed));
            option_is(&ed, "quiver-dark");
            key(&ed, YEW_KEY_ENTER, 0U);
        }
        YEW_ASSERT(!yew_picker_active(&ed));
        option_is(&ed, "gruvbox-light");
        YEW_ASSERT_EQ_STR(ed.theme_last_light, "gruvbox-light");
        YEW_ASSERT_EQ_STR(ed.theme_last_dark, "quiver-dark");
        YEW_ASSERT_EQ_I64(yew_theme_cmd_toggle(&cx), YEW_CMD_OK);
        option_is(&ed, "quiver-dark");
    }
    yew_ed_free(&ed);
}

void test_theme_picker_filter_and_empty_accept(void)
{
    Ed ed;
    const char *p;

    setup(&ed);
    open_picker(&ed);
    for (p = "dracula"; *p != '\0'; p++) key(&ed, (u32)*p, 0U);
    YEW_ASSERT_EQ_U64(yew_picker_shown(&ed), 1U);
    YEW_ASSERT_EQ_STR(yew_theme_name(&ed.theme), "dracula");
    key(&ed, 'z', 0U);
    YEW_ASSERT_EQ_U64(yew_picker_shown(&ed), 0U);
    key(&ed, YEW_KEY_ENTER, 0U);
    YEW_ASSERT(yew_picker_active(&ed));
    option_is(&ed, "quiver-dark");
    key(&ed, YEW_KEY_BACKSPACE, 0U);
    YEW_ASSERT_EQ_U64(yew_picker_shown(&ed), 1U);
    key(&ed, ' ', 0U);
    YEW_ASSERT(!yew_picker_active(&ed));
    option_is(&ed, "dracula");
    yew_ed_free(&ed);
}

void test_theme_picker_replacement_shutdown_and_small_grid(void)
{
    Ed ed;
    CmdCtx cx = {0};
    ThemeEnt before;

    setup(&ed);
    cx.ed = &ed;
    before = yew_theme_tab(&ed)[YEW_ATTR_TEXT];
    open_picker(&ed);
    yew_picker_select_payload(&ed, id("dracula"));
    open_picker(&ed);
    YEW_ASSERT_EQ_STR(yew_theme_name(&ed.theme), "quiver-dark");
    YEW_ASSERT_EQ_MEM(&yew_theme_tab(&ed)[YEW_ATTR_TEXT], &before, sizeof(before));
    yew_picker_select_payload(&ed, id("nord"));
    yew_ed_free(&ed); /* teardown cancels and releases the owned snapshot */
    setup(&ed);
    ed.grid.cols = 10U;
    YEW_ASSERT_EQ_I64(yew_theme_cmd_pick(&cx), YEW_CMD_ERR_STATE);
    YEW_ASSERT(!yew_picker_active(&ed));
    ed.grid.cols = 100U;
    yew_theme_free(&ed.theme);
    yew_theme_sync_surfaces(&ed);
    open_picker(&ed);
    yew_picker_select_payload(&ed, id("dracula"));
    key(&ed, YEW_KEY_ESCAPE, 0U);
    YEW_ASSERT_NULL(yew_theme_name(&ed.theme));
    YEW_ASSERT_EQ_U64(ed.grid.blank.fg.tag, YEW_COLOR_DEFAULT);
    YEW_ASSERT_EQ_U64(ed.grid.blank.bg.tag, YEW_COLOR_DEFAULT);
    yew_ed_free(&ed);
}

void test_theme_picker_mouse_preview_and_apply(void)
{
    Ed ed;
    Key k = {0};
    u16 x;
    u16 y;

    setup(&ed);
    open_picker(&ed);
    yew_picker_select_payload(&ed, id("nord"));
    yew_region_frame_begin();
    yew_picker_draw(&ed, (Rect){0U, 0U, 100U, 24U});
    YEW_ASSERT(yew_picker_sel_cell(&ed, &x, &y));
    k.kind = YEW_EV_MOUSE;
    k.button = YEW_MB_LEFT;
    k.col = x;
    k.row = y;
    k.ev = YEW_KEY_PRESS;
    yew_mouse_event(&ed, &k);
    YEW_ASSERT(yew_picker_active(&ed));
    option_is(&ed, "quiver-dark");
    k.ev = YEW_KEY_RELEASE;
    yew_mouse_event(&ed, &k);
    YEW_ASSERT(!yew_picker_active(&ed));
    option_is(&ed, "nord");
    yew_ed_free(&ed);
}

void test_theme_modal_fallback_and_tiers(void)
{
    Ed ed;
    u32 tier;
    char error[192];

    setup(&ed);
    YEW_ASSERT(yew_theme_set(&ed, "catppuccin-latte", error, sizeof(error)));
    for (tier = 0U; tier < YEW_THEME_RENDITION_COUNT; tier++) {
        ThemeEnt style;
        const ThemeEnt *expected;

        ed.render_ready = true;
        ed.render.plain = tier == YEW_THEME_DUMB;
        ed.render.no_color = tier == YEW_THEME_MONO;
        ed.render.tier = tier == YEW_THEME_TRUECOLOR ? YEW_RENDER_TIER_TRUECOLOR :
                         tier == YEW_THEME_256 ? YEW_RENDER_TIER_256 :
                                                 YEW_RENDER_TIER_16;
        style = yew_theme_modal_style(&ed, "modal.title");
        expected = yew_theme_ui(&ed.theme, "modal.surface", (YewThemeRendition)tier);
        YEW_ASSERT_EQ_MEM(&style.bg, &expected->bg, sizeof(style.bg));
        if (tier >= YEW_THEME_MONO) {
            YEW_ASSERT_EQ_U64(style.fg.tag, YEW_COLOR_DEFAULT);
            YEW_ASSERT_EQ_U64(style.bg.tag, YEW_COLOR_DEFAULT);
        }
    }
    ed.render_ready = false;
    yew_theme_free(&ed.theme);
    YEW_ASSERT_EQ_U64(yew_theme_modal_style(&ed, "modal.surface").bg.tag,
                      YEW_COLOR_DEFAULT);
    yew_ed_free(&ed);
}

static void write_fixture(const char *path, const char *text)
{
    FILE *f = fopen(path, "wb");

    YEW_ASSERT_NOT_NULL(f);
    YEW_ASSERT_EQ_U64(fwrite(text, 1U, strlen(text), f), strlen(text));
    YEW_ASSERT_EQ_I64(fclose(f), 0);
}

void test_theme_picker_custom_discovery_failure_and_removed_source(void)
{
    char root[] = "/tmp/yew-theme-picker.XXXXXX";
    char dir[256];
    char file[256];
    char bad[256];
    char fifo[256];
    const char *env = getenv("YEW_RUNTIME_DIR");
    char *saved = env == NULL ? NULL : strdup(env);
    ThemeNames names;
    Ed ed;
    char error[192];

    setup(&ed);
    YEW_ASSERT_NOT_NULL(mkdtemp(root));
    (void)snprintf(dir, sizeof(dir), "%s/themes", root);
    (void)snprintf(file, sizeof(file), "%s/themes/custom.fl", root);
    (void)snprintf(bad, sizeof(bad), "%s/themes/broken.fl", root);
    (void)snprintf(fifo, sizeof(fifo), "%s/themes/fifo.fl", root);
    YEW_ASSERT_EQ_I64(mkdir(dir, 0700), 0);
    write_fixture(file, "{theme:1,name:\"custom\",kind:\"dark\",palette:{},"
                       "ui:{ul:{error:\"none\",warn:\"none\",info:\"none\"}},"
                       "attrs:{text:{fg:\"#123456\"}}}");
    write_fixture(bad, "not a theme");
    YEW_ASSERT_EQ_I64(mkfifo(fifo, 0600), 0);
    YEW_ASSERT_EQ_I64(setenv("YEW_RUNTIME_DIR", root, 1), 0);
    yew_theme_names(&names);
    YEW_ASSERT_EQ_U64(names.n, 3U);
    YEW_ASSERT_EQ_STR(names.names[0], "broken");
    YEW_ASSERT_EQ_STR(names.names[1], "custom");
    YEW_ASSERT_EQ_STR(names.names[2], "fifo");
    YEW_ASSERT(!names.truncated);
    YEW_ASSERT(yew_theme_set(&ed, "custom", error, sizeof(error)));
    open_picker(&ed);
    yew_picker_select_payload(&ed, id("broken"));
    YEW_ASSERT_EQ_STR(yew_theme_name(&ed.theme), "custom");
    key(&ed, YEW_KEY_ENTER, 0U);
    option_is(&ed, "custom");
    YEW_ASSERT_EQ_STR(yew_theme_name(&ed.theme), "custom");
    open_picker(&ed);
    yew_picker_select_payload(&ed, id("fifo"));
    YEW_ASSERT_EQ_STR(yew_theme_name(&ed.theme), "custom");
    key(&ed, YEW_KEY_ESCAPE, 0U);
    open_picker(&ed);
    YEW_ASSERT_EQ_I64(unlink(file), 0);
    key(&ed, YEW_KEY_ENTER, 0U); /* approve the already-loaded snapshot */
    option_is(&ed, "custom");
    YEW_ASSERT_EQ_STR(yew_theme_name(&ed.theme), "custom");
    open_picker(&ed); /* current file is gone; Escape must still restore */
    key(&ed, YEW_KEY_ESCAPE, 0U);
    YEW_ASSERT_EQ_STR(yew_theme_name(&ed.theme), "custom");
    YEW_ASSERT_EQ_U64(yew_theme_tab(&ed)[YEW_ATTR_TEXT].fg.r, 0x12U);
    YEW_ASSERT_EQ_I64(unlink(bad), 0);
    YEW_ASSERT_EQ_I64(unlink(fifo), 0);
    YEW_ASSERT_EQ_I64(rmdir(dir), 0);
    YEW_ASSERT_EQ_I64(rmdir(root), 0);
    if (saved != NULL) {
        YEW_ASSERT_EQ_I64(setenv("YEW_RUNTIME_DIR", saved, 1), 0);
        free(saved);
    } else {
        YEW_ASSERT_EQ_I64(unsetenv("YEW_RUNTIME_DIR"), 0);
    }
    yew_ed_free(&ed);
}

void test_theme_picker_opener_cancels_preview(void)
{
    Ed ed;
    BindRow binding = {"t h", "ed.theme.pick", 0, NULL};
    Key k = {0};

    setup(&ed);
    YEW_ASSERT(yew_keymap_build(&ed.user_keys, "theme fixture", &binding, 1U));
    k.kind = YEW_EV_KEY;
    k.ev = YEW_KEY_PRESS;
    k.code = 't';
    yew_ed_handle_key(&ed, k, 1);
    k.code = 'h';
    yew_ed_handle_key(&ed, k, 2);
    YEW_ASSERT(yew_picker_active(&ed));
    yew_picker_select_payload(&ed, id("nord"));
    k.code = 't';
    yew_ed_handle_key(&ed, k, 3);
    k.code = 'h';
    yew_ed_handle_key(&ed, k, 4);
    YEW_ASSERT(!yew_picker_active(&ed));
    option_is(&ed, "quiver-dark");
    YEW_ASSERT_EQ_STR(yew_theme_name(&ed.theme), "quiver-dark");
    yew_ed_free(&ed);
}

void test_theme_names_bounded_sort_and_user_override(void)
{
    char root[] = "/tmp/yew-theme-names.XXXXXX";
    char dirs[3][256];
    char file[256];
    const char *env = getenv("YEW_RUNTIME_DIR");
    char *runtime = env == NULL ? NULL : strdup(env);
    const char *config_env = getenv("XDG_CONFIG_HOME");
    char *config = config_env == NULL ? NULL : strdup(config_env);
    ThemeNames names;
    Ed ed;
    char error[192];
    u32 i;

    setup(&ed);
    YEW_ASSERT_NOT_NULL(mkdtemp(root));
    (void)snprintf(dirs[0], sizeof(dirs[0]), "%s/themes", root);
    (void)snprintf(dirs[1], sizeof(dirs[1]), "%s/yew", root);
    (void)snprintf(dirs[2], sizeof(dirs[2]), "%s/yew/themes", root);
    for (i = 0U; i < 3U; i++) YEW_ASSERT_EQ_I64(mkdir(dirs[i], 0700), 0);
    /* Deliberately create in reverse lexical order, including invalid
     * stems and a duplicate which the user directory must override. */
    for (i = 270U; i > 0U; i--) {
        (void)snprintf(file, sizeof(file), "%s/themes/name%03u.fl", root, i);
        write_fixture(file, "bad runtime theme");
    }
    (void)snprintf(file, sizeof(file), "%s/yew/themes/name001.fl", root);
    write_fixture(file, "{theme:1,name:\"name001\",kind:\"dark\",palette:{},"
                       "ui:{ul:{error:\"none\",warn:\"none\",info:\"none\"}},"
                       "attrs:{text:{fg:\"#123456\"}}}");
    (void)snprintf(file, sizeof(file), "%s/themes/Invalid.fl", root);
    write_fixture(file, "invalid name");
    YEW_ASSERT_EQ_I64(setenv("YEW_RUNTIME_DIR", root, 1), 0);
    YEW_ASSERT_EQ_I64(setenv("XDG_CONFIG_HOME", root, 1), 0);
    yew_theme_names(&names);
    YEW_ASSERT_EQ_U64(names.n, YEW_THEME_NAMES_MAX);
    YEW_ASSERT(names.truncated);
    YEW_ASSERT_EQ_STR(names.names[0], "name001");
    YEW_ASSERT_EQ_STR(names.names[255], "name256");
    for (i = 1U; i < names.n; i++)
        YEW_ASSERT(strcmp(names.names[i - 1U], names.names[i]) < 0);
    YEW_ASSERT(yew_theme_set(&ed, "name001", error, sizeof(error)));
    YEW_ASSERT_EQ_I64(unlink(file), 0);
    for (i = 1U; i <= 270U; i++) {
        (void)snprintf(file, sizeof(file), "%s/themes/name%03u.fl", root, i);
        YEW_ASSERT_EQ_I64(unlink(file), 0);
    }
    (void)snprintf(file, sizeof(file), "%s/yew/themes/name001.fl", root);
    YEW_ASSERT_EQ_I64(unlink(file), 0);
    for (i = 3U; i > 0U; i--) YEW_ASSERT_EQ_I64(rmdir(dirs[i - 1U]), 0);
    YEW_ASSERT_EQ_I64(rmdir(root), 0);
    if (runtime != NULL) {
        YEW_ASSERT_EQ_I64(setenv("YEW_RUNTIME_DIR", runtime, 1), 0);
        free(runtime);
    } else {
        YEW_ASSERT_EQ_I64(unsetenv("YEW_RUNTIME_DIR"), 0);
    }
    if (config != NULL) {
        YEW_ASSERT_EQ_I64(setenv("XDG_CONFIG_HOME", config, 1), 0);
        free(config);
    } else {
        YEW_ASSERT_EQ_I64(unsetenv("XDG_CONFIG_HOME"), 0);
    }
    yew_ed_free(&ed);
}
