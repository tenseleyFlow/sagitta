#define _POSIX_C_SOURCE 200809L

/* Git owns index locking and replacement.  Killing a hunk-stage process may
 * leave the old index or the fully updated index, but never a partial blob. */
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "mod/git/gutter.h"

enum {
    DEFAULT_TRIALS = 40,
    FIXTURE_LINES = 30000,
    FIXTURE_LINE_CAP = 80,
    MAX_DELAY_NS = 20000000
};

typedef struct Bytes {
    u8 *data;
    size_t len;
    size_t cap;
} Bytes;

static bool bytes_append(Bytes *out, const u8 *data, size_t len)
{
    size_t cap;
    u8 *grown;

    if (out->len > SIZE_MAX - len - 1U)
        return false;
    if (out->len + len + 1U > out->cap) {
        cap = out->cap == 0U ? 4096U : out->cap;
        while (cap < out->len + len + 1U) {
            if (cap > SIZE_MAX / 2U)
                return false;
            cap *= 2U;
        }
        grown = realloc(out->data, cap);
        if (grown == NULL)
            return false;
        out->data = grown;
        out->cap = cap;
    }
    if (len != 0U)
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

static bool write_all(int fd, const u8 *data, size_t len)
{
    while (len != 0U) {
        ssize_t wrote = write(fd, data, len);

        if (wrote < 0 && errno == EINTR)
            continue;
        if (wrote <= 0)
            return false;
        data += (size_t)wrote;
        len -= (size_t)wrote;
    }
    return true;
}

static bool read_all(int fd, Bytes *out)
{
    u8 buf[16384];

    for (;;) {
        ssize_t got = read(fd, buf, sizeof(buf));

        if (got == 0)
            return true;
        if (got < 0 && errno == EINTR)
            continue;
        if (got < 0 || !bytes_append(out, buf, (size_t)got))
            return false;
    }
}

static bool run_process(const char *repo, char *const *argv,
                        const u8 *input, size_t input_len, Bytes *out)
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
        int devnull = open("/dev/null", O_WRONLY);

        (void)close(inpipe[1]);
        (void)close(outpipe[0]);
        if (chdir(repo) != 0 || dup2(inpipe[0], STDIN_FILENO) < 0 ||
            dup2(outpipe[1], STDOUT_FILENO) < 0 ||
            (devnull >= 0 && dup2(devnull, STDERR_FILENO) < 0))
            _exit(126);
        (void)close(inpipe[0]);
        (void)close(outpipe[1]);
        if (devnull >= 0)
            (void)close(devnull);
        execvp(argv[0], argv);
        _exit(127);
    }
    (void)close(inpipe[0]);
    (void)close(outpipe[1]);
    ok = write_all(inpipe[1], input, input_len);
    ok = close(inpipe[1]) == 0 && ok;
    ok = read_all(outpipe[0], out) && close(outpipe[0]) == 0 && ok;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR)
            return false;
    }
    return ok && WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

static bool run_git(const char *repo, char *const *tail,
                    const u8 *input, size_t input_len, Bytes *out)
{
    char *argv[16];
    size_t n = 0U;

    argv[n++] = (char *)"git";
    while (*tail != NULL && n + 1U < YEW_ARRAY_LEN(argv))
        argv[n++] = *tail++;
    if (*tail != NULL)
        return false;
    argv[n] = NULL;
    return run_process(repo, argv, input, input_len, out);
}

static bool make_file(const char *repo, const char *path,
                      const u8 *data, size_t len)
{
    char full[4096];
    int wrote = snprintf(full, sizeof(full), "%s/%s", repo, path);
    int fd;
    bool ok;

    if (wrote <= 0 || (size_t)wrote >= sizeof(full))
        return false;
    fd = open(full, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0)
        return false;
    ok = write_all(fd, data, len);
    return close(fd) == 0 && ok;
}

static bool make_fixture(Bytes *old, Bytes *replacement)
{
    u32 i;

    for (i = 0U; i < FIXTURE_LINES; i++) {
        char line[FIXTURE_LINE_CAP];
        int n = snprintf(line, sizeof(line),
                         "old-%05u-abcdefghijklmnopqrstuvwxyz-0123456789\n", i);

        if (n <= 0 || (size_t)n >= sizeof(line) ||
            !bytes_append(old, (const u8 *)line, (size_t)n))
            return false;
        n = snprintf(line, sizeof(line),
                     "new-%05u-ABCDEFGHIJKLMNOPQRSTUVWXYZ-9876543210\n", i);
        if (n <= 0 || (size_t)n >= sizeof(line) ||
            !bytes_append(replacement, (const u8 *)line, (size_t)n))
            return false;
    }
    return true;
}

static bool repo_init(const char *repo, const Bytes *old)
{
    char *init[] = {(char *)"init", (char *)"-q", NULL};
    char *name[] = {(char *)"config", (char *)"user.name",
                    (char *)"Yew Torture", NULL};
    char *mail[] = {(char *)"config", (char *)"user.email",
                    (char *)"yew@test.invalid", NULL};
    char *add[] = {(char *)"add", (char *)"--", (char *)"payload.txt", NULL};
    char *commit[] = {(char *)"commit", (char *)"-q", (char *)"-m",
                      (char *)"base", NULL};

    return mkdir(repo, 0700) == 0 &&
           run_git(repo, init, NULL, 0U, NULL) &&
           run_git(repo, name, NULL, 0U, NULL) &&
           run_git(repo, mail, NULL, 0U, NULL) &&
           make_file(repo, "payload.txt", old->data, old->len) &&
           run_git(repo, add, NULL, 0U, NULL) &&
           run_git(repo, commit, NULL, 0U, NULL);
}

static pid_t spawn_apply(const char *repo, int *input_fd)
{
    int pipefd[2];
    pid_t pid;

    if (pipe(pipefd) != 0)
        return -1;
    pid = fork();
    if (pid < 0)
        return -1;
    if (pid == 0) {
        int devnull = open("/dev/null", O_WRONLY);

        (void)close(pipefd[1]);
        if (chdir(repo) != 0 || dup2(pipefd[0], STDIN_FILENO) < 0 ||
            (devnull >= 0 && dup2(devnull, STDOUT_FILENO) < 0) ||
            (devnull >= 0 && dup2(devnull, STDERR_FILENO) < 0))
            _exit(126);
        (void)close(pipefd[0]);
        if (devnull >= 0)
            (void)close(devnull);
        execlp("git", "git", "apply", "--cached", "-", (char *)NULL);
        _exit(127);
    }
    (void)close(pipefd[0]);
    *input_fd = pipefd[1];
    return pid;
}

static u64 random_next(u64 *state)
{
    u64 x = *state;

    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    *state = x;
    return x * UINT64_C(2685821657736338717);
}

static bool index_is_allowed(const char *repo, const Bytes *old,
                             const Bytes *replacement, bool *saw_old,
                             bool *saw_new)
{
    char *show[] = {(char *)"show", (char *)":payload.txt", NULL};
    Bytes indexed = {0};
    bool is_old;
    bool is_new;
    bool ok;

    if (!run_git(repo, show, NULL, 0U, &indexed))
        return false;
    is_old = indexed.len == old->len &&
             memcmp(indexed.data, old->data, old->len) == 0;
    is_new = indexed.len == replacement->len &&
             memcmp(indexed.data, replacement->data, replacement->len) == 0;
    *saw_old = *saw_old || is_old;
    *saw_new = *saw_new || is_new;
    ok = is_old || is_new;
    bytes_drop(&indexed);
    return ok;
}

static unsigned parse_trials(void)
{
    const char *text = getenv("YEW_GIT_HUNK_KILL9_ITERS");
    unsigned long value;
    char *end;

    if (text == NULL || text[0] == '\0')
        return DEFAULT_TRIALS;
    errno = 0;
    value = strtoul(text, &end, 0);
    if (errno != 0 || end == text || *end != '\0' || value == 0U ||
        value > 10000U)
        return 0U;
    return (unsigned)value;
}

static bool remove_stale_lock(const char *repo)
{
    char path[4096];
    int wrote = snprintf(path, sizeof(path), "%s/.git/index.lock", repo);

    if (wrote <= 0 || (size_t)wrote >= sizeof(path))
        return false;
    return unlink(path) == 0 || errno == ENOENT;
}

int main(int argc, char **argv)
{
    Bytes old = {0};
    Bytes replacement = {0};
    Bytebuf patch;
    GitHunk whole;
    char repo[4096];
    unsigned trials;
    unsigned i;
    int wrote;
    bool saw_old = false;
    bool saw_new = false;
    u64 random_state = UINT64_C(0x53a7f00d12345678);
    char *reset[] = {(char *)"reset", (char *)"-q", (char *)"HEAD",
                     (char *)"--", (char *)"payload.txt", NULL};

    if (argc != 2) {
        (void)fprintf(stderr, "usage: %s TMPDIR\n", argv[0]);
        return 2;
    }
    trials = parse_trials();
    wrote = snprintf(repo, sizeof(repo), "%s/repo", argv[1]);
    if (trials == 0U || wrote <= 0 || (size_t)wrote >= sizeof(repo) ||
        !make_fixture(&old, &replacement) || !repo_init(repo, &old)) {
        (void)fprintf(stderr, "git_hunk_kill9: fixture setup failed\n");
        return 2;
    }
    whole = (GitHunk){LINENO(0U), LINENO(FIXTURE_LINES), LINENO(0U),
                      LINENO(FIXTURE_LINES), YEW_HUNK_MOD};
    if (!yew_git_hunk_patch(&patch, "payload.txt", old.data, old.len,
                            replacement.data, replacement.len, &whole)) {
        (void)fprintf(stderr, "git_hunk_kill9: patch generation failed\n");
        return 2;
    }
    (void)signal(SIGPIPE, SIG_IGN);
    for (i = 0U; i < trials; i++) {
        struct timespec delay;
        int input_fd;
        int status;
        pid_t pid;

        if (!run_git(repo, reset, NULL, 0U, NULL))
            return 2;
        pid = spawn_apply(repo, &input_fd);
        if (pid < 0)
            return 2;
        (void)write_all(input_fd, patch.data, patch.len);
        (void)close(input_fd);
        delay.tv_sec = 0;
        delay.tv_nsec = (long)(random_next(&random_state) % MAX_DELAY_NS);
        while (nanosleep(&delay, &delay) != 0 && errno == EINTR) { }
        (void)kill(pid, SIGKILL);
        while (waitpid(pid, &status, 0) < 0) {
            if (errno != EINTR)
                return 2;
        }
        if (!index_is_allowed(repo, &old, &replacement, &saw_old, &saw_new)) {
            (void)fprintf(stderr,
                          "git_hunk_kill9: partial index after trial %u\n", i);
            return 1;
        }
        if (!remove_stale_lock(repo)) {
            (void)fprintf(stderr,
                          "git_hunk_kill9: could not remove stale index lock\n");
            return 2;
        }
    }
    if (!run_git(repo, reset, NULL, 0U, NULL) ||
        !run_git(repo,
                 (char *const[]){(char *)"apply", (char *)"--cached",
                                 (char *)"-", NULL},
                 patch.data, patch.len, NULL) ||
        !index_is_allowed(repo, &old, &replacement, &saw_old, &saw_new) ||
        !saw_new) {
        (void)fprintf(stderr, "git_hunk_kill9: successful-apply control failed\n");
        return 1;
    }
    (void)printf("git_hunk_kill9: %u kills, old=%s new=%s, no partial index\n",
                 trials, saw_old ? "yes" : "no", saw_new ? "yes" : "no");
    bytebuf_free(&patch);
    bytes_drop(&replacement);
    bytes_drop(&old);
    return 0;
}
