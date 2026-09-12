/* Sprint 57.11: exact :!yew FILE handoff into the current editor. */
#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700

#include "harness.h"

#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "edit/ed.h"
#include "edit/shell.h"
#include "edit/shell_cmds.h"
#include "ui/groups.h"
#include "ui/tabs.h"

typedef struct SelfFix {
    Ed ed;
    char base[PATH_MAX];
    char workspace[PATH_MAX];
    char shared[PATH_MAX];
} SelfFix;

static void self_mkdir(const char *path)
{
    YEW_ASSERT_EQ_I64(mkdir(path, 0700), 0);
}

static void self_path(char out[PATH_MAX], const char *dir, const char *name)
{
    int n = snprintf(out, PATH_MAX, "%s/%s", dir, name);

    YEW_ASSERT(n > 0 && n < PATH_MAX);
}

static void self_write(const char *path, const char *text)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    size_t len = strlen(text);

    YEW_ASSERT(fd >= 0);
    if (fd < 0)
        return;
    YEW_ASSERT_EQ_I64(write(fd, text, len), (i64)len);
    YEW_ASSERT_EQ_I64(close(fd), 0);
}

static void self_fix_make(SelfFix *f)
{
    const char *tmp = getenv("TMPDIR");
    int n;

    if (tmp == NULL || tmp[0] == '\0')
        tmp = "/tmp";
    n = snprintf(f->base, sizeof(f->base), "%s/yew-self-XXXXXX", tmp);
    YEW_ASSERT(n > 0 && (size_t)n < sizeof(f->base));
    YEW_ASSERT_NOT_NULL(mkdtemp(f->base));
    YEW_ASSERT(yew_test_canonicalize_path(f->base, sizeof(f->base)));
    self_path(f->workspace, f->base, "work");
    self_path(f->shared, f->base, "shared");
    self_mkdir(f->workspace);
    self_mkdir(f->shared);
    yew_cmd_shutdown();
    yew_cmd_init();
    yew_ed_init(&f->ed);
    YEW_ASSERT(yew_ed_open_scratch(&f->ed));
    YEW_ASSERT(yew_ed_set_workspace_root(&f->ed, f->workspace));
    yew_layout_compute(f->ed.pane_root, (Rect){0U, 0U, 80U, 24U});
}

static YewShellSelfResult self_try(Ed *ed, const char *command)
{
    char error[256] = {0};

    return yew_shell_try_self_open(ed, command, error, sizeof(error));
}

void test_shell_self_recognizes_only_exact_safe_argv(void)
{
    static const char *const fallback[] = {
        "yew a.c b.c",
        "yew --version",
        "./build/yew note.txt",
        "command yew note.txt",
        "yew \"$FILE\"",
        "yew *.c",
        "yew note.txt | sed -n 1p",
        "yew note.txt; echo done",
        "yew note.txt >out",
        "yew note.txt\n",
        "yew 'unterminated"
    };
    SelfFix f;
    char note[PATH_MAX];
    char spaced[PATH_MAX];
    char draft[PATH_MAX];
    char directory[PATH_MAX];
    CmdCtx command = {0};
    size_t i;

    self_fix_make(&f);
    self_path(note, f.workspace, "note.txt");
    self_path(spaced, f.workspace, "notes one.md");
    self_path(draft, f.workspace, "-draft");
    self_path(directory, f.workspace, "adir");
    self_write(note, "note\n");
    self_write(spaced, "space\n");
    self_write(draft, "draft\n");
    self_mkdir(directory);

    command.ed = &f.ed;
    command.win = f.ed.win;
    command.sarg = " yew note.txt \t";
    command.sarg_len = (u32)strlen(command.sarg);
    YEW_ASSERT_EQ_U64(yew_shell_cmd_run(&command), YEW_CMD_OK);
    YEW_ASSERT_EQ_STR(yew_tab_at(&f.ed, f.ed.tabs.active)->path, note);
    YEW_ASSERT_EQ_U64(self_try(&f.ed, "yew 'note.txt'"),
                      YEW_SHELL_SELF_OPENED);
    YEW_ASSERT_EQ_U64(self_try(&f.ed, "yew \"notes one.md\""),
                      YEW_SHELL_SELF_OPENED);
    YEW_ASSERT_EQ_U64(self_try(&f.ed, "yew notes\\ one.md"),
                      YEW_SHELL_SELF_OPENED);
    YEW_ASSERT_EQ_U64(self_try(&f.ed, "yew -- '-draft'"),
                      YEW_SHELL_SELF_OPENED);
    YEW_ASSERT_EQ_U64(self_try(&f.ed, "yew"), YEW_SHELL_SELF_ERROR);
    YEW_ASSERT_EQ_U64(self_try(&f.ed, "yew ''"), YEW_SHELL_SELF_ERROR);
    YEW_ASSERT_EQ_U64(self_try(&f.ed, "yew --"), YEW_SHELL_SELF_ERROR);
    YEW_ASSERT_EQ_U64(self_try(&f.ed, "yew adir"),
                      YEW_SHELL_SELF_NOT_HANDLED);
    for (i = 0U; i < YEW_ARRAY_LEN(fallback); i++)
        YEW_ASSERT_EQ_U64(self_try(&f.ed, fallback[i]),
                          YEW_SHELL_SELF_NOT_HANDLED);
    for (i = 0U; i < 10000U; i++)
        YEW_ASSERT_EQ_U64(self_try(&f.ed, "yew note.txt | false"),
                          YEW_SHELL_SELF_NOT_HANDLED);
    for (i = 0U; i < 1000U; i++)
        YEW_ASSERT_EQ_U64(self_try(&f.ed, "yew note.txt"),
                          YEW_SHELL_SELF_OPENED);
    YEW_ASSERT_EQ_U64(f.ed.jobs.len, 0U);
    YEW_ASSERT_EQ_U64(yew_tab_count(&f.ed), 4U);

    yew_ed_free(&f.ed);
    YEW_ASSERT_EQ_I64(unlink(note), 0);
    YEW_ASSERT_EQ_I64(unlink(spaced), 0);
    YEW_ASSERT_EQ_I64(unlink(draft), 0);
    YEW_ASSERT_EQ_I64(rmdir(directory), 0);
    YEW_ASSERT_EQ_I64(rmdir(f.workspace), 0);
    YEW_ASSERT_EQ_I64(rmdir(f.shared), 0);
    YEW_ASSERT_EQ_I64(rmdir(f.base), 0);
}

void test_shell_self_resolves_workspace_and_outside_tabs(void)
{
    SelfFix f;
    char inside[PATH_MAX];
    char outside[PATH_MAX];
    char command[PATH_MAX + 8U];
    u32 broad;
    u32 before;
    int idx;

    self_fix_make(&f);
    self_path(inside, f.workspace, "inside.txt");
    self_path(outside, f.shared, "outside.txt");
    self_write(inside, "inside\n");
    self_write(outside, "outside\n");

    /* Even an unusual broad group must not absorb an outside-workspace tab. */
    broad = yew_group_create(&f.ed, f.base, "broad");
    yew_group_add_member(&f.ed, broad, 0);
    YEW_ASSERT_EQ_U64(self_try(&f.ed, "yew inside.txt"),
                      YEW_SHELL_SELF_OPENED);
    idx = f.ed.tabs.active;
    YEW_ASSERT_EQ_STR(yew_tab_at(&f.ed, idx)->path, inside);
    YEW_ASSERT_EQ_U64(yew_tab_at(&f.ed, idx)->group_id, broad);

    YEW_ASSERT_EQ_U64(self_try(&f.ed, "yew ../shared/outside.txt"),
                      YEW_SHELL_SELF_OPENED);
    idx = f.ed.tabs.active;
    YEW_ASSERT_EQ_STR(yew_tab_at(&f.ed, idx)->path, outside);
    YEW_ASSERT_EQ_U64(yew_tab_at(&f.ed, idx)->group_id, 0U);
    YEW_ASSERT_EQ_I64(idx, (int)yew_tab_count(&f.ed) - 1);
    before = yew_tab_count(&f.ed);
    YEW_ASSERT(snprintf(command, sizeof(command), "yew %s", outside) > 0);
    YEW_ASSERT_EQ_U64(self_try(&f.ed, command), YEW_SHELL_SELF_OPENED);
    YEW_ASSERT_EQ_U64(yew_tab_count(&f.ed), before);
    YEW_ASSERT_EQ_STR(yew_tab_at(&f.ed, f.ed.tabs.active)->path, outside);
    YEW_ASSERT_EQ_U64(f.ed.jobs.len, 0U);

    yew_ed_free(&f.ed);
    YEW_ASSERT_EQ_I64(unlink(inside), 0);
    YEW_ASSERT_EQ_I64(unlink(outside), 0);
    YEW_ASSERT_EQ_I64(rmdir(f.workspace), 0);
    YEW_ASSERT_EQ_I64(rmdir(f.shared), 0);
    YEW_ASSERT_EQ_I64(rmdir(f.base), 0);
}

void test_shell_self_places_groups_by_root_and_stable_membership(void)
{
    SelfFix f;
    char src[PATH_MAX];
    char deep[PATH_MAX];
    char src2[PATH_MAX];
    char alias[PATH_MAX];
    char path[PATH_MAX];
    u32 outer;
    u32 nested;
    u32 equal;
    int outer_seed;
    int nested_seed;
    int equal_seed;
    int existing;
    u32 existing_ordinal;

    self_fix_make(&f);
    self_path(src, f.workspace, "src");
    self_path(deep, src, "deep");
    self_path(src2, f.workspace, "src2");
    self_path(alias, f.workspace, "alias");
    self_mkdir(src);
    self_mkdir(deep);
    self_mkdir(src2);
    YEW_ASSERT_EQ_I64(symlink(deep, alias), 0);

    self_path(path, src, "outer.txt");
    self_write(path, "outer\n");
    YEW_ASSERT_EQ_U64(self_try(&f.ed, "yew src/outer.txt"),
                      YEW_SHELL_SELF_OPENED);
    outer_seed = f.ed.tabs.active;
    outer = yew_group_create(&f.ed, src, "outer");
    yew_group_add_member(&f.ed, outer, outer_seed);

    self_path(path, deep, "nested.txt");
    self_write(path, "nested\n");
    YEW_ASSERT_EQ_U64(self_try(&f.ed, "yew src/deep/nested.txt"),
                      YEW_SHELL_SELF_OPENED);
    nested_seed = f.ed.tabs.active;
    nested = yew_group_create(&f.ed, deep, "nested");
    yew_group_add_member(&f.ed, nested, nested_seed);

    self_path(path, src, "existing.txt");
    self_write(path, "existing\n");
    YEW_ASSERT_EQ_U64(self_try(&f.ed, "yew src/existing.txt"),
                      YEW_SHELL_SELF_OPENED);
    existing = f.ed.tabs.active;
    yew_group_add_member(&f.ed, outer, existing);
    existing_ordinal = yew_tab_at(&f.ed, existing)->group_ordinal;

    yew_tab_switch(&f.ed, outer_seed);
    YEW_ASSERT_EQ_U64(self_try(&f.ed, "yew src/deep/new.txt"),
                      YEW_SHELL_SELF_OPENED);
    YEW_ASSERT_EQ_U64(yew_tab_at(&f.ed, f.ed.tabs.active)->group_id, nested);
    YEW_ASSERT_EQ_U64(yew_tab_at(&f.ed, f.ed.tabs.active)->group_ordinal, 2U);

    equal = yew_group_create(&f.ed, deep, "equal");
    YEW_ASSERT_EQ_U64(self_try(&f.ed, "yew src/deep/equal-seed.txt"),
                      YEW_SHELL_SELF_OPENED);
    equal_seed = f.ed.tabs.active;
    yew_group_add_member(&f.ed, equal, equal_seed);
    yew_tab_switch(&f.ed, equal_seed);
    YEW_ASSERT_EQ_U64(self_try(&f.ed, "yew src/deep/equal-new.txt"),
                      YEW_SHELL_SELF_OPENED);
    YEW_ASSERT_EQ_U64(yew_tab_at(&f.ed, f.ed.tabs.active)->group_id, equal);
    YEW_ASSERT_EQ_U64(yew_tab_at(&f.ed, f.ed.tabs.active)->group_ordinal, 2U);

    YEW_ASSERT_EQ_U64(self_try(&f.ed, "yew src2/collision.txt"),
                      YEW_SHELL_SELF_OPENED);
    YEW_ASSERT_EQ_U64(yew_tab_at(&f.ed, f.ed.tabs.active)->group_id, 0U);

    /* Canonicalize a new file through a symlinked parent before grouping. */
    yew_tab_switch(&f.ed, nested_seed);
    YEW_ASSERT_EQ_U64(self_try(&f.ed, "yew alias/symlink-new.txt"),
                      YEW_SHELL_SELF_OPENED);
    YEW_ASSERT_EQ_U64(yew_tab_at(&f.ed, f.ed.tabs.active)->group_id, nested);
    self_path(path, deep, "symlink-new.txt");
    YEW_ASSERT_EQ_STR(yew_tab_at(&f.ed, f.ed.tabs.active)->path, path);

    yew_tab_switch(&f.ed, equal_seed);
    YEW_ASSERT_EQ_U64(self_try(&f.ed, "yew src/existing.txt"),
                      YEW_SHELL_SELF_OPENED);
    YEW_ASSERT_EQ_I64(f.ed.tabs.active, existing);
    YEW_ASSERT_EQ_U64(yew_tab_at(&f.ed, existing)->group_id, outer);
    YEW_ASSERT_EQ_U64(yew_tab_at(&f.ed, existing)->group_ordinal,
                      existing_ordinal);

    yew_ed_free(&f.ed);
    self_path(path, src, "outer.txt");
    YEW_ASSERT_EQ_I64(unlink(path), 0);
    self_path(path, deep, "nested.txt");
    YEW_ASSERT_EQ_I64(unlink(path), 0);
    self_path(path, src, "existing.txt");
    YEW_ASSERT_EQ_I64(unlink(path), 0);
    YEW_ASSERT_EQ_I64(unlink(alias), 0);
    YEW_ASSERT_EQ_I64(rmdir(deep), 0);
    YEW_ASSERT_EQ_I64(rmdir(src), 0);
    YEW_ASSERT_EQ_I64(rmdir(src2), 0);
    YEW_ASSERT_EQ_I64(rmdir(f.workspace), 0);
    YEW_ASSERT_EQ_I64(rmdir(f.shared), 0);
    YEW_ASSERT_EQ_I64(rmdir(f.base), 0);
}

void test_shell_self_errors_roll_back_tabs_buffers_and_focus(void)
{
    SelfFix f;
    char too_long[PATH_MAX + 32U];
    char cap_path[64];
    u32 tab_count;
    u32 buffer_count;
    u32 group_count;
    u32 active_id;

    self_fix_make(&f);
    tab_count = yew_tab_count(&f.ed);
    buffer_count = f.ed.ws.nbufs;
    group_count = (u32)f.ed.groups.v.len;
    active_id = yew_tab_at(&f.ed, f.ed.tabs.active)->tab_id;

    YEW_ASSERT_EQ_U64(self_try(&f.ed, "yew /dev/null/child"),
                      YEW_SHELL_SELF_ERROR);
    YEW_ASSERT_EQ_U64(yew_tab_count(&f.ed), tab_count);
    YEW_ASSERT_EQ_U64(f.ed.ws.nbufs, buffer_count);
    YEW_ASSERT_EQ_U64(f.ed.groups.v.len, group_count);
    YEW_ASSERT_EQ_U64(yew_tab_at(&f.ed, f.ed.tabs.active)->tab_id,
                      active_id);
    YEW_ASSERT_EQ_U64(f.ed.jobs.len, 0U);

    (void)memcpy(too_long, "yew /", 5U);
    (void)memset(too_long + 5U, 'a', sizeof(too_long) - 6U);
    too_long[sizeof(too_long) - 1U] = '\0';
    YEW_ASSERT_EQ_U64(self_try(&f.ed, too_long), YEW_SHELL_SELF_ERROR);
    YEW_ASSERT_EQ_U64(yew_tab_count(&f.ed), tab_count);
    YEW_ASSERT_EQ_U64(f.ed.ws.nbufs, buffer_count);
    YEW_ASSERT_EQ_U64(yew_tab_at(&f.ed, f.ed.tabs.active)->tab_id,
                      active_id);

    while (yew_tab_count(&f.ed) < (u32)YEW_TAB_MAX) {
        int idx;

        YEW_ASSERT(snprintf(cap_path, sizeof(cap_path),
                            "/tmp/yew-self-cap-%u.txt",
                            (unsigned)yew_tab_count(&f.ed)) > 0);
        idx = yew_tab_open(&f.ed, cap_path);
        if (idx < 0)
            break;
    }
    tab_count = yew_tab_count(&f.ed);
    buffer_count = f.ed.ws.nbufs;
    group_count = (u32)f.ed.groups.v.len;
    active_id = yew_tab_at(&f.ed, f.ed.tabs.active)->tab_id;
    YEW_ASSERT_EQ_U64(self_try(&f.ed, "yew cap-over.txt"),
                      YEW_SHELL_SELF_ERROR);
    YEW_ASSERT_EQ_U64(yew_tab_count(&f.ed), tab_count);
    YEW_ASSERT_EQ_U64(f.ed.ws.nbufs, buffer_count);
    YEW_ASSERT_EQ_U64(f.ed.groups.v.len, group_count);
    YEW_ASSERT_EQ_U64(yew_tab_at(&f.ed, f.ed.tabs.active)->tab_id,
                      active_id);
    YEW_ASSERT_EQ_U64(f.ed.jobs.len, 0U);

    yew_ed_free(&f.ed);
    YEW_ASSERT_EQ_I64(rmdir(f.workspace), 0);
    YEW_ASSERT_EQ_I64(rmdir(f.shared), 0);
    YEW_ASSERT_EQ_I64(rmdir(f.base), 0);
}
