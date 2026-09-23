/*
 * The readline/Emacs editing keys Insert mode binds.
 *
 * Every case drives a REGISTERED command through yew_ed_invoke against
 * the loaded runtime, so the transaction wrapping, the multi-cursor
 * fan-out and the yank-stack write are the ones a keystroke gets; the
 * binding tests at the bottom drive the real keys through
 * yew_ed_handle_key on top of that.
 */
#include "harness.h"

#include <string.h>

#include "edit/ed.h"
#include "edit/mode.h"
#include "text/clipboard.h"
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

/* The newest yank-stack entry -- where every kill lands (Sprint 57.28). */
static void rl_expect_kill(RlFixture *f, const char *want)
{
    const Bytebuf *value = yew_yank_at(&f->ed.yank, 0U);
    size_t len = strlen(want);

    YEW_ASSERT_NOT_NULL(value);
    YEW_ASSERT_EQ_U64(value->len, len);
    YEW_ASSERT_EQ_MEM(value->data, want, len);
}

/*
 * "alpha beta gamma\ndelta epsilon"
 *  0     6    11     16 17    23
 */
static const u8 rl_words[] = "alpha beta gamma\ndelta epsilon";

/* ---------------------------------------------------------------- kills */

/*
 * The headline key.  One press per caret, every caret, and ONE yank-stack
 * entry holding what all of them removed, in document order -- the same
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

    /* Already at column 0: no-op, and the stack keeps the last kill. */
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
 * The round trip.  A kill lands on the yank stack and the yank puts those
 * exact bytes back.
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

/* An empty yank stack is a no-op, not an error. */
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

/* ------------------------------------------------------------- the keys */

/*
 * The bindings, driven as real keys through yew_ed_handle_key against the
 * loaded runtime -- so what these pin is what a user's fingers get, not
 * what a command table says.  Modified chords carry no text: the decoder
 * reports them through emit_named, which leaves ntext zero.
 */
static Key rl_chord(u32 code, u16 mods)
{
    Key key = {0};

    key.code = code;
    key.kind = YEW_EV_KEY;
    key.ev = YEW_KEY_PRESS;
    key.mods = mods;
    return key;
}

static void rl_send(RlFixture *f, Key key)
{
    yew_ed_handle_key(&f->ed, key, 0);
    YEW_ASSERT_EQ_U64(f->ed.last_status, YEW_CMD_OK);
}

static void rl_key_runs(RlFixture *f, Key key, const char *name)
{
    rl_send(f, key);
    YEW_ASSERT_EQ_U64(f->ed.last_cmd.v,
                      yew_cmd_lookup(name, (u32)strlen(name)).v);
}

void test_readline_insert_keys_reach_their_commands(void)
{
    RlFixture f;

    rl_fixture(&f, rl_words, sizeof(rl_words) - 1U);
    YEW_ASSERT_EQ_U64(f.ed.mode, YEW_MODE_I);

    rl_at(&f, 10U);
    rl_key_runs(&f, rl_chord((u32)'w', YEW_MOD_CTRL),
                "ed.edit.kill.word_prev");
    rl_at(&f, 6U);
    rl_key_runs(&f, rl_chord(YEW_KEY_BACKSPACE, YEW_MOD_ALT),
                "ed.edit.kill.word_prev");
    rl_key_runs(&f, rl_chord((u32)'d', YEW_MOD_ALT),
                "ed.edit.kill.word_next");
    rl_key_runs(&f, rl_chord((u32)'u', YEW_MOD_CTRL),
                "ed.edit.kill.to_home");
    rl_key_runs(&f, rl_chord((u32)'k', YEW_MOD_CTRL),
                "ed.edit.kill.to_end");
    rl_key_runs(&f, rl_chord((u32)'y', YEW_MOD_CTRL),
                "ed.edit.kill.yank");
    rl_key_runs(&f, rl_chord((u32)'d', YEW_MOD_CTRL),
                "ed.edit.delete.grapheme");
    rl_key_runs(&f, rl_chord((u32)'t', YEW_MOD_CTRL),
                "ed.edit.transpose.chars");
    rl_key_runs(&f, rl_chord((u32)'t', YEW_MOD_ALT),
                "ed.edit.transpose.words");
    rl_key_runs(&f, rl_chord((u32)'u', YEW_MOD_ALT),
                "ed.edit.case.upper_word");
    rl_key_runs(&f, rl_chord((u32)'l', YEW_MOD_ALT),
                "ed.edit.case.lower_word");
    rl_key_runs(&f, rl_chord((u32)'c', YEW_MOD_ALT),
                "ed.edit.case.cap_word");
    rl_key_runs(&f, rl_chord((u32)'p', YEW_MOD_CTRL), "ed.move.line.up");
    rl_key_runs(&f, rl_chord((u32)'n', YEW_MOD_CTRL), "ed.move.line.down");

    /* Both spellings of the undo chord: 0x1F decodes as C-_ in a legacy
     * terminal, and ctrl+/ arrives as itself under the kitty protocol. */
    rl_key_runs(&f, rl_chord((u32)'_', YEW_MOD_CTRL), "ed.edit.undo");
    rl_key_runs(&f, rl_chord((u32)'/', YEW_MOD_CTRL), "ed.edit.undo");

    /* And the key signature help was moved onto. */
    YEW_ASSERT_EQ_U64(f.ed.mode, YEW_MODE_I);
    rl_free(&f);
}

/* The whole path, end to end: a real C-w removes the word. */
void test_readline_ctrl_w_kills_a_word_through_the_key(void)
{
    RlFixture f;

    rl_fixture(&f, rl_words, sizeof(rl_words) - 1U);
    rl_at(&f, 10U);
    rl_send(&f, rl_chord((u32)'w', YEW_MOD_CTRL));
    rl_expect(&f, "alpha  gamma\ndelta epsilon");
    rl_expect_kill(&f, "beta");
    rl_send(&f, rl_chord((u32)'y', YEW_MOD_CTRL));
    rl_expect(&f, "alpha beta gamma\ndelta epsilon");
    rl_free(&f);
}

/*
 * The one thing C-d must NOT do.  In a shell an empty line plus C-d is
 * EOF; here the key is bound to the forward delete and nothing else, so
 * on an empty line it removes the line's terminator and the editor stays
 * exactly where it was.
 */
void test_readline_ctrl_d_on_an_empty_line_does_not_quit(void)
{
    static const u8 text[] = "one\n\ntwo";
    RlFixture f;

    rl_fixture(&f, text, sizeof(text) - 1U);
    rl_at(&f, 4U);
    rl_send(&f, rl_chord((u32)'d', YEW_MOD_CTRL));
    YEW_ASSERT(!f.ed.quit);
    YEW_ASSERT_EQ_U64(f.ed.mode, YEW_MODE_I);
    rl_expect(&f, "one\ntwo");

    /* And again at the very end of the buffer, where there is nothing
     * left to delete at all. */
    rl_at(&f, yew_textbuf_len(f.ed.buffer.tb));
    rl_send(&f, rl_chord((u32)'d', YEW_MOD_CTRL));
    YEW_ASSERT(!f.ed.quit);
    rl_expect(&f, "one\ntwo");
    rl_free(&f);
}

/* Signature help is still reachable, on A-k. */
void test_readline_signature_help_moved_to_alt_k(void)
{
    RlFixture f;

    rl_fixture(&f, rl_words, sizeof(rl_words) - 1U);
    yew_ed_handle_key(&f.ed, rl_chord((u32)'k', YEW_MOD_ALT), 0);
    YEW_ASSERT_EQ_U64(f.ed.last_cmd.v,
                      yew_cmd_lookup("ed.lsp.signature",
                                     (u32)strlen("ed.lsp.signature")).v);
    rl_free(&f);
}

/* ------------------------------------------------ Sprint 57.28: the stack */

static void rl_snap_regval(Bytebuf *out, const RegVal *v)
{
    bytebuf_append(out, &v->type, sizeof(v->type));
    bytebuf_append(out, &v->ragged, sizeof(v->ragged));
    bytebuf_append(out, &v->width, sizeof(v->width));
    bytebuf_append(out, &v->t_wall, sizeof(v->t_wall));
    bytebuf_append(out, &v->bytes.len, sizeof(v->bytes.len));
    if (v->bytes.len != 0U)
        bytebuf_append(out, v->bytes.data, v->bytes.len);
    bytebuf_append(out, &v->rows.len, sizeof(v->rows.len));
    if (v->rows.len != 0U)
        bytebuf_append(out, v->rows.data, v->rows.len * sizeof(Span));
}

/*
 * Every byte a register holds, and the Registers struct itself (so the
 * ring's head, length and byte count, the paste-cycle state and every
 * buffer pointer are covered too).  Byte-identical before and after
 * means nothing in the register file moved.
 */
static void rl_snap_regs(Bytebuf *out, const Registers *r)
{
    size_t i;

    bytebuf_init(out);
    bytebuf_append(out, r, sizeof(*r));
    for (i = 0U; i < 26U; i++)
        rl_snap_regval(out, &r->named[i]);
    rl_snap_regval(out, &r->unnamed);
    for (i = 0U; i < 10U; i++)
        rl_snap_regval(out, &r->numbered[i]);
    rl_snap_regval(out, &r->small_del);
    rl_snap_regval(out, &r->last_insert);
    rl_snap_regval(out, &r->search);
    rl_snap_regval(out, &r->cmdline);
    rl_snap_regval(out, &r->file);
    rl_snap_regval(out, &r->alt_file);
    rl_snap_regval(out, &r->system);
    for (i = 0U; i < YEW_KILL_RING_MAX; i++)
        rl_snap_regval(out, &r->ring[i]);
}

static void rl_snap_yank(Bytebuf *out, const YewYankStack *y)
{
    u32 k;

    bytebuf_init(out);
    bytebuf_append(out, &y->len, sizeof(y->len));
    bytebuf_append(out, &y->bytes, sizeof(y->bytes));
    for (k = 0U; k < y->len; k++) {
        const Bytebuf *entry = yew_yank_at(y, k);

        bytebuf_append(out, &entry->len, sizeof(entry->len));
        bytebuf_append(out, entry->data, entry->len);
    }
}

static void rl_expect_same(const Bytebuf *a, const Bytebuf *b)
{
    YEW_ASSERT_EQ_U64(a->len, b->len);
    YEW_ASSERT_EQ_MEM(a->data, b->data, a->len);
}

/*
 * THE separation.  With clipboard.sync = all -- where every register
 * delete reaches the system clipboard (the control proves it) -- C-k,
 * C-w, A-d, C-u and C-y in Insert mode leave every register and the
 * clipboard queue byte-identical.
 */
void test_readline_kills_leave_registers_and_clipboard_untouched(void)
{
    static const u8 text[] = "keep me\nalpha beta gamma\ndelta\n";
    RlFixture f;
    Bytebuf before;
    Bytebuf after;
    RegVal seeded;

    rl_fixture(&f, text, sizeof(text) - 1U);
    YEW_ASSERT_EQ_U64(yew_mode_enter(&f.ed, YEW_MODE_L), YEW_CMD_OK);
    f.ed.regs.clipboard_sync = YEW_CLIP_SYNC_ALL;
    yew_regval_init(&seeded);
    bytebuf_append(&seeded.bytes, "named", 5U);
    yew_reg_yank(&f.ed.regs, (u8)'a', &seeded);
    yew_regval_free(&seeded);
    yew_clip_reset();
    YEW_ASSERT(!yew_clip_pending());
    /* The control: under sync = all, a register DELETE -- the entry point
     * these kills used before the yank stack -- queues a clipboard write
     * and becomes the unnamed register. */
    yew_regval_init(&seeded);
    bytebuf_append(&seeded.bytes, "gone", 4U);
    yew_reg_delete(&f.ed.regs, 0U, &seeded);
    yew_regval_free(&seeded);
    YEW_ASSERT(yew_clip_pending());
    yew_clip_reset();
    YEW_ASSERT(!yew_clip_pending());
    rl_at(&f, 0U);
    rl_send(&f, rl_chord((u32)'d', 0U));
    rl_send(&f, rl_chord((u32)'d', 0U));
    rl_expect(&f, "alpha beta gamma\ndelta\n");

    rl_snap_regs(&before, &f.ed.regs);
    YEW_ASSERT_EQ_U64(yew_mode_enter(&f.ed, YEW_MODE_I), YEW_CMD_OK);
    rl_at(&f, 11U);
    rl_send(&f, rl_chord((u32)'k', YEW_MOD_CTRL));
    rl_send(&f, rl_chord((u32)'w', YEW_MOD_CTRL));
    rl_send(&f, rl_chord(YEW_KEY_LEFT, 0U));
    rl_send(&f, rl_chord((u32)'d', YEW_MOD_ALT));
    rl_send(&f, rl_chord((u32)'u', YEW_MOD_CTRL));
    /* C-k C-w joined; the Left split; A-d C-u joined. */
    YEW_ASSERT_EQ_U64(f.ed.yank.len, 2U);
    YEW_ASSERT_EQ_U64(yew_yank_at(&f.ed.yank, 1U)->len, 10U);
    YEW_ASSERT_EQ_MEM(yew_yank_at(&f.ed.yank, 1U)->data, "beta gamma", 10U);
    rl_send(&f, rl_chord((u32)'y', YEW_MOD_CTRL));
    rl_snap_regs(&after, &f.ed.regs);
    rl_expect_same(&before, &after);
    YEW_ASSERT(!yew_clip_pending());
    YEW_ASSERT(!yew_clip_busy());
    /* The unnamed register still holds the control's value, not a kill. */
    YEW_ASSERT_EQ_U64(yew_reg_get(&f.ed.regs, (u8)'"')->bytes.len, 4U);
    YEW_ASSERT_EQ_MEM(yew_reg_get(&f.ed.regs, (u8)'"')->bytes.data,
                      "gone", 4U);
    bytebuf_free(&before);
    bytebuf_free(&after);
    rl_free(&f);
    yew_clip_reset();
}

/* And the other way: `d d` and a selection yank leave the yank stack as
 * it was, so C-y still yanks the last KILL. */
void test_readline_register_writes_leave_the_yank_stack(void)
{
    static const u8 text[] = "one two\nthree\nfour\n";
    RlFixture f;
    Bytebuf before;
    Bytebuf after;
    CmdId yank = yew_cmd_lookup("ed.sel.yank", 11U);
    CmdCtx cx = {0};

    rl_fixture(&f, text, sizeof(text) - 1U);
    rl_at(&f, 7U);
    rl_send(&f, rl_chord((u32)'w', YEW_MOD_CTRL));
    rl_expect_kill(&f, "two");
    rl_snap_yank(&before, &f.ed.yank);
    rl_send(&f, rl_chord(YEW_KEY_ESCAPE, 0U));
    rl_send(&f, rl_chord(YEW_KEY_ESCAPE, 0U));
    YEW_ASSERT_EQ_U64(f.ed.mode, YEW_MODE_L);
    rl_at(&f, 9U);
    rl_send(&f, rl_chord((u32)'d', 0U));
    rl_send(&f, rl_chord((u32)'d', 0U));
    rl_expect(&f, "one \nfour\n");
    f.ed.win->cs.curs.data[0].anchor = BYTEOFF(0U);
    f.ed.win->cs.curs.data[0].pos = BYTEOFF(3U);
    YEW_ASSERT(yank.v != 0U);
    cx.ed = &f.ed;
    cx.win = f.ed.win;
    cx.count = 1U;
    cx.source = YEW_SRC_TEST;
    (void)yew_ed_invoke(&f.ed, yank, &cx);
    rl_snap_yank(&after, &f.ed.yank);
    rl_expect_same(&before, &after);
    bytebuf_free(&before);
    bytebuf_free(&after);
    rl_free(&f);
}

/* Consecutive kills through the keys join; a motion in between splits. */
void test_readline_consecutive_kill_keys_join(void)
{
    RlFixture f;

    rl_fixture(&f, rl_words, sizeof(rl_words) - 1U);
    rl_at(&f, 16U);
    rl_send(&f, rl_chord((u32)'w', YEW_MOD_CTRL));
    rl_send(&f, rl_chord(YEW_KEY_BACKSPACE, YEW_MOD_ALT));
    rl_expect_kill(&f, "beta gamma");
    YEW_ASSERT_EQ_U64(f.ed.yank.len, 1U);
    rl_send(&f, rl_chord(YEW_KEY_LEFT, 0U));
    rl_send(&f, rl_chord((u32)'k', YEW_MOD_CTRL));
    YEW_ASSERT_EQ_U64(f.ed.yank.len, 2U);
    rl_expect_kill(&f, " ");
    rl_send(&f, rl_chord((u32)'k', YEW_MOD_CTRL));
    rl_expect_kill(&f, " \n");
    rl_expect(&f, "alphadelta epsilon");
    rl_free(&f);
}

/* Insert-mode parity: A-<del> kills the word ahead; A-y cycles. */
void test_readline_insert_alt_del_and_alt_y(void)
{
    RlFixture f;

    rl_fixture(&f, rl_words, sizeof(rl_words) - 1U);
    rl_at(&f, 6U);
    rl_key_runs(&f, rl_chord(YEW_KEY_DELETE, YEW_MOD_ALT),
                "ed.edit.kill.word_next");
    rl_expect(&f, "alpha gamma\ndelta epsilon");
    rl_expect_kill(&f, "beta ");
    rl_send(&f, rl_chord(YEW_KEY_RIGHT, 0U));
    rl_send(&f, rl_chord((u32)'d', YEW_MOD_ALT));
    rl_expect(&f, "alpha gdelta epsilon");
    rl_expect_kill(&f, "amma\n");
    rl_key_runs(&f, rl_chord((u32)'y', YEW_MOD_CTRL), "ed.edit.kill.yank");
    rl_expect(&f, "alpha gamma\ndelta epsilon");
    rl_key_runs(&f, rl_chord((u32)'y', YEW_MOD_ALT),
                "ed.edit.kill.yank_pop");
    rl_expect(&f, "alpha gbeta delta epsilon");
    rl_send(&f, rl_chord((u32)'y', YEW_MOD_ALT));
    rl_expect(&f, "alpha gamma\ndelta epsilon");
    /* One undo per yank-pop. */
    rl_ok(&f, "ed.edit.undo");
    rl_expect(&f, "alpha gbeta delta epsilon");
    rl_free(&f);
}

/* Multi-cursor: the yank lands at every caret, and A-y replaces it at
 * every caret from the spans the yank left behind. */
void test_readline_multicursor_yank_pop(void)
{
    static const u8 text[] = "ab\ncd\n";
    RlFixture f;

    rl_fixture(&f, text, sizeof(text) - 1U);
    yew_yank_kill(&f.ed.yank, (const u8 *)"OLD", 3U, YEW_KILL_FORWARD, 0U,
                  NULL);
    yew_yank_kill(&f.ed.yank, (const u8 *)"new", 3U, YEW_KILL_FORWARD, 0U,
                  NULL);
    YEW_ASSERT_EQ_U64(f.ed.yank.len, 2U);
    rl_at(&f, 1U);
    rl_add(&f, 4U);
    rl_send(&f, rl_chord((u32)'y', YEW_MOD_CTRL));
    rl_expect(&f, "anewb\ncnewd\n");
    rl_send(&f, rl_chord((u32)'y', YEW_MOD_ALT));
    rl_expect(&f, "aOLDb\ncOLDd\n");
    YEW_ASSERT_EQ_U64(rl_pos(&f, 0U), 4U);
    YEW_ASSERT_EQ_U64(rl_pos(&f, 1U), 10U);
    rl_send(&f, rl_chord((u32)'y', YEW_MOD_ALT));
    rl_expect(&f, "anewb\ncnewd\n");
    rl_ok(&f, "ed.edit.undo");
    rl_expect(&f, "aOLDb\ncOLDd\n");
    rl_free(&f);
}
