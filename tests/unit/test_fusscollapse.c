/* Sprint 52: collapsed paths and selections survive structural refreshes. */
#include "harness.h"

#include <stdio.h>
#include <string.h>

#include "mod/git/fusstree.h"

#define FC_DIRS 200U

static void fc_entries(GitEntry entries[FC_DIRS], char paths[FC_DIRS][24])
{
    size_t i;

    for (i = 0U; i < FC_DIRS; i++) {
        int n = snprintf(paths[i], 24U, "d%03u/file.c", (unsigned)i);
        YEW_ASSERT(n > 0 && n < 24);
        (void)memset(&entries[i], 0, sizeof(entries[i]));
        entries[i].kind = GIT_E_ORDINARY;
        entries[i].path = paths[i];
        entries[i].path_len = (u32)n;
        entries[i].unstaged = true;
    }
}

static GitSnapshot fc_snapshot(GitEntry *entries, size_t n, u32 gen)
{
    GitSnapshot s;

    (void)memset(&s, 0, sizeof(s));
    s.state = YEW_GIT_OK;
    s.entries.data = entries;
    s.entries.len = n;
    s.gen = gen;
    return s;
}

static i32 fc_row(const FussTree *t, const char *path)
{
    FussSel sel;
    i32 row;

    (void)memset(&sel, 0, sizeof(sel));
    yew_fuss_sel_set(&sel, path, (u32)strlen(path));
    row = yew_fuss_row_of(t, &sel);
    yew_fuss_sel_clear(&sel);
    return row;
}

static FussNode *fc_node(FussTree *t, const char *path)
{
    size_t i;
    size_t n = strlen(path);

    for (i = 0U; i < t->items.len; i++)
        if (t->items.data[i].path_len == n &&
            memcmp(t->items.data[i].path, path, n) == 0)
            return &t->nodes.data[t->items.data[i].node];
    return NULL;
}

static void fc_collapse_seven(FussTree *t)
{
    static const char *const paths[] = {
        "d003", "d019", "d041", "d088", "d109", "d157", "d199"
    };
    size_t i;

    for (i = 0U; i < YEW_ARRAY_LEN(paths); i++) {
        FussNode *n = fc_node(t, paths[i]);
        YEW_ASSERT_NOT_NULL(n);
        YEW_ASSERT(n->expanded);
        n->expanded = false;
    }
    yew_fuss_flatten(t);
}

void test_fusscollapse_harvest_restore_roundtrips_seven_of_two_hundred(void)
{
    static const char *const want[] = {
        "d003", "d019", "d041", "d088", "d109", "d157", "d199"
    };
    GitEntry entries[FC_DIRS];
    char paths[FC_DIRS][24];
    GitSnapshot s;
    FussOpts o = {false, false};
    FussTree old;
    FussTree nw;
    Arena a;
    char **collapsed = NULL;
    u32 n;
    size_t i;

    fc_entries(entries, paths);
    s = fc_snapshot(entries, FC_DIRS, 1U);
    yew_fuss_tree_init(&old);
    yew_fuss_tree_init(&nw);
    yew_fuss_build(&old, &s, &o);
    fc_collapse_seven(&old);
    arena_init(&a);
    n = yew_fuss_harvest_collapsed(&old, &a, &collapsed);

    YEW_ASSERT_EQ_U64(n, YEW_ARRAY_LEN(want));
    for (i = 0U; i < YEW_ARRAY_LEN(want); i++)
        YEW_ASSERT_EQ_STR(collapsed[i], want[i]);

    s.gen = 2U;
    yew_fuss_build(&nw, &s, &o);
    yew_fuss_restore_collapsed(&nw, collapsed, n);
    for (i = 0U; i < YEW_ARRAY_LEN(want); i++) {
        FussNode *node = fc_node(&nw, want[i]);
        YEW_ASSERT_NOT_NULL(node);
        YEW_ASSERT(!node->expanded);
    }
    YEW_ASSERT_EQ_U64(nw.items.len, FC_DIRS * 2U - YEW_ARRAY_LEN(want));
    arena_free_all(&a);
    yew_fuss_tree_drop(&old);
    yew_fuss_tree_drop(&nw);
}

void test_fusscollapse_state_has_zero_drift_across_one_hundred_refreshes(void)
{
    static const char *const want[] = {
        "d003", "d019", "d041", "d088", "d109", "d157", "d199"
    };
    GitEntry entries[FC_DIRS];
    char paths[FC_DIRS][24];
    GitSnapshot s;
    FussOpts o = {false, false};
    FussTree t;
    u32 cycle;

    fc_entries(entries, paths);
    s = fc_snapshot(entries, FC_DIRS, 10U);
    yew_fuss_tree_init(&t);
    yew_fuss_build(&t, &s, &o);
    fc_collapse_seven(&t);

    for (cycle = 0U; cycle < 100U; cycle++) {
        Arena a;
        char **collapsed = NULL;
        u32 n;
        size_t i;

        arena_init(&a);
        n = yew_fuss_harvest_collapsed(&t, &a, &collapsed);
        YEW_ASSERT_EQ_U64(n, YEW_ARRAY_LEN(want));
        for (i = 0U; i < YEW_ARRAY_LEN(want); i++)
            YEW_ASSERT_EQ_STR(collapsed[i], want[i]);
        s.gen++;
        yew_fuss_build(&t, &s, &o);
        yew_fuss_restore_collapsed(&t, collapsed, n);
        for (i = 0U; i < YEW_ARRAY_LEN(want); i++) {
            FussNode *node = fc_node(&t, want[i]);
            YEW_ASSERT_NOT_NULL(node);
            YEW_ASSERT(!node->expanded);
        }
        arena_free_all(&a);
    }
    yew_fuss_tree_drop(&t);
}

void test_fusscollapse_vanished_directories_drop_and_new_ones_expand(void)
{
    GitEntry old_entries[2];
    GitEntry new_entries[2];
    GitSnapshot old_snap;
    GitSnapshot new_snap;
    FussOpts o = {false, false};
    FussTree old;
    FussTree nw;
    Arena a;
    char **collapsed = NULL;
    u32 n;
    FussNode *node;

    old_entries[0] = (GitEntry){.kind = GIT_E_ORDINARY,
        .path = "gone/file", .path_len = 9U, .unstaged = true};
    old_entries[1] = (GitEntry){.kind = GIT_E_ORDINARY,
        .path = "kept/file", .path_len = 9U, .unstaged = true};
    new_entries[0] = old_entries[1];
    new_entries[1] = (GitEntry){.kind = GIT_E_ORDINARY,
        .path = "new/file", .path_len = 8U, .unstaged = true};
    old_snap = fc_snapshot(old_entries, 2U, 1U);
    new_snap = fc_snapshot(new_entries, 2U, 2U);
    yew_fuss_tree_init(&old);
    yew_fuss_tree_init(&nw);
    yew_fuss_build(&old, &old_snap, &o);
    node = fc_node(&old, "gone");
    YEW_ASSERT_NOT_NULL(node);
    node->expanded = false;
    yew_fuss_flatten(&old);
    arena_init(&a);
    n = yew_fuss_harvest_collapsed(&old, &a, &collapsed);
    YEW_ASSERT_EQ_U64(n, 1U);
    YEW_ASSERT_EQ_STR(collapsed[0], "gone");

    yew_fuss_build(&nw, &new_snap, &o);
    yew_fuss_restore_collapsed(&nw, collapsed, n);
    YEW_ASSERT_NULL(fc_node(&nw, "gone"));
    node = fc_node(&nw, "kept");
    YEW_ASSERT_NOT_NULL(node);
    YEW_ASSERT(node->expanded);
    node = fc_node(&nw, "new");
    YEW_ASSERT_NOT_NULL(node);
    YEW_ASSERT(node->expanded);
    arena_free_all(&a);
    yew_fuss_tree_drop(&old);
    yew_fuss_tree_drop(&nw);
}

void test_fusscollapse_selection_survives_deleting_an_earlier_row(void)
{
    GitEntry old_entries[3];
    GitEntry new_entries[2];
    GitSnapshot s;
    FussOpts o = {false, false};
    FussTree t;
    FussSel sel;
    i32 before;
    i32 after;

    old_entries[0] = (GitEntry){.kind = GIT_E_ORDINARY,
        .path = "a", .path_len = 1U, .unstaged = true};
    old_entries[1] = (GitEntry){.kind = GIT_E_ORDINARY,
        .path = "b", .path_len = 1U, .unstaged = true};
    old_entries[2] = (GitEntry){.kind = GIT_E_ORDINARY,
        .path = "c", .path_len = 1U, .unstaged = true};
    new_entries[0] = old_entries[1];
    new_entries[1] = old_entries[2];
    yew_fuss_tree_init(&t);
    (void)memset(&sel, 0, sizeof(sel));
    s = fc_snapshot(old_entries, 3U, 1U);
    yew_fuss_build(&t, &s, &o);
    yew_fuss_sel_set(&sel, "c", 1U);
    before = yew_fuss_row_of(&t, &sel);
    YEW_ASSERT_EQ_I64(before, 2);

    s = fc_snapshot(new_entries, 2U, 2U);
    yew_fuss_build(&t, &s, &o);
    after = yew_fuss_row_of(&t, &sel);
    YEW_ASSERT_EQ_I64(after, 1);
    YEW_ASSERT_EQ_STR(t.items.data[after].path, "c");
    YEW_ASSERT_EQ_STR(sel.path, "c");
    YEW_ASSERT_EQ_U64(sel.len, 1U);
    yew_fuss_sel_clear(&sel);
    yew_fuss_tree_drop(&t);
}

void test_fusscollapse_selection_falls_to_nearest_existing_ancestor(void)
{
    GitEntry old_entry = {.kind = GIT_E_ORDINARY,
        .path = "src/deep/file.c", .path_len = 15U, .unstaged = true};
    GitEntry new_entry = {.kind = GIT_E_ORDINARY,
        .path = "src/keep.c", .path_len = 10U, .unstaged = true};
    FussOpts o = {false, false};
    FussTree t;
    FussSel sel;
    GitSnapshot s;
    i32 row;

    yew_fuss_tree_init(&t);
    (void)memset(&sel, 0, sizeof(sel));
    s = fc_snapshot(&old_entry, 1U, 1U);
    yew_fuss_build(&t, &s, &o);
    yew_fuss_sel_set(&sel, old_entry.path, old_entry.path_len);
    YEW_ASSERT(yew_fuss_row_of(&t, &sel) >= 0);

    s = fc_snapshot(&new_entry, 1U, 2U);
    yew_fuss_build(&t, &s, &o);
    row = yew_fuss_row_of(&t, &sel);
    YEW_ASSERT(row >= 0);
    YEW_ASSERT_EQ_STR(t.items.data[row].path, "src");
    YEW_ASSERT(!t.items.data[row].is_file);
    yew_fuss_sel_clear(&sel);
    yew_fuss_tree_drop(&t);
}

void test_fusscollapse_selection_falls_to_row_zero_without_ancestor(void)
{
    GitEntry old_entry = {.kind = GIT_E_ORDINARY,
        .path = "gone/file", .path_len = 9U, .unstaged = true};
    GitEntry new_entry = {.kind = GIT_E_ORDINARY,
        .path = "kept/file", .path_len = 9U, .unstaged = true};
    FussOpts o = {false, false};
    FussTree t;
    FussSel sel;
    GitSnapshot s;
    i32 row;

    yew_fuss_tree_init(&t);
    (void)memset(&sel, 0, sizeof(sel));
    s = fc_snapshot(&old_entry, 1U, 1U);
    yew_fuss_build(&t, &s, &o);
    yew_fuss_sel_from_row(&sel, &t, fc_row(&t, "gone/file"));
    YEW_ASSERT_EQ_STR(sel.path, "gone/file");

    s = fc_snapshot(&new_entry, 1U, 2U);
    yew_fuss_build(&t, &s, &o);
    row = yew_fuss_row_of(&t, &sel);
    YEW_ASSERT_EQ_I64(row, 0);
    YEW_ASSERT_EQ_STR(t.items.data[row].path, "kept");
    yew_fuss_sel_clear(&sel);
    yew_fuss_tree_drop(&t);
}
