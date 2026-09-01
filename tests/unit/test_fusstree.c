/* Sprint 52: pure construction rules for the dirty-first FUSS tree. */
#include "harness.h"

#include <string.h>

#include "mod/git/fusstree.h"

static GitEntry ft_entry(const char *path)
{
    GitEntry e;

    (void)memset(&e, 0, sizeof(e));
    e.kind = GIT_E_ORDINARY;
    e.path = (char *)path;
    e.path_len = (u32)strlen(path);
    return e;
}

static GitSnapshot ft_snapshot(GitEntry *entries, size_t n, u32 gen)
{
    GitSnapshot s;

    (void)memset(&s, 0, sizeof(s));
    s.state = YEW_GIT_OK;
    s.entries.data = entries;
    s.entries.len = n;
    s.gen = gen;
    return s;
}

static void ft_drop(FussTree *t)
{
    yew_fuss_tree_drop(t);
}

void test_fuss_marker_order_and_conflict_override(void)
{
    FussNode node;
    FussMarkerKind got[4];
    u8 mask;
    u8 expected;
    u8 at;

    (void)memset(&node, 0, sizeof(node));
    for (mask = 1U; mask < 16U; mask++) {
        node.staged = (mask & 1U) != 0U;
        node.unstaged = (mask & 2U) != 0U;
        node.untracked = (mask & 4U) != 0U;
        node.incoming = (mask & 8U) != 0U;
        node.conflicted = false;
        expected = 0U;
        if (node.staged) expected++;
        if (node.unstaged) expected++;
        if (node.untracked) expected++;
        if (node.incoming) expected++;
        YEW_ASSERT_EQ_U64(yew_fuss_marker_kinds(&node, got), expected);
        at = 0U;
        if (node.staged)
            YEW_ASSERT_EQ_U64(got[at++], YEW_FUSS_MARK_STAGED);
        if (node.unstaged)
            YEW_ASSERT_EQ_U64(got[at++], YEW_FUSS_MARK_UNSTAGED);
        if (node.untracked)
            YEW_ASSERT_EQ_U64(got[at++], YEW_FUSS_MARK_UNTRACKED);
        if (node.incoming)
            YEW_ASSERT_EQ_U64(got[at++], YEW_FUSS_MARK_INCOMING);
        YEW_ASSERT_EQ_U64(at, expected);
    }
    node.conflicted = true;
    YEW_ASSERT_EQ_U64(yew_fuss_marker_kinds(&node, got), 1U);
    YEW_ASSERT_EQ_U64(got[0], YEW_FUSS_MARK_CONFLICT);
}

static const FussItem *ft_item(const FussTree *t, const char *path)
{
    size_t i;
    size_t n = strlen(path);

    for (i = 0U; i < t->items.len; i++)
        if (t->items.data[i].path_len == n &&
            memcmp(t->items.data[i].path, path, n) == 0)
            return &t->items.data[i];
    return NULL;
}

static const FussNode *ft_node(const FussTree *t, const char *path)
{
    size_t i;
    size_t n = strlen(path);

    for (i = 1U; i < t->nodes.len; i++)
        if (t->nodes.data[i].path_len == n &&
            memcmp(t->nodes.data[i].path, path, n) == 0)
            return &t->nodes.data[i];
    return NULL;
}

static const FussNode *ft_node_raw(const FussTree *t, const char *path)
{
    return ft_node(t, path);
}

static void ft_expand_all(FussTree *t)
{
    FussOpenMemory memory;
    size_t i;

    yew_fuss_open_memory_init(&memory);
    for (i = 1U; i < t->nodes.len; i++)
        if (!t->nodes.data[i].is_file)
            (void)yew_fuss_open_memory_set(&memory,
                                            t->nodes.data[i].path,
                                            t->nodes.data[i].path_len, true);
    yew_fuss_apply_expansion(t, &memory, NULL, 0U);
    yew_fuss_open_memory_drop(&memory);
}

static void ft_fill_fixture(GitEntry e[8])
{
    e[0] = ft_entry("src/zeta.c");
    e[0].staged = true;
    e[1] = ft_entry("src/Alpha.c");
    e[1].unstaged = true;
    e[2] = ft_entry("src/mod/both.c");
    e[2].staged = true;
    e[2].unstaged = true;
    e[3] = ft_entry("README.md");
    e[3].kind = GIT_E_UNTRACKED;
    e[3].untracked = true;
    e[4] = ft_entry("conflict.c");
    e[4].kind = GIT_E_UNMERGED;
    e[4].staged = true;
    e[4].unstaged = true;
    e[4].conflicted = true;
    e[5] = ft_entry("incoming.c");
    e[5].incoming = true;
    e[6] = ft_entry("ignored.log");
    e[6].kind = GIT_E_IGNORED;
    e[7] = ft_entry("vendor/");
    e[7].kind = GIT_E_UNTRACKED;
    e[7].untracked = true;
    e[7].is_dir = true;
}

void test_fusstree_builds_every_status_flag(void)
{
    GitEntry e[8];
    GitSnapshot s;
    FussOpts o = {false, true};
    FussTree t;
    const FussNode *n;

    ft_fill_fixture(e);
    s = ft_snapshot(e, YEW_ARRAY_LEN(e), 17U);
    yew_fuss_tree_init(&t);
    yew_fuss_build(&t, &s, &o);

    YEW_ASSERT_EQ_U64(t.snap_gen, 17U);
    YEW_ASSERT(t.nodes.len > 8U);
    YEW_ASSERT_EQ_U64(t.nodes.data[0].parent, 0U);
    YEW_ASSERT(!t.nodes.data[0].is_file);

    n = ft_node(&t, "src/zeta.c");
    YEW_ASSERT_NOT_NULL(n);
    YEW_ASSERT(n->is_file);
    YEW_ASSERT(n->staged);
    YEW_ASSERT(!n->unstaged);
    YEW_ASSERT(!n->untracked);
    YEW_ASSERT(!n->incoming);
    YEW_ASSERT(!n->conflicted);

    n = ft_node(&t, "src/Alpha.c");
    YEW_ASSERT_NOT_NULL(n);
    YEW_ASSERT(!n->staged);
    YEW_ASSERT(n->unstaged);
    YEW_ASSERT(!n->untracked);

    n = ft_node(&t, "src/mod/both.c");
    YEW_ASSERT_NOT_NULL(n);
    YEW_ASSERT(n->staged);
    YEW_ASSERT(n->unstaged);

    n = ft_node(&t, "README.md");
    YEW_ASSERT_NOT_NULL(n);
    YEW_ASSERT(n->untracked);
    YEW_ASSERT(!n->staged);
    YEW_ASSERT(!n->unstaged);

    n = ft_node(&t, "incoming.c");
    YEW_ASSERT_NOT_NULL(n);
    YEW_ASSERT(n->incoming);

    n = ft_node(&t, "conflict.c");
    YEW_ASSERT_NOT_NULL(n);
    YEW_ASSERT(n->conflicted);
    YEW_ASSERT(n->staged);
    YEW_ASSERT(n->unstaged);

    n = ft_node(&t, "ignored.log");
    YEW_ASSERT_NOT_NULL(n);
    YEW_ASSERT(n->ignored);
    ft_drop(&t);
}

void test_fusstree_directories_or_descendant_flags(void)
{
    GitEntry e[8];
    GitSnapshot s;
    FussOpts o = {false, true};
    FussTree t;
    const FussNode *src;
    const FussNode *mod;

    ft_fill_fixture(e);
    s = ft_snapshot(e, YEW_ARRAY_LEN(e), 1U);
    yew_fuss_tree_init(&t);
    yew_fuss_build(&t, &s, &o);
    src = ft_node(&t, "src");
    mod = ft_node(&t, "src/mod");

    YEW_ASSERT_NOT_NULL(src);
    YEW_ASSERT(!src->is_file);
    YEW_ASSERT(src->staged);
    YEW_ASSERT(src->unstaged);
    YEW_ASSERT(!src->untracked);
    YEW_ASSERT(!src->incoming);
    YEW_ASSERT(!src->conflicted);
    YEW_ASSERT(!src->expanded);

    YEW_ASSERT_NOT_NULL(mod);
    YEW_ASSERT(!mod->is_file);
    YEW_ASSERT(mod->staged);
    YEW_ASSERT(mod->unstaged);
    YEW_ASSERT(!mod->untracked);
    YEW_ASSERT(!mod->expanded);
    ft_drop(&t);
}

void test_fusstree_sorts_directories_before_files_deterministically(void)
{
    static const char *const want[] = {
        "src", "src/mod", "src/mod/both.c", "src/Alpha.c", "src/zeta.c",
        "vendor", "conflict.c", "ignored.log", "incoming.c", "README.md"
    };
    GitEntry a[8];
    GitEntry b[8];
    GitSnapshot sa;
    GitSnapshot sb;
    FussOpts o = {false, true};
    FussTree ta;
    FussTree tb;
    size_t i;

    ft_fill_fixture(a);
    for (i = 0U; i < YEW_ARRAY_LEN(a); i++)
        b[i] = a[YEW_ARRAY_LEN(a) - 1U - i];
    sa = ft_snapshot(a, YEW_ARRAY_LEN(a), 2U);
    sb = ft_snapshot(b, YEW_ARRAY_LEN(b), 3U);
    yew_fuss_tree_init(&ta);
    yew_fuss_tree_init(&tb);
    yew_fuss_build(&ta, &sa, &o);
    yew_fuss_build(&tb, &sb, &o);
    ft_expand_all(&ta);
    ft_expand_all(&tb);

    YEW_ASSERT_EQ_U64(ta.items.len, YEW_ARRAY_LEN(want));
    YEW_ASSERT_EQ_U64(tb.items.len, YEW_ARRAY_LEN(want));
    for (i = 0U; i < YEW_ARRAY_LEN(want); i++) {
        YEW_ASSERT_EQ_STR(ta.items.data[i].path, want[i]);
        YEW_ASSERT_EQ_STR(tb.items.data[i].path, want[i]);
        YEW_ASSERT_EQ_U64(ta.items.data[i].depth, tb.items.data[i].depth);
        YEW_ASSERT_EQ_I64(ta.items.data[i].is_file,
                          tb.items.data[i].is_file);
    }
    ft_drop(&ta);
    ft_drop(&tb);
}

void test_fusstree_untracked_directory_starts_collapsed(void)
{
    GitEntry e = ft_entry("vendor/");
    GitSnapshot s;
    FussOpts o = {false, false};
    FussTree t;
    const FussNode *n;

    e.kind = GIT_E_UNTRACKED;
    e.untracked = true;
    e.is_dir = true;
    s = ft_snapshot(&e, 1U, 4U);
    yew_fuss_tree_init(&t);
    yew_fuss_build(&t, &s, &o);
    n = ft_node(&t, "vendor");

    YEW_ASSERT_EQ_U64(t.items.len, 1U);
    YEW_ASSERT_NOT_NULL(n);
    YEW_ASSERT(!n->is_file);
    YEW_ASSERT(n->untracked);
    YEW_ASSERT(n->untracked_dir);
    YEW_ASSERT(!n->expanded);
    YEW_ASSERT_EQ_U64(n->first_child, 0U);
    ft_drop(&t);
}

void test_fusstree_hidden_rows_obey_the_option(void)
{
    GitEntry e[2];
    GitSnapshot s;
    FussOpts hidden = {false, false};
    FussOpts shown = {false, true};
    FussTree t;

    e[0] = ft_entry(".secret");
    e[0].unstaged = true;
    e[1] = ft_entry("ignored.log");
    e[1].kind = GIT_E_IGNORED;
    s = ft_snapshot(e, 2U, 5U);
    yew_fuss_tree_init(&t);
    yew_fuss_build(&t, &s, &hidden);
    YEW_ASSERT_EQ_U64(t.items.len, 0U);
    YEW_ASSERT_NULL(ft_item(&t, ".secret"));
    YEW_ASSERT_NULL(ft_item(&t, "ignored.log"));
    ft_drop(&t);

    yew_fuss_tree_init(&t);
    yew_fuss_build(&t, &s, &shown);
    YEW_ASSERT_EQ_U64(t.items.len, 2U);
    YEW_ASSERT_NOT_NULL(ft_item(&t, ".secret"));
    YEW_ASSERT_NOT_NULL(ft_item(&t, "ignored.log"));
    ft_drop(&t);
}

void test_fusstree_dirty_first_excludes_clean_until_all_files_is_enabled(void)
{
    GitEntry e[2];
    GitSnapshot s;
    FussOpts dirty = {false, false};
    FussOpts all = {true, false};
    FussTree t;

    e[0] = ft_entry("clean.c");
    e[1] = ft_entry("dirty.c");
    e[1].unstaged = true;
    s = ft_snapshot(e, 2U, 6U);
    yew_fuss_tree_init(&t);
    yew_fuss_build(&t, &s, &dirty);
    YEW_ASSERT_EQ_U64(t.items.len, 1U);
    YEW_ASSERT_NULL(ft_item(&t, "clean.c"));
    YEW_ASSERT_NOT_NULL(ft_item(&t, "dirty.c"));
    YEW_ASSERT(!t.all_files);

    yew_fuss_build(&t, &s, &all);
    YEW_ASSERT_EQ_U64(t.items.len, 2U);
    YEW_ASSERT_NOT_NULL(ft_item(&t, "clean.c"));
    YEW_ASSERT_NOT_NULL(ft_item(&t, "dirty.c"));
    YEW_ASSERT(t.all_files);
    ft_drop(&t);
}

void test_fusstree_all_files_preserves_ignored_row_style(void)
{
    char *paths[] = {(char *)"clean.c", (char *)"nested/ignored.log"};
    u8 kinds[] = {0U, 0U};
    GitPath ignored[] = {
        {(char *)"nested/ignored.log", 18U, false}
    };
    GitSnapshot s = ft_snapshot(NULL, 0U, 7U);
    FileList files = {0};
    FussOpts o = {true, true};
    FussTree t;
    const FussNode *nested;
    const FussNode *row;

    s.ignored.data = ignored;
    s.ignored.len = YEW_ARRAY_LEN(ignored);
    files.paths.data = paths;
    files.paths.len = YEW_ARRAY_LEN(paths);
    files.paths.cap = YEW_ARRAY_LEN(paths);
    files.is_dir.data = kinds;
    files.is_dir.len = YEW_ARRAY_LEN(kinds);
    files.is_dir.cap = YEW_ARRAY_LEN(kinds);
    yew_fuss_tree_init(&t);

    YEW_ASSERT(yew_fuss_merge_files(&t, &files, &s, &o));
    row = ft_node(&t, "nested/ignored.log");
    nested = ft_node(&t, "nested");
    YEW_ASSERT_NOT_NULL(row);
    YEW_ASSERT(row->ignored);
    YEW_ASSERT_NOT_NULL(nested);
    YEW_ASSERT(nested->ignored);
    ft_drop(&t);
}

void test_fusstree_items_hold_rebuild_safe_indices(void)
{
    GitEntry e[8];
    GitSnapshot s;
    FussOpts o = {false, true};
    FussTree t;
    size_t i;

    ft_fill_fixture(e);
    s = ft_snapshot(e, YEW_ARRAY_LEN(e), 9U);
    yew_fuss_tree_init(&t);
    yew_fuss_build(&t, &s, &o);
    for (i = 0U; i < t.items.len; i++) {
        YEW_ASSERT(t.items.data[i].node > 0U);
        YEW_ASSERT(t.items.data[i].node < t.nodes.len);
        YEW_ASSERT_NOT_NULL(t.items.data[i].path);
    }

    s.gen = 10U;
    yew_fuss_build(&t, &s, &o);
    YEW_ASSERT_EQ_U64(t.snap_gen, 10U);
    for (i = 0U; i < t.items.len; i++) {
        YEW_ASSERT(t.items.data[i].node > 0U);
        YEW_ASSERT(t.items.data[i].node < t.nodes.len);
        YEW_ASSERT_NOT_NULL(t.items.data[i].path);
        YEW_ASSERT_EQ_U64(t.items.data[i].path_len,
                          strlen(t.items.data[i].path));
    }
    ft_drop(&t);
}

void test_fusstree_untracked_expansion_splices_one_level_and_caches(void)
{
    GitEntry entry = ft_entry("vendor/");
    GitSnapshot s;
    GitPath child_data[3];
    GitPathList children;
    FussOpts o = {false, false};
    FussTree t;
    const FussNode *vendor;
    u32 node;

    entry.kind = GIT_E_UNTRACKED;
    entry.untracked = true;
    entry.is_dir = true;
    s = ft_snapshot(&entry, 1U, 11U);
    child_data[0] = (GitPath){"vendor/z.c", 10U, false};
    child_data[1] = (GitPath){"vendor/sub/", 11U, true};
    child_data[2] = (GitPath){"vendor/A.c", 10U, false};
    children.data = child_data;
    children.len = YEW_ARRAY_LEN(child_data);
    yew_fuss_tree_init(&t);
    yew_fuss_build(&t, &s, &o);
    vendor = ft_node(&t, "vendor");
    YEW_ASSERT_NOT_NULL(vendor);
    node = (u32)(vendor - t.nodes.data);

    YEW_ASSERT(yew_fuss_expand_untracked(&t, node, &children));
    vendor = ft_node(&t, "vendor");
    YEW_ASSERT_NOT_NULL(vendor);
    YEW_ASSERT(vendor->expanded);
    YEW_ASSERT(vendor->untracked_dir);
    YEW_ASSERT_NOT_NULL(ft_node(&t, "vendor/sub"));
    YEW_ASSERT_NOT_NULL(ft_node(&t, "vendor/A.c"));
    YEW_ASSERT_NOT_NULL(ft_node(&t, "vendor/z.c"));
    YEW_ASSERT_EQ_STR(t.items.data[1].path, "vendor/sub");
    YEW_ASSERT_EQ_STR(t.items.data[2].path, "vendor/A.c");
    YEW_ASSERT_EQ_STR(t.items.data[3].path, "vendor/z.c");
    YEW_ASSERT(!yew_fuss_expand_untracked(&t, node, &children));
    YEW_ASSERT_EQ_U64(t.items.len, 4U);
    ft_drop(&t);
}

void test_fusstree_untracked_expansion_cache_survives_refresh(void)
{
    GitEntry entry = ft_entry("vendor/");
    GitSnapshot s;
    GitPath child = {"vendor/A.c", 10U, false};
    GitPathList children = {&child, 1U};
    FussOpts o = {false, false};
    FussTree t;
    const FussNode *vendor;
    u32 node;

    entry.kind = GIT_E_UNTRACKED;
    entry.untracked = true;
    entry.is_dir = true;
    s = ft_snapshot(&entry, 1U, 12U);
    yew_fuss_tree_init(&t);
    yew_fuss_build(&t, &s, &o);
    vendor = ft_node(&t, "vendor");
    YEW_ASSERT_NOT_NULL(vendor);
    node = (u32)(vendor - t.nodes.data);
    YEW_ASSERT(yew_fuss_expand_untracked(&t, node, &children));
    YEW_ASSERT(yew_fuss_nav_toggle(&t, 0));

    s.gen++;
    yew_fuss_build(&t, &s, &o);
    vendor = ft_node(&t, "vendor");
    YEW_ASSERT_NOT_NULL(vendor);
    YEW_ASSERT(vendor->untracked_loaded);
    YEW_ASSERT(!vendor->expanded);
    YEW_ASSERT_NOT_NULL(ft_node_raw(&t, "vendor/A.c"));
    YEW_ASSERT_EQ_U64(t.items.len, 1U);

    YEW_ASSERT(yew_fuss_nav_toggle(&t, 0));
    YEW_ASSERT_EQ_U64(t.items.len, 2U);
    YEW_ASSERT_EQ_STR(t.items.data[1].path, "vendor/A.c");
    vendor = ft_node(&t, "vendor");
    node = (u32)(vendor - t.nodes.data);
    YEW_ASSERT(!yew_fuss_expand_untracked(&t, node, &children));
    ft_drop(&t);
}
