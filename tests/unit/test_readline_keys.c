/*
 * The readline/Emacs editing keys Insert mode binds.
 *
 * Every case drives a REGISTERED command through yew_ed_invoke against
 * the loaded runtime, so the transaction wrapping, the multi-cursor
 * fan-out and the kill-ring write are the ones a keystroke gets; the
 * binding tests at the bottom drive the real keys through
 * yew_ed_handle_key on top of that.
 */
#include "harness.h"

#include <string.h>

#include "edit/ed.h"
#include "edit/mode.h"
#include "text/register.h"

typedef struct RlFixture {
    Ed ed;
} RlFixture;

static void rl_fixture(RlFixture *f, const u8 *bytes, size_t len)
{
    Cursor *cursor;

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
    f->ed.buffer.meta.lf_count = 1U;
    f->ed.regs.clipboard_sync = YEW_CLIP_SYNC_OFF;
    f->ed.win->vp.rows = 8U;
    f->ed.win->vp.cols = 80U;
    f->ed.win->rect.h = 8U;
    f->ed.win->rect.w = 80U;
    cursor = yew_ed_cursor(&f->ed);
    YEW_ASSERT_NOT_NULL(cursor);
    cursor->pos = BYTEOFF(0U);
    cursor->anchor = BYTEOFF(0U);
    cursor->goal_col = (CCol){0U};
    YEW_ASSERT_EQ_U64(yew_mode_enter(&f->ed, YEW_MODE_I), YEW_CMD_OK);
}

static void rl_free(RlFixture *f)
{
    yew_ed_free(&f->ed);
}

static void rl_at(RlFixture *f, u64 off)
{
    Cursor *cursor = yew_ed_cursor(&f->ed);

    YEW_ASSERT_NOT_NULL(cursor);
    cursor->pos = BYTEOFF(off);
    cursor->anchor = BYTEOFF(off);
    cursor->goal_col = (CCol){0U};
}

static void rl_add(RlFixture *f, u64 off)
{
    Cursor extra = {BYTEOFF(off), {0U}, BYTEOFF(off)};

    YEW_ASSERT(yew_cset_add(&f->ed.win->cs, extra));
    yew_cset_normalize(f->ed.buffer.tb, &f->ed.win->cs);
}

static u64 rl_pos(const RlFixture *f, size_t index)
{
    YEW_ASSERT(index < f->ed.win->cs.curs.len);
    return f->ed.win->cs.curs.data[index].pos.v;
}

static CmdStatus rl_invoke(RlFixture *f, const char *name)
{
    CmdId id = yew_cmd_lookup(name, (u32)strlen(name));
    CmdCtx cx = {0};

    YEW_ASSERT(id.v != 0U);
    cx.ed = &f->ed;
    cx.win = f->ed.win;
    cx.count = 1U;
    cx.source = YEW_SRC_TEST;
    return yew_ed_invoke(&f->ed, id, &cx);
}

static void rl_ok(RlFixture *f, const char *name)
{
    YEW_ASSERT_EQ_U64(rl_invoke(f, name), YEW_CMD_OK);
}

static Bytebuf rl_materialize(const TextBuf *tb)
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

static void rl_expect(RlFixture *f, const char *want)
{
    size_t len = strlen(want);
    Bytebuf got = rl_materialize(f->ed.buffer.tb);

    YEW_ASSERT_EQ_U64(got.len, len);
    YEW_ASSERT_EQ_MEM(got.data, want, len);
    bytebuf_free(&got);
}

static void rl_expect_kill(RlFixture *f, const char *want)
{
    const RegVal *value = yew_reg_get(&f->ed.regs, (u8)'"');
    size_t len = strlen(want);

    YEW_ASSERT_NOT_NULL(value);
    YEW_ASSERT_EQ_U64(value->bytes.len, len);
    YEW_ASSERT_EQ_MEM(value->bytes.data, want, len);
}

/*
 * "alpha beta gamma\ndelta epsilon"
 *  0     6    11     16 17    23
 */
static const u8 rl_words[] = "alpha beta gamma\ndelta epsilon";

/* ---------------------------------------------------------------- kills */

/*
 * The headline key.  One press per caret, every caret, and ONE register
 * value holding what all of them removed, in document order -- the same
 * shape ed.sel.delete gives a multi-cursor cut.
 */
void test_readline_kill_word_prev_reaches_every_cursor(void)
{
    RlFixture f;

    rl_fixture(&f, rl_words, sizeof(rl_words) - 1U);
    rl_at(&f, 10U);           /* end of "beta" */
    rl_add(&f, 22U);          /* end of "delta" */
    rl_ok(&f, "ed.edit.kill.word_prev");
    rl_expect(&f, "alpha  gamma\n epsilon");
    rl_expect_kill(&f, "betadelta");
    YEW_ASSERT_EQ_U64(f.ed.win->cs.curs.len, 2U);
    YEW_ASSERT_EQ_U64(rl_pos(&f, 0U), 6U);
    YEW_ASSERT_EQ_U64(rl_pos(&f, 1U), 13U);
    rl_free(&f);
}

/* Nothing before the caret is not an error: an error status would make
 * yew_ed_invoke abort the open Insert transaction and un-type the line. */
void test_readline_kill_word_prev_at_buffer_start_is_a_no_op(void)
{
    RlFixture f;

    rl_fixture(&f, rl_words, sizeof(rl_words) - 1U);
    rl_at(&f, 0U);
    rl_ok(&f, "ed.edit.kill.word_prev");
    rl_expect(&f, "alpha beta gamma\ndelta epsilon");
    YEW_ASSERT_EQ_U64(rl_pos(&f, 0U), 0U);
    rl_free(&f);
}

/* Forward to where A-f lands: the word AND the blanks after it. */
void test_readline_kill_word_next_takes_the_word_and_its_gap(void)
{
    RlFixture f;

    rl_fixture(&f, rl_words, sizeof(rl_words) - 1U);
    rl_at(&f, 6U);
    rl_ok(&f, "ed.edit.kill.word_next");
    rl_expect(&f, "alpha gamma\ndelta epsilon");
    rl_expect_kill(&f, "beta ");
    YEW_ASSERT_EQ_U64(rl_pos(&f, 0U), 6U);
    rl_free(&f);
}

void test_readline_kill_to_home_stops_at_the_line_start(void)
{
    RlFixture f;

    rl_fixture(&f, rl_words, sizeof(rl_words) - 1U);
    rl_at(&f, 22U);
    rl_ok(&f, "ed.edit.kill.to_home");
    rl_expect(&f, "alpha beta gamma\n epsilon");
    rl_expect_kill(&f, "delta");
    YEW_ASSERT_EQ_U64(rl_pos(&f, 0U), 17U);

    /* Already at column 0: no-op, and the register keeps the last kill. */
    rl_ok(&f, "ed.edit.kill.to_home");
    rl_expect(&f, "alpha beta gamma\n epsilon");
    rl_expect_kill(&f, "delta");
    rl_free(&f);
}

/*
 * Two carets on one line both reach back to its start.  The spans overlap,
 * and the union is what both of them asked for -- dropping the second
 * would leave text a caret told the editor to remove.
 */
void test_readline_kill_to_home_merges_carets_sharing_a_line(void)
{
    RlFixture f;

    rl_fixture(&f, rl_words, sizeof(rl_words) - 1U);
    rl_at(&f, 5U);
    rl_add(&f, 10U);
    rl_ok(&f, "ed.edit.kill.to_home");
    rl_expect(&f, " gamma\ndelta epsilon");
    rl_expect_kill(&f, "alpha beta");
    YEW_ASSERT_EQ_U64(f.ed.win->cs.curs.len, 1U);
    YEW_ASSERT_EQ_U64(rl_pos(&f, 0U), 0U);
    rl_free(&f);
}

void test_readline_kill_to_end_stops_at_the_line_end(void)
{
    RlFixture f;

    rl_fixture(&f, rl_words, sizeof(rl_words) - 1U);
    rl_at(&f, 11U);
    rl_ok(&f, "ed.edit.kill.to_end");
    rl_expect(&f, "alpha beta \ndelta epsilon");
    rl_expect_kill(&f, "gamma");
    YEW_ASSERT_EQ_U64(rl_pos(&f, 0U), 11U);
    rl_free(&f);
}

/*
 * At the end of the line the terminator itself is the kill, which is what
 * makes the key join lines instead of going dead -- and what makes it
 * remove an empty line.  The LAST line has no terminator, so there it is
 * genuinely a no-op.
 */
void test_readline_kill_to_end_takes_the_line_ending_and_empty_lines(void)
{
    static const u8 text[] = "one\n\ntwo";
    RlFixture f;

    rl_fixture(&f, text, sizeof(text) - 1U);
    rl_at(&f, 4U);            /* the empty line */
    rl_ok(&f, "ed.edit.kill.to_end");
    rl_expect(&f, "one\ntwo");
    rl_expect_kill(&f, "\n");

    rl_at(&f, 3U);            /* end of "one": joins the two lines */
    rl_ok(&f, "ed.edit.kill.to_end");
    rl_expect(&f, "onetwo");

    rl_at(&f, 6U);            /* end of the final line: nothing left */
    rl_ok(&f, "ed.edit.kill.to_end");
    rl_expect(&f, "onetwo");
    rl_free(&f);
}

/* A CRLF terminator is ONE grapheme cluster, so the kill takes both bytes
 * or neither.  Half a CRLF would be invariant 2's byte confusion. */
void test_readline_kill_to_end_takes_a_whole_crlf(void)
{
    static const u8 text[] = "one\r\ntwo\r\n";
    RlFixture f;

    rl_fixture(&f, text, sizeof(text) - 1U);
    f.ed.buffer.meta.eol = YEW_EOL_CRLF;
    f.ed.buffer.meta.dominant_eol = YEW_EOL_CRLF;
    rl_at(&f, 3U);
    rl_ok(&f, "ed.edit.kill.to_end");
    rl_expect(&f, "onetwo\r\n");
    rl_expect_kill(&f, "\r\n");
    rl_free(&f);
}

/* Multi-byte clusters go whole: three-byte CJK and a combining mark that
 * belongs to the letter in front of it. */
void test_readline_kills_take_whole_graphemes(void)
{
    static const u8 text[] = "ab \xe6\x97\xa5\xe6\x9c\xac cafe\xcc\x81";
    RlFixture f;

    rl_fixture(&f, text, sizeof(text) - 1U);
    rl_at(&f, 9U);            /* past both ideographs */
    rl_ok(&f, "ed.edit.kill.word_prev");
    rl_expect(&f, "ab \xe6\x97\xa5 cafe\xcc\x81");
    rl_expect_kill(&f, "\xe6\x9c\xac");

    /* "café" written e + U+0301: one cluster, three bytes back. */
    rl_at(&f, yew_textbuf_len(f.ed.buffer.tb));
    rl_ok(&f, "ed.edit.transpose.chars");
    rl_expect(&f, "ab \xe6\x97\xa5 cae\xcc\x81" "f");
    rl_free(&f);
}

/* ----------------------------------------------------------- kill + yank */

/*
 * The round trip.  A kill lands in the unnamed register -- which is also
 * the head of the kill ring -- and the yank puts those exact bytes back.
 */
void test_readline_kill_then_yank_round_trips(void)
{
    RlFixture f;

    rl_fixture(&f, rl_words, sizeof(rl_words) - 1U);
    rl_at(&f, 10U);
    rl_ok(&f, "ed.edit.kill.word_prev");
    rl_expect(&f, "alpha  gamma\ndelta epsilon");
    rl_ok(&f, "ed.edit.kill.yank");
    rl_expect(&f, "alpha beta gamma\ndelta epsilon");
    YEW_ASSERT_EQ_U64(rl_pos(&f, 0U), 10U);
    rl_free(&f);
}

/* Every caret yanks the one value, and each lands after its own copy. */
void test_readline_yank_reaches_every_cursor(void)
{
    RlFixture f;

    rl_fixture(&f, rl_words, sizeof(rl_words) - 1U);
    rl_at(&f, 10U);
    rl_add(&f, 22U);
    rl_ok(&f, "ed.edit.kill.word_prev");
    rl_expect_kill(&f, "betadelta");
    rl_ok(&f, "ed.edit.kill.yank");
    rl_expect(&f, "alpha betadelta gamma\nbetadelta epsilon");
    YEW_ASSERT_EQ_U64(rl_pos(&f, 0U), 15U);
    rl_free(&f);
}

/* An empty kill register is a no-op, not an error. */
void test_readline_yank_without_a_kill_is_a_no_op(void)
{
    RlFixture f;

    rl_fixture(&f, rl_words, sizeof(rl_words) - 1U);
    rl_at(&f, 6U);
    rl_ok(&f, "ed.edit.kill.yank");
    rl_expect(&f, "alpha beta gamma\ndelta epsilon");
    rl_free(&f);
}

/* ------------------------------------------------------------------ undo */

/*
 * The whole fan-out is one transaction, so one undo restores every
 * cursor's text -- not one undo per cursor.
 */
void test_readline_multicursor_kill_undoes_in_one_step(void)
{
    RlFixture f;

    rl_fixture(&f, rl_words, sizeof(rl_words) - 1U);
    rl_at(&f, 10U);
    rl_add(&f, 22U);
    rl_ok(&f, "ed.edit.kill.word_prev");
    rl_expect(&f, "alpha  gamma\n epsilon");
    rl_ok(&f, "ed.edit.undo");
    rl_expect(&f, "alpha beta gamma\ndelta epsilon");
    rl_free(&f);
}

/*
 * And a kill and a yank are SEPARATE steps even inside one Insert run:
 * each readline key closes the typing transaction rather than joining it,
 * so undo walks back one deliberate operation at a time.
 */
void test_readline_multicursor_yank_undoes_in_one_step(void)
{
    RlFixture f;

    rl_fixture(&f, rl_words, sizeof(rl_words) - 1U);
    rl_at(&f, 10U);
    rl_add(&f, 22U);
    rl_ok(&f, "ed.edit.kill.word_prev");
    rl_ok(&f, "ed.edit.kill.yank");
    rl_expect(&f, "alpha betadelta gamma\nbetadelta epsilon");
    rl_ok(&f, "ed.edit.undo");
    rl_expect(&f, "alpha  gamma\n epsilon");
    rl_ok(&f, "ed.edit.undo");
    rl_expect(&f, "alpha beta gamma\ndelta epsilon");
    rl_free(&f);
}

/*
 * The reason each press is its own step: typing then killing then undoing
 * must give back the kill, not the typing.  One transaction for both
 * would make a mistyped C-k discard the sentence in front of it.
 */
void test_readline_kill_does_not_swallow_the_typing_before_it(void)
{
    RlFixture f;
    CmdId insert;
    CmdCtx cx = {0};

    rl_fixture(&f, rl_words, sizeof(rl_words) - 1U);
    rl_at(&f, 30U);
    insert = yew_cmd_lookup("ed.edit.insert.text",
                            (u32)strlen("ed.edit.insert.text"));
    YEW_ASSERT(insert.v != 0U);
    cx.ed = &f.ed;
    cx.win = f.ed.win;
    cx.count = 1U;
    cx.source = YEW_SRC_TEST;
    cx.sarg = " zeta";
    cx.sarg_len = 5U;
    YEW_ASSERT_EQ_U64(yew_ed_invoke(&f.ed, insert, &cx), YEW_CMD_OK);
    rl_expect(&f, "alpha beta gamma\ndelta epsilon zeta");

    rl_ok(&f, "ed.edit.kill.to_home");
    rl_expect(&f, "alpha beta gamma\n");
    rl_ok(&f, "ed.edit.undo");
    rl_expect(&f, "alpha beta gamma\ndelta epsilon zeta");
    rl_free(&f);
}

/* -------------------------------------------------------------- transpose */

void test_readline_transpose_chars_swaps_around_the_caret(void)
{
    static const u8 text[] = "abcd\nxy";
    RlFixture f;

    rl_fixture(&f, text, sizeof(text) - 1U);
    rl_at(&f, 2U);
    rl_ok(&f, "ed.edit.transpose.chars");
    rl_expect(&f, "acbd\nxy");
    YEW_ASSERT_EQ_U64(rl_pos(&f, 0U), 3U);
    rl_free(&f);
}

/*
 * At the end of a line there is nothing to the right, so the two
 * graphemes BEFORE the caret swap and the caret stays put.  At the start
 * of a line there is no pair at all, and the key is a no-op -- never a
 * swap across the line ending.
 */
void test_readline_transpose_chars_at_line_end_and_line_start(void)
{
    static const u8 text[] = "abcd\nxy";
    RlFixture f;

    rl_fixture(&f, text, sizeof(text) - 1U);
    rl_at(&f, 4U);
    rl_ok(&f, "ed.edit.transpose.chars");
    rl_expect(&f, "abdc\nxy");
    YEW_ASSERT_EQ_U64(rl_pos(&f, 0U), 4U);

    rl_at(&f, 5U);
    rl_ok(&f, "ed.edit.transpose.chars");
    rl_expect(&f, "abdc\nxy");
    YEW_ASSERT_EQ_U64(rl_pos(&f, 0U), 5U);
    rl_free(&f);
}

/* An empty line has no pair on it either. */
void test_readline_transpose_chars_on_an_empty_line_is_a_no_op(void)
{
    static const u8 text[] = "ab\n\ncd";
    RlFixture f;

    rl_fixture(&f, text, sizeof(text) - 1U);
    rl_at(&f, 3U);
    rl_ok(&f, "ed.edit.transpose.chars");
    rl_expect(&f, "ab\n\ncd");
    rl_free(&f);
}

void test_readline_transpose_chars_reaches_every_cursor(void)
{
    static const u8 text[] = "abcd\nwxyz";
    RlFixture f;

    rl_fixture(&f, text, sizeof(text) - 1U);
    rl_at(&f, 2U);
    rl_add(&f, 7U);
    rl_ok(&f, "ed.edit.transpose.chars");
    rl_expect(&f, "acbd\nwyxz");
    YEW_ASSERT_EQ_U64(rl_pos(&f, 0U), 3U);
    YEW_ASSERT_EQ_U64(rl_pos(&f, 1U), 8U);
    rl_free(&f);
}

/* The separator stays where it is; only the two words move. */
void test_readline_transpose_words_drags_one_word_past_the_next(void)
{
    RlFixture f;

    rl_fixture(&f, rl_words, sizeof(rl_words) - 1U);
    rl_at(&f, 6U);            /* start of "beta" */
    rl_ok(&f, "ed.edit.transpose.words");
    rl_expect(&f, "beta alpha gamma\ndelta epsilon");
    YEW_ASSERT_EQ_U64(rl_pos(&f, 0U), 10U);
    rl_free(&f);
}

/* Before the first word there is no pair, so the key does nothing. */
void test_readline_transpose_words_needs_a_word_on_each_side(void)
{
    RlFixture f;

    rl_fixture(&f, rl_words, sizeof(rl_words) - 1U);
    rl_at(&f, 0U);
    rl_ok(&f, "ed.edit.transpose.words");
    rl_expect(&f, "alpha beta gamma\ndelta epsilon");
    rl_free(&f);
}

/* ------------------------------------------------------------- word case */

/*
 * From the CARET, not from the word's start: the half already typed keeps
 * the case it was typed with, and the caret ends past the word.
 */
void test_readline_word_case_runs_from_the_caret_and_steps_past(void)
{
    static const u8 text[] = "alpha BETA gamma";
    RlFixture f;

    rl_fixture(&f, text, sizeof(text) - 1U);
    rl_at(&f, 0U);
    rl_ok(&f, "ed.edit.case.upper_word");
    rl_expect(&f, "ALPHA BETA gamma");
    YEW_ASSERT_EQ_U64(rl_pos(&f, 0U), 5U);

    rl_at(&f, 6U);
    rl_ok(&f, "ed.edit.case.lower_word");
    rl_expect(&f, "ALPHA beta gamma");
    YEW_ASSERT_EQ_U64(rl_pos(&f, 0U), 10U);

    rl_at(&f, 11U);
    rl_ok(&f, "ed.edit.case.cap_word");
    rl_expect(&f, "ALPHA beta Gamma");
    YEW_ASSERT_EQ_U64(rl_pos(&f, 0U), 16U);

    /* Mid-word: only the tail changes. */
    rl_at(&f, 13U);
    rl_ok(&f, "ed.edit.case.upper_word");
    rl_expect(&f, "ALPHA beta GaMMA");
    rl_free(&f);
}

/* On blanks the key reaches forward to the next word rather than dying. */
void test_readline_word_case_skips_blanks_to_the_next_word(void)
{
    static const u8 text[] = "alpha   beta";
    RlFixture f;

    rl_fixture(&f, text, sizeof(text) - 1U);
    rl_at(&f, 5U);
    rl_ok(&f, "ed.edit.case.upper_word");
    rl_expect(&f, "alpha   BETA");
    YEW_ASSERT_EQ_U64(rl_pos(&f, 0U), 12U);

    /* Past the last word there is nothing left to case. */
    rl_ok(&f, "ed.edit.case.upper_word");
    rl_expect(&f, "alpha   BETA");
    rl_free(&f);
}

void test_readline_word_case_reaches_every_cursor(void)
{
    RlFixture f;

    rl_fixture(&f, rl_words, sizeof(rl_words) - 1U);
    rl_at(&f, 6U);
    rl_add(&f, 17U);
    rl_ok(&f, "ed.edit.case.upper_word");
    rl_expect(&f, "alpha BETA gamma\nDELTA epsilon");
    YEW_ASSERT_EQ_U64(rl_pos(&f, 0U), 10U);
    YEW_ASSERT_EQ_U64(rl_pos(&f, 1U), 22U);
    rl_free(&f);
}
