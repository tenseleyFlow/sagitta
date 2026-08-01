#include "harness.h"

#include "text/edit.h"

typedef struct {
    TextBuf *tb;
    MarkSet *marks;
    CursorSet cursors;
    UndoTree *undo;
    EditCtx edit;
    u64 mono;
    i64 wall;
} MarkFixture;

typedef struct {
    MarkId id;
    u64 pos;
    MarkBias bias;
} OracleMark;

static u64 marks_mono(void *ctx)
{
    return ((MarkFixture *)ctx)->mono;
}

static i64 marks_wall(void *ctx)
{
    return ((MarkFixture *)ctx)->wall;
}

static void marks_fixture_init(MarkFixture *f, const u8 *bytes, u64 len)
{
    Cursor cursor;

    cursor.pos = BYTEOFF(0U);
    cursor.goal_col = (GCol){0U};
    cursor.anchor = BYTEOFF(0U);
    f->tb = sag_textbuf_from_bytes(bytes, len);
    f->marks = sag_marks_new();
    sag_cset_init(&f->cursors, cursor);
    f->undo = sag_undo_new(f->tb);
    f->mono = 1000U;
    f->wall = 100;
    sag_undo_set_clock(f->undo, marks_mono, marks_wall, f);
    f->edit = (EditCtx){f->tb, f->marks, &f->cursors, 0U, NULL, f->undo,
                       NULL};
}

static void marks_fixture_free(MarkFixture *f)
{
    sag_undo_free(f->undo);
    sag_cset_free(&f->cursors);
    sag_marks_free(f->marks);
    sag_textbuf_free(f->tb);
}

static u64 marks_random(u64 *state)
{
    u64 x = *state;

    x ^= x >> 12U;
    x ^= x << 25U;
    x ^= x >> 27U;
    *state = x;
    return x * UINT64_C(2685821657736338717);
}

static void marks_oracle_adjust(OracleMark *marks, size_t count, u8 kind,
                                u64 at, u64 len)
{
    size_t i;

    for (i = 0U; i < count; i++) {
        if (kind == SAG_JOURNAL_INS) {
            if (marks[i].pos > at ||
                (marks[i].pos == at && marks[i].bias == SAG_BIAS_RIGHT))
                marks[i].pos += len;
        } else if (marks[i].pos < at) {
            continue;
        } else if (marks[i].pos < at + len) {
            marks[i].pos = at;
        } else {
            marks[i].pos -= len;
        }
    }
}

void test_undo_marks_repairs_each_delete_run_in_transaction(void)
{
    MarkFixture f;
    MarkId first;
    MarkId second;
    const UndoNode *node;
    const UndoRepairRun *run0;
    const UndoRepairRun *run1;

    marks_fixture_init(&f, (const u8 *)"abcdefghij", 10U);
    first = sag_mark_add(f.marks, BYTEOFF(2U), SAG_BIAS_LEFT);
    second = sag_mark_add(f.marks, BYTEOFF(7U), SAG_BIAS_RIGHT);
    sag_undo_begin(&f.edit, SAG_TXN_CUT);
    sag_edit_delete(&f.edit, (Span){1U, 4U});
    sag_edit_delete(&f.edit, (Span){3U, 5U});
    sag_undo_end(&f.edit);
    node = &f.undo->nodes.data[f.undo->cur - 1U];
    SAG_ASSERT_EQ_U64(node->n_ops, 2U);
    SAG_ASSERT_EQ_U64(node->n_rep, 2U);
    SAG_ASSERT_EQ_U64(f.undo->repair_runs.len, f.undo->ops.len);
    run0 = &f.undo->repair_runs.data[node->ops_at];
    run1 = &f.undo->repair_runs.data[node->ops_at + 1U];
    SAG_ASSERT_EQ_U64(run0->len, 1U);
    SAG_ASSERT_EQ_U64(run1->len, 1U);
    SAG_ASSERT(run0->at != run1->at);
    SAG_ASSERT_EQ_U64(sag_mark_pos(f.marks, first).v, 1U);
    SAG_ASSERT_EQ_U64(sag_mark_pos(f.marks, second).v, 3U);
    SAG_ASSERT(sag_undo(&f.edit));
    SAG_ASSERT_EQ_U64(sag_mark_pos(f.marks, first).v, 2U);
    SAG_ASSERT_EQ_U64(sag_mark_pos(f.marks, second).v, 7U);
    marks_fixture_free(&f);
}

void test_undo_marks_stale_generation_skips_repair(void)
{
    MarkFixture f;
    MarkId stale;
    MarkId fresh;

    marks_fixture_init(&f, (const u8 *)"abcdef", 6U);
    stale = sag_mark_add(f.marks, BYTEOFF(3U), SAG_BIAS_LEFT);
    sag_undo_begin(&f.edit, SAG_TXN_CUT);
    sag_edit_delete(&f.edit, (Span){2U, 5U});
    sag_undo_end(&f.edit);
    sag_mark_del(f.marks, stale);
    fresh = sag_mark_add(f.marks, BYTEOFF(0U), SAG_BIAS_LEFT);
    SAG_ASSERT_EQ_U64(fresh.id, stale.id);
    SAG_ASSERT(fresh.gen != stale.gen);
    SAG_ASSERT(sag_undo(&f.edit));
    SAG_ASSERT_EQ_U64(sag_mark_pos(f.marks, fresh).v, 0U);
    SAG_ASSERT_EQ_U64(sag_textbuf_len(f.tb), 6U);
    SAG_ASSERT_EQ_U64(f.undo->cur, f.undo->root);
    marks_fixture_free(&f);
}

void test_undo_marks_insert_records_empty_repair_run(void)
{
    MarkFixture f;
    MarkId left;
    MarkId right;
    const UndoNode *node;
    const UndoRepairRun *run;

    marks_fixture_init(&f, (const u8 *)"ab", 2U);
    left = sag_mark_add(f.marks, BYTEOFF(1U), SAG_BIAS_LEFT);
    right = sag_mark_add(f.marks, BYTEOFF(1U), SAG_BIAS_RIGHT);
    sag_edit_insert(&f.edit, BYTEOFF(1U), (const u8 *)"X", 1U);
    node = &f.undo->nodes.data[f.undo->cur - 1U];
    run = &f.undo->repair_runs.data[node->ops_at];
    SAG_ASSERT_EQ_U64(run->len, 0U);
    SAG_ASSERT_EQ_U64(node->n_rep, 0U);
    SAG_ASSERT_EQ_U64(sag_mark_pos(f.marks, left).v, 1U);
    SAG_ASSERT_EQ_U64(sag_mark_pos(f.marks, right).v, 2U);
    SAG_ASSERT(sag_undo(&f.edit));
    SAG_ASSERT_EQ_U64(sag_mark_pos(f.marks, left).v, 1U);
    SAG_ASSERT_EQ_U64(sag_mark_pos(f.marks, right).v, 1U);
    marks_fixture_free(&f);
}

void test_undo_marks_redo_recollapses_repaired_marks(void)
{
    MarkFixture f;
    MarkId a;
    MarkId b;

    marks_fixture_init(&f, (const u8 *)"0123456789", 10U);
    a = sag_mark_add(f.marks, BYTEOFF(3U), SAG_BIAS_LEFT);
    b = sag_mark_add(f.marks, BYTEOFF(6U), SAG_BIAS_RIGHT);
    sag_undo_begin(&f.edit, SAG_TXN_CUT);
    sag_edit_delete(&f.edit, (Span){2U, 8U});
    sag_undo_end(&f.edit);
    SAG_ASSERT_EQ_U64(sag_mark_pos(f.marks, a).v, 2U);
    SAG_ASSERT_EQ_U64(sag_mark_pos(f.marks, b).v, 2U);
    SAG_ASSERT(sag_undo(&f.edit));
    SAG_ASSERT_EQ_U64(sag_mark_pos(f.marks, a).v, 3U);
    SAG_ASSERT_EQ_U64(sag_mark_pos(f.marks, b).v, 6U);
    SAG_ASSERT(sag_redo(&f.edit));
    SAG_ASSERT_EQ_U64(sag_mark_pos(f.marks, a).v, 2U);
    SAG_ASSERT_EQ_U64(sag_mark_pos(f.marks, b).v, 2U);
    marks_fixture_free(&f);
}

void test_undo_marks_randomized_oracle_restores_1000_marks(void)
{
    enum { MARKS = 1000, EDITS = 1000 };
    MarkFixture f;
    OracleMark *oracle = sag_xmalloc(sizeof(*oracle) * MARKS);
    u64 *before = sag_xmalloc(sizeof(*before) * MARKS);
    u64 state = UINT64_C(0xd1b54a32d192ed03);
    u64 text_len = 4096U;
    u8 *initial = sag_xmalloc((size_t)text_len);
    size_t i;

    (void)memset(initial, 'x', (size_t)text_len);
    marks_fixture_init(&f, initial, text_len);
    free(initial);
    for (i = 0U; i < MARKS; i++) {
        oracle[i].pos = marks_random(&state) % (text_len + 1U);
        oracle[i].bias = (marks_random(&state) & 1U) != 0U
                             ? SAG_BIAS_RIGHT
                             : SAG_BIAS_LEFT;
        oracle[i].id = sag_mark_add(f.marks, BYTEOFF(oracle[i].pos),
                                    oracle[i].bias);
    }
    for (i = 0U; i < EDITS; i++) {
        bool inserting = text_len == 0U || (marks_random(&state) & 1U) != 0U;
        u64 at;
        u64 len;
        size_t j;

        for (j = 0U; j < MARKS; j++)
            before[j] = oracle[j].pos;
        f.mono += SAG_UNDO_BURST_MS;
        sag_undo_begin(&f.edit, inserting ? SAG_TXN_PASTE : SAG_TXN_CUT);
        if (inserting) {
            u8 bytes[8];

            at = marks_random(&state) % (text_len + 1U);
            len = 1U + marks_random(&state) % sizeof(bytes);
            (void)memset(bytes, (int)(i & 0xffU), (size_t)len);
            sag_edit_insert(&f.edit, BYTEOFF(at), bytes, len);
            marks_oracle_adjust(oracle, MARKS, SAG_JOURNAL_INS, at, len);
            text_len += len;
        } else {
            at = marks_random(&state) % text_len;
            len = 1U + marks_random(&state) % (text_len - at);
            if (len > 8U)
                len = 8U;
            sag_edit_delete(&f.edit, (Span){at, at + len});
            marks_oracle_adjust(oracle, MARKS, SAG_JOURNAL_DEL, at, len);
            text_len -= len;
        }
        sag_undo_end(&f.edit);
        for (j = 0U; j < MARKS; j++)
            SAG_ASSERT_EQ_U64(sag_mark_pos(f.marks, oracle[j].id).v,
                              oracle[j].pos);
        SAG_ASSERT(sag_undo(&f.edit));
        for (j = 0U; j < MARKS; j++) {
            oracle[j].pos = before[j];
            SAG_ASSERT_EQ_U64(sag_mark_pos(f.marks, oracle[j].id).v,
                              before[j]);
        }
        if (inserting)
            text_len -= len;
        else {
            text_len += len;
            SAG_ASSERT(sag_redo(&f.edit));
            SAG_ASSERT(sag_undo(&f.edit));
            for (j = 0U; j < MARKS; j++)
                SAG_ASSERT_EQ_U64(sag_mark_pos(f.marks, oracle[j].id).v,
                                  before[j]);
        }
        SAG_ASSERT_EQ_U64(sag_textbuf_len(f.tb), text_len);
    }
    for (i = 0U; i < MARKS; i++)
        SAG_ASSERT(sag_mark_pos(f.marks, oracle[i].id).v <= text_len);
    free(before);
    free(oracle);
    marks_fixture_free(&f);
}
