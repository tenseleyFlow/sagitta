#include "harness.h"

#include <string.h>

#include "edit/cmd.h"
#include "edit/dispatch.h"
#include "edit/ed.h"

static u32 chord_short_calls;
static u32 chord_force_calls;
static u32 chord_refeed_calls;
static u32 chord_long_calls;
static u32 chord_tail_calls;

static CmdStatus chord_short(CmdCtx *cx)
{
    (void)cx;
    chord_short_calls++;
    return SAG_CMD_OK;
}

static CmdStatus chord_force(CmdCtx *cx)
{
    (void)cx;
    chord_force_calls++;
    return SAG_CMD_OK;
}

static CmdStatus chord_refeed(CmdCtx *cx)
{
    (void)cx;
    chord_refeed_calls++;
    return SAG_CMD_OK;
}

static CmdStatus chord_long(CmdCtx *cx)
{
    (void)cx;
    chord_long_calls++;
    return SAG_CMD_OK;
}

static CmdStatus chord_tail(CmdCtx *cx)
{
    (void)cx;
    chord_tail_calls++;
    return SAG_CMD_OK;
}

static void chord_register_commands(void)
{
    static const CmdDesc commands[] = {
        {"ed.ui.toggle", chord_short, SAG_ARITY_NONE, 0U, "short probe"},
        {"ed.ui.open", chord_force, SAG_ARITY_NONE, 0U, "force probe"},
        {"ed.ui.close", chord_refeed, SAG_ARITY_NONE, 0U, "refeed probe"},
        {"ed.ui.next", chord_long, SAG_ARITY_NONE, 0U, "long probe"},
        {"ed.ui.prev", chord_tail, SAG_ARITY_NONE, 0U, "tail probe"},
    };
    size_t i;

    sag_cmd_init();
    for (i = 0U; i < SAG_ARRAY_LEN(commands); i++) {
        if (sag_cmd_lookup(commands[i].name,
                           (u32)strlen(commands[i].name)).v == 0U)
            (void)sag_cmd_register(&commands[i]);
    }
}

static Key chord_text_key(char c)
{
    Key key = {0};

    key.code = (u32)(u8)c;
    key.kind = SAG_EV_KEY;
    key.ev = SAG_KEY_PRESS;
    key.ntext = 1U;
    key.text[0] = (u8)c;
    return key;
}

static Key chord_named_key(u32 code)
{
    Key key = {0};

    key.code = code;
    key.kind = SAG_EV_KEY;
    key.ev = SAG_KEY_PRESS;
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
    sag_dispatch_init(ed);
    sag_keymap_free(&ed->mode_keys[SAG_MODE_L]);
    SAG_ASSERT(sag_keymap_build(&ed->mode_keys[SAG_MODE_L], "mode:test",
                                rows, n));
    ed->keys.l[0] = &ed->mode_keys[SAG_MODE_L];
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

    chord_editor(&ed, rows, SAG_ARRAY_LEN(rows));
    chord_probe_reset();
    sag_dispatch_key(&ed, chord_text_key('q'), 1000);
    SAG_ASSERT_EQ_I64(sag_dispatch_deadline(&ed), 1500);
    SAG_ASSERT_EQ_STR(sag_dispatch_owner(&ed), "mode:test");
    sag_dispatch_tick(&ed, 1499);
    SAG_ASSERT_EQ_U64(chord_short_calls, 0U);
    sag_dispatch_tick(&ed, 1500);
    SAG_ASSERT_EQ_U64(chord_short_calls, 1U);
    SAG_ASSERT_EQ_I64(sag_dispatch_deadline(&ed), -1);

    sag_dispatch_key(&ed, chord_text_key('q'), 2000);
    sag_dispatch_key(&ed, chord_text_key('!'), 2001);
    SAG_ASSERT_EQ_U64(chord_force_calls, 1U);
    SAG_ASSERT_EQ_U64(chord_short_calls, 1U);

    sag_dispatch_key(&ed, chord_text_key('q'), 3000);
    sag_dispatch_key(&ed, chord_text_key('i'), 3001);
    SAG_ASSERT_EQ_U64(chord_short_calls, 2U);
    SAG_ASSERT_EQ_U64(chord_refeed_calls, 1U);
    SAG_ASSERT_EQ_U64(ed.dispatch_count, 4U);
    sag_dispatch_free(&ed);
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

    chord_editor(&ed, rows, SAG_ARRAY_LEN(rows));
    chord_probe_reset();
    sag_dispatch_key(&ed, chord_text_key('g'), 10);
    sag_dispatch_key(&ed, chord_text_key('z'), 11);
    SAG_ASSERT_EQ_U64(chord_long_calls, 0U);
    SAG_ASSERT_EQ_U64(chord_tail_calls, 0U);
    SAG_ASSERT(strstr(sag_dispatch_message(&ed), "unbound sequence") != NULL);
    SAG_ASSERT_EQ_I64(sag_dispatch_deadline(&ed), -1);

    sag_dispatch_key(&ed, chord_text_key('q'), 20);
    SAG_ASSERT(sag_dispatch_pending(&ed)->n != 0U);
    sag_dispatch_key(&ed, chord_named_key(SAG_KEY_ESCAPE), 21);
    SAG_ASSERT_EQ_U64(sag_dispatch_pending(&ed)->n, 0U);
    SAG_ASSERT_EQ_U64(chord_short_calls, 0U);
    SAG_ASSERT_EQ_I64(sag_dispatch_deadline(&ed), -1);

    release.ev = SAG_KEY_RELEASE;
    sag_dispatch_key(&ed, release, 30);
    SAG_ASSERT_EQ_U64(chord_tail_calls, 0U);
    SAG_ASSERT_EQ_U64(ed.dispatch_count, 0U);

    sag_dispatch_key(&ed, chord_text_key('g'), 40);
    sag_dispatch_tick(&ed, 540);
    SAG_ASSERT(strstr(sag_dispatch_message(&ed), "unbound sequence") != NULL);
    SAG_ASSERT_EQ_U64(chord_long_calls, 0U);
    sag_dispatch_free(&ed);
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

    chord_editor(&ed, mode_rows, SAG_ARRAY_LEN(mode_rows));
    SAG_ASSERT(sag_keymap_build(&plugin, "plug:vimish", plugin_rows,
                                SAG_ARRAY_LEN(plugin_rows)));
    ed.keys.l[2] = &plugin;
    ed.keys.n = 3U;
    chord_probe_reset();
    sag_dispatch_key(&ed, chord_text_key('g'), 100);
    SAG_ASSERT_EQ_STR(sag_dispatch_owner(&ed), "plug:vimish");
    SAG_ASSERT_EQ_I64(sag_dispatch_pending(&ed)->layer, 2);
    sag_dispatch_key(&ed, chord_text_key('g'), 101);
    SAG_ASSERT_EQ_U64(chord_short_calls, 1U);
    SAG_ASSERT_EQ_U64(chord_long_calls, 0U);
    SAG_ASSERT_EQ_STR(sag_dispatch_owner(&ed), "plug:vimish");
    sag_dispatch_tick(&ed, 601);
    SAG_ASSERT_EQ_U64(chord_short_calls, 2U);
    SAG_ASSERT_EQ_U64(chord_long_calls, 0U);
    sag_keymap_free(&plugin);
    sag_dispatch_free(&ed);
}
