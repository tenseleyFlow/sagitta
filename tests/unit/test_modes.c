#include "harness.h"

#include <string.h>

#include "edit/ed.h"
#include "edit/mode.h"

static Key modes_key(u32 code)
{
    Key key = {0};

    key.code = code;
    key.kind = YEW_EV_KEY;
    key.ev = YEW_KEY_PRESS;
    if (code < 0x80U) {
        key.ntext = 1U;
        key.text[0] = (u8)code;
    }
    return key;
}

static void modes_editor(Ed *ed)
{
    yew_ed_init(ed);
    YEW_ASSERT(yew_ed_open_scratch(ed));
    yew_test_load_runtime(ed);
    YEW_ASSERT_EQ_U64(ed->mode, YEW_MODE_L);
    YEW_ASSERT_EQ_U64(ed->prev_unit, YEW_MODE_L);
}

void test_modes_escape_cancels_chord_before_prompt_or_mode(void)
{
    Ed ed;

    modes_editor(&ed);
    YEW_ASSERT_EQ_U64(yew_mode_enter(&ed, YEW_MODE_I), YEW_CMD_OK);
    ed.full_damage = false;
    yew_ed_prompt(&ed, YEW_PROMPT_QUIT_DIRTY);
    ed.chord.seq[0] = yew_keyid(modes_key((u32)'q'));
    ed.chord.n = 1U;
    ed.chord.layer = 0;
    ed.chord.deadline = 500;

    yew_ed_handle_key(&ed, modes_key(YEW_KEY_ESCAPE), 10);
    YEW_ASSERT_EQ_U64(ed.chord.n, 0U);
    YEW_ASSERT_EQ_I64(ed.chord.layer, -1);
    YEW_ASSERT_EQ_I64(ed.chord.deadline, 0);
    YEW_ASSERT_EQ_U64(ed.prompt, YEW_PROMPT_QUIT_DIRTY);
    YEW_ASSERT_EQ_U64(ed.mode, YEW_MODE_I);
    yew_ed_free(&ed);
}

void test_modes_escape_cancels_count_before_prompt_or_mode(void)
{
    Ed ed;

    modes_editor(&ed);
    YEW_ASSERT_EQ_U64(yew_mode_enter(&ed, YEW_MODE_I), YEW_CMD_OK);
    ed.full_damage = false;
    yew_ed_prompt(&ed, YEW_PROMPT_QUIT_DIRTY);
    ed.chord.count = 42U;
    ed.chord.count_given = true;

    yew_ed_handle_key(&ed, modes_key(YEW_KEY_ESCAPE), 10);
    YEW_ASSERT(!ed.chord.count_given);
    YEW_ASSERT_EQ_U64(ed.chord.count, 0U);
    YEW_ASSERT_EQ_U64(ed.prompt, YEW_PROMPT_QUIT_DIRTY);
    YEW_ASSERT_EQ_U64(ed.mode, YEW_MODE_I);
    yew_ed_free(&ed);
}

void test_modes_escape_closes_prompt_before_changing_mode(void)
{
    Ed ed;

    modes_editor(&ed);
    YEW_ASSERT_EQ_U64(yew_mode_enter(&ed, YEW_MODE_I), YEW_CMD_OK);
    ed.full_damage = false;
    yew_ed_prompt(&ed, YEW_PROMPT_QUIT_DIRTY);
    YEW_ASSERT(ed.msg.active);

    yew_ed_handle_key(&ed, modes_key(YEW_KEY_ESCAPE), 10);
    YEW_ASSERT_EQ_U64(ed.prompt, YEW_PROMPT_NONE);
    YEW_ASSERT(!ed.msg.active);
    YEW_ASSERT_EQ_U64(ed.mode, YEW_MODE_I);
    YEW_ASSERT(!ed.full_damage);
    yew_ed_free(&ed);
}

void test_modes_escape_from_insert_enters_line_and_repaints(void)
{
    Ed ed;

    modes_editor(&ed);
    YEW_ASSERT_EQ_U64(yew_mode_enter(&ed, YEW_MODE_I), YEW_CMD_OK);
    ed.full_damage = false;

    yew_ed_handle_key(&ed, modes_key(YEW_KEY_ESCAPE), 10);
    YEW_ASSERT_EQ_U64(ed.last_status, YEW_CMD_OK);
    YEW_ASSERT_EQ_U64(ed.mode, YEW_MODE_L);
    YEW_ASSERT_EQ_U64(ed.prev_unit, YEW_MODE_L);
    YEW_ASSERT(!ed.full_damage);
    YEW_ASSERT_EQ_U64(ed.keys.l[0], &ed.mode_keys[YEW_MODE_L]);
    yew_ed_free(&ed);
}

void test_modes_escape_in_line_is_repaint_noop(void)
{
    Ed ed;

    modes_editor(&ed);
    ed.full_damage = false;
    yew_ed_handle_key(&ed, modes_key(YEW_KEY_ESCAPE), 10);
    YEW_ASSERT_EQ_U64(ed.mode, YEW_MODE_L);
    YEW_ASSERT_EQ_U64(ed.last_status, YEW_CMD_OK);
    YEW_ASSERT(!ed.full_damage);
    YEW_ASSERT_EQ_U64(ed.dispatch_count, 1U);
    yew_ed_free(&ed);
}

void test_modes_deferred_entries_name_their_sprints(void)
{
    Ed ed;

    modes_editor(&ed);
    yew_ed_handle_key(&ed, modes_key((u32)'e'), 10);
    YEW_ASSERT_EQ_U64(ed.last_status, YEW_CMD_OK);
    YEW_ASSERT_EQ_U64(ed.mode, YEW_MODE_E);
    YEW_ASSERT(ed.cmdline.active);
    yew_ed_handle_key(&ed, modes_key(YEW_KEY_ESCAPE), 11);
    YEW_ASSERT_EQ_U64(ed.mode, YEW_MODE_L);
    YEW_ASSERT(!ed.cmdline.active);

    yew_ed_handle_key(&ed, modes_key((u32)'f'), 12);
    YEW_ASSERT_EQ_U64(ed.last_status, YEW_CMD_ERR_DEFERRED);
    YEW_ASSERT_EQ_U64(ed.mode, YEW_MODE_L);
    YEW_ASSERT(ed.msg.active);
    YEW_ASSERT_EQ_U64(ed.msg.sev, YEW_MSG_ERROR);
    YEW_ASSERT(strstr(ed.msg.text, "F") != NULL);
    YEW_ASSERT(strstr(ed.msg.text, "52") != NULL);
    yew_ed_free(&ed);
}

void test_modes_only_line_and_insert_are_enterable_in_sprint14(void)
{
    Ed ed;

    modes_editor(&ed);
    YEW_ASSERT_EQ_U64(yew_mode_enter(&ed, YEW_MODE_I), YEW_CMD_OK);
    YEW_ASSERT_EQ_U64(ed.mode, YEW_MODE_I);
    YEW_ASSERT_EQ_U64(ed.prev_unit, YEW_MODE_L);
    YEW_ASSERT_EQ_U64(yew_mode_enter(&ed, YEW_MODE_L), YEW_CMD_OK);
    YEW_ASSERT_EQ_U64(ed.mode, YEW_MODE_L);
    YEW_ASSERT_EQ_U64(ed.prev_unit, YEW_MODE_L);
    YEW_ASSERT_EQ_U64(yew_mode_enter(&ed, YEW_MODE_W), YEW_CMD_OK);
    YEW_ASSERT_EQ_U64(ed.mode, YEW_MODE_W);
    YEW_ASSERT_EQ_U64(ed.prev_unit, YEW_MODE_W);
    YEW_ASSERT_EQ_U64(yew_mode_enter(&ed, YEW_MODE_B), YEW_CMD_OK);
    YEW_ASSERT_EQ_U64(ed.mode, YEW_MODE_B);
    YEW_ASSERT_EQ_U64(ed.prev_unit, YEW_MODE_B);
    YEW_ASSERT_EQ_U64(yew_mode_enter(&ed, YEW_MODE_E), YEW_CMD_OK);
    YEW_ASSERT_EQ_U64(ed.mode, YEW_MODE_E);
    YEW_ASSERT(ed.cmdline.active);
    yew_cmdline_close(&ed, false);
    YEW_ASSERT_EQ_U64(ed.mode, YEW_MODE_B);
    YEW_ASSERT_EQ_U64(yew_mode_enter(&ed, YEW_MODE__N), YEW_CMD_ERR_ARG);
    YEW_ASSERT_EQ_U64(ed.mode, YEW_MODE_B);
    yew_ed_free(&ed);
}
