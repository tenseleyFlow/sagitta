#include "harness.h"

#include <string.h>

#include "edit/ed.h"
#include "edit/edit_cmds.h"
#include "edit/motion.h"

static Key h_key(u32 code)
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

static void h_editor(Ed *ed, const char *text)
{
    sag_ed_init(ed);
    SAG_ASSERT(sag_ed_open_scratch(ed));
    if (text != NULL)
        sag_textbuf_insert(ed->win->buf->tb, BYTEOFF(0U),
                           (const u8 *)text, strlen(text));
}

static void h_place(Ed *ed, u64 pos, u64 anchor)
{
    Cursor *cursor = &ed->win->cs.curs.data[ed->win->cs.primary];
    Span line;

    cursor->pos = BYTEOFF(pos);
    cursor->anchor = BYTEOFF(anchor);
    line = sag_textbuf_line_span(ed->win->buf->tb,
                                 sag_textbuf_line_of(ed->win->buf->tb,
                                                     cursor->pos));
    cursor->goal_col = sag_off_to_gcol(ed->win->buf->tb, line, cursor->pos);
}

static void h_assert_text(const Ed *ed, const u8 *want, size_t want_len)
{
    Bytebuf got;
    TextIter it;

    bytebuf_init(&got);
    if (sag_textiter_begin(&it, ed->buffer.tb, BYTEOFF(0U))) {
        do {
            const u8 *bytes;
            u64 len;

            SAG_ASSERT(sag_textiter_chunk(&it, ed->buffer.tb, &bytes,
                                          &len));
            bytebuf_append(&got, bytes, (size_t)len);
        } while (sag_textiter_advance(&it, ed->buffer.tb));
    }
    SAG_ASSERT_EQ_U64(got.len, want_len);
    SAG_ASSERT_EQ_MEM(got.data, want, want_len);
    bytebuf_free(&got);
}

void test_hmode_borrows_l_w_b_and_i_char_units(void)
{
    static const struct {
        Mode source;
        const UnitOps *unit;
    } cases[] = {{SAG_MODE_L, &sag_unit_line},
                 {SAG_MODE_W, &sag_unit_word},
                 {SAG_MODE_B, &sag_unit_block},
                 {SAG_MODE_I, &sag_unit_char}};
    Ed ed;
    size_t i;

    h_editor(&ed, "one two\nthree\n");
    for (i = 0U; i < SAG_ARRAY_LEN(cases); i++) {
        SAG_ASSERT_EQ_U64(sag_mode_enter(&ed, cases[i].source), SAG_CMD_OK);
        SAG_ASSERT_EQ_U64(sag_mode_enter(&ed, SAG_MODE_H), SAG_CMD_OK);
        SAG_ASSERT_EQ_U64(ed.mode, SAG_MODE_H);
        SAG_ASSERT_EQ_U64(ed.win->h.from, cases[i].source);
        SAG_ASSERT_EQ_U64(ed.win->h.unit, cases[i].unit);
        SAG_ASSERT_EQ_U64(ed.win->h.kind, SAG_SEL_CHAR);
        SAG_ASSERT(!ed.win->h.sticky);
    }
    sag_ed_free(&ed);
}

void test_hmode_sticky_entry_parses_unit_in_one_string_argument(void)
{
    static const struct {
        const char *arg;
        Mode from;
        const UnitOps *unit;
    } cases[] = {{"H L", SAG_MODE_L, &sag_unit_line},
                 {"H W", SAG_MODE_W, &sag_unit_word},
                 {"H B", SAG_MODE_B, &sag_unit_block},
                 {"H C", SAG_MODE_I, &sag_unit_char}};
    Ed ed;
    CmdCtx cx = {0};
    size_t i;

    h_editor(&ed, "text");
    cx.ed = &ed;
    cx.win = ed.win;
    for (i = 0U; i < SAG_ARRAY_LEN(cases); i++) {
        cx.sarg = cases[i].arg;
        cx.sarg_len = 3U;
        SAG_ASSERT_EQ_U64(sag_edit_cmd_mode_enter(&cx), SAG_CMD_OK);
        SAG_ASSERT_EQ_U64(ed.win->h.from, cases[i].from);
        SAG_ASSERT_EQ_U64(ed.win->h.unit, cases[i].unit);
        SAG_ASSERT(ed.win->h.sticky);
    }
    cx.sarg = "H X";
    SAG_ASSERT_EQ_U64(sag_edit_cmd_mode_enter(&cx), SAG_CMD_ERR_ARG);
    sag_ed_free(&ed);
}

void test_hmode_char_motion_kind_prefix_swap_and_escape_preserve_contract(void)
{
    Ed ed;
    CmdCtx cx = {0};
    Cursor extra;

    h_editor(&ed, "abcdef");
    h_place(&ed, 1U, 1U);
    cx.ed = &ed;
    cx.win = ed.win;
    cx.sarg = "H C";
    cx.sarg_len = 3U;
    SAG_ASSERT_EQ_U64(sag_edit_cmd_mode_enter(&cx), SAG_CMD_OK);
    SAG_ASSERT_EQ_U64(sag_edit_cmd_move_unit_next(&cx), SAG_CMD_OK);
    SAG_ASSERT_EQ_U64(ed.win->cs.curs.data[0].pos.v, 2U);
    SAG_ASSERT_EQ_U64(ed.win->cs.curs.data[0].anchor.v, 1U);

    sag_ed_handle_key(&ed, h_key((u32)'v'), 10);
    sag_ed_handle_key(&ed, h_key((u32)'r'), 11);
    SAG_ASSERT_EQ_U64(ed.win->h.kind, SAG_SEL_RECT);
    SAG_ASSERT_EQ_U64(sag_edit_cmd_sel_swap_ends(&cx), SAG_CMD_OK);
    SAG_ASSERT_EQ_U64(ed.win->cs.curs.data[0].pos.v, 1U);
    SAG_ASSERT_EQ_U64(ed.win->cs.curs.data[0].anchor.v, 2U);

    extra = (Cursor){BYTEOFF(4U), {4U}, BYTEOFF(4U)};
    SAG_ASSERT(sag_cset_add(&ed.win->cs, extra));
    extra = (Cursor){BYTEOFF(5U), {5U}, BYTEOFF(5U)};
    SAG_ASSERT(sag_cset_add(&ed.win->cs, extra));
    SAG_ASSERT_EQ_U64(ed.win->cs.curs.len, 3U);
    SAG_ASSERT_EQ_U64(sag_mode_escape(&ed), SAG_CMD_OK);
    SAG_ASSERT_EQ_U64(ed.mode, SAG_MODE_L);
    SAG_ASSERT_EQ_U64(ed.win->cs.curs.len, 1U);
    SAG_ASSERT_EQ_U64(ed.win->cs.curs.data[0].anchor.v,
                      ed.win->cs.curs.data[0].pos.v);
    sag_ed_free(&ed);
}

void test_hmode_word_arrows_mirror_word_layer_and_keep_anchor(void)
{
    Ed ed;

    h_editor(&ed, "one two\nthree four");
    h_place(&ed, 4U, 4U);
    SAG_ASSERT_EQ_U64(sag_mode_enter(&ed, SAG_MODE_W), SAG_CMD_OK);
    SAG_ASSERT_EQ_U64(sag_mode_enter(&ed, SAG_MODE_H), SAG_CMD_OK);
    sag_ed_handle_key(&ed, h_key(SAG_KEY_DOWN), 10);
    SAG_ASSERT_EQ_U64(sag_textbuf_line_of(ed.win->buf->tb,
                                          ed.win->cs.curs.data[0].pos).v,
                      1U);
    SAG_ASSERT_EQ_U64(ed.win->cs.curs.data[0].anchor.v, 4U);
    sag_ed_handle_key(&ed, h_key(SAG_KEY_LEFT), 11);
    SAG_ASSERT_EQ_U64(ed.win->cs.curs.data[0].anchor.v, 4U);
    SAG_ASSERT_EQ_U64(ed.win->cs.curs.data[0].pos.v, 8U);
    sag_ed_free(&ed);
}

void test_hmode_modified_motions_keep_collapsed_and_selected_anchors(void)
{
    Ed ed;
    CmdCtx cx = {0};

    h_editor(&ed, "camelCase word\n");
    h_place(&ed, 0U, 0U);
    SAG_ASSERT_EQ_U64(sag_mode_enter(&ed, SAG_MODE_W), SAG_CMD_OK);
    SAG_ASSERT_EQ_U64(sag_mode_enter(&ed, SAG_MODE_H), SAG_CMD_OK);
    cx.ed = &ed;
    cx.win = ed.win;
    SAG_ASSERT_EQ_U64(sag_edit_cmd_move_word_sub_next(&cx), SAG_CMD_OK);
    SAG_ASSERT(ed.win->cs.curs.data[0].pos.v > 0U);
    SAG_ASSERT_EQ_U64(ed.win->cs.curs.data[0].anchor.v, 0U);
    SAG_ASSERT_EQ_U64(sag_edit_cmd_move_word_sub_prev(&cx), SAG_CMD_OK);
    SAG_ASSERT_EQ_U64(ed.win->cs.curs.data[0].anchor.v, 0U);
    sag_ed_free(&ed);

    h_editor(&ed, "abcdef\n");
    h_place(&ed, 2U, 2U);
    SAG_ASSERT_EQ_U64(sag_mode_enter_highlight(&ed, SAG_MODE_I, false),
                      SAG_CMD_OK);
    cx.ed = &ed;
    cx.win = ed.win;
    SAG_ASSERT_EQ_U64(sag_edit_cmd_move_line_end(&cx), SAG_CMD_OK);
    SAG_ASSERT_EQ_U64(ed.win->cs.curs.data[0].pos.v, 6U);
    SAG_ASSERT_EQ_U64(ed.win->cs.curs.data[0].anchor.v, 2U);
    SAG_ASSERT_EQ_U64(sag_edit_cmd_move_line_home(&cx), SAG_CMD_OK);
    SAG_ASSERT_EQ_U64(ed.win->cs.curs.data[0].pos.v, 0U);
    SAG_ASSERT_EQ_U64(ed.win->cs.curs.data[0].anchor.v, 2U);
    sag_ed_free(&ed);

    h_editor(&ed, "(abc)");
    h_place(&ed, 0U, 0U);
    SAG_ASSERT_EQ_U64(sag_mode_enter_highlight(&ed, SAG_MODE_B, false),
                      SAG_CMD_OK);
    cx.ed = &ed;
    cx.win = ed.win;
    SAG_ASSERT_EQ_U64(sag_edit_cmd_move_block_match_next(&cx), SAG_CMD_OK);
    SAG_ASSERT_EQ_U64(ed.win->cs.curs.data[0].pos.v, 4U);
    SAG_ASSERT_EQ_U64(ed.win->cs.curs.data[0].anchor.v, 0U);
    sag_ed_free(&ed);
}

void test_hmode_lifts_lines_matches_and_ends(void)
{
    Ed ed;
    CmdCtx cx = {0};

    h_editor(&ed, "aa\nb\nccc");
    h_place(&ed, 6U, 1U);
    SAG_ASSERT_EQ_U64(sag_mode_enter(&ed, SAG_MODE_H), SAG_CMD_OK);
    cx.ed = &ed;
    cx.win = ed.win;
    SAG_ASSERT_EQ_U64(sag_edit_cmd_cursor_lift_lines(&cx), SAG_CMD_OK);
    SAG_ASSERT_EQ_U64(ed.mode, SAG_MODE_L);
    SAG_ASSERT_EQ_U64(ed.win->cs.curs.len, 3U);
    SAG_ASSERT_EQ_U64(ed.win->cs.curs.data[0].pos.v, 1U);
    SAG_ASSERT_EQ_U64(ed.win->cs.curs.data[1].pos.v, 4U);
    SAG_ASSERT_EQ_U64(ed.win->cs.curs.data[2].pos.v, 6U);
    sag_ed_free(&ed);

    h_editor(&ed, "ab ab ab");
    h_place(&ed, 8U, 0U);
    SAG_ASSERT_EQ_U64(sag_mode_enter(&ed, SAG_MODE_H), SAG_CMD_OK);
    cx.ed = &ed;
    cx.win = ed.win;
    cx.sarg = "ab";
    cx.sarg_len = 2U;
    SAG_ASSERT_EQ_U64(sag_edit_cmd_cursor_lift_matches(&cx), SAG_CMD_OK);
    SAG_ASSERT_EQ_U64(ed.win->cs.curs.len, 3U);
    SAG_ASSERT_EQ_U64(ed.win->cs.curs.data[0].pos.v, 0U);
    SAG_ASSERT_EQ_U64(ed.win->cs.curs.data[1].pos.v, 3U);
    SAG_ASSERT_EQ_U64(ed.win->cs.curs.data[2].pos.v, 6U);
    sag_ed_free(&ed);

    h_editor(&ed, "abcdef");
    h_place(&ed, 4U, 1U);
    SAG_ASSERT_EQ_U64(sag_mode_enter(&ed, SAG_MODE_H), SAG_CMD_OK);
    cx.ed = &ed;
    cx.win = ed.win;
    cx.sarg = NULL;
    cx.sarg_len = 0U;
    SAG_ASSERT_EQ_U64(sag_edit_cmd_cursor_lift_ends(&cx), SAG_CMD_OK);
    SAG_ASSERT_EQ_U64(ed.win->cs.curs.len, 2U);
    SAG_ASSERT_EQ_U64(ed.win->cs.curs.data[0].pos.v, 1U);
    SAG_ASSERT_EQ_U64(ed.win->cs.curs.data[1].pos.v, 4U);
    sag_ed_free(&ed);
}

void test_hmode_lift_ends_preserves_nonzero_primary_active_end(void)
{
    Ed ed;
    CmdCtx cx = {0};
    Cursor lower = {BYTEOFF(3U), {3U}, BYTEOFF(1U)};

    h_editor(&ed, "0123456789");
    h_place(&ed, 8U, 6U);
    SAG_ASSERT_EQ_U64(sag_mode_enter(&ed, SAG_MODE_H), SAG_CMD_OK);
    SAG_ASSERT(sag_cset_add(&ed.win->cs, lower));
    SAG_ASSERT_EQ_U64(ed.win->cs.primary, 1U);
    cx.ed = &ed;
    cx.win = ed.win;
    SAG_ASSERT_EQ_U64(sag_edit_cmd_cursor_lift_ends(&cx), SAG_CMD_OK);
    SAG_ASSERT_EQ_U64(ed.win->cs.curs.len, 4U);
    SAG_ASSERT_EQ_U64(ed.win->cs.primary, 3U);
    SAG_ASSERT_EQ_U64(ed.win->cs.curs.data[ed.win->cs.primary].pos.v, 8U);
    sag_ed_free(&ed);
}

void test_hmode_selection_kind_damages_secondary_selection_rows(void)
{
    Ed ed;
    CmdCtx cx = {0};
    Cursor secondary = {BYTEOFF(14U), {0U}, BYTEOFF(12U)};

    h_editor(&ed, "aa\nbb\ncc\ndd\nee\n");
    ed.win->rect = (Rect){0U, 0U, 40U, 5U};
    ed.win->vp.rows = 5U;
    ed.win->vp.cols = 40U;
    h_place(&ed, 2U, 0U);
    SAG_ASSERT_EQ_U64(sag_mode_enter(&ed, SAG_MODE_H), SAG_CMD_OK);
    SAG_ASSERT(sag_cset_add(&ed.win->cs, secondary));
    ed.full_damage = false;
    ed.doc_damage_lo = ed.win->rect.h;
    ed.doc_damage_hi = 0U;
    cx.ed = &ed;
    cx.win = ed.win;
    cx.sarg = "l";
    cx.sarg_len = 1U;
    SAG_ASSERT_EQ_U64(sag_edit_cmd_sel_kind(&cx), SAG_CMD_OK);
    SAG_ASSERT(!ed.full_damage);
    SAG_ASSERT_EQ_U64(ed.doc_damage_lo, 0U);
    SAG_ASSERT_EQ_U64(ed.doc_damage_hi, 5U);
    sag_ed_free(&ed);
}

void test_hmode_keyboard_entry_is_reachable_from_each_source_mode(void)
{
    Ed ed;
    Key key;

    h_editor(&ed, "one two\n");
    SAG_ASSERT_EQ_U64(sag_mode_enter(&ed, SAG_MODE_W), SAG_CMD_OK);
    sag_ed_handle_key(&ed, h_key((u32)'h'), 10);
    SAG_ASSERT_EQ_U64(ed.mode, SAG_MODE_H);
    SAG_ASSERT_EQ_U64(ed.win->h.from, SAG_MODE_W);

    SAG_ASSERT_EQ_U64(sag_mode_escape(&ed), SAG_CMD_OK);
    SAG_ASSERT_EQ_U64(sag_mode_enter(&ed, SAG_MODE_B), SAG_CMD_OK);
    sag_ed_handle_key(&ed, h_key((u32)'h'), 11);
    SAG_ASSERT_EQ_U64(ed.mode, SAG_MODE_H);
    SAG_ASSERT_EQ_U64(ed.win->h.from, SAG_MODE_B);

    SAG_ASSERT_EQ_U64(sag_mode_escape(&ed), SAG_CMD_OK);
    SAG_ASSERT_EQ_U64(sag_mode_enter(&ed, SAG_MODE_I), SAG_CMD_OK);
    key = h_key((u32)'h');
    key.mods = SAG_MOD_ALT;
    sag_ed_handle_key(&ed, key, 12);
    SAG_ASSERT_EQ_U64(ed.mode, SAG_MODE_H);
    SAG_ASSERT_EQ_U64(ed.win->h.from, SAG_MODE_I);
    SAG_ASSERT_EQ_U64(ed.win->h.unit, &sag_unit_char);
    sag_ed_free(&ed);
}

void test_hmode_replace_captures_exactly_one_text_key_and_escape_cancels(void)
{
    static const u8 replaced[] = "aZc";
    Ed ed;

    h_editor(&ed, "abc");
    h_place(&ed, 2U, 1U);
    SAG_ASSERT_EQ_U64(sag_mode_enter_highlight(&ed, SAG_MODE_I, false),
                      SAG_CMD_OK);
    sag_ed_handle_key(&ed, h_key((u32)'r'), 10);
    SAG_ASSERT(ed.capture_cmd.v != 0U);
    SAG_ASSERT_EQ_U64(ed.dispatch_count, 0U);
    h_assert_text(&ed, (const u8 *)"abc", 3U);
    sag_ed_handle_key(&ed, h_key((u32)'Z'), 11);
    SAG_ASSERT_EQ_U64(ed.capture_cmd.v, 0U);
    SAG_ASSERT_EQ_U64(ed.dispatch_count, 1U);
    SAG_ASSERT_EQ_U64(ed.last_status, SAG_CMD_OK);
    SAG_ASSERT_EQ_U64(ed.mode, SAG_MODE_L);
    h_assert_text(&ed, replaced, sizeof(replaced) - 1U);

    h_place(&ed, 2U, 1U);
    SAG_ASSERT_EQ_U64(sag_mode_enter_highlight(&ed, SAG_MODE_I, false),
                      SAG_CMD_OK);
    sag_ed_handle_key(&ed, h_key((u32)'r'), 12);
    SAG_ASSERT(ed.capture_cmd.v != 0U);
    sag_ed_handle_key(&ed, h_key(SAG_KEY_ESCAPE), 13);
    SAG_ASSERT_EQ_U64(ed.capture_cmd.v, 0U);
    SAG_ASSERT_EQ_U64(ed.mode, SAG_MODE_H);
    SAG_ASSERT_EQ_U64(ed.dispatch_count, 1U);
    h_assert_text(&ed, replaced, sizeof(replaced) - 1U);
    sag_ed_free(&ed);
}

void test_hmode_keyboard_extension_damages_only_old_new_selection_union(void)
{
    static const char text[] =
        "zero\n"
        "one\n"
        "two\n"
        "three\n"
        "four\n"
        "five\n"
        "six\n"
        "seven\n";
    Ed ed;
    u64 dispatched;

    h_editor(&ed, text);
    ed.win->rect = (Rect){0U, 0U, 40U, 8U};
    ed.win->vp.rows = 8U;
    ed.win->vp.cols = 40U;
    h_place(&ed, 9U, 9U); /* line two */
    SAG_ASSERT_EQ_U64(sag_mode_enter(&ed, SAG_MODE_L), SAG_CMD_OK);
    SAG_ASSERT_EQ_U64(sag_mode_enter(&ed, SAG_MODE_H), SAG_CMD_OK);
    ed.full_damage = false;
    ed.doc_damage_lo = ed.win->rect.h;
    ed.doc_damage_hi = 0U;
    dispatched = ed.dispatch_count;

    sag_ed_handle_key(&ed, h_key(SAG_KEY_DOWN), 20);
    SAG_ASSERT_EQ_U64(ed.dispatch_count, dispatched + 1U);
    SAG_ASSERT_EQ_U64(ed.last_status, SAG_CMD_OK);
    SAG_ASSERT(!ed.full_damage);
    SAG_ASSERT_EQ_U64(ed.doc_damage_lo, 2U);
    SAG_ASSERT_EQ_U64(ed.doc_damage_hi, 4U);
    SAG_ASSERT_EQ_U64(ed.doc_damage_hi - ed.doc_damage_lo, 2U);

    sag_ed_handle_key(&ed, h_key(SAG_KEY_DOWN), 21);
    SAG_ASSERT_EQ_U64(ed.dispatch_count, dispatched + 2U);
    SAG_ASSERT_EQ_U64(ed.last_status, SAG_CMD_OK);
    SAG_ASSERT(!ed.full_damage);
    SAG_ASSERT_EQ_U64(ed.doc_damage_lo, 2U);
    SAG_ASSERT_EQ_U64(ed.doc_damage_hi, 5U);
    SAG_ASSERT(ed.doc_damage_hi < ed.win->rect.h);
    sag_ed_free(&ed);
}
