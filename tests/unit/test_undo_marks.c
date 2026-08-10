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
    f->tb = yew_textbuf_from_bytes(bytes, len);
    f->marks = yew_marks_new();
    yew_cset_init(&f->cursors, cursor);
    f->undo = yew_undo_new(f->tb);
    f->mono = 1000U;
    f->wall = 100;
    yew_undo_set_clock(f->undo, marks_mono, marks_wall, f);
    f->edit = (EditCtx){f->tb, f->marks, &f->cursors, 0U, NULL, f->undo,
                       NULL, NULL, NULL, 0};
}

static void marks_fixture_free(MarkFixture *f)
{
    yew_undo_free(f->undo);
    yew_cset_free(&f->cursors);
    yew_marks_free(f->marks);
    yew_textbuf_free(f->tb);
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
        if (kind == YEW_JOURNAL_INS) {
            if (marks[i].pos > at ||
                (marks[i].pos == at && marks[i].bias == YEW_BIAS_RIGHT))
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
    first = yew_mark_add(f.marks, BYTEOFF(2U), YEW_BIAS_LEFT);
    second = yew_mark_add(f.marks, BYTEOFF(7U), YEW_BIAS_RIGHT);
    yew_undo_begin(&f.edit, YEW_TXN_CUT);
    yew_edit_delete(&f.edit, (Span){1U, 4U});
    yew_edit_delete(&f.edit, (Span){3U, 5U});
    yew_undo_end(&f.edit);
    node = &f.undo->nodes.data[f.undo->cur - 1U];
    YEW_ASSERT_EQ_U64(node->n_ops, 2U);
    YEW_ASSERT_EQ_U64(node->n_rep, 2U);
    YEW_ASSERT_EQ_U64(f.undo->repair_runs.len, f.undo->ops.len);
    run0 = &f.undo->repair_runs.data[node->ops_at];
    run1 = &f.undo->repair_runs.data[node->ops_at + 1U];
    YEW_ASSERT_EQ_U64(run0->len, 1U);
    YEW_ASSERT_EQ_U64(run1->len, 1U);
    YEW_ASSERT(run0->at != run1->at);
    YEW_ASSERT_EQ_U64(yew_mark_pos(f.marks, first).v, 1U);
    YEW_ASSERT_EQ_U64(yew_mark_pos(f.marks, second).v, 3U);
    YEW_ASSERT(yew_undo(&f.edit));
    YEW_ASSERT_EQ_U64(yew_mark_pos(f.marks, first).v, 2U);
    YEW_ASSERT_EQ_U64(yew_mark_pos(f.marks, second).v, 7U);
    marks_fixture_free(&f);
}

void test_undo_marks_stale_generation_skips_repair(void)
{
    MarkFixture f;
    MarkId stale;
    MarkId fresh;

    marks_fixture_init(&f, (const u8 *)"abcdef", 6U);
    stale = yew_mark_add(f.marks, BYTEOFF(3U), YEW_BIAS_LEFT);
    yew_undo_begin(&f.edit, YEW_TXN_CUT);
    yew_edit_delete(&f.edit, (Span){2U, 5U});
    yew_undo_end(&f.edit);
    yew_mark_del(f.marks, stale);
    fresh = yew_mark_add(f.marks, BYTEOFF(0U), YEW_BIAS_LEFT);
    YEW_ASSERT_EQ_U64(fresh.id, stale.id);
    YEW_ASSERT(fresh.gen != stale.gen);
    YEW_ASSERT(yew_undo(&f.edit));
    YEW_ASSERT_EQ_U64(yew_mark_pos(f.marks, fresh).v, 0U);
    YEW_ASSERT_EQ_U64(yew_textbuf_len(f.tb), 6U);
    YEW_ASSERT_EQ_U64(f.undo->cur, f.undo->root);
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
    left = yew_mark_add(f.marks, BYTEOFF(1U), YEW_BIAS_LEFT);
    right = yew_mark_add(f.marks, BYTEOFF(1U), YEW_BIAS_RIGHT);
    yew_edit_insert(&f.edit, BYTEOFF(1U), (const u8 *)"X", 1U);
    node = &f.undo->nodes.data[f.undo->cur - 1U];
    run = &f.undo->repair_runs.data[node->ops_at];
    YEW_ASSERT_EQ_U64(run->len, 0U);
    YEW_ASSERT_EQ_U64(node->n_rep, 0U);
    YEW_ASSERT_EQ_U64(yew_mark_pos(f.marks, left).v, 1U);
    YEW_ASSERT_EQ_U64(yew_mark_pos(f.marks, right).v, 2U);
    YEW_ASSERT(yew_undo(&f.edit));
    YEW_ASSERT_EQ_U64(yew_mark_pos(f.marks, left).v, 1U);
    YEW_ASSERT_EQ_U64(yew_mark_pos(f.marks, right).v, 1U);
    marks_fixture_free(&f);
}

void test_undo_marks_redo_recollapses_repaired_marks(void)
{
    MarkFixture f;
    MarkId a;
    MarkId b;

    marks_fixture_init(&f, (const u8 *)"0123456789", 10U);
    a = yew_mark_add(f.marks, BYTEOFF(3U), YEW_BIAS_LEFT);
    b = yew_mark_add(f.marks, BYTEOFF(6U), YEW_BIAS_RIGHT);
    yew_undo_begin(&f.edit, YEW_TXN_CUT);
    yew_edit_delete(&f.edit, (Span){2U, 8U});
    yew_undo_end(&f.edit);
    YEW_ASSERT_EQ_U64(yew_mark_pos(f.marks, a).v, 2U);
    YEW_ASSERT_EQ_U64(yew_mark_pos(f.marks, b).v, 2U);
    YEW_ASSERT(yew_undo(&f.edit));
    YEW_ASSERT_EQ_U64(yew_mark_pos(f.marks, a).v, 3U);
    YEW_ASSERT_EQ_U64(yew_mark_pos(f.marks, b).v, 6U);
    YEW_ASSERT(yew_redo(&f.edit));
    YEW_ASSERT_EQ_U64(yew_mark_pos(f.marks, a).v, 2U);
    YEW_ASSERT_EQ_U64(yew_mark_pos(f.marks, b).v, 2U);
    marks_fixture_free(&f);
}

void test_undo_marks_randomized_oracle_restores_1000_marks(void)
{
    enum { MARKS = 1000, EDITS = 1000 };
    MarkFixture f;
    OracleMark *oracle = yew_xmalloc(sizeof(*oracle) * MARKS);
    u64 *before = yew_xmalloc(sizeof(*before) * MARKS);
    u64 state = UINT64_C(0xd1b54a32d192ed03);
    u64 text_len = 4096U;
    u8 *initial = yew_xmalloc((size_t)text_len);
    size_t i;

    (void)memset(initial, 'x', (size_t)text_len);
    marks_fixture_init(&f, initial, text_len);
    free(initial);
    for (i = 0U; i < MARKS; i++) {
        oracle[i].pos = marks_random(&state) % (text_len + 1U);
        oracle[i].bias = (marks_random(&state) & 1U) != 0U
                             ? YEW_BIAS_RIGHT
                             : YEW_BIAS_LEFT;
        oracle[i].id = yew_mark_add(f.marks, BYTEOFF(oracle[i].pos),
                                    oracle[i].bias);
    }
    for (i = 0U; i < EDITS; i++) {
        bool inserting = text_len == 0U || (marks_random(&state) & 1U) != 0U;
        u64 at;
        u64 len;
        size_t j;

        for (j = 0U; j < MARKS; j++)
            before[j] = oracle[j].pos;
        f.mono += YEW_UNDO_BURST_MS;
        yew_undo_begin(&f.edit, inserting ? YEW_TXN_PASTE : YEW_TXN_CUT);
        if (inserting) {
            u8 bytes[8];

            at = marks_random(&state) % (text_len + 1U);
            len = 1U + marks_random(&state) % sizeof(bytes);
            (void)memset(bytes, (int)(i & 0xffU), (size_t)len);
            yew_edit_insert(&f.edit, BYTEOFF(at), bytes, len);
            marks_oracle_adjust(oracle, MARKS, YEW_JOURNAL_INS, at, len);
            text_len += len;
        } else {
            at = marks_random(&state) % text_len;
            len = 1U + marks_random(&state) % (text_len - at);
            if (len > 8U)
                len = 8U;
            yew_edit_delete(&f.edit, (Span){at, at + len});
            marks_oracle_adjust(oracle, MARKS, YEW_JOURNAL_DEL, at, len);
            text_len -= len;
        }
        yew_undo_end(&f.edit);
        for (j = 0U; j < MARKS; j++)
            YEW_ASSERT_EQ_U64(yew_mark_pos(f.marks, oracle[j].id).v,
                              oracle[j].pos);
        YEW_ASSERT(yew_undo(&f.edit));
        for (j = 0U; j < MARKS; j++) {
            oracle[j].pos = before[j];
            YEW_ASSERT_EQ_U64(yew_mark_pos(f.marks, oracle[j].id).v,
                              before[j]);
        }
        if (inserting)
            text_len -= len;
        else {
            text_len += len;
            YEW_ASSERT(yew_redo(&f.edit));
            YEW_ASSERT(yew_undo(&f.edit));
            for (j = 0U; j < MARKS; j++)
                YEW_ASSERT_EQ_U64(yew_mark_pos(f.marks, oracle[j].id).v,
                                  before[j]);
        }
        YEW_ASSERT_EQ_U64(yew_textbuf_len(f.tb), text_len);
    }
    for (i = 0U; i < MARKS; i++)
        YEW_ASSERT(yew_mark_pos(f.marks, oracle[i].id).v <= text_len);
    free(before);
    free(oracle);
    marks_fixture_free(&f);
}
