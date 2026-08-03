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
    key.kind = SAG_EV_KEY;
    key.ev = SAG_KEY_PRESS;
    if (code < 0x80U) {
        key.ntext = 1U;
        key.text[0] = (u8)code;
    }
    return key;
}

static Key edit_text_key(const u8 *bytes, u8 len)
{
    Key key = {0};

    SAG_ASSERT(len <= sizeof(key.text));
    key.code = len == 1U ? bytes[0] : 0U;
    key.kind = SAG_EV_KEY;
    key.ev = SAG_KEY_PRESS;
    key.ntext = len;
    (void)memcpy(key.text, bytes, len);
    return key;
}

static void edit_fixture(Ed *ed, const u8 *bytes, size_t len, SagEol eol)
{
    Cursor *cursor;

    sag_ed_init(ed);
    SAG_ASSERT(sag_ed_open_scratch(ed));
    sag_undo_free(ed->buffer.undo);
    sag_textbuf_free(ed->buffer.tb);
    ed->buffer.tb = sag_textbuf_from_bytes(bytes, len);
    ed->buffer.undo = sag_undo_new(ed->buffer.tb);
    ed->buffer.meta.eol = eol;
    ed->buffer.meta.dominant_eol = eol;
    ed->buffer.meta.crlf_count = eol == SAG_EOL_CRLF ? 1U : 0U;
    ed->buffer.meta.lf_count = eol == SAG_EOL_LF ? 1U : 0U;
    ed->win->vp.rows = 4U;
    ed->win->vp.cols = 80U;
    ed->win->rect.h = 4U;
    ed->win->rect.w = 80U;
    cursor = sag_ed_cursor(ed);
    SAG_ASSERT_NOT_NULL(cursor);
    cursor->pos = BYTEOFF(0U);
    cursor->anchor = BYTEOFF(0U);
    cursor->goal_col = (GCol){0U};
    SAG_ASSERT_EQ_U64(sag_undo_current(ed->buffer.undo),
                      ed->buffer.undo->root);
}

static CmdStatus edit_invoke(Ed *ed, const char *name, u32 count,
                             bool count_given, const char *arg, u32 arg_len)
{
    CmdId id = sag_cmd_lookup(name, (u32)strlen(name));
    CmdCtx cx = {0};

    SAG_ASSERT(id.v != 0U);
    cx.ed = ed;
    cx.win = ed->win;
    cx.count = count;
    cx.count_given = count_given;
    cx.sarg = arg;
    cx.sarg_len = arg_len;
    cx.source = SAG_SRC_TEST;
    return sag_ed_invoke(ed, id, &cx);
}

static Bytebuf edit_materialize(const TextBuf *tb)
{
    Bytebuf out;
    TextIter iter;

    bytebuf_init(&out);
    if (sag_textiter_begin(&iter, tb, BYTEOFF(0U))) {
        do {
            const u8 *chunk;
            u64 len;

            SAG_ASSERT(sag_textiter_chunk(&iter, tb, &chunk, &len));
            bytebuf_append(&out, chunk, (size_t)len);
        } while (sag_textiter_advance(&iter, tb));
    }
    return out;
}

static void edit_assert_text(const Ed *ed, const u8 *want, size_t want_len)
{
    Bytebuf got = edit_materialize(ed->buffer.tb);

    SAG_ASSERT_EQ_U64(sag_textbuf_len(ed->buffer.tb), want_len);
    SAG_ASSERT_EQ_U64(got.len, want_len);
    SAG_ASSERT_EQ_MEM(got.data, want, want_len);
    bytebuf_free(&got);
}

static void edit_place(Ed *ed, u64 off)
{
    Cursor *cursor = sag_ed_cursor(ed);
    Span line;

    SAG_ASSERT_NOT_NULL(cursor);
    cursor->pos = BYTEOFF(off);
    cursor->anchor = cursor->pos;
    line = sag_textbuf_line_span(ed->buffer.tb,
                                 sag_textbuf_line_of(ed->buffer.tb,
                                                    cursor->pos));
    cursor->goal_col = sag_off_to_gcol(ed->buffer.tb, line, cursor->pos);
}

void test_edit_l_motion_table_handles_unicode_crlf_and_viewport_counts(void)
{
    Ed ed;
    Cursor *cursor;
    Span line;

    edit_fixture(&ed, mixed_fixture, sizeof(mixed_fixture) - 1U,
                 SAG_EOL_CRLF);
    cursor = sag_ed_cursor(&ed);
    edit_place(&ed, 5U);
    SAG_ASSERT_EQ_U64(edit_invoke(&ed, "ed.move.line.home", 1U, false,
                                  NULL, 0U), SAG_CMD_OK);
    SAG_ASSERT_EQ_U64(cursor->pos.v, 0U);
    SAG_ASSERT_EQ_U64(edit_invoke(&ed, "ed.move.line.end", 1U, false,
                                  NULL, 0U), SAG_CMD_OK);
    SAG_ASSERT_EQ_U64(cursor->pos.v, 9U);
    SAG_ASSERT_EQ_U64(edit_invoke(&ed, "ed.move.line.first_nonblank", 1U,
                                  false, NULL, 0U), SAG_CMD_OK);
    SAG_ASSERT_EQ_U64(cursor->pos.v, 2U);
    SAG_ASSERT_EQ_U64(edit_invoke(&ed, "ed.move.line.last_nonblank", 1U,
                                  false, NULL, 0U), SAG_CMD_OK);
    SAG_ASSERT_EQ_U64(cursor->pos.v, 6U);

    SAG_ASSERT_EQ_U64(edit_invoke(&ed, "ed.move.line.down", 1U, false,
                                  NULL, 0U), SAG_CMD_OK);
    SAG_ASSERT_EQ_U64(sag_textbuf_line_of(ed.buffer.tb, cursor->pos).v, 1U);
    SAG_ASSERT_EQ_U64(edit_invoke(&ed, "ed.move.line.up", 1U, false,
                                  NULL, 0U), SAG_CMD_OK);
    SAG_ASSERT_EQ_U64(sag_textbuf_line_of(ed.buffer.tb, cursor->pos).v, 0U);
    SAG_ASSERT_EQ_U64(edit_invoke(&ed, "ed.move.line.half_page_down", 1U,
                                  false, NULL, 0U), SAG_CMD_OK);
    SAG_ASSERT_EQ_U64(sag_textbuf_line_of(ed.buffer.tb, cursor->pos).v, 2U);
    SAG_ASSERT_EQ_U64(edit_invoke(&ed, "ed.view.page_down", 1U, false,
                                  NULL, 0U), SAG_CMD_OK);
    SAG_ASSERT_EQ_U64(sag_textbuf_line_of(ed.buffer.tb, cursor->pos).v, 4U);
    SAG_ASSERT_EQ_U64(edit_invoke(&ed, "ed.view.page_up", 1U, false,
                                  NULL, 0U), SAG_CMD_OK);
    SAG_ASSERT_EQ_U64(sag_textbuf_line_of(ed.buffer.tb, cursor->pos).v, 2U);
    SAG_ASSERT_EQ_U64(edit_invoke(&ed, "ed.move.line.half_page_up", 1U,
                                  false, NULL, 0U), SAG_CMD_OK);
    SAG_ASSERT_EQ_U64(sag_textbuf_line_of(ed.buffer.tb, cursor->pos).v, 0U);

    SAG_ASSERT_EQ_U64(edit_invoke(&ed, "ed.move.buf.end", 12U, true,
                                  NULL, 0U), SAG_CMD_OK);
    SAG_ASSERT_EQ_U64(sag_textbuf_line_of(ed.buffer.tb, cursor->pos).v, 11U);
    line = sag_textbuf_line_span(ed.buffer.tb, LINENO(11U));
    SAG_ASSERT_EQ_U64(cursor->pos.v, line.lo);
    SAG_ASSERT_EQ_U64(edit_invoke(&ed, "ed.move.buf.home", 1U, false,
                                  NULL, 0U), SAG_CMD_OK);
    SAG_ASSERT_EQ_U64(cursor->pos.v, 0U);
    sag_ed_free(&ed);
}

void test_edit_12G_dispatch_lands_on_one_based_line_twelve(void)
{
    Ed ed;
    Cursor *cursor;

    edit_fixture(&ed, mixed_fixture, sizeof(mixed_fixture) - 1U,
                 SAG_EOL_CRLF);
    sag_ed_handle_key(&ed, edit_key((u32)'1'), 0);
    sag_ed_handle_key(&ed, edit_key((u32)'2'), 0);
    SAG_ASSERT(ed.chord.count_given);
    SAG_ASSERT_EQ_U64(ed.chord.count, 12U);
    sag_ed_handle_key(&ed, edit_key((u32)'G'), 0);
    cursor = sag_ed_cursor(&ed);
    SAG_ASSERT_EQ_U64(ed.last_status, SAG_CMD_OK);
    SAG_ASSERT_EQ_U64(sag_textbuf_line_of(ed.buffer.tb, cursor->pos).v, 11U);
    SAG_ASSERT_EQ_U64(cursor->pos.v,
                      sag_textbuf_line_start(ed.buffer.tb, LINENO(11U)).v);
    SAG_ASSERT(!ed.chord.count_given);
    SAG_ASSERT_EQ_U64(ed.dispatch_count, 1U);
    sag_ed_free(&ed);
}

void test_edit_delete_zwj_grapheme_removes_exactly_twenty_five_bytes(void)
{
    static const u8 before[] = FAMILY "X";
    static const u8 after[] = "X";
    Ed ed;

    edit_fixture(&ed, before, sizeof(before) - 1U, SAG_EOL_LF);
    SAG_ASSERT_EQ_U64(sizeof(before) - sizeof(after), 25U);
    SAG_ASSERT_EQ_U64(edit_invoke(&ed, "ed.edit.delete.grapheme", 1U,
                                  false, NULL, 0U), SAG_CMD_OK);
    edit_assert_text(&ed, after, sizeof(after) - 1U);
    SAG_ASSERT_EQ_U64(sag_ed_cursor(&ed)->pos.v, 0U);
    SAG_ASSERT_EQ_U64(sag_undo_current(ed.buffer.undo),
                      ed.buffer.undo->root + 1U);
    sag_ed_free(&ed);
}

void test_edit_4x_is_one_undo_step_for_four_clusters(void)
{
    static const u8 before[] = "a" FAMILY "\xe6\xbc\xa2" "bc";
    static const u8 after[] = "c";
    Ed ed;

    edit_fixture(&ed, before, sizeof(before) - 1U, SAG_EOL_LF);
    sag_ed_handle_key(&ed, edit_key((u32)'4'), 0);
    sag_ed_handle_key(&ed, edit_key((u32)'x'), 0);
    SAG_ASSERT_EQ_U64(ed.last_status, SAG_CMD_OK);
    SAG_ASSERT_EQ_U64(ed.dispatch_count, 1U);
    SAG_ASSERT_EQ_U64(sag_undo_current(ed.buffer.undo),
                      ed.buffer.undo->root + 1U);
    edit_assert_text(&ed, after, sizeof(after) - 1U);
    SAG_ASSERT_EQ_U64(edit_invoke(&ed, "ed.edit.undo", 1U, false,
                                  NULL, 0U), SAG_CMD_OK);
    SAG_ASSERT_EQ_U64(sag_undo_current(ed.buffer.undo),
                      ed.buffer.undo->root);
    edit_assert_text(&ed, before, sizeof(before) - 1U);
    sag_ed_free(&ed);
}

void test_edit_insert_newline_uses_crlf_bytes(void)
{
    static const u8 before[] = "ab\r\ncd";
    static const u8 after[] = "ab\r\n\r\ncd";
    Ed ed;

    edit_fixture(&ed, before, sizeof(before) - 1U, SAG_EOL_CRLF);
    edit_place(&ed, 4U);
    SAG_ASSERT_EQ_U64(sag_mode_enter(&ed, SAG_MODE_I), SAG_CMD_OK);
    sag_ed_handle_key(&ed, edit_key(SAG_KEY_ENTER), 0);
    SAG_ASSERT_EQ_U64(ed.last_status, SAG_CMD_OK);
    edit_assert_text(&ed, after, sizeof(after) - 1U);
    SAG_ASSERT_EQ_U64(sag_ed_cursor(&ed)->pos.v, 6U);
    SAG_ASSERT_EQ_U64(sag_undo_current(ed.buffer.undo),
                      ed.buffer.undo->root + 1U);
    SAG_ASSERT(!ed.insert_txn);
    SAG_ASSERT_EQ_U64(ed.doc_damage_lo, 1U);
    SAG_ASSERT_EQ_U64(ed.doc_damage_hi, 4U);
    sag_ed_free(&ed);
}

void test_edit_backspace_at_line_start_joins_crlf_lines(void)
{
    static const u8 before[] = "ab\r\ncd";
    static const u8 after[] = "abcd";
    Ed ed;

    edit_fixture(&ed, before, sizeof(before) - 1U, SAG_EOL_CRLF);
    edit_place(&ed, 4U);
    SAG_ASSERT_EQ_U64(sag_mode_enter(&ed, SAG_MODE_I), SAG_CMD_OK);
    sag_ed_handle_key(&ed, edit_key(SAG_KEY_BACKSPACE), 0);
    SAG_ASSERT_EQ_U64(ed.last_status, SAG_CMD_OK);
    edit_assert_text(&ed, after, sizeof(after) - 1U);
    SAG_ASSERT_EQ_U64(sag_ed_cursor(&ed)->pos.v, 2U);
    SAG_ASSERT(ed.insert_txn);
    sag_ed_handle_key(&ed, edit_key(SAG_KEY_ESCAPE), 0);
    SAG_ASSERT(!ed.insert_txn);
    SAG_ASSERT_EQ_U64(sag_undo_current(ed.buffer.undo),
                      ed.buffer.undo->root + 1U);
    sag_ed_free(&ed);
}

void test_edit_insert_text_and_tab_share_one_insert_transaction(void)
{
    static const u8 acute[] = {0xc3U, 0xa9U};
    static const u8 want[] = {0xc3U, 0xa9U, (u8)'\t'};
    Ed ed;

    edit_fixture(&ed, NULL, 0U, SAG_EOL_LF);
    SAG_ASSERT_EQ_U64(sag_mode_enter(&ed, SAG_MODE_I), SAG_CMD_OK);
    sag_ed_handle_key(&ed, edit_text_key(acute, sizeof(acute)), 0);
    SAG_ASSERT_EQ_U64(ed.last_status, SAG_CMD_OK);
    SAG_ASSERT(ed.insert_txn);
    SAG_ASSERT_EQ_U64(ed.doc_damage_lo, 0U);
    SAG_ASSERT_EQ_U64(ed.doc_damage_hi, 1U);
    sag_ed_handle_key(&ed, edit_key(SAG_KEY_TAB), 0);
    SAG_ASSERT_EQ_U64(ed.last_status, SAG_CMD_OK);
    SAG_ASSERT(ed.insert_txn);
    sag_ed_handle_key(&ed, edit_key(SAG_KEY_ESCAPE), 0);
    edit_assert_text(&ed, want, sizeof(want));
    SAG_ASSERT_EQ_U64(sag_undo_current(ed.buffer.undo),
                      ed.buffer.undo->root + 1U);
    SAG_ASSERT(!ed.insert_txn);
    SAG_ASSERT_EQ_U64(edit_invoke(&ed, "ed.edit.undo", 1U, false,
                                  NULL, 0U), SAG_CMD_OK);
    edit_assert_text(&ed, NULL, 0U);
    sag_ed_free(&ed);
}

void test_edit_insert_motion_splits_typing_into_two_undo_steps(void)
{
    static const u8 typed[] = "hello";
    static const u8 after_motion[] = "helol";
    static const u8 hel[] = "hel";
    static const u8 empty[] = "";
    Ed ed;
    size_t i;

    edit_fixture(&ed, NULL, 0U, SAG_EOL_LF);
    SAG_ASSERT_EQ_U64(sag_mode_enter(&ed, SAG_MODE_I), SAG_CMD_OK);
    for (i = 0U; i < 3U; i++)
        sag_ed_handle_key(&ed, edit_key((u32)typed[i]), 0);
    SAG_ASSERT(ed.insert_txn);
    sag_ed_handle_key(&ed, edit_key(SAG_KEY_LEFT), 0);
    SAG_ASSERT(!ed.insert_txn);
    for (i = 3U; i < sizeof(typed) - 1U; i++)
        sag_ed_handle_key(&ed, edit_key((u32)typed[i]), 0);
    sag_ed_handle_key(&ed, edit_key(SAG_KEY_ESCAPE), 0);
    edit_assert_text(&ed, after_motion, sizeof(after_motion) - 1U);
    SAG_ASSERT_EQ_U64(sag_undo_current(ed.buffer.undo),
                      ed.buffer.undo->root + 2U);
    SAG_ASSERT_EQ_U64(edit_invoke(&ed, "ed.edit.undo", 1U, false,
                                  NULL, 0U), SAG_CMD_OK);
    edit_assert_text(&ed, hel, sizeof(hel) - 1U);
    SAG_ASSERT_EQ_U64(edit_invoke(&ed, "ed.edit.undo", 1U, false,
                                  NULL, 0U), SAG_CMD_OK);
    edit_assert_text(&ed, empty, 0U);
    sag_ed_free(&ed);
}

void test_edit_escape_closes_typing_as_one_undo_step(void)
{
    static const u8 hello[] = "hello";
    Ed ed;
    size_t i;

    edit_fixture(&ed, NULL, 0U, SAG_EOL_LF);
    SAG_ASSERT_EQ_U64(sag_mode_enter(&ed, SAG_MODE_I), SAG_CMD_OK);
    for (i = 0U; i < sizeof(hello) - 1U; i++)
        sag_ed_handle_key(&ed, edit_key((u32)hello[i]), 0);
    SAG_ASSERT(ed.insert_txn);
    SAG_ASSERT_EQ_U64(sag_undo_current(ed.buffer.undo),
                      ed.buffer.undo->root + 1U);
    SAG_ASSERT_EQ_U64(ed.buffer.undo->depth, 1U);
    sag_ed_handle_key(&ed, edit_key(SAG_KEY_ESCAPE), 0);
    SAG_ASSERT_EQ_U64(ed.mode, SAG_MODE_L);
    SAG_ASSERT(!ed.insert_txn);
    SAG_ASSERT_EQ_U64(sag_undo_current(ed.buffer.undo),
                      ed.buffer.undo->root + 1U);
    edit_assert_text(&ed, hello, sizeof(hello) - 1U);
    SAG_ASSERT_EQ_U64(edit_invoke(&ed, "ed.edit.undo", 1U, false,
                                  NULL, 0U), SAG_CMD_OK);
    edit_assert_text(&ed, NULL, 0U);
    sag_ed_free(&ed);
}

void test_edit_open_above_and_below_emit_native_eol_and_enter_insert(void)
{
    static const u8 before[] = "aa\r\nbb";
    static const u8 above[] = "aa\r\n\r\nbb";
    static const u8 both[] = "aa\r\n\r\n\r\nbb";
    Ed ed;

    edit_fixture(&ed, before, sizeof(before) - 1U, SAG_EOL_CRLF);
    edit_place(&ed, 4U);
    SAG_ASSERT_EQ_U64(edit_invoke(&ed, "ed.edit.line.open_above", 1U,
                                  false, NULL, 0U), SAG_CMD_OK);
    SAG_ASSERT_EQ_U64(ed.mode, SAG_MODE_I);
    SAG_ASSERT_EQ_U64(sag_ed_cursor(&ed)->pos.v, 4U);
    edit_assert_text(&ed, above, sizeof(above) - 1U);
    SAG_ASSERT_EQ_U64(sag_mode_escape(&ed), SAG_CMD_OK);
    SAG_ASSERT_EQ_U64(edit_invoke(&ed, "ed.edit.line.open_below", 1U,
                                  false, NULL, 0U), SAG_CMD_OK);
    SAG_ASSERT_EQ_U64(ed.mode, SAG_MODE_I);
    SAG_ASSERT_EQ_U64(sag_ed_cursor(&ed)->pos.v, 6U);
    edit_assert_text(&ed, both, sizeof(both) - 1U);
    sag_ed_free(&ed);
}

void test_edit_line_delete_undo_and_redo_preserve_crlf_bytes(void)
{
    static const u8 before[] = "aa\r\nbb\r\ncc";
    static const u8 after[] = "aa\r\ncc";
    Ed ed;

    edit_fixture(&ed, before, sizeof(before) - 1U, SAG_EOL_CRLF);
    edit_place(&ed, 4U);
    sag_ed_handle_key(&ed, edit_key((u32)'d'), 0);
    SAG_ASSERT_EQ_U64(ed.chord.n, 1U);
    sag_ed_handle_key(&ed, edit_key((u32)'d'), 0);
    SAG_ASSERT_EQ_U64(ed.last_status, SAG_CMD_OK);
    edit_assert_text(&ed, after, sizeof(after) - 1U);
    SAG_ASSERT_EQ_U64(sag_undo_current(ed.buffer.undo),
                      ed.buffer.undo->root + 1U);
    sag_ed_handle_key(&ed, edit_key((u32)'u'), 0);
    edit_assert_text(&ed, before, sizeof(before) - 1U);
    SAG_ASSERT_EQ_U64(sag_undo_current(ed.buffer.undo),
                      ed.buffer.undo->root);
    SAG_ASSERT_EQ_U64(edit_invoke(&ed, "ed.edit.redo", 1U, false,
                                  NULL, 0U), SAG_CMD_OK);
    edit_assert_text(&ed, after, sizeof(after) - 1U);
    sag_ed_free(&ed);
}

void test_edit_insert_after_and_i_arrows_use_grapheme_boundaries(void)
{
    static const u8 before[] = "A" FAMILY "\xe6\xbc\xa2\r\nZ";
    Ed ed;
    Cursor *cursor;

    edit_fixture(&ed, before, sizeof(before) - 1U, SAG_EOL_CRLF);
    cursor = sag_ed_cursor(&ed);
    SAG_ASSERT_EQ_U64(edit_invoke(&ed, "ed.edit.insert.after", 1U, false,
                                  NULL, 0U), SAG_CMD_OK);
    SAG_ASSERT_EQ_U64(ed.mode, SAG_MODE_I);
    SAG_ASSERT_EQ_U64(cursor->pos.v, 1U);
    sag_ed_handle_key(&ed, edit_key(SAG_KEY_RIGHT), 0);
    SAG_ASSERT_EQ_U64(cursor->pos.v, 26U);
    sag_ed_handle_key(&ed, edit_key(SAG_KEY_RIGHT), 0);
    SAG_ASSERT_EQ_U64(cursor->pos.v, 29U);
    sag_ed_handle_key(&ed, edit_key(SAG_KEY_LEFT), 0);
    SAG_ASSERT_EQ_U64(cursor->pos.v, 26U);
    sag_ed_handle_key(&ed, edit_key(SAG_KEY_END), 0);
    SAG_ASSERT_EQ_U64(cursor->pos.v, 29U);
    sag_ed_handle_key(&ed, edit_key(SAG_KEY_DOWN), 0);
    SAG_ASSERT_EQ_U64(sag_textbuf_line_of(ed.buffer.tb, cursor->pos).v, 1U);
    sag_ed_handle_key(&ed, edit_key(SAG_KEY_HOME), 0);
    SAG_ASSERT_EQ_U64(cursor->pos.v,
                      sag_textbuf_line_start(ed.buffer.tb, LINENO(1U)).v);
    sag_ed_handle_key(&ed, edit_key(SAG_KEY_UP), 0);
    SAG_ASSERT_EQ_U64(sag_textbuf_line_of(ed.buffer.tb, cursor->pos).v, 0U);
    sag_ed_free(&ed);
}

void test_edit_forward_delete_removes_invalid_byte_as_one_cluster(void)
{
    static const u8 before[] = {(u8)'a', 0xffU, (u8)'b'};
    static const u8 after[] = {(u8)'a', (u8)'b'};
    Ed ed;

    edit_fixture(&ed, before, sizeof(before), SAG_EOL_LF);
    edit_place(&ed, 1U);
    SAG_ASSERT_EQ_U64(sag_mode_enter(&ed, SAG_MODE_I), SAG_CMD_OK);
    sag_ed_handle_key(&ed, edit_key(SAG_KEY_DELETE), 0);
    SAG_ASSERT_EQ_U64(ed.last_status, SAG_CMD_OK);
    edit_assert_text(&ed, after, sizeof(after));
    SAG_ASSERT_EQ_U64(sag_ed_cursor(&ed)->pos.v, 1U);
    sag_ed_free(&ed);
}

void test_edit_journal_open_failure_returns_io_without_mutation(void)
{
    static const u8 before[] = "abc";
    static const u8 after[] = "Xabc";
    char root[] = "/tmp/sagitta-edit-cmd-journal-XXXXXX";
    char blocker[128];
    char journal_dir[128];
    char sagitta_dir[128];
    char source_path[128];
    const char source[] = "/tmp/sagitta-edit-cmd-source";
    const char *saved_state;
    char *saved_copy = NULL;
    FILE *fp;
    Ed ed;
    size_t source_len = sizeof(source);
    int n;

    saved_state = getenv("XDG_STATE_HOME");
    if (saved_state != NULL) {
        size_t saved_len = strlen(saved_state) + 1U;

        saved_copy = sag_xmalloc(saved_len);
        (void)memcpy(saved_copy, saved_state, saved_len);
    }
    SAG_ASSERT_NOT_NULL(mkdtemp(root));
    n = snprintf(blocker, sizeof(blocker), "%s/blocker", root);
    SAG_ASSERT(n > 0 && (size_t)n < sizeof(blocker));
    fp = fopen(blocker, "wb");
    SAG_ASSERT_NOT_NULL(fp);
    SAG_ASSERT_EQ_U64(fwrite("x", 1U, 1U, fp), 1U);
    SAG_ASSERT_EQ_I64(fclose(fp), 0);
    SAG_ASSERT_EQ_I64(setenv("XDG_STATE_HOME", blocker, 1), 0);

    edit_fixture(&ed, before, sizeof(before) - 1U, SAG_EOL_LF);
    ed.buffer.path = arena_strdup(&ed.arena, source);
    ed.buffer.meta.realpath = sag_xmalloc(source_len);
    (void)memcpy(ed.buffer.meta.realpath, source, source_len);
    SAG_ASSERT_EQ_U64(edit_invoke(&ed, "ed.edit.insert.text", 1U, false,
                                  "X", 1U), SAG_CMD_ERR_IO);
    edit_assert_text(&ed, before, sizeof(before) - 1U);
    SAG_ASSERT_EQ_U64(sag_undo_current(ed.buffer.undo),
                      ed.buffer.undo->root);
    SAG_ASSERT_EQ_U64(ed.buffer.undo->nodes.len, 1U);
    SAG_ASSERT_EQ_U64(ed.buffer.undo->ops.len, 0U);
    SAG_ASSERT_NULL(ed.buffer.jrn);
    SAG_ASSERT_EQ_U64(sag_ed_cursor(&ed)->pos.v, 0U);
    SAG_ASSERT(ed.durability_failed);
    SAG_ASSERT(ed.msg.active);
    SAG_ASSERT_EQ_U64(ed.msg.sev, SAG_MSG_ERROR);
    SAG_ASSERT(strstr(ed.msg.text, "crash journal failed") != NULL);
    SAG_ASSERT_EQ_U64(edit_invoke(&ed, "ed.edit.insert.text", 1U, false,
                                  "Y", 1U), SAG_CMD_ERR_IO);
    edit_assert_text(&ed, before, sizeof(before) - 1U);
    SAG_ASSERT_EQ_U64(sag_ed_request_quit(&ed, false), SAG_CMD_ERR_IO);
    SAG_ASSERT(!ed.quit);
    SAG_ASSERT_EQ_U64(sag_ed_request_quit(&ed, true), SAG_CMD_OK);
    SAG_ASSERT(ed.quit);
    sag_ed_free(&ed);

    n = snprintf(source_path, sizeof(source_path), "%s/source.txt", root);
    SAG_ASSERT(n > 0 && (size_t)n < sizeof(source_path));
    fp = fopen(source_path, "wb");
    SAG_ASSERT_NOT_NULL(fp);
    SAG_ASSERT_EQ_U64(fwrite(before, 1U, sizeof(before) - 1U, fp),
                      sizeof(before) - 1U);
    SAG_ASSERT_EQ_I64(fclose(fp), 0);
    SAG_ASSERT_EQ_I64(setenv("XDG_STATE_HOME", root, 1), 0);
    sag_ed_init(&ed);
    SAG_ASSERT_EQ_U64(sag_ed_open(&ed, source_path), SAG_LOAD_OK);
    SAG_ASSERT_EQ_U64(edit_invoke(&ed, "ed.edit.insert.text", 1U, false,
                                  "X", 1U), SAG_CMD_OK);
    edit_assert_text(&ed, after, sizeof(after) - 1U);
    SAG_ASSERT_EQ_U64(sag_ed_file_save(&ed, false), SAG_CMD_OK);
    SAG_ASSERT_NULL(ed.buffer.jrn);
    SAG_ASSERT_EQ_I64(setenv("XDG_STATE_HOME", blocker, 1), 0);
    SAG_ASSERT_EQ_U64(edit_invoke(&ed, "ed.edit.undo", 1U, false,
                                  NULL, 0U), SAG_CMD_ERR_IO);
    edit_assert_text(&ed, after, sizeof(after) - 1U);
    SAG_ASSERT(ed.durability_failed);
    SAG_ASSERT_EQ_U64(sag_ed_request_quit(&ed, false), SAG_CMD_ERR_IO);
    SAG_ASSERT(!ed.quit);
    sag_ed_free(&ed);

    if (saved_copy != NULL) {
        SAG_ASSERT_EQ_I64(setenv("XDG_STATE_HOME", saved_copy, 1), 0);
    } else {
        SAG_ASSERT_EQ_I64(unsetenv("XDG_STATE_HOME"), 0);
    }
    free(saved_copy);
    n = snprintf(journal_dir, sizeof(journal_dir), "%s/sagitta/journal",
                 root);
    SAG_ASSERT(n > 0 && (size_t)n < sizeof(journal_dir));
    n = snprintf(sagitta_dir, sizeof(sagitta_dir), "%s/sagitta", root);
    SAG_ASSERT(n > 0 && (size_t)n < sizeof(sagitta_dir));
    SAG_ASSERT_EQ_I64(unlink(source_path), 0);
    SAG_ASSERT_EQ_I64(unlink(blocker), 0);
    SAG_ASSERT_EQ_I64(rmdir(journal_dir), 0);
    SAG_ASSERT_EQ_I64(rmdir(sagitta_dir), 0);
    SAG_ASSERT_EQ_I64(rmdir(root), 0);
}

void test_edit_unit_selection_replays_and_invalidates(void)
{
    static const u8 text[] = "one\n\n  two\n    three\n";
    Ed ed;
    Cursor *cursor;
    Span level0;
    Span level1;

    edit_fixture(&ed, text, sizeof(text) - 1U, SAG_EOL_LF);
    edit_place(&ed, 16U);
    cursor = sag_ed_cursor(&ed);
    SAG_ASSERT_EQ_U64(ed.mode, SAG_MODE_L);

    ed.full_damage = false;
    SAG_ASSERT_EQ_U64(edit_invoke(&ed, "ed.sel.unit.expand", 1U, false,
                                  NULL, 0U), SAG_CMD_OK);
    SAG_ASSERT(ed.full_damage);
    SAG_ASSERT_EQ_U64(ed.win->sels.n, 1U);
    level0 = ed.win->sels.s[0];
    SAG_ASSERT_EQ_U64(cursor->anchor.v, level0.lo);
    SAG_ASSERT_EQ_U64(cursor->pos.v, level0.hi);
    SAG_ASSERT_EQ_U64(edit_invoke(&ed, "ed.sel.unit.expand", 1U, false,
                                  NULL, 0U), SAG_CMD_OK);
    SAG_ASSERT_EQ_U64(ed.win->sels.n, 2U);
    level1 = ed.win->sels.s[1];
    SAG_ASSERT(level1.lo <= level0.lo && level1.hi >= level0.hi);
    SAG_ASSERT_EQ_U64(edit_invoke(&ed, "ed.sel.unit.expand", 1U, false,
                                  NULL, 0U), SAG_CMD_OK);
    SAG_ASSERT(ed.win->sels.n >= 3U);

    ed.full_damage = false;
    SAG_ASSERT_EQ_U64(edit_invoke(&ed, "ed.sel.unit.contract", 1U, false,
                                  NULL, 0U), SAG_CMD_OK);
    SAG_ASSERT(ed.full_damage);
    SAG_ASSERT_EQ_U64(cursor->anchor.v,
                      ed.win->sels.s[ed.win->sels.n - 1U].lo);
    SAG_ASSERT_EQ_U64(cursor->pos.v,
                      ed.win->sels.s[ed.win->sels.n - 1U].hi);
    SAG_ASSERT_EQ_U64(edit_invoke(&ed, "ed.sel.unit.contract", 1U, false,
                                  NULL, 0U), SAG_CMD_OK);
    SAG_ASSERT_EQ_U64(ed.win->sels.n, 1U);
    SAG_ASSERT_EQ_U64(cursor->anchor.v, level0.lo);
    SAG_ASSERT_EQ_U64(cursor->pos.v, level0.hi);

    SAG_ASSERT_EQ_U64(edit_invoke(&ed, "ed.move.line.home", 1U, false,
                                  NULL, 0U), SAG_CMD_OK);
    SAG_ASSERT_EQ_U64(ed.win->sels.n, 0U);
    edit_place(&ed, 16U);
    SAG_ASSERT_EQ_U64(edit_invoke(&ed, "ed.sel.unit.expand", 1U, false,
                                  NULL, 0U), SAG_CMD_OK);
    SAG_ASSERT_EQ_U64(ed.win->sels.n, 1U);
    SAG_ASSERT_EQ_U64(edit_invoke(&ed, "ed.edit.insert.text", 1U, false,
                                  "X", 1U), SAG_CMD_OK);
    SAG_ASSERT_EQ_U64(ed.win->sels.n, 0U);
    sag_ed_free(&ed);
}

void test_edit_word_and_block_key_layers_dispatch(void)
{
    static const u8 line_text[] = "abcdef\nxy";
    static const u8 word_text[] =
        "foo \xe6\xbc\xa2\xe5\xad\x97 tail\n";
    static const u8 block_text[] = "one\n\n  two\n    three\n";
    Ed ed;
    Key key;

    edit_fixture(&ed, line_text, sizeof(line_text) - 1U, SAG_EOL_LF);
    ed.win->vp.cols = 3U;
    ed.win->vp.wrap = true;
    edit_place(&ed, 1U);
    sag_ed_handle_key(&ed, edit_key(SAG_KEY_DOWN), 0);
    SAG_ASSERT_EQ_U64(ed.last_cmd.v,
                      sag_cmd_lookup("ed.move.unit.next", 17U).v);
    SAG_ASSERT_EQ_U64(sag_ed_cursor(&ed)->pos.v, 4U);
    key = edit_key(SAG_KEY_DOWN);
    key.mods = SAG_MOD_ALT;
    sag_ed_handle_key(&ed, key, 1);
    SAG_ASSERT_EQ_U64(ed.last_cmd.v,
                      sag_cmd_lookup("ed.move.unit.next_alt", 21U).v);
    SAG_ASSERT_EQ_U64(sag_ed_cursor(&ed)->pos.v, 8U);
    sag_ed_handle_key(&ed, edit_key(SAG_KEY_LEFT), 2);
    SAG_ASSERT_EQ_U64(ed.last_cmd.v,
                      sag_cmd_lookup("ed.move.unit.home", 17U).v);
    SAG_ASSERT_EQ_U64(sag_ed_cursor(&ed)->pos.v, 7U);
    sag_ed_free(&ed);

    edit_fixture(&ed, word_text, sizeof(word_text) - 1U, SAG_EOL_LF);
    sag_ed_handle_key(&ed, edit_key((u32)'w'), 0);
    SAG_ASSERT_EQ_U64(ed.mode, SAG_MODE_W);
    sag_ed_handle_key(&ed, edit_key(SAG_KEY_RIGHT), 1);
    SAG_ASSERT_EQ_U64(sag_ed_cursor(&ed)->pos.v, 4U);
    sag_ed_free(&ed);

    edit_fixture(&ed, block_text, sizeof(block_text) - 1U, SAG_EOL_LF);
    edit_place(&ed, 16U);
    sag_ed_handle_key(&ed, edit_key((u32)'b'), 0);
    SAG_ASSERT_EQ_U64(ed.mode, SAG_MODE_B);
    key = edit_key(SAG_KEY_UP);
    key.mods = SAG_MOD_ALT;
    sag_ed_handle_key(&ed, key, 1);
    SAG_ASSERT_EQ_U64(ed.win->sels.n, 1U);
    sag_ed_free(&ed);
}

void test_edit_unit_selection_multicursor_names_sprint17(void)
{
    static const u8 text[] = "alpha\nbeta\n";
    Ed ed;
    Cursor second = {BYTEOFF(6U), {0U}, BYTEOFF(6U)};
    Bytebuf output;
    int pipefd[2];
    pid_t child;
    pid_t waited;
    int status;
    ssize_t count;
    u8 chunk[256];

    edit_fixture(&ed, text, sizeof(text) - 1U, SAG_EOL_LF);
    SAG_ASSERT(sag_cset_add(&ed.win->cs, second));
    bytebuf_init(&output);
    SAG_ASSERT_EQ_I64(fflush(NULL), 0);
    SAG_ASSERT_EQ_I64(pipe(pipefd), 0);
    child = fork();
    SAG_ASSERT(child >= 0);
    if (child == 0) {
        (void)close(pipefd[0]);
        if (dup2(pipefd[1], STDERR_FILENO) < 0)
            _exit(126);
        (void)close(pipefd[1]);
        (void)setenv("SAG_LOG", "/dev/null", 1);
        (void)edit_invoke(&ed, "ed.sel.unit.expand", 1U, false,
                          NULL, 0U);
        _exit(99);
    }
    (void)close(pipefd[1]);
    for (;;) {
        count = read(pipefd[0], chunk, sizeof(chunk));
        if (count > 0) {
            bytebuf_append(&output, chunk, (size_t)count);
            continue;
        }
        if (count < 0 && errno == EINTR)
            continue;
        break;
    }
    (void)close(pipefd[0]);
    do {
        waited = waitpid(child, &status, 0);
    } while (waited < 0 && errno == EINTR);
    SAG_ASSERT_EQ_I64(waited, child);
    SAG_ASSERT(WIFEXITED(status));
    SAG_ASSERT_EQ_I64(WEXITSTATUS(status), SAG_EXIT_BUG);
    bytebuf_append(&output, "", 1U);
    SAG_ASSERT(strstr((const char *)output.data, "Sprint 17") != NULL);
    bytebuf_free(&output);
    sag_ed_free(&ed);
}

#undef FAMILY
