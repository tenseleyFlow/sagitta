#include "harness.h"

#include <string.h>
#include <unistd.h>

#include "edit/ed.h"
#include "edit/option.h"
#include "edit/theme_cmds.h"
#include "syn_toy.h"

void test_theme_switch_repaints_without_rehighlighting(void)
{
    Ed ed;
    SynToy toy;
    char error[192];

    yew_ed_init(&ed);
    YEW_ASSERT(yew_ed_open_scratch(&ed));
    YEW_ASSERT(yew_grid_init(&ed.grid, &ed.interner, 50U, 200U));
    ed.grid_ready = true;
    yew_grid_flip(&ed.grid);
    ed.full_damage = false;

    syn_toy_init(&toy);
    yew_syn_buf_bind(&ed.buffer.syn, toy.engine);
    yew_syn_engine_reset_counters(toy.engine);

    YEW_ASSERT(yew_theme_apply(&ed, "quiver-light", error, sizeof(error)));
    YEW_ASSERT_EQ_STR(yew_theme_name(&ed.theme), "quiver-light");
    YEW_ASSERT(ed.full_damage);
    YEW_ASSERT_EQ_U64(ed.grid.dmg_lo, 0U);
    YEW_ASSERT_EQ_U64(ed.grid.dmg_hi, 50U);
    YEW_ASSERT_EQ_U64(ed.grid.dmg[0].lo, 0U);
    YEW_ASSERT_EQ_U64(ed.grid.dmg[0].hi, 200U);
    YEW_ASSERT_EQ_U64(yew_syn_engine_line_calls(toy.engine), 0U);
    YEW_ASSERT_EQ_MEM(&ed.grid.blank.fg,
                      &yew_theme_ui_tab(&ed, "fg")->fg,
                      sizeof(ed.grid.blank.fg));
    YEW_ASSERT_EQ_MEM(&ed.grid.blank.bg,
                      &yew_theme_ui_tab(&ed, "bg")->bg,
                      sizeof(ed.grid.blank.bg));

    yew_grid_flip(&ed.grid);
    ed.full_damage = false;
    YEW_ASSERT(!yew_theme_apply(&ed, "missing-theme", error,
                                sizeof(error)));
    YEW_ASSERT_EQ_STR(yew_theme_name(&ed.theme), "quiver-light");
    YEW_ASSERT(!ed.full_damage);
    YEW_ASSERT_EQ_U64(ed.grid.dmg_lo, 50U);
    YEW_ASSERT_EQ_U64(ed.grid.dmg_hi, 0U);
    YEW_ASSERT_EQ_U64(yew_syn_engine_line_calls(toy.engine), 0U);

    yew_ed_free(&ed);
    syn_toy_free(&toy);
}

void test_theme_commands_set_and_toggle_live_theme(void)
{
    Ed ed;
    CmdCtx cx = {0};
    OptVal option;

    yew_ed_init(&ed);
    YEW_ASSERT(yew_ed_open_scratch(&ed));
    cx.ed = &ed;
    cx.count = 1U;
    cx.sarg = "quiver-light";
    cx.sarg_len = (u32)strlen(cx.sarg);
    YEW_ASSERT_EQ_I64(yew_theme_cmd_set(&cx), YEW_CMD_OK);
    YEW_ASSERT_EQ_STR(yew_theme_name(&ed.theme), "quiver-light");
    YEW_ASSERT(yew_opt_get(&ed, NULL, NULL, "theme", 5U, &option));
    YEW_ASSERT_EQ_U64(option.type, YEW_OPT_STR);
    YEW_ASSERT_EQ_MEM(option.as.str.s, "quiver-light",
                      sizeof("quiver-light") - 1U);

    cx.sarg = NULL;
    cx.sarg_len = 0U;
    YEW_ASSERT_EQ_I64(yew_theme_cmd_toggle(&cx), YEW_CMD_OK);
    YEW_ASSERT_EQ_STR(yew_theme_name(&ed.theme), "quiver-dark");
    YEW_ASSERT(yew_opt_get(&ed, NULL, NULL, "theme", 5U, &option));
    YEW_ASSERT_EQ_U64(option.as.str.len, sizeof("quiver-dark") - 1U);
    YEW_ASSERT_EQ_MEM(option.as.str.s, "quiver-dark",
                      sizeof("quiver-dark") - 1U);

    cx.sarg = "does-not-exist";
    cx.sarg_len = (u32)strlen(cx.sarg);
    YEW_ASSERT_EQ_I64(yew_theme_cmd_set(&cx), YEW_CMD_ERR_ARG);
    YEW_ASSERT_EQ_STR(yew_theme_name(&ed.theme), "quiver-dark");
    YEW_ASSERT(yew_opt_get(&ed, NULL, NULL, "theme", 5U, &option));
    YEW_ASSERT_EQ_U64(option.as.str.len, sizeof("quiver-dark") - 1U);
    YEW_ASSERT_EQ_MEM(option.as.str.s, "quiver-dark",
                      sizeof("quiver-dark") - 1U);
    yew_ed_free(&ed);
}

void test_theme_option_rejects_invalid_name_transactionally(void)
{
    Ed ed;
    OptVal value = {YEW_OPT_STR,
                    {.str = {"does-not-exist",
                             sizeof("does-not-exist") - 1U}}};
    OptVal current;
    const char *error = NULL;

    yew_ed_init(&ed);
    YEW_ASSERT_EQ_STR(yew_theme_name(&ed.theme), "quiver-dark");
    YEW_ASSERT(!yew_opt_set(&ed, YEW_OPT_GLOBAL, "theme", 5U, &value,
                            &error));
    YEW_ASSERT(error != NULL);
    YEW_ASSERT(strstr(error, "does-not-exist") != NULL);
    YEW_ASSERT_EQ_STR(yew_theme_name(&ed.theme), "quiver-dark");
    YEW_ASSERT(yew_opt_get(&ed, NULL, NULL, "theme", 5U, &current));
    YEW_ASSERT_EQ_U64(current.as.str.len, sizeof("quiver-dark") - 1U);
    YEW_ASSERT_EQ_MEM(current.as.str.s, "quiver-dark",
                      sizeof("quiver-dark") - 1U);
    yew_ed_free(&ed);
}

void test_theme_default_syncs_underline_palette_after_render_init(void)
{
    Ed ed;
    TtyCaps caps = {0};
    YewColor expected;

    caps.truecolor = true;
    yew_ed_init(&ed);
    YEW_ASSERT_EQ_STR(yew_theme_name(&ed.theme), "quiver-dark");
    YEW_ASSERT_EQ_U64(ed.render.underline_colors[0].tag, 0U);
    yew_render_init(&ed.render, &caps, NULL);
    ed.render_ready = true;
    yew_theme_sync_surfaces(&ed);
    expected = yew_theme_underline(&ed.theme, YEW_THEME_TRUECOLOR,
                                   YEW_THEME_UL_ERROR);
    YEW_ASSERT_EQ_MEM(&ed.render.underline_colors[0], &expected,
                      sizeof(expected));
    expected = yew_theme_underline(&ed.theme, YEW_THEME_TRUECOLOR,
                                   YEW_THEME_UL_WARN);
    YEW_ASSERT_EQ_MEM(&ed.render.underline_colors[1], &expected,
                      sizeof(expected));
    expected = yew_theme_underline(&ed.theme, YEW_THEME_TRUECOLOR,
                                   YEW_THEME_UL_INFO);
    YEW_ASSERT_EQ_MEM(&ed.render.underline_colors[2], &expected,
                      sizeof(expected));
    yew_ed_free(&ed);
}

void test_theme_auto_uses_fake_osc11_io_and_is_default_off(void)
{
    static const u8 reply[] = "key\x1b]11;rgb:eeee/eeee/eeee\x07";
    static const u8 query[] = "\x1b]11;?\x07";
    Ed ed;
    OptVal enabled = {YEW_OPT_BOOL, {.b = true}};
    const char *error = NULL;
    int input[2];
    int output[2];
    u8 actual[sizeof(query) - 1U];
    ssize_t n;
    const char *initial;

    yew_ed_init(&ed);
    initial = yew_theme_name(&ed.theme);
    YEW_ASSERT(!yew_theme_auto_startup(&ed));
    YEW_ASSERT(yew_theme_name(&ed.theme) == initial);
    YEW_ASSERT_EQ_I64(pipe(input), 0);
    YEW_ASSERT_EQ_I64(pipe(output), 0);
    bytebuf_init(&ed.tty.pending);
    ed.tty.rfd = input[0];
    ed.tty.wfd = output[1];
    YEW_ASSERT_EQ_I64(write(input[1], reply, sizeof(reply) - 1U),
                      (ssize_t)sizeof(reply) - 1);
    YEW_ASSERT(yew_opt_set(&ed, YEW_OPT_GLOBAL, "theme_auto", 10U,
                           &enabled, &error));
    YEW_ASSERT(yew_theme_auto_startup(&ed));
    YEW_ASSERT_EQ_STR(yew_theme_name(&ed.theme), "quiver-light");
    YEW_ASSERT(yew_opt_get(&ed, NULL, NULL, "theme", 5U, &enabled));
    YEW_ASSERT_EQ_U64(enabled.type, YEW_OPT_STR);
    YEW_ASSERT_EQ_MEM(enabled.as.str.s, "quiver-light",
                      sizeof("quiver-light") - 1U);
    n = read(output[0], actual, sizeof(actual));
    YEW_ASSERT_EQ_I64(n, (ssize_t)sizeof(actual));
    YEW_ASSERT_EQ_MEM(actual, query, sizeof(actual));
    YEW_ASSERT_EQ_U64(ed.tty.pending.len, 3U);
    YEW_ASSERT_EQ_MEM(ed.tty.pending.data, "key", 3U);

    bytebuf_free(&ed.tty.pending);
    YEW_ASSERT_EQ_I64(close(input[0]), 0);
    YEW_ASSERT_EQ_I64(close(input[1]), 0);
    YEW_ASSERT_EQ_I64(close(output[0]), 0);
    YEW_ASSERT_EQ_I64(close(output[1]), 0);
    ed.tty.rfd = -1;
    ed.tty.wfd = -1;
    yew_ed_free(&ed);
}
