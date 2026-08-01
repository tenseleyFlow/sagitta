#include "harness.h"

#include "text/edit.h"

typedef struct {
    u64 mono;
    i64 wall;
} TreeClock;

typedef struct {
    TextBuf *tb;
    CursorSet cursors;
    UndoTree *undo;
    EditCtx edit;
    TreeClock clock;
} TreeFixture;

static u64 tree_mono(void *ctx)
{
    return ((TreeClock *)ctx)->mono;
}

static i64 tree_wall(void *ctx)
{
    return ((TreeClock *)ctx)->wall;
}

static void tree_fixture_init(TreeFixture *f)
{
    Cursor cursor;

    cursor.pos = BYTEOFF(0U);
    cursor.goal_col = (GCol){0U};
    cursor.anchor = BYTEOFF(0U);
    f->tb = sag_textbuf_new();
    sag_cset_init(&f->cursors, cursor);
    f->undo = sag_undo_new(f->tb);
    f->clock = (TreeClock){1000U, 100};
    sag_undo_set_clock(f->undo, tree_mono, tree_wall, &f->clock);
    f->edit = (EditCtx){f->tb, NULL, &f->cursors, 1U, NULL, f->undo, NULL};
}

static void tree_fixture_free(TreeFixture *f)
{
    sag_undo_free(f->undo);
    sag_cset_free(&f->cursors);
    sag_textbuf_free(f->tb);
}

static u32 tree_append(TreeFixture *f, u8 byte)
{
    f->clock.mono += SAG_UNDO_BURST_MS;
    f->clock.wall++;
    sag_undo_begin(&f->edit, SAG_TXN_PASTE);
    sag_edit_insert(&f->edit, BYTEOFF(sag_textbuf_len(f->tb)), &byte, 1U);
    sag_undo_end(&f->edit);
    return sag_undo_current(f->undo);
}

static void tree_assert_text(const TreeFixture *f, const char *want)
{
    TextIter it;
    u64 want_len = strlen(want);
    u64 done = 0U;

    SAG_ASSERT_EQ_U64(sag_textbuf_len(f->tb), want_len);
    if (want_len == 0U)
        return;
    SAG_ASSERT(sag_textiter_begin(&it, f->tb, BYTEOFF(0U)));
    while (done < want_len) {
        const u8 *bytes;
        u64 len;
        u64 take;

        SAG_ASSERT(sag_textiter_chunk(&it, f->tb, &bytes, &len));
        take = len < want_len - done ? len : want_len - done;
        SAG_ASSERT_EQ_MEM(bytes, (const u8 *)want + done, take);
        done += take;
        if (done < want_len)
            SAG_ASSERT(sag_textiter_advance(&it, f->tb));
    }
}

static const UndoNode *tree_node(const UndoTree *ut, u32 id)
{
    SAG_ASSERT(id != 0U);
    SAG_ASSERT(id <= ut->nodes.len);
    return &ut->nodes.data[id - 1U];
}

void test_undo_tree_preserves_abandoned_branch(void)
{
    TreeFixture f;
    u32 a;
    u32 b;
    u32 children[4] = {0U};

    tree_fixture_init(&f);
    a = tree_append(&f, 'A');
    SAG_ASSERT(sag_undo(&f.edit));
    b = tree_append(&f, 'B');
    SAG_ASSERT(a != b);
    SAG_ASSERT_EQ_U64(f.undo->nodes.len, 3U);
    SAG_ASSERT_EQ_U64(sag_undo_children(f.undo, f.undo->root, children, 4U),
                      2U);
    SAG_ASSERT_EQ_U64(children[0], a);
    SAG_ASSERT_EQ_U64(children[1], b);
    SAG_ASSERT(sag_undo_to(&f.edit, a));
    tree_assert_text(&f, "A");
    SAG_ASSERT(sag_undo_to(&f.edit, b));
    tree_assert_text(&f, "B");
    SAG_ASSERT((tree_node(f.undo, a)->flags & SAG_TXN_DEAD) == 0U);
    tree_fixture_free(&f);
}

void test_undo_tree_reopen_rejects_non_tail_branch_leaf(void)
{
    TreeFixture f;
    u32 a;
    u32 b;
    u32 c;
    u32 children[2] = {0U};

    tree_fixture_init(&f);
    a = tree_append(&f, 'A');
    SAG_ASSERT(sag_undo(&f.edit));
    b = tree_append(&f, 'B');
    SAG_ASSERT(sag_undo_to(&f.edit, a));
    SAG_ASSERT(!sag_undo_reopen(&f.edit, SAG_TXN_PASTE));
    SAG_ASSERT_EQ_U64(f.undo->depth, 0U);
    SAG_ASSERT_EQ_U64(f.undo->open, 0U);
    SAG_ASSERT(!f.undo->reopened);
    tree_assert_text(&f, "A");

    c = tree_append(&f, 'C');
    tree_assert_text(&f, "AC");
    SAG_ASSERT_EQ_U64(tree_node(f.undo, c)->parent, a);
    SAG_ASSERT_EQ_U64(sag_undo_children(f.undo, f.undo->root, children, 2U),
                      2U);
    SAG_ASSERT_EQ_U64(children[0], a);
    SAG_ASSERT_EQ_U64(children[1], b);
    SAG_ASSERT(sag_undo(&f.edit));
    tree_assert_text(&f, "A");
    SAG_ASSERT(sag_undo_to(&f.edit, b));
    tree_assert_text(&f, "B");
    tree_fixture_free(&f);
}

void test_undo_tree_reopen_abort_preserves_original_node(void)
{
    TreeFixture f;
    u32 node_id;
    size_t ops_len;
    size_t repairs_len;
    size_t cursors_len;
    size_t blobs_len;
    u32 node_ops;

    tree_fixture_init(&f);
    node_id = tree_append(&f, 'A');
    ops_len = f.undo->ops.len;
    repairs_len = f.undo->repairs.len;
    cursors_len = f.undo->cursors.len;
    blobs_len = f.undo->blobs.len;
    node_ops = tree_node(f.undo, node_id)->n_ops;

    SAG_ASSERT(sag_undo_reopen(&f.edit, SAG_TXN_PASTE));
    sag_edit_insert(&f.edit, BYTEOFF(1U), (const u8 *)"B", 1U);
    tree_assert_text(&f, "AB");
    sag_undo_abort(&f.edit);

    tree_assert_text(&f, "A");
    SAG_ASSERT_EQ_U64(sag_undo_current(f.undo), node_id);
    SAG_ASSERT_EQ_U64(f.undo->depth, 0U);
    SAG_ASSERT_EQ_U64(f.undo->open, 0U);
    SAG_ASSERT(!f.undo->reopened);
    SAG_ASSERT_EQ_U64(f.undo->nodes.len, 2U);
    SAG_ASSERT_EQ_U64(tree_node(f.undo, node_id)->n_ops, node_ops);
    SAG_ASSERT_EQ_U64(f.undo->ops.len, ops_len);
    SAG_ASSERT_EQ_U64(f.undo->repairs.len, repairs_len);
    SAG_ASSERT_EQ_U64(f.undo->cursors.len, cursors_len);
    SAG_ASSERT_EQ_U64(f.undo->blobs.len, blobs_len);
    SAG_ASSERT(sag_undo(&f.edit));
    tree_assert_text(&f, "");
    SAG_ASSERT(sag_redo(&f.edit));
    tree_assert_text(&f, "A");
    tree_fixture_free(&f);
}

void test_undo_tree_redo_child_tracks_most_recent_traversal(void)
{
    TreeFixture f;
    u32 a;
    u32 b;

    tree_fixture_init(&f);
    a = tree_append(&f, 'A');
    SAG_ASSERT(sag_undo(&f.edit));
    b = tree_append(&f, 'B');
    SAG_ASSERT_EQ_U64(tree_node(f.undo, f.undo->root)->redo_child, b);
    SAG_ASSERT(sag_undo(&f.edit));
    SAG_ASSERT(sag_undo_to(&f.edit, a));
    SAG_ASSERT_EQ_U64(tree_node(f.undo, f.undo->root)->redo_child, a);
    SAG_ASSERT(sag_undo(&f.edit));
    SAG_ASSERT(sag_redo(&f.edit));
    SAG_ASSERT_EQ_U64(sag_undo_current(f.undo), a);
    tree_assert_text(&f, "A");
    tree_fixture_free(&f);
}

void test_undo_tree_branch_cycle_rotates_live_siblings(void)
{
    TreeFixture f;
    u32 a;
    u32 b;

    tree_fixture_init(&f);
    a = tree_append(&f, 'A');
    SAG_ASSERT(sag_undo(&f.edit));
    b = tree_append(&f, 'B');
    SAG_ASSERT(sag_undo(&f.edit));
    SAG_ASSERT_EQ_U64(sag_undo_branch_cycle(f.undo, 1), a);
    SAG_ASSERT_EQ_U64(tree_node(f.undo, f.undo->root)->redo_child, a);
    SAG_ASSERT_EQ_U64(sag_undo_branch_cycle(f.undo, 1), b);
    SAG_ASSERT_EQ_U64(sag_undo_branch_cycle(f.undo, -1), a);
    SAG_ASSERT_EQ_U64(sag_undo_current(f.undo), f.undo->root);
    tree_fixture_free(&f);
}

void test_undo_tree_state_navigation_crosses_branches_by_id(void)
{
    TreeFixture f;
    u32 a;
    u32 b;

    tree_fixture_init(&f);
    a = tree_append(&f, 'A');
    SAG_ASSERT(sag_undo(&f.edit));
    b = tree_append(&f, 'B');
    SAG_ASSERT(sag_undo_to(&f.edit, a));
    SAG_ASSERT(sag_undo_state(&f.edit, 1));
    SAG_ASSERT_EQ_U64(sag_undo_current(f.undo), b);
    tree_assert_text(&f, "B");
    SAG_ASSERT(sag_undo_state(&f.edit, -1));
    SAG_ASSERT_EQ_U64(sag_undo_current(f.undo), a);
    tree_assert_text(&f, "A");
    SAG_ASSERT(!sag_undo_state(&f.edit, -1) ||
               sag_undo_current(f.undo) == f.undo->root);
    tree_fixture_free(&f);
}

void test_undo_tree_wall_clock_is_clamped_monotone(void)
{
    TreeFixture f;
    u32 a;
    u32 b;

    tree_fixture_init(&f);
    f.clock.wall = 500;
    a = tree_append(&f, 'A');
    f.clock.wall = 100;
    b = tree_append(&f, 'B');
    SAG_ASSERT(tree_node(f.undo, a)->t_wall >= 501);
    SAG_ASSERT_EQ_I64(tree_node(f.undo, b)->t_wall,
                      tree_node(f.undo, a)->t_wall);
    SAG_ASSERT(sag_undo_to(&f.edit, f.undo->root));
    SAG_ASSERT(sag_undo_time(&f.edit, tree_node(f.undo, b)->t_wall));
    SAG_ASSERT_EQ_U64(sag_undo_current(f.undo), b);
    tree_assert_text(&f, "AB");
    tree_fixture_free(&f);
}

void test_undo_tree_lca_moves_between_deep_branches(void)
{
    TreeFixture f;
    u32 trunk[5];
    u32 branch_a[3];
    u32 branch_b[3];
    size_t i;

    tree_fixture_init(&f);
    for (i = 0U; i < 5U; i++)
        trunk[i] = tree_append(&f, (u8)('0' + i));
    SAG_ASSERT(sag_undo_to(&f.edit, trunk[1]));
    for (i = 0U; i < 3U; i++)
        branch_a[i] = tree_append(&f, (u8)('a' + i));
    SAG_ASSERT(sag_undo_to(&f.edit, trunk[1]));
    for (i = 0U; i < 3U; i++)
        branch_b[i] = tree_append(&f, (u8)('x' + i));
    SAG_ASSERT_EQ_U64(tree_node(f.undo, branch_a[2])->depth, 5U);
    SAG_ASSERT_EQ_U64(tree_node(f.undo, branch_b[2])->depth, 5U);
    SAG_ASSERT(sag_undo_to(&f.edit, branch_a[2]));
    tree_assert_text(&f, "01abc");
    SAG_ASSERT(sag_undo_to(&f.edit, branch_b[2]));
    tree_assert_text(&f, "01xyz");
    SAG_ASSERT_EQ_U64(sag_undo_current(f.undo), branch_b[2]);
    SAG_ASSERT_EQ_U64(tree_node(f.undo, branch_b[0])->parent, trunk[1]);
    SAG_ASSERT_EQ_U64(tree_node(f.undo, branch_a[0])->parent, trunk[1]);
    tree_fixture_free(&f);
}

void test_undo_tree_save_point_tracks_current_state(void)
{
    TreeFixture f;
    u32 saved;

    tree_fixture_init(&f);
    (void)tree_append(&f, 'A');
    sag_undo_mark_saved(f.undo);
    saved = sag_undo_current(f.undo);
    SAG_ASSERT(sag_undo_at_save_point(f.undo));
    SAG_ASSERT_EQ_U64(f.undo->saved, saved);
    SAG_ASSERT((tree_node(f.undo, saved)->flags & SAG_TXN_SAVED) != 0U);
    (void)tree_append(&f, 'B');
    SAG_ASSERT(!sag_undo_at_save_point(f.undo));
    SAG_ASSERT(sag_undo(&f.edit));
    SAG_ASSERT(sag_undo_at_save_point(f.undo));
    SAG_ASSERT_EQ_U64(sag_undo_current(f.undo), saved);
    tree_fixture_free(&f);
}

void test_undo_tree_query_reports_path_and_byte_totals(void)
{
    TreeFixture f;
    UndoNodeInfo info[8];
    u32 count;
    char desc[128];

    tree_fixture_init(&f);
    sag_undo_begin(&f.edit, SAG_TXN_PASTE);
    sag_edit_insert(&f.edit, BYTEOFF(0U), (const u8 *)"abcd", 4U);
    sag_edit_delete(&f.edit, (Span){1U, 3U});
    sag_undo_end(&f.edit);
    count = sag_undo_list(f.undo, info, SAG_ARRAY_LEN(info));
    SAG_ASSERT_EQ_U64(count, 2U);
    SAG_ASSERT(info[0].on_current_path);
    SAG_ASSERT(info[1].on_current_path);
    SAG_ASSERT(info[1].is_current);
    SAG_ASSERT_EQ_U64(info[1].bytes_ins, 4U);
    SAG_ASSERT_EQ_U64(info[1].bytes_del, 2U);
    SAG_ASSERT_EQ_U64(info[1].n_children, 0U);
    sag_undo_describe(f.undo, info[1].id, f.clock.wall + 180, desc,
                      sizeof(desc));
    SAG_ASSERT(strstr(desc, "+4") != NULL);
    SAG_ASSERT(strstr(desc, "-2") != NULL || strstr(desc, "\xe2\x88\x92" "2") != NULL);
    tree_fixture_free(&f);
}
