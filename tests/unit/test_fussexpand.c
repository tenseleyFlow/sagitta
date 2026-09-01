/* Sprint 57.5: positive expansion memory and open-file ancestry. */
#include "harness.h"

#include <string.h>

#include "mod/git/fusstree.h"

static GitEntry fx_entry(const char *path)
{
    GitEntry entry;

    (void)memset(&entry, 0, sizeof(entry));
    entry.kind = GIT_E_ORDINARY;
    entry.path = (char *)path;
    entry.path_len = (u32)strlen(path);
    entry.unstaged = true;
    return entry;
}

static GitSnapshot fx_snapshot(GitEntry *entries, size_t n, u32 gen)
{
    GitSnapshot snapshot;

    (void)memset(&snapshot, 0, sizeof(snapshot));
    snapshot.state = YEW_GIT_OK;
    snapshot.entries.data = entries;
    snapshot.entries.len = n;
    snapshot.gen = gen;
    return snapshot;
}

static const FussNode *fx_node(const FussTree *tree, const char *path)
{
    size_t len = strlen(path);
    size_t i;

    for (i = 1U; i < tree->nodes.len; i++)
        if (tree->nodes.data[i].path_len == len &&
            memcmp(tree->nodes.data[i].path, path, len) == 0)
            return &tree->nodes.data[i];
    return NULL;
}

static i32 fx_row(const FussTree *tree, const char *path)
{
    size_t len = strlen(path);
    size_t i;

    for (i = 0U; i < tree->items.len; i++)
        if (tree->items.data[i].path_len == len &&
            memcmp(tree->items.data[i].path, path, len) == 0)
            return (i32)i;
    return -1;
}

void test_fussexpand_positive_memory_is_sorted_owned_and_idempotent(void)
{
    char mutable[] = "zeta";
    FussOpenMemory memory;

    yew_fuss_open_memory_init(&memory);
    YEW_ASSERT(yew_fuss_open_memory_set(&memory, mutable, 4U, true));
    mutable[0] = 'x';
    YEW_ASSERT(yew_fuss_open_memory_set(&memory, "alpha", 5U, true));
    YEW_ASSERT(yew_fuss_open_memory_set(&memory, "mid", 3U, true));
    YEW_ASSERT(!yew_fuss_open_memory_set(&memory, "mid", 3U, true));
    YEW_ASSERT_EQ_U64(memory.len, 3U);
    YEW_ASSERT_EQ_STR(memory.data[0].path, "alpha");
    YEW_ASSERT_EQ_STR(memory.data[1].path, "mid");
    YEW_ASSERT_EQ_STR(memory.data[2].path, "zeta");
    YEW_ASSERT(yew_fuss_open_memory_has(&memory, "zeta", 4U));
    YEW_ASSERT(!yew_fuss_open_memory_has(&memory, "zet", 3U));
    YEW_ASSERT(yew_fuss_open_memory_set(&memory, "mid", 3U, false));
    YEW_ASSERT(!yew_fuss_open_memory_set(&memory, "mid", 3U, false));
    YEW_ASSERT_EQ_U64(memory.len, 2U);
    YEW_ASSERT_EQ_STR(memory.data[0].path, "alpha");
    YEW_ASSERT_EQ_STR(memory.data[1].path, "zeta");
    yew_fuss_open_memory_drop(&memory);
}

void test_fussexpand_new_tree_is_collapsed_until_manually_opened(void)
{
    GitEntry entries[] = {
        fx_entry("src/edit/a.c"), fx_entry("src/mod/b.c"),
        fx_entry("tests/t.c")
    };
    GitSnapshot snapshot = fx_snapshot(entries, YEW_ARRAY_LEN(entries), 1U);
    FussOpts opts = {false, false};
    FussOpenMemory memory;
    FussTree tree;

    yew_fuss_tree_init(&tree);
    yew_fuss_open_memory_init(&memory);
    yew_fuss_build(&tree, &snapshot, &opts);
    YEW_ASSERT_EQ_U64(tree.items.len, 2U);
    YEW_ASSERT_EQ_STR(tree.items.data[0].path, "src");
    YEW_ASSERT_EQ_STR(tree.items.data[1].path, "tests");
    YEW_ASSERT(!fx_node(&tree, "src")->expanded);
    YEW_ASSERT(!fx_node(&tree, "src/mod")->expanded);

    YEW_ASSERT(yew_fuss_open_memory_set(&memory, "src", 3U, true));
    yew_fuss_apply_expansion(&tree, &memory, NULL, 0U);
    YEW_ASSERT_NOT_NULL(fx_node(&tree, "src/edit"));
    YEW_ASSERT(fx_row(&tree, "src/edit") >= 0);
    YEW_ASSERT_EQ_I64(fx_row(&tree, "src/edit/a.c"), -1);

    YEW_ASSERT(yew_fuss_open_memory_set(&memory, "src/edit", 8U, true));
    yew_fuss_apply_expansion(&tree, &memory, NULL, 0U);
    YEW_ASSERT(fx_row(&tree, "src/edit/a.c") >= 0);
    YEW_ASSERT_EQ_I64(fx_row(&tree, "src/mod/b.c"), -1);
    yew_fuss_open_memory_drop(&memory);
    yew_fuss_tree_drop(&tree);
}

void test_fussexpand_open_file_ancestry_respects_path_segments(void)
{
    GitEntry entries[] = {
        fx_entry("src/mod/a.c"), fx_entry("src/modem.c")
    };
    GitSnapshot snapshot = fx_snapshot(entries, YEW_ARRAY_LEN(entries), 2U);
    FussOpts opts = {false, false};
    FussOpenMemory memory;
    FussPathRef modem[] = {{"src/modem.c", 11U}, {"src/modem.c", 11U}};
    FussPathRef nested = {"src/mod/a.c", 11U};
    FussTree tree;

    yew_fuss_tree_init(&tree);
    yew_fuss_open_memory_init(&memory);
    yew_fuss_build(&tree, &snapshot, &opts);
    yew_fuss_apply_expansion(&tree, &memory, modem, YEW_ARRAY_LEN(modem));
    YEW_ASSERT(fx_node(&tree, "src")->expanded);
    YEW_ASSERT(!fx_node(&tree, "src/mod")->expanded);
    YEW_ASSERT(fx_row(&tree, "src/mod") >= 0);
    YEW_ASSERT_EQ_I64(fx_row(&tree, "src/mod/a.c"), -1);

    yew_fuss_apply_expansion(&tree, &memory, &nested, 1U);
    YEW_ASSERT(fx_node(&tree, "src")->expanded);
    YEW_ASSERT(fx_node(&tree, "src/mod")->expanded);
    YEW_ASSERT(fx_row(&tree, "src/mod/a.c") >= 0);
    YEW_ASSERT_EQ_U64(memory.len, 0U);
    yew_fuss_open_memory_drop(&memory);
    yew_fuss_tree_drop(&tree);
}

void test_fussexpand_automatic_open_wins_over_manual_collapse(void)
{
    GitEntry entry = fx_entry("a/b/file.c");
    GitSnapshot snapshot = fx_snapshot(&entry, 1U, 3U);
    FussOpts opts = {false, false};
    FussPathRef open = {"a/b/file.c", 10U};
    FussOpenMemory memory;
    FussTree tree;

    yew_fuss_tree_init(&tree);
    yew_fuss_open_memory_init(&memory);
    yew_fuss_build(&tree, &snapshot, &opts);
    YEW_ASSERT(yew_fuss_open_memory_set(&memory, "a", 1U, true));
    YEW_ASSERT(yew_fuss_open_memory_set(&memory, "a", 1U, false));
    yew_fuss_apply_expansion(&tree, &memory, &open, 1U);
    YEW_ASSERT(fx_node(&tree, "a")->expanded);
    YEW_ASSERT(fx_node(&tree, "a/b")->expanded);
    YEW_ASSERT_EQ_U64(memory.len, 0U);
    yew_fuss_apply_expansion(&tree, &memory, NULL, 0U);
    YEW_ASSERT(!fx_node(&tree, "a")->expanded);
    YEW_ASSERT(!fx_node(&tree, "a/b")->expanded);
    yew_fuss_open_memory_drop(&memory);
    yew_fuss_tree_drop(&tree);
}

void test_fussexpand_memory_survives_absence_and_reappears(void)
{
    GitEntry present = fx_entry("remembered/file.c");
    GitEntry absent = fx_entry("other/file.c");
    FussOpts opts = {false, false};
    FussOpenMemory memory;
    FussTree tree;
    GitSnapshot snapshot;

    yew_fuss_tree_init(&tree);
    yew_fuss_open_memory_init(&memory);
    YEW_ASSERT(yew_fuss_open_memory_set(&memory, "remembered", 10U, true));
    snapshot = fx_snapshot(&present, 1U, 4U);
    yew_fuss_build(&tree, &snapshot, &opts);
    yew_fuss_apply_expansion(&tree, &memory, NULL, 0U);
    YEW_ASSERT(fx_node(&tree, "remembered")->expanded);

    snapshot = fx_snapshot(&absent, 1U, 5U);
    yew_fuss_build(&tree, &snapshot, &opts);
    yew_fuss_apply_expansion(&tree, &memory, NULL, 0U);
    YEW_ASSERT_NULL(fx_node(&tree, "remembered"));
    YEW_ASSERT(yew_fuss_open_memory_has(&memory, "remembered", 10U));

    snapshot = fx_snapshot(&present, 1U, 6U);
    yew_fuss_build(&tree, &snapshot, &opts);
    yew_fuss_apply_expansion(&tree, &memory, NULL, 0U);
    YEW_ASSERT(fx_node(&tree, "remembered")->expanded);
    YEW_ASSERT(fx_row(&tree, "remembered/file.c") >= 0);
    yew_fuss_open_memory_drop(&memory);
    yew_fuss_tree_drop(&tree);
}

void test_fussexpand_selection_falls_to_nearest_existing_ancestor(void)
{
    GitEntry old_entry = fx_entry("src/deep/file.c");
    GitEntry new_entry = fx_entry("src/keep.c");
    FussPathRef open = {"src/deep/file.c", 15U};
    FussOpts opts = {false, false};
    FussOpenMemory memory;
    FussTree tree;
    FussSel selection = {0};
    GitSnapshot snapshot;
    i32 row;

    yew_fuss_tree_init(&tree);
    yew_fuss_open_memory_init(&memory);
    snapshot = fx_snapshot(&old_entry, 1U, 7U);
    yew_fuss_build(&tree, &snapshot, &opts);
    yew_fuss_apply_expansion(&tree, &memory, &open, 1U);
    yew_fuss_sel_set(&selection, old_entry.path, old_entry.path_len);
    YEW_ASSERT(yew_fuss_row_of(&tree, &selection) >= 0);

    snapshot = fx_snapshot(&new_entry, 1U, 8U);
    yew_fuss_build(&tree, &snapshot, &opts);
    row = yew_fuss_row_of(&tree, &selection);
    YEW_ASSERT(row >= 0);
    YEW_ASSERT_EQ_STR(tree.items.data[row].path, "src");
    YEW_ASSERT(!tree.items.data[row].is_file);
    yew_fuss_sel_clear(&selection);
    yew_fuss_open_memory_drop(&memory);
    yew_fuss_tree_drop(&tree);
}

void test_fussexpand_selection_falls_to_row_zero_without_ancestor(void)
{
    GitEntry old_entry = fx_entry("gone/file");
    GitEntry new_entry = fx_entry("kept/file");
    FussPathRef open = {"gone/file", 9U};
    FussOpts opts = {false, false};
    FussOpenMemory memory;
    FussTree tree;
    FussSel selection = {0};
    GitSnapshot snapshot;
    i32 row;

    yew_fuss_tree_init(&tree);
    yew_fuss_open_memory_init(&memory);
    snapshot = fx_snapshot(&old_entry, 1U, 9U);
    yew_fuss_build(&tree, &snapshot, &opts);
    yew_fuss_apply_expansion(&tree, &memory, &open, 1U);
    yew_fuss_sel_from_row(&selection, &tree, fx_row(&tree, "gone/file"));
    YEW_ASSERT_EQ_STR(selection.path, "gone/file");

    snapshot = fx_snapshot(&new_entry, 1U, 10U);
    yew_fuss_build(&tree, &snapshot, &opts);
    row = yew_fuss_row_of(&tree, &selection);
    YEW_ASSERT_EQ_I64(row, 0);
    YEW_ASSERT_EQ_STR(tree.items.data[row].path, "kept");
    yew_fuss_sel_clear(&selection);
    yew_fuss_open_memory_drop(&memory);
    yew_fuss_tree_drop(&tree);
}
