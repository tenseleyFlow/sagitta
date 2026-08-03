#include "harness.h"

#include <string.h>

#include "edit/ed.h"
#include "edit/mode.h"

static Key modes_key(u32 code)
{
    Key key = {0};

    key.code = code;
    key.kind = SAG_EV_KEY;
    key.ev = SAG_KEY_PRESS;
    if (code < 0x80U) {
        key.ntext = 1U;
        key.text[0] = (u8)code;
    }
    return key;
}

static void modes_editor(Ed *ed)
{
    sag_ed_init(ed);
    SAG_ASSERT(sag_ed_open_scratch(ed));
    SAG_ASSERT_EQ_U64(ed->mode, SAG_MODE_L);
    SAG_ASSERT_EQ_U64(ed->prev_unit, SAG_MODE_L);
}

void test_modes_escape_cancels_chord_before_prompt_or_mode(void)
{
    Ed ed;

    modes_editor(&ed);
    SAG_ASSERT_EQ_U64(sag_mode_enter(&ed, SAG_MODE_I), SAG_CMD_OK);
    ed.full_damage = false;
    sag_ed_prompt(&ed, SAG_PROMPT_QUIT_DIRTY);
    ed.chord.seq[0] = sag_keyid(modes_key((u32)'q'));
    ed.chord.n = 1U;
    ed.chord.layer = 0;
    ed.chord.deadline = 500;

    sag_ed_handle_key(&ed, modes_key(SAG_KEY_ESCAPE), 10);
    SAG_ASSERT_EQ_U64(ed.chord.n, 0U);
    SAG_ASSERT_EQ_I64(ed.chord.layer, -1);
    SAG_ASSERT_EQ_I64(ed.chord.deadline, 0);
    SAG_ASSERT_EQ_U64(ed.prompt, SAG_PROMPT_QUIT_DIRTY);
    SAG_ASSERT_EQ_U64(ed.mode, SAG_MODE_I);
    sag_ed_free(&ed);
}

void test_modes_escape_cancels_count_before_prompt_or_mode(void)
{
    Ed ed;

    modes_editor(&ed);
    SAG_ASSERT_EQ_U64(sag_mode_enter(&ed, SAG_MODE_I), SAG_CMD_OK);
    ed.full_damage = false;
    sag_ed_prompt(&ed, SAG_PROMPT_QUIT_DIRTY);
    ed.chord.count = 42U;
    ed.chord.count_given = true;

    sag_ed_handle_key(&ed, modes_key(SAG_KEY_ESCAPE), 10);
    SAG_ASSERT(!ed.chord.count_given);
    SAG_ASSERT_EQ_U64(ed.chord.count, 0U);
    SAG_ASSERT_EQ_U64(ed.prompt, SAG_PROMPT_QUIT_DIRTY);
    SAG_ASSERT_EQ_U64(ed.mode, SAG_MODE_I);
    sag_ed_free(&ed);
}

void test_modes_escape_closes_prompt_before_changing_mode(void)
{
    Ed ed;

    modes_editor(&ed);
    SAG_ASSERT_EQ_U64(sag_mode_enter(&ed, SAG_MODE_I), SAG_CMD_OK);
    ed.full_damage = false;
    sag_ed_prompt(&ed, SAG_PROMPT_QUIT_DIRTY);
    SAG_ASSERT(ed.msg.active);

    sag_ed_handle_key(&ed, modes_key(SAG_KEY_ESCAPE), 10);
    SAG_ASSERT_EQ_U64(ed.prompt, SAG_PROMPT_NONE);
    SAG_ASSERT(!ed.msg.active);
    SAG_ASSERT_EQ_U64(ed.mode, SAG_MODE_I);
    SAG_ASSERT(!ed.full_damage);
    sag_ed_free(&ed);
}

void test_modes_escape_from_insert_enters_line_and_repaints(void)
{
    Ed ed;

    modes_editor(&ed);
    SAG_ASSERT_EQ_U64(sag_mode_enter(&ed, SAG_MODE_I), SAG_CMD_OK);
    ed.full_damage = false;

    sag_ed_handle_key(&ed, modes_key(SAG_KEY_ESCAPE), 10);
    SAG_ASSERT_EQ_U64(ed.last_status, SAG_CMD_OK);
    SAG_ASSERT_EQ_U64(ed.mode, SAG_MODE_L);
    SAG_ASSERT_EQ_U64(ed.prev_unit, SAG_MODE_L);
    SAG_ASSERT(!ed.full_damage);
    SAG_ASSERT_EQ_U64(ed.keys.l[0], &ed.mode_keys[SAG_MODE_L]);
    sag_ed_free(&ed);
}

void test_modes_escape_in_line_is_repaint_noop(void)
{
    Ed ed;

    modes_editor(&ed);
    ed.full_damage = false;
    sag_ed_handle_key(&ed, modes_key(SAG_KEY_ESCAPE), 10);
    SAG_ASSERT_EQ_U64(ed.mode, SAG_MODE_L);
    SAG_ASSERT_EQ_U64(ed.last_status, SAG_CMD_OK);
    SAG_ASSERT(!ed.full_damage);
    SAG_ASSERT_EQ_U64(ed.dispatch_count, 1U);
    sag_ed_free(&ed);
}

void test_modes_deferred_entries_name_their_sprints(void)
{
    static const struct {
        char key;
        const char *mode;
        const char *sprint;
    } cases[] = {
        {'h', "H", "17"},
        {'e', "E", "18"},
        {'f', "F", "52"},
    };
    Ed ed;
    size_t i;

    modes_editor(&ed);
    for (i = 0U; i < SAG_ARRAY_LEN(cases); i++) {
        sag_ed_handle_key(&ed, modes_key((u32)(u8)cases[i].key), 10);
        SAG_ASSERT_EQ_U64(ed.last_status, SAG_CMD_ERR_DEFERRED);
        SAG_ASSERT_EQ_U64(ed.mode, SAG_MODE_L);
        SAG_ASSERT(ed.msg.active);
        SAG_ASSERT_EQ_U64(ed.msg.sev, SAG_MSG_ERROR);
        SAG_ASSERT(strstr(ed.msg.text, cases[i].mode) != NULL);
        SAG_ASSERT(strstr(ed.msg.text, cases[i].sprint) != NULL);
    }
    sag_ed_free(&ed);
}

void test_modes_only_line_and_insert_are_enterable_in_sprint14(void)
{
    Ed ed;

    modes_editor(&ed);
    SAG_ASSERT_EQ_U64(sag_mode_enter(&ed, SAG_MODE_I), SAG_CMD_OK);
    SAG_ASSERT_EQ_U64(ed.mode, SAG_MODE_I);
    SAG_ASSERT_EQ_U64(ed.prev_unit, SAG_MODE_L);
    SAG_ASSERT_EQ_U64(sag_mode_enter(&ed, SAG_MODE_L), SAG_CMD_OK);
    SAG_ASSERT_EQ_U64(ed.mode, SAG_MODE_L);
    SAG_ASSERT_EQ_U64(ed.prev_unit, SAG_MODE_L);
    SAG_ASSERT_EQ_U64(sag_mode_enter(&ed, SAG_MODE_W), SAG_CMD_OK);
    SAG_ASSERT_EQ_U64(ed.mode, SAG_MODE_W);
    SAG_ASSERT_EQ_U64(ed.prev_unit, SAG_MODE_W);
    SAG_ASSERT_EQ_U64(sag_mode_enter(&ed, SAG_MODE_B), SAG_CMD_OK);
    SAG_ASSERT_EQ_U64(ed.mode, SAG_MODE_B);
    SAG_ASSERT_EQ_U64(ed.prev_unit, SAG_MODE_B);
    SAG_ASSERT_EQ_U64(sag_mode_enter(&ed, SAG_MODE__N), SAG_CMD_ERR_ARG);
    SAG_ASSERT_EQ_U64(ed.mode, SAG_MODE_B);
    sag_ed_free(&ed);
}
