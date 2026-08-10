/* Sprint 38: edit-a-macro uses the ordinary buffer and save machinery. */

#include "harness.h"

#include <string.h>

#include "edit/ed.h"
#include "edit/flapi_cmds.h"
#include "text/edit.h"
#include "text/register.h"
#include "text/undo.h"
#include "ui/macrobrowse.h"
#include "ui/message.h"

static void macro_set(Ed *ed, u8 reg, const char *source)
{
    YEW_ASSERT_EQ_I64(yew_flapi_reg_write(ed, reg, (const u8 *)source,
                                          (u32)strlen(source), false),
                      YEW_CMD_OK);
}

static Bytebuf macro_text(const TextBuf *tb)
{
    Bytebuf out;
    TextIter it;
    u64 left = yew_textbuf_len(tb);

    bytebuf_init(&out);
    if (left == 0U)
        return out;
    YEW_ASSERT(yew_textiter_begin(&it, tb, BYTEOFF(0U)));
    while (left != 0U) {
        const u8 *bytes;
        u64 len;
        size_t take;

        YEW_ASSERT(yew_textiter_chunk(&it, tb, &bytes, &len));
        if (len > left)
            len = left;
        take = len > SIZE_MAX ? SIZE_MAX : (size_t)len;
        bytebuf_append(&out, bytes, take);
        left -= len;
        if (left != 0U)
            YEW_ASSERT(yew_textiter_advance(&it, tb));
    }
    return out;
}

static void macro_replace_all(Ed *ed, Buffer *scratch, const char *source)
{
    EditCtx ec = yew_ed_edit_ctx_for(ed, ed->win);
    u64 old_len = yew_textbuf_len(scratch->tb);
    size_t len = strlen(source);

    YEW_ASSERT(ec.tb == scratch->tb);
    yew_undo_begin(&ec, YEW_TXN_TYPE);
    if (old_len != 0U)
        YEW_ASSERT(yew_edit_delete(&ec, (Span){0U, old_len}));
    if (len != 0U)
        YEW_ASSERT(yew_edit_insert(&ec, BYTEOFF(0U), (const u8 *)source,
                                   len));
    yew_undo_end(&ec);
    yew_ed_finish_edit(ed, &ec);
}

static const char *macro_message(const Ed *ed)
{
    return ed->msg.full == NULL ? ed->msg.text : ed->msg.full;
}

void test_macrobrowse_unknown_motion_store_is_atomic_and_points_at_word(void)
{
    static const char original[] = "fn original() { return 1 }\n";
    static const char invalid[] =
        "fn edited() {\n"
        "  return 1\n"
        "  @[ yankk ]\n"
        "}\n";
    Ed ed;
    Buffer *scratch;
    const RegVal *reg;
    const char *message;

    yew_ed_init(&ed);
    YEW_ASSERT(yew_ed_open_scratch(&ed));
    macro_set(&ed, (u8)'a', original);
    YEW_ASSERT_EQ_I64(yew_macro_edit(&ed, (u8)'a'), YEW_CMD_OK);
    scratch = ed.win->buf;
    macro_replace_all(&ed, scratch, invalid);
    YEW_ASSERT_EQ_I64(yew_macro_store(&ed, scratch), YEW_CMD_ERR_ARG);
    reg = yew_reg_get(&ed.regs, (u8)'a');
    YEW_ASSERT_NOT_NULL(reg);
    YEW_ASSERT_EQ_U64(reg->bytes.len, sizeof(original) - 1U);
    YEW_ASSERT_EQ_MEM(reg->bytes.data, original, sizeof(original) - 1U);
    message = macro_message(&ed);
    YEW_ASSERT(strstr(message, "no command has word 'yankk'") != NULL);
    YEW_ASSERT(strstr(message, "did you mean 'yank'?") != NULL);
    YEW_ASSERT(strstr(message, "*macro a*:3:6: error:") != NULL);
    YEW_ASSERT(strstr(message, "  @[ yankk ]\n     ^~~~~") != NULL);
    YEW_ASSERT_EQ_U64(ed.win->cs.curs.data[ed.win->cs.primary].pos.v,
                      strlen("fn edited() {\n  return 1\n  @[ "));
    YEW_ASSERT(!yew_undo_at_save_point(scratch->undo));
    yew_ed_free(&ed);
}

void test_macrobrowse_valid_store_marks_clean_and_sets_undo_boundary(void)
{
    static const char original[] = "fn original() { return 1 }\n";
    static const char edited[] = "fn edited() { return 2 }\n";
    Ed ed;
    Buffer *scratch;
    const RegVal *reg;
    u32 current;
    u32 nodes;

    yew_ed_init(&ed);
    YEW_ASSERT(yew_ed_open_scratch(&ed));
    macro_set(&ed, (u8)'b', original);
    YEW_ASSERT_EQ_I64(yew_macro_edit(&ed, (u8)'b'), YEW_CMD_OK);
    scratch = ed.win->buf;
    macro_replace_all(&ed, scratch, edited);
    YEW_ASSERT(!yew_undo_at_save_point(scratch->undo));
    current = yew_undo_current(scratch->undo);
    nodes = scratch->undo->nodes.len;
    YEW_ASSERT_EQ_I64(yew_macro_store(&ed, scratch), YEW_CMD_OK);
    YEW_ASSERT(yew_undo_at_save_point(scratch->undo));
    YEW_ASSERT(scratch->undo->boundary);
    YEW_ASSERT_EQ_U64(yew_undo_current(scratch->undo), current);
    YEW_ASSERT_EQ_U64(scratch->undo->nodes.len, nodes);
    YEW_ASSERT_EQ_U64(scratch->undo->saved, current);
    reg = yew_reg_get(&ed.regs, (u8)'b');
    YEW_ASSERT_NOT_NULL(reg);
    YEW_ASSERT_EQ_U64(reg->bytes.len, sizeof(edited) - 1U);
    YEW_ASSERT_EQ_MEM(reg->bytes.data, edited, sizeof(edited) - 1U);
    YEW_ASSERT_EQ_STR(ed.msg.text, "stored macro @b (1 lines)");
    yew_ed_free(&ed);
}

void test_macrobrowse_edit_is_an_ordinary_multicursor_undo_buffer(void)
{
    static const char source[] = "fn plain() { return 1 }\n";
    static const char inserted[] = "Xfn plain() { return 1 }\nX";
    Ed ed;
    Buffer *scratch;
    Cursor second;
    CmdCtx cx = {0};
    CmdId insert;
    Bytebuf text;
    EditCtx ec;

    yew_ed_init(&ed);
    YEW_ASSERT(yew_ed_open_scratch(&ed));
    macro_set(&ed, (u8)'c', source);
    YEW_ASSERT_EQ_I64(yew_macro_edit(&ed, (u8)'c'), YEW_CMD_OK);
    scratch = ed.win->buf;
    YEW_ASSERT(scratch != &ed.buffer);
    YEW_ASSERT_EQ_STR(scratch->name, "*macro c*");
    YEW_ASSERT_NULL(scratch->path);
    YEW_ASSERT_EQ_STR(scratch->lang, "fletch");
    YEW_ASSERT_EQ_U64(scratch->macro_reg, (u8)'c');
    YEW_ASSERT((scratch->flags & YEW_BUF_SCRATCH) != 0U);
    YEW_ASSERT((scratch->flags & YEW_BUF_NOUNDO) == 0U);
    YEW_ASSERT(yew_undo_at_save_point(scratch->undo));
    second = (Cursor){BYTEOFF(sizeof(source) - 1U), {0U},
                      BYTEOFF(sizeof(source) - 1U)};
    YEW_ASSERT(yew_cset_add(&ed.win->cs, second));
    insert = yew_cmd_lookup("ed.edit.insert.text", 19U);
    YEW_ASSERT(insert.v != 0U);
    cx.ed = &ed;
    cx.win = ed.win;
    cx.count = 1U;
    cx.sarg = "X";
    cx.sarg_len = 1U;
    cx.source = YEW_SRC_TEST;
    YEW_ASSERT_EQ_I64(yew_ed_invoke(&ed, insert, &cx), YEW_CMD_OK);
    YEW_ASSERT_EQ_U64(ed.win->cs.curs.len, 2U);
    YEW_ASSERT_EQ_U64(scratch->undo->nodes.len, 2U);
    YEW_ASSERT_EQ_U64(scratch->undo->nodes.data[1].reason, YEW_TXN_MULTI);
    YEW_ASSERT_EQ_U64(scratch->undo->nodes.data[1].n_ops, 2U);
    text = macro_text(scratch->tb);
    YEW_ASSERT_EQ_U64(text.len, sizeof(inserted) - 1U);
    YEW_ASSERT_EQ_MEM(text.data, inserted, sizeof(inserted) - 1U);
    bytebuf_free(&text);
    ec = yew_ed_edit_ctx_for(&ed, ed.win);
    YEW_ASSERT(yew_undo(&ec));
    yew_ed_finish_edit(&ed, &ec);
    text = macro_text(scratch->tb);
    YEW_ASSERT_EQ_U64(text.len, sizeof(source) - 1U);
    YEW_ASSERT_EQ_MEM(text.data, source, sizeof(source) - 1U);
    bytebuf_free(&text);
    YEW_ASSERT_EQ_U64(ed.win->cs.curs.len, 2U);
    YEW_ASSERT(yew_undo_at_save_point(scratch->undo));
    yew_ed_free(&ed);
}
