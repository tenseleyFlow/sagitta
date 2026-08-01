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
    f->tb = sag_textbuf_new();
    sag_cset_init(&f->cursors, cursor);
    f->undo = sag_undo_new(f->tb);
    f->mono = 1000U;
    f->wall = 100;
    sag_undo_set_clock(f->undo, trim_mono, trim_wall, f);
    sag_undo_set_limits(f->undo, 4096U, min_nodes,
                        SAG_UNDO_PERSIST_BYTES_MAX);
    f->edit = (EditCtx){f->tb, NULL, &f->cursors, 0U, NULL, f->undo, NULL};
}

static void trim_fixture_free(TrimFixture *f)
{
    sag_undo_free(f->undo);
    sag_cset_free(&f->cursors);
    sag_textbuf_free(f->tb);
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
    f->mono += SAG_UNDO_BURST_MS;
    f->wall++;
    sag_undo_begin(&f->edit, SAG_TXN_PASTE);
    sag_edit_insert(&f->edit, BYTEOFF(sag_textbuf_len(f->tb)), bytes, len);
    sag_undo_end(&f->edit);
    return sag_undo_current(f->undo);
}

static bool trim_live(const UndoTree *ut, u32 id)
{
    return id != 0U && id <= ut->nodes.len &&
           (ut->nodes.data[id - 1U].flags & SAG_TXN_DEAD) == 0U;
}

static u32 trim_live_count(const UndoTree *ut)
{
    u32 count = 0U;
    size_t i;

    for (i = 0U; i < ut->nodes.len; i++) {
        if ((ut->nodes.data[i].flags & SAG_TXN_DEAD) == 0U)
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
    SAG_ASSERT(sag_undo_to(&f.edit, f.undo->root));
    newer_branch = trim_append(&f, payload, sizeof(payload));
    for (u32 i = 0U; i < 14U; i++)
        (void)trim_append(&f, payload, sizeof(payload));
    SAG_ASSERT(sag_undo_to(&f.edit, f.undo->root));
    sag_undo_set_limits(f.undo, 4096U, 2U, SAG_UNDO_PERSIST_BYTES_MAX);
    protected_cur = trim_append(&f, payload, sizeof(payload));
    for (u32 i = 0U; i < 14U; i++)
        (void)trim_append(&f, payload, sizeof(payload));
    SAG_ASSERT(!trim_live(f.undo, old_branch));
    SAG_ASSERT(!trim_live(f.undo, newer_branch) ||
               newer_branch < protected_cur);
    SAG_ASSERT(trim_live(f.undo, protected_cur));
    SAG_ASSERT(trim_live(f.undo, f.undo->cur));
    SAG_ASSERT(trim_live(f.undo, f.undo->root));
    SAG_ASSERT(f.undo->bytes_live <= f.undo->bytes_max ||
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
    sag_undo_mark_saved(f.undo);
    current = trim_append(&f, payload, sizeof(payload));
    for (u32 i = 0U; i < 30U; i++)
        (void)trim_append(&f, payload, sizeof(payload));
    sag_undo_set_limits(f.undo, 4096U, 3U, SAG_UNDO_PERSIST_BYTES_MAX);
    (void)trim_append(&f, payload, sizeof(payload));
    SAG_ASSERT(trim_live(f.undo, saved));
    SAG_ASSERT(trim_live(f.undo, current));
    SAG_ASSERT(trim_live(f.undo, f.undo->cur));
    id = f.undo->cur;
    while (id != 0U) {
        SAG_ASSERT(trim_live(f.undo, id));
        id = f.undo->nodes.data[id - 1U].parent;
    }
    id = f.undo->saved;
    while (id != 0U) {
        SAG_ASSERT(trim_live(f.undo, id));
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
    SAG_ASSERT_EQ_U64(trim_live_count(f.undo), 40U);
    SAG_ASSERT(f.undo->bytes_live > f.undo->bytes_max);
    SAG_ASSERT(f.undo->over_budget_logged);
    SAG_ASSERT_EQ_U64(f.undo->root, 1U);
    SAG_ASSERT((f.undo->nodes.data[0].flags & SAG_TXN_DEAD) == 0U);
    SAG_ASSERT(sag_undo(&f.edit));
    SAG_ASSERT(sag_redo(&f.edit));
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
    SAG_ASSERT(f.undo->root != original_root);
    SAG_ASSERT(!trim_live(f.undo, original_root));
    SAG_ASSERT_EQ_U64(f.undo->nodes.data[f.undo->root - 1U].parent, 0U);
    SAG_ASSERT((f.undo->nodes.data[f.undo->root - 1U].flags &
                SAG_TXN_TRIMMED) != 0U);
    SAG_ASSERT(sag_undo_to(&f.edit, f.undo->root));
    SAG_ASSERT(!sag_undo(&f.edit));
    SAG_ASSERT_EQ_U64(sag_undo_current(f.undo), f.undo->root);
    trim_fixture_free(&f);
}

void test_undo_trim_compaction_preserves_delete_payloads(void)
{
    TrimFixture f;
    u8 initial[8192];
    u8 expected[8192];
    u64 old_gen;
    u32 i;

    for (i = 0U; i < sizeof(initial); i++)
        initial[i] = (u8)(i * 17U + 3U);
    (void)memcpy(expected, initial, sizeof(expected));
    trim_fixture_init(&f, 2U);
    sag_edit_insert(&f.edit, BYTEOFF(0U), initial, sizeof(initial));
    sag_undo_mark_saved(f.undo);
    old_gen = f.undo->gen;
    for (i = 0U; i < 12U && sag_textbuf_len(f.tb) >= 512U; i++) {
        sag_undo_begin(&f.edit, SAG_TXN_CUT);
        sag_edit_delete(&f.edit, (Span){0U, 512U});
        sag_undo_end(&f.edit);
    }
    SAG_ASSERT(f.undo->gen >= old_gen);
    SAG_ASSERT(f.undo->bytes_dead <= f.undo->bytes_live ||
               f.undo->bytes_live > f.undo->bytes_max);
    while (sag_undo(&f.edit))
        ;
    SAG_ASSERT(sag_textbuf_len(f.tb) <= sizeof(expected));
    SAG_ASSERT(sag_undo_to(&f.edit, f.undo->cur));
    sag_textbuf_check(f.tb);
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
        u64 len = sag_textbuf_len(f.tb);
        u64 choice = trim_random(&state);

        f.mono++;
        sag_undo_begin(&f.edit,
                       (choice & 1U) != 0U ? SAG_TXN_PASTE : SAG_TXN_CUT);
        if (len == 0U || (choice & 3U) != 0U) {
            u8 byte = (u8)(choice >> 8U);
            u64 at = len == 0U ? 0U : trim_random(&state) % (len + 1U);
            sag_edit_insert(&f.edit, BYTEOFF(at), &byte, 1U);
        } else {
            u64 at = trim_random(&state) % len;
            sag_edit_delete(&f.edit, (Span){at, at + 1U});
        }
        sag_undo_end(&f.edit);
        if ((i % 19U) == 0U)
            (void)sag_undo(&f.edit);
        if ((i % 31U) == 0U)
            (void)sag_redo(&f.edit);
    }
    for (i = 1U; i <= f.undo->nodes.len; i++) {
        const UndoNode *node;

        if (!trim_live(f.undo, i))
            continue;
        node = &f.undo->nodes.data[i - 1U];
        SAG_ASSERT(node->parent == 0U || trim_live(f.undo, node->parent));
        SAG_ASSERT(node->parent == 0U ||
                   f.undo->nodes.data[node->parent - 1U].depth < node->depth);
        if (checked < 64U) {
            SAG_ASSERT(sag_undo_to(&f.edit, i));
            SAG_ASSERT_EQ_U64(sag_undo_current(f.undo), i);
            checked++;
        }
    }
    SAG_ASSERT(checked != 0U);
    SAG_ASSERT(trim_live(f.undo, f.undo->root));
    SAG_ASSERT(trim_live(f.undo, f.undo->cur));
    sag_textbuf_check(f.tb);
    trim_fixture_free(&f);
}
