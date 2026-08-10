#define _POSIX_C_SOURCE 200809L

#include "harness.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "edit/ed.h"
#include "edit/sel_actions.h"

typedef struct SelActionFixture {
    Ed ed;
} SelActionFixture;

static void fixture_init(SelActionFixture *f, const u8 *bytes, size_t len)
{
    yew_ed_init(&f->ed);
    YEW_ASSERT(yew_ed_open_scratch(&f->ed));
    yew_test_load_runtime(&f->ed);
    yew_undo_free(f->ed.buffer.undo);
    yew_textbuf_free(f->ed.buffer.tb);
    f->ed.buffer.tb = yew_textbuf_from_bytes(bytes, len);
    f->ed.buffer.undo = yew_undo_new(f->ed.buffer.tb);
    f->ed.buffer.tabwidth = 4U;
    f->ed.buffer.meta.eol = YEW_EOL_LF;
    f->ed.buffer.meta.dominant_eol = YEW_EOL_LF;
    f->ed.regs.clipboard_sync = YEW_CLIP_SYNC_OFF;
    YEW_ASSERT_EQ_U64(yew_mode_enter_highlight(&f->ed, YEW_MODE_L, false),
                      YEW_CMD_OK);
}

static void fixture_free(SelActionFixture *f)
{
    yew_ed_free(&f->ed);
}

static Cursor make_selection(u64 lo, u64 hi)
{
    Cursor cursor = {BYTEOFF(hi), {0U}, BYTEOFF(lo)};

    return cursor;
}

static void set_selection(SelActionFixture *f, SelKind kind,
                          u64 lo, u64 hi)
{
    f->ed.win->h.kind = kind;
    f->ed.win->cs.curs.data[f->ed.win->cs.primary] =
        make_selection(lo, hi);
    yew_cset_normalize(f->ed.buffer.tb, &f->ed.win->cs);
}

static void add_selection(SelActionFixture *f, u64 lo, u64 hi)
{
    YEW_ASSERT(yew_cset_add(&f->ed.win->cs, make_selection(lo, hi)));
    yew_cset_normalize(f->ed.buffer.tb, &f->ed.win->cs);
}

static Bytebuf materialize(const TextBuf *tb)
{
    Bytebuf out;
    TextIter it;

    bytebuf_init(&out);
    if (yew_textiter_begin(&it, tb, BYTEOFF(0U))) {
        do {
            const u8 *bytes;
            u64 len;

            YEW_ASSERT(yew_textiter_chunk(&it, tb, &bytes, &len));
            bytebuf_append(&out, bytes, (size_t)len);
        } while (yew_textiter_advance(&it, tb));
    }
    return out;
}

static void assert_text(SelActionFixture *f, const u8 *want, size_t len)
{
    Bytebuf got = materialize(f->ed.buffer.tb);

    YEW_ASSERT_EQ_U64(got.len, len);
    YEW_ASSERT_EQ_MEM(got.data, want, len);
    bytebuf_free(&got);
}

static CmdStatus invoke_change(SelActionFixture *f, CmdFn fn,
                               const char *arg, u32 arg_len)
{
    EditCtx ec = yew_ed_edit_ctx(&f->ed);
    CmdCtx cx = {0};
    CmdStatus status;

    cx.ed = &f->ed;
    cx.win = f->ed.win;
    cx.sarg = arg;
    cx.sarg_len = arg_len;
    cx.count = 1U;
    cx.source = YEW_SRC_TEST;
    yew_undo_begin(&ec, f->ed.win->cs.curs.len > 1U ? YEW_TXN_MULTI :
                                                        YEW_TXN_TYPE);
    status = fn(&cx);
    if (status == YEW_CMD_OK)
        yew_undo_end(&ec);
    else
        yew_undo_abort(&ec);
    return status;
}

static CmdStatus invoke_registered(SelActionFixture *f, const char *name,
                                   const char *arg, u32 arg_len)
{
    CmdCtx cx = {0};
    CmdId id = yew_cmd_lookup(name, (u32)strlen(name));

    YEW_ASSERT(id.v != 0U);
    cx.ed = &f->ed;
    cx.win = f->ed.win;
    cx.sarg = arg;
    cx.sarg_len = arg_len;
    cx.count = 1U;
    cx.source = YEW_SRC_TEST;
    return yew_ed_invoke(&f->ed, id, &cx);
}

void test_sel_actions_yank_uses_char_line_and_block_register_types(void)
{
    static const u8 bytes[] = "ab\ncd\nef";
    SelActionFixture f;
    CmdCtx cx = {0};
    RegVal *reg;

    fixture_init(&f, bytes, sizeof(bytes) - 1U);
    cx.ed = &f.ed;
    cx.win = f.ed.win;
    set_selection(&f, YEW_SEL_CHAR, 0U, 2U);
    YEW_ASSERT_EQ_U64(yew_sel_cmd_yank(&cx), YEW_CMD_OK);
    reg = yew_reg_get(&f.ed.regs, '"');
    YEW_ASSERT_EQ_U64(reg->type, YEW_REG_CHARWISE);
    YEW_ASSERT_EQ_U64(reg->bytes.len, 2U);
    YEW_ASSERT_EQ_MEM(reg->bytes.data, "ab", 2U);

    set_selection(&f, YEW_SEL_LINE, 3U, 4U);
    YEW_ASSERT_EQ_U64(yew_sel_cmd_yank(&cx), YEW_CMD_OK);
    YEW_ASSERT_EQ_U64(reg->type, YEW_REG_LINEWISE);
    YEW_ASSERT_EQ_MEM(reg->bytes.data, "cd\n", 3U);

    set_selection(&f, YEW_SEL_RECT, 1U, 8U);
    YEW_ASSERT_EQ_U64(yew_sel_cmd_yank(&cx), YEW_CMD_OK);
    YEW_ASSERT_EQ_U64(reg->type, YEW_REG_BLOCKWISE);
    YEW_ASSERT_EQ_U64(reg->rows.len, 3U);
    YEW_ASSERT_EQ_U64(reg->width, 1U);
    YEW_ASSERT_EQ_MEM(reg->bytes.data, "bdf", 3U);
    fixture_free(&f);
}

void test_sel_actions_aggregate_delete_is_one_edit_and_one_register(void)
{
    static const u8 bytes[] = "ab cd ef";
    static const u8 want[] = " cd ";
    SelActionFixture f;
    RegVal *reg;

    fixture_init(&f, bytes, sizeof(bytes) - 1U);
    set_selection(&f, YEW_SEL_CHAR, 0U, 2U);
    add_selection(&f, 6U, 8U);
    YEW_ASSERT_EQ_U64(invoke_registered(&f, "ed.sel.delete", NULL, 0U),
                      YEW_CMD_OK);
    assert_text(&f, want, sizeof(want) - 1U);
    reg = yew_reg_get(&f.ed.regs, '"');
    YEW_ASSERT_EQ_U64(reg->type, YEW_REG_CHARWISE);
    YEW_ASSERT_EQ_U64(reg->bytes.len, 4U);
    YEW_ASSERT_EQ_MEM(reg->bytes.data, "abef", 4U);
    YEW_ASSERT_EQ_U64(f.ed.win->cs.curs.len, 2U);
    YEW_ASSERT_EQ_U64(f.ed.mode, YEW_MODE_L);
    YEW_ASSERT_EQ_U64(yew_undo_current(f.ed.buffer.undo),
                      f.ed.buffer.undo->root + 1U);
    fixture_free(&f);
}

void test_sel_actions_delete_line_includes_its_eol(void)
{
    SelActionFixture f;
    RegVal *reg;

    fixture_init(&f, (const u8 *)"aa\nbb\ncc\n", 9U);
    set_selection(&f, YEW_SEL_LINE, 3U, 4U);
    YEW_ASSERT_EQ_U64(invoke_registered(&f, "ed.sel.delete", NULL, 0U),
                      YEW_CMD_OK);
    assert_text(&f, (const u8 *)"aa\ncc\n", 6U);
    reg = yew_reg_get(&f.ed.regs, '"');
    YEW_ASSERT_EQ_U64(reg->type, YEW_REG_LINEWISE);
    YEW_ASSERT_EQ_MEM(reg->bytes.data, "bb\n", 3U);
    YEW_ASSERT_EQ_U64(f.ed.win->cs.curs.data[0].pos.v, 3U);
    YEW_ASSERT_EQ_U64(f.ed.mode, YEW_MODE_L);
    fixture_free(&f);
}

void test_sel_actions_delete_rect_removes_raw_invalid_byte(void)
{
    static const u8 bytes[] = "a\xFFz\nbcz\n";
    static const u8 want[] = "az\nbz\n";
    SelActionFixture f;
    RegVal *reg;

    fixture_init(&f, bytes, sizeof(bytes) - 1U);
    set_selection(&f, YEW_SEL_RECT, 1U, 6U);
    YEW_ASSERT_EQ_U64(invoke_registered(&f, "ed.sel.delete", NULL, 0U),
                      YEW_CMD_OK);
    assert_text(&f, want, sizeof(want) - 1U);
    reg = yew_reg_get(&f.ed.regs, '"');
    YEW_ASSERT_EQ_U64(reg->type, YEW_REG_BLOCKWISE);
    YEW_ASSERT_EQ_U64(reg->rows.len, 2U);
    YEW_ASSERT_EQ_U64(reg->rows.data[0].hi - reg->rows.data[0].lo, 1U);
    YEW_ASSERT_EQ_U64(reg->bytes.data[reg->rows.data[0].lo], 0xFFU);
    YEW_ASSERT_EQ_U64(f.ed.mode, YEW_MODE_L);
    fixture_free(&f);
}

void test_sel_actions_unicode_case_maps_expand_and_preserve_raw_bytes(void)
{
    static const u8 bytes[] = "a\xC3\x9F!\xFFz";
    static const u8 upper[] = "ASS!\xFFz";
    SelActionFixture f;

    fixture_init(&f, bytes, sizeof(bytes) - 1U);
    set_selection(&f, YEW_SEL_CHAR, 0U, 5U);
    YEW_ASSERT_EQ_U64(invoke_change(&f, yew_sel_cmd_case_upper, NULL, 0U),
                      YEW_CMD_OK);
    assert_text(&f, upper, sizeof(upper) - 1U);
    YEW_ASSERT_EQ_U64(f.ed.win->cs.curs.data[0].pos.v, 0U);
    fixture_free(&f);

    fixture_init(&f, (const u8 *)"\xC4\xB0X", 3U);
    set_selection(&f, YEW_SEL_CHAR, 0U, 2U);
    YEW_ASSERT_EQ_U64(invoke_change(&f, yew_sel_cmd_case_lower, NULL, 0U),
                      YEW_CMD_OK);
    assert_text(&f, (const u8 *)"i\xCC\x87X", 4U);
    fixture_free(&f);

    fixture_init(&f, (const u8 *)"aB", 2U);
    set_selection(&f, YEW_SEL_CHAR, 0U, 2U);
    YEW_ASSERT_EQ_U64(invoke_change(&f, yew_sel_cmd_case_toggle, NULL, 0U),
                      YEW_CMD_OK);
    assert_text(&f, (const u8 *)"Ab", 2U);
    fixture_free(&f);
}

void test_sel_actions_change_captures_then_enters_insert_at_start(void)
{
    SelActionFixture f;
    RegVal *reg;

    fixture_init(&f, (const u8 *)"alpha beta", 10U);
    set_selection(&f, YEW_SEL_CHAR, 0U, 5U);
    YEW_ASSERT_EQ_U64(invoke_registered(&f, "ed.sel.change", NULL, 0U),
                      YEW_CMD_OK);
    assert_text(&f, (const u8 *)" beta", 5U);
    reg = yew_reg_get(&f.ed.regs, '"');
    YEW_ASSERT_EQ_U64(reg->type, YEW_REG_CHARWISE);
    YEW_ASSERT_EQ_MEM(reg->bytes.data, "alpha", 5U);
    YEW_ASSERT_EQ_U64(f.ed.mode, YEW_MODE_I);
    YEW_ASSERT_EQ_U64(f.ed.win->cs.curs.data[0].pos.v, 0U);
    fixture_free(&f);
}

void test_sel_actions_change_line_opens_insert_at_line_start(void)
{
    SelActionFixture f;
    RegVal *reg;

    fixture_init(&f, (const u8 *)"aa\nbb\ncc\n", 9U);
    set_selection(&f, YEW_SEL_LINE, 3U, 4U);
    YEW_ASSERT_EQ_U64(invoke_registered(&f, "ed.sel.change", NULL, 0U),
                      YEW_CMD_OK);
    assert_text(&f, (const u8 *)"aa\ncc\n", 6U);
    reg = yew_reg_get(&f.ed.regs, '"');
    YEW_ASSERT_EQ_U64(reg->type, YEW_REG_LINEWISE);
    YEW_ASSERT_EQ_MEM(reg->bytes.data, "bb\n", 3U);
    YEW_ASSERT_EQ_U64(f.ed.win->cs.curs.len, 1U);
    YEW_ASSERT_EQ_U64(f.ed.win->cs.curs.data[0].pos.v, 3U);
    YEW_ASSERT_EQ_U64(f.ed.mode, YEW_MODE_I);
    fixture_free(&f);
}

void test_sel_actions_change_rect_creates_one_caret_per_row(void)
{
    SelActionFixture f;

    fixture_init(&f, (const u8 *)"abc\nabc\n", 8U);
    set_selection(&f, YEW_SEL_RECT, 1U, 6U);
    YEW_ASSERT_EQ_U64(invoke_registered(&f, "ed.sel.change", NULL, 0U),
                      YEW_CMD_OK);
    assert_text(&f, (const u8 *)"ac\nac\n", 6U);
    YEW_ASSERT_EQ_U64(f.ed.win->cs.curs.len, 2U);
    YEW_ASSERT_EQ_U64(f.ed.win->cs.curs.data[0].pos.v, 1U);
    YEW_ASSERT_EQ_U64(f.ed.win->cs.curs.data[0].anchor.v, 1U);
    YEW_ASSERT_EQ_U64(f.ed.win->cs.curs.data[1].pos.v, 4U);
    YEW_ASSERT_EQ_U64(f.ed.win->cs.curs.data[1].anchor.v, 4U);
    YEW_ASSERT_EQ_U64(f.ed.mode, YEW_MODE_I);
    fixture_free(&f);
}

void test_sel_actions_case_commands_cover_whole_line_selections(void)
{
    SelActionFixture f;

    fixture_init(&f, (const u8 *)"a\xC3\x9F\nz\n", 6U);
    set_selection(&f, YEW_SEL_LINE, 0U, 1U);
    YEW_ASSERT_EQ_U64(invoke_change(&f, yew_sel_cmd_case_upper, NULL, 0U),
                      YEW_CMD_OK);
    assert_text(&f, (const u8 *)"ASS\nz\n", 6U);
    fixture_free(&f);

    fixture_init(&f, (const u8 *)"AB\nz\n", 5U);
    set_selection(&f, YEW_SEL_LINE, 0U, 1U);
    YEW_ASSERT_EQ_U64(invoke_change(&f, yew_sel_cmd_case_lower, NULL, 0U),
                      YEW_CMD_OK);
    assert_text(&f, (const u8 *)"ab\nz\n", 5U);
    fixture_free(&f);

    fixture_init(&f, (const u8 *)"aB\nz\n", 5U);
    set_selection(&f, YEW_SEL_LINE, 0U, 1U);
    YEW_ASSERT_EQ_U64(invoke_change(&f, yew_sel_cmd_case_toggle, NULL, 0U),
                      YEW_CMD_OK);
    assert_text(&f, (const u8 *)"Ab\nz\n", 5U);
    fixture_free(&f);
}

void test_sel_actions_case_commands_transform_each_rect_row(void)
{
    SelActionFixture f;

    fixture_init(&f, (const u8 *)"abq\nabq\n", 8U);
    set_selection(&f, YEW_SEL_RECT, 0U, 6U);
    YEW_ASSERT_EQ_U64(invoke_change(&f, yew_sel_cmd_case_upper, NULL, 0U),
                      YEW_CMD_OK);
    assert_text(&f, (const u8 *)"ABq\nABq\n", 8U);
    fixture_free(&f);

    fixture_init(&f, (const u8 *)"ABq\nABq\n", 8U);
    set_selection(&f, YEW_SEL_RECT, 0U, 6U);
    YEW_ASSERT_EQ_U64(invoke_change(&f, yew_sel_cmd_case_lower, NULL, 0U),
                      YEW_CMD_OK);
    assert_text(&f, (const u8 *)"abq\nabq\n", 8U);
    fixture_free(&f);

    fixture_init(&f, (const u8 *)"aBq\naBq\n", 8U);
    set_selection(&f, YEW_SEL_RECT, 0U, 6U);
    YEW_ASSERT_EQ_U64(invoke_change(&f, yew_sel_cmd_case_toggle, NULL, 0U),
                      YEW_CMD_OK);
    assert_text(&f, (const u8 *)"Abq\nAbq\n", 8U);
    fixture_free(&f);
}

void test_sel_actions_indent_dedent_mixed_whitespace_and_noop(void)
{
    static const u8 bytes[] = "one\n\ttwo\n  three\n";
    static const u8 indented[] = "    one\n\t\ttwo\n      three\n";
    static const u8 dedented[] = "one\n\t\ttwo\n      three\n";
    SelActionFixture f;

    fixture_init(&f, bytes, sizeof(bytes) - 1U);
    set_selection(&f, YEW_SEL_CHAR, 0U, 15U);
    YEW_ASSERT_EQ_U64(invoke_change(&f, yew_sel_cmd_indent, NULL, 0U),
                      YEW_CMD_OK);
    assert_text(&f, indented, sizeof(indented) - 1U);
    YEW_ASSERT_EQ_U64(invoke_change(&f, yew_sel_cmd_dedent, NULL, 0U),
                      YEW_CMD_OK);
    /* The collapsed selection covers one line; only its indent is removed. */
    assert_text(&f, dedented, sizeof(dedented) - 1U);
    fixture_free(&f);
}

void test_sel_actions_indent_and_dedent_cover_lines_for_line_and_rect(void)
{
    SelActionFixture f;

    fixture_init(&f, (const u8 *)"a\nb\n", 4U);
    set_selection(&f, YEW_SEL_LINE, 0U, 2U);
    YEW_ASSERT_EQ_U64(invoke_change(&f, yew_sel_cmd_indent, NULL, 0U),
                      YEW_CMD_OK);
    assert_text(&f, (const u8 *)"    a\n    b\n", 12U);
    fixture_free(&f);

    fixture_init(&f, (const u8 *)"    a\n    b\n", 12U);
    set_selection(&f, YEW_SEL_LINE, 0U, 6U);
    YEW_ASSERT_EQ_U64(invoke_change(&f, yew_sel_cmd_dedent, NULL, 0U),
                      YEW_CMD_OK);
    assert_text(&f, (const u8 *)"a\nb\n", 4U);
    fixture_free(&f);

    fixture_init(&f, (const u8 *)"a\nb\n", 4U);
    set_selection(&f, YEW_SEL_RECT, 0U, 2U);
    YEW_ASSERT_EQ_U64(invoke_change(&f, yew_sel_cmd_indent, NULL, 0U),
                      YEW_CMD_OK);
    assert_text(&f, (const u8 *)"    a\n    b\n", 12U);
    fixture_free(&f);

    fixture_init(&f, (const u8 *)"    a\n    b\n", 12U);
    set_selection(&f, YEW_SEL_RECT, 0U, 6U);
    YEW_ASSERT_EQ_U64(invoke_change(&f, yew_sel_cmd_dedent, NULL, 0U),
                      YEW_CMD_OK);
    assert_text(&f, (const u8 *)"a\nb\n", 4U);
    fixture_free(&f);
}

void test_sel_actions_join_collapses_space_without_space_before_punctuation(void)
{
    static const u8 bytes[] = "one  \n  two\n , three\nnext";
    static const u8 want[] = "one two, three\nnext";
    SelActionFixture f;

    fixture_init(&f, bytes, sizeof(bytes) - 1U);
    set_selection(&f, YEW_SEL_CHAR, 1U, 17U);
    YEW_ASSERT_EQ_U64(invoke_change(&f, yew_sel_cmd_join, NULL, 0U),
                      YEW_CMD_OK);
    assert_text(&f, want, sizeof(want) - 1U);
    YEW_ASSERT_EQ_U64(f.ed.win->cs.curs.data[0].pos.v, 0U);
    fixture_free(&f);
}

void test_sel_actions_join_uses_covered_lines_for_line_and_rect(void)
{
    SelActionFixture f;

    fixture_init(&f, (const u8 *)"one\n  two\n", 10U);
    set_selection(&f, YEW_SEL_LINE, 0U, 6U);
    YEW_ASSERT_EQ_U64(invoke_change(&f, yew_sel_cmd_join, NULL, 0U),
                      YEW_CMD_OK);
    assert_text(&f, (const u8 *)"one two\n", 8U);
    fixture_free(&f);

    fixture_init(&f, (const u8 *)"one\n  two\n", 10U);
    set_selection(&f, YEW_SEL_RECT, 0U, 6U);
    YEW_ASSERT_EQ_U64(invoke_change(&f, yew_sel_cmd_join, NULL, 0U),
                      YEW_CMD_OK);
    assert_text(&f, (const u8 *)"one two\n", 8U);
    fixture_free(&f);
}

void test_sel_actions_replace_char_counts_graphemes_for_char_and_rect(void)
{
    static const u8 bytes[] = "a\xCC\x81\xE6\xBC\xA2z\nabc\n";
    static const u8 rect_want[] = "a*c\na*c\n";
    SelActionFixture f;

    fixture_init(&f, bytes, sizeof(bytes) - 1U);
    set_selection(&f, YEW_SEL_CHAR, 0U, 6U);
    YEW_ASSERT_EQ_U64(invoke_change(&f, yew_sel_cmd_replace_char, "*", 1U),
                      YEW_CMD_OK);
    assert_text(&f, (const u8 *)"**z\nabc\n", 8U);
    fixture_free(&f);

    fixture_init(&f, (const u8 *)"abc\nabc\n", 8U);
    set_selection(&f, YEW_SEL_RECT, 1U, 6U);
    YEW_ASSERT_EQ_U64(invoke_change(&f, yew_sel_cmd_replace_char, "*", 1U),
                      YEW_CMD_OK);
    assert_text(&f, rect_want, sizeof(rect_want) - 1U);
    fixture_free(&f);
}

void test_sel_actions_replace_char_covers_complete_line_selection(void)
{
    SelActionFixture f;

    fixture_init(&f, (const u8 *)"ab\nz\n", 5U);
    set_selection(&f, YEW_SEL_LINE, 0U, 1U);
    YEW_ASSERT_EQ_U64(invoke_change(&f, yew_sel_cmd_replace_char, "*", 1U),
                      YEW_CMD_OK);
    assert_text(&f, (const u8 *)"***z\n", 5U);
    YEW_ASSERT_EQ_U64(f.ed.mode, YEW_MODE_L);
    fixture_free(&f);
}

void test_sel_actions_shift_char_line_and_rect_are_byte_exact(void)
{
    SelActionFixture f;

    fixture_init(&f, (const u8 *)"abcd", 4U);
    set_selection(&f, YEW_SEL_CHAR, 1U, 3U);
    YEW_ASSERT_EQ_U64(invoke_change(&f, yew_sel_cmd_shift_left, NULL, 0U),
                      YEW_CMD_OK);
    assert_text(&f, (const u8 *)"bcad", 4U);
    fixture_free(&f);

    fixture_init(&f, (const u8 *)"aa\nbb\ncc\n", 9U);
    set_selection(&f, YEW_SEL_LINE, 3U, 4U);
    YEW_ASSERT_EQ_U64(invoke_change(&f, yew_sel_cmd_shift_right, NULL, 0U),
                      YEW_CMD_OK);
    assert_text(&f, (const u8 *)"aa\ncc\nbb\n", 9U);
    fixture_free(&f);

    fixture_init(&f, (const u8 *)"abcd\nabcd\n", 10U);
    set_selection(&f, YEW_SEL_RECT, 1U, 7U);
    YEW_ASSERT_EQ_U64(invoke_change(&f, yew_sel_cmd_shift_right, NULL, 0U),
                      YEW_CMD_OK);
    assert_text(&f, (const u8 *)"acbd\nacbd\n", 10U);
    fixture_free(&f);
}

void test_sel_actions_shift_inverse_directions_are_byte_exact(void)
{
    SelActionFixture f;

    fixture_init(&f, (const u8 *)"abcd", 4U);
    set_selection(&f, YEW_SEL_CHAR, 1U, 3U);
    YEW_ASSERT_EQ_U64(invoke_change(&f, yew_sel_cmd_shift_right, NULL, 0U),
                      YEW_CMD_OK);
    assert_text(&f, (const u8 *)"adbc", 4U);
    fixture_free(&f);

    fixture_init(&f, (const u8 *)"aa\nbb\ncc\n", 9U);
    set_selection(&f, YEW_SEL_LINE, 3U, 4U);
    YEW_ASSERT_EQ_U64(invoke_change(&f, yew_sel_cmd_shift_left, NULL, 0U),
                      YEW_CMD_OK);
    assert_text(&f, (const u8 *)"bb\naa\ncc\n", 9U);
    fixture_free(&f);

    fixture_init(&f, (const u8 *)"abcd\nabcd\n", 10U);
    set_selection(&f, YEW_SEL_RECT, 1U, 7U);
    YEW_ASSERT_EQ_U64(invoke_change(&f, yew_sel_cmd_shift_left, NULL, 0U),
                      YEW_CMD_OK);
    assert_text(&f, (const u8 *)"bacd\nbacd\n", 10U);
    fixture_free(&f);
}

void test_sel_actions_rect_insert_and_append_pad_and_lift_rows(void)
{
    static const u8 bytes[] = "abcd\n\nwxyz\n";
    static const u8 padded[] = "abcd\n \nwxyz\n";
    SelActionFixture f;

    fixture_init(&f, bytes, sizeof(bytes) - 1U);
    set_selection(&f, YEW_SEL_RECT, 1U, 9U);
    YEW_ASSERT_EQ_U64(invoke_change(&f, yew_sel_cmd_rect_insert, NULL, 0U),
                      YEW_CMD_OK);
    assert_text(&f, padded, sizeof(padded) - 1U);
    YEW_ASSERT_EQ_U64(f.ed.mode, YEW_MODE_I);
    YEW_ASSERT_EQ_U64(f.ed.win->cs.curs.len, 3U);
    YEW_ASSERT_EQ_U64(f.ed.win->cs.curs.data[0].pos.v, 1U);
    YEW_ASSERT_EQ_U64(f.ed.win->cs.curs.data[1].pos.v, 6U);
    YEW_ASSERT_EQ_U64(f.ed.win->cs.curs.data[2].pos.v, 8U);
    fixture_free(&f);

    fixture_init(&f, bytes, sizeof(bytes) - 1U);
    set_selection(&f, YEW_SEL_RECT, 1U, 9U);
    YEW_ASSERT_EQ_U64(invoke_change(&f, yew_sel_cmd_rect_append, NULL, 0U),
                      YEW_CMD_OK);
    assert_text(&f, (const u8 *)"abcd\n   \nwxyz\n", 14U);
    YEW_ASSERT_EQ_U64(f.ed.mode, YEW_MODE_I);
    YEW_ASSERT_EQ_U64(f.ed.win->cs.curs.len, 3U);
    fixture_free(&f);
}

void test_sel_actions_rect_insert_typing_is_one_multi_undo_transaction(void)
{
    static const u8 before[] = "abcd\n\nwxyz\n";
    static const u8 typed[] = "aXbcd\n X\nwXxyz\n";
    SelActionFixture f;
    EditCtx ec;
    Key key = {0};

    fixture_init(&f, before, sizeof(before) - 1U);
    set_selection(&f, YEW_SEL_RECT, 1U, 9U);
    YEW_ASSERT_EQ_U64(invoke_registered(&f, "ed.edit.rect.insert", NULL,
                                        0U), YEW_CMD_OK);
    YEW_ASSERT(f.ed.insert_txn);
    YEW_ASSERT_EQ_U64(f.ed.buffer.undo->pending_reason, YEW_TXN_MULTI);
    YEW_ASSERT_EQ_U64(f.ed.win->cs.curs.len, 3U);

    key.kind = YEW_EV_KEY;
    key.ev = YEW_KEY_PRESS;
    key.code = (u32)'X';
    key.ntext = 1U;
    key.text[0] = (u8)'X';
    yew_ed_handle_key(&f.ed, key, 10);
    assert_text(&f, typed, sizeof(typed) - 1U);
    key.code = YEW_KEY_ESCAPE;
    key.ntext = 0U;
    yew_ed_handle_key(&f.ed, key, 11);
    YEW_ASSERT(!f.ed.insert_txn);
    YEW_ASSERT_EQ_U64(f.ed.buffer.undo->nodes.len, 2U);

    ec = yew_ed_edit_ctx(&f.ed);
    YEW_ASSERT(yew_undo(&ec));
    assert_text(&f, before, sizeof(before) - 1U);
    YEW_ASSERT_EQ_U64(f.ed.win->cs.curs.len, 1U);
    YEW_ASSERT_EQ_U64(f.ed.win->cs.curs.data[0].anchor.v, 1U);
    YEW_ASSERT_EQ_U64(f.ed.win->cs.curs.data[0].pos.v, 9U);
    fixture_free(&f);
}

void test_sel_actions_rect_append_uses_effective_wide_and_tab_edges(void)
{
    SelActionFixture f;

    fixture_init(&f, (const u8 *)"abq\nx\xE6\xBC\xA2z\nabq\n", 14U);
    set_selection(&f, YEW_SEL_RECT, 0U, 12U);
    YEW_ASSERT_EQ_U64(invoke_change(&f, yew_sel_cmd_rect_append, NULL, 0U),
                      YEW_CMD_OK);
    YEW_ASSERT_EQ_U64(f.ed.win->cs.curs.len, 3U);
    YEW_ASSERT_EQ_U64(f.ed.win->cs.curs.data[0].pos.v, 2U);
    YEW_ASSERT_EQ_U64(f.ed.win->cs.curs.data[1].pos.v, 8U);
    YEW_ASSERT_EQ_U64(f.ed.win->cs.curs.data[2].pos.v, 12U);
    fixture_free(&f);

    fixture_init(&f, (const u8 *)"abq\nx\tz\nabq\n", 12U);
    set_selection(&f, YEW_SEL_RECT, 0U, 10U);
    YEW_ASSERT_EQ_U64(invoke_change(&f, yew_sel_cmd_rect_append, NULL, 0U),
                      YEW_CMD_OK);
    YEW_ASSERT_EQ_U64(f.ed.win->cs.curs.len, 3U);
    YEW_ASSERT_EQ_U64(f.ed.win->cs.curs.data[0].pos.v, 2U);
    YEW_ASSERT_EQ_U64(f.ed.win->cs.curs.data[1].pos.v, 6U);
    YEW_ASSERT_EQ_U64(f.ed.win->cs.curs.data[2].pos.v, 10U);
    fixture_free(&f);
}

/*
 * The journal handle a selection edit opens must land on the BUFFER.
 *
 * apply_edits works through a COPY of the EditCtx, and yew_edit_delete
 * opens the crash journal into that copy on the first write.  When the
 * copy was dropped without yew_ed_finish_edit, b->jrn stayed NULL: the
 * descriptor outlived the buffer that owned it (valgrind's --track-fds
 * reported it at exit in s17_char_delete_matches_highlight), and the
 * journal nobody held was a journal nobody could sync or discard — a
 * crash after a selection delete would have recovered nothing.
 *
 * Asserted on the buffer rather than on the fd count, because the fd is
 * the symptom and the dangling ownership is the defect.
 */
void test_sel_actions_delete_hands_the_journal_to_the_buffer(void)
{
    static const u8 before[] = "alpha\nbeta\n";
    char root[] = "/tmp/yew-seljrn-XXXXXX";
    char source[512];
    char *saved_copy = NULL;
    const char *saved_state;
    CmdCtx cx = {0};
    CmdId id;
    FILE *fp;
    Ed ed;
    int n;

    saved_state = getenv("XDG_STATE_HOME");
    if (saved_state != NULL) {
        size_t saved_len = strlen(saved_state) + 1U;

        saved_copy = yew_xmalloc(saved_len);
        (void)memcpy(saved_copy, saved_state, saved_len);
    }
    YEW_ASSERT_NOT_NULL(mkdtemp(root));
    n = snprintf(source, sizeof(source), "%s/source.txt", root);
    YEW_ASSERT(n > 0 && (size_t)n < sizeof(source));
    fp = fopen(source, "wb");
    YEW_ASSERT_NOT_NULL(fp);
    YEW_ASSERT_EQ_U64(fwrite(before, 1U, sizeof(before) - 1U, fp),
                      sizeof(before) - 1U);
    YEW_ASSERT_EQ_I64(fclose(fp), 0);
    YEW_ASSERT_EQ_I64(setenv("XDG_STATE_HOME", root, 1), 0);

    yew_ed_init(&ed);
    YEW_ASSERT_EQ_U64(yew_ed_open(&ed, source), YEW_LOAD_OK);
    YEW_ASSERT_NULL(ed.buffer.jrn);
    YEW_ASSERT_EQ_U64(yew_mode_enter_highlight(&ed, YEW_MODE_L, false),
                      YEW_CMD_OK);
    ed.win->h.kind = YEW_SEL_CHAR;
    ed.win->cs.curs.data[ed.win->cs.primary] = make_selection(0U, 5U);
    yew_cset_normalize(ed.buffer.tb, &ed.win->cs);

    id = yew_cmd_lookup("ed.sel.delete", 13U);
    YEW_ASSERT(id.v != 0U);
    cx.ed = &ed;
    cx.win = ed.win;
    cx.count = 1U;
    cx.source = YEW_SRC_TEST;
    YEW_ASSERT_EQ_U64(yew_ed_invoke(&ed, id, &cx), YEW_CMD_OK);
    YEW_ASSERT_NOT_NULL(ed.buffer.jrn);
    YEW_ASSERT(!ed.durability_failed);

    /* A clean save retires the journal through the same handle. */
    YEW_ASSERT_EQ_U64(yew_ed_file_save(&ed, false), YEW_CMD_OK);
    YEW_ASSERT_NULL(ed.buffer.jrn);
    yew_ed_free(&ed);

    if (saved_copy != NULL) {
        YEW_ASSERT_EQ_I64(setenv("XDG_STATE_HOME", saved_copy, 1), 0);
    } else {
        YEW_ASSERT_EQ_I64(unsetenv("XDG_STATE_HOME"), 0);
    }
    free(saved_copy);
    (void)unlink(source);
}
