#define _POSIX_C_SOURCE 200809L

#include "harness.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "edit/ed.h"

#define FAMILY                                                              \
    "\xf0\x9f\x91\xa8\xe2\x80\x8d\xf0\x9f\x91\xa9"                \
    "\xe2\x80\x8d\xf0\x9f\x91\xa7\xe2\x80\x8d"                    \
    "\xf0\x9f\x91\xa6"

static const u8 mixed_fixture[] =
    "  alpha  \r\n"
    "e\xcc\x81 \xe6\xbc\xa2 " FAMILY " X\r\n"
    "line03\r\n"
    "line04\r\n"
    "line05\r\n"
    "line06\r\n"
    "line07\r\n"
    "line08\r\n"
    "line09\r\n"
    "line10\r\n"
    "line11\r\n"
    "line12\r\n"
    "line13\r\n"
    "bad:" "\xff" ":byte";

static Key edit_key(u32 code)
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

static Key edit_text_key(const u8 *bytes, u8 len)
{
    Key key = {0};

    YEW_ASSERT(len <= sizeof(key.text));
    key.code = len == 1U ? bytes[0] : 0U;
    key.kind = YEW_EV_KEY;
    key.ev = YEW_KEY_PRESS;
    key.ntext = len;
    (void)memcpy(key.text, bytes, len);
    return key;
}

static void edit_fixture(Ed *ed, const u8 *bytes, size_t len, YewEol eol)
{
    Cursor *cursor;

    yew_ed_init(ed);
    YEW_ASSERT(yew_ed_open_scratch(ed));
    yew_test_load_runtime(ed);
    yew_undo_free(ed->buffer.undo);
    yew_textbuf_free(ed->buffer.tb);
    ed->buffer.tb = yew_textbuf_from_bytes(bytes, len);
    ed->buffer.undo = yew_undo_new(ed->buffer.tb);
    ed->buffer.meta.eol = eol;
    ed->buffer.meta.dominant_eol = eol;
    ed->buffer.meta.crlf_count = eol == YEW_EOL_CRLF ? 1U : 0U;
    ed->buffer.meta.lf_count = eol == YEW_EOL_LF ? 1U : 0U;
    ed->win->vp.rows = 4U;
    ed->win->vp.cols = 80U;
    ed->win->rect.h = 4U;
    ed->win->rect.w = 80U;
    cursor = yew_ed_cursor(ed);
    YEW_ASSERT_NOT_NULL(cursor);
    cursor->pos = BYTEOFF(0U);
    cursor->anchor = BYTEOFF(0U);
    cursor->goal_col = (GCol){0U};
    YEW_ASSERT_EQ_U64(yew_undo_current(ed->buffer.undo),
                      ed->buffer.undo->root);
}

static CmdStatus edit_invoke(Ed *ed, const char *name, u32 count,
                             bool count_given, const char *arg, u32 arg_len)
{
    CmdId id = yew_cmd_lookup(name, (u32)strlen(name));
    CmdCtx cx = {0};

    YEW_ASSERT(id.v != 0U);
    cx.ed = ed;
    cx.win = ed->win;
    cx.count = count;
    cx.count_given = count_given;
    cx.sarg = arg;
    cx.sarg_len = arg_len;
    cx.source = YEW_SRC_TEST;
    return yew_ed_invoke(ed, id, &cx);
}

static Bytebuf edit_materialize(const TextBuf *tb)
{
    Bytebuf out;
    TextIter iter;

    bytebuf_init(&out);
    if (yew_textiter_begin(&iter, tb, BYTEOFF(0U))) {
        do {
            const u8 *chunk;
            u64 len;

            YEW_ASSERT(yew_textiter_chunk(&iter, tb, &chunk, &len));
            bytebuf_append(&out, chunk, (size_t)len);
        } while (yew_textiter_advance(&iter, tb));
    }
    return out;
}

static void edit_assert_text(const Ed *ed, const u8 *want, size_t want_len)
{
    Bytebuf got = edit_materialize(ed->buffer.tb);

    YEW_ASSERT_EQ_U64(yew_textbuf_len(ed->buffer.tb), want_len);
    YEW_ASSERT_EQ_U64(got.len, want_len);
    YEW_ASSERT_EQ_MEM(got.data, want, want_len);
    bytebuf_free(&got);
}

static void edit_place(Ed *ed, u64 off)
{
    Cursor *cursor = yew_ed_cursor(ed);
    Span line;

    YEW_ASSERT_NOT_NULL(cursor);
    cursor->pos = BYTEOFF(off);
    cursor->anchor = cursor->pos;
    line = yew_textbuf_line_span(ed->buffer.tb,
                                 yew_textbuf_line_of(ed->buffer.tb,
                                                    cursor->pos));
    cursor->goal_col = yew_off_to_gcol(ed->buffer.tb, line, cursor->pos);
}

void test_edit_l_motion_table_handles_unicode_crlf_and_viewport_counts(void)
{
    Ed ed;
    Cursor *cursor;
    Span line;

    edit_fixture(&ed, mixed_fixture, sizeof(mixed_fixture) - 1U,
                 YEW_EOL_CRLF);
    cursor = yew_ed_cursor(&ed);
    edit_place(&ed, 5U);
    YEW_ASSERT_EQ_U64(edit_invoke(&ed, "ed.move.line.home", 1U, false,
                                  NULL, 0U), YEW_CMD_OK);
    YEW_ASSERT_EQ_U64(cursor->pos.v, 0U);
    YEW_ASSERT_EQ_U64(edit_invoke(&ed, "ed.move.line.end", 1U, false,
                                  NULL, 0U), YEW_CMD_OK);
    YEW_ASSERT_EQ_U64(cursor->pos.v, 9U);
    YEW_ASSERT_EQ_U64(edit_invoke(&ed, "ed.move.line.first_nonblank", 1U,
                                  false, NULL, 0U), YEW_CMD_OK);
    YEW_ASSERT_EQ_U64(cursor->pos.v, 2U);
    YEW_ASSERT_EQ_U64(edit_invoke(&ed, "ed.move.line.last_nonblank", 1U,
                                  false, NULL, 0U), YEW_CMD_OK);
    YEW_ASSERT_EQ_U64(cursor->pos.v, 6U);

    YEW_ASSERT_EQ_U64(edit_invoke(&ed, "ed.move.line.down", 1U, false,
                                  NULL, 0U), YEW_CMD_OK);
    YEW_ASSERT_EQ_U64(yew_textbuf_line_of(ed.buffer.tb, cursor->pos).v, 1U);
    YEW_ASSERT_EQ_U64(edit_invoke(&ed, "ed.move.line.up", 1U, false,
                                  NULL, 0U), YEW_CMD_OK);
    YEW_ASSERT_EQ_U64(yew_textbuf_line_of(ed.buffer.tb, cursor->pos).v, 0U);
    YEW_ASSERT_EQ_U64(edit_invoke(&ed, "ed.move.line.half_page_down", 1U,
                                  false, NULL, 0U), YEW_CMD_OK);
    YEW_ASSERT_EQ_U64(yew_textbuf_line_of(ed.buffer.tb, cursor->pos).v, 2U);
    YEW_ASSERT_EQ_U64(edit_invoke(&ed, "ed.view.page_down", 1U, false,
                                  NULL, 0U), YEW_CMD_OK);
    YEW_ASSERT_EQ_U64(yew_textbuf_line_of(ed.buffer.tb, cursor->pos).v, 4U);
    YEW_ASSERT_EQ_U64(edit_invoke(&ed, "ed.view.page_up", 1U, false,
                                  NULL, 0U), YEW_CMD_OK);
    YEW_ASSERT_EQ_U64(yew_textbuf_line_of(ed.buffer.tb, cursor->pos).v, 2U);
    YEW_ASSERT_EQ_U64(edit_invoke(&ed, "ed.move.line.half_page_up", 1U,
                                  false, NULL, 0U), YEW_CMD_OK);
    YEW_ASSERT_EQ_U64(yew_textbuf_line_of(ed.buffer.tb, cursor->pos).v, 0U);

    YEW_ASSERT_EQ_U64(edit_invoke(&ed, "ed.move.buf.end", 12U, true,
                                  NULL, 0U), YEW_CMD_OK);
    YEW_ASSERT_EQ_U64(yew_textbuf_line_of(ed.buffer.tb, cursor->pos).v, 11U);
    line = yew_textbuf_line_span(ed.buffer.tb, LINENO(11U));
    YEW_ASSERT_EQ_U64(cursor->pos.v, line.lo);
    YEW_ASSERT_EQ_U64(edit_invoke(&ed, "ed.move.buf.home", 1U, false,
                                  NULL, 0U), YEW_CMD_OK);
    YEW_ASSERT_EQ_U64(cursor->pos.v, 0U);
    yew_ed_free(&ed);
}

void test_edit_12G_dispatch_lands_on_one_based_line_twelve(void)
{
    Ed ed;
    Cursor *cursor;

    edit_fixture(&ed, mixed_fixture, sizeof(mixed_fixture) - 1U,
                 YEW_EOL_CRLF);
    yew_ed_handle_key(&ed, edit_key((u32)'1'), 0);
    yew_ed_handle_key(&ed, edit_key((u32)'2'), 0);
    YEW_ASSERT(ed.chord.count_given);
    YEW_ASSERT_EQ_U64(ed.chord.count, 12U);
    yew_ed_handle_key(&ed, edit_key((u32)'G'), 0);
    cursor = yew_ed_cursor(&ed);
    YEW_ASSERT_EQ_U64(ed.last_status, YEW_CMD_OK);
    YEW_ASSERT_EQ_U64(yew_textbuf_line_of(ed.buffer.tb, cursor->pos).v, 11U);
    YEW_ASSERT_EQ_U64(cursor->pos.v,
                      yew_textbuf_line_start(ed.buffer.tb, LINENO(11U)).v);
    YEW_ASSERT(!ed.chord.count_given);
    YEW_ASSERT_EQ_U64(ed.dispatch_count, 1U);
    yew_ed_free(&ed);
}

void test_edit_delete_zwj_grapheme_removes_exactly_twenty_five_bytes(void)
{
    static const u8 before[] = FAMILY "X";
    static const u8 after[] = "X";
    Ed ed;

    edit_fixture(&ed, before, sizeof(before) - 1U, YEW_EOL_LF);
    YEW_ASSERT_EQ_U64(sizeof(before) - sizeof(after), 25U);
    YEW_ASSERT_EQ_U64(edit_invoke(&ed, "ed.edit.delete.grapheme", 1U,
                                  false, NULL, 0U), YEW_CMD_OK);
    edit_assert_text(&ed, after, sizeof(after) - 1U);
    YEW_ASSERT_EQ_U64(yew_ed_cursor(&ed)->pos.v, 0U);
    YEW_ASSERT_EQ_U64(yew_undo_current(ed.buffer.undo),
                      ed.buffer.undo->root + 1U);
    yew_ed_free(&ed);
}

void test_edit_4x_is_one_undo_step_for_four_clusters(void)
{
    static const u8 before[] = "a" FAMILY "\xe6\xbc\xa2" "bc";
    static const u8 after[] = "c";
    Ed ed;

    edit_fixture(&ed, before, sizeof(before) - 1U, YEW_EOL_LF);
    yew_ed_handle_key(&ed, edit_key((u32)'4'), 0);
    yew_ed_handle_key(&ed, edit_key((u32)'x'), 0);
    YEW_ASSERT_EQ_U64(ed.last_status, YEW_CMD_OK);
    YEW_ASSERT_EQ_U64(ed.dispatch_count, 1U);
    YEW_ASSERT_EQ_U64(yew_undo_current(ed.buffer.undo),
                      ed.buffer.undo->root + 1U);
    edit_assert_text(&ed, after, sizeof(after) - 1U);
    YEW_ASSERT_EQ_U64(edit_invoke(&ed, "ed.edit.undo", 1U, false,
                                  NULL, 0U), YEW_CMD_OK);
    YEW_ASSERT_EQ_U64(yew_undo_current(ed.buffer.undo),
                      ed.buffer.undo->root);
    edit_assert_text(&ed, before, sizeof(before) - 1U);
    yew_ed_free(&ed);
}

void test_edit_insert_newline_uses_crlf_bytes(void)
{
    static const u8 before[] = "ab\r\ncd";
    static const u8 after[] = "ab\r\n\r\ncd";
    Ed ed;

    edit_fixture(&ed, before, sizeof(before) - 1U, YEW_EOL_CRLF);
    edit_place(&ed, 4U);
    YEW_ASSERT_EQ_U64(yew_mode_enter(&ed, YEW_MODE_I), YEW_CMD_OK);
    yew_ed_handle_key(&ed, edit_key(YEW_KEY_ENTER), 0);
    YEW_ASSERT_EQ_U64(ed.last_status, YEW_CMD_OK);
    edit_assert_text(&ed, after, sizeof(after) - 1U);
    YEW_ASSERT_EQ_U64(yew_ed_cursor(&ed)->pos.v, 6U);
    YEW_ASSERT_EQ_U64(yew_undo_current(ed.buffer.undo),
                      ed.buffer.undo->root + 1U);
    YEW_ASSERT(!ed.insert_txn);
    YEW_ASSERT_EQ_U64(ed.doc_damage_lo, 1U);
    YEW_ASSERT_EQ_U64(ed.doc_damage_hi, 4U);
    yew_ed_free(&ed);
}

void test_edit_backspace_at_line_start_joins_crlf_lines(void)
{
    static const u8 before[] = "ab\r\ncd";
    static const u8 after[] = "abcd";
    Ed ed;

    edit_fixture(&ed, before, sizeof(before) - 1U, YEW_EOL_CRLF);
    edit_place(&ed, 4U);
    YEW_ASSERT_EQ_U64(yew_mode_enter(&ed, YEW_MODE_I), YEW_CMD_OK);
    yew_ed_handle_key(&ed, edit_key(YEW_KEY_BACKSPACE), 0);
    YEW_ASSERT_EQ_U64(ed.last_status, YEW_CMD_OK);
    edit_assert_text(&ed, after, sizeof(after) - 1U);
    YEW_ASSERT_EQ_U64(yew_ed_cursor(&ed)->pos.v, 2U);
    YEW_ASSERT(ed.insert_txn);
    yew_ed_handle_key(&ed, edit_key(YEW_KEY_ESCAPE), 0);
    YEW_ASSERT(!ed.insert_txn);
    YEW_ASSERT_EQ_U64(yew_undo_current(ed.buffer.undo),
                      ed.buffer.undo->root + 1U);
    yew_ed_free(&ed);
}

void test_edit_insert_text_and_tab_share_one_insert_transaction(void)
{
    static const u8 acute[] = {0xc3U, 0xa9U};
    static const u8 want[] = {0xc3U, 0xa9U, (u8)'\t'};
    Ed ed;

    edit_fixture(&ed, NULL, 0U, YEW_EOL_LF);
    YEW_ASSERT_EQ_U64(yew_mode_enter(&ed, YEW_MODE_I), YEW_CMD_OK);
    yew_ed_handle_key(&ed, edit_text_key(acute, sizeof(acute)), 0);
    YEW_ASSERT_EQ_U64(ed.last_status, YEW_CMD_OK);
    YEW_ASSERT(ed.insert_txn);
    YEW_ASSERT_EQ_U64(ed.doc_damage_lo, 0U);
    YEW_ASSERT_EQ_U64(ed.doc_damage_hi, 1U);
    yew_ed_handle_key(&ed, edit_key(YEW_KEY_TAB), 0);
    YEW_ASSERT_EQ_U64(ed.last_status, YEW_CMD_OK);
    YEW_ASSERT(ed.insert_txn);
    yew_ed_handle_key(&ed, edit_key(YEW_KEY_ESCAPE), 0);
    edit_assert_text(&ed, want, sizeof(want));
    YEW_ASSERT_EQ_U64(yew_undo_current(ed.buffer.undo),
                      ed.buffer.undo->root + 1U);
    YEW_ASSERT(!ed.insert_txn);
    YEW_ASSERT_EQ_U64(edit_invoke(&ed, "ed.edit.undo", 1U, false,
                                  NULL, 0U), YEW_CMD_OK);
    edit_assert_text(&ed, NULL, 0U);
    yew_ed_free(&ed);
}

void test_edit_insert_motion_splits_typing_into_two_undo_steps(void)
{
    static const u8 typed[] = "hello";
    static const u8 after_motion[] = "helol";
    static const u8 hel[] = "hel";
    static const u8 empty[] = "";
    Ed ed;
    size_t i;

    edit_fixture(&ed, NULL, 0U, YEW_EOL_LF);
    YEW_ASSERT_EQ_U64(yew_mode_enter(&ed, YEW_MODE_I), YEW_CMD_OK);
    for (i = 0U; i < 3U; i++)
        yew_ed_handle_key(&ed, edit_key((u32)typed[i]), 0);
    YEW_ASSERT(ed.insert_txn);
    yew_ed_handle_key(&ed, edit_key(YEW_KEY_LEFT), 0);
    YEW_ASSERT(!ed.insert_txn);
    for (i = 3U; i < sizeof(typed) - 1U; i++)
        yew_ed_handle_key(&ed, edit_key((u32)typed[i]), 0);
    yew_ed_handle_key(&ed, edit_key(YEW_KEY_ESCAPE), 0);
    edit_assert_text(&ed, after_motion, sizeof(after_motion) - 1U);
    YEW_ASSERT_EQ_U64(yew_undo_current(ed.buffer.undo),
                      ed.buffer.undo->root + 2U);
    YEW_ASSERT_EQ_U64(edit_invoke(&ed, "ed.edit.undo", 1U, false,
                                  NULL, 0U), YEW_CMD_OK);
    edit_assert_text(&ed, hel, sizeof(hel) - 1U);
    YEW_ASSERT_EQ_U64(edit_invoke(&ed, "ed.edit.undo", 1U, false,
                                  NULL, 0U), YEW_CMD_OK);
    edit_assert_text(&ed, empty, 0U);
    yew_ed_free(&ed);
}

void test_edit_escape_closes_typing_as_one_undo_step(void)
{
    static const u8 hello[] = "hello";
    Ed ed;
    size_t i;

    edit_fixture(&ed, NULL, 0U, YEW_EOL_LF);
    YEW_ASSERT_EQ_U64(yew_mode_enter(&ed, YEW_MODE_I), YEW_CMD_OK);
    for (i = 0U; i < sizeof(hello) - 1U; i++)
        yew_ed_handle_key(&ed, edit_key((u32)hello[i]), 0);
    YEW_ASSERT(ed.insert_txn);
    YEW_ASSERT_EQ_U64(yew_undo_current(ed.buffer.undo),
                      ed.buffer.undo->root + 1U);
    YEW_ASSERT_EQ_U64(ed.buffer.undo->depth, 1U);
    yew_ed_handle_key(&ed, edit_key(YEW_KEY_ESCAPE), 0);
    YEW_ASSERT_EQ_U64(ed.mode, YEW_MODE_L);
    YEW_ASSERT(!ed.insert_txn);
    YEW_ASSERT_EQ_U64(yew_undo_current(ed.buffer.undo),
                      ed.buffer.undo->root + 1U);
    edit_assert_text(&ed, hello, sizeof(hello) - 1U);
    YEW_ASSERT_EQ_U64(edit_invoke(&ed, "ed.edit.undo", 1U, false,
                                  NULL, 0U), YEW_CMD_OK);
    edit_assert_text(&ed, NULL, 0U);
    yew_ed_free(&ed);
}

void test_edit_open_above_and_below_emit_native_eol_and_enter_insert(void)
{
    static const u8 before[] = "aa\r\nbb";
    static const u8 above[] = "aa\r\n\r\nbb";
    static const u8 both[] = "aa\r\n\r\n\r\nbb";
    Ed ed;

    edit_fixture(&ed, before, sizeof(before) - 1U, YEW_EOL_CRLF);
    edit_place(&ed, 4U);
    YEW_ASSERT_EQ_U64(edit_invoke(&ed, "ed.edit.line.open_above", 1U,
                                  false, NULL, 0U), YEW_CMD_OK);
    YEW_ASSERT_EQ_U64(ed.mode, YEW_MODE_I);
    YEW_ASSERT_EQ_U64(yew_ed_cursor(&ed)->pos.v, 4U);
    edit_assert_text(&ed, above, sizeof(above) - 1U);
    YEW_ASSERT_EQ_U64(yew_mode_escape(&ed), YEW_CMD_OK);
    YEW_ASSERT_EQ_U64(edit_invoke(&ed, "ed.edit.line.open_below", 1U,
                                  false, NULL, 0U), YEW_CMD_OK);
    YEW_ASSERT_EQ_U64(ed.mode, YEW_MODE_I);
    YEW_ASSERT_EQ_U64(yew_ed_cursor(&ed)->pos.v, 6U);
    edit_assert_text(&ed, both, sizeof(both) - 1U);
    yew_ed_free(&ed);
}

void test_edit_line_delete_undo_and_redo_preserve_crlf_bytes(void)
{
    static const u8 before[] = "aa\r\nbb\r\ncc";
    static const u8 after[] = "aa\r\ncc";
    Ed ed;

    edit_fixture(&ed, before, sizeof(before) - 1U, YEW_EOL_CRLF);
    edit_place(&ed, 4U);
    yew_ed_handle_key(&ed, edit_key((u32)'d'), 0);
    YEW_ASSERT_EQ_U64(ed.chord.n, 1U);
    yew_ed_handle_key(&ed, edit_key((u32)'d'), 0);
    YEW_ASSERT_EQ_U64(ed.last_status, YEW_CMD_OK);
    edit_assert_text(&ed, after, sizeof(after) - 1U);
    YEW_ASSERT_EQ_U64(yew_undo_current(ed.buffer.undo),
                      ed.buffer.undo->root + 1U);
    yew_ed_handle_key(&ed, edit_key((u32)'u'), 0);
    edit_assert_text(&ed, before, sizeof(before) - 1U);
    YEW_ASSERT_EQ_U64(yew_undo_current(ed.buffer.undo),
                      ed.buffer.undo->root);
    YEW_ASSERT_EQ_U64(edit_invoke(&ed, "ed.edit.redo", 1U, false,
                                  NULL, 0U), YEW_CMD_OK);
    edit_assert_text(&ed, after, sizeof(after) - 1U);
    yew_ed_free(&ed);
}

void test_edit_insert_after_and_i_arrows_use_grapheme_boundaries(void)
{
    static const u8 before[] = "A" FAMILY "\xe6\xbc\xa2\r\nZ";
    Ed ed;
    Cursor *cursor;

    edit_fixture(&ed, before, sizeof(before) - 1U, YEW_EOL_CRLF);
    cursor = yew_ed_cursor(&ed);
    YEW_ASSERT_EQ_U64(edit_invoke(&ed, "ed.edit.insert.after", 1U, false,
                                  NULL, 0U), YEW_CMD_OK);
    YEW_ASSERT_EQ_U64(ed.mode, YEW_MODE_I);
    YEW_ASSERT_EQ_U64(cursor->pos.v, 1U);
    yew_ed_handle_key(&ed, edit_key(YEW_KEY_RIGHT), 0);
    YEW_ASSERT_EQ_U64(cursor->pos.v, 26U);
    yew_ed_handle_key(&ed, edit_key(YEW_KEY_RIGHT), 0);
    YEW_ASSERT_EQ_U64(cursor->pos.v, 29U);
    yew_ed_handle_key(&ed, edit_key(YEW_KEY_LEFT), 0);
    YEW_ASSERT_EQ_U64(cursor->pos.v, 26U);
    yew_ed_handle_key(&ed, edit_key(YEW_KEY_END), 0);
    YEW_ASSERT_EQ_U64(cursor->pos.v, 29U);
    yew_ed_handle_key(&ed, edit_key(YEW_KEY_DOWN), 0);
    YEW_ASSERT_EQ_U64(yew_textbuf_line_of(ed.buffer.tb, cursor->pos).v, 1U);
    yew_ed_handle_key(&ed, edit_key(YEW_KEY_HOME), 0);
    YEW_ASSERT_EQ_U64(cursor->pos.v,
                      yew_textbuf_line_start(ed.buffer.tb, LINENO(1U)).v);
    yew_ed_handle_key(&ed, edit_key(YEW_KEY_UP), 0);
    YEW_ASSERT_EQ_U64(yew_textbuf_line_of(ed.buffer.tb, cursor->pos).v, 0U);
    yew_ed_free(&ed);
}

void test_edit_forward_delete_removes_invalid_byte_as_one_cluster(void)
{
    static const u8 before[] = {(u8)'a', 0xffU, (u8)'b'};
    static const u8 after[] = {(u8)'a', (u8)'b'};
    Ed ed;

    edit_fixture(&ed, before, sizeof(before), YEW_EOL_LF);
    edit_place(&ed, 1U);
    YEW_ASSERT_EQ_U64(yew_mode_enter(&ed, YEW_MODE_I), YEW_CMD_OK);
    yew_ed_handle_key(&ed, edit_key(YEW_KEY_DELETE), 0);
    YEW_ASSERT_EQ_U64(ed.last_status, YEW_CMD_OK);
    edit_assert_text(&ed, after, sizeof(after));
    YEW_ASSERT_EQ_U64(yew_ed_cursor(&ed)->pos.v, 1U);
    yew_ed_free(&ed);
}

void test_edit_journal_open_failure_returns_io_without_mutation(void)
{
    static const u8 before[] = "abc";
    static const u8 after[] = "Xabc";
    char root[] = "/tmp/yew-edit-cmd-journal-XXXXXX";
    char blocker[128];
    char journal_dir[128];
    char yew_dir[128];
    char source_path[128];
    const char source[] = "/tmp/yew-edit-cmd-source";
    const char *saved_state;
    char *saved_copy = NULL;
    FILE *fp;
    Ed ed;
    size_t source_len = sizeof(source);
    int n;

    saved_state = getenv("XDG_STATE_HOME");
    if (saved_state != NULL) {
        size_t saved_len = strlen(saved_state) + 1U;

        saved_copy = yew_xmalloc(saved_len);
        (void)memcpy(saved_copy, saved_state, saved_len);
    }
    YEW_ASSERT_NOT_NULL(mkdtemp(root));
    n = snprintf(blocker, sizeof(blocker), "%s/blocker", root);
    YEW_ASSERT(n > 0 && (size_t)n < sizeof(blocker));
    fp = fopen(blocker, "wb");
    YEW_ASSERT_NOT_NULL(fp);
    YEW_ASSERT_EQ_U64(fwrite("x", 1U, 1U, fp), 1U);
    YEW_ASSERT_EQ_I64(fclose(fp), 0);
    YEW_ASSERT_EQ_I64(setenv("XDG_STATE_HOME", blocker, 1), 0);

    edit_fixture(&ed, before, sizeof(before) - 1U, YEW_EOL_LF);
    ed.buffer.path = arena_strdup(&ed.arena, source);
    ed.buffer.meta.realpath = yew_xmalloc(source_len);
    (void)memcpy(ed.buffer.meta.realpath, source, source_len);
    YEW_ASSERT_EQ_U64(edit_invoke(&ed, "ed.edit.insert.text", 1U, false,
                                  "X", 1U), YEW_CMD_ERR_IO);
    edit_assert_text(&ed, before, sizeof(before) - 1U);
    YEW_ASSERT_EQ_U64(yew_undo_current(ed.buffer.undo),
                      ed.buffer.undo->root);
    YEW_ASSERT_EQ_U64(ed.buffer.undo->nodes.len, 1U);
    YEW_ASSERT_EQ_U64(ed.buffer.undo->ops.len, 0U);
    YEW_ASSERT_NULL(ed.buffer.jrn);
    YEW_ASSERT_EQ_U64(yew_ed_cursor(&ed)->pos.v, 0U);
    YEW_ASSERT(ed.durability_failed);
    YEW_ASSERT(ed.msg.active);
    YEW_ASSERT_EQ_U64(ed.msg.sev, YEW_MSG_ERROR);
    YEW_ASSERT(strstr(ed.msg.text, "crash journal failed") != NULL);
    YEW_ASSERT_EQ_U64(edit_invoke(&ed, "ed.edit.insert.text", 1U, false,
                                  "Y", 1U), YEW_CMD_ERR_IO);
    edit_assert_text(&ed, before, sizeof(before) - 1U);
    YEW_ASSERT_EQ_U64(yew_ed_request_quit(&ed, false), YEW_CMD_ERR_IO);
    YEW_ASSERT(!ed.quit);
    YEW_ASSERT_EQ_U64(yew_ed_request_quit(&ed, true), YEW_CMD_OK);
    YEW_ASSERT(ed.quit);
    yew_ed_free(&ed);

    n = snprintf(source_path, sizeof(source_path), "%s/source.txt", root);
    YEW_ASSERT(n > 0 && (size_t)n < sizeof(source_path));
    fp = fopen(source_path, "wb");
    YEW_ASSERT_NOT_NULL(fp);
    YEW_ASSERT_EQ_U64(fwrite(before, 1U, sizeof(before) - 1U, fp),
                      sizeof(before) - 1U);
    YEW_ASSERT_EQ_I64(fclose(fp), 0);
    YEW_ASSERT_EQ_I64(setenv("XDG_STATE_HOME", root, 1), 0);
    yew_ed_init(&ed);
    YEW_ASSERT_EQ_U64(yew_ed_open(&ed, source_path), YEW_LOAD_OK);
    YEW_ASSERT_EQ_U64(edit_invoke(&ed, "ed.edit.insert.text", 1U, false,
                                  "X", 1U), YEW_CMD_OK);
    edit_assert_text(&ed, after, sizeof(after) - 1U);
    YEW_ASSERT_EQ_U64(yew_ed_file_save(&ed, false), YEW_CMD_OK);
    YEW_ASSERT_NULL(ed.buffer.jrn);
    YEW_ASSERT_EQ_I64(setenv("XDG_STATE_HOME", blocker, 1), 0);
    YEW_ASSERT_EQ_U64(edit_invoke(&ed, "ed.edit.undo", 1U, false,
                                  NULL, 0U), YEW_CMD_ERR_IO);
    edit_assert_text(&ed, after, sizeof(after) - 1U);
    YEW_ASSERT(ed.durability_failed);
    YEW_ASSERT_EQ_U64(yew_ed_request_quit(&ed, false), YEW_CMD_ERR_IO);
    YEW_ASSERT(!ed.quit);
    yew_ed_free(&ed);

    if (saved_copy != NULL) {
        YEW_ASSERT_EQ_I64(setenv("XDG_STATE_HOME", saved_copy, 1), 0);
    } else {
        YEW_ASSERT_EQ_I64(unsetenv("XDG_STATE_HOME"), 0);
    }
    free(saved_copy);
    n = snprintf(journal_dir, sizeof(journal_dir), "%s/yew/journal",
                 root);
    YEW_ASSERT(n > 0 && (size_t)n < sizeof(journal_dir));
    n = snprintf(yew_dir, sizeof(yew_dir), "%s/yew", root);
    YEW_ASSERT(n > 0 && (size_t)n < sizeof(yew_dir));
    YEW_ASSERT_EQ_I64(unlink(source_path), 0);
    YEW_ASSERT_EQ_I64(unlink(blocker), 0);
    YEW_ASSERT_EQ_I64(rmdir(journal_dir), 0);
    YEW_ASSERT_EQ_I64(rmdir(yew_dir), 0);
    YEW_ASSERT_EQ_I64(rmdir(root), 0);
}

void test_edit_unit_selection_replays_and_invalidates(void)
{
    static const u8 text[] = "one\n\n  two\n    three\n";
    Ed ed;
    Cursor *cursor;
    Span level0;
    Span level1;

    edit_fixture(&ed, text, sizeof(text) - 1U, YEW_EOL_LF);
    edit_place(&ed, 16U);
    cursor = yew_ed_cursor(&ed);
    YEW_ASSERT_EQ_U64(ed.mode, YEW_MODE_L);

    ed.full_damage = false;
    YEW_ASSERT_EQ_U64(edit_invoke(&ed, "ed.sel.unit.expand", 1U, false,
                                  NULL, 0U), YEW_CMD_OK);
    YEW_ASSERT(ed.full_damage);
    YEW_ASSERT_EQ_U64(ed.win->cs.selstacks.data[0].n, 1U);
    level0 = ed.win->cs.selstacks.data[0].s[0];
    YEW_ASSERT_EQ_U64(cursor->anchor.v, level0.lo);
    YEW_ASSERT_EQ_U64(cursor->pos.v, level0.hi);
    YEW_ASSERT_EQ_U64(edit_invoke(&ed, "ed.sel.unit.expand", 1U, false,
                                  NULL, 0U), YEW_CMD_OK);
    YEW_ASSERT_EQ_U64(ed.win->cs.selstacks.data[0].n, 2U);
    level1 = ed.win->cs.selstacks.data[0].s[1];
    YEW_ASSERT(level1.lo <= level0.lo && level1.hi >= level0.hi);
    YEW_ASSERT_EQ_U64(edit_invoke(&ed, "ed.sel.unit.expand", 1U, false,
                                  NULL, 0U), YEW_CMD_OK);
    YEW_ASSERT(ed.win->cs.selstacks.data[0].n >= 3U);

    ed.full_damage = false;
    YEW_ASSERT_EQ_U64(edit_invoke(&ed, "ed.sel.unit.contract", 1U, false,
                                  NULL, 0U), YEW_CMD_OK);
    YEW_ASSERT(ed.full_damage);
    YEW_ASSERT_EQ_U64(cursor->anchor.v,
                      ed.win->cs.selstacks.data[0]
                          .s[ed.win->cs.selstacks.data[0].n - 1U].lo);
    YEW_ASSERT_EQ_U64(cursor->pos.v,
                      ed.win->cs.selstacks.data[0]
                          .s[ed.win->cs.selstacks.data[0].n - 1U].hi);
    YEW_ASSERT_EQ_U64(edit_invoke(&ed, "ed.sel.unit.contract", 1U, false,
                                  NULL, 0U), YEW_CMD_OK);
    YEW_ASSERT_EQ_U64(ed.win->cs.selstacks.data[0].n, 1U);
    YEW_ASSERT_EQ_U64(cursor->anchor.v, level0.lo);
    YEW_ASSERT_EQ_U64(cursor->pos.v, level0.hi);

    YEW_ASSERT_EQ_U64(edit_invoke(&ed, "ed.move.line.home", 1U, false,
                                  NULL, 0U), YEW_CMD_OK);
    YEW_ASSERT_EQ_U64(ed.win->cs.selstacks.data[0].n, 0U);
    edit_place(&ed, 16U);
    YEW_ASSERT_EQ_U64(edit_invoke(&ed, "ed.sel.unit.expand", 1U, false,
                                  NULL, 0U), YEW_CMD_OK);
    YEW_ASSERT_EQ_U64(ed.win->cs.selstacks.data[0].n, 1U);
    YEW_ASSERT_EQ_U64(edit_invoke(&ed, "ed.edit.insert.text", 1U, false,
                                  "X", 1U), YEW_CMD_OK);
    YEW_ASSERT_EQ_U64(ed.win->cs.selstacks.data[0].n, 0U);
    yew_ed_free(&ed);
}

void test_edit_word_and_block_key_layers_dispatch(void)
{
    static const u8 line_text[] = "abcdef\nxy";
    static const u8 word_text[] =
        "foo \xe6\xbc\xa2\xe5\xad\x97 tail\n";
    static const u8 block_text[] = "one\n\n  two\n    three\n";
    Ed ed;
    Key key;

    edit_fixture(&ed, line_text, sizeof(line_text) - 1U, YEW_EOL_LF);
    ed.win->vp.cols = 3U;
    ed.win->vp.wrap = true;
    edit_place(&ed, 1U);
    yew_ed_handle_key(&ed, edit_key(YEW_KEY_DOWN), 0);
    YEW_ASSERT_EQ_U64(ed.last_cmd.v,
                      yew_cmd_lookup("ed.move.unit.next", 17U).v);
    YEW_ASSERT_EQ_U64(yew_ed_cursor(&ed)->pos.v, 4U);
    key = edit_key(YEW_KEY_DOWN);
    key.mods = YEW_MOD_ALT;
    yew_ed_handle_key(&ed, key, 1);
    YEW_ASSERT_EQ_U64(ed.last_cmd.v,
                      yew_cmd_lookup("ed.shadow.accept_line", 21U).v);
    YEW_ASSERT_EQ_U64(yew_ed_cursor(&ed)->pos.v, 4U);
    yew_ed_handle_key(&ed, edit_key(YEW_KEY_LEFT), 2);
    YEW_ASSERT_EQ_U64(ed.last_cmd.v,
                      yew_cmd_lookup("ed.move.unit.home", 17U).v);
    YEW_ASSERT_EQ_U64(yew_ed_cursor(&ed)->pos.v, 0U);
    yew_ed_free(&ed);

    edit_fixture(&ed, word_text, sizeof(word_text) - 1U, YEW_EOL_LF);
    yew_ed_handle_key(&ed, edit_key((u32)'w'), 0);
    YEW_ASSERT_EQ_U64(ed.mode, YEW_MODE_W);
    yew_ed_handle_key(&ed, edit_key(YEW_KEY_RIGHT), 1);
    YEW_ASSERT_EQ_U64(yew_ed_cursor(&ed)->pos.v, 4U);
    yew_ed_free(&ed);

    edit_fixture(&ed, block_text, sizeof(block_text) - 1U, YEW_EOL_LF);
    edit_place(&ed, 16U);
    yew_ed_handle_key(&ed, edit_key((u32)'b'), 0);
    YEW_ASSERT_EQ_U64(ed.mode, YEW_MODE_B);
    key = edit_key(YEW_KEY_UP);
    key.mods = YEW_MOD_ALT;
    yew_ed_handle_key(&ed, key, 1);
    YEW_ASSERT_EQ_U64(ed.win->cs.selstacks.data[0].n, 1U);
    yew_ed_free(&ed);
}

void test_edit_unit_selection_multicursor_has_independent_stacks(void)
{
    static const u8 text[] = "alpha\n\nbeta\n";
    Ed ed;
    Cursor second = {BYTEOFF(7U), {0U}, BYTEOFF(7U)};

    edit_fixture(&ed, text, sizeof(text) - 1U, YEW_EOL_LF);
    edit_place(&ed, 1U);
    YEW_ASSERT(yew_cset_add(&ed.win->cs, second));
    YEW_ASSERT_EQ_U64(ed.win->cs.curs.len, 2U);
    YEW_ASSERT_EQ_U64(edit_invoke(&ed, "ed.sel.unit.expand", 1U, false,
                                  NULL, 0U), YEW_CMD_OK);
    YEW_ASSERT_EQ_U64(ed.win->cs.curs.len, 2U);
    YEW_ASSERT_EQ_U64(ed.win->cs.selstacks.len, 2U);
    YEW_ASSERT_EQ_U64(ed.win->cs.selstacks.data[0].n, 1U);
    YEW_ASSERT_EQ_U64(ed.win->cs.selstacks.data[1].n, 1U);
    YEW_ASSERT(ed.win->cs.curs.data[0].pos.v !=
               ed.win->cs.curs.data[0].anchor.v);
    YEW_ASSERT(ed.win->cs.curs.data[1].pos.v !=
               ed.win->cs.curs.data[1].anchor.v);
    YEW_ASSERT_EQ_U64(edit_invoke(&ed, "ed.sel.unit.contract", 1U, false,
                                  NULL, 0U), YEW_CMD_OK);
    YEW_ASSERT_EQ_U64(ed.win->cs.curs.len, 2U);
    YEW_ASSERT_EQ_U64(ed.win->cs.selstacks.data[0].n, 0U);
    YEW_ASSERT_EQ_U64(ed.win->cs.selstacks.data[1].n, 0U);
    YEW_ASSERT_EQ_U64(ed.win->cs.curs.data[0].pos.v,
                      ed.win->cs.curs.data[0].anchor.v);
    YEW_ASSERT_EQ_U64(ed.win->cs.curs.data[1].pos.v,
                      ed.win->cs.curs.data[1].anchor.v);
    yew_ed_free(&ed);
}

#undef FAMILY
