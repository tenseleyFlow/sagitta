#include "harness.h"

#include "text/register.h"

static void ring_value(RegVal *v, const char *bytes)
{
    yew_regval_init(v);
    v->type = YEW_REG_CHARWISE;
    bytebuf_append(&v->bytes, bytes, strlen(bytes));
}

void test_kill_ring_depth_evicts_oldest(void)
{
    Registers r;
    RegVal a;
    RegVal b;
    RegVal c;
    RegInfo info[3];
    yew_reg_init(&r);
    r.ring_depth = 2U;
    ring_value(&a, "a"); ring_value(&b, "bb"); ring_value(&c, "ccc");
    yew_reg_ring_push(&r, &a);
    yew_reg_ring_push(&r, &b);
    yew_reg_ring_push(&r, &c);
    YEW_ASSERT_EQ_U64(r.ring_len, 2U);
    YEW_ASSERT_EQ_U64(r.ring_bytes, 5U);
    YEW_ASSERT_EQ_U64(yew_reg_ring_list(&r, info, 3U), 2U);
    YEW_ASSERT_EQ_U64(info[0].bytes, 3U);
    YEW_ASSERT_EQ_U64(info[1].bytes, 2U);
    yew_regval_free(&c); yew_regval_free(&b); yew_regval_free(&a);
    yew_reg_free(&r);
}

void test_kill_ring_shrink_does_not_resurrect_entries(void)
{
    Registers r;
    RegVal a;
    RegVal b;
    RegVal c;
    RegInfo info[3];

    yew_reg_init(&r);
    r.clipboard_sync = YEW_CLIP_SYNC_OFF;
    ring_value(&a, "a"); ring_value(&b, "bb"); ring_value(&c, "ccc");
    yew_reg_ring_push(&r, &a);
    yew_reg_ring_push(&r, &b);
    yew_reg_ring_push(&r, &c);
    yew_reg_ring_set_depth(&r, 1U);
    YEW_ASSERT_EQ_U64(r.ring_len, 1U);
    YEW_ASSERT_EQ_U64(r.ring_bytes, 3U);
    yew_reg_ring_set_depth(&r, 3U);
    YEW_ASSERT_EQ_U64(yew_reg_ring_list(&r, info, 3U), 1U);
    YEW_ASSERT_EQ_U64(info[0].bytes, 3U);
    yew_regval_free(&c); yew_regval_free(&b); yew_regval_free(&a);
    yew_reg_free(&r);
}

void test_kill_ring_byte_cap_and_oversized_entry(void)
{
    Registers r;
    RegVal a;
    RegVal b;
    RegVal huge;
    yew_reg_init(&r);
    r.ring_bytes_max = 4U;
    ring_value(&a, "aa"); ring_value(&b, "bbb"); ring_value(&huge, "12345");
    yew_reg_ring_push(&r, &a);
    yew_reg_ring_push(&r, &b);
    YEW_ASSERT_EQ_U64(r.ring_len, 1U);
    YEW_ASSERT_EQ_U64(r.ring_bytes, 3U);
    YEW_ASSERT_EQ_MEM(r.ring[r.ring_head].bytes.data, "bbb", 3U);
    yew_reg_ring_push(&r, &huge);
    YEW_ASSERT_EQ_U64(r.ring_len, 1U);
    YEW_ASSERT_EQ_U64(r.ring_bytes, 5U);
    YEW_ASSERT_EQ_MEM(r.ring[r.ring_head].bytes.data, "12345", 5U);
    yew_regval_free(&huge); yew_regval_free(&b); yew_regval_free(&a);
    yew_reg_free(&r);
}

void test_kill_ring_zero_depth_disables_history(void)
{
    Registers r;
    RegVal v;
    yew_reg_init(&r);
    r.clipboard_sync = YEW_CLIP_SYNC_OFF;
    r.ring_depth = 0U;
    ring_value(&v, "value");
    yew_reg_yank(&r, 0U, &v);
    YEW_ASSERT_EQ_U64(r.ring_len, 0U);
    YEW_ASSERT_EQ_U64(r.ring_bytes, 0U);
    YEW_ASSERT_EQ_U64(r.unnamed.bytes.len, 5U);
    yew_regval_free(&v);
    yew_reg_free(&r);
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
    yew_reg_init(&f->regs);
    f->regs.clipboard_sync = YEW_CLIP_SYNC_OFF;
    f->tb = yew_textbuf_from_bytes((const u8 *)"z", 1U);
    yew_cset_init(&f->cursors, c);
    f->undo = yew_undo_new(f->tb);
    yew_filemeta_init(&f->meta);
    f->edit = (EditCtx){f->tb, NULL, &f->cursors, 3U, NULL, f->undo,
                       NULL, NULL, NULL, 0};
}

static void ring_fixture_free(RingFixture *f)
{
    yew_filemeta_dispose(&f->meta);
    yew_undo_free(f->undo);
    yew_cset_free(&f->cursors);
    yew_textbuf_free(f->tb);
    yew_reg_free(&f->regs);
}

static void ring_assert_text(const TextBuf *tb, const char *want)
{
    TextIter it;
    u64 done = 0U;
    u64 total = yew_textbuf_len(tb);
    YEW_ASSERT_EQ_U64(yew_textbuf_len(tb), strlen(want));
    YEW_ASSERT(yew_textiter_begin(&it, tb, BYTEOFF(0U)));
    while (done < total) {
        const u8 *bytes;
        u64 len;
        YEW_ASSERT(yew_textiter_chunk(&it, tb, &bytes, &len));
        YEW_ASSERT_EQ_MEM(bytes, want + done, len);
        done += len;
        if (done < total)
            YEW_ASSERT(yew_textiter_advance(&it, tb));
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
    yew_reg_yank(&f.regs, 0U, &old);
    yew_reg_yank(&f.regs, 0U, &recent);
    YEW_ASSERT(yew_reg_paste(&f.regs, &f.edit, '"', true, 8U));
    ring_assert_text(f.tb, "Bz");
    for (i = 0U; i < 5U; i++)
        YEW_ASSERT(yew_reg_ring_cycle(&f.regs, &f.edit, 1));
    ring_assert_text(f.tb, "Az");
    YEW_ASSERT_EQ_U64(f.undo->nodes.len, 2U);
    YEW_ASSERT_EQ_U64(f.undo->cur, 2U);
    YEW_ASSERT(yew_undo(&f.edit));
    ring_assert_text(f.tb, "z");
    YEW_ASSERT_EQ_U64(f.cursors.curs.data[0].pos.v, 0U);
    yew_regval_free(&recent); yew_regval_free(&old);
    ring_fixture_free(&f);
}

void test_kill_ring_cycle_refuses_after_intervening_edit(void)
{
    RingFixture f;
    RegVal v;
    ring_fixture_init(&f);
    ring_value(&v, "A");
    yew_reg_yank(&f.regs, 0U, &v);
    YEW_ASSERT(yew_reg_paste(&f.regs, &f.edit, '"', true, 8U));
    yew_edit_insert(&f.edit, BYTEOFF(2U), (const u8 *)"!", 1U);
    yew_test_capture_log();
    YEW_ASSERT(!yew_reg_ring_cycle(&f.regs, &f.edit, 1));
    YEW_ASSERT(yew_test_log_contains(YEW_LOG_WARN, "refused"));
    ring_assert_text(f.tb, "Az!");
    YEW_ASSERT_EQ_U64(f.undo->nodes.len, 3U);
    yew_regval_free(&v);
    ring_fixture_free(&f);
}
