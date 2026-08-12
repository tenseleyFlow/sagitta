#include "harness.h"

#include "text/edit.h"

typedef struct {
    TextBuf *tb;
    CursorSet cursors;
    UndoTree *undo;
    EditCtx edit;
    u64 mono;
    i64 wall;
} TrimFixture;

static u64 trim_mono(void *ctx)
{
    return ((TrimFixture *)ctx)->mono;
}

static i64 trim_wall(void *ctx)
{
    return ((TrimFixture *)ctx)->wall;
}

static void trim_fixture_init(TrimFixture *f, u32 min_nodes)
{
    Cursor cursor;

    cursor.pos = BYTEOFF(0U);
    cursor.goal_col = (GCol){0U};
    cursor.anchor = BYTEOFF(0U);
    f->tb = yew_textbuf_new();
    yew_cset_init(&f->cursors, cursor);
    f->undo = yew_undo_new(f->tb);
    f->mono = 1000U;
    f->wall = 100;
    yew_undo_set_clock(f->undo, trim_mono, trim_wall, f);
    yew_undo_set_limits(f->undo, 4096U, min_nodes,
                        YEW_UNDO_PERSIST_BYTES_MAX);
    f->edit = (EditCtx){f->tb, NULL, &f->cursors, 0U, NULL, f->undo, NULL,
                       NULL, NULL, 0, NULL, NULL, {0}, 0U};
}

static void trim_fixture_free(TrimFixture *f)
{
    yew_undo_free(f->undo);
    yew_cset_free(&f->cursors);
    yew_textbuf_free(f->tb);
}

static u64 trim_random(u64 *state)
{
    u64 x = *state;

    x ^= x >> 12U;
    x ^= x << 25U;
    x ^= x >> 27U;
    *state = x;
    return x * UINT64_C(2685821657736338717);
}

static u32 trim_append(TrimFixture *f, const u8 *bytes, u64 len)
{
    f->mono += YEW_UNDO_BURST_MS;
    f->wall++;
    yew_undo_begin(&f->edit, YEW_TXN_PASTE);
    yew_edit_insert(&f->edit, BYTEOFF(yew_textbuf_len(f->tb)), bytes, len);
    yew_undo_end(&f->edit);
    return yew_undo_current(f->undo);
}

static void trim_assert_bytes(const TextBuf *tb, const u8 *want, u64 len)
{
    TextIter it;
    u64 at = 0U;

    YEW_ASSERT_EQ_U64(yew_textbuf_len(tb), len);
    if (len == 0U)
        return;
    YEW_ASSERT(yew_textiter_begin(&it, tb, BYTEOFF(0U)));
    while (at < len) {
        const u8 *bytes;
        u64 avail;
        u64 take;

        YEW_ASSERT(yew_textiter_chunk(&it, tb, &bytes, &avail));
        take = avail < len - at ? avail : len - at;
        YEW_ASSERT_EQ_MEM(bytes, want + at, take);
        at += take;
        if (at < len)
            YEW_ASSERT(yew_textiter_advance(&it, tb));
    }
}

static bool trim_live(const UndoTree *ut, u32 id)
{
    return id != 0U && id <= ut->nodes.len &&
           (ut->nodes.data[id - 1U].flags & YEW_TXN_DEAD) == 0U;
}

static u32 trim_live_count(const UndoTree *ut)
{
    u32 count = 0U;
    size_t i;

    for (i = 0U; i < ut->nodes.len; i++) {
        if ((ut->nodes.data[i].flags & YEW_TXN_DEAD) == 0U)
            count++;
    }
    return count;
}

void test_undo_trim_prunes_oldest_unprotected_branch_first(void)
{
    TrimFixture f;
    u8 payload[1024];
    u32 old_branch;
    u32 newer_branch;
    u32 protected_cur;

    (void)memset(payload, 'p', sizeof(payload));
    trim_fixture_init(&f, 100U);
    old_branch = trim_append(&f, payload, sizeof(payload));
    for (u32 i = 0U; i < 14U; i++)
        (void)trim_append(&f, payload, sizeof(payload));
    YEW_ASSERT(yew_undo_to(&f.edit, f.undo->root));
    newer_branch = trim_append(&f, payload, sizeof(payload));
    for (u32 i = 0U; i < 14U; i++)
        (void)trim_append(&f, payload, sizeof(payload));
    YEW_ASSERT(yew_undo_to(&f.edit, f.undo->root));
    yew_undo_set_limits(f.undo, 4096U, 2U, YEW_UNDO_PERSIST_BYTES_MAX);
    protected_cur = trim_append(&f, payload, sizeof(payload));
    for (u32 i = 0U; i < 14U; i++)
        (void)trim_append(&f, payload, sizeof(payload));
    YEW_ASSERT(!trim_live(f.undo, old_branch));
    YEW_ASSERT(!trim_live(f.undo, newer_branch) ||
               newer_branch < protected_cur);
    YEW_ASSERT(trim_live(f.undo, protected_cur));
    YEW_ASSERT(trim_live(f.undo, f.undo->cur));
    YEW_ASSERT(trim_live(f.undo, f.undo->root));
    YEW_ASSERT(f.undo->bytes_live <= f.undo->bytes_max ||
               f.undo->over_budget_logged);
    trim_fixture_free(&f);
}

void test_undo_trim_never_prunes_current_or_saved_paths(void)
{
    TrimFixture f;
    u8 payload[768];
    u32 saved;
    u32 current;
    u32 id;

    (void)memset(payload, 's', sizeof(payload));
    trim_fixture_init(&f, 100U);
    saved = trim_append(&f, payload, sizeof(payload));
    yew_undo_mark_saved(f.undo);
    current = trim_append(&f, payload, sizeof(payload));
    for (u32 i = 0U; i < 30U; i++)
        (void)trim_append(&f, payload, sizeof(payload));
    yew_undo_set_limits(f.undo, 4096U, 3U, YEW_UNDO_PERSIST_BYTES_MAX);
    (void)trim_append(&f, payload, sizeof(payload));
    YEW_ASSERT(trim_live(f.undo, saved));
    YEW_ASSERT(trim_live(f.undo, current));
    YEW_ASSERT(trim_live(f.undo, f.undo->cur));
    id = f.undo->cur;
    while (id != 0U) {
        YEW_ASSERT(trim_live(f.undo, id));
        id = f.undo->nodes.data[id - 1U].parent;
    }
    id = f.undo->saved;
    while (id != 0U) {
        YEW_ASSERT(trim_live(f.undo, id));
        id = f.undo->nodes.data[id - 1U].parent;
    }
    trim_fixture_free(&f);
}

void test_undo_trim_min_nodes_keeps_soft_budget_history(void)
{
    TrimFixture f;
    u8 payload[2048];
    u32 i;

    (void)memset(payload, 'm', sizeof(payload));
    trim_fixture_init(&f, 40U);
    for (i = 0U; i < 39U; i++)
        (void)trim_append(&f, payload, sizeof(payload));
    YEW_ASSERT_EQ_U64(trim_live_count(f.undo), 40U);
    YEW_ASSERT(f.undo->bytes_live > f.undo->bytes_max);
    YEW_ASSERT(f.undo->over_budget_logged);
    YEW_ASSERT_EQ_U64(f.undo->root, 1U);
    YEW_ASSERT((f.undo->nodes.data[0].flags & YEW_TXN_DEAD) == 0U);
    YEW_ASSERT(yew_undo(&f.edit));
    YEW_ASSERT(yew_redo(&f.edit));
    trim_fixture_free(&f);
}

void test_undo_trim_reroot_stops_undo_at_oldest_survivor(void)
{
    TrimFixture f;
    u8 payload[1536];
    u32 original_root;
    u32 i;

    (void)memset(payload, 'r', sizeof(payload));
    trim_fixture_init(&f, 2U);
    original_root = f.undo->root;
    for (i = 0U; i < 40U; i++)
        (void)trim_append(&f, payload, sizeof(payload));
    YEW_ASSERT(f.undo->root != original_root);
    YEW_ASSERT(!trim_live(f.undo, original_root));
    YEW_ASSERT_EQ_U64(f.undo->nodes.data[f.undo->root - 1U].parent, 0U);
    YEW_ASSERT_EQ_U64(f.undo->nodes.data[f.undo->root - 1U].depth, 0U);
    YEW_ASSERT((f.undo->nodes.data[f.undo->root - 1U].flags &
                YEW_TXN_TRIMMED) != 0U);
    YEW_ASSERT(yew_undo_to(&f.edit, f.undo->root));
    YEW_ASSERT(!yew_undo(&f.edit));
    YEW_ASSERT_EQ_U64(yew_undo_current(f.undo), f.undo->root);
    trim_fixture_free(&f);
}

void test_undo_trim_compaction_preserves_delete_payloads(void)
{
    TrimFixture f;
    u8 initial[8192];
    u8 before[8193];
    u8 after[8193 - 256];
    const UndoNode *current;
    const UndoOp *op;
    u64 old_gen;
    u32 i;

    for (i = 0U; i < sizeof(initial); i++)
        initial[i] = (u8)(i * 17U + 3U);
    (void)memcpy(before, initial, sizeof(initial));
    before[sizeof(initial)] = 'x';
    (void)memcpy(after, before, 100U);
    (void)memcpy(after + 100U, before + 356U,
                 sizeof(before) - 356U);

    trim_fixture_init(&f, 1U);
    yew_undo_set_limits(f.undo, 1024U * 1024U, 1U,
                        YEW_UNDO_PERSIST_BYTES_MAX);
    (void)trim_append(&f, initial, sizeof(initial));
    yew_undo_mark_saved(f.undo);
    yew_undo_begin(&f.edit, YEW_TXN_CUT);
    yew_edit_delete(&f.edit, (Span){0U, 6144U});
    yew_undo_end(&f.edit);
    YEW_ASSERT(yew_undo(&f.edit));
    (void)trim_append(&f, (const u8 *)"x", 1U);
    old_gen = f.undo->gen;

    yew_undo_set_limits(f.undo, 2048U, 1U,
                        YEW_UNDO_PERSIST_BYTES_MAX);
    yew_undo_begin(&f.edit, YEW_TXN_CUT);
    yew_edit_delete(&f.edit, (Span){100U, 356U});
    yew_undo_end(&f.edit);

    YEW_ASSERT(f.undo->gen > old_gen);
    YEW_ASSERT_EQ_U64(f.undo->blobs.len, 256U);
    YEW_ASSERT_EQ_U64(f.undo->ops.len, 3U);
    YEW_ASSERT_EQ_U64(f.undo->repair_runs.len, 3U);
    YEW_ASSERT_EQ_U64(f.undo->replay_spans.len, 3U);
    YEW_ASSERT_EQ_U64(f.undo->cursors.len, 6U);
    current = &f.undo->nodes.data[f.undo->cur - 1U];
    YEW_ASSERT_EQ_U64(current->n_ops, 1U);
    op = &f.undo->ops.data[current->ops_at];
    YEW_ASSERT_EQ_U64(op->kind, YEW_OP_DEL);
    YEW_ASSERT_EQ_U64(op->payload, 0U);
    trim_assert_bytes(f.tb, after, sizeof(after));
    YEW_ASSERT(yew_undo(&f.edit));
    trim_assert_bytes(f.tb, before, sizeof(before));
    YEW_ASSERT(yew_redo(&f.edit));
    trim_assert_bytes(f.tb, after, sizeof(after));
    yew_textbuf_check(f.tb);
    trim_fixture_free(&f);
}

void test_undo_trim_random_edits_keep_live_parent_chains(void)
{
    enum { EDITS = 10000 };
    TrimFixture f;
    u64 state = UINT64_C(0x9e3779b97f4a7c15);
    u32 i;
    u32 checked = 0U;

    trim_fixture_init(&f, 32U);
    for (i = 0U; i < EDITS; i++) {
        u64 len = yew_textbuf_len(f.tb);
        u64 choice = trim_random(&state);

        f.mono++;
        yew_undo_begin(&f.edit,
                       (choice & 1U) != 0U ? YEW_TXN_PASTE : YEW_TXN_CUT);
        if (len == 0U || (choice & 3U) != 0U) {
            u8 byte = (u8)(choice >> 8U);
            u64 at = len == 0U ? 0U : trim_random(&state) % (len + 1U);
            yew_edit_insert(&f.edit, BYTEOFF(at), &byte, 1U);
        } else {
            u64 at = trim_random(&state) % len;
            yew_edit_delete(&f.edit, (Span){at, at + 1U});
        }
        yew_undo_end(&f.edit);
        if ((i % 19U) == 0U)
            (void)yew_undo(&f.edit);
        if ((i % 31U) == 0U)
            (void)yew_redo(&f.edit);
    }
    for (i = 1U; i <= f.undo->nodes.len; i++) {
        const UndoNode *node;

        if (!trim_live(f.undo, i))
            continue;
        node = &f.undo->nodes.data[i - 1U];
        YEW_ASSERT(node->parent == 0U || trim_live(f.undo, node->parent));
        YEW_ASSERT(node->parent == 0U
                       ? node->depth == 0U
                       : node->depth ==
                             f.undo->nodes.data[node->parent - 1U].depth +
                                 1U);
        YEW_ASSERT(yew_undo_to(&f.edit, i));
        YEW_ASSERT_EQ_U64(yew_undo_current(f.undo), i);
        checked++;
    }
    YEW_ASSERT(checked != 0U);
    YEW_ASSERT(trim_live(f.undo, f.undo->root));
    YEW_ASSERT(trim_live(f.undo, f.undo->cur));
    YEW_ASSERT(f.undo->ops.len < 512U);
    YEW_ASSERT(f.undo->cursors.len < 1024U);
    yew_textbuf_check(f.tb);
    trim_fixture_free(&f);
}
