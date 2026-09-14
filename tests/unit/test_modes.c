#include "harness.h"

#include <string.h>

#include "edit/ed.h"
#include "edit/mode.h"

/* The same key with modifiers held, for the global tab jumps. */
static Key modes_key_mods(u32 code, u16 mods)
{
    Key key = {0};

    key.code = code;
    key.kind = YEW_EV_KEY;
    key.ev = YEW_KEY_PRESS;
    key.mods = mods;
    return key;
}

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
#if YEW_WITH_FUSS
    YEW_ASSERT_EQ_U64(ed.last_status, YEW_CMD_OK);
    YEW_ASSERT_EQ_U64(ed.mode, YEW_MODE_F);
    yew_ed_handle_key(&ed, modes_key(YEW_KEY_ESCAPE), 13);
    YEW_ASSERT_EQ_U64(ed.last_status, YEW_CMD_OK);
    YEW_ASSERT_EQ_U64(ed.mode, YEW_MODE_L);
#else
    YEW_ASSERT_EQ_U64(ed.last_status, YEW_CMD_ERR_STATE);
    YEW_ASSERT_EQ_U64(ed.mode, YEW_MODE_L);
    YEW_ASSERT(ed.msg.active);
    YEW_ASSERT_EQ_U64(ed.msg.sev, YEW_MSG_ERROR);
    YEW_ASSERT_NOT_NULL(strstr(ed.msg.text, "no fuss module"));
#endif
    yew_ed_free(&ed);
}

void test_modes_bang_seeds_shell_command_from_command_modes(void)
{
    static const Mode sources[] = {
        YEW_MODE_L, YEW_MODE_W, YEW_MODE_B, YEW_MODE_H,
    };
    size_t i;

    for (i = 0U; i < YEW_ARRAY_LEN(sources); i++) {
        const u8 *bytes;
        u64 len;
        TextIter iter;
        Ed ed;

        modes_editor(&ed);
        if (sources[i] != YEW_MODE_L)
            YEW_ASSERT_EQ_U64(yew_mode_enter(&ed, sources[i]), YEW_CMD_OK);
        yew_ed_handle_key(&ed, modes_key((u32)'!'), 10);
        YEW_ASSERT_EQ_U64(ed.last_status, YEW_CMD_OK);
        YEW_ASSERT_EQ_U64(ed.mode, YEW_MODE_E);
        YEW_ASSERT(ed.cmdline.active);
        YEW_ASSERT_EQ_U64(ed.cmdline.kind, YEW_PROMPT_CMD);
        YEW_ASSERT_EQ_U64(ed.cmdline.return_mode, sources[i]);
        YEW_ASSERT_EQ_U64(yew_textbuf_len(ed.cmdline.buf), 1U);
        YEW_ASSERT(yew_textiter_begin(&iter, ed.cmdline.buf, BYTEOFF(0U)));
        YEW_ASSERT(yew_textiter_chunk(&iter, ed.cmdline.buf, &bytes, &len));
        YEW_ASSERT_EQ_U64(len, 1U);
        YEW_ASSERT_EQ_U64(bytes[0], (u8)'!');
        yew_ed_free(&ed);
    }
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
#if YEW_WITH_FUSS
    YEW_ASSERT_EQ_U64(yew_mode_enter(&ed, YEW_MODE_F), YEW_CMD_OK);
    YEW_ASSERT_EQ_U64(ed.mode, YEW_MODE_F);
    YEW_ASSERT_EQ_U64(yew_mode_enter(&ed, YEW_MODE_L), YEW_CMD_OK);
    YEW_ASSERT_EQ_U64(ed.mode, YEW_MODE_L);
#else
    YEW_ASSERT_EQ_U64(yew_mode_enter(&ed, YEW_MODE_F), YEW_CMD_ERR_STATE);
    YEW_ASSERT_EQ_U64(ed.mode, YEW_MODE_B);
#endif
    YEW_ASSERT_EQ_U64(yew_mode_enter(&ed, YEW_MODE__N), YEW_CMD_ERR_ARG);
#if YEW_WITH_FUSS
    YEW_ASSERT_EQ_U64(ed.mode, YEW_MODE_L);
#else
    YEW_ASSERT_EQ_U64(ed.mode, YEW_MODE_B);
#endif
    yew_ed_free(&ed);
}

/*
 * Sprint 57.19: every unit mode reaches every other with ONE keypress.
 *
 * Word and Block are arrow-driven, so before this their only letter was
 * `h`: getting from Block back to Line meant Escape first, and Escape is
 * a different intent — it abandons what you were doing. A mode switch
 * should not have to be spelled as a cancel.
 *
 * Driven through yew_ed_handle_key against the real runtime bindings,
 * so this pins the keymap the user actually gets rather than the table
 * a test built for itself.
 */
void test_modes_every_unit_mode_switches_in_one_key(void)
{
    static const struct {
        Mode from;
        u32 key;
        Mode want;
    } steps[] = {
        {YEW_MODE_L, (u32)'w', YEW_MODE_W},
        {YEW_MODE_L, (u32)'b', YEW_MODE_B},
        {YEW_MODE_W, (u32)'l', YEW_MODE_L},
        {YEW_MODE_W, (u32)'b', YEW_MODE_B},
        {YEW_MODE_W, (u32)'i', YEW_MODE_I},
        {YEW_MODE_B, (u32)'l', YEW_MODE_L},
        {YEW_MODE_B, (u32)'w', YEW_MODE_W},
        {YEW_MODE_B, (u32)'i', YEW_MODE_I}
    };
    size_t i;

    for (i = 0U; i < YEW_ARRAY_LEN(steps); i++) {
        Ed ed;

        modes_editor(&ed);
        YEW_ASSERT_EQ_U64(yew_mode_enter(&ed, steps[i].from), YEW_CMD_OK);
        YEW_ASSERT_EQ_U64(ed.mode, steps[i].from);
        yew_ed_handle_key(&ed, modes_key(steps[i].key), 10);
        YEW_ASSERT_EQ_U64(ed.mode, steps[i].want);
        yew_ed_free(&ed);
    }
}

/*
 * FUSS opens from every resting unit mode, not only Line.
 *
 * `f` was bound in L alone, so reaching the file tree from Word or Block
 * meant a detour through Line first — the same "spell a mode change as
 * something else" problem the one-key switches removed. Insert and
 * Execute are excluded because `f` is text there, and Highlight because
 * entering FUSS would silently drop a live selection.
 *
 * Guarded like its sibling above so a MODULES="" build asserts the
 * refusal rather than skipping the case.
 */
void test_modes_fuss_opens_from_every_unit_mode(void)
{
    static const Mode from[] = {YEW_MODE_L, YEW_MODE_W, YEW_MODE_B};
    size_t i;

    for (i = 0U; i < YEW_ARRAY_LEN(from); i++) {
        Ed ed;

        modes_editor(&ed);
        YEW_ASSERT_EQ_U64(yew_mode_enter(&ed, from[i]), YEW_CMD_OK);
        YEW_ASSERT_EQ_U64(ed.mode, from[i]);
        yew_ed_handle_key(&ed, modes_key((u32)'f'), 10);
#if YEW_WITH_FUSS
        YEW_ASSERT_EQ_U64(ed.last_status, YEW_CMD_OK);
        YEW_ASSERT_EQ_U64(ed.mode, YEW_MODE_F);
#else
        YEW_ASSERT_EQ_U64(ed.last_status, YEW_CMD_ERR_STATE);
        YEW_ASSERT_EQ_U64(ed.mode, from[i]);
        YEW_ASSERT_NOT_NULL(strstr(ed.msg.text, "no fuss module"));
#endif
        yew_ed_free(&ed);
    }
}

/*
 * The numbered tab jumps are global, and Insert keeps what you type.
 *
 * FIELD REPORT: alt+N and ctrl+N only fired in Line mode, so switching
 * tabs meant leaving whatever mode you were in and coming back.
 *
 * Insert carries a hazard the other modes do not. The 500 ms
 * digit-extension window (Sprint 24 §7) sits BEFORE the insert text
 * path in dispatch, precisely so `alt+1` `5` reaches tab 15 rather than
 * typing a 5 into the document. In Insert that is exactly backwards: a
 * digit there is the character the user is writing. So the jump fires
 * and the window is NOT armed, which costs two-digit jumps in Insert
 * and never costs a keystroke. The same holds in FUSS, whose
 * type-to-jump reads bare printables.
 */
void test_modes_tab_jumps_are_global_and_insert_keeps_its_digits(void)
{
    static const Mode from[] = {YEW_MODE_L, YEW_MODE_W, YEW_MODE_B,
                                YEW_MODE_I, YEW_MODE_H};
    size_t i;

    for (i = 0U; i < YEW_ARRAY_LEN(from); i++) {
        Ed ed;

        modes_editor(&ed);
        YEW_ASSERT(yew_tab_open(&ed, "/tmp/yew-jump-a.txt") >= 0);
        YEW_ASSERT(yew_tab_open(&ed, "/tmp/yew-jump-b.txt") >= 0);
        YEW_ASSERT_EQ_U64(yew_mode_enter(&ed, from[i]), YEW_CMD_OK);
        yew_tab_switch(&ed, 2);
        ed.now_ms = 1000;

        /* alt+1 reaches tab 1 from every mode... */
        yew_ed_handle_key(&ed, modes_key_mods((u32)'1', YEW_MOD_ALT), 1000);
        YEW_ASSERT_EQ_I64(ed.tabs.active, 0);
        /* ...and does not change the mode on the way. */
        YEW_ASSERT_EQ_U64(ed.mode, from[i]);

        /* The extension window arms everywhere a bare digit is not text. */
        if (from[i] == YEW_MODE_I)
            YEW_ASSERT(!yew_tab_jump_armed());
        else
            YEW_ASSERT(yew_tab_jump_armed());

        yew_tab_jump_clear(&ed);
        yew_ed_free(&ed);
    }
}
