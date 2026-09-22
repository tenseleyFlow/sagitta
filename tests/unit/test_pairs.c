#include "harness.h"

#include <string.h>

#include "edit/block.h"
#include "edit/ed.h"
#include "edit/multicursor.h"
#include "edit/option.h"
#include "edit/pairs.h"
#include "syn/defs.h"
#include "syn/engine.h"
#include "syn/theme.h"
#include "text/mark.h"
#include "util/base.h"

typedef struct {
    Ed ed;
    Win win;
    SynEngine *engine;
} PairFixture;

static Cursor pair_cursor(u64 pos)
{
    Cursor cursor;

    cursor.pos = BYTEOFF(pos);
    cursor.anchor = BYTEOFF(pos);
    cursor.goal_col = (CCol){0U};
    return cursor;
}

static void pair_init(PairFixture *fx, const char *text)
{
    (void)memset(fx, 0, sizeof(*fx));
    yew_syn_buf_init(&fx->ed.buffer.syn);
    fx->ed.buffer.tb = yew_textbuf_from_bytes((const u8 *)text,
                                              (u64)strlen(text));
    fx->ed.buffer.tabwidth = 4U;
    fx->ed.buffer.undo = yew_undo_new(fx->ed.buffer.tb);
    fx->ed.buffer.marks = yew_marks_new();
    yew_timers_init(&fx->ed.timers);
    fx->win.buf = &fx->ed.buffer;
    yew_cset_init(&fx->win.cs, pair_cursor(0U));
    fx->ed.win = &fx->win;
    fx->ed.model_ready = true;
}

/* Binds a real language so the syntax query can answer; `settle` decides
 * whether the entry states the query needs actually exist. */
static void pair_init_lang(PairFixture *fx, const char *text,
                           const char *lang, bool settle)
{
    u32 lang_id = yew_syn_lang_named(lang);
    const SynDef *def;

    pair_init(fx, text);
    YEW_ASSERT(lang_id != YEW_LANG_NONE);
    def = yew_syn_def_for(lang_id);
    YEW_ASSERT_NOT_NULL(def);
    fx->engine = yew_syn_engine_new((SynDef *)def);
    YEW_ASSERT_NOT_NULL(fx->engine);
    fx->ed.buffer.lang = lang;
    yew_syn_buf_bind(&fx->ed.buffer.syn, fx->engine);
    yew_syn_attach(&fx->ed.buffer.syn, lang_id, fx->ed.buffer.tb);
    if (settle) {
        SynSettleReport report;
        u64 lines = yew_textbuf_line_count(fx->ed.buffer.tb);

        yew_syn_settle(&fx->ed.buffer.syn, fx->ed.buffer.tb, LINENO(0U),
                       LINENO(lines), INT64_MAX, &report);
        YEW_ASSERT(report.fixpoint);
    }
}

static void pair_free(PairFixture *fx)
{
    yew_msg_clear(&fx->ed);
    yew_timers_free(&fx->ed.timers);
    yew_cset_free(&fx->win.cs);
    yew_syn_detach(&fx->ed.buffer.syn);
    if (fx->engine != NULL)
        yew_syn_engine_free(fx->engine);
    yew_pairs_clear(&fx->ed.buffer);
    yew_opt_scope_free(&fx->ed.buffer.opt_overrides);
    yew_opt_scope_free(&fx->win.opt_overrides);
    yew_theme_free(&fx->ed.theme);
    yew_xfree(fx->ed.theme_last_dark);
    yew_xfree(fx->ed.theme_last_light);
    yew_marks_free(fx->ed.buffer.marks);
    yew_undo_free(fx->ed.buffer.undo);
    yew_textbuf_free(fx->ed.buffer.tb);
}

static void pair_type(PairFixture *fx, const char *bytes)
{
    CmdId id = yew_cmd_lookup("ed.edit.insert.text", 19U);
    CmdCtx cx = {0};

    YEW_ASSERT(id.v != 0U);
    cx.win = &fx->win;
    cx.count = 1U;
    cx.sarg = bytes;
    cx.sarg_len = (u32)strlen(bytes);
    cx.source = YEW_SRC_TEST;
    YEW_ASSERT_EQ_I64(yew_ed_invoke(&fx->ed, id, &cx), YEW_CMD_OK);
}

static void pair_backspace(PairFixture *fx)
{
    CmdId id = yew_cmd_lookup("ed.edit.delete.grapheme_left", 28U);
    CmdCtx cx = {0};

    YEW_ASSERT(id.v != 0U);
    cx.win = &fx->win;
    cx.count = 1U;
    cx.source = YEW_SRC_TEST;
    YEW_ASSERT_EQ_I64(yew_ed_invoke(&fx->ed, id, &cx), YEW_CMD_OK);
}

static void pair_delete_forward(PairFixture *fx)
{
    CmdId id = yew_cmd_lookup("ed.edit.delete.grapheme", 23U);
    CmdCtx cx = {0};

    YEW_ASSERT(id.v != 0U);
    cx.win = &fx->win;
    cx.count = 1U;
    cx.source = YEW_SRC_TEST;
    YEW_ASSERT_EQ_I64(yew_ed_invoke(&fx->ed, id, &cx), YEW_CMD_OK);
}

static void pair_assert_text(const PairFixture *fx, const char *want)
{
    TextIter it;
    u64 want_len = (u64)strlen(want);
    u64 done = 0U;

    YEW_ASSERT_EQ_U64(yew_textbuf_len(fx->ed.buffer.tb), want_len);
    if (want_len == 0U)
        return;
    YEW_ASSERT(yew_textiter_begin(&it, fx->ed.buffer.tb, BYTEOFF(0U)));
    while (done < want_len) {
        const u8 *bytes;
        u64 len;
        u64 take;

        YEW_ASSERT(yew_textiter_chunk(&it, fx->ed.buffer.tb, &bytes, &len));
        take = len < want_len - done ? len : want_len - done;
        YEW_ASSERT_EQ_MEM(bytes, want + done, take);
        done += take;
        if (done < want_len)
            YEW_ASSERT(yew_textiter_advance(&it, fx->ed.buffer.tb));
    }
}

static u64 pair_caret(const PairFixture *fx)
{
    return fx->win.cs.curs.data[fx->win.cs.primary].pos.v;
}

static void pair_place(PairFixture *fx, u64 off)
{
    fx->win.cs.curs.data[fx->win.cs.primary] = pair_cursor(off);
}

static void pair_set_bool(PairFixture *fx, const char *name, bool on)
{
    OptVal value = {(u8)YEW_OPT_BOOL, {0}};
    const char *err = NULL;

    value.as.b = on;
    yew_opt_init(&fx->ed);
    YEW_ASSERT(yew_opt_set_for(&fx->ed, &fx->ed.buffer, NULL,
                               YEW_OPT_SCOPE_DECLARED, name,
                               (u32)strlen(name), &value, &err));
    YEW_ASSERT_NULL(err);
}

void test_pairs_open_inserts_the_closer_and_lands_between_them(void)
{
    PairFixture fx;

    pair_init(&fx, "");
    pair_type(&fx, "(");
    pair_assert_text(&fx, "()");
    YEW_ASSERT_EQ_U64(pair_caret(&fx), 1U);
    /* One insert of both bytes, so one op. */
    YEW_ASSERT_EQ_U64(fx.ed.buffer.undo->nodes.len, 2U);
    YEW_ASSERT_EQ_U64(fx.ed.buffer.undo->nodes.data[1].n_ops, 1U);

    /* Typing the closer advances over it and makes no edit at all. */
    pair_type(&fx, ")");
    pair_assert_text(&fx, "()");
    YEW_ASSERT_EQ_U64(pair_caret(&fx), 2U);
    YEW_ASSERT_EQ_U64(fx.ed.buffer.undo->nodes.len, 2U);

    /* The memory is gone, so a second closer is ordinary text. */
    pair_type(&fx, ")");
    pair_assert_text(&fx, "())");
    YEW_ASSERT_EQ_U64(pair_caret(&fx), 3U);

    pair_type(&fx, "[");
    pair_assert_text(&fx, "())[]");
    pair_type(&fx, "{");
    pair_assert_text(&fx, "())[{}]");
    YEW_ASSERT_EQ_U64(pair_caret(&fx), 5U);
    pair_free(&fx);
}

void test_pairs_type_over_survives_editing_between_the_delimiters(void)
{
    PairFixture fx;

    pair_init(&fx, "");
    pair_type(&fx, "(");
    pair_type(&fx, "a");
    pair_type(&fx, "b");
    pair_type(&fx, "c");
    pair_assert_text(&fx, "(abc)");
    YEW_ASSERT_EQ_U64(pair_caret(&fx), 4U);
    /* The remembered closer is a Mark, so the edits between the
     * delimiters moved it with the text. */
    pair_type(&fx, ")");
    pair_assert_text(&fx, "(abc)");
    YEW_ASSERT_EQ_U64(pair_caret(&fx), 5U);

    /* Deleting between them works the same way. */
    pair_free(&fx);
    pair_init(&fx, "");
    pair_type(&fx, "(");
    pair_type(&fx, "x");
    pair_type(&fx, "y");
    pair_backspace(&fx);
    pair_assert_text(&fx, "(x)");
    YEW_ASSERT_EQ_U64(pair_caret(&fx), 2U);
    pair_type(&fx, ")");
    pair_assert_text(&fx, "(x)");
    YEW_ASSERT_EQ_U64(pair_caret(&fx), 3U);
    pair_free(&fx);
}

void test_pairs_are_suppressed_inside_a_settled_string_or_comment(void)
{
    static const char text[] =
        "const char *s = \"aa\";\n"
        "/* cc */\n"
        "int n;\n";
    PairFixture fx;

    pair_init_lang(&fx, text, "c", true);
    /* Inside the string literal. */
    pair_place(&fx, 18U);
    pair_type(&fx, "(");
    pair_assert_text(&fx,
                     "const char *s = \"a(a\";\n"
                     "/* cc */\n"
                     "int n;\n");
    YEW_ASSERT_EQ_U64(pair_caret(&fx), 19U);
    pair_free(&fx);

    pair_init_lang(&fx, text, "c", true);
    /* Inside the block comment. */
    pair_place(&fx, 26U);
    pair_type(&fx, "[");
    pair_assert_text(&fx,
                     "const char *s = \"aa\";\n"
                     "/* c[c */\n"
                     "int n;\n");
    pair_free(&fx);

    /* Ordinary code on the same buffer still pairs. */
    pair_init_lang(&fx, text, "c", true);
    pair_place(&fx, 34U);
    pair_type(&fx, "(");
    pair_assert_text(&fx,
                     "const char *s = \"aa\";\n"
                     "/* cc */\n"
                     "int() n;\n");
    pair_free(&fx);
}

void test_pairs_fail_open_on_an_unsettled_line(void)
{
    static const char text[] = "int n;\nconst char *s = \"aa\";\n";
    PairFixture fx;

    /*
     * Attaching settles line 0 only, so line 1's entry state does not
     * exist and the query cannot answer.  It returns false, and fail-open
     * means the pair IS inserted even though the caret is inside a string:
     * pairing in unsettled text is a cosmetic annoyance, refusing to type
     * is not.
     */
    pair_init_lang(&fx, text, "c", false);
    YEW_ASSERT_EQ_U64(fx.ed.buffer.syn.settled_to.v, 1U);
    pair_place(&fx, 25U);
    pair_type(&fx, "(");
    pair_assert_text(&fx, "int n;\nconst char *s = \"a()a\";\n");
    YEW_ASSERT_EQ_U64(pair_caret(&fx), 26U);
    pair_free(&fx);

    /* Once the wave reaches that line the same keystroke is literal. */
    pair_init_lang(&fx, text, "c", true);
    pair_place(&fx, 25U);
    pair_type(&fx, "(");
    pair_assert_text(&fx, "int n;\nconst char *s = \"a(a\";\n");
    pair_free(&fx);
}

void test_pairs_quote_after_a_word_byte_stays_literal(void)
{
    PairFixture fx;

    pair_init(&fx, "");
    pair_type(&fx, "'");
    pair_assert_text(&fx, "''");
    YEW_ASSERT_EQ_U64(pair_caret(&fx), 1U);
    pair_free(&fx);

    pair_init(&fx, "don");
    pair_place(&fx, 3U);
    pair_type(&fx, "'");
    pair_assert_text(&fx, "don'");
    YEW_ASSERT_EQ_U64(pair_caret(&fx), 4U);
    pair_type(&fx, "t");
    pair_assert_text(&fx, "don't");
    pair_free(&fx);

    /* A double quote after a word byte is suppressed for the same reason. */
    pair_init(&fx, "x");
    pair_place(&fx, 1U);
    pair_type(&fx, "\"");
    pair_assert_text(&fx, "x\"");
    pair_free(&fx);
}

void test_pairs_language_table_adds_and_removes_quote_pairs(void)
{
    PairFixture fx;

    /* Rust: a lone apostrophe is a lifetime, so it never closes. */
    pair_init_lang(&fx, "\n", "rust", true);
    pair_place(&fx, 0U);
    pair_type(&fx, "'");
    pair_assert_text(&fx, "'\n");
    pair_type(&fx, "(");
    pair_assert_text(&fx, "'()\n");
    pair_free(&fx);

    /* Markdown: the backtick is a real delimiter and does close. */
    pair_init_lang(&fx, "\n", "markdown", true);
    pair_place(&fx, 0U);
    pair_type(&fx, "`");
    pair_assert_text(&fx, "``\n");
    YEW_ASSERT_EQ_U64(pair_caret(&fx), 1U);
    pair_type(&fx, "`");
    pair_assert_text(&fx, "``\n");
    YEW_ASSERT_EQ_U64(pair_caret(&fx), 2U);
    pair_free(&fx);

    /* C leaves the backtick alone. */
    pair_init_lang(&fx, "\n", "c", true);
    pair_place(&fx, 0U);
    pair_type(&fx, "`");
    pair_assert_text(&fx, "`\n");
    pair_free(&fx);
}

void test_pairs_stack_overflow_drops_the_oldest_entry(void)
{
    PairFixture fx;
    char want[80];
    u32 i;

    pair_init(&fx, "");
    for (i = 0U; i < (u32)YEW_PAIR_STACK_MAX + 1U; i++)
        pair_type(&fx, "(");
    for (i = 0U; i < (u32)YEW_PAIR_STACK_MAX + 1U; i++)
        want[i] = '(';
    for (i = 0U; i < (u32)YEW_PAIR_STACK_MAX + 1U; i++)
        want[(u32)YEW_PAIR_STACK_MAX + 1U + i] = ')';
    want[2U * ((u32)YEW_PAIR_STACK_MAX + 1U)] = '\0';
    pair_assert_text(&fx, want);
    YEW_ASSERT_EQ_U64(pair_caret(&fx), (u64)YEW_PAIR_STACK_MAX + 1U);

    /* Every entry still remembered skips. */
    for (i = 0U; i < (u32)YEW_PAIR_STACK_MAX; i++)
        pair_type(&fx, ")");
    pair_assert_text(&fx, want);
    YEW_ASSERT_EQ_U64(pair_caret(&fx),
                      2U * (u64)YEW_PAIR_STACK_MAX + 1U);

    /* The oldest was dropped when the stack filled, so the outermost
     * closer is ordinary text and typing one inserts one. */
    pair_type(&fx, ")");
    YEW_ASSERT_EQ_U64(yew_textbuf_len(fx.ed.buffer.tb),
                      2U * ((u64)YEW_PAIR_STACK_MAX + 1U) + 1U);
    pair_free(&fx);
}

void test_pairs_backspace_between_a_fresh_pair_takes_both(void)
{
    PairFixture fx;

    pair_init(&fx, "");
    pair_type(&fx, "(");
    pair_backspace(&fx);
    pair_assert_text(&fx, "");
    YEW_ASSERT_EQ_U64(pair_caret(&fx), 0U);

    /* Once the pair holds text it is no longer fresh: Backspace takes one
     * grapheme. */
    pair_free(&fx);
    pair_init(&fx, "");
    pair_type(&fx, "(");
    pair_type(&fx, "z");
    pair_backspace(&fx);
    pair_assert_text(&fx, "()");
    YEW_ASSERT_EQ_U64(pair_caret(&fx), 1U);
    /* And now it is empty again, so the next Backspace takes both. */
    pair_backspace(&fx);
    pair_assert_text(&fx, "");
    pair_free(&fx);
}

void test_pairs_multicursor_pairing_is_one_multi_transaction(void)
{
    PairFixture fx;
    const UndoNode *node;

    pair_init(&fx, "ab");
    YEW_ASSERT(yew_cset_add(&fx.win.cs, pair_cursor(2U)));
    pair_type(&fx, "(");
    pair_assert_text(&fx, "()ab()");
    YEW_ASSERT_EQ_U64(fx.win.cs.curs.len, 2U);
    YEW_ASSERT_EQ_U64(fx.win.cs.curs.data[0].pos.v, 1U);
    YEW_ASSERT_EQ_U64(fx.win.cs.curs.data[1].pos.v, 5U);
    YEW_ASSERT_EQ_U64(fx.ed.buffer.undo->nodes.len, 2U);
    node = &fx.ed.buffer.undo->nodes.data[1];
    YEW_ASSERT_EQ_U64(node->reason, YEW_TXN_MULTI);
    YEW_ASSERT_EQ_U64(node->n_ops, 2U);

    /* Both cursors type over their own remembered closer. */
    pair_type(&fx, ")");
    pair_assert_text(&fx, "()ab()");
    YEW_ASSERT_EQ_U64(fx.win.cs.curs.data[0].pos.v, 2U);
    YEW_ASSERT_EQ_U64(fx.win.cs.curs.data[1].pos.v, 6U);
    pair_free(&fx);
}

void test_pairs_autopair_off_inserts_one_literal_byte(void)
{
    PairFixture fx;

    pair_init(&fx, "");
    pair_set_bool(&fx, "autopair", false);
    pair_type(&fx, "(");
    pair_assert_text(&fx, "(");
    YEW_ASSERT_EQ_U64(pair_caret(&fx), 1U);
    pair_type(&fx, ")");
    pair_assert_text(&fx, "()");
    YEW_ASSERT_EQ_U64(pair_caret(&fx), 2U);
    pair_backspace(&fx);
    pair_assert_text(&fx, "(");
    yew_opt_free(&fx.ed);
    pair_free(&fx);
}

void test_pairs_ordinary_characters_never_reach_the_syntax_query(void)
{
    static const char letters[] = "abcdefghijklmnopqrstuvwxyz";
    PairFixture fx;
    char one[2] = {0, 0};
    u32 i;
    u32 openers = 0U;

    /* The query heap-allocates and re-lexes the caret's line.  It may be
     * asked when an opener is typed and at no other time. */
    pair_init_lang(&fx, "int main(void)\n", "c", true);
    pair_place(&fx, 15U);
    yew_syn_in_string_or_comment_calls_reset();
    for (i = 0U; i < 200U; i++) {
        if (i % 10U == 0U) {
            openers++;
            pair_type(&fx, "(");
        } else if (i % 10U == 1U) {
            pair_type(&fx, ")");
        } else {
            one[0] = letters[i % (u32)(sizeof(letters) - 1U)];
            pair_type(&fx, one);
        }
    }
    YEW_ASSERT_EQ_U64(yew_syn_in_string_or_comment_calls(), (u64)openers);
    YEW_ASSERT_EQ_U64(openers, 20U);
    pair_free(&fx);
}

void test_pairs_a_clamped_mark_never_skips_someone_elses_byte(void)
{
    PairFixture fx;

    /*
     * A mark inside a deleted range clamps to the deletion point rather
     * than dying (text/mark.c adjust_delete), so a remembered closer can
     * end up naming a byte that is not that closer — or no byte at all.
     * Skipping there would move the caret over text the user never asked
     * to pass, which is byte confusion.  The entry must be retired and the
     * keystroke must insert.
     */
    pair_init(&fx, "");
    pair_type(&fx, "(");
    pair_assert_text(&fx, "()");
    pair_delete_forward(&fx);
    pair_assert_text(&fx, "(");
    YEW_ASSERT_EQ_U64(pair_caret(&fx), 1U);
    pair_type(&fx, ")");
    pair_assert_text(&fx, "()");
    YEW_ASSERT_EQ_U64(pair_caret(&fx), 2U);
    pair_free(&fx);

    /* Same shape with other text under the clamped mark. */
    pair_init(&fx, "zz");
    pair_place(&fx, 0U);
    pair_type(&fx, "[");
    pair_assert_text(&fx, "[]zz");
    pair_delete_forward(&fx);
    pair_assert_text(&fx, "[zz");
    YEW_ASSERT_EQ_U64(pair_caret(&fx), 1U);
    pair_type(&fx, "]");
    pair_assert_text(&fx, "[]zz");
    YEW_ASSERT_EQ_U64(pair_caret(&fx), 2U);
    pair_free(&fx);
}
