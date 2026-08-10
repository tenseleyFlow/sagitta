#include "harness.h"

#include "text/register.h"

static void ring_value(RegVal *v, const char *bytes)
{
    sag_regval_init(v);
    v->type = SAG_REG_CHARWISE;
    bytebuf_append(&v->bytes, bytes, strlen(bytes));
}

void test_kill_ring_depth_evicts_oldest(void)
{
    Registers r;
    RegVal a;
    RegVal b;
    RegVal c;
    RegInfo info[3];
    sag_reg_init(&r);
    r.ring_depth = 2U;
    ring_value(&a, "a"); ring_value(&b, "bb"); ring_value(&c, "ccc");
    sag_reg_ring_push(&r, &a);
    sag_reg_ring_push(&r, &b);
    sag_reg_ring_push(&r, &c);
    SAG_ASSERT_EQ_U64(r.ring_len, 2U);
    SAG_ASSERT_EQ_U64(r.ring_bytes, 5U);
    SAG_ASSERT_EQ_U64(sag_reg_ring_list(&r, info, 3U), 2U);
    SAG_ASSERT_EQ_U64(info[0].bytes, 3U);
    SAG_ASSERT_EQ_U64(info[1].bytes, 2U);
    sag_regval_free(&c); sag_regval_free(&b); sag_regval_free(&a);
    sag_reg_free(&r);
}

void test_kill_ring_shrink_does_not_resurrect_entries(void)
{
    Registers r;
    RegVal a;
    RegVal b;
    RegVal c;
    RegInfo info[3];

    sag_reg_init(&r);
    r.clipboard_sync = SAG_CLIP_SYNC_OFF;
    ring_value(&a, "a"); ring_value(&b, "bb"); ring_value(&c, "ccc");
    sag_reg_ring_push(&r, &a);
    sag_reg_ring_push(&r, &b);
    sag_reg_ring_push(&r, &c);
    sag_reg_ring_set_depth(&r, 1U);
    SAG_ASSERT_EQ_U64(r.ring_len, 1U);
    SAG_ASSERT_EQ_U64(r.ring_bytes, 3U);
    sag_reg_ring_set_depth(&r, 3U);
    SAG_ASSERT_EQ_U64(sag_reg_ring_list(&r, info, 3U), 1U);
    SAG_ASSERT_EQ_U64(info[0].bytes, 3U);
    sag_regval_free(&c); sag_regval_free(&b); sag_regval_free(&a);
    sag_reg_free(&r);
}

void test_kill_ring_byte_cap_and_oversized_entry(void)
{
    Registers r;
    RegVal a;
    RegVal b;
    RegVal huge;
    sag_reg_init(&r);
    r.ring_bytes_max = 4U;
    ring_value(&a, "aa"); ring_value(&b, "bbb"); ring_value(&huge, "12345");
    sag_reg_ring_push(&r, &a);
    sag_reg_ring_push(&r, &b);
    SAG_ASSERT_EQ_U64(r.ring_len, 1U);
    SAG_ASSERT_EQ_U64(r.ring_bytes, 3U);
    SAG_ASSERT_EQ_MEM(r.ring[r.ring_head].bytes.data, "bbb", 3U);
    sag_reg_ring_push(&r, &huge);
    SAG_ASSERT_EQ_U64(r.ring_len, 1U);
    SAG_ASSERT_EQ_U64(r.ring_bytes, 5U);
    SAG_ASSERT_EQ_MEM(r.ring[r.ring_head].bytes.data, "12345", 5U);
    sag_regval_free(&huge); sag_regval_free(&b); sag_regval_free(&a);
    sag_reg_free(&r);
}

void test_kill_ring_zero_depth_disables_history(void)
{
    Registers r;
    RegVal v;
    sag_reg_init(&r);
    r.clipboard_sync = SAG_CLIP_SYNC_OFF;
    r.ring_depth = 0U;
    ring_value(&v, "value");
    sag_reg_yank(&r, 0U, &v);
    SAG_ASSERT_EQ_U64(r.ring_len, 0U);
    SAG_ASSERT_EQ_U64(r.ring_bytes, 0U);
    SAG_ASSERT_EQ_U64(r.unnamed.bytes.len, 5U);
    sag_regval_free(&v);
    sag_reg_free(&r);
}

typedef struct RingFixture {
    Registers regs;
    TextBuf *tb;
    CursorSet cursors;
    UndoTree *undo;
    FileMeta meta;
    EditCtx edit;
} RingFixture;

static void ring_fixture_init(RingFixture *f)
{
    Cursor c = {BYTEOFF(0U), {0U}, BYTEOFF(0U)};
    sag_reg_init(&f->regs);
    f->regs.clipboard_sync = SAG_CLIP_SYNC_OFF;
    f->tb = sag_textbuf_from_bytes((const u8 *)"z", 1U);
    sag_cset_init(&f->cursors, c);
    f->undo = sag_undo_new(f->tb);
    sag_filemeta_init(&f->meta);
    f->edit = (EditCtx){f->tb, NULL, &f->cursors, 3U, NULL, f->undo,
                       NULL, NULL, NULL, 0};
}

static void ring_fixture_free(RingFixture *f)
{
    sag_filemeta_dispose(&f->meta);
    sag_undo_free(f->undo);
    sag_cset_free(&f->cursors);
    sag_textbuf_free(f->tb);
    sag_reg_free(&f->regs);
}

static void ring_assert_text(const TextBuf *tb, const char *want)
{
    TextIter it;
    u64 done = 0U;
    u64 total = sag_textbuf_len(tb);
    SAG_ASSERT_EQ_U64(sag_textbuf_len(tb), strlen(want));
    SAG_ASSERT(sag_textiter_begin(&it, tb, BYTEOFF(0U)));
    while (done < total) {
        const u8 *bytes;
        u64 len;
        SAG_ASSERT(sag_textiter_chunk(&it, tb, &bytes, &len));
        SAG_ASSERT_EQ_MEM(bytes, want + done, len);
        done += len;
        if (done < total)
            SAG_ASSERT(sag_textiter_advance(&it, tb));
    }
}

void test_kill_ring_five_cycles_amend_one_undo_node(void)
{
    RingFixture f;
    RegVal old;
    RegVal recent;
    u32 i;
    ring_fixture_init(&f);
    ring_value(&old, "A");
    ring_value(&recent, "B");
    sag_reg_yank(&f.regs, 0U, &old);
    sag_reg_yank(&f.regs, 0U, &recent);
    SAG_ASSERT(sag_reg_paste(&f.regs, &f.edit, '"', true, 8U));
    ring_assert_text(f.tb, "Bz");
    for (i = 0U; i < 5U; i++)
        SAG_ASSERT(sag_reg_ring_cycle(&f.regs, &f.edit, 1));
    ring_assert_text(f.tb, "Az");
    SAG_ASSERT_EQ_U64(f.undo->nodes.len, 2U);
    SAG_ASSERT_EQ_U64(f.undo->cur, 2U);
    SAG_ASSERT(sag_undo(&f.edit));
    ring_assert_text(f.tb, "z");
    SAG_ASSERT_EQ_U64(f.cursors.curs.data[0].pos.v, 0U);
    sag_regval_free(&recent); sag_regval_free(&old);
    ring_fixture_free(&f);
}

void test_kill_ring_cycle_refuses_after_intervening_edit(void)
{
    RingFixture f;
    RegVal v;
    ring_fixture_init(&f);
    ring_value(&v, "A");
    sag_reg_yank(&f.regs, 0U, &v);
    SAG_ASSERT(sag_reg_paste(&f.regs, &f.edit, '"', true, 8U));
    sag_edit_insert(&f.edit, BYTEOFF(2U), (const u8 *)"!", 1U);
    sag_test_capture_log();
    SAG_ASSERT(!sag_reg_ring_cycle(&f.regs, &f.edit, 1));
    SAG_ASSERT(sag_test_log_contains(SAG_LOG_WARN, "refused"));
    ring_assert_text(f.tb, "Az!");
    SAG_ASSERT_EQ_U64(f.undo->nodes.len, 3U);
    sag_regval_free(&v);
    ring_fixture_free(&f);
}
