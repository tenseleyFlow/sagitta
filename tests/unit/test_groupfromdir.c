#define _POSIX_C_SOURCE 200809L

#include "harness.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "edit/ed.h"
#include "text/file.h"
#include "ui/groupfromdir.h"
#include "ui/grouppicker.h"
#include "ui/groups.h"
#include "ui/tabs.h"

typedef struct GroupDirTree {
    char root[64];
    char sub[128];
    char git[128];
    char a[256];
    char z[256];
    char nested[256];
    char ignored[256];
    char git_file[256];
    char ignore_file[256];
} GroupDirTree;

static void gd_write(const char *path)
{
    FILE *file = fopen(path, "w");

    YEW_ASSERT_NOT_NULL(file);
    YEW_ASSERT(fputs("group from dir\n", file) >= 0);
    YEW_ASSERT_EQ_I64(fclose(file), 0);
}

static void gd_make(GroupDirTree *tree)
{
    (void)snprintf(tree->root, sizeof(tree->root),
                   "/tmp/yew-group-dir-XXXXXX");
    YEW_ASSERT_NOT_NULL(mkdtemp(tree->root));
    YEW_ASSERT(yew_test_canonicalize_path(tree->root,
                                           sizeof(tree->root)));
    (void)snprintf(tree->sub, sizeof(tree->sub), "%s/sub", tree->root);
    (void)snprintf(tree->git, sizeof(tree->git), "%s/.git", tree->root);
    YEW_ASSERT_EQ_I64(mkdir(tree->sub, 0700), 0);
    YEW_ASSERT_EQ_I64(mkdir(tree->git, 0700), 0);
    (void)snprintf(tree->a, sizeof(tree->a), "%s/a.txt", tree->root);
    (void)snprintf(tree->z, sizeof(tree->z), "%s/z.txt", tree->root);
    (void)snprintf(tree->nested, sizeof(tree->nested), "%s/m.txt",
                   tree->sub);
    (void)snprintf(tree->ignored, sizeof(tree->ignored),
                   "%s/ignored.txt", tree->root);
    (void)snprintf(tree->git_file, sizeof(tree->git_file), "%s/index",
                   tree->git);
    (void)snprintf(tree->ignore_file, sizeof(tree->ignore_file),
                   "%s/.gitignore", tree->root);
    /* Deliberately scrambled creation order. */
    gd_write(tree->z);
    gd_write(tree->nested);
    gd_write(tree->ignored);
    gd_write(tree->a);
    gd_write(tree->git_file);
    {
        FILE *file = fopen(tree->ignore_file, "w");

        YEW_ASSERT_NOT_NULL(file);
        YEW_ASSERT(fputs("ignored.txt\n", file) >= 0);
        YEW_ASSERT_EQ_I64(fclose(file), 0);
    }
}

static void gd_remove(GroupDirTree *tree)
{
    (void)unlink(tree->a);
    (void)unlink(tree->z);
    (void)unlink(tree->nested);
    (void)unlink(tree->ignored);
    (void)unlink(tree->git_file);
    (void)unlink(tree->ignore_file);
    (void)rmdir(tree->sub);
    (void)rmdir(tree->git);
    (void)rmdir(tree->root);
}

static void gd_ed(Ed *ed)
{
    yew_cmd_shutdown();
    yew_cmd_init();
    yew_ed_init(ed);
    YEW_ASSERT(yew_ed_open_scratch(ed));
    yew_layout_compute(ed->pane_root, (Rect){0U, 0U, 80U, 24U});
}

static Key gd_key(u32 code)
{
    Key key = {0};

    key.kind = YEW_EV_KEY;
    key.ev = YEW_KEY_PRESS;
    key.code = code;
    return key;
}

void test_groupfromdir_sorted_recursive_ignored_and_one_read(void)
{
    GroupDirTree tree;
    GroupFromDirOpts opts = {true, true, YEW_GROUP_MAX_MEMBERS, "work"};
    Ed ed;
    int members[8];
    u64 reads;
    u32 gid;
    int n;

    gd_make(&tree);
    gd_ed(&ed);
    reads = yew_file_load_count();
    gid = yew_group_from_dir(&ed, tree.root, &opts);
    YEW_ASSERT(gid != 0U);
    YEW_ASSERT_EQ_U64(yew_file_load_count(), reads + 1U);
    n = yew_group_members(&ed, gid, members, (int)YEW_ARRAY_LEN(members));
    YEW_ASSERT_EQ_I64(n, 3);
    YEW_ASSERT_EQ_STR(yew_tab_at(&ed, members[0])->path, tree.a);
    YEW_ASSERT_EQ_STR(yew_tab_at(&ed, members[1])->path, tree.nested);
    YEW_ASSERT_EQ_STR(yew_tab_at(&ed, members[2])->path, tree.z);
    YEW_ASSERT_EQ_STR(yew_group_at(&ed, gid)->label, "work");
    YEW_ASSERT_EQ_STR(yew_group_at(&ed, gid)->dir_path, tree.root);
    YEW_ASSERT_EQ_STR(yew_group_at(&ed, gid)->last_active_member, tree.a);
    YEW_ASSERT_EQ_I64(ed.tabs.active, members[0]);
    YEW_ASSERT(yew_tab_is_resident(&ed, members[0]));
    YEW_ASSERT(!yew_tab_is_resident(&ed, members[1]));
    YEW_ASSERT(!yew_tab_is_resident(&ed, members[2]));
    YEW_ASSERT_EQ_I64(yew_tab_find_by_path(&ed, tree.ignored), -1);
    YEW_ASSERT_EQ_I64(yew_tab_find_by_path(&ed, tree.git_file), -1);
    yew_ed_free(&ed);
    gd_remove(&tree);
}

void test_groupfromdir_nonrecursive_and_default_label(void)
{
    GroupDirTree tree;
    GroupFromDirOpts opts = {false, false, YEW_GROUP_MAX_MEMBERS, NULL};
    Ed ed;
    u32 gid;

    gd_make(&tree);
    gd_ed(&ed);
    gid = yew_group_from_dir(&ed, tree.root, &opts);
    YEW_ASSERT(gid != 0U);
    YEW_ASSERT_EQ_I64(yew_group_member_count(&ed, gid), 3);
    YEW_ASSERT_EQ_I64(yew_tab_find_by_path(&ed, tree.nested), -1);
    YEW_ASSERT_NOT_NULL(strstr(yew_group_at(&ed, gid)->label,
                               "yew-group-dir-"));
    yew_ed_free(&ed);
    gd_remove(&tree);
}

void test_groupfromdir_adopts_without_duplicate_and_repairs_ordinals(void)
{
    GroupDirTree tree;
    GroupFromDirOpts opts = {false, false, YEW_GROUP_MAX_MEMBERS, NULL};
    Ed ed;
    int a;
    int nested;
    u32 old_gid;
    u32 new_gid;
    u32 tabs_before;

    gd_make(&tree);
    gd_ed(&ed);
    a = yew_tab_open(&ed, tree.a);
    nested = yew_tab_open(&ed, tree.nested);
    YEW_ASSERT(a >= 0);
    YEW_ASSERT(nested >= 0);
    old_gid = yew_group_create(&ed, tree.root, "old");
    yew_group_add_member(&ed, old_gid, a);
    yew_group_add_member(&ed, old_gid, nested);
    tabs_before = yew_tab_count(&ed);

    new_gid = yew_group_from_dir(&ed, tree.root, &opts);
    YEW_ASSERT(new_gid != 0U);
    /* a is adopted, while z and ignored are newly opened. */
    YEW_ASSERT_EQ_U64(yew_tab_count(&ed), tabs_before + 2U);
    YEW_ASSERT_EQ_I64(yew_group_member_count(&ed, new_gid), 3);
    YEW_ASSERT_EQ_I64(yew_group_member_count(&ed, old_gid), 1);
    YEW_ASSERT_EQ_U64(yew_tab_at(&ed, nested)->group_ordinal, 1U);
    YEW_ASSERT_EQ_I64(yew_tab_find_by_path(&ed, tree.a), a);
    yew_ed_free(&ed);
    gd_remove(&tree);
}

void test_groupfromdir_empty_reports_and_oversize_waits_for_confirm(void)
{
    GroupDirTree tree;
    GroupFromDirOpts opts = {false, true, 1U, NULL};
    Ed ed;
    char empty[64] = "/tmp/yew-group-empty-XXXXXX";
    u32 tabs_before;

    gd_make(&tree);
    gd_ed(&ed);
    YEW_ASSERT_NOT_NULL(mkdtemp(empty));
    YEW_ASSERT_EQ_U64(yew_group_from_dir(&ed, empty, NULL), 0U);
    YEW_ASSERT_EQ_U64(ed.groups.v.len, 0U);
    YEW_ASSERT_NOT_NULL(strstr(ed.msg.text, "no files in"));

    tabs_before = yew_tab_count(&ed);
    YEW_ASSERT_EQ_U64(yew_group_from_dir(&ed, tree.root, &opts), 0U);
    YEW_ASSERT(yew_gp_active());
    YEW_ASSERT_EQ_U64(ed.groups.v.len, 0U);
    YEW_ASSERT_EQ_U64(yew_tab_count(&ed), tabs_before);
    YEW_ASSERT(yew_gp_key(&ed, gd_key(YEW_KEY_TAB)));
    YEW_ASSERT(yew_gp_key(&ed, gd_key(YEW_KEY_ENTER)));
    YEW_ASSERT_EQ_U64(ed.groups.v.len, 0U);
    yew_gp_apply(&ed);
    YEW_ASSERT_EQ_U64(ed.groups.v.len, 1U);
    YEW_ASSERT_EQ_I64(yew_group_member_count(&ed, ed.groups.v.data[0].id),
                      2);
    yew_ed_free(&ed);
    gd_remove(&tree);
    (void)rmdir(empty);
}
