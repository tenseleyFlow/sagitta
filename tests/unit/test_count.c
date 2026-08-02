#include "harness.h"

#include <string.h>

#include "edit/cmd.h"
#include "edit/dispatch.h"
#include "edit/ed.h"

static u32 count_calls;
static u32 count_seen;
static bool count_given_seen;

static CmdStatus count_repeat(CmdCtx *cx)
{
    count_calls++;
    count_seen = cx->count;
    count_given_seen = cx->count_given;
    return SAG_CMD_OK;
}

static CmdStatus count_take(CmdCtx *cx)
{
    count_calls++;
    count_seen = cx->count;
    count_given_seen = cx->count_given;
    return SAG_CMD_OK;
}

static CmdStatus count_plain(CmdCtx *cx)
{
    count_calls++;
    count_seen = cx->count;
    count_given_seen = cx->count_given;
    return SAG_CMD_OK;
}

static void count_register_commands(void)
{
    static const CmdDesc commands[] = {
        {"ed.ui.grow", count_repeat, SAG_ARITY_NONE,
         SAG_CMD_REPEATABLE, "repeat count probe"},
        {"ed.ui.shrink", count_take, SAG_ARITY_NONE,
         SAG_CMD_TAKES_COUNT, "consuming count probe"},
        {"ed.ui.expand", count_plain, SAG_ARITY_NONE,
         0U, "plain count probe"},
    };
    size_t i;

    sag_cmd_init();
    for (i = 0U; i < SAG_ARRAY_LEN(commands); i++) {
        if (sag_cmd_lookup(commands[i].name,
                           (u32)strlen(commands[i].name)).v == 0U)
            (void)sag_cmd_register(&commands[i]);
    }
}

static Key count_text_key(char c)
{
    Key key = {0};

    key.code = (u32)(u8)c;
    key.kind = SAG_EV_KEY;
    key.ev = SAG_KEY_PRESS;
    key.ntext = 1U;
    key.text[0] = (u8)c;
    return key;
}

static Key count_named_key(u32 code)
{
    Key key = {0};

    key.code = code;
    key.kind = SAG_EV_KEY;
    key.ev = SAG_KEY_PRESS;
    return key;
}

static void count_probe_reset(void)
{
    count_calls = 0U;
    count_seen = 0U;
    count_given_seen = false;
}

static void count_editor(Ed *ed)
{
    static const BindRow rows[] = {
        {"<down>", "ed.ui.grow", 0, NULL},
        {"G", "ed.ui.shrink", 0, NULL},
        {"x", "ed.ui.expand", 0, NULL},
    };

    count_register_commands();
    sag_dispatch_init(ed);
    sag_keymap_free(&ed->mode_keys[SAG_MODE_L]);
    SAG_ASSERT(sag_keymap_build(&ed->mode_keys[SAG_MODE_L], "mode:count",
                                rows, SAG_ARRAY_LEN(rows)));
    ed->keys.l[0] = &ed->mode_keys[SAG_MODE_L];
}

void test_count_repeat_and_takes_count(void)
{
    Ed ed;
    u32 i;

    count_editor(&ed);
    count_probe_reset();
    sag_dispatch_key(&ed, count_text_key('4'), 0);
    SAG_ASSERT(sag_dispatch_pending(&ed)->count_given);
    SAG_ASSERT_EQ_U64(sag_dispatch_pending(&ed)->count, 4U);
    sag_dispatch_key(&ed, count_named_key(SAG_KEY_DOWN), 0);
    SAG_ASSERT_EQ_U64(count_calls, 4U);
    SAG_ASSERT_EQ_U64(count_seen, 4U);
    SAG_ASSERT(count_given_seen);
    SAG_ASSERT(!sag_dispatch_pending(&ed)->count_given);

    count_probe_reset();
    sag_dispatch_key(&ed, count_text_key('1'), 0);
    sag_dispatch_key(&ed, count_text_key('0'), 0);
    sag_dispatch_key(&ed, count_named_key(SAG_KEY_DOWN), 0);
    SAG_ASSERT_EQ_U64(count_calls, 10U);
    SAG_ASSERT_EQ_U64(count_seen, 10U);
    SAG_ASSERT(count_given_seen);

    count_probe_reset();
    for (i = 0U; i < 7U; i++)
        sag_dispatch_key(&ed, count_text_key('9'), 0);
    SAG_ASSERT_EQ_U64(sag_dispatch_pending(&ed)->count, SAG_COUNT_MAX);
    SAG_ASSERT(strstr(sag_dispatch_message(&ed), "99999") != NULL);
    sag_dispatch_key(&ed, count_text_key('G'), 0);
    SAG_ASSERT_EQ_U64(count_calls, 1U);
    SAG_ASSERT_EQ_U64(count_seen, SAG_COUNT_MAX);
    SAG_ASSERT(count_given_seen);
    SAG_ASSERT(!sag_dispatch_pending(&ed)->count_given);
    sag_dispatch_free(&ed);
}

void test_count_zero_reset_and_escape(void)
{
    Ed ed;

    count_editor(&ed);
    count_probe_reset();
    sag_dispatch_key(&ed, count_text_key('0'), 0);
    SAG_ASSERT_EQ_U64(count_calls, 0U);
    SAG_ASSERT(!sag_dispatch_pending(&ed)->count_given);
    SAG_ASSERT(strstr(sag_dispatch_message(&ed), "unbound") != NULL);

    sag_dispatch_key(&ed, count_text_key('3'), 0);
    SAG_ASSERT(sag_dispatch_pending(&ed)->count_given);
    sag_dispatch_key(&ed, count_named_key(SAG_KEY_ESCAPE), 0);
    SAG_ASSERT(!sag_dispatch_pending(&ed)->count_given);
    SAG_ASSERT_EQ_U64(sag_dispatch_pending(&ed)->count, 0U);

    count_probe_reset();
    sag_dispatch_key(&ed, count_text_key('x'), 0);
    SAG_ASSERT_EQ_U64(count_calls, 1U);
    SAG_ASSERT_EQ_U64(count_seen, 1U);
    SAG_ASSERT(!count_given_seen);
    sag_dispatch_key(&ed, count_text_key('1'), 0);
    sag_dispatch_key(&ed, count_text_key('x'), 0);
    SAG_ASSERT_EQ_U64(count_calls, 2U);
    SAG_ASSERT_EQ_U64(count_seen, 1U);
    SAG_ASSERT(count_given_seen);
    sag_dispatch_free(&ed);
}

void test_count_insert_mode_digits_are_keys(void)
{
    static const BindRow rows[] = {
        {"4", "ed.ui.expand", 0, NULL},
    };
    Ed ed;

    count_editor(&ed);
    sag_keymap_free(&ed.mode_keys[SAG_MODE_I]);
    SAG_ASSERT(sag_keymap_build(&ed.mode_keys[SAG_MODE_I], "mode:I-test",
                                rows, SAG_ARRAY_LEN(rows)));
    sag_dispatch_set_mode(&ed, SAG_MODE_I);
    count_probe_reset();
    sag_dispatch_key(&ed, count_text_key('4'), 0);
    SAG_ASSERT_EQ_U64(count_calls, 1U);
    SAG_ASSERT_EQ_U64(count_seen, 1U);
    SAG_ASSERT(!count_given_seen);
    SAG_ASSERT(!sag_dispatch_pending(&ed)->count_given);
    sag_dispatch_free(&ed);
}
