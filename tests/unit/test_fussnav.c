/* Sprint 52: fuss-compatible depth navigation over the flat tree. */
#include "harness.h"

#include <string.h>

#include "mod/git/fusstree.h"

static void fn_depth_fixture(FussTree *t)
{
    static const u16 depths[] = {0U, 1U, 1U, 2U, 3U, 2U,
                                 1U, 0U, 1U, 2U, 0U, 0U};
    static FussItem items[YEW_ARRAY_LEN(depths)];
    size_t i;

    (void)memset(t, 0, sizeof(*t));
    (void)memset(items, 0, sizeof(items));
    for (i = 0U; i < YEW_ARRAY_LEN(depths); i++)
        items[i].depth = depths[i];
    t->items.data = items;
    t->items.len = YEW_ARRAY_LEN(items);
    t->items.cap = YEW_ARRAY_LEN(items);
}

static GitEntry fn_entry(const char *path, bool dir)
{
    GitEntry e;

    (void)memset(&e, 0, sizeof(e));
    e.kind = dir ? GIT_E_UNTRACKED : GIT_E_ORDINARY;
    e.path = (char *)path;
    e.path_len = (u32)strlen(path);
    e.is_dir = dir;
    e.untracked = dir;
    e.unstaged = !dir;
    return e;
}

static GitSnapshot fn_snapshot(GitEntry *entries, size_t n)
{
    GitSnapshot s;

    (void)memset(&s, 0, sizeof(s));
    s.entries.data = entries;
    s.entries.len = n;
    s.gen = 1U;
    return s;
}

static i32 fn_row(const FussTree *t, const char *path)
{
    size_t i;
    size_t n = strlen(path);

    for (i = 0U; i < t->items.len; i++)
        if (t->items.data[i].path_len == n &&
            memcmp(t->items.data[i].path, path, n) == 0)
            return (i32)i;
    return -1;
}

void test_fussnav_next_visits_the_next_row_at_the_same_depth(void)
{
    static const i32 want[] = {7, 2, 6, 5, 4, 9, 8, 10, 1, 3, 11, 0};
    FussTree t;
    size_t i;

    fn_depth_fixture(&t);
    for (i = 0U; i < YEW_ARRAY_LEN(want); i++)
        YEW_ASSERT_EQ_I64(yew_fuss_nav_step(&t, (i32)i, 1), want[i]);
}

void test_fussnav_prev_visits_the_previous_row_at_the_same_depth(void)
{
    static const i32 want[] = {11, 8, 1, 9, 4, 3, 2, 0, 6, 5, 7, 10};
    FussTree t;
    size_t i;

    fn_depth_fixture(&t);
    for (i = 0U; i < YEW_ARRAY_LEN(want); i++)
        YEW_ASSERT_EQ_I64(yew_fuss_nav_step(&t, (i32)i, -1), want[i]);
}

void test_fussnav_only_item_at_depth_stays_selected(void)
{
    FussTree t;

    fn_depth_fixture(&t);
    YEW_ASSERT_EQ_I64(yew_fuss_nav_step(&t, 4, 1), 4);
    YEW_ASSERT_EQ_I64(yew_fuss_nav_step(&t, 4, -1), 4);
}

void test_fussnav_raw_rows_ignore_depth_and_clamp(void)
{
    FussTree t;

    fn_depth_fixture(&t);
    YEW_ASSERT_EQ_I64(yew_fuss_nav_raw(&t, 0, 1), 1);
    YEW_ASSERT_EQ_I64(yew_fuss_nav_raw(&t, 1, 1), 2);
    YEW_ASSERT_EQ_I64(yew_fuss_nav_raw(&t, 11, 1), 11);
    YEW_ASSERT_EQ_I64(yew_fuss_nav_raw(&t, 11, -1), 10);
    YEW_ASSERT_EQ_I64(yew_fuss_nav_raw(&t, 1, -1), 0);
    YEW_ASSERT_EQ_I64(yew_fuss_nav_raw(&t, 0, -1), 0);
}

void test_fussnav_parent_finds_the_nearest_earlier_shallower_row(void)
{
    FussTree t;

    fn_depth_fixture(&t);
    YEW_ASSERT_EQ_I64(yew_fuss_nav_parent(&t, 0), 0);
    YEW_ASSERT_EQ_I64(yew_fuss_nav_parent(&t, 1), 0);
    YEW_ASSERT_EQ_I64(yew_fuss_nav_parent(&t, 2), 0);
    YEW_ASSERT_EQ_I64(yew_fuss_nav_parent(&t, 3), 2);
    YEW_ASSERT_EQ_I64(yew_fuss_nav_parent(&t, 4), 3);
    YEW_ASSERT_EQ_I64(yew_fuss_nav_parent(&t, 5), 2);
    YEW_ASSERT_EQ_I64(yew_fuss_nav_parent(&t, 6), 0);
    YEW_ASSERT_EQ_I64(yew_fuss_nav_parent(&t, 7), 7);
    YEW_ASSERT_EQ_I64(yew_fuss_nav_parent(&t, 8), 7);
    YEW_ASSERT_EQ_I64(yew_fuss_nav_parent(&t, 9), 8);
    YEW_ASSERT_EQ_I64(yew_fuss_nav_parent(&t, 10), 10);
    YEW_ASSERT_EQ_I64(yew_fuss_nav_parent(&t, 11), 11);
}

void test_fussnav_enter_expands_then_descends(void)
{
    GitEntry e[2];
    GitSnapshot s;
    FussOpts o = {false, false};
    FussTree t;
    i32 a;
    i32 deep;
    i32 leaf;

    e[0] = fn_entry("a/deep/leaf.c", false);
    e[1] = fn_entry("root.c", false);
    s = fn_snapshot(e, YEW_ARRAY_LEN(e));
    yew_fuss_tree_init(&t);
    yew_fuss_build(&t, &s, &o);
    a = fn_row(&t, "a");
    deep = fn_row(&t, "a/deep");
    leaf = fn_row(&t, "a/deep/leaf.c");

    YEW_ASSERT(a >= 0);
    YEW_ASSERT(deep > a);
    YEW_ASSERT(leaf > deep);
    YEW_ASSERT(yew_fuss_nav_toggle(&t, deep));
    YEW_ASSERT_EQ_I64(fn_row(&t, "a/deep/leaf.c"), -1);
    deep = fn_row(&t, "a/deep");
    YEW_ASSERT_EQ_I64(yew_fuss_nav_enter(&t, deep), deep + 1);
    YEW_ASSERT_EQ_STR(t.items.data[deep + 1].path, "a/deep/leaf.c");
    YEW_ASSERT(t.nodes.data[t.items.data[deep].node].expanded);
    yew_fuss_tree_drop(&t);
}

void test_fussnav_enter_on_file_or_childless_directory_stays(void)
{
    GitEntry e[2];
    GitSnapshot s;
    FussOpts o = {false, false};
    FussTree t;
    i32 file;
    i32 empty;

    e[0] = fn_entry("file.c", false);
    e[1] = fn_entry("empty/", true);
    s = fn_snapshot(e, YEW_ARRAY_LEN(e));
    yew_fuss_tree_init(&t);
    yew_fuss_build(&t, &s, &o);
    file = fn_row(&t, "file.c");
    empty = fn_row(&t, "empty");

    YEW_ASSERT(file >= 0);
    YEW_ASSERT(empty >= 0);
    YEW_ASSERT_EQ_I64(yew_fuss_nav_enter(&t, file), file);
    YEW_ASSERT_EQ_I64(yew_fuss_nav_enter(&t, empty), empty);
    YEW_ASSERT(!yew_fuss_nav_toggle(&t, file));
    YEW_ASSERT(!yew_fuss_nav_toggle(&t, empty));
    YEW_ASSERT_EQ_I64(fn_row(&t, "empty"), empty);
    yew_fuss_tree_drop(&t);
}

void test_fussnav_empty_tree_is_total(void)
{
    FussTree t;

    yew_fuss_tree_init(&t);
    YEW_ASSERT_EQ_I64(yew_fuss_nav_step(&t, 0, 1), -1);
    YEW_ASSERT_EQ_I64(yew_fuss_nav_step(&t, 0, -1), -1);
    YEW_ASSERT_EQ_I64(yew_fuss_nav_raw(&t, 0, 1), -1);
    YEW_ASSERT_EQ_I64(yew_fuss_nav_raw(&t, 0, -1), -1);
    YEW_ASSERT_EQ_I64(yew_fuss_nav_parent(&t, 0), -1);
    YEW_ASSERT_EQ_I64(yew_fuss_nav_enter(&t, 0), -1);
    YEW_ASSERT(!yew_fuss_nav_toggle(&t, 0));
    yew_fuss_tree_drop(&t);
}
