#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "edit/ed.h"
#include "edit/job.h"
#include "edit/mode.h"
#include "mod/git/fussmode.h"
#include "text/file.h"
#include "ui/groupfromdir.h"
#include "ui/grouppicker.h"
#include "ui/groups.h"
#include "ui/message.h"
#include "ui/tabs.h"

static unsigned assertions;
static unsigned failures;

#define CHECK(expr) do {                                                   \
    assertions++;                                                          \
    if (!(expr)) {                                                         \
        (void)fprintf(stderr, "group_from_dir:%d: check failed: %s\n",   \
                      __LINE__, #expr);                                    \
        failures++;                                                        \
    }                                                                      \
} while (0)

static bool path_make(char *out, size_t cap, const char *parent,
                      const char *name)
{
    int n = snprintf(out, cap, "%s/%s", parent, name);

    return n > 0 && (size_t)n < cap;
}

static bool file_write(const char *path, const char *bytes)
{
    size_t len = strlen(bytes);
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
    const char *at = bytes;

    if (fd < 0)
        return false;
    while (len != 0U) {
        ssize_t wrote = write(fd, at, len);

        if (wrote < 0 && errno == EINTR)
            continue;
        if (wrote <= 0) {
            (void)close(fd);
            return false;
        }
        at += (size_t)wrote;
        len -= (size_t)wrote;
    }
    return close(fd) == 0;
}

static void ed_init(Ed *ed)
{
    yew_ed_init(ed);
    CHECK(yew_ed_open_scratch(ed));
    yew_layout_compute(ed->pane_root, (Rect){0U, 0U, 80U, 24U});
}

static bool make_numbered_files(const char *dir, unsigned count,
                                bool reverse)
{
    unsigned step;

    for (step = 0U; step < count; step++) {
        unsigned i = reverse ? count - step - 1U : step;
        char name[32];
        char path[4096];

        if (snprintf(name, sizeof(name), "file-%04u.txt", i) <= 0 ||
            !path_make(path, sizeof(path), dir, name) ||
            !file_write(path, "group acceptance\n"))
            return false;
    }
    return true;
}

static void remove_numbered_files(const char *dir, unsigned count)
{
    unsigned i;

    for (i = 0U; i < count; i++) {
        char name[32];
        char path[4096];

        if (snprintf(name, sizeof(name), "file-%04u.txt", i) > 0 &&
            path_make(path, sizeof(path), dir, name))
            CHECK(unlink(path) == 0);
    }
}

static void check_sorted_group(const char *dir)
{
    GroupFromDirOpts opts = {false, false, YEW_GROUP_MAX_MEMBERS, "forty"};
    int members[YEW_GROUP_MAX_MEMBERS];
    u64 reads_before;
    Ed ed;
    u32 gid;
    int n;
    int i;

    ed_init(&ed);
    reads_before = yew_file_load_count();
    gid = yew_group_from_dir(&ed, dir, &opts);
    CHECK(gid != 0U);
    CHECK(yew_file_load_count() == reads_before + 1U);
    n = yew_group_members(&ed, gid, members,
                          (int)YEW_ARRAY_LEN(members));
    CHECK(n == 40);
    for (i = 0; i < n; i++) {
        char name[32];
        char expected[4096];
        Tab *tab = yew_tab_at(&ed, members[i]);

        CHECK(snprintf(name, sizeof(name), "file-%04d.txt", i) > 0);
        CHECK(path_make(expected, sizeof(expected), dir, name));
        CHECK(tab != NULL);
        if (tab != NULL) {
            CHECK(tab->path != NULL && strcmp(tab->path, expected) == 0);
            CHECK(tab->group_ordinal == (u32)i + 1U);
            CHECK(yew_tab_is_resident(&ed, members[i]) == (i == 0));
        }
    }
    yew_ed_free(&ed);
}

static void test_scrambled_forty_is_deterministic(const char *parent)
{
    char dir[4096];

    CHECK(path_make(dir, sizeof(dir), parent, "scrambled-forty"));
    CHECK(mkdir(dir, 0700) == 0);
    CHECK(make_numbered_files(dir, 40U, true));
    check_sorted_group(dir);
    check_sorted_group(dir);
    remove_numbered_files(dir, 40U);
    CHECK(rmdir(dir) == 0);
}

static void test_open_tab_is_adopted_without_duplication(const char *parent)
{
    GroupFromDirOpts opts = {false, false, YEW_GROUP_MAX_MEMBERS, NULL};
    char dir[4096];
    char first[4096];
    int first_idx;
    u32 tabs_before;
    u32 gid;
    Ed ed;

    CHECK(path_make(dir, sizeof(dir), parent, "adopt-open"));
    CHECK(mkdir(dir, 0700) == 0);
    CHECK(make_numbered_files(dir, 3U, true));
    CHECK(path_make(first, sizeof(first), dir, "file-0000.txt"));
    ed_init(&ed);
    first_idx = yew_tab_open(&ed, first);
    CHECK(first_idx >= 0);
    tabs_before = yew_tab_count(&ed);
    gid = yew_group_from_dir(&ed, dir, &opts);
    CHECK(gid != 0U);
    CHECK(yew_tab_count(&ed) == tabs_before + 2U);
    CHECK(yew_tab_find_by_path(&ed, first) == first_idx);
    CHECK(yew_group_member_count(&ed, gid) == 3);
    yew_ed_free(&ed);
    remove_numbered_files(dir, 3U);
    CHECK(rmdir(dir) == 0);
}

static void test_other_group_adoption_compacts_both_groups(const char *parent)
{
    GroupFromDirOpts opts = {false, false, YEW_GROUP_MAX_MEMBERS, NULL};
    char dir[4096];
    char outside[4096];
    char paths[3][4096];
    int adopted[2];
    int keeper;
    int members[8];
    u32 old_gid;
    u32 new_gid;
    Ed ed;
    int n;
    int i;

    CHECK(path_make(dir, sizeof(dir), parent, "adopt-group"));
    CHECK(mkdir(dir, 0700) == 0);
    CHECK(make_numbered_files(dir, 3U, true));
    CHECK(path_make(outside, sizeof(outside), parent, "group-keeper.txt"));
    CHECK(file_write(outside, "keep\n"));
    for (i = 0; i < 3; i++) {
        char name[32];

        CHECK(snprintf(name, sizeof(name), "file-%04d.txt", i) > 0);
        CHECK(path_make(paths[i], sizeof(paths[i]), dir, name));
    }
    ed_init(&ed);
    adopted[0] = yew_tab_open(&ed, paths[1]);
    keeper = yew_tab_open(&ed, outside);
    adopted[1] = yew_tab_open(&ed, paths[2]);
    CHECK(adopted[0] >= 0 && keeper >= 0 && adopted[1] >= 0);
    old_gid = yew_group_create(&ed, parent, "old");
    yew_group_add_member(&ed, old_gid, adopted[0]);
    yew_group_add_member(&ed, old_gid, keeper);
    yew_group_add_member(&ed, old_gid, adopted[1]);

    new_gid = yew_group_from_dir(&ed, dir, &opts);
    CHECK(new_gid != 0U);
    CHECK(yew_group_member_count(&ed, old_gid) == 1);
    CHECK(yew_tab_at(&ed, keeper)->group_ordinal == 1U);
    n = yew_group_members(&ed, new_gid, members,
                          (int)YEW_ARRAY_LEN(members));
    CHECK(n == 3);
    for (i = 0; i < n; i++) {
        Tab *tab = yew_tab_at(&ed, members[i]);

        CHECK(tab != NULL && tab->path != NULL &&
              strcmp(tab->path, paths[i]) == 0);
        CHECK(tab != NULL && tab->group_ordinal == (u32)i + 1U);
    }
    yew_ed_free(&ed);
    CHECK(unlink(outside) == 0);
    remove_numbered_files(dir, 3U);
    CHECK(rmdir(dir) == 0);
}

static Key key_press(u32 code)
{
    Key key = {0};

    key.kind = YEW_EV_KEY;
    key.ev = YEW_KEY_PRESS;
    key.code = code;
    if (code < 128U) {
        key.text[0] = (u8)code;
        key.ntext = 1U;
    }
    return key;
}

static void test_five_thousand_files_open_picker_only(const char *parent)
{
    GroupFromDirOpts opts = {false, false, YEW_GROUP_MAX_MEMBERS, NULL};
    char dir[4096];
    u32 tabs_before;
    Ed ed;

    CHECK(path_make(dir, sizeof(dir), parent, "five-thousand"));
    CHECK(mkdir(dir, 0700) == 0);
    CHECK(make_numbered_files(dir, 5000U, true));
    ed_init(&ed);
    tabs_before = yew_tab_count(&ed);
    CHECK(yew_group_from_dir(&ed, dir, &opts) == 0U);
    CHECK(yew_gp_active());
    CHECK(ed.groups.v.len == 0U);
    CHECK(yew_tab_count(&ed) == tabs_before);
    CHECK(yew_gp_key(&ed, key_press(YEW_KEY_ESCAPE)));
    CHECK(!yew_gp_active());
    yew_ed_free(&ed);
    remove_numbered_files(dir, 5000U);
    CHECK(rmdir(dir) == 0);
}

static void test_empty_directory_reports_without_creating_group(
    const char *parent)
{
    char dir[4096];
    Ed ed;

    CHECK(path_make(dir, sizeof(dir), parent, "empty"));
    CHECK(mkdir(dir, 0700) == 0);
    ed_init(&ed);
    CHECK(yew_group_from_dir(&ed, dir, NULL) == 0U);
    CHECK(ed.groups.v.len == 0U);
    CHECK(ed.msg.active && ed.msg.sev == YEW_MSG_INFO);
    CHECK(strstr(ed.msg.text, "no files in") != NULL);
    yew_ed_free(&ed);
    CHECK(rmdir(dir) == 0);
}

static bool run_git(const char *repo, char *const *tail)
{
    char *argv[16];
    size_t n = 0U;
    pid_t pid;
    int status;

    argv[n++] = (char *)"git";
    while (*tail != NULL && n + 1U < YEW_ARRAY_LEN(argv))
        argv[n++] = *tail++;
    if (*tail != NULL)
        return false;
    argv[n] = NULL;
    pid = fork();
    if (pid < 0)
        return false;
    if (pid == 0) {
        int devnull = open("/dev/null", O_WRONLY);

        if (chdir(repo) != 0 ||
            (devnull >= 0 && dup2(devnull, STDOUT_FILENO) < 0) ||
            (devnull >= 0 && dup2(devnull, STDERR_FILENO) < 0))
            _exit(126);
        if (devnull >= 0)
            (void)close(devnull);
        execvp(argv[0], argv);
        _exit(errno == ENOENT ? 127 : 126);
    }
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR)
            return false;
    }
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

static bool run_jobs_idle(Ed *ed)
{
    i64 start = yew_now_ms();

    while (ed->jobs.len != 0U) {
        struct pollfd pfd[YEW_JOB_MAX * 4U];
        u32 n = 0U;

        yew_job_collect_fds(ed, pfd, &n);
        if (n != 0U)
            (void)poll(pfd, (nfds_t)n, 20);
        yew_job_pump(ed, pfd, n);
        yew_job_reap(ed);
        yew_job_tick(ed, yew_now_ms());
        yew_job_settle(ed);
        yew_fuss_tick(ed, yew_now_ms());
        if (yew_now_ms() - start > 10000)
            return false;
    }
    yew_fuss_tick(ed, yew_now_ms());
    return true;
}

static void test_f_mode_g_opens_selected_directory_group(const char *parent)
{
    char repo[4096];
    char selected[4096];
    char nested[4096];
    char base[4096];
    char *init[] = {(char *)"init", (char *)"-q", (char *)"-b",
                    (char *)"trunk", NULL};
    CmdCtx cx = {0};
    char *picked;
    Ed ed;

    CHECK(path_make(repo, sizeof(repo), parent, "f-mode-repo"));
    CHECK(mkdir(repo, 0700) == 0);
    CHECK(run_git(repo, init));
    CHECK(path_make(base, sizeof(base), repo, "base.txt"));
    CHECK(file_write(base, "base\n"));
    CHECK(path_make(selected, sizeof(selected), repo, "aaa"));
    CHECK(mkdir(selected, 0700) == 0);
    CHECK(path_make(nested, sizeof(nested), selected, "nested.txt"));
    CHECK(file_write(nested, "nested\n"));

    ed_init(&ed);
    ed.ws.dir = arena_strdup(&ed.arena, repo);
    CHECK(yew_mode_enter(&ed, YEW_MODE_F) == YEW_CMD_OK);
    CHECK(run_jobs_idle(&ed));
    cx.ed = &ed;
    cx.win = ed.win;
    cx.count = 1U;
    cx.source = YEW_SRC_TEST;
    picked = yew_fuss_selected_directory(&cx);
    CHECK(picked != NULL && strcmp(picked, selected) == 0);
    free(picked);
    yew_ed_handle_key(&ed, key_press((u32)'g'), yew_now_ms());
    CHECK(ed.last_cmd.v == yew_cmd_lookup("ed.group.from_dir", 17U).v);
    CHECK(ed.last_status == YEW_CMD_OK);
    CHECK(ed.groups.v.len == 1U);
    CHECK(yew_group_member_count(&ed, ed.groups.v.data[0].id) == 1);
    CHECK(yew_tab_find_by_path(&ed, nested) >= 0);
    yew_ed_free(&ed);
    CHECK(unlink(nested) == 0);
    CHECK(rmdir(selected) == 0);
    CHECK(unlink(base) == 0);
    /* The Make target owns and removes this exact temporary repository,
     * including Git's implementation-defined administrative files. */
}

int main(int argc, char **argv)
{
    struct stat st;

    if (argc != 2) {
        (void)fprintf(stderr, "usage: %s EMPTY_DIRECTORY\n", argv[0]);
        return 2;
    }
    if (stat(argv[1], &st) != 0 || !S_ISDIR(st.st_mode)) {
        (void)fprintf(stderr, "group_from_dir: not a directory: %s\n",
                      argv[1]);
        return 2;
    }
    test_scrambled_forty_is_deterministic(argv[1]);
    test_open_tab_is_adopted_without_duplication(argv[1]);
    test_other_group_adoption_compacts_both_groups(argv[1]);
    test_five_thousand_files_open_picker_only(argv[1]);
    test_empty_directory_reports_without_creating_group(argv[1]);
    test_f_mode_g_opens_selected_directory_group(argv[1]);
    if (failures != 0U) {
        (void)fprintf(stderr, "group_from_dir: %u/%u checks failed\n",
                      failures, assertions);
        return 1;
    }
    (void)printf("group_from_dir: %u assertions: ok\n", assertions);
    return 0;
}
