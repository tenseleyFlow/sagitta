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
#include "edit/loop.h"
#include "edit/mode.h"
#include "mod/git/fussmode.h"
#include "mod/git/git_int.h"

typedef struct Bytes {
    u8 *data;
    size_t len;
    size_t cap;
} Bytes;

typedef struct SpawnCapture {
    const char *repo;
    char *argv[32];
    size_t argc;
    u8 stdin_bytes[1024];
    size_t stdin_len;
    bool literal_paths;
    unsigned calls;
    int exit_code;
} SpawnCapture;

static unsigned assertions;
static unsigned failures;
static u32 next_job_id = 8000U;

#define CHECK(expr) do {                                                   \
    assertions++;                                                          \
    if (!(expr)) {                                                         \
        (void)fprintf(stderr, "fuss_commands:%d: check failed: %s\n",     \
                      __LINE__, #expr);                                    \
        failures++;                                                        \
    }                                                                      \
} while (0)

static bool bytes_append(Bytes *out, const u8 *data, size_t len)
{
    size_t cap;
    u8 *grown;

    if (len == 0U)
        return true;
    if (out->len > SIZE_MAX - len - 1U)
        return false;
    if (out->len + len + 1U > out->cap) {
        cap = out->cap == 0U ? 256U : out->cap;
        while (cap < out->len + len + 1U)
            cap *= 2U;
        grown = realloc(out->data, cap);
        if (grown == NULL)
            return false;
        out->data = grown;
        out->cap = cap;
    }
    (void)memcpy(out->data + out->len, data, len);
    out->len += len;
    out->data[out->len] = 0U;
    return true;
}

static void bytes_drop(Bytes *out)
{
    free(out->data);
    (void)memset(out, 0, sizeof(*out));
}

static bool read_all(int fd, Bytes *out)
{
    u8 buf[4096];

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

static bool write_all(int fd, const u8 *data, size_t len)
{
    while (len != 0U) {
        ssize_t wrote = write(fd, data, len);

        if (wrote < 0) {
            if (errno == EINTR)
                continue;
            return false;
        }
        data += (size_t)wrote;
        len -= (size_t)wrote;
    }
    return true;
}

static bool run_process(const char *repo, char *const *argv,
                        const u8 *input, size_t input_len, Bytes *out,
                        int *exit_code, bool literal_paths)
{
    int inpipe[2];
    int outpipe[2];
    pid_t pid;
    int status;
    bool ok;

    if (pipe(inpipe) != 0 || pipe(outpipe) != 0)
        return false;
    pid = fork();
    if (pid < 0)
        return false;
    if (pid == 0) {
        (void)close(inpipe[1]);
        (void)close(outpipe[0]);
        if (chdir(repo) != 0 || dup2(inpipe[0], STDIN_FILENO) < 0 ||
            dup2(outpipe[1], STDOUT_FILENO) < 0 ||
            dup2(outpipe[1], STDERR_FILENO) < 0)
            _exit(126);
        (void)close(inpipe[0]);
        (void)close(outpipe[1]);
        if (literal_paths && setenv("GIT_LITERAL_PATHSPECS", "1", 1) != 0)
            _exit(126);
        execvp(argv[0], argv);
        _exit(127);
    }
    (void)close(inpipe[0]);
    (void)close(outpipe[1]);
    ok = write_all(inpipe[1], input, input_len);
    ok = close(inpipe[1]) == 0 && ok;
    if (out != NULL)
        ok = read_all(outpipe[0], out) && ok;
    else {
        Bytes ignored = {0};

        ok = read_all(outpipe[0], &ignored) && ok;
        bytes_drop(&ignored);
    }
    ok = close(outpipe[0]) == 0 && ok;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR)
            return false;
    }
    if (WIFEXITED(status))
        *exit_code = WEXITSTATUS(status);
    else
        *exit_code = 128;
    return ok;
}

static bool run_git(const char *repo, char *const *tail, Bytes *out,
                    int *exit_code)
{
    char *argv[24];
    size_t n = 0U;

    argv[n++] = (char *)"git";
    while (*tail != NULL && n + 1U < YEW_ARRAY_LEN(argv))
        argv[n++] = *tail++;
    if (*tail != NULL)
        return false;
    argv[n] = NULL;
    return run_process(repo, argv, NULL, 0U, out, exit_code, false);
}

static void capture_drop(SpawnCapture *capture)
{
    size_t i;

    for (i = 0U; i < capture->argc; i++)
        free(capture->argv[i]);
    capture->argc = 0U;
}

static u32 capture_and_run(Ed *ed, const GitVerb *verb, char *const *argv,
                           const GitReq *req, void *opaque, char *err,
                           size_t errsz)
{
    SpawnCapture *capture = opaque;
    size_t i;

    (void)ed;
    (void)verb;
    capture->calls++;
    capture->literal_paths = req->literal_paths;
    for (i = 0U; argv[i] != NULL && i < YEW_ARRAY_LEN(capture->argv); i++) {
        capture->argv[i] = strdup(argv[i]);
        if (capture->argv[i] == NULL) {
            if (errsz != 0U)
                (void)snprintf(err, errsz, "out of memory");
            return 0U;
        }
    }
    capture->argc = i;
    if (req->stdin_len > sizeof(capture->stdin_bytes)) {
        if (errsz != 0U)
            (void)snprintf(err, errsz, "stdin capture too large");
        return 0U;
    }
    if (req->stdin_len != 0U)
        (void)memcpy(capture->stdin_bytes, req->stdin_bytes,
                     (size_t)req->stdin_len);
    capture->stdin_len = (size_t)req->stdin_len;
    if (!run_process(capture->repo, argv, req->stdin_bytes,
                     (size_t)req->stdin_len, NULL, &capture->exit_code,
                     req->literal_paths)) {
        if (errsz != 0U)
            (void)snprintf(err, errsz, "cannot execute git");
        return 0U;
    }
    return next_job_id++;
}

static CmdStatus invoke(Ed *ed, const char *name, const char *arg,
                        u32 arg_len)
{
    CmdId id = yew_cmd_lookup(name, (u32)strlen(name));
    CmdCtx cx = {0};

    CHECK(id.v != 0U);
    cx.ed = ed;
    cx.win = ed->win;
    cx.count = 1U;
    cx.sarg = arg;
    cx.sarg_len = arg_len;
    cx.source = YEW_SRC_TEST;
    return id.v == 0U ? YEW_CMD_ERR_ARG : yew_ed_invoke(ed, id, &cx);
}

static CmdStatus invoke_real(const char *repo, const char *name,
                             const char *arg, u32 arg_len,
                             SpawnCapture *capture)
{
    Ed ed;
    CmdStatus status;

    yew_ed_init(&ed);
    CHECK(yew_ed_open_scratch(&ed));
    ed.ws.dir = arena_strdup(&ed.arena, repo);
    capture->repo = repo;
    yew_git_test_spawn_set(capture_and_run, capture);
    status = invoke(&ed, name, arg, arg_len);
    yew_git_test_spawn_set(NULL, NULL);
    yew_ed_free(&ed);
    return status;
}

static bool write_repo_file(const char *repo, const char *path,
                            const char *contents)
{
    char full[4096];
    int fd;
    size_t len = strlen(contents);
    bool ok;

    if (snprintf(full, sizeof(full), "%s/%s", repo, path) <= 0)
        return false;
    fd = open(full, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0)
        return false;
    ok = write_all(fd, (const u8 *)contents, len);
    return close(fd) == 0 && ok;
}

static bool output_is_path(const Bytes *out, const char *path)
{
    size_t len = strlen(path);

    return out->len == len + 1U && memcmp(out->data, path, len) == 0 &&
           out->data[len] == 0U;
}

static bool bytes_contains(const Bytes *haystack, const u8 *needle,
                           size_t needle_len)
{
    size_t i;

    if (needle_len == 0U)
        return true;
    if (haystack->len < needle_len)
        return false;
    for (i = 0U; i <= haystack->len - needle_len; i++) {
        if (memcmp(haystack->data + i, needle, needle_len) == 0)
            return true;
    }
    return false;
}

static size_t argv_index(const SpawnCapture *capture, const char *arg)
{
    size_t i;

    for (i = 0U; i < capture->argc; i++) {
        if (strcmp(capture->argv[i], arg) == 0)
            return i;
    }
    return SIZE_MAX;
}

static bool cached_path_is(const char *repo, const char *path, bool staged)
{
    char *tail[] = {
        (char *)"diff", (char *)"--cached", (char *)"--name-only",
        (char *)"-z", (char *)"--", (char *)path, NULL
    };
    Bytes out = {0};
    int exit_code = -1;
    bool ok = run_git(repo, tail, &out, &exit_code) && exit_code == 0 &&
              (staged ? output_is_path(&out, path) : out.len == 0U);

    bytes_drop(&out);
    return ok;
}

static bool setup_repo(const char *repo)
{
    char *init[] = {(char *)"init", (char *)"-q", (char *)"-b",
                    (char *)"trunk", NULL};
    char *name[] = {(char *)"config", (char *)"user.name",
                    (char *)"Yew Test", NULL};
    char *email[] = {(char *)"config", (char *)"user.email",
                     (char *)"yew@example.invalid", NULL};
    char *add[] = {(char *)"add", (char *)"--", (char *)"base.txt", NULL};
    char *commit[] = {(char *)"commit", (char *)"-q", (char *)"-m",
                      (char *)"base", NULL};
    int code;

    return run_git(repo, init, NULL, &code) && code == 0 &&
           run_git(repo, name, NULL, &code) && code == 0 &&
           run_git(repo, email, NULL, &code) && code == 0 &&
           write_repo_file(repo, "base.txt", "base\n") &&
           run_git(repo, add, NULL, &code) && code == 0 &&
           run_git(repo, commit, NULL, &code) && code == 0;
}

static void test_stage_preserves_hostile_paths(const char *repo)
{
    static const char *const paths[] = {
        "quote\"name.txt", "literal$(id).txt", "line\nbreak.txt", "-n"
    };
    size_t i;

    for (i = 0U; i < YEW_ARRAY_LEN(paths); i++) {
        SpawnCapture capture = {0};
        size_t dash;

        CHECK(write_repo_file(repo, paths[i], "new\n"));
        CHECK(invoke_real(repo, "ed.git.stage", paths[i],
                          (u32)strlen(paths[i]), &capture) == YEW_CMD_OK);
        dash = argv_index(&capture, "--");
        CHECK(capture.calls == 1U);
        CHECK(capture.exit_code == 0);
        CHECK(capture.literal_paths);
        CHECK(dash != SIZE_MAX && argv_index(&capture, paths[i]) > dash);
        CHECK(cached_path_is(repo, paths[i], true));
        capture_drop(&capture);
    }
}

static void test_unstage_preserves_hostile_paths(const char *repo)
{
    static const char *const paths[] = {
        "quote\"name.txt", "literal$(id).txt", "line\nbreak.txt", "-n"
    };
    size_t i;

    for (i = 0U; i < YEW_ARRAY_LEN(paths); i++) {
        SpawnCapture capture = {0};
        size_t dash;

        CHECK(invoke_real(repo, "ed.git.unstage", paths[i],
                          (u32)strlen(paths[i]), &capture) == YEW_CMD_OK);
        dash = argv_index(&capture, "--");
        CHECK(capture.calls == 1U);
        CHECK(capture.exit_code == 0);
        CHECK(capture.literal_paths);
        CHECK(dash != SIZE_MAX && argv_index(&capture, paths[i]) > dash);
        CHECK(cached_path_is(repo, paths[i], false));
        capture_drop(&capture);
    }
}

static void test_stage_all_stages_every_change(const char *repo)
{
    SpawnCapture capture = {0};

    CHECK(write_repo_file(repo, "all-one.txt", "one\n"));
    CHECK(write_repo_file(repo, "all-two.txt", "two\n"));
    CHECK(invoke_real(repo, "ed.git.stage.all", NULL, 0U, &capture) ==
          YEW_CMD_OK);
    CHECK(capture.calls == 1U && capture.exit_code == 0);
    CHECK(cached_path_is(repo, "all-one.txt", true));
    CHECK(cached_path_is(repo, "all-two.txt", true));
    capture_drop(&capture);
}

static void test_unstage_all_clears_the_index(const char *repo)
{
    SpawnCapture capture = {0};

    CHECK(invoke_real(repo, "ed.git.unstage.all", NULL, 0U, &capture) ==
          YEW_CMD_OK);
    CHECK(capture.calls == 1U && capture.exit_code == 0);
    CHECK(capture.literal_paths);
    CHECK(cached_path_is(repo, "all-one.txt", false));
    CHECK(cached_path_is(repo, "all-two.txt", false));
    capture_drop(&capture);
}

static void test_branch_create_uses_the_exact_argument(const char *repo)
{
    const char branch[] = "topic-$(id)-n";
    char *tail[] = {(char *)"symbolic-ref", (char *)"--short",
                    (char *)"HEAD", NULL};
    SpawnCapture capture = {0};
    Bytes out = {0};
    size_t branch_at;
    int code = -1;

    CHECK(invoke_real(repo, "ed.git.branch.create", branch,
                      (u32)strlen(branch), &capture) == YEW_CMD_OK);
    CHECK(capture.calls == 1U && capture.exit_code == 0);
    branch_at = argv_index(&capture, branch);
    CHECK(branch_at >= 2U && branch_at < capture.argc);
    CHECK(branch_at >= 2U && strcmp(capture.argv[branch_at - 1U], "-c") == 0 &&
          strcmp(capture.argv[branch_at - 2U], "switch") == 0);
    CHECK(run_git(repo, tail, &out, &code) && code == 0);
    CHECK(out.len == strlen(branch) + 1U);
    CHECK(out.len != 0U && memcmp(out.data, branch, strlen(branch)) == 0);
    bytes_drop(&out);
    capture_drop(&capture);
}

static void test_tag_uses_the_exact_argument(const char *repo)
{
    const char tag[] = "tag-quote\"-$(id)";
    char *tail[] = {(char *)"tag", (char *)"--list", (char *)tag, NULL};
    SpawnCapture capture = {0};
    Bytes out = {0};
    int code = -1;

    CHECK(invoke_real(repo, "ed.git.tag", tag, (u32)strlen(tag), &capture) ==
          YEW_CMD_OK);
    CHECK(capture.calls == 1U && capture.exit_code == 0);
    CHECK(argv_index(&capture, "--") != SIZE_MAX);
    CHECK(argv_index(&capture, tag) > argv_index(&capture, "--"));
    CHECK(run_git(repo, tail, &out, &code) && code == 0);
    CHECK(out.len == strlen(tag) + 1U);
    CHECK(out.len != 0U && memcmp(out.data, tag, strlen(tag)) == 0);
    bytes_drop(&out);
    capture_drop(&capture);
}

static void test_stash_push_preserves_message_argv(const char *repo)
{
    static const char message[] = "quote\" $(id)\n-n";
    char *tail[] = {(char *)"log", (char *)"-1", (char *)"--format=%B",
                    (char *)"refs/stash", NULL};
    SpawnCapture capture = {0};
    Bytes out = {0};
    int code = -1;

    CHECK(write_repo_file(repo, "base.txt", "dirty\n"));
    CHECK(invoke_real(repo, "ed.git.stash.push", message,
                      (u32)(sizeof(message) - 1U), &capture) == YEW_CMD_OK);
    CHECK(capture.calls == 1U && capture.exit_code == 0);
    CHECK(capture.stdin_len == 0U);
    CHECK(argv_index(&capture, message) > argv_index(&capture, "-m"));
    CHECK(run_git(repo, tail, &out, &code) && code == 0);
    CHECK(out.len >= sizeof(message) - 1U);
    CHECK(bytes_contains(&out, (const u8 *)message, sizeof(message) - 1U));
    bytes_drop(&out);
    capture_drop(&capture);
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
        if (yew_now_ms() - start > 10000)
            return false;
    }
    return true;
}

static void test_empty_commit_message_does_not_spawn_git(const char *repo)
{
    SpawnCapture capture = {0};
    Buffer *commit;
    bool handled = false;
    Ed ed;

    yew_ed_init(&ed);
    CHECK(yew_ed_open_scratch(&ed));
    ed.ws.dir = arena_strdup(&ed.arena, repo);
    CHECK(yew_mode_enter(&ed, YEW_MODE_F) == YEW_CMD_OK);
    CHECK(run_jobs_idle(&ed));
    CHECK(invoke(&ed, "ed.git.commit", NULL, 0U) == YEW_CMD_OK);
    commit = ed.win == NULL ? NULL : ed.win->buf;
    capture.repo = repo;
    yew_git_test_spawn_set(capture_and_run, &capture);
    CHECK(yew_fuss_commit_save(&ed, commit, &handled) == YEW_CMD_ERR_STATE);
    yew_git_test_spawn_set(NULL, NULL);
    CHECK(handled);
    CHECK(capture.calls == 0U);
    capture_drop(&capture);
    yew_ed_free(&ed);
}

int main(int argc, char **argv)
{
    struct stat st;

    if (argc != 2) {
        (void)fprintf(stderr, "usage: %s EMPTY_DIRECTORY\n", argv[0]);
        return 2;
    }
    if (stat(argv[1], &st) != 0 || !S_ISDIR(st.st_mode)) {
        (void)fprintf(stderr, "fuss_commands: not a directory: %s\n",
                      argv[1]);
        return 2;
    }
    CHECK(setup_repo(argv[1]));
    test_stage_preserves_hostile_paths(argv[1]);
    test_unstage_preserves_hostile_paths(argv[1]);
    test_stage_all_stages_every_change(argv[1]);
    test_unstage_all_clears_the_index(argv[1]);
    test_branch_create_uses_the_exact_argument(argv[1]);
    test_tag_uses_the_exact_argument(argv[1]);
    test_stash_push_preserves_message_argv(argv[1]);
    test_empty_commit_message_does_not_spawn_git(argv[1]);
    if (failures != 0U) {
        (void)fprintf(stderr, "fuss_commands: %u/%u checks failed\n",
                      failures, assertions);
        return 1;
    }
    (void)printf("fuss_commands: %u assertions: ok\n", assertions);
    return 0;
}
