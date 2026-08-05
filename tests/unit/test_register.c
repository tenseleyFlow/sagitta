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
    sag_regval_init(v);
    v->type = (u8)type;
    bytebuf_append(&v->bytes, bytes, len);
}

static void reg_assert_value(const RegVal *v, RegType type,
                             const u8 *bytes, size_t len)
{
    SAG_ASSERT_EQ_U64(v->type, type);
    SAG_ASSERT_EQ_U64(v->bytes.len, len);
    SAG_ASSERT_EQ_MEM(v->bytes.data, bytes, len);
}

void test_register_defaults_and_empty_deferred_slots(void)
{
    Registers r;

    sag_reg_init(&r);
    SAG_ASSERT_EQ_U64(r.ring_depth, 32U);
    SAG_ASSERT_EQ_U64(r.ring_bytes_max, UINT64_C(8) * 1024U * 1024U);
    SAG_ASSERT_EQ_U64(r.clipboard_sync, SAG_CLIP_SYNC_YANK);
    SAG_ASSERT_EQ_U64(sag_reg_get(&r, '/')->bytes.len, 0U);
    SAG_ASSERT_EQ_U64(sag_reg_get(&r, ':')->bytes.len, 0U);
    SAG_ASSERT_EQ_U64(sag_reg_get(&r, '#')->bytes.len, 0U);
    SAG_ASSERT_NULL(sag_reg_get(&r, '_'));
    SAG_ASSERT_NULL(sag_reg_get(&r, '?'));
    SAG_ASSERT(sag_reg_get(&r, '*') == sag_reg_get(&r, '+'));
    sag_reg_set_cmdline(&r, (const u8 *)"write", 5U);
    reg_assert_value(sag_reg_get(&r, ':'), SAG_REG_CHARWISE,
                     (const u8 *)"write", 5U);
    sag_reg_free(&r);
}

void test_register_computes_last_insert_and_current_path_on_read(void)
{
    static const u8 inserted[] = {(u8)'a', 0U, (u8)'b'};
    static const char path[] = "/tmp/sagitta-current-path";
    Registers r;
    TextBuf *tb;
    CursorSet cursors;
    Cursor cursor = {BYTEOFF(0U), {0U}, BYTEOFF(0U)};
    UndoTree *undo;
    FileMeta meta;
    EditCtx edit;

    sag_reg_init(&r);
    tb = sag_textbuf_new();
    sag_cset_init(&cursors, cursor);
    undo = sag_undo_new(tb);
    sag_filemeta_init(&meta);
    meta.realpath = sag_xmalloc(sizeof(path));
    (void)memcpy(meta.realpath, path, sizeof(path));
    edit = (EditCtx){tb, NULL, &cursors, 1U, NULL, undo, NULL, NULL, NULL, 0};
    sag_edit_insert(&edit, BYTEOFF(0U), inserted, sizeof(inserted));
    sag_edit_delete(&edit, (Span){2U, 3U});
    sag_reg_bind_context(&r, undo, &meta);

    reg_assert_value(sag_reg_get(&r, '.'), SAG_REG_CHARWISE,
                     inserted, sizeof(inserted));
    reg_assert_value(sag_reg_get(&r, '%'), SAG_REG_CHARWISE,
                     (const u8 *)path, sizeof(path) - 1U);

    sag_filemeta_dispose(&meta);
    sag_undo_free(undo);
    sag_cset_free(&cursors);
    sag_textbuf_free(tb);
    sag_reg_free(&r);
}

void test_register_last_insert_reflects_backspace_in_type_run(void)
{
    Registers r;
    TextBuf *tb;
    UndoTree *undo;
    EditCtx edit;

    sag_reg_init(&r);
    tb = sag_textbuf_from_bytes((const u8 *)"--", 2U);
    undo = sag_undo_new(tb);
    edit = (EditCtx){tb, NULL, NULL, 1U, NULL, undo, NULL, NULL, NULL, 0};
    sag_undo_begin(&edit, SAG_TXN_TYPE);
    sag_edit_insert(&edit, BYTEOFF(1U), (const u8 *)"abc", 3U);
    sag_edit_delete(&edit, (Span){2U, 3U});
    sag_edit_insert(&edit, BYTEOFF(2U), (const u8 *)"X", 1U);
    sag_edit_delete(&edit, (Span){0U, 2U});
    sag_undo_end(&edit);
    sag_reg_bind_context(&r, undo, NULL);

    reg_assert_value(sag_reg_get(&r, '.'), SAG_REG_CHARWISE,
                     (const u8 *)"Xc", 2U);

    sag_undo_free(undo);
    sag_textbuf_free(tb);
    sag_reg_free(&r);
}

void test_register_yank_routes_named_unnamed_zero_and_ring(void)
{
    Registers r;
    RegVal v;

    sag_reg_init(&r);
    r.clipboard_sync = SAG_CLIP_SYNC_OFF;
    reg_test_value(&v, SAG_REG_CHARWISE, (const u8 *)"yank\0", 5U);
    sag_reg_yank(&r, 'b', &v);
    reg_assert_value(sag_reg_get(&r, 'b'), SAG_REG_CHARWISE,
                     (const u8 *)"yank\0", 5U);
    reg_assert_value(sag_reg_get(&r, '"'), SAG_REG_CHARWISE,
                     (const u8 *)"yank\0", 5U);
    reg_assert_value(sag_reg_get(&r, '0'), SAG_REG_CHARWISE,
                     (const u8 *)"yank\0", 5U);
    SAG_ASSERT_EQ_U64(r.ring_len, 1U);
    reg_assert_value(&r.ring[r.ring_head], SAG_REG_CHARWISE,
                     (const u8 *)"yank\0", 5U);
    SAG_ASSERT_EQ_U64(sag_reg_get(&r, '1')->bytes.len, 0U);
    SAG_ASSERT_EQ_U64(sag_reg_get(&r, '-')->bytes.len, 0U);
    sag_regval_free(&v);
    sag_reg_free(&r);
}

void test_register_delete_shift_boundary(void)
{
    Registers r;
    RegVal small;
    RegVal lf;
    RegVal line;

    sag_reg_init(&r);
    reg_test_value(&small, SAG_REG_CHARWISE, (const u8 *)"x", 1U);
    reg_test_value(&lf, SAG_REG_CHARWISE, (const u8 *)"x\ny", 3U);
    reg_test_value(&line, SAG_REG_LINEWISE, (const u8 *)"line", 4U);
    sag_reg_delete(&r, 0U, &small);
    reg_assert_value(&r.small_del, SAG_REG_CHARWISE, (const u8 *)"x", 1U);
    SAG_ASSERT_EQ_U64(r.numbered[1].bytes.len, 0U);
    sag_reg_delete(&r, 0U, &lf);
    reg_assert_value(&r.numbered[1], SAG_REG_CHARWISE,
                     (const u8 *)"x\ny", 3U);
    sag_reg_delete(&r, 0U, &line);
    reg_assert_value(&r.numbered[1], SAG_REG_LINEWISE,
                     (const u8 *)"line", 4U);
    reg_assert_value(&r.numbered[2], SAG_REG_CHARWISE,
                     (const u8 *)"x\ny", 3U);
    reg_assert_value(&r.unnamed, SAG_REG_LINEWISE,
                     (const u8 *)"line", 4U);
    SAG_ASSERT_EQ_U64(r.numbered[0].bytes.len, 0U);
    SAG_ASSERT_EQ_U64(r.ring_len, 3U);
    sag_regval_free(&line);
    sag_regval_free(&lf);
    sag_regval_free(&small);
    sag_reg_free(&r);
}

void test_register_reserved_histories_ignore_explicit_names(void)
{
    Registers r;
    RegVal keep_zero;
    RegVal keep_one;
    RegVal replacement;

    sag_reg_init(&r);
    r.clipboard_sync = SAG_CLIP_SYNC_OFF;
    reg_test_value(&keep_zero, SAG_REG_CHARWISE, (const u8 *)"zero", 4U);
    reg_test_value(&keep_one, SAG_REG_CHARWISE, (const u8 *)"one", 3U);
    reg_test_value(&replacement, SAG_REG_CHARWISE,
                   (const u8 *)"replacement", 11U);
    sag_regval_copy(&r.numbered[0], &keep_zero);
    sag_regval_copy(&r.numbered[1], &keep_one);

    sag_reg_delete(&r, '0', &replacement);
    reg_assert_value(&r.numbered[0], SAG_REG_CHARWISE,
                     (const u8 *)"zero", 4U);
    sag_reg_yank(&r, '1', &replacement);
    reg_assert_value(&r.numbered[1], SAG_REG_CHARWISE,
                     (const u8 *)"one", 3U);
    reg_assert_value(&r.numbered[0], SAG_REG_CHARWISE,
                     (const u8 *)"replacement", 11U);

    sag_regval_free(&replacement);
    sag_regval_free(&keep_one);
    sag_regval_free(&keep_zero);
    sag_reg_free(&r);
}

void test_register_delete_shifts_nine_and_discards_oldest(void)
{
    Registers r;
    RegVal v;
    u8 byte = 0U;
    u32 i;

    sag_reg_init(&r);
    reg_test_value(&v, SAG_REG_LINEWISE, &byte, 0U);
    for (i = 1U; i <= 10U; i++) {
        byte = (u8)('a' + i - 1U);
        v.bytes.len = 0U;
        bytebuf_append(&v.bytes, &byte, 1U);
        sag_reg_delete(&r, 0U, &v);
    }
    SAG_ASSERT_EQ_U64(r.numbered[1].bytes.data[0], 'j');
    SAG_ASSERT_EQ_U64(r.numbered[2].bytes.data[0], 'i');
    SAG_ASSERT_EQ_U64(r.numbered[9].bytes.data[0], 'b');
    sag_regval_free(&v);
    sag_reg_free(&r);
}

void test_register_blackhole_is_total_discard(void)
{
    Registers r;
    RegVal keep;
    RegVal drop;

    sag_reg_init(&r);
    r.clipboard_sync = SAG_CLIP_SYNC_OFF;
    reg_test_value(&keep, SAG_REG_CHARWISE, (const u8 *)"keep", 4U);
    reg_test_value(&drop, SAG_REG_LINEWISE, (const u8 *)"drop\n", 5U);
    sag_reg_yank(&r, 0U, &keep);
    sag_reg_delete(&r, '_', &drop);
    reg_assert_value(&r.unnamed, SAG_REG_CHARWISE,
                     (const u8 *)"keep", 4U);
    reg_assert_value(&r.numbered[0], SAG_REG_CHARWISE,
                     (const u8 *)"keep", 4U);
    SAG_ASSERT_EQ_U64(r.numbered[1].bytes.len, 0U);
    SAG_ASSERT_EQ_U64(r.small_del.bytes.len, 0U);
    SAG_ASSERT_EQ_U64(r.ring_len, 1U);
    sag_reg_yank(&r, '_', &drop);
    SAG_ASSERT_EQ_U64(r.ring_len, 1U);
    sag_regval_free(&drop);
    sag_regval_free(&keep);
    sag_reg_free(&r);
}

void test_register_uppercase_routes_append_but_unnamed_is_new_value(void)
{
    Registers r;
    RegVal one;
    RegVal two;

    sag_reg_init(&r);
    r.clipboard_sync = SAG_CLIP_SYNC_OFF;
    reg_test_value(&one, SAG_REG_CHARWISE, (const u8 *)"one", 3U);
    reg_test_value(&two, SAG_REG_CHARWISE, (const u8 *)"two", 3U);
    sag_reg_yank(&r, 'a', &one);
    sag_reg_yank(&r, 'A', &two);
    reg_assert_value(&r.named[0], SAG_REG_CHARWISE,
                     (const u8 *)"onetwo", 6U);
    reg_assert_value(&r.unnamed, SAG_REG_CHARWISE,
                     (const u8 *)"two", 3U);
    reg_assert_value(&r.numbered[0], SAG_REG_CHARWISE,
                     (const u8 *)"two", 3U);
    SAG_ASSERT_EQ_U64(r.ring_len, 2U);
    sag_regval_free(&two);
    sag_regval_free(&one);
    sag_reg_free(&r);
}

void test_register_line_capture_synthesizes_only_missing_eol(void)
{
    TextBuf *tb = sag_textbuf_from_bytes((const u8 *)"abc", 3U);
    FileMeta meta;
    RegVal line;
    RegVal chars;

    sag_filemeta_init(&meta);
    meta.eol = SAG_EOL_CRLF;
    meta.dominant_eol = SAG_EOL_CRLF;
    sag_regval_init(&line);
    sag_regval_init(&chars);
    sag_regval_from_span(&line, tb, (Span){0U, 3U}, SAG_REG_LINEWISE,
                         &meta);
    sag_regval_from_span(&chars, tb, (Span){0U, 3U}, SAG_REG_CHARWISE,
                         &meta);
    reg_assert_value(&line, SAG_REG_LINEWISE,
                     (const u8 *)"abc\r\n", 5U);
    reg_assert_value(&chars, SAG_REG_CHARWISE, (const u8 *)"abc", 3U);
    sag_regval_free(&chars);
    sag_regval_free(&line);
    sag_filemeta_dispose(&meta);
    sag_textbuf_free(tb);
}

void test_register_accepts_explicit_blockwise_values(void)
{
    static const u8 bytes[] = "ab\nZ";
    static const Span rows[] = {{0U, 2U}, {3U, 4U}};
    Registers r;
    RegVal block;
    const RegVal *stored;
    size_t i;

    sag_reg_init(&r);
    reg_test_value(&block, SAG_REG_BLOCKWISE, bytes, sizeof(bytes) - 1U);
    block.width = 2U;
    block.ragged = true;
    for (i = 0U; i < sizeof(rows) / sizeof(rows[0]); i++)
        SagRegRowVec_push(&block.rows, rows[i]);
    sag_reg_set(&r, 'a', &block);
    stored = sag_reg_get(&r, 'a');
    reg_assert_value(stored, SAG_REG_BLOCKWISE, bytes, sizeof(bytes) - 1U);
    SAG_ASSERT_EQ_U64(stored->width, 2U);
    SAG_ASSERT(stored->ragged);
    SAG_ASSERT_EQ_U64(stored->rows.len, 2U);
    SAG_ASSERT_EQ_U64(stored->rows.data[0].lo, 0U);
    SAG_ASSERT_EQ_U64(stored->rows.data[0].hi, 2U);
    SAG_ASSERT_EQ_U64(stored->rows.data[1].lo, 3U);
    SAG_ASSERT_EQ_U64(stored->rows.data[1].hi, 4U);
    sag_regval_free(&block);
    sag_reg_free(&r);
}
