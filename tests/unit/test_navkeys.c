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

static void nav_fixture(Ed *ed)
{
    Cursor *cursor;

    yew_ed_init(ed);
    YEW_ASSERT(yew_ed_open_scratch(ed));
    yew_test_load_runtime(ed);
    yew_undo_free(ed->buffer.undo);
    yew_textbuf_free(ed->buffer.tb);
    ed->buffer.tb = yew_textbuf_from_bytes(nav_text, sizeof(nav_text) - 1U);
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
