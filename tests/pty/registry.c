#include "harness.h"

#include <signal.h>
#include <string.h>

static const char restore_blob[] =
    "\x1b[<u"
    "\x1b[?2004l"
    "\x1b[?1002l"
    "\x1b[?1006l"
    "\x1b[?1004l"
    "\x1b[?2026l"
    "\x1b[0m"
    "\x1b[?1049l"
    "\x1b[?25h";

static void spawn_scene(PtyCtx *c, const char *scene)
{
    ptc_spawn(c, ptc_demo_bin(c), "--scene", scene, NULL);
    ptc_settle(c, 0);
}

static void quit_cleanly(PtyCtx *c)
{
    ptc_allow_restore(c);
    ptc_keys(c, "q");
    ptc_expect_exit(c, 0);
}

static void case_probe_modern(PtyCtx *c)
{
    static const char queries[] = "\x1b[?u\x1b[?2026$p\x1b[c";
    static const char enable[] =
        "\x1b[?2004h\x1b[?1002h\x1b[?1006h\x1b[?1004h\x1b[>21u";

    spawn_scene(c, "basic");
    ptc_expect_output(c, queries, sizeof(queries) - 1U);
    ptc_expect_output(c, enable, sizeof(enable) - 1U);
    ptc_check(c, c->vt.nprobes == 3U &&
                 c->vt.probe_order[0] == VT_PROBE_KITTY &&
                 c->vt.probe_order[1] == VT_PROBE_SYNC &&
                 c->vt.probe_order[2] == VT_PROBE_DA,
              "terminal probe query order differs from the pinned order");
    ptc_check(c, c->vt.ksp == 1 && c->vt.kitty[0] == 21U,
              "modern profile did not exercise the kitty keyboard push");
    ptc_snapshot(c, "probe_modern");
    quit_cleanly(c);
}

static void case_probe_dumb(PtyCtx *c)
{
    static const char kitty_push[] = "\x1b[>21u";

    spawn_scene(c, "basic");
    ptc_check(c, c->vt.nprobes == 3U,
              "dumb profile did not observe all three probe queries");
    ptc_reject_output(c, kitty_push, sizeof(kitty_push) - 1U);
    ptc_check(c, c->vt.nsync_pairs == 0U,
              "dumb profile unexpectedly used synchronized output");
    ptc_snapshot(c, "probe_dumb");
    quit_cleanly(c);
}

static void case_paint_basic(PtyCtx *c)
{
    spawn_scene(c, "basic");
    ptc_snapshot(c, "paint_basic");
    quit_cleanly(c);
}

static void case_paint_wide(PtyCtx *c)
{
    spawn_scene(c, "wide");
    ptc_snapshot(c, "paint_wide");
    quit_cleanly(c);
}

static void case_colors_truecolor(PtyCtx *c)
{
    spawn_scene(c, "colors");
    ptc_snapshot(c, "paint_colors_truecolor");
    quit_cleanly(c);
}

static void case_colors_256(PtyCtx *c)
{
    spawn_scene(c, "colors");
    ptc_snapshot(c, "paint_colors_256");
    quit_cleanly(c);
}

static void case_colors_16(PtyCtx *c)
{
    spawn_scene(c, "colors");
    ptc_snapshot(c, "paint_colors_16");
    quit_cleanly(c);
}

static void case_paint_damage(PtyCtx *c)
{
    spawn_scene(c, "damage");
    ptc_keys(c, "a");
    ptc_settle(c, 0);
    ptc_snapshot(c, "paint_damage");
    quit_cleanly(c);
}

static void case_paint_resize(PtyCtx *c)
{
    spawn_scene(c, "resize");
    ptc_resize(c, 31U, 96U);
    ptc_settle(c, 0);
    ptc_snapshot(c, "paint_resize");
    quit_cleanly(c);
}

static void case_restore_quit(PtyCtx *c)
{
    spawn_scene(c, "basic");
    ptc_snapshot(c, "restore_quit");
    quit_cleanly(c);
    ptc_expect_tail(c, restore_blob, sizeof(restore_blob) - 1U);
}

static void case_restore_crash(PtyCtx *c)
{
    static const char crash_tail[] =
        "\x1b[<u"
        "\x1b[?2004l"
        "\x1b[?1002l"
        "\x1b[?1006l"
        "\x1b[?1004l"
        "\x1b[?2026l"
        "\x1b[0m"
        "\x1b[?1049l"
        "\x1b[?25h"
        "sagitta: fatal signal, terminal restored\r\r\n";

    ptc_allow_primary(c);
    ptc_allow_restore(c);
    ptc_spawn(c, ptc_demo_bin(c), "--scene", "basic", "--crash", NULL);
    ptc_settle(c, 0);
    ptc_expect_signal(c, SIGSEGV);
    ptc_expect_tail(c, crash_tail, sizeof(crash_tail) - 1U);
    ptc_snapshot(c, "restore_crash");
}

static void case_restore_suspend(PtyCtx *c)
{
    spawn_scene(c, "basic");
    ptc_suspend_resume(c);
    ptc_settle(c, 0);
    ptc_check(c, c->vt.alt, "alternate screen was not re-entered after resume");
    ptc_snapshot(c, "restore_suspend");
    quit_cleanly(c);
}

static void input_script(PtyCtx *c)
{
    spawn_scene(c, "echo");
    ptc_keys(c, "ctrl+a");
    ptc_settle(c, 0);
    ptc_allow_restore(c);
    quit_cleanly(c);
    ptc_snapshot(c, "input_keys");
}

static void case_input_modern(PtyCtx *c)
{
    input_script(c);
}

static void case_input_legacy(PtyCtx *c)
{
    input_script(c);
}

static void case_driver_defer(PtyCtx *c)
{
    static const char message[] =
        "sagitta: error: the editor is not yet implemented: Sprint 14 (modes L and I)";

    ptc_allow_primary(c);
    ptc_spawn(c, ptc_sagitta_bin(c), "foo.txt", NULL);
    ptc_settle(c, 0);
    ptc_expect_exit(c, 1);
    ptc_expect_output(c, message, sizeof(message) - 1U);
    ptc_snapshot(c, "driver_defer");
}

#define C(name, profile, rows, cols, fn) \
    {#name, #profile, rows, cols, fn}

const PtyCase sag_pty_cases[] = {
    C(probe_modern, modern, 24U, 80U, case_probe_modern),
    C(probe_dumb, dumb, 24U, 80U, case_probe_dumb),
    C(paint_basic, modern, 24U, 80U, case_paint_basic),
    C(paint_wide, modern, 24U, 80U, case_paint_wide),
    C(paint_colors_truecolor, modern, 24U, 80U, case_colors_truecolor),
    C(paint_colors_256, modern, 24U, 80U, case_colors_256),
    C(paint_colors_16, modern, 24U, 80U, case_colors_16),
    C(paint_damage, modern, 24U, 80U, case_paint_damage),
    C(paint_resize, modern, 24U, 80U, case_paint_resize),
    C(restore_quit, modern, 24U, 80U, case_restore_quit),
    C(restore_crash, modern, 24U, 80U, case_restore_crash),
    C(restore_suspend, modern, 24U, 80U, case_restore_suspend),
    C(input_keys_modern, modern, 24U, 80U, case_input_modern),
    C(input_keys_legacy, nokitty, 24U, 80U, case_input_legacy),
    C(driver_defer, dumb, 24U, 80U, case_driver_defer),
    {NULL, NULL, 0U, 0U, NULL}
};

#undef C
