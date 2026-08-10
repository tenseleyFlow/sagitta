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
    return YEW_CMD_OK;
}

static CmdStatus count_take(CmdCtx *cx)
{
    count_calls++;
    count_seen = cx->count;
    count_given_seen = cx->count_given;
    return YEW_CMD_OK;
}

static CmdStatus count_plain(CmdCtx *cx)
{
    count_calls++;
    count_seen = cx->count;
    count_given_seen = cx->count_given;
    return YEW_CMD_OK;
}

static void count_register_commands(void)
{
    static const CmdDesc commands[] = {
        {"ed.ui.grow", count_repeat, YEW_ARITY_NONE,
         YEW_CMD_REPEATABLE, "repeat count probe", NULL},
        {"ed.ui.shrink", count_take, YEW_ARITY_NONE,
         YEW_CMD_TAKES_COUNT, "consuming count probe", NULL},
        {"ed.ui.expand", count_plain, YEW_ARITY_NONE,
         0U, "plain count probe", NULL},
    };
    size_t i;

    yew_cmd_init();
    for (i = 0U; i < YEW_ARRAY_LEN(commands); i++) {
        if (yew_cmd_lookup(commands[i].name,
                           (u32)strlen(commands[i].name)).v == 0U)
            (void)yew_cmd_register(&commands[i]);
    }
}

static Key count_text_key(char c)
{
    Key key = {0};

    key.code = (u32)(u8)c;
    key.kind = YEW_EV_KEY;
    key.ev = YEW_KEY_PRESS;
    key.ntext = 1U;
    key.text[0] = (u8)c;
    return key;
}

static Key count_named_key(u32 code)
{
    Key key = {0};

    key.code = code;
    key.kind = YEW_EV_KEY;
    key.ev = YEW_KEY_PRESS;
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
    yew_dispatch_init(ed);
    yew_keymap_free(&ed->mode_keys[YEW_MODE_L]);
    YEW_ASSERT(yew_keymap_build(&ed->mode_keys[YEW_MODE_L], "mode:count",
                                rows, YEW_ARRAY_LEN(rows)));
    ed->keys.l[0] = &ed->mode_keys[YEW_MODE_L];
}

void test_count_repeat_and_takes_count(void)
{
    Ed ed = {0};
    u32 i;

    count_editor(&ed);
    count_probe_reset();
    yew_dispatch_key(&ed, count_text_key('4'), 0);
    YEW_ASSERT(yew_dispatch_pending(&ed)->count_given);
    YEW_ASSERT_EQ_U64(yew_dispatch_pending(&ed)->count, 4U);
    yew_dispatch_key(&ed, count_named_key(YEW_KEY_DOWN), 0);
    YEW_ASSERT_EQ_U64(count_calls, 4U);
    YEW_ASSERT_EQ_U64(count_seen, 4U);
    YEW_ASSERT(count_given_seen);
    YEW_ASSERT(!yew_dispatch_pending(&ed)->count_given);

    count_probe_reset();
    yew_dispatch_key(&ed, count_text_key('1'), 0);
    yew_dispatch_key(&ed, count_text_key('0'), 0);
    yew_dispatch_key(&ed, count_named_key(YEW_KEY_DOWN), 0);
    YEW_ASSERT_EQ_U64(count_calls, 10U);
    YEW_ASSERT_EQ_U64(count_seen, 10U);
    YEW_ASSERT(count_given_seen);

    count_probe_reset();
    for (i = 0U; i < 7U; i++)
        yew_dispatch_key(&ed, count_text_key('9'), 0);
    YEW_ASSERT_EQ_U64(yew_dispatch_pending(&ed)->count, YEW_COUNT_MAX);
    YEW_ASSERT(strstr(yew_dispatch_message(&ed), "99999") != NULL);
    yew_dispatch_key(&ed, count_text_key('G'), 0);
    YEW_ASSERT_EQ_U64(count_calls, 1U);
    YEW_ASSERT_EQ_U64(count_seen, YEW_COUNT_MAX);
    YEW_ASSERT(count_given_seen);
    YEW_ASSERT(!yew_dispatch_pending(&ed)->count_given);
    yew_dispatch_free(&ed);
}

void test_count_zero_reset_and_escape(void)
{
    Ed ed = {0};

    count_editor(&ed);
    count_probe_reset();
    yew_dispatch_key(&ed, count_text_key('0'), 0);
    YEW_ASSERT_EQ_U64(count_calls, 0U);
    YEW_ASSERT(!yew_dispatch_pending(&ed)->count_given);
    YEW_ASSERT(strstr(yew_dispatch_message(&ed), "unbound") != NULL);

    yew_dispatch_key(&ed, count_text_key('3'), 0);
    YEW_ASSERT(yew_dispatch_pending(&ed)->count_given);
    yew_dispatch_key(&ed, count_named_key(YEW_KEY_ESCAPE), 0);
    YEW_ASSERT(!yew_dispatch_pending(&ed)->count_given);
    YEW_ASSERT_EQ_U64(yew_dispatch_pending(&ed)->count, 0U);

    count_probe_reset();
    yew_dispatch_key(&ed, count_text_key('x'), 0);
    YEW_ASSERT_EQ_U64(count_calls, 1U);
    YEW_ASSERT_EQ_U64(count_seen, 1U);
    YEW_ASSERT(!count_given_seen);
    yew_dispatch_key(&ed, count_text_key('1'), 0);
    yew_dispatch_key(&ed, count_text_key('x'), 0);
    YEW_ASSERT_EQ_U64(count_calls, 2U);
    YEW_ASSERT_EQ_U64(count_seen, 1U);
    YEW_ASSERT(count_given_seen);
    yew_dispatch_free(&ed);
}

void test_count_insert_mode_digits_are_keys(void)
{
    static const BindRow rows[] = {
        {"4", "ed.ui.expand", 0, NULL},
    };
    Ed ed = {0};

    count_editor(&ed);
    yew_keymap_free(&ed.mode_keys[YEW_MODE_I]);
    YEW_ASSERT(yew_keymap_build(&ed.mode_keys[YEW_MODE_I], "mode:I-test",
                                rows, YEW_ARRAY_LEN(rows)));
    yew_dispatch_set_mode(&ed, YEW_MODE_I);
    count_probe_reset();
    yew_dispatch_key(&ed, count_text_key('4'), 0);
    YEW_ASSERT_EQ_U64(count_calls, 1U);
    YEW_ASSERT_EQ_U64(count_seen, 1U);
    YEW_ASSERT(!count_given_seen);
    YEW_ASSERT(!yew_dispatch_pending(&ed)->count_given);
    yew_dispatch_free(&ed);
}
