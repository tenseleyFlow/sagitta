#include "harness.h"

#include <string.h>

#include "edit/cmd.h"
#include "edit/bind.h"
#include "edit/dispatch.h"
#include "edit/ed.h"
#include "fl/origin.h"

static u32 chord_short_calls;
static u32 chord_force_calls;
static u32 chord_refeed_calls;
static u32 chord_long_calls;
static u32 chord_tail_calls;

static CmdStatus chord_short(CmdCtx *cx)
{
    (void)cx;
    chord_short_calls++;
    return YEW_CMD_OK;
}

static CmdStatus chord_force(CmdCtx *cx)
{
    (void)cx;
    chord_force_calls++;
    return YEW_CMD_OK;
}

static CmdStatus chord_refeed(CmdCtx *cx)
{
    (void)cx;
    chord_refeed_calls++;
    return YEW_CMD_OK;
}

static CmdStatus chord_long(CmdCtx *cx)
{
    (void)cx;
    chord_long_calls++;
    return YEW_CMD_OK;
}

static CmdStatus chord_tail(CmdCtx *cx)
{
    (void)cx;
    chord_tail_calls++;
    return YEW_CMD_OK;
}

static void chord_register_commands(void)
{
    static const CmdDesc commands[] = {
        {"ed.ui.toggle", chord_short, YEW_ARITY_NONE, 0U, "short probe", NULL},
        {"ed.ui.open", chord_force, YEW_ARITY_NONE, 0U, "force probe", NULL},
        {"ed.ui.close", chord_refeed, YEW_ARITY_NONE, 0U, "refeed probe", NULL},
        {"ed.ui.next", chord_long, YEW_ARITY_NONE, 0U, "long probe", NULL},
        {"ed.ui.prev", chord_tail, YEW_ARITY_NONE, 0U, "tail probe", NULL},
    };
    size_t i;

    yew_cmd_init();
    for (i = 0U; i < YEW_ARRAY_LEN(commands); i++) {
        if (yew_cmd_lookup(commands[i].name,
                           (u32)strlen(commands[i].name)).v == 0U)
            (void)yew_cmd_register(&commands[i]);
    }
}

static Key chord_text_key(char c)
{
    Key key = {0};

    key.code = (u32)(u8)c;
    key.kind = YEW_EV_KEY;
    key.ev = YEW_KEY_PRESS;
    key.ntext = 1U;
    key.text[0] = (u8)c;
    return key;
}

static Key chord_named_key(u32 code)
{
    Key key = {0};

    key.code = code;
    key.kind = YEW_EV_KEY;
    key.ev = YEW_KEY_PRESS;
    return key;
}

static void chord_probe_reset(void)
{
    chord_short_calls = 0U;
    chord_force_calls = 0U;
    chord_refeed_calls = 0U;
    chord_long_calls = 0U;
    chord_tail_calls = 0U;
}

static void chord_editor(Ed *ed, const BindRow *rows, u32 n)
{
    chord_register_commands();
    yew_dispatch_init(ed);
    yew_keymap_free(&ed->mode_keys[YEW_MODE_L]);
    YEW_ASSERT(yew_keymap_build(&ed->mode_keys[YEW_MODE_L], "mode:test",
                                rows, n));
    ed->keys.l[0] = &ed->mode_keys[YEW_MODE_L];
    ed->chord_timeout_ms = 500U;
}

void test_chord_full_prefix_timeout_and_refeed(void)
{
    static const BindRow rows[] = {
        {"q", "ed.ui.toggle", 0, NULL},
        {"q !", "ed.ui.open", 0, NULL},
        {"i", "ed.ui.close", 0, NULL},
    };
    Ed ed = {0};

    chord_editor(&ed, rows, YEW_ARRAY_LEN(rows));
    chord_probe_reset();
    yew_dispatch_key(&ed, chord_text_key('q'), 1000);
    YEW_ASSERT_EQ_I64(yew_dispatch_deadline(&ed), 1500);
    YEW_ASSERT_EQ_STR(yew_dispatch_owner(&ed), "mode:test");
    yew_dispatch_tick(&ed, 1499);
    YEW_ASSERT_EQ_U64(chord_short_calls, 0U);
    yew_dispatch_tick(&ed, 1500);
    YEW_ASSERT_EQ_U64(chord_short_calls, 1U);
    YEW_ASSERT_EQ_I64(yew_dispatch_deadline(&ed), -1);

    yew_dispatch_key(&ed, chord_text_key('q'), 2000);
    yew_dispatch_key(&ed, chord_text_key('!'), 2001);
    YEW_ASSERT_EQ_U64(chord_force_calls, 1U);
    YEW_ASSERT_EQ_U64(chord_short_calls, 1U);

    yew_dispatch_key(&ed, chord_text_key('q'), 3000);
    yew_dispatch_key(&ed, chord_text_key('i'), 3001);
    YEW_ASSERT_EQ_U64(chord_short_calls, 2U);
    YEW_ASSERT_EQ_U64(chord_refeed_calls, 1U);
    YEW_ASSERT_EQ_U64(ed.dispatch_count, 4U);
    yew_dispatch_free(&ed);
}

void test_chord_dead_sequence_escape_and_release(void)
{
    static const BindRow rows[] = {
        {"g g", "ed.ui.next", 0, NULL},
        {"z", "ed.ui.prev", 0, NULL},
        {"q", "ed.ui.toggle", 0, NULL},
        {"q !", "ed.ui.open", 0, NULL},
    };
    Ed ed = {0};
    Key release = chord_text_key('z');

    chord_editor(&ed, rows, YEW_ARRAY_LEN(rows));
    chord_probe_reset();
    yew_dispatch_key(&ed, chord_text_key('g'), 10);
    yew_dispatch_key(&ed, chord_text_key('z'), 11);
    YEW_ASSERT_EQ_U64(chord_long_calls, 0U);
    YEW_ASSERT_EQ_U64(chord_tail_calls, 0U);
    YEW_ASSERT(strstr(yew_dispatch_message(&ed), "unbound sequence") != NULL);
    YEW_ASSERT_EQ_I64(yew_dispatch_deadline(&ed), -1);

    yew_dispatch_key(&ed, chord_text_key('q'), 20);
    YEW_ASSERT(yew_dispatch_pending(&ed)->n != 0U);
    yew_dispatch_key(&ed, chord_named_key(YEW_KEY_ESCAPE), 21);
    YEW_ASSERT_EQ_U64(yew_dispatch_pending(&ed)->n, 0U);
    YEW_ASSERT_EQ_U64(chord_short_calls, 0U);
    YEW_ASSERT_EQ_I64(yew_dispatch_deadline(&ed), -1);

    release.ev = YEW_KEY_RELEASE;
    yew_dispatch_key(&ed, release, 30);
    YEW_ASSERT_EQ_U64(chord_tail_calls, 0U);
    YEW_ASSERT_EQ_U64(ed.dispatch_count, 0U);

    yew_dispatch_key(&ed, chord_text_key('g'), 40);
    yew_dispatch_tick(&ed, 540);
    YEW_ASSERT(strstr(yew_dispatch_message(&ed), "unbound sequence") != NULL);
    YEW_ASSERT_EQ_U64(chord_long_calls, 0U);
    yew_dispatch_free(&ed);
}

void test_chord_top_layer_owns_sequence(void)
{
    static const BindRow mode_rows[] = {
        {"g g", "ed.ui.next", 0, NULL},
    };
    static const BindRow plugin_rows[] = {
        {"g", "ed.ui.toggle", 0, NULL},
        {"g x", "ed.ui.open", 0, NULL},
    };
    Keymap plugin = {0};
    Ed ed = {0};

    chord_editor(&ed, mode_rows, YEW_ARRAY_LEN(mode_rows));
    YEW_ASSERT(yew_keymap_build(&plugin, "plug:vimish", plugin_rows,
                                YEW_ARRAY_LEN(plugin_rows)));
    ed.keys.l[2] = &plugin;
    ed.keys.n = 3U;
    chord_probe_reset();
    yew_dispatch_key(&ed, chord_text_key('g'), 100);
    YEW_ASSERT_EQ_STR(yew_dispatch_owner(&ed), "plug:vimish");
    YEW_ASSERT_EQ_I64(yew_dispatch_pending(&ed)->layer, 2);
    yew_dispatch_key(&ed, chord_text_key('g'), 101);
    YEW_ASSERT_EQ_U64(chord_short_calls, 1U);
    YEW_ASSERT_EQ_U64(chord_long_calls, 0U);
    YEW_ASSERT_EQ_STR(yew_dispatch_owner(&ed), "plug:vimish");
    yew_dispatch_tick(&ed, 601);
    YEW_ASSERT_EQ_U64(chord_short_calls, 2U);
    YEW_ASSERT_EQ_U64(chord_long_calls, 0U);
    yew_keymap_free(&plugin);
    yew_dispatch_free(&ed);
}

void test_chord_rebind_and_two_plugins_keep_partial_ownership(void)
{
    static const BindRow mode_rows[] = {
        {"g g", "ed.ui.next", 0, NULL},
    };
    Ed ed;
    u32 builtin;
    u32 user;
    u32 plugin_a;
    u32 plugin_b;

    chord_register_commands();
    yew_ed_init(&ed);
    YEW_ASSERT(yew_ed_open_scratch(&ed));
    yew_keymap_free(&ed.mode_keys[YEW_MODE_L]);
    YEW_ASSERT(yew_keymap_build(&ed.mode_keys[YEW_MODE_L], "mode:audit",
                                mode_rows, YEW_ARRAY_LEN(mode_rows)));
    builtin = fl_origin_register(&ed, FL_ORIGIN_BUILTIN,
                                 "/runtime/init.fl", 0U);
    user = fl_origin_register(&ed, FL_ORIGIN_CONFIG, "/home/init.fl", 0U);
    plugin_a = fl_origin_register(&ed, FL_ORIGIN_PLUGIN, "plug:a", 0U);
    plugin_b = fl_origin_register(&ed, FL_ORIGIN_PLUGIN, "plug:b", 0U);
    yew_bind_batch_begin(&ed);
    YEW_ASSERT(yew_bind_add(&ed, builtin, YEW_MODE_L, "g i",
                            yew_cmd_lookup("ed.ui.prev", 10U), 0, NULL,
                            FL_NIL_V) != 0U);
    YEW_ASSERT(yew_bind_add(&ed, user, YEW_MODE_L, "g u",
                            yew_cmd_lookup("ed.ui.close", 11U), 0, NULL,
                            FL_NIL_V) != 0U);
    YEW_ASSERT(yew_bind_add(&ed, plugin_a, YEW_MODE_L, "g x",
                            yew_cmd_lookup("ed.ui.open", 10U), 0, NULL,
                            FL_NIL_V) != 0U);
    YEW_ASSERT(yew_bind_add(&ed, plugin_b, YEW_MODE_L, "g",
                            yew_cmd_lookup("ed.ui.toggle", 12U), 0, NULL,
                            FL_NIL_V) != 0U);
    YEW_ASSERT(yew_bind_add(&ed, plugin_b, YEW_MODE_L, "g p",
                            yew_cmd_lookup("ed.ui.prev", 10U), 0, NULL,
                            FL_NIL_V) != 0U);

    /* Every origin reaches the same validator.  In particular a plugin
     * cannot smuggle an Escape prefix into the rebuilt config map. */
    YEW_ASSERT_EQ_U64(yew_bind_add(&ed, builtin, YEW_MODE_L, "<esc> i",
                                    yew_cmd_lookup("ed.nop", 6U), 0, NULL,
                                    FL_NIL_V), 0U);
    YEW_ASSERT_EQ_U64(yew_bind_add(&ed, user, YEW_MODE_L, "<esc> u",
                                    yew_cmd_lookup("ed.nop", 6U), 0, NULL,
                                    FL_NIL_V), 0U);
    YEW_ASSERT_EQ_U64(yew_bind_add(&ed, plugin_a, YEW_MODE_L, "<esc> a",
                                    yew_cmd_lookup("ed.nop", 6U), 0, NULL,
                                    FL_NIL_V), 0U);
    YEW_ASSERT_EQ_U64(yew_bind_add(&ed, plugin_b, YEW_MODE_L, "<esc> b",
                                    yew_cmd_lookup("ed.nop", 6U), 0, NULL,
                                    FL_NIL_V), 0U);
    yew_bind_batch_end(&ed);

    chord_probe_reset();
    yew_dispatch_key(&ed, chord_text_key('g'), 100);
    YEW_ASSERT_EQ_STR(yew_dispatch_owner(&ed), "config");
    YEW_ASSERT_EQ_I64(yew_dispatch_pending(&ed)->layer, 2);
    yew_dispatch_key(&ed, chord_text_key('g'), 101);
    YEW_ASSERT_EQ_U64(chord_short_calls, 1U);
    YEW_ASSERT_EQ_U64(chord_long_calls, 0U);
    yew_dispatch_tick(&ed, 601);
    YEW_ASSERT_EQ_U64(chord_short_calls, 2U);
    YEW_ASSERT_EQ_U64(chord_long_calls, 0U);

    yew_dispatch_key(&ed, chord_text_key('g'), 700);
    yew_dispatch_key(&ed, chord_text_key('x'), 701);
    YEW_ASSERT_EQ_U64(chord_force_calls, 1U);
    yew_dispatch_key(&ed, chord_text_key('g'), 800);
    yew_dispatch_key(&ed, chord_text_key('u'), 801);
    YEW_ASSERT_EQ_U64(chord_refeed_calls, 1U);
    yew_dispatch_key(&ed, chord_text_key('g'), 900);
    yew_dispatch_key(&ed, chord_text_key('p'), 901);
    YEW_ASSERT_EQ_U64(chord_tail_calls, 1U);
    yew_ed_free(&ed);
}
