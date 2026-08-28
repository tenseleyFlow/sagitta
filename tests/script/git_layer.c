#define _POSIX_C_SOURCE 200809L

#include <dirent.h>
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
#include "edit/loop.h"
#include "mod/git/git.h"
#include "mod/git/git_int.h"

typedef struct Bytes {
    u8 *data;
    size_t len;
    size_t cap;
} Bytes;

static unsigned assertions;
static unsigned failures;

#define CHECK(expr) do {                                                   \
    assertions++;                                                          \
    if (!(expr)) {                                                         \
        (void)fprintf(stderr, "git_layer:%d: check failed: %s\n",          \
                      __LINE__, #expr);                                    \
        failures++;                                                        \
    }                                                                      \
} while (0)

static bool bytes_append(Bytes *b, const u8 *p, size_t n)
{
    size_t cap;
    u8 *grown;

    if (n == 0U)
        return true;
    if (b->len > SIZE_MAX - n - 1U)
        return false;
    if (b->len + n + 1U > b->cap) {
        cap = b->cap == 0U ? 4096U : b->cap;
        while (cap < b->len + n + 1U) {
            if (cap > SIZE_MAX / 2U)
                return false;
            cap *= 2U;
        }
        grown = realloc(b->data, cap);
        if (grown == NULL)
            return false;
        b->data = grown;
        b->cap = cap;
    }
    (void)memcpy(b->data + b->len, p, n);
    b->len += n;
    b->data[b->len] = 0U;
    return true;
}

static void bytes_drop(Bytes *b)
{
    free(b->data);
    (void)memset(b, 0, sizeof(*b));
}

static bool read_all(int fd, Bytes *out)
{
    u8 buf[16384];

    for (;;) {
        ssize_t got = read(fd, buf, sizeof(buf));

        if (got == 0)
            return true;
        if (got < 0) {
            if (errno == EINTR)
                continue;
            return false;
        }
        if (!bytes_append(out, buf, (size_t)got))
            return false;
    }
}

static bool remove_tree_fd(int dirfd)
{
    int scanfd;
    DIR *dir;
    struct dirent *entry;
    bool ok = true;

    scanfd = dup(dirfd);
    if (scanfd < 0)
        return false;
    dir = fdopendir(scanfd);
    if (dir == NULL) {
        (void)close(scanfd);
        return false;
    }
    errno = 0;
    while ((entry = readdir(dir)) != NULL) {
        struct stat st;

        if (strcmp(entry->d_name, ".") == 0 ||
            strcmp(entry->d_name, "..") == 0)
            continue;
        if (fstatat(dirfd, entry->d_name, &st, AT_SYMLINK_NOFOLLOW) != 0) {
            ok = false;
            break;
        }
        if (S_ISDIR(st.st_mode)) {
            int child = openat(dirfd, entry->d_name,
                               O_RDONLY | O_DIRECTORY | O_NOFOLLOW |
                                   O_CLOEXEC);

            if (child < 0 || !remove_tree_fd(child)) {
                if (child >= 0)
                    (void)close(child);
                ok = false;
                break;
            }
            if (close(child) != 0 ||
                unlinkat(dirfd, entry->d_name, AT_REMOVEDIR) != 0) {
                ok = false;
                break;
            }
        } else if (unlinkat(dirfd, entry->d_name, 0) != 0) {
            ok = false;
            break;
        }
        errno = 0;
    }
    if (ok && errno != 0)
        ok = false;
    if (closedir(dir) != 0)
        ok = false;
    return ok;
}

static bool remove_long_path(const char *repo)
{
    int repofd;
    int pathfd;
    bool ok;

    repofd = open(repo, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (repofd < 0)
        return false;
    pathfd = openat(repofd, "long-path",
                    O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (pathfd < 0) {
        ok = errno == ENOENT;
        (void)close(repofd);
        return ok;
    }
    ok = remove_tree_fd(pathfd);
    if (close(pathfd) != 0)
        ok = false;
    if (ok && unlinkat(repofd, "long-path", AT_REMOVEDIR) != 0)
        ok = false;
    if (close(repofd) != 0)
        ok = false;
    return ok;
}

static bool run_git(const char *repo, char *const *tail, Bytes *out)
{
    char *argv[24];
    int pipefd[2];
    pid_t pid;
    size_t n = 0U;
    int status;

    argv[n++] = (char *)"git";
    argv[n++] = (char *)"-C";
    argv[n++] = (char *)repo;
    while (*tail != NULL && n + 1U < sizeof(argv) / sizeof(argv[0]))
        argv[n++] = *tail++;
    if (*tail != NULL)
        return false;
    argv[n] = NULL;
    if (pipe(pipefd) != 0)
        return false;
    pid = fork();
    if (pid < 0) {
        (void)close(pipefd[0]);
        (void)close(pipefd[1]);
        return false;
    }
    if (pid == 0) {
        int devnull = open("/dev/null", O_WRONLY);

        (void)close(pipefd[0]);
        if (dup2(pipefd[1], STDOUT_FILENO) < 0 ||
            (devnull >= 0 && dup2(devnull, STDERR_FILENO) < 0))
            _exit(126);
        (void)close(pipefd[1]);
        if (devnull >= 0)
            (void)close(devnull);
        execvp(argv[0], argv);
        _exit(errno == ENOENT ? 127 : 126);
    }
    (void)close(pipefd[1]);
    if (!read_all(pipefd[0], out)) {
        (void)close(pipefd[0]);
        (void)waitpid(pid, &status, 0);
        return false;
    }
    if (close(pipefd[0]) != 0)
        return false;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR)
            return false;
    }
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

static bool path_eq(const char *p, u32 n, const char *want)
{
    size_t want_len = strlen(want);

    return (size_t)n == want_len && memcmp(p, want, want_len) == 0;
}

static const GitEntry *entry_find(const GitSnapshot *snap, const char *path)
{
    size_t i;

    for (i = 0U; i < snap->entries.len; i++) {
        if (path_eq(snap->entries.data[i].path,
                    snap->entries.data[i].path_len, path))
            return &snap->entries.data[i];
    }
    return NULL;
}

static void check_spawn_argv(const GitSnapshot *snap);

typedef struct SpawnCapture {
    char argv[32][256];
    size_t argc;
    bool literal_paths;
    unsigned calls;
} SpawnCapture;

static u32 capture_spawn(Ed *ed, const GitVerb *verb, char *const *argv,
                         const GitReq *req, void *opaque,
                         char *err, size_t errsz)
{
    SpawnCapture *capture = opaque;
    size_t i;

    (void)ed;
    (void)verb;
    (void)err;
    (void)errsz;
    capture->calls++;
    capture->literal_paths = req != NULL && req->literal_paths;
    for (i = 0U; argv[i] != NULL && i < 32U; i++) {
        size_t len = strlen(argv[i]);

        if (len >= sizeof(capture->argv[i]))
            len = sizeof(capture->argv[i]) - 1U;
        (void)memcpy(capture->argv[i], argv[i], len);
        capture->argv[i][len] = '\0';
    }
    capture->argc = i;
    return 700U;
}

static size_t argv_index(const SpawnCapture *capture, const char *value)
{
    size_t i;

    for (i = 0U; i < capture->argc; i++) {
        if (strcmp(capture->argv[i], value) == 0)
            return i;
    }
    return SIZE_MAX;
}

static void check_status_fixture(const char *repo)
{
    static char *const tail[] = {
        (char *)"-c", (char *)"core.quotepath=false",
        (char *)"-c", (char *)"status.renames=true",
        (char *)"status", (char *)"--porcelain=v2", (char *)"--branch",
        (char *)"-z", (char *)"--untracked-files=normal",
        (char *)"--ignored=no", NULL
    };
    static const char *const paths[] = {
        "copy-candidate.txt", "modified.txt", "rename-new.txt",
        "staged-and-modified.txt", "staged.txt", "typechange.txt",
        "conflict.txt", "-n", "line\nbreak.txt", "quote\"$dollar.txt",
        "space name.txt", "untracked-dir/", "untracked.txt", "漢字.txt"
    };
    GitSnapshot snap;
    GitParseErr err = {0};
    Bytes out = {0};
    size_t i;
    const GitEntry *e;

    CHECK(run_git(repo, tail, &out));
    yew_git_snapshot_init(&snap);
    CHECK(yew_git_parse_status(&snap, out.data, (u64)out.len, &err));
    CHECK(snap.entries.len == sizeof(paths) / sizeof(paths[0]) ||
          snap.entries.len == sizeof(paths) / sizeof(paths[0]) + 1U);
    for (i = 0U; i < sizeof(paths) / sizeof(paths[0]); i++)
        CHECK(entry_find(&snap, paths[i]) != NULL);
    if (snap.entries.len == sizeof(paths) / sizeof(paths[0]) + 1U)
        CHECK(entry_find(&snap, "long-path/") != NULL);
    e = entry_find(&snap, "rename-new.txt");
    CHECK(e != NULL && e->kind == GIT_E_RENAME && e->score == 100U);
    CHECK(e != NULL && path_eq(e->orig_path, e->orig_len,
                               "rename-old.txt"));
    e = entry_find(&snap, "conflict.txt");
    CHECK(e != NULL && e->kind == GIT_E_UNMERGED && e->conflicted);
    e = entry_find(&snap, "untracked-dir/");
    CHECK(e != NULL && e->kind == GIT_E_UNTRACKED && e->is_dir);
    CHECK(snap.conflicted && snap.state == YEW_GIT_CONFLICTED);
    check_spawn_argv(&snap);
    yew_git_snapshot_drop(&snap);
    bytes_drop(&out);
}

static void check_incoming(const char *repo)
{
    char upstream[4096];
    static char *const status_tail[] = {
        (char *)"status", (char *)"--porcelain=v2", (char *)"--branch",
        (char *)"-z", NULL
    };
    static char *const diff_tail[] = {
        (char *)"diff", (char *)"--name-only", (char *)"-z",
        (char *)"HEAD...@{upstream}", NULL
    };
    GitSnapshot snap;
    GitPathList incoming = {0};
    GitParseErr err = {0};
    Arena arena;
    Bytes out = {0};

    CHECK(snprintf(upstream, sizeof(upstream),
                   "%s/.fixture-variants/upstream", repo) > 0);
    yew_git_snapshot_init(&snap);
    CHECK(run_git(repo, status_tail, &out));
    CHECK(yew_git_parse_status(&snap, out.data, (u64)out.len, &err));
    CHECK(snap.upstream == NULL && snap.state == YEW_GIT_CONFLICTED);
    yew_git_snapshot_drop(&snap);
    bytes_drop(&out);

    yew_git_snapshot_init(&snap);
    CHECK(run_git(upstream, status_tail, &out));
    CHECK(yew_git_parse_status(&snap, out.data, (u64)out.len, &err));
    CHECK(snap.upstream != NULL && strcmp(snap.upstream, "origin/trunk") == 0);
    bytes_drop(&out);
    CHECK(run_git(upstream, diff_tail, &out));
    arena_init(&arena);
    CHECK(yew_git_parse_z_paths(&arena, out.data, (u64)out.len,
                                &incoming, &err));
    CHECK(incoming.len == 1U);
    CHECK(incoming.len == 1U && path_eq(incoming.data[0].path,
                                        incoming.data[0].len, "remote.txt"));
    arena_free_all(&arena);
    yew_git_snapshot_drop(&snap);
    bytes_drop(&out);
}

static void check_copied_entry(void)
{
    static const u8 record[] =
        "2 C. N... 100644 100644 100644 "
        "1111111111111111111111111111111111111111 "
        "2222222222222222222222222222222222222222 "
        "C100 copy-result.txt\0copy-source.txt\0";
    GitSnapshot snap;
    GitParseErr err = {0};
    const GitEntry *entry;
    SpawnCapture capture = {0};
    GitReq req = {0};
    char *tail[5];
    const GitVerb *verb;
    Ed ed;
    char spawn_err[128] = {0};
    size_t dash;

    yew_git_snapshot_init(&snap);
    CHECK(yew_git_parse_status(&snap, record, sizeof(record) - 1U, &err));
    CHECK(snap.entries.len == 1U);
    entry = entry_find(&snap, "copy-result.txt");
    CHECK(entry != NULL && entry->kind == GIT_E_RENAME && entry->x == 'C');
    CHECK(entry != NULL && entry->score == 100U &&
          path_eq(entry->orig_path, entry->orig_len, "copy-source.txt"));
    tail[0] = (char *)"add";
    tail[1] = (char *)"--";
    tail[2] = entry == NULL ? (char *)"copy-result.txt" : entry->path;
    tail[3] = entry == NULL ? (char *)"copy-source.txt" : entry->orig_path;
    tail[4] = NULL;
    yew_ed_init(&ed);
    arena_init(&req.arena);
    req.kind = YEW_GREQ_VERB;
    req.literal_paths = true;
    verb = yew_git_verb("stage");
    yew_git_test_spawn_set(capture_spawn, &capture);
    CHECK(verb != NULL);
    if (verb != NULL)
        CHECK(yew_git_spawn(&ed, verb, tail, &req, spawn_err,
                            sizeof(spawn_err)) == 700U);
    yew_git_test_spawn_set(NULL, NULL);
    dash = argv_index(&capture, "--");
    CHECK(dash != SIZE_MAX);
    CHECK(argv_index(&capture, tail[2]) > dash);
    CHECK(argv_index(&capture, tail[3]) > dash);
    arena_free_all(&req.arena);
    yew_ed_free(&ed);
    yew_git_snapshot_drop(&snap);
}

static bool touch_path(const char *path)
{
    int fd = open(path, O_WRONLY | O_CREAT, 0600);

    return fd >= 0 && close(fd) == 0;
}

static void check_spawn_argv(const GitSnapshot *snap)
{
    static const char *const awkward[] = {
        "space name.txt", "quote\"$dollar.txt", "line\nbreak.txt", "-n",
        "漢字.txt"
    };
    char *tail[sizeof(awkward) / sizeof(awkward[0]) + 3U];
    SpawnCapture capture = {0};
    GitReq req = {0};
    const GitVerb *verb = yew_git_verb("stage");
    Ed ed;
    char err[128] = {0};
    size_t dash;
    size_t i;

    tail[0] = (char *)"add";
    tail[1] = (char *)"--";
    for (i = 0U; i < sizeof(awkward) / sizeof(awkward[0]); i++) {
        const GitEntry *entry = entry_find(snap, awkward[i]);

        CHECK(entry != NULL);
        tail[i + 2U] = entry == NULL ? (char *)awkward[i] : entry->path;
    }
    tail[sizeof(awkward) / sizeof(awkward[0]) + 2U] = NULL;
    yew_ed_init(&ed);
    arena_init(&req.arena);
    req.kind = YEW_GREQ_VERB;
    req.literal_paths = true;
    yew_git_test_spawn_set(capture_spawn, &capture);
    CHECK(verb != NULL);
    if (verb != NULL)
        CHECK(yew_git_spawn(&ed, verb, tail, &req, err, sizeof(err)) == 700U);
    yew_git_test_spawn_set(NULL, NULL);
    CHECK(capture.calls == 1U && capture.literal_paths);
    CHECK(capture.argc >= 13U);
    CHECK(capture.argc != 0U && strcmp(capture.argv[0], "git") == 0);
    CHECK(argv_index(&capture, "--no-pager") == 1U);
    CHECK(argv_index(&capture, "core.quotepath=false") != SIZE_MAX);
    CHECK(argv_index(&capture, "status.renames=true") != SIZE_MAX);
    CHECK(argv_index(&capture, "--no-optional-locks") == SIZE_MAX);
    dash = argv_index(&capture, "--");
    CHECK(dash != SIZE_MAX);
    for (i = 0U; i < sizeof(awkward) / sizeof(awkward[0]); i++) {
        const GitEntry *entry = entry_find(snap, awkward[i]);
        size_t at = argv_index(&capture, tail[i + 2U]);

        CHECK(at != SIZE_MAX && at > dash);
        CHECK(entry != NULL && at != SIZE_MAX &&
              strlen(capture.argv[at]) == (size_t)entry->path_len &&
              memcmp(capture.argv[at], entry->path, entry->path_len) == 0);
    }
    CHECK(strchr(tail[3], '"') != NULL);
    CHECK(strchr(tail[3], '$') != NULL);
    arena_free_all(&req.arena);
    yew_ed_free(&ed);
}

static bool run_job_until_released(Ed *ed, u32 id)
{
    i64 start = yew_now_ms();

    while (yew_job_find(ed, id) != NULL) {
        struct pollfd pfd[YEW_JOB_MAX * 4U];
        u32 n = 0U;

        yew_job_collect_fds(ed, pfd, &n);
        if (n != 0U)
            (void)poll(pfd, (nfds_t)n, 20);
        else
            (void)poll(NULL, 0U, 5);
        yew_job_pump(ed, pfd, n);
        yew_job_reap(ed);
        yew_job_tick(ed, yew_now_ms());
        yew_job_settle(ed);
        if (yew_now_ms() - start > 10000)
            return false;
    }
    return true;
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
        else
            (void)poll(NULL, 0U, 5);
        yew_job_pump(ed, pfd, n);
        yew_job_reap(ed);
        yew_job_tick(ed, yew_now_ms());
        yew_job_settle(ed);
        if (yew_now_ms() - start > 10000)
            return false;
    }
    return true;
}

static CmdStatus invoke_git_info(Ed *ed)
{
    CmdCtx cx = {0};
    CmdId id = yew_cmd_lookup("ed.git.info", 11U);

    CHECK(id.v != 0U);
    cx.ed = ed;
    cx.count = 1U;
    cx.source = YEW_SRC_TEST;
    return id.v == 0U ? YEW_CMD_ERR_ARG : yew_ed_invoke(ed, id, &cx);
}

static void check_info_state(const char *worktree, const char *relative,
                             bool directory, GitStatusCode want,
                             bool already_exists)
{
    char git_dir[4096];
    char probe[4096];
    char expected[512];
    Ed ed;
    const GitSnapshot *snap;

    CHECK(snprintf(git_dir, sizeof(git_dir), "%s/.git", worktree) > 0);
    CHECK(snprintf(probe, sizeof(probe), "%s/%s", git_dir, relative) > 0);
    if (!already_exists) {
        if (directory)
            CHECK(mkdir(probe, 0700) == 0);
        else
            CHECK(touch_path(probe));
    } else {
        CHECK(access(probe, F_OK) == 0);
    }
    yew_ed_init(&ed);
    ed.ws.dir = arena_strdup(&ed.arena, worktree);
    yew_git_test_now_set(&ed, 1000);
    CHECK(yew_git_refresh(&ed, true));
    CHECK(run_jobs_idle(&ed));
    snap = yew_git_snapshot(&ed);
    CHECK(snap != NULL && snap->gen == 1U && snap->state == want);
    CHECK(invoke_git_info(&ed) == YEW_CMD_OK);
    CHECK(snprintf(expected, sizeof(expected),
                   "git: %s; repo %s; branch trunk; age 0 ms",
                   yew_git_state_str(want), worktree) > 0);
    CHECK(ed.msg.active);
    CHECK(strcmp(ed.msg.text, expected) == 0);
    yew_ed_free(&ed);
    if (!already_exists) {
        if (directory)
            CHECK(rmdir(probe) == 0);
        else
            CHECK(unlink(probe) == 0);
    }
}

static void check_info_mid_states(const char *repo)
{
    char clean[4096];

    CHECK(snprintf(clean, sizeof(clean), "%s/.fixture-variants/upstream",
                   repo) > 0);
    check_info_state(repo, "MERGE_HEAD", false, YEW_GIT_MID_MERGE, true);
    check_info_state(clean, "rebase-merge", true, YEW_GIT_MID_REBASE, false);
    check_info_state(clean, "CHERRY_PICK_HEAD", false,
                     YEW_GIT_MID_CHERRY_PICK, false);
    check_info_state(clean, "REVERT_HEAD", false,
                     YEW_GIT_MID_REVERT, false);
    check_info_state(clean, "BISECT_LOG", false,
                     YEW_GIT_MID_BISECT, false);
}

typedef struct SavedEnv {
    const char *name;
    char *value;
    bool existed;
} SavedEnv;

static bool env_save(SavedEnv *saved, const char *name)
{
    const char *value = getenv(name);

    saved->name = name;
    saved->existed = value != NULL;
    saved->value = value == NULL ? NULL : strdup(value);
    return value == NULL || saved->value != NULL;
}

static bool env_restore(SavedEnv *saved)
{
    int rc = saved->existed ? setenv(saved->name, saved->value, 1)
                            : unsetenv(saved->name);

    free(saved->value);
    saved->value = NULL;
    return rc == 0;
}

static const char *dump_value(const Bytes *dump, const char *name)
{
    size_t name_len = strlen(name);
    size_t at = 0U;

    while (at < dump->len) {
        size_t end = at;

        while (end < dump->len && dump->data[end] != (u8)'\n')
            end++;
        if (end - at > name_len &&
            memcmp(dump->data + at, name, name_len) == 0 &&
            dump->data[at + name_len] == (u8)'=')
            return (const char *)dump->data + at + name_len + 1U;
        at = end + (end < dump->len ? 1U : 0U);
    }
    return NULL;
}

static bool dump_value_eq(const Bytes *dump, const char *name,
                          const char *want)
{
    const char *value = dump_value(dump, name);
    size_t len;

    if (value == NULL)
        return false;
    len = strcspn(value, "\n");
    return len == strlen(want) && memcmp(value, want, len) == 0;
}

static void check_spawn_env(const char *repo)
{
    static const char *const names[] = {
        "PATH", "YEW_GIT_ENV_DUMP", "GIT_DIR", "GIT_WORK_TREE",
        "GIT_COMMON_DIR", "GIT_INDEX_FILE", "GIT_OBJECT_DIRECTORY",
        "GIT_ALTERNATE_OBJECT_DIRECTORIES", "GIT_NAMESPACE",
        "GIT_CEILING_DIRECTORIES", "GIT_CONFIG_GLOBAL",
        "GIT_CONFIG_SYSTEM", "GIT_CONFIG_COUNT", "GIT_CONFIG_KEY_0",
        "GIT_CONFIG_VALUE_0", "GIT_SSH", "GIT_SSH_COMMAND",
        "GIT_ASKPASS", "SSH_ASKPASS", "SSH_AUTH_SOCK",
        "SSH_ASKPASS_REQUIRE", "GIT_TERMINAL_PROMPT",
        "GIT_EDITOR", "GIT_SEQUENCE_EDITOR", "EDITOR", "VISUAL",
        "GIT_PAGER", "PAGER", "GIT_FLUSH", "GIT_TRACE",
        "GIT_TRACE2_EVENT", "GIT_TRACE_PACKET", "GIT_TRACE_PERFORMANCE",
        "GIT_CURL_VERBOSE", "GIT_TRANSFER_TRACE", "LC_ALL",
        "GIT_OPTIONAL_LOCKS",
        "GIT_LITERAL_PATHSPECS"
    };
    static const struct {
        const char *name;
        const char *value;
    } sentinels[] = {
        {"GIT_DIR", "parent-git-dir"},
        {"GIT_WORK_TREE", "parent-work-tree"},
        {"GIT_COMMON_DIR", "parent-common-dir"},
        {"GIT_INDEX_FILE", "parent-index"},
        {"GIT_OBJECT_DIRECTORY", "parent-objects"},
        {"GIT_ALTERNATE_OBJECT_DIRECTORIES", "parent-alt-objects"},
        {"GIT_NAMESPACE", "parent-namespace"},
        {"GIT_CEILING_DIRECTORIES", "parent-ceiling"},
        {"GIT_CONFIG_GLOBAL", "parent-global-config"},
        {"GIT_CONFIG_SYSTEM", "parent-system-config"},
        {"GIT_CONFIG_COUNT", "1"},
        {"GIT_CONFIG_KEY_0", "user.name"},
        {"GIT_CONFIG_VALUE_0", "Yew Test"},
        {"GIT_SSH", "parent-git-ssh"},
        {"GIT_SSH_COMMAND", "ssh -F sentinel"},
        {"GIT_ASKPASS", "parent-git-askpass"},
        {"SSH_ASKPASS", "parent-ssh-askpass"},
        {"SSH_AUTH_SOCK", "/sentinel/agent.sock"},
        {"SSH_ASKPASS_REQUIRE", "force"},
        {"EDITOR", "parent-editor"},
        {"VISUAL", "parent-visual"},
        {"GIT_OPTIONAL_LOCKS", "parent-lock-policy"},
        {"GIT_TRACE", "parent-trace"},
        {"GIT_TRACE2_EVENT", "parent-trace2"},
        {"GIT_TRACE_PACKET", "parent-packet"},
        {"GIT_TRACE_PERFORMANCE", "parent-performance"},
        {"GIT_CURL_VERBOSE", "parent-curl"},
        {"GIT_TRANSFER_TRACE", "parent-transfer"}
    };
    static char *const tail[] = {(char *)"init", NULL};
    SavedEnv saved[sizeof(names) / sizeof(names[0])];
    char fake_dir[4096];
    char fake_git[4096];
    char dump_path[4096];
    const char script[] =
        "#!/bin/sh\n/usr/bin/env >\"$YEW_GIT_ENV_DUMP\"\nexit 0\n";
    GitReq req = {0};
    const GitVerb *verb;
    Ed ed;
    Bytes dump = {0};
    int fd;
    u32 id = 0U;
    size_t i;
    char err[128] = {0};

    for (i = 0U; i < sizeof(saved) / sizeof(saved[0]); i++)
        CHECK(env_save(&saved[i], names[i]));
    CHECK(snprintf(fake_dir, sizeof(fake_dir), "%s/.fake-git-bin", repo) > 0);
    CHECK(mkdir(fake_dir, 0700) == 0);
    CHECK(snprintf(fake_git, sizeof(fake_git), "%s/git", fake_dir) > 0);
    CHECK(snprintf(dump_path, sizeof(dump_path), "%s/env.dump", fake_dir) > 0);
    fd = open(fake_git, O_WRONLY | O_CREAT | O_EXCL, 0700);
    CHECK(fd >= 0);
    if (fd >= 0) {
        CHECK(write(fd, script, sizeof(script) - 1U) ==
              (ssize_t)(sizeof(script) - 1U));
        CHECK(close(fd) == 0);
        CHECK(chmod(fake_git, 0700) == 0);
    }
    CHECK(setenv("PATH", fake_dir, 1) == 0);
    CHECK(setenv("YEW_GIT_ENV_DUMP", dump_path, 1) == 0);
    for (i = 0U; i < sizeof(sentinels) / sizeof(sentinels[0]); i++)
        CHECK(setenv(sentinels[i].name, sentinels[i].value, 1) == 0);

    yew_ed_init(&ed);
    ed.ws.dir = arena_strdup(&ed.arena, repo);
    arena_init(&req.arena);
    req.kind = YEW_GREQ_VERB;
    req.literal_paths = true;
    verb = yew_git_verb("init");
    CHECK(verb != NULL);
    if (verb != NULL)
        id = yew_git_spawn(&ed, verb, tail, &req, err, sizeof(err));
    CHECK(id != 0U);
    if (id != 0U)
        CHECK(run_job_until_released(&ed, id));
    for (i = 0U; i < sizeof(sentinels) / sizeof(sentinels[0]); i++)
        CHECK(getenv(sentinels[i].name) != NULL &&
              strcmp(getenv(sentinels[i].name), sentinels[i].value) == 0);
    for (i = 0U; i < sizeof(saved) / sizeof(saved[0]); i++) {
        size_t j;
        bool intentionally_changed = strcmp(saved[i].name, "PATH") == 0 ||
            strcmp(saved[i].name, "YEW_GIT_ENV_DUMP") == 0;

        for (j = 0U; j < sizeof(sentinels) / sizeof(sentinels[0]); j++) {
            if (strcmp(saved[i].name, sentinels[j].name) == 0) {
                intentionally_changed = true;
                break;
            }
        }
        if (!intentionally_changed) {
            const char *current = getenv(saved[i].name);

            CHECK((current != NULL) == saved[i].existed);
            if (saved[i].existed)
                CHECK(current != NULL && strcmp(current, saved[i].value) == 0);
        }
    }
    arena_free_all(&req.arena);
    yew_ed_free(&ed);
    fd = open(dump_path, O_RDONLY);
    CHECK(fd >= 0);
    if (fd >= 0) {
        CHECK(read_all(fd, &dump));
        CHECK(close(fd) == 0);
    }
    CHECK(dump_value_eq(&dump, "GIT_DIR", "parent-git-dir"));
    CHECK(dump_value_eq(&dump, "GIT_WORK_TREE", "parent-work-tree"));
    CHECK(dump_value_eq(&dump, "GIT_COMMON_DIR", "parent-common-dir"));
    CHECK(dump_value_eq(&dump, "GIT_INDEX_FILE", "parent-index"));
    CHECK(dump_value_eq(&dump, "GIT_OBJECT_DIRECTORY", "parent-objects"));
    CHECK(dump_value_eq(&dump, "GIT_ALTERNATE_OBJECT_DIRECTORIES",
                        "parent-alt-objects"));
    CHECK(dump_value_eq(&dump, "GIT_NAMESPACE", "parent-namespace"));
    CHECK(dump_value_eq(&dump, "GIT_CEILING_DIRECTORIES", "parent-ceiling"));
    CHECK(dump_value_eq(&dump, "GIT_CONFIG_GLOBAL", "parent-global-config"));
    CHECK(dump_value_eq(&dump, "GIT_CONFIG_SYSTEM", "parent-system-config"));
    CHECK(dump_value_eq(&dump, "GIT_CONFIG_COUNT", "1"));
    CHECK(dump_value_eq(&dump, "GIT_CONFIG_KEY_0", "user.name"));
    CHECK(dump_value_eq(&dump, "GIT_CONFIG_VALUE_0", "Yew Test"));
    CHECK(dump_value_eq(&dump, "GIT_SSH", "parent-git-ssh"));
    CHECK(dump_value_eq(&dump, "GIT_SSH_COMMAND", "ssh -F sentinel"));
    CHECK(dump_value_eq(&dump, "GIT_ASKPASS", "parent-git-askpass"));
    CHECK(dump_value_eq(&dump, "SSH_ASKPASS", "parent-ssh-askpass"));
    CHECK(dump_value_eq(&dump, "SSH_AUTH_SOCK", "/sentinel/agent.sock"));
    CHECK(dump_value_eq(&dump, "SSH_ASKPASS_REQUIRE", "force"));
    CHECK(dump_value_eq(&dump, "EDITOR", "parent-editor"));
    CHECK(dump_value_eq(&dump, "VISUAL", "parent-visual"));
    CHECK(dump_value_eq(&dump, "GIT_OPTIONAL_LOCKS", "parent-lock-policy"));
    CHECK(dump_value_eq(&dump, "GIT_TERMINAL_PROMPT", "0"));
    CHECK(dump_value_eq(&dump, "GIT_EDITOR", "false"));
    CHECK(dump_value_eq(&dump, "GIT_SEQUENCE_EDITOR", "false"));
    CHECK(dump_value_eq(&dump, "GIT_PAGER", "cat"));
    CHECK(dump_value_eq(&dump, "PAGER", "cat"));
    CHECK(dump_value_eq(&dump, "GIT_FLUSH", "1"));
    CHECK(dump_value_eq(&dump, "LC_ALL", "C"));
    CHECK(dump_value_eq(&dump, "GIT_LITERAL_PATHSPECS", "1"));
    CHECK(dump_value(&dump, "GIT_TRACE") == NULL);
    CHECK(dump_value(&dump, "GIT_TRACE2_EVENT") == NULL);
    CHECK(dump_value(&dump, "GIT_TRACE_PACKET") == NULL);
    CHECK(dump_value(&dump, "GIT_TRACE_PERFORMANCE") == NULL);
    CHECK(dump_value(&dump, "GIT_CURL_VERBOSE") == NULL);
    CHECK(dump_value(&dump, "GIT_TRANSFER_TRACE") == NULL);
    bytes_drop(&dump);
    for (i = 0U; i < sizeof(saved) / sizeof(saved[0]); i++)
        CHECK(env_restore(&saved[i]));
}

static void check_ignore_compaction(const char *repo)
{
    char dir[4096];
    char exclude[4096];
    char file[4096];
    static char *const tail[] = {
        (char *)"ls-files", (char *)"-z", (char *)"--others",
        (char *)"--ignored", (char *)"--exclude-standard",
        (char *)"--directory", (char *)"--no-empty-directory", NULL
    };
    const char rule[] = "/node_modules/\n";
    int fd;
    unsigned i;
    Bytes out = {0};
    Arena arena;
    GitIgnoreSet set = {0};
    GitParseErr err = {0};

    CHECK(snprintf(dir, sizeof(dir), "%s/node_modules", repo) > 0);
    CHECK(mkdir(dir, 0700) == 0);
    CHECK(snprintf(exclude, sizeof(exclude), "%s/.git/info/exclude", repo) > 0);
    fd = open(exclude, O_WRONLY | O_APPEND);
    CHECK(fd >= 0);
    if (fd >= 0) {
        CHECK(write(fd, rule, sizeof(rule) - 1U) ==
              (ssize_t)(sizeof(rule) - 1U));
        CHECK(close(fd) == 0);
    }
    for (i = 0U; i < 5000U; i++) {
        int n = snprintf(file, sizeof(file), "%s/f%04u", dir, i);

        CHECK(n > 0 && (size_t)n < sizeof(file));
        fd = open(file, O_WRONLY | O_CREAT | O_EXCL, 0600);
        CHECK(fd >= 0);
        if (fd >= 0)
            CHECK(close(fd) == 0);
    }
    CHECK(run_git(repo, tail, &out));
    arena_init(&arena);
    CHECK(yew_git_parse_ignore(&arena, out.data, (u64)out.len, &set, &err));
    CHECK(set.len == 4U);
    CHECK(yew_git_ignored(&set, "node_modules/f4321", 18U));
    CHECK(yew_git_ignored(&set, "ignored-dir/deep/file", 21U));
    CHECK(!yew_git_ignored(&set, "node_module/f4321", 17U));
    arena_free_all(&arena);
    bytes_drop(&out);
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        (void)fprintf(stderr, "usage: %s FIXTURE_REPO\n", argv[0]);
        return 2;
    }
    check_status_fixture(argv[1]);
    check_incoming(argv[1]);
    check_copied_entry();
    check_info_mid_states(argv[1]);
    check_ignore_compaction(argv[1]);
    check_spawn_env(argv[1]);
    CHECK(remove_long_path(argv[1]));
    if (failures != 0U) {
        (void)fprintf(stderr, "git_layer: %u/%u checks failed\n",
                      failures, assertions);
        return 1;
    }
    (void)printf("git_layer: %u assertions: ok\n", assertions);
    return 0;
}
