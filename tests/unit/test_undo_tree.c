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
    f->tb = yew_textbuf_new();
    yew_cset_init(&f->cursors, cursor);
    f->undo = yew_undo_new(f->tb);
    f->clock = (TreeClock){1000U, 100};
    yew_undo_set_clock(f->undo, tree_mono, tree_wall, &f->clock);
    f->edit = (EditCtx){f->tb, NULL, &f->cursors, 1U, NULL, f->undo, NULL,
                       NULL, NULL, 0, NULL, NULL, {0}, 0U};
}

static void tree_fixture_free(TreeFixture *f)
{
    yew_undo_free(f->undo);
    yew_cset_free(&f->cursors);
    yew_textbuf_free(f->tb);
}

static u32 tree_append(TreeFixture *f, u8 byte)
{
    f->clock.mono += YEW_UNDO_BURST_MS;
    f->clock.wall++;
    yew_undo_begin(&f->edit, YEW_TXN_PASTE);
    yew_edit_insert(&f->edit, BYTEOFF(yew_textbuf_len(f->tb)), &byte, 1U);
    yew_undo_end(&f->edit);
    return yew_undo_current(f->undo);
}

static void tree_assert_text(const TreeFixture *f, const char *want)
{
    TextIter it;
    u64 want_len = strlen(want);
    u64 done = 0U;

    YEW_ASSERT_EQ_U64(yew_textbuf_len(f->tb), want_len);
    if (want_len == 0U)
        return;
    YEW_ASSERT(yew_textiter_begin(&it, f->tb, BYTEOFF(0U)));
    while (done < want_len) {
        const u8 *bytes;
        u64 len;
        u64 take;

        YEW_ASSERT(yew_textiter_chunk(&it, f->tb, &bytes, &len));
        take = len < want_len - done ? len : want_len - done;
        YEW_ASSERT_EQ_MEM(bytes, (const u8 *)want + done, take);
        done += take;
        if (done < want_len)
            YEW_ASSERT(yew_textiter_advance(&it, f->tb));
    }
}

static const UndoNode *tree_node(const UndoTree *ut, u32 id)
{
    YEW_ASSERT(id != 0U);
    YEW_ASSERT(id <= ut->nodes.len);
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
    YEW_ASSERT(yew_undo(&f.edit));
    b = tree_append(&f, 'B');
    YEW_ASSERT(a != b);
    YEW_ASSERT_EQ_U64(f.undo->nodes.len, 3U);
    YEW_ASSERT_EQ_U64(yew_undo_children(f.undo, f.undo->root, children, 4U),
                      2U);
    YEW_ASSERT_EQ_U64(children[0], a);
    YEW_ASSERT_EQ_U64(children[1], b);
    YEW_ASSERT(yew_undo_to(&f.edit, a));
    tree_assert_text(&f, "A");
    YEW_ASSERT(yew_undo_to(&f.edit, b));
    tree_assert_text(&f, "B");
    YEW_ASSERT((tree_node(f.undo, a)->flags & YEW_TXN_DEAD) == 0U);
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
    YEW_ASSERT(yew_undo(&f.edit));
    b = tree_append(&f, 'B');
    YEW_ASSERT(yew_undo_to(&f.edit, a));
    YEW_ASSERT(!yew_undo_reopen(&f.edit, YEW_TXN_PASTE));
    YEW_ASSERT_EQ_U64(f.undo->depth, 0U);
    YEW_ASSERT_EQ_U64(f.undo->open, 0U);
    YEW_ASSERT(!f.undo->reopened);
    tree_assert_text(&f, "A");

    c = tree_append(&f, 'C');
    tree_assert_text(&f, "AC");
    YEW_ASSERT_EQ_U64(tree_node(f.undo, c)->parent, a);
    YEW_ASSERT_EQ_U64(yew_undo_children(f.undo, f.undo->root, children, 2U),
                      2U);
    YEW_ASSERT_EQ_U64(children[0], a);
    YEW_ASSERT_EQ_U64(children[1], b);
    YEW_ASSERT(yew_undo(&f.edit));
    tree_assert_text(&f, "A");
    YEW_ASSERT(yew_undo_to(&f.edit, b));
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

    YEW_ASSERT(yew_undo_reopen(&f.edit, YEW_TXN_PASTE));
    yew_edit_insert(&f.edit, BYTEOFF(1U), (const u8 *)"B", 1U);
    tree_assert_text(&f, "AB");
    yew_undo_abort(&f.edit);

    tree_assert_text(&f, "A");
    YEW_ASSERT_EQ_U64(yew_undo_current(f.undo), node_id);
    YEW_ASSERT_EQ_U64(f.undo->depth, 0U);
    YEW_ASSERT_EQ_U64(f.undo->open, 0U);
    YEW_ASSERT(!f.undo->reopened);
    YEW_ASSERT_EQ_U64(f.undo->nodes.len, 2U);
    YEW_ASSERT_EQ_U64(tree_node(f.undo, node_id)->n_ops, node_ops);
    YEW_ASSERT_EQ_U64(f.undo->ops.len, ops_len);
    YEW_ASSERT_EQ_U64(f.undo->repairs.len, repairs_len);
    YEW_ASSERT_EQ_U64(f.undo->cursors.len, cursors_len);
    YEW_ASSERT_EQ_U64(f.undo->blobs.len, blobs_len);
    YEW_ASSERT(yew_undo(&f.edit));
    tree_assert_text(&f, "");
    YEW_ASSERT(yew_redo(&f.edit));
    tree_assert_text(&f, "A");
    tree_fixture_free(&f);
}

void test_undo_tree_reopen_commit_reuses_cursor_snapshot_storage(void)
{
    TreeFixture f;
    u32 node_id;
    u32 i;

    tree_fixture_init(&f);
    node_id = tree_append(&f, 'A');
    YEW_ASSERT_EQ_U64(f.undo->cursors.len, 2U);
    for (i = 0U; i < 100U; i++) {
        YEW_ASSERT(yew_undo_reopen(&f.edit, YEW_TXN_PASTE));
        yew_edit_insert(&f.edit, BYTEOFF(yew_textbuf_len(f.tb)),
                        (const u8 *)"x", 1U);
        yew_undo_end(&f.edit);
        YEW_ASSERT_EQ_U64(yew_undo_current(f.undo), node_id);
        YEW_ASSERT_EQ_U64(f.undo->cursors.len, 2U);
        YEW_ASSERT_EQ_U64(tree_node(f.undo, node_id)->cur_before, 0U);
        YEW_ASSERT_EQ_U64(tree_node(f.undo, node_id)->cur_after, 1U);
    }
    YEW_ASSERT_EQ_U64(tree_node(f.undo, node_id)->n_ops, 101U);
    YEW_ASSERT(yew_undo(&f.edit));
    tree_assert_text(&f, "");
    YEW_ASSERT(yew_redo(&f.edit));
    YEW_ASSERT_EQ_U64(yew_textbuf_len(f.tb), 101U);
    tree_fixture_free(&f);
}

void test_undo_tree_redo_child_tracks_most_recent_traversal(void)
{
    TreeFixture f;
    u32 a;
    u32 b;

    tree_fixture_init(&f);
    a = tree_append(&f, 'A');
    YEW_ASSERT(yew_undo(&f.edit));
    b = tree_append(&f, 'B');
    YEW_ASSERT_EQ_U64(tree_node(f.undo, f.undo->root)->redo_child, b);
    YEW_ASSERT(yew_undo(&f.edit));
    YEW_ASSERT(yew_undo_to(&f.edit, a));
    YEW_ASSERT_EQ_U64(tree_node(f.undo, f.undo->root)->redo_child, a);
    YEW_ASSERT(yew_undo(&f.edit));
    YEW_ASSERT(yew_redo(&f.edit));
    YEW_ASSERT_EQ_U64(yew_undo_current(f.undo), a);
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
    YEW_ASSERT(yew_undo(&f.edit));
    b = tree_append(&f, 'B');
    YEW_ASSERT(yew_undo(&f.edit));
    YEW_ASSERT_EQ_U64(yew_undo_branch_cycle(f.undo, 1), a);
    YEW_ASSERT_EQ_U64(tree_node(f.undo, f.undo->root)->redo_child, a);
    YEW_ASSERT_EQ_U64(yew_undo_branch_cycle(f.undo, 1), b);
    YEW_ASSERT_EQ_U64(yew_undo_branch_cycle(f.undo, -1), a);
    YEW_ASSERT_EQ_U64(yew_undo_current(f.undo), f.undo->root);
    tree_fixture_free(&f);
}

void test_undo_tree_state_navigation_crosses_branches_by_id(void)
{
    TreeFixture f;
    u32 a;
    u32 b;

    tree_fixture_init(&f);
    a = tree_append(&f, 'A');
    YEW_ASSERT(yew_undo(&f.edit));
    b = tree_append(&f, 'B');
    YEW_ASSERT(yew_undo_to(&f.edit, a));
    YEW_ASSERT(yew_undo_state(&f.edit, 1));
    YEW_ASSERT_EQ_U64(yew_undo_current(f.undo), b);
    tree_assert_text(&f, "B");
    YEW_ASSERT(yew_undo_state(&f.edit, -1));
    YEW_ASSERT_EQ_U64(yew_undo_current(f.undo), a);
    tree_assert_text(&f, "A");
    YEW_ASSERT(!yew_undo_state(&f.edit, -1) ||
               yew_undo_current(f.undo) == f.undo->root);
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
    YEW_ASSERT(tree_node(f.undo, a)->t_wall >= 501);
    YEW_ASSERT_EQ_I64(tree_node(f.undo, b)->t_wall,
                      tree_node(f.undo, a)->t_wall);
    YEW_ASSERT(yew_undo_to(&f.edit, f.undo->root));
    YEW_ASSERT(yew_undo_time(&f.edit, tree_node(f.undo, b)->t_wall));
    YEW_ASSERT_EQ_U64(yew_undo_current(f.undo), b);
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
    YEW_ASSERT(yew_undo_to(&f.edit, trunk[1]));
    for (i = 0U; i < 3U; i++)
        branch_a[i] = tree_append(&f, (u8)('a' + i));
    YEW_ASSERT(yew_undo_to(&f.edit, trunk[1]));
    for (i = 0U; i < 3U; i++)
        branch_b[i] = tree_append(&f, (u8)('x' + i));
    YEW_ASSERT_EQ_U64(tree_node(f.undo, branch_a[2])->depth, 5U);
    YEW_ASSERT_EQ_U64(tree_node(f.undo, branch_b[2])->depth, 5U);
    YEW_ASSERT(yew_undo_to(&f.edit, branch_a[2]));
    tree_assert_text(&f, "01abc");
    YEW_ASSERT(yew_undo_to(&f.edit, branch_b[2]));
    tree_assert_text(&f, "01xyz");
    YEW_ASSERT_EQ_U64(yew_undo_current(f.undo), branch_b[2]);
    YEW_ASSERT_EQ_U64(tree_node(f.undo, branch_b[0])->parent, trunk[1]);
    YEW_ASSERT_EQ_U64(tree_node(f.undo, branch_a[0])->parent, trunk[1]);
    tree_fixture_free(&f);
}

void test_undo_tree_save_point_tracks_current_state(void)
{
    TreeFixture f;
    u32 saved;

    tree_fixture_init(&f);
    (void)tree_append(&f, 'A');
    yew_undo_mark_saved(f.undo);
    saved = yew_undo_current(f.undo);
    YEW_ASSERT(yew_undo_at_save_point(f.undo));
    YEW_ASSERT_EQ_U64(f.undo->saved, saved);
    YEW_ASSERT((tree_node(f.undo, saved)->flags & YEW_TXN_SAVED) != 0U);
    (void)tree_append(&f, 'B');
    YEW_ASSERT(!yew_undo_at_save_point(f.undo));
    YEW_ASSERT(yew_undo(&f.edit));
    YEW_ASSERT(yew_undo_at_save_point(f.undo));
    YEW_ASSERT_EQ_U64(yew_undo_current(f.undo), saved);
    tree_fixture_free(&f);
}

void test_undo_tree_query_reports_path_and_byte_totals(void)
{
    TreeFixture f;
    UndoNodeInfo info[8];
    u32 count;
    char desc[128];

    tree_fixture_init(&f);
    yew_undo_begin(&f.edit, YEW_TXN_PASTE);
    yew_edit_insert(&f.edit, BYTEOFF(0U), (const u8 *)"abcd", 4U);
    yew_edit_delete(&f.edit, (Span){1U, 3U});
    yew_undo_end(&f.edit);
    count = yew_undo_list(f.undo, info, YEW_ARRAY_LEN(info));
    YEW_ASSERT_EQ_U64(count, 2U);
    YEW_ASSERT(info[0].on_current_path);
    YEW_ASSERT(info[1].on_current_path);
    YEW_ASSERT(info[1].is_current);
    YEW_ASSERT_EQ_U64(info[1].bytes_ins, 4U);
    YEW_ASSERT_EQ_U64(info[1].bytes_del, 2U);
    YEW_ASSERT_EQ_U64(info[1].n_children, 0U);
    yew_undo_describe(f.undo, info[1].id, f.clock.wall + 180, desc,
                      sizeof(desc));
    YEW_ASSERT(strstr(desc, "+4") != NULL);
    YEW_ASSERT(strstr(desc, "-2") != NULL || strstr(desc, "\xe2\x88\x92" "2") != NULL);
    tree_fixture_free(&f);
}
