#define _POSIX_C_SOURCE 200809L

#include "harness.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "text/register.h"

typedef struct PasteFixture {
    Registers regs;
    TextBuf *tb;
    CursorSet cursors;
    UndoTree *undo;
    FileMeta meta;
    EditCtx edit;
} PasteFixture;

static Cursor paste_cursor(u64 pos)
{
    Cursor c;
    c.pos = BYTEOFF(pos);
    c.anchor = BYTEOFF(pos);
    c.goal_col = (GCol){0U};
    return c;
}

static void paste_fixture_init(PasteFixture *f, const char *text, u64 cursor)
{
    yew_reg_init(&f->regs);
    f->tb = yew_textbuf_from_bytes((const u8 *)text, strlen(text));
    yew_cset_init(&f->cursors, paste_cursor(cursor));
    f->undo = yew_undo_new(f->tb);
    yew_filemeta_init(&f->meta);
    f->edit = (EditCtx){f->tb, NULL, &f->cursors, 1U, NULL, f->undo,
                       NULL, NULL, NULL, 0};
}

static void paste_fixture_enable_meta(PasteFixture *f, YewEol eol)
{
    static const char path[] = "/tmp/yew-s12-register-test";
    f->meta.realpath = yew_xmalloc(sizeof(path));
    (void)memcpy(f->meta.realpath, path, sizeof(path));
    f->meta.eol = eol;
    f->meta.dominant_eol = eol;
    f->edit.meta = &f->meta;
}

static void paste_fixture_free(PasteFixture *f)
{
    if (f->edit.jrnl != NULL)
        yew_journal_discard(f->edit.jrnl);
    yew_filemeta_dispose(&f->meta);
    yew_undo_free(f->undo);
    yew_cset_free(&f->cursors);
    yew_textbuf_free(f->tb);
    yew_reg_free(&f->regs);
}

static void paste_set(RegVal *v, RegType type, const char *text)
{
    yew_regval_init(v);
    v->type = (u8)type;
    bytebuf_append(&v->bytes, text, strlen(text));
}

static void paste_materialize(const TextBuf *tb, Bytebuf *out)
{
    TextIter it;
    u64 done = 0U;
    u64 total = yew_textbuf_len(tb);
    out->len = 0U;
    if (total == 0U)
        return;
    YEW_ASSERT(yew_textiter_begin(&it, tb, BYTEOFF(0U)));
    while (done < total) {
        const u8 *bytes;
        u64 len;
        YEW_ASSERT(yew_textiter_chunk(&it, tb, &bytes, &len));
        bytebuf_append(out, bytes, (size_t)len);
        done += len;
        if (done < total)
            YEW_ASSERT(yew_textiter_advance(&it, tb));
    }
}

static void paste_assert_text(const TextBuf *tb, const char *want)
{
    Bytebuf got;
    bytebuf_init(&got);
    paste_materialize(tb, &got);
    YEW_ASSERT_EQ_U64(got.len, strlen(want));
    YEW_ASSERT_EQ_MEM(got.data, want, got.len);
    bytebuf_free(&got);
}

static void paste_install(Registers *r, const RegVal *v)
{
    yew_regval_copy(&r->unnamed, v);
}

void test_paste_char_before_and_after_are_one_undo_node(void)
{
    PasteFixture before;
    PasteFixture after;
    RegVal v;

    paste_set(&v, YEW_REG_CHARWISE, "XY");
    paste_fixture_init(&before, "abc\n", 1U);
    paste_install(&before.regs, &v);
    YEW_ASSERT(yew_reg_paste(&before.regs, &before.edit, '"', true, 8U));
    paste_assert_text(before.tb, "aXYbc\n");
    YEW_ASSERT_EQ_U64(before.cursors.curs.data[0].pos.v, 2U);
    YEW_ASSERT_EQ_U64(before.undo->nodes.len, 2U);
    YEW_ASSERT(yew_undo(&before.edit));
    paste_assert_text(before.tb, "abc\n");
    YEW_ASSERT_EQ_U64(before.cursors.curs.data[0].pos.v, 1U);

    paste_fixture_init(&after, "abc\n", 1U);
    paste_install(&after.regs, &v);
    YEW_ASSERT(yew_reg_paste(&after.regs, &after.edit, '"', false, 8U));
    paste_assert_text(after.tb, "abXYc\n");
    YEW_ASSERT_EQ_U64(after.cursors.curs.data[0].pos.v, 3U);
    YEW_ASSERT_EQ_U64(after.undo->nodes.len, 2U);
    paste_fixture_free(&after);
    paste_fixture_free(&before);
    yew_regval_free(&v);
}

void test_paste_char_after_eol_stays_before_eol(void)
{
    PasteFixture f;
    RegVal v;
    paste_set(&v, YEW_REG_CHARWISE, "Z");
    paste_fixture_init(&f, "a\nb", 0U);
    paste_install(&f.regs, &v);
    YEW_ASSERT(yew_reg_paste(&f.regs, &f.edit, '"', false, 8U));
    paste_assert_text(f.tb, "aZ\nb");
    YEW_ASSERT_EQ_U64(f.cursors.curs.data[0].pos.v, 1U);
    paste_fixture_free(&f);
    yew_regval_free(&v);
}

void test_paste_line_before_first_and_middle(void)
{
    PasteFixture first;
    PasteFixture middle;
    RegVal v;
    paste_set(&v, YEW_REG_LINEWISE, "  new\n");

    paste_fixture_init(&first, "one\ntwo\n", 0U);
    paste_install(&first.regs, &v);
    YEW_ASSERT(yew_reg_paste(&first.regs, &first.edit, '"', true, 8U));
    paste_assert_text(first.tb, "  new\none\ntwo\n");
    YEW_ASSERT_EQ_U64(first.cursors.curs.data[0].pos.v, 2U);

    paste_fixture_init(&middle, "one\ntwo\nthree\n", 5U);
    paste_install(&middle.regs, &v);
    YEW_ASSERT(yew_reg_paste(&middle.regs, &middle.edit, '"', false, 8U));
    paste_assert_text(middle.tb, "one\ntwo\n  new\nthree\n");
    YEW_ASSERT_EQ_U64(middle.cursors.curs.data[0].pos.v, 10U);
    YEW_ASSERT_EQ_U64(middle.undo->nodes.len, 2U);
    paste_fixture_free(&middle);
    paste_fixture_free(&first);
    yew_regval_free(&v);
}

void test_paste_line_after_missing_final_newline_uses_destination_eol(void)
{
    PasteFixture failed;
    PasteFixture f;
    RegVal v;
    char root[] = "/tmp/yew-paste-line-XXXXXX";
    char blocker[128];
    char journal_dir[128];
    char yew_dir[128];
    const char *saved_state = getenv("XDG_STATE_HOME");
    char *saved_copy = NULL;
    FILE *fp;
    int n;

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

    paste_set(&v, YEW_REG_LINEWISE, "new\n");
    paste_fixture_init(&failed, "last", 2U);
    paste_fixture_enable_meta(&failed, YEW_EOL_CRLF);
    paste_install(&failed.regs, &v);
    YEW_ASSERT_EQ_I64(setenv("XDG_STATE_HOME", blocker, 1), 0);
    YEW_ASSERT(!yew_reg_paste(&failed.regs, &failed.edit, '"', false, 8U));
    paste_assert_text(failed.tb, "last");
    YEW_ASSERT_EQ_U64(failed.cursors.curs.data[0].pos.v, 2U);
    YEW_ASSERT_EQ_U64(failed.undo->nodes.len, 1U);
    paste_fixture_free(&failed);

    YEW_ASSERT_EQ_I64(setenv("XDG_STATE_HOME", root, 1), 0);
    paste_fixture_init(&f, "last", 2U);
    paste_fixture_enable_meta(&f, YEW_EOL_CRLF);
    paste_install(&f.regs, &v);
    YEW_ASSERT(yew_reg_paste(&f.regs, &f.edit, '"', false, 8U));
    paste_assert_text(f.tb, "last\r\nnew");
    YEW_ASSERT_EQ_U64(f.cursors.curs.data[0].pos.v, 6U);
    YEW_ASSERT_EQ_U64(f.undo->nodes.len, 2U);
    YEW_ASSERT(yew_undo(&f.edit));
    paste_assert_text(f.tb, "last");
    YEW_ASSERT_EQ_U64(f.cursors.curs.data[0].pos.v, 2U);
    paste_fixture_free(&f);
    yew_regval_free(&v);

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
    YEW_ASSERT_EQ_I64(unlink(blocker), 0);
    YEW_ASSERT_EQ_I64(rmdir(journal_dir), 0);
    YEW_ASSERT_EQ_I64(rmdir(yew_dir), 0);
    YEW_ASSERT_EQ_I64(rmdir(root), 0);
}

static void paste_block_value(RegVal *v, const char *bytes,
                              const Span *rows, size_t count, u32 width,
                              bool ragged)
{
    size_t i;
    paste_set(v, YEW_REG_BLOCKWISE, bytes);
    for (i = 0U; i < count; i++)
        YewRegRowVec_push(&v->rows, rows[i]);
    v->width = width;
    v->ragged = ragged;
}

void test_paste_journal_failure_stops_char_and_block(void)
{
    static const Span direct_row[] = {{0U, 1U}};
    static const Span deferred_rows[] = {{0U, 0U}, {0U, 1U}};
    PasteFixture character;
    PasteFixture direct;
    PasteFixture padded;
    PasteFixture extended;
    RegVal char_value;
    RegVal direct_value;
    RegVal deferred_value;
    char root[] = "/tmp/yew-paste-failure-XXXXXX";
    char blocker[128];
    const char *saved_state = getenv("XDG_STATE_HOME");
    char *saved_copy = NULL;
    FILE *fp;
    int n;

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

    paste_set(&char_value, YEW_REG_CHARWISE, "Z");
    paste_fixture_init(&character, "abc", 1U);
    paste_fixture_enable_meta(&character, YEW_EOL_LF);
    paste_install(&character.regs, &char_value);
    YEW_ASSERT(!yew_reg_paste(&character.regs, &character.edit, '"',
                              false, 8U));
    paste_assert_text(character.tb, "abc");
    YEW_ASSERT_EQ_U64(character.cursors.curs.data[0].pos.v, 1U);
    YEW_ASSERT_EQ_U64(character.undo->nodes.len, 1U);
    YEW_ASSERT_EQ_U64(character.regs.paste_spans.len, 0U);
    paste_fixture_free(&character);
    yew_regval_free(&char_value);

    paste_block_value(&direct_value, "X", direct_row, 1U, 1U, false);
    paste_fixture_init(&direct, "abc", 1U);
    paste_fixture_enable_meta(&direct, YEW_EOL_LF);
    paste_install(&direct.regs, &direct_value);
    YEW_ASSERT(!yew_reg_paste(&direct.regs, &direct.edit, '"', true, 8U));
    paste_assert_text(direct.tb, "abc");
    YEW_ASSERT_EQ_U64(direct.cursors.curs.data[0].pos.v, 1U);
    YEW_ASSERT_EQ_U64(direct.undo->nodes.len, 1U);
    YEW_ASSERT_EQ_U64(direct.regs.paste_spans.len, 0U);
    paste_fixture_free(&direct);
    yew_regval_free(&direct_value);

    paste_block_value(&deferred_value, "Y", deferred_rows, 2U, 1U,
                      true);
    paste_fixture_init(&padded, "abcd\nx", 3U);
    paste_fixture_enable_meta(&padded, YEW_EOL_LF);
    paste_install(&padded.regs, &deferred_value);
    YEW_ASSERT(!yew_reg_paste(&padded.regs, &padded.edit, '"', true, 8U));
    paste_assert_text(padded.tb, "abcd\nx");
    YEW_ASSERT_EQ_U64(padded.cursors.curs.data[0].pos.v, 3U);
    YEW_ASSERT_EQ_U64(padded.undo->nodes.len, 1U);
    YEW_ASSERT_EQ_U64(padded.regs.paste_spans.len, 0U);
    paste_fixture_free(&padded);

    paste_fixture_init(&extended, "a", 0U);
    paste_fixture_enable_meta(&extended, YEW_EOL_LF);
    paste_install(&extended.regs, &deferred_value);
    YEW_ASSERT(!yew_reg_paste(&extended.regs, &extended.edit, '"', true,
                              8U));
    paste_assert_text(extended.tb, "a");
    YEW_ASSERT_EQ_U64(extended.cursors.curs.data[0].pos.v, 0U);
    YEW_ASSERT_EQ_U64(extended.undo->nodes.len, 1U);
    YEW_ASSERT_EQ_U64(extended.regs.paste_spans.len, 0U);
    paste_fixture_free(&extended);
    yew_regval_free(&deferred_value);

    if (saved_copy != NULL) {
        YEW_ASSERT_EQ_I64(setenv("XDG_STATE_HOME", saved_copy, 1), 0);
    } else {
        YEW_ASSERT_EQ_I64(unsetenv("XDG_STATE_HOME"), 0);
    }
    free(saved_copy);
    YEW_ASSERT_EQ_I64(unlink(blocker), 0);
    YEW_ASSERT_EQ_I64(rmdir(root), 0);
}

void test_paste_block_short_lines_and_extension(void)
{
    static const Span rows[] = {{0U, 1U}, {1U, 2U}, {2U, 3U}};
    PasteFixture f;
    RegVal v;
    char root[] = "/tmp/yew-paste-block-XXXXXX";
    char journal_dir[128];
    char yew_dir[128];
    const char *saved_state = getenv("XDG_STATE_HOME");
    char *saved_copy = NULL;
    int n;

    if (saved_state != NULL) {
        size_t saved_len = strlen(saved_state) + 1U;

        saved_copy = yew_xmalloc(saved_len);
        (void)memcpy(saved_copy, saved_state, saved_len);
    }
    YEW_ASSERT_NOT_NULL(mkdtemp(root));
    YEW_ASSERT_EQ_I64(setenv("XDG_STATE_HOME", root, 1), 0);
    paste_block_value(&v, "XYZ", rows, YEW_ARRAY_LEN(rows), 1U, false);
    paste_fixture_init(&f, "abcd\na", 3U);
    paste_fixture_enable_meta(&f, YEW_EOL_LF);
    paste_install(&f.regs, &v);
    YEW_ASSERT(yew_reg_paste(&f.regs, &f.edit, '"', true, 8U));
    paste_assert_text(f.tb, "abcXd\na  Y\n   Z");
    YEW_ASSERT_EQ_U64(f.cursors.curs.data[0].pos.v, 3U);
    YEW_ASSERT_EQ_U64(f.undo->nodes.len, 2U);
    YEW_ASSERT(yew_undo(&f.edit));
    paste_assert_text(f.tb, "abcd\na");
    YEW_ASSERT_EQ_U64(f.cursors.curs.data[0].pos.v, 3U);
    paste_fixture_free(&f);
    yew_regval_free(&v);

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
    YEW_ASSERT_EQ_I64(rmdir(journal_dir), 0);
    YEW_ASSERT_EQ_I64(rmdir(yew_dir), 0);
    YEW_ASSERT_EQ_I64(rmdir(root), 0);
}

void test_paste_block_tab_round_left_adds_spaces(void)
{
    static const Span rows[] = {{0U, 1U}, {1U, 2U}};
    PasteFixture f;
    RegVal v;
    paste_block_value(&v, "XY", rows, 2U, 1U, false);
    paste_fixture_init(&f, "abc\n\tz\n", 2U);
    paste_install(&f.regs, &v);
    YEW_ASSERT(yew_reg_paste(&f.regs, &f.edit, '"', true, 4U));
    paste_assert_text(f.tb, "abXc\n  Y\tz\n");
    YEW_ASSERT_EQ_U64(f.cursors.curs.data[0].pos.v, 2U);
    paste_fixture_free(&f);
    yew_regval_free(&v);
}

void test_paste_block_cjk_column_is_cell_based(void)
{
    static const Span rows[] = {{0U, 1U}, {1U, 2U}};
    PasteFixture f;
    RegVal v;
    paste_block_value(&v, "XY", rows, 2U, 1U, false);
    paste_fixture_init(&f, "界a\nq\n", 3U);
    paste_install(&f.regs, &v);
    YEW_ASSERT(yew_reg_paste(&f.regs, &f.edit, '"', true, 8U));
    paste_assert_text(f.tb, "界Xa\nq Y\n");
    YEW_ASSERT_EQ_U64(f.cursors.curs.data[0].pos.v, 3U);
    paste_fixture_free(&f);
    yew_regval_free(&v);
}

void test_paste_block_ragged_rows_remain_ragged(void)
{
    static const Span rows[] = {{0U, 1U}, {1U, 4U}};
    PasteFixture f;
    RegVal v;
    paste_block_value(&v, "xyyy", rows, 2U, 3U, true);
    paste_fixture_init(&f, "ab\ncd\n", 1U);
    paste_install(&f.regs, &v);
    YEW_ASSERT(yew_reg_paste(&f.regs, &f.edit, '"', true, 8U));
    paste_assert_text(f.tb, "axb\ncyyyd\n");
    YEW_ASSERT_EQ_U64(f.undo->nodes.len, 2U);
    paste_fixture_free(&f);
    yew_regval_free(&v);
}
