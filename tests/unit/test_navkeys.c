/*
 * The navigation keys a caret user reaches for: the Home toggle, Insert
 * mode's Ctrl arrows, and its Alt arrows.  Every case drives real keys
 * through yew_ed_handle_key against the loaded runtime keymap, so the
 * bindings these pin are the ones a user actually gets.
 */
#include "harness.h"

#include <string.h>

#include "edit/ed.h"
#include "edit/mode.h"
#include "edit/shadow.h"
#include "ui/cmdline.h"

/*
 * Line 0 "    indented" -- four spaces of indent.
 * Line 1 "noindent"     -- no indent at all: first non-blank IS column 0.
 * Line 2 "      "       -- nothing but whitespace, the no-oscillation case.
 * Line 3 "\t  tabmix"   -- a tab and two spaces, so the indent is ragged.
 * Line 4 "last"         -- the final line, which carries no EOL.
 */
static const u8 nav_text[] =
    "    indented\n"
    "noindent\n"
    "      \n"
    "\t  tabmix\n"
    "last";

enum {
    NAV_L0 = 0U,
    NAV_L0_TEXT = 4U,
    NAV_L0_END = 12U,
    NAV_L1 = 13U,
    NAV_L1_END = 21U,
    NAV_L2 = 22U,
    NAV_L2_END = 28U,
    NAV_L3 = 29U,
    NAV_L3_TEXT = 32U,
    NAV_L3_END = 38U,
    NAV_L4 = 39U,
    NAV_L4_END = 43U
};

/* One word per gap, so every word start is an obvious offset:
 * alpha 0, beta 6, gamma 11, delta 17 (on the second line). */
static const u8 nav_words[] = "alpha beta gamma\ndelta";

static void nav_fixture_bytes(Ed *ed, const u8 *bytes, size_t len)
{
    Cursor *cursor;

    yew_ed_init(ed);
    YEW_ASSERT(yew_ed_open_scratch(ed));
    yew_test_load_runtime(ed);
    yew_undo_free(ed->buffer.undo);
    yew_textbuf_free(ed->buffer.tb);
    ed->buffer.tb = yew_textbuf_from_bytes(bytes, len);
    ed->buffer.undo = yew_undo_new(ed->buffer.tb);
    ed->buffer.meta.eol = YEW_EOL_LF;
    ed->buffer.meta.dominant_eol = YEW_EOL_LF;
    ed->buffer.meta.lf_count = 1U;
    ed->win->vp.rows = 8U;
    ed->win->vp.cols = 80U;
    ed->win->rect.h = 8U;
    ed->win->rect.w = 80U;
    cursor = yew_ed_cursor(ed);
    YEW_ASSERT_NOT_NULL(cursor);
    cursor->pos = BYTEOFF(0U);
    cursor->anchor = BYTEOFF(0U);
    cursor->goal_col = (GCol){0U};
}

static void nav_fixture(Ed *ed)
{
    nav_fixture_bytes(ed, nav_text, sizeof(nav_text) - 1U);
}

static void nav_word_fixture(Ed *ed)
{
    nav_fixture_bytes(ed, nav_words, sizeof(nav_words) - 1U);
}

static void nav_at(Ed *ed, u64 off)
{
    Cursor *cursor = yew_ed_cursor(ed);

    YEW_ASSERT_NOT_NULL(cursor);
    cursor->pos = BYTEOFF(off);
    cursor->anchor = BYTEOFF(off);
    cursor->goal_col = (GCol){0U};
}

static u64 nav_pos(const Ed *ed)
{
    const Cursor *cursor = &ed->win->cs.curs.data[ed->win->cs.primary];

    return cursor->pos.v;
}

static CmdStatus nav_invoke(Ed *ed, const char *name)
{
    CmdId id = yew_cmd_lookup(name, (u32)strlen(name));
    CmdCtx cx = {0};

    YEW_ASSERT(id.v != 0U);
    cx.ed = ed;
    cx.win = ed->win;
    cx.count = 1U;
    cx.source = YEW_SRC_TEST;
    return yew_ed_invoke(ed, id, &cx);
}

static void nav_home_toggle(Ed *ed)
{
    YEW_ASSERT_EQ_U64(nav_invoke(ed, "ed.move.line.home_toggle"),
                      YEW_CMD_OK);
}

/*
 * VSCode's Home: the first non-blank, then column 0, then back.  The
 * answer is a pure function of the caret's CURRENT offset against the
 * line's computed first non-blank -- no remembered previous keypress.
 */
void test_nav_home_toggle_alternates_indent_and_column_zero(void)
{
    Ed ed;

    nav_fixture(&ed);

    /* Right of the indent: indent, then column 0, then indent again. */
    nav_at(&ed, 8U);
    nav_home_toggle(&ed);
    YEW_ASSERT_EQ_U64(nav_pos(&ed), NAV_L0_TEXT);
    nav_home_toggle(&ed);
    YEW_ASSERT_EQ_U64(nav_pos(&ed), NAV_L0);
    nav_home_toggle(&ed);
    YEW_ASSERT_EQ_U64(nav_pos(&ed), NAV_L0_TEXT);

    /* From the end of the line the first press still lands on the text. */
    nav_at(&ed, NAV_L0_END);
    nav_home_toggle(&ed);
    YEW_ASSERT_EQ_U64(nav_pos(&ed), NAV_L0_TEXT);

    /* Inside the indent itself -- still not AT the first non-blank. */
    nav_at(&ed, 2U);
    nav_home_toggle(&ed);
    YEW_ASSERT_EQ_U64(nav_pos(&ed), NAV_L0_TEXT);

    yew_ed_free(&ed);
}

/* A line with no indent has its first non-blank AT column 0, so the two
 * stops coincide and Home is idempotent rather than oscillating. */
void test_nav_home_toggle_is_idempotent_without_indent(void)
{
    Ed ed;

    nav_fixture(&ed);
    nav_at(&ed, 17U);
    nav_home_toggle(&ed);
    YEW_ASSERT_EQ_U64(nav_pos(&ed), NAV_L1);
    nav_home_toggle(&ed);
    YEW_ASSERT_EQ_U64(nav_pos(&ed), NAV_L1);
    nav_home_toggle(&ed);
    YEW_ASSERT_EQ_U64(nav_pos(&ed), NAV_L1);
    YEW_ASSERT_EQ_U64(nav_pos(&ed), NAV_L1_END - 8U);
    yew_ed_free(&ed);
}

/*
 * A line that is entirely whitespace has NO first non-blank.  Landing on
 * its content end would make Home oscillate between column 0 and the end
 * of an empty-looking line, so the defined answer is column 0, always.
 */
void test_nav_home_toggle_parks_at_column_zero_on_a_blank_line(void)
{
    Ed ed;

    nav_fixture(&ed);
    nav_at(&ed, 25U);
    nav_home_toggle(&ed);
    YEW_ASSERT_EQ_U64(nav_pos(&ed), NAV_L2);
    nav_home_toggle(&ed);
    YEW_ASSERT_EQ_U64(nav_pos(&ed), NAV_L2);
    nav_at(&ed, NAV_L2_END);
    nav_home_toggle(&ed);
    YEW_ASSERT_EQ_U64(nav_pos(&ed), NAV_L2);
    yew_ed_free(&ed);
}

/* A tab followed by spaces is one ragged indent, not three stops. */
void test_nav_home_toggle_treats_mixed_tabs_and_spaces_as_one_indent(void)
{
    Ed ed;

    nav_fixture(&ed);
    nav_at(&ed, 36U);
    nav_home_toggle(&ed);
    YEW_ASSERT_EQ_U64(nav_pos(&ed), NAV_L3_TEXT);
    nav_home_toggle(&ed);
    YEW_ASSERT_EQ_U64(nav_pos(&ed), NAV_L3);
    nav_home_toggle(&ed);
    YEW_ASSERT_EQ_U64(nav_pos(&ed), NAV_L3_TEXT);

    /* Standing on the tab is inside the indent, not at its end. */
    nav_at(&ed, NAV_L3 + 1U);
    nav_home_toggle(&ed);
    YEW_ASSERT_EQ_U64(nav_pos(&ed), NAV_L3_TEXT);
    yew_ed_free(&ed);
}

/* The final line carries no EOL; its span ends at the buffer end. */
void test_nav_home_toggle_handles_the_unterminated_last_line(void)
{
    Ed ed;

    nav_fixture(&ed);
    nav_at(&ed, NAV_L4_END);
    nav_home_toggle(&ed);
    YEW_ASSERT_EQ_U64(nav_pos(&ed), NAV_L4);
    nav_home_toggle(&ed);
    YEW_ASSERT_EQ_U64(nav_pos(&ed), NAV_L4);
    yew_ed_free(&ed);
}

static Key nav_key(u32 code)
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

static Key nav_mod_key(u32 code, u16 mods)
{
    Key key = nav_key(code);

    key.mods = mods;
    return key;
}

static void nav_send(Ed *ed, Key key, i64 now)
{
    yew_ed_handle_key(ed, key, now);
    YEW_ASSERT_EQ_U64(ed->last_status, YEW_CMD_OK);
}

static void nav_expect_cmd(const Ed *ed, const char *name)
{
    YEW_ASSERT_EQ_U64(ed->last_cmd.v,
                      yew_cmd_lookup(name, (u32)strlen(name)).v);
}

/* The key, not just the command: `<home>` is the toggle in every mode
 * that binds it, which is what the user actually presses. */
static void nav_home_key_cycle(Ed *ed)
{
    nav_at(ed, 8U);
    nav_send(ed, nav_key(YEW_KEY_HOME), 0);
    nav_expect_cmd(ed, "ed.move.line.home_toggle");
    YEW_ASSERT_EQ_U64(nav_pos(ed), NAV_L0_TEXT);
    nav_send(ed, nav_key(YEW_KEY_HOME), 1);
    YEW_ASSERT_EQ_U64(nav_pos(ed), NAV_L0);
    nav_send(ed, nav_key(YEW_KEY_HOME), 2);
    YEW_ASSERT_EQ_U64(nav_pos(ed), NAV_L0_TEXT);
}

void test_nav_home_key_toggles_in_line_word_and_insert_modes(void)
{
    Ed ed;

    nav_fixture(&ed);
    YEW_ASSERT_EQ_U64(ed.mode, YEW_MODE_L);
    nav_home_key_cycle(&ed);
    yew_ed_free(&ed);

    nav_fixture(&ed);
    nav_send(&ed, nav_key((u32)'w'), 0);
    YEW_ASSERT_EQ_U64(ed.mode, YEW_MODE_W);
    nav_home_key_cycle(&ed);
    yew_ed_free(&ed);

    nav_fixture(&ed);
    nav_send(&ed, nav_key((u32)'i'), 0);
    YEW_ASSERT_EQ_U64(ed.mode, YEW_MODE_I);
    nav_home_key_cycle(&ed);
    yew_ed_free(&ed);
}

/* H keeps its anchor: the toggle is a motion, and a motion in H drags
 * the selection's moving end. */
void test_nav_home_key_toggles_under_every_highlight_unit(void)
{
    static const Mode units[] = {YEW_MODE_L, YEW_MODE_W, YEW_MODE_I};
    Ed ed;
    u32 i;

    for (i = 0U; i < YEW_ARRAY_LEN(units); i++) {
        nav_fixture(&ed);
        nav_at(&ed, 8U);
        YEW_ASSERT_EQ_U64(yew_mode_enter_highlight(&ed, units[i], false),
                          YEW_CMD_OK);
        YEW_ASSERT_EQ_U64(ed.mode, YEW_MODE_H);
        nav_send(&ed, nav_key(YEW_KEY_HOME), 0);
        nav_expect_cmd(&ed, "ed.move.line.home_toggle");
        YEW_ASSERT_EQ_U64(nav_pos(&ed), NAV_L0_TEXT);
        YEW_ASSERT_EQ_U64(
            ed.win->cs.curs.data[ed.win->cs.primary].anchor.v, 8U);
        nav_send(&ed, nav_key(YEW_KEY_HOME), 1);
        YEW_ASSERT_EQ_U64(nav_pos(&ed), NAV_L0);
        YEW_ASSERT_EQ_U64(
            ed.win->cs.curs.data[ed.win->cs.primary].anchor.v, 8U);
        yew_ed_free(&ed);
    }
}

/* E mode edits the command line's own buffer, so the toggle has to walk
 * that buffer's single line, not the document's. */
void test_nav_home_key_toggles_on_the_command_line(void)
{
    Ed ed;

    nav_fixture(&ed);
    yew_cmdline_open(&ed, YEW_PROMPT_CMD, "   set x");
    YEW_ASSERT(ed.cmdline.active);
    YEW_ASSERT_EQ_U64(ed.mode, YEW_MODE_E);
    YEW_ASSERT_EQ_U64(ed.cmdline.cur.pos.v, 8U);
    nav_send(&ed, nav_key(YEW_KEY_HOME), 0);
    nav_expect_cmd(&ed, "ed.move.line.home_toggle");
    YEW_ASSERT_EQ_U64(ed.cmdline.cur.pos.v, 3U);
    nav_send(&ed, nav_key(YEW_KEY_HOME), 1);
    YEW_ASSERT_EQ_U64(ed.cmdline.cur.pos.v, 0U);
    nav_send(&ed, nav_key(YEW_KEY_HOME), 2);
    YEW_ASSERT_EQ_U64(ed.cmdline.cur.pos.v, 3U);
    /* C-a is deliberately NOT the toggle: it is the unconditional
     * start-of-line the shell-key habit expects. */
    nav_send(&ed, nav_mod_key((u32)'a', YEW_MOD_CTRL), 3);
    nav_expect_cmd(&ed, "ed.move.line.home");
    YEW_ASSERT_EQ_U64(ed.cmdline.cur.pos.v, 0U);
    yew_cmdline_close(&ed, false);
    yew_ed_free(&ed);
}

/*
 * Motions in yew are single-cursor: yew_ed_dispatch_resolved only fans a
 * command out over the cursor set when it CHANGES the buffer.  The toggle
 * must behave exactly like its neighbours here -- move the primary, leave
 * the rest of the set alone -- so a multi-cursor session cannot be
 * surprised by one motion that is special.
 */
void test_nav_home_key_leaves_secondary_cursors_alone(void)
{
    Cursor second = {BYTEOFF(36U), {0U}, BYTEOFF(36U)};
    Ed ed;

    nav_fixture(&ed);
    nav_at(&ed, 8U);
    YEW_ASSERT(yew_cset_add(&ed.win->cs, second));
    YEW_ASSERT_EQ_U64(ed.win->cs.curs.len, 2U);

    nav_send(&ed, nav_key(YEW_KEY_HOME), 0);
    YEW_ASSERT_EQ_U64(ed.win->cs.curs.len, 2U);
    YEW_ASSERT_EQ_U64(ed.win->cs.curs.data[ed.win->cs.primary].pos.v,
                      NAV_L0_TEXT);
    YEW_ASSERT_EQ_U64(ed.win->cs.curs.data[1U].pos.v, 36U);
    nav_send(&ed, nav_key(YEW_KEY_HOME), 1);
    YEW_ASSERT_EQ_U64(ed.win->cs.curs.data[ed.win->cs.primary].pos.v,
                      NAV_L0);
    YEW_ASSERT_EQ_U64(ed.win->cs.curs.data[1U].pos.v, 36U);
    yew_ed_free(&ed);
}

/*
 * Deliberately NOT the usual editor convention.  In yew the WORD jump is
 * Alt+arrow; Ctrl+arrow in Insert mode is the line's start and end, the
 * same pair `<home>`/`<end>` give there.  Do not "fix" this into a word
 * motion -- W mode's Ctrl arrows are the subword motions and stay that
 * way, which the second half of this test pins.
 */
void test_nav_ctrl_arrows_reach_the_line_ends_in_insert_mode(void)
{
    Ed ed;

    nav_fixture(&ed);
    nav_send(&ed, nav_key((u32)'i'), 0);
    YEW_ASSERT_EQ_U64(ed.mode, YEW_MODE_I);

    nav_at(&ed, 8U);
    nav_send(&ed, nav_mod_key(YEW_KEY_LEFT, YEW_MOD_CTRL), 1);
    nav_expect_cmd(&ed, "ed.move.line.home_toggle");
    YEW_ASSERT_EQ_U64(nav_pos(&ed), NAV_L0_TEXT);
    nav_send(&ed, nav_mod_key(YEW_KEY_LEFT, YEW_MOD_CTRL), 2);
    YEW_ASSERT_EQ_U64(nav_pos(&ed), NAV_L0);

    nav_send(&ed, nav_mod_key(YEW_KEY_RIGHT, YEW_MOD_CTRL), 3);
    nav_expect_cmd(&ed, "ed.move.line.end");
    YEW_ASSERT_EQ_U64(nav_pos(&ed), NAV_L0_END);
    nav_send(&ed, nav_mod_key(YEW_KEY_RIGHT, YEW_MOD_CTRL), 4);
    YEW_ASSERT_EQ_U64(nav_pos(&ed), NAV_L0_END);

    /* A line whose indent is the whole line: both ends still resolve. */
    nav_at(&ed, NAV_L2 + 3U);
    nav_send(&ed, nav_mod_key(YEW_KEY_LEFT, YEW_MOD_CTRL), 5);
    YEW_ASSERT_EQ_U64(nav_pos(&ed), NAV_L2);
    nav_send(&ed, nav_mod_key(YEW_KEY_RIGHT, YEW_MOD_CTRL), 6);
    YEW_ASSERT_EQ_U64(nav_pos(&ed), NAV_L2_END);
    yew_ed_free(&ed);

    /* W mode keeps its subword Ctrl arrows. */
    nav_fixture(&ed);
    nav_send(&ed, nav_key((u32)'w'), 0);
    YEW_ASSERT_EQ_U64(ed.mode, YEW_MODE_W);
    nav_at(&ed, 8U);
    nav_send(&ed, nav_mod_key(YEW_KEY_LEFT, YEW_MOD_CTRL), 1);
    nav_expect_cmd(&ed, "ed.move.word.sub_prev");
    nav_send(&ed, nav_mod_key(YEW_KEY_RIGHT, YEW_MOD_CTRL), 2);
    nav_expect_cmd(&ed, "ed.move.word.sub_next");
    yew_ed_free(&ed);
}

/*
 * The word unit's own motions, reachable from any mode.  W mode gets word
 * stepping from `ed.move.unit.next/prev` because its UNIT is the word;
 * Insert mode's unit is the grapheme, so its Alt arrows need a motion that
 * names the word outright.
 */
void test_nav_word_motions_step_between_word_starts(void)
{
    Ed ed;

    nav_word_fixture(&ed);
    nav_at(&ed, 0U);
    YEW_ASSERT_EQ_U64(nav_invoke(&ed, "ed.move.word.next"), YEW_CMD_OK);
    YEW_ASSERT_EQ_U64(nav_pos(&ed), 6U);
    YEW_ASSERT_EQ_U64(nav_invoke(&ed, "ed.move.word.next"), YEW_CMD_OK);
    YEW_ASSERT_EQ_U64(nav_pos(&ed), 11U);
    /* Across the newline, because a word jump is not a line motion. */
    YEW_ASSERT_EQ_U64(nav_invoke(&ed, "ed.move.word.next"), YEW_CMD_OK);
    YEW_ASSERT_EQ_U64(nav_pos(&ed), 17U);
    YEW_ASSERT_EQ_U64(nav_invoke(&ed, "ed.move.word.next"), YEW_CMD_OK);
    YEW_ASSERT_EQ_U64(nav_pos(&ed), 22U);
    YEW_ASSERT_EQ_U64(nav_invoke(&ed, "ed.move.word.next"), YEW_CMD_OK);
    YEW_ASSERT_EQ_U64(nav_pos(&ed), 22U);

    /* Backwards: from mid-word to this word's start, then the one before. */
    nav_at(&ed, 13U);
    YEW_ASSERT_EQ_U64(nav_invoke(&ed, "ed.move.word.prev"), YEW_CMD_OK);
    YEW_ASSERT_EQ_U64(nav_pos(&ed), 11U);
    YEW_ASSERT_EQ_U64(nav_invoke(&ed, "ed.move.word.prev"), YEW_CMD_OK);
    YEW_ASSERT_EQ_U64(nav_pos(&ed), 6U);
    YEW_ASSERT_EQ_U64(nav_invoke(&ed, "ed.move.word.prev"), YEW_CMD_OK);
    YEW_ASSERT_EQ_U64(nav_pos(&ed), 0U);
    YEW_ASSERT_EQ_U64(nav_invoke(&ed, "ed.move.word.prev"), YEW_CMD_OK);
    YEW_ASSERT_EQ_U64(nav_pos(&ed), 0U);

    nav_at(&ed, 17U);
    YEW_ASSERT_EQ_U64(nav_invoke(&ed, "ed.move.word.prev"), YEW_CMD_OK);
    YEW_ASSERT_EQ_U64(nav_pos(&ed), 11U);
    yew_ed_free(&ed);
}

static void nav_deliver_ghost(Ed *ed, const char *text)
{
    ShadowSug suggestion = {0};
    Shadow *shadow = &ed->win->shadow;

    suggestion.seq = shadow->seq_next[YEW_SHADOW_INDEX]++;
    suggestion.prov = (u8)YEW_SHADOW_INDEX;
    suggestion.buf_id = ed->win->buf->id;
    suggestion.buf_gen = ed->win->buf->tb->gen;
    suggestion.pos = ed->win->cs.curs.data[ed->win->cs.primary].pos;
    suggestion.text = (const u8 *)text;
    suggestion.len = (u32)strlen(text);
    yew_shadow_deliver(ed, &suggestion);
    YEW_ASSERT(ed->win->shadow.live);
}

/* Alt+arrow is yew's word jump.  Alt+Left is unconditional. */
void test_nav_alt_left_jumps_a_word_in_insert_mode(void)
{
    Ed ed;

    nav_word_fixture(&ed);
    nav_send(&ed, nav_key((u32)'i'), 0);
    YEW_ASSERT_EQ_U64(ed.mode, YEW_MODE_I);
    nav_at(&ed, 13U);
    nav_send(&ed, nav_mod_key(YEW_KEY_LEFT, YEW_MOD_ALT), 1);
    nav_expect_cmd(&ed, "ed.move.word.prev");
    YEW_ASSERT_EQ_U64(nav_pos(&ed), 11U);
    nav_send(&ed, nav_mod_key(YEW_KEY_LEFT, YEW_MOD_ALT), 2);
    YEW_ASSERT_EQ_U64(nav_pos(&ed), 6U);
    yew_ed_free(&ed);
}

/*
 * Alt+Right stays contextual: it accepts one word of the ghost while a
 * suggestion is actually showing, and is the word jump otherwise, so the
 * key is never dead.  "Showing" is the shadow's own drawn predicate --
 * live and not suppressed -- not a guess.
 */
void test_nav_alt_right_jumps_a_word_without_a_live_suggestion(void)
{
    Ed ed;

    nav_word_fixture(&ed);
    nav_send(&ed, nav_key((u32)'i'), 0);
    nav_at(&ed, 16U);
    YEW_ASSERT(!ed.win->shadow.live);
    nav_send(&ed, nav_mod_key(YEW_KEY_RIGHT, YEW_MOD_ALT), 1);
    nav_expect_cmd(&ed, "ed.shadow.accept_or_word");
    YEW_ASSERT_EQ_U64(ed.shadow_stats.accepted_word, 0U);
    YEW_ASSERT_EQ_U64(yew_textbuf_len(ed.win->buf->tb), 22U);
    YEW_ASSERT_EQ_U64(nav_pos(&ed), 17U);
    nav_send(&ed, nav_mod_key(YEW_KEY_RIGHT, YEW_MOD_ALT), 2);
    YEW_ASSERT_EQ_U64(nav_pos(&ed), 22U);
    yew_ed_free(&ed);
}

void test_nav_alt_right_accepts_while_a_suggestion_is_showing(void)
{
    Ed ed;
    u64 grown;

    nav_word_fixture(&ed);
    nav_send(&ed, nav_key((u32)'i'), 0);
    nav_at(&ed, 16U);
    nav_deliver_ghost(&ed, " tail more");

    nav_send(&ed, nav_mod_key(YEW_KEY_RIGHT, YEW_MOD_ALT), 1);
    nav_expect_cmd(&ed, "ed.shadow.accept_or_word");
    YEW_ASSERT_EQ_U64(ed.shadow_stats.accepted_word, 1U);
    grown = yew_textbuf_len(ed.win->buf->tb);
    YEW_ASSERT(grown > 22U);
    YEW_ASSERT_EQ_U64(nav_pos(&ed), 16U + (grown - 22U));

    /* A menu over the ghost suppresses the drawing, so the key has to be
     * the motion again even though the suggestion is still live. */
    ed.win->shadow.suppressed = true;
    YEW_ASSERT(ed.win->shadow.live);
    nav_send(&ed, nav_mod_key(YEW_KEY_RIGHT, YEW_MOD_ALT), 2);
    YEW_ASSERT_EQ_U64(ed.shadow_stats.accepted_word, 1U);
    YEW_ASSERT_EQ_U64(yew_textbuf_len(ed.win->buf->tb), grown);
    yew_ed_free(&ed);
}

/* A passive suggestion is single-cursor only, so a multi-cursor window
 * can only ever take the motion half of the dispatcher. */
void test_nav_alt_right_is_the_word_jump_with_many_cursors(void)
{
    Cursor second = {BYTEOFF(17U), {0U}, BYTEOFF(17U)};
    Ed ed;

    nav_word_fixture(&ed);
    nav_send(&ed, nav_key((u32)'i'), 0);
    nav_at(&ed, 0U);
    YEW_ASSERT(yew_cset_add(&ed.win->cs, second));
    YEW_ASSERT_EQ_U64(ed.win->cs.curs.len, 2U);
    YEW_ASSERT(!ed.win->shadow.live);

    nav_send(&ed, nav_mod_key(YEW_KEY_RIGHT, YEW_MOD_ALT), 1);
    YEW_ASSERT_EQ_U64(ed.shadow_stats.accepted_word, 0U);
    YEW_ASSERT_EQ_U64(yew_textbuf_len(ed.win->buf->tb), 22U);
    YEW_ASSERT_EQ_U64(ed.win->cs.curs.data[ed.win->cs.primary].pos.v, 6U);
    YEW_ASSERT_EQ_U64(ed.win->cs.curs.data[1U].pos.v, 17U);
    nav_send(&ed, nav_mod_key(YEW_KEY_LEFT, YEW_MOD_ALT), 2);
    YEW_ASSERT_EQ_U64(ed.win->cs.curs.data[ed.win->cs.primary].pos.v, 0U);
    YEW_ASSERT_EQ_U64(ed.win->cs.curs.data[1U].pos.v, 17U);
    yew_ed_free(&ed);
}
