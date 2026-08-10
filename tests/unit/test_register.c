#define _POSIX_C_SOURCE 200809L

#include "harness.h"

#include <errno.h>
#include <stdio.h>
#include <sys/wait.h>
#include <unistd.h>

#include "text/register.h"

static void reg_test_value(RegVal *v, RegType type, const u8 *bytes,
                           size_t len)
{
    yew_regval_init(v);
    v->type = (u8)type;
    bytebuf_append(&v->bytes, bytes, len);
}

static void reg_assert_value(const RegVal *v, RegType type,
                             const u8 *bytes, size_t len)
{
    YEW_ASSERT_EQ_U64(v->type, type);
    YEW_ASSERT_EQ_U64(v->bytes.len, len);
    YEW_ASSERT_EQ_MEM(v->bytes.data, bytes, len);
}

void test_register_defaults_and_empty_deferred_slots(void)
{
    Registers r;

    yew_reg_init(&r);
    YEW_ASSERT_EQ_U64(r.ring_depth, 32U);
    YEW_ASSERT_EQ_U64(r.ring_bytes_max, UINT64_C(8) * 1024U * 1024U);
    YEW_ASSERT_EQ_U64(r.clipboard_sync, YEW_CLIP_SYNC_YANK);
    YEW_ASSERT_EQ_U64(yew_reg_get(&r, '/')->bytes.len, 0U);
    YEW_ASSERT_EQ_U64(yew_reg_get(&r, ':')->bytes.len, 0U);
    YEW_ASSERT_EQ_U64(yew_reg_get(&r, '#')->bytes.len, 0U);
    YEW_ASSERT_NULL(yew_reg_get(&r, '_'));
    YEW_ASSERT_NULL(yew_reg_get(&r, '?'));
    YEW_ASSERT(yew_reg_get(&r, '*') == yew_reg_get(&r, '+'));
    yew_reg_set_cmdline(&r, (const u8 *)"write", 5U);
    reg_assert_value(yew_reg_get(&r, ':'), YEW_REG_CHARWISE,
                     (const u8 *)"write", 5U);
    yew_reg_free(&r);
}

void test_register_computes_last_insert_and_current_path_on_read(void)
{
    static const u8 inserted[] = {(u8)'a', 0U, (u8)'b'};
    static const char path[] = "/tmp/yew-current-path";
    Registers r;
    TextBuf *tb;
    CursorSet cursors;
    Cursor cursor = {BYTEOFF(0U), {0U}, BYTEOFF(0U)};
    UndoTree *undo;
    FileMeta meta;
    EditCtx edit;

    yew_reg_init(&r);
    tb = yew_textbuf_new();
    yew_cset_init(&cursors, cursor);
    undo = yew_undo_new(tb);
    yew_filemeta_init(&meta);
    meta.realpath = yew_xmalloc(sizeof(path));
    (void)memcpy(meta.realpath, path, sizeof(path));
    edit = (EditCtx){tb, NULL, &cursors, 1U, NULL, undo, NULL, NULL, NULL, 0};
    yew_edit_insert(&edit, BYTEOFF(0U), inserted, sizeof(inserted));
    yew_edit_delete(&edit, (Span){2U, 3U});
    yew_reg_bind_context(&r, undo, &meta);

    reg_assert_value(yew_reg_get(&r, '.'), YEW_REG_CHARWISE,
                     inserted, sizeof(inserted));
    reg_assert_value(yew_reg_get(&r, '%'), YEW_REG_CHARWISE,
                     (const u8 *)path, sizeof(path) - 1U);

    yew_filemeta_dispose(&meta);
    yew_undo_free(undo);
    yew_cset_free(&cursors);
    yew_textbuf_free(tb);
    yew_reg_free(&r);
}

void test_register_last_insert_reflects_backspace_in_type_run(void)
{
    Registers r;
    TextBuf *tb;
    UndoTree *undo;
    EditCtx edit;

    yew_reg_init(&r);
    tb = yew_textbuf_from_bytes((const u8 *)"--", 2U);
    undo = yew_undo_new(tb);
    edit = (EditCtx){tb, NULL, NULL, 1U, NULL, undo, NULL, NULL, NULL, 0};
    yew_undo_begin(&edit, YEW_TXN_TYPE);
    yew_edit_insert(&edit, BYTEOFF(1U), (const u8 *)"abc", 3U);
    yew_edit_delete(&edit, (Span){2U, 3U});
    yew_edit_insert(&edit, BYTEOFF(2U), (const u8 *)"X", 1U);
    yew_edit_delete(&edit, (Span){0U, 2U});
    yew_undo_end(&edit);
    yew_reg_bind_context(&r, undo, NULL);

    reg_assert_value(yew_reg_get(&r, '.'), YEW_REG_CHARWISE,
                     (const u8 *)"Xc", 2U);

    yew_undo_free(undo);
    yew_textbuf_free(tb);
    yew_reg_free(&r);
}

void test_register_yank_routes_named_unnamed_zero_and_ring(void)
{
    Registers r;
    RegVal v;

    yew_reg_init(&r);
    r.clipboard_sync = YEW_CLIP_SYNC_OFF;
    reg_test_value(&v, YEW_REG_CHARWISE, (const u8 *)"yank\0", 5U);
    yew_reg_yank(&r, 'b', &v);
    reg_assert_value(yew_reg_get(&r, 'b'), YEW_REG_CHARWISE,
                     (const u8 *)"yank\0", 5U);
    reg_assert_value(yew_reg_get(&r, '"'), YEW_REG_CHARWISE,
                     (const u8 *)"yank\0", 5U);
    reg_assert_value(yew_reg_get(&r, '0'), YEW_REG_CHARWISE,
                     (const u8 *)"yank\0", 5U);
    YEW_ASSERT_EQ_U64(r.ring_len, 1U);
    reg_assert_value(&r.ring[r.ring_head], YEW_REG_CHARWISE,
                     (const u8 *)"yank\0", 5U);
    YEW_ASSERT_EQ_U64(yew_reg_get(&r, '1')->bytes.len, 0U);
    YEW_ASSERT_EQ_U64(yew_reg_get(&r, '-')->bytes.len, 0U);
    yew_regval_free(&v);
    yew_reg_free(&r);
}

void test_register_delete_shift_boundary(void)
{
    Registers r;
    RegVal small;
    RegVal lf;
    RegVal line;

    yew_reg_init(&r);
    reg_test_value(&small, YEW_REG_CHARWISE, (const u8 *)"x", 1U);
    reg_test_value(&lf, YEW_REG_CHARWISE, (const u8 *)"x\ny", 3U);
    reg_test_value(&line, YEW_REG_LINEWISE, (const u8 *)"line", 4U);
    yew_reg_delete(&r, 0U, &small);
    reg_assert_value(&r.small_del, YEW_REG_CHARWISE, (const u8 *)"x", 1U);
    YEW_ASSERT_EQ_U64(r.numbered[1].bytes.len, 0U);
    yew_reg_delete(&r, 0U, &lf);
    reg_assert_value(&r.numbered[1], YEW_REG_CHARWISE,
                     (const u8 *)"x\ny", 3U);
    yew_reg_delete(&r, 0U, &line);
    reg_assert_value(&r.numbered[1], YEW_REG_LINEWISE,
                     (const u8 *)"line", 4U);
    reg_assert_value(&r.numbered[2], YEW_REG_CHARWISE,
                     (const u8 *)"x\ny", 3U);
    reg_assert_value(&r.unnamed, YEW_REG_LINEWISE,
                     (const u8 *)"line", 4U);
    YEW_ASSERT_EQ_U64(r.numbered[0].bytes.len, 0U);
    YEW_ASSERT_EQ_U64(r.ring_len, 3U);
    yew_regval_free(&line);
    yew_regval_free(&lf);
    yew_regval_free(&small);
    yew_reg_free(&r);
}

void test_register_reserved_histories_ignore_explicit_names(void)
{
    Registers r;
    RegVal keep_zero;
    RegVal keep_one;
    RegVal replacement;

    yew_reg_init(&r);
    r.clipboard_sync = YEW_CLIP_SYNC_OFF;
    reg_test_value(&keep_zero, YEW_REG_CHARWISE, (const u8 *)"zero", 4U);
    reg_test_value(&keep_one, YEW_REG_CHARWISE, (const u8 *)"one", 3U);
    reg_test_value(&replacement, YEW_REG_CHARWISE,
                   (const u8 *)"replacement", 11U);
    yew_regval_copy(&r.numbered[0], &keep_zero);
    yew_regval_copy(&r.numbered[1], &keep_one);

    yew_reg_delete(&r, '0', &replacement);
    reg_assert_value(&r.numbered[0], YEW_REG_CHARWISE,
                     (const u8 *)"zero", 4U);
    yew_reg_yank(&r, '1', &replacement);
    reg_assert_value(&r.numbered[1], YEW_REG_CHARWISE,
                     (const u8 *)"one", 3U);
    reg_assert_value(&r.numbered[0], YEW_REG_CHARWISE,
                     (const u8 *)"replacement", 11U);

    yew_regval_free(&replacement);
    yew_regval_free(&keep_one);
    yew_regval_free(&keep_zero);
    yew_reg_free(&r);
}

void test_register_delete_shifts_nine_and_discards_oldest(void)
{
    Registers r;
    RegVal v;
    u8 byte = 0U;
    u32 i;

    yew_reg_init(&r);
    reg_test_value(&v, YEW_REG_LINEWISE, &byte, 0U);
    for (i = 1U; i <= 10U; i++) {
        byte = (u8)('a' + i - 1U);
        v.bytes.len = 0U;
        bytebuf_append(&v.bytes, &byte, 1U);
        yew_reg_delete(&r, 0U, &v);
    }
    YEW_ASSERT_EQ_U64(r.numbered[1].bytes.data[0], 'j');
    YEW_ASSERT_EQ_U64(r.numbered[2].bytes.data[0], 'i');
    YEW_ASSERT_EQ_U64(r.numbered[9].bytes.data[0], 'b');
    yew_regval_free(&v);
    yew_reg_free(&r);
}

void test_register_blackhole_is_total_discard(void)
{
    Registers r;
    RegVal keep;
    RegVal drop;

    yew_reg_init(&r);
    r.clipboard_sync = YEW_CLIP_SYNC_OFF;
    reg_test_value(&keep, YEW_REG_CHARWISE, (const u8 *)"keep", 4U);
    reg_test_value(&drop, YEW_REG_LINEWISE, (const u8 *)"drop\n", 5U);
    yew_reg_yank(&r, 0U, &keep);
    yew_reg_delete(&r, '_', &drop);
    reg_assert_value(&r.unnamed, YEW_REG_CHARWISE,
                     (const u8 *)"keep", 4U);
    reg_assert_value(&r.numbered[0], YEW_REG_CHARWISE,
                     (const u8 *)"keep", 4U);
    YEW_ASSERT_EQ_U64(r.numbered[1].bytes.len, 0U);
    YEW_ASSERT_EQ_U64(r.small_del.bytes.len, 0U);
    YEW_ASSERT_EQ_U64(r.ring_len, 1U);
    yew_reg_yank(&r, '_', &drop);
    YEW_ASSERT_EQ_U64(r.ring_len, 1U);
    yew_regval_free(&drop);
    yew_regval_free(&keep);
    yew_reg_free(&r);
}

void test_register_uppercase_routes_append_but_unnamed_is_new_value(void)
{
    Registers r;
    RegVal one;
    RegVal two;

    yew_reg_init(&r);
    r.clipboard_sync = YEW_CLIP_SYNC_OFF;
    reg_test_value(&one, YEW_REG_CHARWISE, (const u8 *)"one", 3U);
    reg_test_value(&two, YEW_REG_CHARWISE, (const u8 *)"two", 3U);
    yew_reg_yank(&r, 'a', &one);
    yew_reg_yank(&r, 'A', &two);
    reg_assert_value(&r.named[0], YEW_REG_CHARWISE,
                     (const u8 *)"onetwo", 6U);
    reg_assert_value(&r.unnamed, YEW_REG_CHARWISE,
                     (const u8 *)"two", 3U);
    reg_assert_value(&r.numbered[0], YEW_REG_CHARWISE,
                     (const u8 *)"two", 3U);
    YEW_ASSERT_EQ_U64(r.ring_len, 2U);
    yew_regval_free(&two);
    yew_regval_free(&one);
    yew_reg_free(&r);
}

void test_register_line_capture_synthesizes_only_missing_eol(void)
{
    TextBuf *tb = yew_textbuf_from_bytes((const u8 *)"abc", 3U);
    FileMeta meta;
    RegVal line;
    RegVal chars;

    yew_filemeta_init(&meta);
    meta.eol = YEW_EOL_CRLF;
    meta.dominant_eol = YEW_EOL_CRLF;
    yew_regval_init(&line);
    yew_regval_init(&chars);
    yew_regval_from_span(&line, tb, (Span){0U, 3U}, YEW_REG_LINEWISE,
                         &meta);
    yew_regval_from_span(&chars, tb, (Span){0U, 3U}, YEW_REG_CHARWISE,
                         &meta);
    reg_assert_value(&line, YEW_REG_LINEWISE,
                     (const u8 *)"abc\r\n", 5U);
    reg_assert_value(&chars, YEW_REG_CHARWISE, (const u8 *)"abc", 3U);
    yew_regval_free(&chars);
    yew_regval_free(&line);
    yew_filemeta_dispose(&meta);
    yew_textbuf_free(tb);
}

void test_register_accepts_explicit_blockwise_values(void)
{
    static const u8 bytes[] = "ab\nZ";
    static const Span rows[] = {{0U, 2U}, {3U, 4U}};
    Registers r;
    RegVal block;
    const RegVal *stored;
    size_t i;

    yew_reg_init(&r);
    reg_test_value(&block, YEW_REG_BLOCKWISE, bytes, sizeof(bytes) - 1U);
    block.width = 2U;
    block.ragged = true;
    for (i = 0U; i < sizeof(rows) / sizeof(rows[0]); i++)
        YewRegRowVec_push(&block.rows, rows[i]);
    yew_reg_set(&r, 'a', &block);
    stored = yew_reg_get(&r, 'a');
    reg_assert_value(stored, YEW_REG_BLOCKWISE, bytes, sizeof(bytes) - 1U);
    YEW_ASSERT_EQ_U64(stored->width, 2U);
    YEW_ASSERT(stored->ragged);
    YEW_ASSERT_EQ_U64(stored->rows.len, 2U);
    YEW_ASSERT_EQ_U64(stored->rows.data[0].lo, 0U);
    YEW_ASSERT_EQ_U64(stored->rows.data[0].hi, 2U);
    YEW_ASSERT_EQ_U64(stored->rows.data[1].lo, 3U);
    YEW_ASSERT_EQ_U64(stored->rows.data[1].hi, 4U);
    yew_regval_free(&block);
    yew_reg_free(&r);
}
