#include "harness.h"

#include <string.h>

#include "edit/ed.h"
#include "edit/flapi_cmds.h"
#include "fl/record.h"
#include "text/undo.h"

typedef struct ReplayMcFix {
    Ed ed;
} ReplayMcFix;

static Cursor replay_cursor(u64 off)
{
    Cursor cursor;

    cursor.pos = BYTEOFF(off);
    cursor.anchor = BYTEOFF(off);
    cursor.goal_col = (GCol){0U};
    return cursor;
}

static void replay_mc_open(ReplayMcFix *f, const char *text)
{
    size_t len = strlen(text);

    yew_ed_init(&f->ed);
    YEW_ASSERT(yew_ed_open_scratch(&f->ed));
    yew_undo_free(f->ed.buffer.undo);
    yew_textbuf_free(f->ed.buffer.tb);
    f->ed.buffer.tb = yew_textbuf_from_bytes((const u8 *)text, len);
    f->ed.buffer.undo = yew_undo_new(f->ed.buffer.tb);
    f->ed.win->buf = &f->ed.buffer;
    yew_reg_bind_context(&f->ed.regs, f->ed.buffer.undo,
                         &f->ed.buffer.meta);
    f->ed.win->cs.curs.data[0] = replay_cursor(0U);
}

static CmdStatus replay_mc_run(ReplayMcFix *f, const char *name,
                               const char *arg, u32 arg_len)
{
    CmdCtx cx = {0};
    CmdId id = yew_cmd_lookup(name, (u32)strlen(name));

    YEW_ASSERT(id.v != 0U);
    cx.ed = &f->ed;
    cx.win = f->ed.win;
    cx.count = 1U;
    cx.sarg = arg;
    cx.sarg_len = arg_len;
    cx.source = YEW_SRC_TEST;
    return yew_ed_invoke(&f->ed, id, &cx);
}

static void replay_assert_text(const ReplayMcFix *f, const char *want)
{
    TextIter it;
    size_t want_len = strlen(want);
    size_t done = 0U;

    YEW_ASSERT_EQ_U64(yew_textbuf_len(f->ed.buffer.tb), want_len);
    if (want_len == 0U)
        return;
    YEW_ASSERT(yew_textiter_begin(&it, f->ed.buffer.tb, BYTEOFF(0U)));
    while (done < want_len) {
        const u8 *bytes;
        u64 len;
        size_t take;

        YEW_ASSERT(yew_textiter_chunk(&it, f->ed.buffer.tb, &bytes, &len));
        take = len < want_len - done ? (size_t)len : want_len - done;
        YEW_ASSERT_EQ_MEM(bytes, want + done, take);
        done += take;
        if (done < want_len)
            YEW_ASSERT(yew_textiter_advance(&it, f->ed.buffer.tb));
    }
}

static void replay_set_two_cursors(ReplayMcFix *f, u64 first, u64 second)
{
    yew_cset_free(&f->ed.win->cs);
    yew_cset_init(&f->ed.win->cs, replay_cursor(first));
    YEW_ASSERT(yew_cset_add(&f->ed.win->cs, replay_cursor(second)));
}

static void replay_assert_two_cursors(const ReplayMcFix *f, u64 first,
                                      u64 second)
{
    YEW_ASSERT_EQ_U64(f->ed.win->cs.curs.len, 2U);
    YEW_ASSERT_EQ_U64(f->ed.win->cs.curs.data[0].pos.v, first);
    YEW_ASSERT_EQ_U64(f->ed.win->cs.curs.data[0].anchor.v, first);
    YEW_ASSERT_EQ_U64(f->ed.win->cs.curs.data[1].pos.v, second);
    YEW_ASSERT_EQ_U64(f->ed.win->cs.curs.data[1].anchor.v, second);
}

void test_macro_replay_insert_fans_out_to_current_cursor_set(void)
{
    EditCtx ec;
    ReplayMcFix f;

    replay_mc_open(&f, "abcd");
    YEW_ASSERT(yew_record_start(&f.ed, (u8)'a'));
    YEW_ASSERT_EQ_I64(replay_mc_run(&f, "ed.edit.insert.text", "X", 1U),
                      YEW_CMD_OK);
    YEW_ASSERT_EQ_I64(yew_record_stop(&f.ed), YEW_CMD_OK);
    ec = yew_ed_edit_ctx(&f.ed);
    YEW_ASSERT(yew_undo(&ec));
    replay_assert_text(&f, "abcd");

    replay_set_two_cursors(&f, 1U, 3U);
    YEW_ASSERT_EQ_I64(yew_macro_replay(&f.ed, (u8)'a', 1U), YEW_CMD_OK);
    replay_assert_text(&f, "aXbcXd");
    YEW_ASSERT_EQ_U64(f.ed.buffer.undo->nodes.data[
                          f.ed.buffer.undo->open == 0U
                              ? f.ed.buffer.undo->cur - 1U
                              : f.ed.buffer.undo->open - 1U].reason,
                      YEW_TXN_MACRO);
    YEW_ASSERT_EQ_U64(f.ed.buffer.undo->pending_reason,
                      YEW_TXN_REASON_MAX);
    ec = yew_ed_edit_ctx(&f.ed);
    YEW_ASSERT(yew_undo(&ec));
    replay_assert_text(&f, "abcd");
    replay_assert_two_cursors(&f, 1U, 3U);
    YEW_ASSERT(!yew_undo(&ec));
    yew_ed_free(&f.ed);
}

void test_macro_replay_delete_fans_out_to_current_cursor_set(void)
{
    EditCtx ec;
    ReplayMcFix f;

    replay_mc_open(&f, "abcd");
    YEW_ASSERT(yew_record_start(&f.ed, (u8)'b'));
    YEW_ASSERT_EQ_I64(replay_mc_run(&f, "ed.edit.delete.grapheme", NULL,
                                    0U), YEW_CMD_OK);
    YEW_ASSERT_EQ_I64(yew_record_stop(&f.ed), YEW_CMD_OK);
    ec = yew_ed_edit_ctx(&f.ed);
    YEW_ASSERT(yew_undo(&ec));
    replay_assert_text(&f, "abcd");

    replay_set_two_cursors(&f, 1U, 3U);
    YEW_ASSERT_EQ_I64(yew_macro_replay(&f.ed, (u8)'b', 1U), YEW_CMD_OK);
    replay_assert_text(&f, "ac");
    ec = yew_ed_edit_ctx(&f.ed);
    YEW_ASSERT(yew_undo(&ec));
    replay_assert_text(&f, "abcd");
    replay_assert_two_cursors(&f, 1U, 3U);
    YEW_ASSERT(!yew_undo(&ec));
    yew_ed_free(&f.ed);
}

void test_macro_replay_multicursor_error_restores_buffer_and_cursors(void)
{
    static const char macro[] =
        "@[ i\"X\" ]\n"
        "missing_function()\n";
    EditCtx ec;
    ReplayMcFix f;

    replay_mc_open(&f, "abcd");
    replay_set_two_cursors(&f, 1U, 3U);
    YEW_ASSERT_EQ_I64(yew_flapi_reg_write(&f.ed, (u8)'c',
                                          (const u8 *)macro,
                                          sizeof(macro) - 1U, false),
                      YEW_CMD_OK);
    YEW_ASSERT_EQ_I64(yew_macro_replay(&f.ed, (u8)'c', 1U),
                      YEW_CMD_ERR_STATE);
    replay_assert_text(&f, "abcd");
    replay_assert_two_cursors(&f, 1U, 3U);
    ec = yew_ed_edit_ctx(&f.ed);
    YEW_ASSERT_EQ_U64(f.ed.buffer.undo->depth, 0U);
    YEW_ASSERT_EQ_U64(f.ed.buffer.undo->pending_reason,
                      YEW_TXN_REASON_MAX);
    YEW_ASSERT(!yew_undo(&ec));
    yew_ed_free(&f.ed);
}

void test_resolved_dispatch_honors_explicit_cursor_target(void)
{
    CmdCtx cx = {0};
    CmdId insert;
    ReplayMcFix f;

    replay_mc_open(&f, "abcd");
    replay_set_two_cursors(&f, 1U, 3U);
    insert = yew_cmd_lookup("ed.edit.insert.text", 19U);
    YEW_ASSERT(insert.v != 0U);
    cx.ed = &f.ed;
    cx.win = f.ed.win;
    cx.cursor_index = 1U;
    cx.cursor_given = true;
    cx.count = 1U;
    cx.sarg = "X";
    cx.sarg_len = 1U;
    cx.source = YEW_SRC_TEST;
    YEW_ASSERT_EQ_I64(yew_ed_invoke(&f.ed, insert, &cx), YEW_CMD_OK);
    replay_assert_text(&f, "abcXd");
    YEW_ASSERT_EQ_U64(f.ed.buffer.undo->nodes.data[
                          f.ed.buffer.undo->cur - 1U].n_ops,
                      1U);
    yew_ed_free(&f.ed);
}
