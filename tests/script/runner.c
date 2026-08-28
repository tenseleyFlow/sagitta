#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700

/*
 * Sprint 37's isolated Fletch script-test runner.
 *
 * The suite carries editor-level coverage parked by Sprints 12, 14, 17,
 * 19, 21, 34, 35, and 36.  Each file gets a fresh process and filesystem
 * root: editor state, configuration, logs, and crashes cannot leak into the
 * next test.  fd 3 is deliberately separate from stdout/stderr because those
 * streams are part of the --batch product contract being tested.
 * A test whose first line is exactly "# CONFIG" runs without --clean; every
 * other test gets --clean.  Keeping the opt-in in byte zero makes discovery
 * deterministic and prevents an incidental comment later in a test from
 * changing its environment.  A marked test may carry <test>.config, passed as
 * an explicit --config path; the runner also pins YEW_RUNTIME_DIR and supplies
 * an absolute YEW_TEST_FAKELSP only through --fakelsp.  An optional sibling
 * <test>.stdout file pins the child's stdout byte-for-byte; tests without one
 * retain the original rules.
 */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
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

enum {
    DEFAULT_BUDGET_MS = 10000,
    RESULT_FD = 3,
    CAPTURE_LIMIT = 16 * 1024 * 1024
};

typedef struct Bytes {
    char *data;
    size_t len;
    size_t cap;
    bool overflow;
} Bytes;

typedef struct TestFile {
    char *name;
    char *path;
    char *stdout_path;
    char *config_path;
    bool config;
} TestFile;

typedef struct TestList {
    TestFile *data;
    size_t len;
    size_t cap;
} TestList;

typedef struct Protocol {
    size_t assertions;
    size_t failures;
    size_t skipped;
    bool valid;
} Protocol;

typedef struct RunResult {
    Bytes out;
    Bytes err;
    Bytes protocol;
    int status;
    bool waited;
    bool timed_out;
    bool setup_failed;
} RunResult;

static bool mark_fd_cloexec(int fd)
{
    int flags = fcntl(fd, F_GETFD);

    if (flags < 0)
        return errno == EBADF;
    return (flags & FD_CLOEXEC) != 0 ||
           fcntl(fd, F_SETFD, flags | FD_CLOEXEC) == 0;
}

/*
 * The runner may inherit descriptors from its launcher (GitHub Actions keeps
 * several high-numbered pipes open).  They belong to the runner, never to the
 * isolated editor children: letting them cross exec both breaks isolation and
 * makes Valgrind's fd report contaminate captured child stderr.
 */
static bool mark_inherited_fds_cloexec(void)
{
    long limit = sysconf(_SC_OPEN_MAX);

    if (limit < 0 || limit > INT_MAX) {
        errno = EOVERFLOW;
        return false;
    }
#if defined(__linux__)
    DIR *dir = opendir("/proc/self/fd");

    if (dir != NULL) {
        struct dirent *entry;
        bool ok = true;

        errno = 0;
        while ((entry = readdir(dir)) != NULL) {
            char *end;
            long fd;

            errno = 0;
            fd = strtol(entry->d_name, &end, 10);
            if (errno != 0 || *entry->d_name == '\0' || *end != '\0' ||
                fd <= STDERR_FILENO || fd >= limit)
                continue;
            if (!mark_fd_cloexec((int)fd))
                ok = false;
        }
        if (errno != 0)
            ok = false;
        if (closedir(dir) != 0)
            ok = false;
        return ok;
    }
#endif
    {
        int fd;

        for (fd = STDERR_FILENO + 1; fd < (int)limit; fd++)
            if (!mark_fd_cloexec(fd))
                return false;
    }
    return true;
}

static void bytes_free(Bytes *bytes)
{
    free(bytes->data);
    memset(bytes, 0, sizeof(*bytes));
}

static bool bytes_append(Bytes *bytes, const char *data, size_t len)
{
    size_t need;
    size_t cap;
    char *grown;

    if (len == 0U || bytes->overflow)
        return !bytes->overflow;
    if (len > CAPTURE_LIMIT - bytes->len) {
        bytes->overflow = true;
        return false;
    }
    need = bytes->len + len + 1U;
    if (need > bytes->cap) {
        cap = bytes->cap == 0U ? 4096U : bytes->cap;
        while (cap < need) {
            if (cap > CAPTURE_LIMIT / 2U) {
                cap = CAPTURE_LIMIT + 1U;
                break;
            }
            cap *= 2U;
        }
        grown = realloc(bytes->data, cap);
        if (grown == NULL) {
            bytes->overflow = true;
            return false;
        }
        bytes->data = grown;
        bytes->cap = cap;
    }
    memcpy(bytes->data + bytes->len, data, len);
    bytes->len += len;
    bytes->data[bytes->len] = '\0';
    return true;
}

static bool bytes_read_path(const char *path, Bytes *bytes)
{
    char buf[16384];
    int fd = open(path, O_RDONLY);
    bool ok = fd >= 0;

    while (ok) {
        ssize_t got = read(fd, buf, sizeof(buf));

        if (got == 0)
            break;
        if (got < 0) {
            if (errno == EINTR)
                continue;
            ok = false;
            break;
        }
        if (!bytes_append(bytes, buf, (size_t)got)) {
            ok = false;
            break;
        }
    }
    if (fd >= 0 && close(fd) != 0)
        ok = false;
    return ok;
}

static bool bytes_equal(const Bytes *left, const Bytes *right)
{
    return !left->overflow && !right->overflow && left->len == right->len &&
           (left->len == 0U ||
            memcmp(left->data, right->data, left->len) == 0);
}

static char *path_join(const char *left, const char *right)
{
    size_t nl = strlen(left);
    size_t nr = strlen(right);
    bool slash = nl != 0U && left[nl - 1U] != '/';
    char *path;

    if (nl > SIZE_MAX - nr - (slash ? 2U : 1U))
        return NULL;
    path = malloc(nl + nr + (slash ? 2U : 1U));
    if (path == NULL)
        return NULL;
    memcpy(path, left, nl);
    if (slash)
        path[nl++] = '/';
    memcpy(path + nl, right, nr + 1U);
    return path;
}

static bool has_fl_suffix(const char *name)
{
    size_t len = strlen(name);

    return len > 3U && strcmp(name + len - 3U, ".fl") == 0;
}

static char *stdout_sibling_path(const char *path)
{
    size_t len = strlen(path);
    char *sibling;

    if (len < 3U || strcmp(path + len - 3U, ".fl") != 0 ||
        len > SIZE_MAX - 5U)
        return NULL;
    sibling = malloc(len + 5U);
    if (sibling == NULL)
        return NULL;
    memcpy(sibling, path, len - 3U);
    memcpy(sibling + len - 3U, ".stdout", sizeof(".stdout"));
    return sibling;
}

static char *config_sibling_path(const char *path)
{
    size_t len = strlen(path);
    char *sibling;

    if (len < 3U || strcmp(path + len - 3U, ".fl") != 0 ||
        len > SIZE_MAX - 5U)
        return NULL;
    sibling = malloc(len + 5U);
    if (sibling == NULL)
        return NULL;
    memcpy(sibling, path, len - 3U);
    memcpy(sibling + len - 3U, ".config", sizeof(".config"));
    return sibling;
}

static bool optional_config_sibling(const char *path, bool requested,
                                    char **sibling)
{
    struct stat st;
    char *candidate;

    *sibling = NULL;
    if (!requested)
        return true;
    candidate = config_sibling_path(path);
    if (candidate == NULL)
        return false;
    if (lstat(candidate, &st) == 0) {
        if (!S_ISREG(st.st_mode)) {
            free(candidate);
            return false;
        }
        *sibling = candidate;
        return true;
    }
    if (errno != ENOENT) {
        free(candidate);
        return false;
    }
    free(candidate);
    return true;
}

static bool optional_stdout_sibling(const char *path, char **sibling)
{
    struct stat st;
    char *candidate = stdout_sibling_path(path);

    *sibling = NULL;
    if (candidate == NULL)
        return false;
    if (lstat(candidate, &st) == 0) {
        if (!S_ISREG(st.st_mode)) {
            free(candidate);
            return false;
        }
        *sibling = candidate;
        return true;
    }
    if (errno != ENOENT) {
        free(candidate);
        return false;
    }
    free(candidate);
    return true;
}

static bool config_header_bytes(const char *data, size_t len)
{
    static const char directive[] = "# CONFIG";
    size_t n = sizeof(directive) - 1U;

    return len >= n && memcmp(data, directive, n) == 0 &&
           (len == n || data[n] == '\n' ||
            (data[n] == '\r' && len > n + 1U && data[n + 1U] == '\n'));
}

static bool header_requests_config(const char *path)
{
    char first[10];
    int fd = open(path, O_RDONLY);
    ssize_t got;

    if (fd < 0)
        return false;
    do {
        got = read(fd, first, sizeof(first));
    } while (got < 0 && errno == EINTR);
    (void)close(fd);
    return got >= 0 && config_header_bytes(first, (size_t)got);
}

static char *test_name(const char *file)
{
    size_t len = strlen(file);
    char *name = malloc(len - 2U);

    if (name == NULL)
        return NULL;
    memcpy(name, file, len - 3U);
    name[len - 3U] = '\0';
    return name;
}

static bool list_push(TestList *list, char *name, char *path,
                      char *stdout_path, char *config_path, bool config)
{
    TestFile *grown;
    size_t cap;

    if (list->len == list->cap) {
        cap = list->cap == 0U ? 32U : list->cap * 2U;
        if (cap < list->cap || cap > SIZE_MAX / sizeof(*list->data))
            return false;
        grown = realloc(list->data, cap * sizeof(*list->data));
        if (grown == NULL)
            return false;
        list->data = grown;
        list->cap = cap;
    }
    list->data[list->len].name = name;
    list->data[list->len].path = path;
    list->data[list->len].stdout_path = stdout_path;
    list->data[list->len].config_path = config_path;
    list->data[list->len].config = config;
    list->len++;
    return true;
}

static void list_free(TestList *list)
{
    size_t i;

    for (i = 0U; i < list->len; i++) {
        free(list->data[i].name);
        free(list->data[i].path);
        free(list->data[i].stdout_path);
        free(list->data[i].config_path);
    }
    free(list->data);
    memset(list, 0, sizeof(*list));
}

/* Stable insertion sort is enough for this small directory and stays C11. */
static void list_sort(TestList *list)
{
    size_t i;

    for (i = 1U; i < list->len; i++) {
        TestFile value = list->data[i];
        size_t at = i;

        while (at != 0U && strcmp(list->data[at - 1U].name,
                                  value.name) > 0) {
            list->data[at] = list->data[at - 1U];
            at--;
        }
        list->data[at] = value;
    }
}

static bool discover(const char *dir_path, TestList *list)
{
    DIR *dir = opendir(dir_path);
    struct dirent *entry;
    bool ok = true;

    if (dir == NULL)
        return false;
    for (;;) {
        struct stat st;
        char *path;
        char *name;
        char *stdout_path = NULL;
        char *config_path = NULL;
        bool config;

        errno = 0;
        entry = readdir(dir);
        if (entry == NULL) {
            if (errno != 0)
                ok = false;
            break;
        }
        if (!has_fl_suffix(entry->d_name))
            continue;
        path = path_join(dir_path, entry->d_name);
        if (path == NULL) {
            ok = false;
            break;
        }
        if (lstat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
            free(path);
            continue;
        }
        name = test_name(entry->d_name);
        config = header_requests_config(path);
        if (name == NULL || !optional_stdout_sibling(path, &stdout_path) ||
            !optional_config_sibling(path, config, &config_path) ||
            !list_push(list, name, path, stdout_path, config_path, config)) {
            free(name);
            free(path);
            if (name != NULL) {
                free(stdout_path);
                free(config_path);
            }
            ok = false;
            break;
        }
    }
    if (closedir(dir) != 0)
        ok = false;
    if (ok)
        list_sort(list);
    return ok;
}

static bool copy_fd(int src, int dst)
{
    char buf[16384];

    for (;;) {
        ssize_t got = read(src, buf, sizeof(buf));
        size_t off = 0U;

        if (got == 0)
            return true;
        if (got < 0) {
            if (errno == EINTR)
                continue;
            return false;
        }
        while (off < (size_t)got) {
            ssize_t put = write(dst, buf + off, (size_t)got - off);

            if (put < 0) {
                if (errno == EINTR)
                    continue;
                return false;
            }
            off += (size_t)put;
        }
    }
}

static bool copy_tree(const char *src, const char *dst)
{
    struct stat st;

    if (lstat(src, &st) != 0)
        return errno == ENOENT;
    if (S_ISREG(st.st_mode)) {
        int in = open(src, O_RDONLY);
        int out;
        bool ok;

        if (in < 0)
            return false;
        out = open(dst, O_WRONLY | O_CREAT | O_TRUNC, st.st_mode & 0777);
        if (out < 0) {
            (void)close(in);
            return false;
        }
        ok = copy_fd(in, out);
        if (close(in) != 0)
            ok = false;
        if (close(out) != 0)
            ok = false;
        return ok;
    }
    if (S_ISLNK(st.st_mode)) {
        size_t cap = st.st_size > 0 ? (size_t)st.st_size + 1U : 256U;
        char *target = malloc(cap);
        ssize_t len;
        bool ok;

        if (target == NULL)
            return false;
        len = readlink(src, target, cap - 1U);
        if (len < 0 || (size_t)len == cap - 1U) {
            free(target);
            return false;
        }
        target[len] = '\0';
        ok = symlink(target, dst) == 0;
        free(target);
        return ok;
    }
    if (S_ISDIR(st.st_mode)) {
        DIR *dir;
        struct dirent *entry;
        bool ok = true;

        if (mkdir(dst, st.st_mode & 0777) != 0 && errno != EEXIST)
            return false;
        dir = opendir(src);
        if (dir == NULL)
            return false;
        for (;;) {
            char *src_child;
            char *dst_child;

            errno = 0;
            entry = readdir(dir);
            if (entry == NULL) {
                if (errno != 0)
                    ok = false;
                break;
            }
            if (strcmp(entry->d_name, ".") == 0 ||
                strcmp(entry->d_name, "..") == 0)
                continue;
            src_child = path_join(src, entry->d_name);
            dst_child = path_join(dst, entry->d_name);
            if (src_child == NULL || dst_child == NULL ||
                !copy_tree(src_child, dst_child))
                ok = false;
            free(src_child);
            free(dst_child);
            if (!ok)
                break;
        }
        if (closedir(dir) != 0)
            ok = false;
        return ok;
    }
    errno = ENOTSUP;
    return false;
}

static bool remove_tree(const char *path)
{
    struct stat st;
    DIR *dir;
    struct dirent *entry;
    bool ok = true;

    if (lstat(path, &st) != 0)
        return errno == ENOENT;
    if (!S_ISDIR(st.st_mode))
        return unlink(path) == 0;
    dir = opendir(path);
    if (dir == NULL)
        return false;
    for (;;) {
        char *child;

        errno = 0;
        entry = readdir(dir);
        if (entry == NULL) {
            if (errno != 0)
                ok = false;
            break;
        }
        if (strcmp(entry->d_name, ".") == 0 ||
            strcmp(entry->d_name, "..") == 0)
            continue;
        child = path_join(path, entry->d_name);
        if (child == NULL || !remove_tree(child))
            ok = false;
        free(child);
    }
    if (closedir(dir) != 0)
        ok = false;
    if (rmdir(path) != 0)
        ok = false;
    return ok;
}

static int64_t now_ms(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return -1;
    return (int64_t)ts.tv_sec * 1000 + (int64_t)ts.tv_nsec / 1000000;
}

static int64_t budget_ms(void)
{
    const char *value = getenv("YEW_SCRIPT_BUDGET_MS");
    int64_t parsed = 0;

    if (value == NULL || *value == '\0')
        return DEFAULT_BUDGET_MS;
    while (*value != '\0') {
        int digit;

        if (*value < '0' || *value > '9')
            return DEFAULT_BUDGET_MS;
        digit = *value - '0';
        if (parsed > (INT64_MAX - digit) / 10)
            return DEFAULT_BUDGET_MS;
        parsed = parsed * 10 + digit;
        value++;
    }
    return parsed > 0 ? parsed : DEFAULT_BUDGET_MS;
}

static bool set_nonblock(int fd)
{
    int flags = fcntl(fd, F_GETFL);

    return flags >= 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

static void close_pipe(int pipefd[2])
{
    if (pipefd[0] >= 0)
        (void)close(pipefd[0]);
    if (pipefd[1] >= 0)
        (void)close(pipefd[1]);
    pipefd[0] = -1;
    pipefd[1] = -1;
}

static bool set_result_fd_environment(void)
{
    return setenv("YEW_SCRIPT_RESULT_FD", "3", 1) == 0;
}

static bool child_environment(const char *root, const char *work,
                              const char *fakelsp)
{
    char *cfg = path_join(root, "cfg");
    char *state = path_join(root, "state");
    char *data = path_join(root, "data");
    char *cache = path_join(root, "cache");
    char *runtime = realpath("runtime", NULL);
    bool fake_ok = fakelsp == NULL ? unsetenv("YEW_TEST_FAKELSP") == 0 :
                   setenv("YEW_TEST_FAKELSP", fakelsp, 1) == 0;
    bool ok = cfg != NULL && state != NULL && data != NULL && cache != NULL &&
              runtime != NULL && fake_ok;

    if (ok)
        ok = setenv("HOME", root, 1) == 0 &&
             setenv("XDG_CONFIG_HOME", cfg, 1) == 0 &&
             setenv("XDG_STATE_HOME", state, 1) == 0 &&
             setenv("XDG_DATA_HOME", data, 1) == 0 &&
             setenv("XDG_CACHE_HOME", cache, 1) == 0 &&
             setenv("YEW_RUNTIME_DIR", runtime, 1) == 0 &&
             setenv("TMPDIR", root, 1) == 0 &&
             setenv("YEW_SCRIPT_TMPDIR", root, 1) == 0 &&
             set_result_fd_environment() &&
             chdir(work) == 0;
    free(cfg);
    free(state);
    free(data);
    free(cache);
    free(runtime);
    return ok;
}

static size_t build_child_argv(char **argv, const char *yew,
                               const char *script, bool config,
                               const char *config_path, bool grant_examples)
{
    size_t argc = 0U;

    argv[argc++] = (char *)yew;
    argv[argc++] = (char *)"--batch";
    argv[argc++] = (char *)"--test";
    if (grant_examples) {
        argv[argc++] = (char *)"--grant";
        argv[argc++] = (char *)"session-notes:fs";
    }
    if (config_path != NULL) {
        argv[argc++] = (char *)"--config";
        argv[argc++] = (char *)config_path;
    } else if (!config) {
        argv[argc++] = (char *)"--clean";
    }
    argv[argc++] = (char *)script;
    argv[argc] = NULL;
    return argc;
}

static void child_exec(const char *yew, const char *script,
                       const char *root, const char *work,
                       bool config, const char *config_path,
                       bool grant_examples,
                       const char *fakelsp, int out_pipe[2], int err_pipe[2],
                       int result_pipe[2])
{
    int fds[6];
    size_t i;
    char *child_argv[10];

    (void)build_child_argv(child_argv, yew, script, config, config_path,
                           grant_examples);

    if (setpgid(0, 0) != 0)
        _exit(126);
    fds[0] = out_pipe[0];
    fds[1] = out_pipe[1];
    fds[2] = err_pipe[0];
    fds[3] = err_pipe[1];
    fds[4] = result_pipe[0];
    fds[5] = result_pipe[1];
    if (dup2(out_pipe[1], STDOUT_FILENO) < 0 ||
        dup2(err_pipe[1], STDERR_FILENO) < 0 ||
        dup2(result_pipe[1], RESULT_FD) < 0)
        _exit(126);
    for (i = 0U; i < sizeof(fds) / sizeof(fds[0]); i++)
        if (fds[i] > RESULT_FD)
            (void)close(fds[i]);
    if (!child_environment(root, work, fakelsp)) {
        (void)dprintf(STDERR_FILENO,
                      "script: cannot prepare sandbox: %s\n",
                      strerror(errno));
        _exit(126);
    }
    execv(yew, child_argv);
    (void)dprintf(STDERR_FILENO, "script: cannot exec %s: %s\n",
                  yew, strerror(errno));
    _exit(127);
}

static void drain_fd(int *fd, Bytes *dst)
{
    char buf[8192];

    for (;;) {
        ssize_t got = read(*fd, buf, sizeof(buf));

        if (got > 0) {
            (void)bytes_append(dst, buf, (size_t)got);
            continue;
        }
        if (got == 0) {
            (void)close(*fd);
            *fd = -1;
        } else if (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK) {
            (void)close(*fd);
            *fd = -1;
            dst->overflow = true;
        }
        return;
    }
}

static bool collect_child(pid_t child, int out_fd, int err_fd, int result_fd,
                          RunResult *result)
{
    int64_t start = now_ms();
    int64_t deadline;
    int fds[3] = {out_fd, err_fd, result_fd};
    Bytes *captures[3];
    bool ok = false;
    size_t i;

    if (start < 0)
        goto done;
    {
        int64_t budget = budget_ms();

        deadline = budget > INT64_MAX - start ? INT64_MAX : start + budget;
    }
    captures[0] = &result->out;
    captures[1] = &result->err;
    captures[2] = &result->protocol;
    if (!set_nonblock(fds[0]) || !set_nonblock(fds[1]) ||
        !set_nonblock(fds[2]))
        goto done;
    while (!result->waited || fds[0] >= 0 || fds[1] >= 0 || fds[2] >= 0) {
        struct pollfd pollfds[3];
        int64_t now = now_ms();
        int timeout = 20;

        if (now < 0)
            goto done;
        if (!result->timed_out && now >= deadline) {
            if (kill(-child, SIGKILL) != 0 && errno != ESRCH)
                goto done;
            result->timed_out = true;
        }
        if (!result->waited) {
            pid_t got = waitpid(child, &result->status, WNOHANG);

            if (got == child)
                result->waited = true;
            else if (got < 0 && errno != EINTR)
                goto done;
        }
        for (i = 0U; i < 3U; i++) {
            pollfds[i].fd = fds[i];
            pollfds[i].events = POLLIN | POLLHUP;
            pollfds[i].revents = 0;
        }
        if (!result->timed_out && deadline > now && deadline - now < timeout)
            timeout = (int)(deadline - now);
        if (result->waited)
            timeout = 0;
        if (poll(pollfds, 3U, timeout) < 0 && errno != EINTR)
            goto done;
        for (i = 0U; i < 3U; i++)
            if (fds[i] >= 0 &&
                (pollfds[i].revents & (POLLIN | POLLHUP | POLLERR)) != 0)
                drain_fd(&fds[i], captures[i]);
    }
    ok = true;
done:
    for (i = 0U; i < 3U; i++)
        if (fds[i] >= 0)
            (void)close(fds[i]);
    return ok;
}

static bool make_sandbox(const char *fixtures, char **root_out,
                         char **work_out)
{
    const char *tmp = getenv("TMPDIR");
    char *template;
    char *canonical;
    char *work;
    char *fixture_dst;
    char *cfg;
    char *state;
    char *data;
    char *cache;
    bool ok;

    if (tmp == NULL || *tmp == '\0')
        tmp = "/tmp";
    template = path_join(tmp, "yew-script-XXXXXX");
    if (template == NULL || mkdtemp(template) == NULL) {
        free(template);
        return false;
    }
    canonical = realpath(template, NULL);
    if (canonical == NULL) {
        (void)remove_tree(template);
        free(template);
        return false;
    }
    free(template);
    template = canonical;
    work = path_join(template, "work");
    fixture_dst = work == NULL ? NULL : path_join(work, "fixtures");
    cfg = path_join(template, "cfg");
    state = path_join(template, "state");
    data = path_join(template, "data");
    cache = path_join(template, "cache");
    ok = work != NULL && fixture_dst != NULL && cfg != NULL &&
         state != NULL && data != NULL && cache != NULL &&
         mkdir(work, 0700) == 0 && mkdir(cfg, 0700) == 0 &&
         mkdir(state, 0700) == 0 && mkdir(data, 0700) == 0 &&
         mkdir(cache, 0700) == 0 && copy_tree(fixtures, fixture_dst);
    free(fixture_dst);
    free(cfg);
    free(state);
    free(data);
    free(cache);
    if (!ok) {
        (void)remove_tree(template);
        free(work);
        free(template);
        return false;
    }
    *root_out = template;
    *work_out = work;
    return true;
}

static bool stage_example_plugins(const char *root)
{
    char *data = path_join(root, "data");
    char *yew = data == NULL ? NULL : path_join(data, "yew");
    char *plugins = yew == NULL ? NULL : path_join(yew, "plugins");
    char *work = path_join(root, "work");
    char *guide = work == NULL ? NULL :
                  path_join(work, "plugins-authoring.md");
    bool ok = data != NULL && yew != NULL && plugins != NULL &&
              work != NULL && guide != NULL &&
              mkdir(yew, 0700) == 0 &&
              copy_tree("examples/plugins", plugins) &&
              copy_tree("docs/plugins-authoring.md", guide);

    free(data);
    free(yew);
    free(plugins);
    free(work);
    free(guide);
    return ok;
}

static bool run_test(const char *yew, const char *fixtures,
                     const char *fakelsp, const TestFile *test,
                     char **sandbox, RunResult *result)
{
    int out_pipe[2] = {-1, -1};
    int err_pipe[2] = {-1, -1};
    int result_pipe[2] = {-1, -1};
    char *work = NULL;
    pid_t child;
    bool ok;

    memset(result, 0, sizeof(*result));
    if (!make_sandbox(fixtures, sandbox, &work)) {
        result->setup_failed = true;
        return false;
    }
    if (strncmp(test->name, "plug_examples_", 14U) == 0 &&
        !stage_example_plugins(*sandbox)) {
        free(work);
        result->setup_failed = true;
        return false;
    }
    if (pipe(out_pipe) != 0 || pipe(err_pipe) != 0 || pipe(result_pipe) != 0) {
        close_pipe(out_pipe);
        close_pipe(err_pipe);
        close_pipe(result_pipe);
        free(work);
        result->setup_failed = true;
        return false;
    }
    child = fork();
    if (child < 0) {
        close_pipe(out_pipe);
        close_pipe(err_pipe);
        close_pipe(result_pipe);
        free(work);
        result->setup_failed = true;
        return false;
    }
    if (child == 0)
        child_exec(yew, test->path, *sandbox, work,
                   test->config, test->config_path,
                   strcmp(test->name, "plug_examples_matrix") == 0,
                   fakelsp,
                   out_pipe, err_pipe, result_pipe);
    if (setpgid(child, child) != 0 && errno != EACCES && errno != ESRCH) {
        (void)kill(child, SIGKILL);
        while (waitpid(child, &result->status, 0) < 0 && errno == EINTR) {
        }
        result->waited = true;
        close_pipe(out_pipe);
        close_pipe(err_pipe);
        close_pipe(result_pipe);
        free(work);
        result->setup_failed = true;
        return false;
    }
    (void)close(out_pipe[1]);
    (void)close(err_pipe[1]);
    (void)close(result_pipe[1]);
    out_pipe[1] = -1;
    err_pipe[1] = -1;
    result_pipe[1] = -1;
    ok = collect_child(child, out_pipe[0], err_pipe[0], result_pipe[0],
                       result);
    if (!ok && !result->waited) {
        (void)kill(-child, SIGKILL);
        while (waitpid(child, &result->status, 0) < 0 && errno == EINTR) {
        }
        result->waited = true;
    }
    if (!ok)
        result->setup_failed = true;
    free(work);
    return ok;
}

static bool parse_size(const char *text, size_t *value)
{
    size_t parsed = 0U;

    if (*text == '\0')
        return false;
    while (*text != '\0') {
        size_t digit;

        if (*text < '0' || *text > '9')
            return false;
        digit = (size_t)(*text - '0');
        if (parsed > (SIZE_MAX - digit) / 10U)
            return false;
        parsed = parsed * 10U + digit;
        text++;
    }
    *value = parsed;
    return true;
}

static Protocol parse_protocol(const Bytes *bytes)
{
    Protocol parsed = {0};
    char *copy;
    char *line;
    char *save = NULL;
    bool saw_summary = false;
    size_t fail_lines = 0U;

    if (bytes->overflow || bytes->len == 0U ||
        bytes->data[bytes->len - 1U] != '\n' ||
        memchr(bytes->data, '\0', bytes->len) != NULL)
        return parsed;
    copy = malloc(bytes->len + 1U);
    if (copy == NULL)
        return parsed;
    memcpy(copy, bytes->data, bytes->len + 1U);
    for (line = strtok_r(copy, "\n", &save); line != NULL;
         line = strtok_r(NULL, "\n", &save)) {
        if (strncmp(line, "FAIL\t", 5U) == 0 && !saw_summary) {
            fail_lines++;
            continue;
        }
        if (strncmp(line, "YEWTEST\t", 8U) == 0 && !saw_summary) {
            char *a = line + 8U;
            char *f = strchr(a, '\t');
            char *s;

            if (f == NULL)
                break;
            *f++ = '\0';
            s = strchr(f, '\t');
            if (s == NULL || strchr(s + 1U, '\t') != NULL)
                break;
            *s++ = '\0';
            if (!parse_size(a, &parsed.assertions) ||
                !parse_size(f, &parsed.failures) ||
                !parse_size(s, &parsed.skipped))
                break;
            saw_summary = true;
            continue;
        }
        break;
    }
    parsed.valid = saw_summary && line == NULL &&
                   ((parsed.failures == 0U && fail_lines == 0U) ||
                    (parsed.failures != 0U && fail_lines != 0U));
    free(copy);
    return parsed;
}

static bool contains_span(const char *name, const char *span, size_t span_len)
{
    size_t name_len = strlen(name);
    size_t i;

    if (span_len == 0U || span_len > name_len)
        return false;
    for (i = 0U; i <= name_len - span_len; i++)
        if (memcmp(name + i, span, span_len) == 0)
            return true;
    return false;
}

static bool excluded_test(const char *name, const char *exclude)
{
    const char *start;

    if (exclude == NULL)
        return false;
    start = exclude;
    for (;;) {
        const char *comma = strchr(start, ',');
        size_t len = comma == NULL ? strlen(start) : (size_t)(comma - start);

        if (contains_span(name, start, len))
            return true;
        if (comma == NULL)
            return false;
        start = comma + 1;
    }
}

static bool selected_test(const char *name, const char *filter,
                          const char *exclude)
{
    return (filter == NULL || strstr(name, filter) != NULL) &&
           !excluded_test(name, exclude);
}

static size_t selected_count(const TestList *tests, const char *filter,
                             const char *exclude)
{
    size_t selected = 0U;
    size_t i;

    for (i = 0U; i < tests->len; i++)
        if (selected_test(tests->data[i].name, filter, exclude))
            selected++;
    return selected;
}

static int selection_status(size_t selected)
{
    return selected == 0U ? 1 : 0;
}

static bool format_count_line(char *line, size_t cap, const char *verdict,
                              const char *name, size_t assertions,
                              size_t failures)
{
    int n;

    if (failures == 0U)
        n = snprintf(line, cap, "%s %-36s (%zu assertions)\n",
                     verdict, name, assertions);
    else
        n = snprintf(line, cap,
                     "%s %-36s (%zu assertions, %zu failure%s)\n",
                     verdict, name, assertions, failures,
                     failures == 1U ? "" : "s");
    return n >= 0 && (size_t)n < cap;
}

static bool finish_sandbox(const char *sandbox, bool passed)
{
    return !passed || remove_tree(sandbox);
}

static void print_capture(const char *label, const Bytes *bytes)
{
    size_t at = 0U;

    if (bytes->len == 0U && !bytes->overflow)
        return;
    (void)printf("  child %s:\n", label);
    while (at < bytes->len) {
        size_t end = at;

        while (end < bytes->len && bytes->data[end] != '\n')
            end++;
        (void)fputs("    ", stdout);
        if (end != at)
            (void)fwrite(bytes->data + at, 1U, end - at, stdout);
        (void)fputc('\n', stdout);
        at = end < bytes->len ? end + 1U : end;
    }
    if (bytes->overflow)
        (void)printf("    [capture exceeded %u bytes]\n", CAPTURE_LIMIT);
}

static void print_protocol_failures(const Bytes *bytes)
{
    size_t at = 0U;

    while (at < bytes->len) {
        size_t end = at;

        while (end < bytes->len && bytes->data[end] != '\n')
            end++;
        if (end - at >= 5U &&
            memcmp(bytes->data + at, "FAIL\t", 5U) == 0) {
            (void)fputs("  ", stdout);
            if (end - at != 5U)
                (void)fwrite(bytes->data + at + 5U, 1U,
                             end - at - 5U, stdout);
            (void)fputc('\n', stdout);
        }
        at = end < bytes->len ? end + 1U : end;
    }
}

static const char *failure_reason(const RunResult *result,
                                  const Protocol *protocol,
                                  char *buf, size_t cap)
{
    if (result->setup_failed)
        return "runner setup failed";
    if (result->timed_out)
        return "timeout";
    if (!result->waited)
        return "runner lost child";
    if (WIFSIGNALED(result->status)) {
        (void)snprintf(buf, cap, "signal %d", WTERMSIG(result->status));
        return buf;
    }
    if (!WIFEXITED(result->status))
        return "unknown child status";
    if (WEXITSTATUS(result->status) != 0) {
        (void)snprintf(buf, cap, "exit %d", WEXITSTATUS(result->status));
        return buf;
    }
    if (!protocol->valid)
        return "invalid assertion protocol";
    if (protocol->failures != 0U)
        return "assertion failure";
    if (protocol->skipped == 0U && protocol->assertions == 0U)
        return "no assertions";
    return NULL;
}

static const char *expected_stdout_failure(const TestFile *test,
                                           const RunResult *result,
                                           Bytes *expected)
{
    if (test->stdout_path == NULL)
        return NULL;
    if (!bytes_read_path(test->stdout_path, expected))
        return "cannot read expected stdout";
    if (!bytes_equal(&result->out, expected))
        return "stdout differs";
    return NULL;
}

static char *absolute_existing(const char *path)
{
    return realpath(path, NULL);
}

static char *fakelsp_beside_yew(const char *yew)
{
    char *dir = strdup(yew);
    char *slash;
    char *path;

    if (dir == NULL)
        return NULL;
    slash = strrchr(dir, '/');
    if (slash == NULL) {
        free(dir);
        return NULL;
    }
    *slash = '\0';
    path = path_join(dir, "tests/helpers/fakelsp");
    free(dir);
    return path;
}

static bool parse_cli(int argc, char **argv, const char **filter,
                      const char **exclude,
                      const char **yew, const char **fakelsp,
                      bool *list, bool *selftest)
{
    int i;

    *filter = NULL;
    *exclude = NULL;
    *yew = "build/yew";
    *fakelsp = NULL;
    *list = false;
    *selftest = false;
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--list") == 0) {
            *list = true;
        } else if (strcmp(argv[i], "--selftest") == 0) {
            *selftest = true;
        } else if (strcmp(argv[i], "--filter") == 0) {
            if (++i >= argc) {
                (void)fprintf(stderr,
                              "script: --filter requires a substring\n");
                return false;
            }
            *filter = argv[i];
        } else if (strcmp(argv[i], "--exclude") == 0) {
            if (++i >= argc) {
                (void)fprintf(stderr,
                              "script: --exclude requires a substring\n");
                return false;
            }
            *exclude = argv[i];
        } else if (strcmp(argv[i], "--yew") == 0) {
            if (++i >= argc) {
                (void)fprintf(stderr, "script: --yew requires a path\n");
                return false;
            }
            *yew = argv[i];
        } else if (strcmp(argv[i], "--fakelsp") == 0) {
            if (++i >= argc) {
                (void)fprintf(stderr,
                              "script: --fakelsp requires a path\n");
                return false;
            }
            *fakelsp = argv[i];
        } else {
            (void)fprintf(stderr, "script: unknown option '%s'\n", argv[i]);
            return false;
        }
    }
    return true;
}

static bool protocol_from_text(const char *text, Protocol *protocol,
                               RunResult *result)
{
    memset(result, 0, sizeof(*result));
    result->waited = true;
    result->status = 0;
    if (!bytes_append(&result->protocol, text, strlen(text)))
        return false;
    *protocol = parse_protocol(&result->protocol);
    return true;
}

static bool selftest_zero_assertions(void)
{
    RunResult result;
    Protocol protocol;
    char reason_buf[64];
    const char *reason;
    bool ok;

    if (!protocol_from_text("YEWTEST\t0\t0\t0\n", &protocol, &result))
        return false;
    reason = failure_reason(&result, &protocol,
                            reason_buf, sizeof(reason_buf));
    ok = protocol.valid && reason != NULL &&
         strcmp(reason, "no assertions") == 0;
    bytes_free(&result.protocol);
    return ok;
}

static bool selftest_exclude_list(void)
{
    return !selected_test("lsp_sync", NULL, "lsp_,ai_") &&
           !selected_test("ai_preset_local", NULL, "lsp_,ai_") &&
           selected_test("file_bytes", NULL, "lsp_,ai_") &&
           selected_test("ai_preset_local", "preset", "lsp_") &&
           !selected_test("ai_preset_local", "status", "lsp_");
}

static bool selftest_skip_report(void)
{
    static const char name[] = "012345678901234567890123456789012345";
    static const char expected[] =
        "SKIP 012345678901234567890123456789012345 (4 assertions)\n";
    RunResult result;
    Protocol protocol;
    char reason_buf[64];
    char line[128];
    const char *reason;
    bool ok;

    if (!protocol_from_text("YEWTEST\t4\t0\t1\n", &protocol, &result))
        return false;
    reason = failure_reason(&result, &protocol,
                            reason_buf, sizeof(reason_buf));
    ok = protocol.valid && protocol.skipped == 1U && reason == NULL &&
         format_count_line(line, sizeof(line), "SKIP", name,
                           protocol.assertions, 0U) &&
         strcmp(line, expected) == 0;
    bytes_free(&result.protocol);
    return ok;
}

static bool selftest_failure_report(void)
{
    static const char text[] =
        "FAIL\tt.text\twant | a\\ngot | b\nYEWTEST\t9\t1\t0\n";
    static const char name[] = "012345678901234567890123456789012345";
    static const char expected[] =
        "FAIL 012345678901234567890123456789012345 "
        "(9 assertions, 1 failure)\n";
    RunResult result;
    Protocol protocol;
    char reason_buf[64];
    char line[160];
    const char *reason;
    bool ok;

    if (!protocol_from_text(text, &protocol, &result))
        return false;
    reason = failure_reason(&result, &protocol,
                            reason_buf, sizeof(reason_buf));
    ok = protocol.valid && protocol.failures == 1U && reason != NULL &&
         strcmp(reason, "assertion failure") == 0 &&
         format_count_line(line, sizeof(line), "FAIL", name,
                           protocol.assertions, protocol.failures) &&
         strcmp(line, expected) == 0;
    bytes_free(&result.protocol);
    return ok;
}

static bool selftest_sandbox_lifecycle(const char *fixtures)
{
    char *root = NULL;
    char *work = NULL;
    struct stat st;
    bool ok;

    if (!make_sandbox(fixtures, &root, &work))
        return false;
    ok = finish_sandbox(root, false) && lstat(root, &st) == 0 &&
         S_ISDIR(st.st_mode) && finish_sandbox(root, true);
    errno = 0;
    ok = ok && lstat(root, &st) != 0 && errno == ENOENT;
    if (!ok)
        (void)remove_tree(root);
    free(work);
    free(root);
    return ok;
}

static bool selftest_result_fd_env(void)
{
    const char *current = getenv("YEW_SCRIPT_RESULT_FD");
    char *saved = current == NULL ? NULL : strdup(current);
    const char *installed;
    bool ok;

    if (current != NULL && saved == NULL)
        return false;
    ok = set_result_fd_environment();
    installed = getenv("YEW_SCRIPT_RESULT_FD");
    ok = ok && installed != NULL && strcmp(installed, "3") == 0;
    if (saved == NULL) {
        if (unsetenv("YEW_SCRIPT_RESULT_FD") != 0)
            ok = false;
    } else if (setenv("YEW_SCRIPT_RESULT_FD", saved, 1) != 0) {
        ok = false;
    }
    free(saved);
    return ok;
}

static bool selftest_zero_filter(void)
{
    TestFile data[] = {
        {(char *)"alpha", (char *)"alpha.fl", NULL, NULL, false},
        {(char *)"beta", (char *)"beta.fl", NULL, NULL, false}
    };
    TestList tests = {data, sizeof(data) / sizeof(data[0]),
                      sizeof(data) / sizeof(data[0])};

    return selected_count(&tests, "no-match", NULL) == 0U &&
           selection_status(selected_count(&tests, "no-match", NULL)) == 1 &&
           selected_count(&tests, "a", NULL) == 2U &&
           selection_status(selected_count(&tests, "a", NULL)) == 0 &&
           selected_count(&tests, NULL, "beta") == 1U &&
           selected_count(&tests, "a", "alpha") == 1U;
}

static bool selftest_stdout_expectation_is_byte_exact(void)
{
    static char expected_data[] = "a\t1\n";
    static char exact_data[] = "a\t1\n";
    static char missing_newline_data[] = "a\t1";
    static char extra_data[] = "a\t1\n\n";
    Bytes expected = {expected_data, sizeof(expected_data) - 1U,
                      sizeof(expected_data), false};
    Bytes exact = {exact_data, sizeof(exact_data) - 1U,
                   sizeof(exact_data), false};
    Bytes missing_newline = {missing_newline_data,
                             sizeof(missing_newline_data) - 1U,
                             sizeof(missing_newline_data), false};
    Bytes extra = {extra_data, sizeof(extra_data) - 1U,
                   sizeof(extra_data), false};

    return bytes_equal(&expected, &exact) &&
           !bytes_equal(&expected, &missing_newline) &&
           !bytes_equal(&expected, &extra);
}

static bool selftest_config_directive(void)
{
    static const char yes_lf[] = "# CONFIG\nprint(1)\n";
    static const char yes_crlf[] = "# CONFIG\r\nprint(1)\r\n";
    static const char no_later[] = "# comment\n# CONFIG\n";
    static const char no_suffix[] = "# CONFIGURE\n";
    char *clean_argv[10];
    char *config_argv[10];
    char *sibling_argv[10];
    char *grant_argv[10];
    size_t clean_argc = build_child_argv(clean_argv, "yew", "test.fl",
                                         false, NULL, false);
    size_t config_argc = build_child_argv(config_argv, "yew", "test.fl",
                                          true, NULL, false);
    size_t sibling_argc = build_child_argv(sibling_argv, "yew", "test.fl",
                                           true, "test.config", false);
    size_t grant_argc = build_child_argv(grant_argv, "yew", "test.fl",
                                         false, NULL, true);

    return config_header_bytes(yes_lf, sizeof(yes_lf) - 1U) &&
           config_header_bytes(yes_crlf, sizeof(yes_crlf) - 1U) &&
           !config_header_bytes(no_later, sizeof(no_later) - 1U) &&
           !config_header_bytes(no_suffix, sizeof(no_suffix) - 1U) &&
           clean_argc == 5U && strcmp(clean_argv[3], "--clean") == 0 &&
           strcmp(clean_argv[4], "test.fl") == 0 &&
           config_argc == 4U && strcmp(config_argv[3], "test.fl") == 0 &&
           config_argv[4] == NULL && sibling_argc == 6U &&
           strcmp(sibling_argv[3], "--config") == 0 &&
           strcmp(sibling_argv[4], "test.config") == 0 &&
           strcmp(sibling_argv[5], "test.fl") == 0 &&
           sibling_argv[6] == NULL && grant_argc == 7U &&
           strcmp(grant_argv[3], "--grant") == 0 &&
           strcmp(grant_argv[4], "session-notes:fs") == 0 &&
           strcmp(grant_argv[5], "--clean") == 0 &&
           strcmp(grant_argv[6], "test.fl") == 0 &&
           grant_argv[7] == NULL;
}

static bool protocol_has_all_negative_assertions(const Bytes *protocol)
{
    static const char *const expected[] = {
        "t.eq", "t.ne", "t.text", "t.line", "t.cursor", "t.cursors",
        "t.sel", "t.reg", "t.undo", "t.file", "t.raises", "t.log"
    };
    bool seen[sizeof(expected) / sizeof(expected[0])] = {false};
    size_t failures = 0U;
    size_t at = 0U;

    while (at < protocol->len) {
        size_t end = at;

        while (end < protocol->len && protocol->data[end] != '\n')
            end++;
        if (end - at >= 6U &&
            memcmp(protocol->data + at, "FAIL\t", 5U) == 0) {
            const char *name = protocol->data + at + 5U;
            const char *tab = memchr(name, '\t', end - at - 5U);
            size_t i;
            bool found = false;

            if (tab == NULL)
                return false;
            for (i = 0U; i < sizeof(expected) / sizeof(expected[0]); i++) {
                size_t n = strlen(expected[i]);

                if ((size_t)(tab - name) == n &&
                    memcmp(name, expected[i], n) == 0) {
                    if (seen[i])
                        return false;
                    seen[i] = true;
                    found = true;
                    break;
                }
            }
            if (!found)
                return false;
            failures++;
        }
        at = end < protocol->len ? end + 1U : end;
    }
    if (failures != sizeof(expected) / sizeof(expected[0]))
        return false;
    for (at = 0U; at < sizeof(expected) / sizeof(expected[0]); at++)
        if (!seen[at])
            return false;
    return true;
}

static bool selftest_negative_assertion_host(const char *yew,
                                             const char *fixtures,
                                             const char *script_dir)
{
    char *meta_dir = path_join(script_dir, "meta");
    char *script = meta_dir == NULL ? NULL :
                   path_join(meta_dir, "assertion_failures.fl");
    TestFile test = {(char *)"assertion_failures", script, NULL, NULL,
                     false};
    RunResult result;
    Protocol protocol = {0};
    char *sandbox = NULL;
    bool attempted = false;
    bool ran = false;
    bool ok = false;

    if (script != NULL) {
        attempted = true;
        ran = run_test(yew, fixtures, NULL, &test, &sandbox, &result);
    }
    if (ran) {
        protocol = parse_protocol(&result.protocol);
        ok = !result.setup_failed && !result.timed_out && result.waited &&
             WIFEXITED(result.status) && WEXITSTATUS(result.status) == 2 &&
             result.out.len == 0U && result.err.len == 0U &&
             sandbox != NULL && protocol.valid &&
             protocol.assertions == 12U && protocol.failures == 12U &&
             protocol.skipped == 0U &&
             protocol_has_all_negative_assertions(&result.protocol);
        if (ok && sandbox != NULL)
            ok = finish_sandbox(sandbox, true);
    }
    if (!ok && sandbox != NULL)
        (void)printf("  negative-host sandbox preserved: %s\n", sandbox);
    if (attempted) {
        bytes_free(&result.out);
        bytes_free(&result.err);
        bytes_free(&result.protocol);
    }
    free(sandbox);
    free(script);
    free(meta_dir);
    return ok;
}

static size_t report_selftest(const char *name, bool ok)
{
    (void)printf("%s %s\n", ok ? "PASS" : "FAIL", name);
    return ok ? 0U : 1U;
}

static int run_selftests(const char *yew, const char *fixtures,
                         const char *script_dir)
{
    size_t failures = 0U;

    failures += report_selftest("zero_assertions_fail",
                                selftest_zero_assertions());
    failures += report_selftest("exclude_list_selects_by_substring",
                                selftest_exclude_list());
    failures += report_selftest("skip_reports_skipped",
                                selftest_skip_report());
    failures += report_selftest("failure_report_is_pinned",
                                selftest_failure_report());
    failures += report_selftest("sandbox_preserve_and_remove",
                                selftest_sandbox_lifecycle(fixtures));
    failures += report_selftest("result_protocol_uses_fd3",
                                selftest_result_fd_env());
    failures += report_selftest("zero_filter_selects_none",
                                selftest_zero_filter());
    failures += report_selftest("config_header_controls_clean",
                                selftest_config_directive());
    failures += report_selftest("stdout_expectation_is_byte_exact",
                                selftest_stdout_expectation_is_byte_exact());
    failures += report_selftest("negative_assertion_host_continues",
                                selftest_negative_assertion_host(
                                    yew, fixtures, script_dir));
    (void)printf("script-runner-selftest: %zu tests, %zu failure%s\n",
                 (size_t)9U, failures,
                 failures == 1U ? "" : "s");
    (void)fflush(stdout);
    return failures == 0U ? 0 : 1;
}

int main(int argc, char **argv)
{
    const char *filter;
    const char *exclude;
    const char *yew_arg;
    const char *fakelsp_arg;
    bool list_only;
    bool selftest;
    char *root;
    char *script_dir;
    char *fixtures;
    char *yew;
    char *fakelsp = NULL;
    TestList tests = {0};
    size_t selected = 0U;
    size_t suite_assertions = 0U;
    size_t suite_failures = 0U;
    size_t suite_skipped = 0U;
    size_t i;

    if (!mark_inherited_fds_cloexec()) {
        (void)fprintf(stderr,
                      "script: cannot isolate inherited descriptors: %s\n",
                      strerror(errno));
        return 1;
    }
    if (!parse_cli(argc, argv, &filter, &exclude, &yew_arg, &fakelsp_arg,
                   &list_only, &selftest))
        return 1;
    root = getcwd(NULL, 0U);
    script_dir = root == NULL ? NULL : path_join(root, "tests/script");
    fixtures = script_dir == NULL ? NULL : path_join(script_dir, "fixtures");
    if (root == NULL || script_dir == NULL || fixtures == NULL) {
        (void)fprintf(stderr, "script: cannot locate tests/script: %s\n",
                      strerror(errno));
        free(root);
        free(script_dir);
        free(fixtures);
        return 1;
    }
    if (selftest) {
        int rc;

        yew = absolute_existing(yew_arg);
        if (yew == NULL) {
            (void)fprintf(stderr,
                          "script: cannot resolve yew '%s': %s\n",
                          yew_arg, strerror(errno));
            free(root);
            free(script_dir);
            free(fixtures);
            return 1;
        }
        rc = run_selftests(yew, fixtures, script_dir);

        free(yew);
        free(root);
        free(script_dir);
        free(fixtures);
        return rc;
    }
    if (!discover(script_dir, &tests)) {
        (void)fprintf(stderr, "script: cannot discover tests/script: %s\n",
                      strerror(errno));
        free(root);
        free(script_dir);
        free(fixtures);
        list_free(&tests);
        return 1;
    }
    selected = selected_count(&tests, filter, exclude);
    if (selection_status(selected) != 0) {
        (void)fprintf(stderr, "script: filter matched zero tests\n");
        free(root);
        free(script_dir);
        free(fixtures);
        list_free(&tests);
        return selection_status(selected);
    }
    if (list_only) {
        for (i = 0U; i < tests.len; i++)
            if (selected_test(tests.data[i].name, filter, exclude))
                (void)printf("%s\n", tests.data[i].name);
        free(root);
        free(script_dir);
        free(fixtures);
        list_free(&tests);
        return 0;
    }
    yew = absolute_existing(yew_arg);
    if (yew == NULL) {
        (void)fprintf(stderr, "script: cannot resolve yew '%s': %s\n",
                      yew_arg, strerror(errno));
        free(root);
        free(script_dir);
        free(fixtures);
        list_free(&tests);
        return 1;
    }
    for (i = 0U; i < tests.len; i++) {
        if (selected_test(tests.data[i].name, filter, exclude) &&
            tests.data[i].config_path != NULL) {
            char *derived = fakelsp_arg == NULL ? fakelsp_beside_yew(yew) :
                            NULL;
            const char *requested = fakelsp_arg == NULL ? derived :
                                    fakelsp_arg;

            fakelsp = requested == NULL ? NULL :
                      absolute_existing(requested);
            if (fakelsp == NULL) {
                (void)fprintf(stderr,
                              "script: cannot resolve fakelsp '%s': %s\n",
                              requested == NULL ? "(beside yew)" : requested,
                              strerror(errno));
                free(derived);
                free(yew);
                free(root);
                free(script_dir);
                free(fixtures);
                list_free(&tests);
                return 1;
            }
            free(derived);
            break;
        }
    }
    if (signal(SIGPIPE, SIG_IGN) == SIG_ERR) {
        (void)fprintf(stderr, "script: cannot ignore SIGPIPE: %s\n",
                      strerror(errno));
        free(yew);
        free(fakelsp);
        free(root);
        free(script_dir);
        free(fixtures);
        list_free(&tests);
        return 1;
    }
    for (i = 0U; i < tests.len; i++) {
        RunResult result;
        Protocol protocol;
        Bytes expected_stdout = {0};
        char *sandbox = NULL;
        char reason_buf[64];
        char count_line[512];
        const char *reason;
        bool stdout_mismatch = false;

        if (!selected_test(tests.data[i].name, filter, exclude))
            continue;
        (void)run_test(yew, fixtures, fakelsp, &tests.data[i], &sandbox,
                       &result);
        protocol = parse_protocol(&result.protocol);
        if (protocol.valid)
            suite_assertions += protocol.assertions;
        reason = failure_reason(&result, &protocol,
                                reason_buf, sizeof(reason_buf));
        if (reason == NULL) {
            reason = expected_stdout_failure(&tests.data[i], &result,
                                             &expected_stdout);
            stdout_mismatch = reason != NULL &&
                              strcmp(reason, "stdout differs") == 0;
        }
        if (reason == NULL && protocol.skipped != 0U) {
            if (format_count_line(count_line, sizeof(count_line), "SKIP",
                                  tests.data[i].name,
                                  protocol.assertions, 0U))
                (void)fputs(count_line, stdout);
            suite_skipped++;
            if (sandbox != NULL && !finish_sandbox(sandbox, true)) {
                (void)printf("FAIL %-36s (cannot remove sandbox)\n",
                             tests.data[i].name);
                (void)printf("  sandbox preserved: %s\n", sandbox);
                suite_failures++;
            }
        } else if (reason == NULL) {
            if (format_count_line(count_line, sizeof(count_line), "PASS",
                                  tests.data[i].name,
                                  protocol.assertions, 0U))
                (void)fputs(count_line, stdout);
            if (sandbox != NULL && !finish_sandbox(sandbox, true)) {
                (void)printf("FAIL %-36s (cannot remove sandbox)\n",
                             tests.data[i].name);
                (void)printf("  sandbox preserved: %s\n", sandbox);
                suite_failures++;
            }
        } else {
            if (protocol.valid && protocol.failures != 0U) {
                if (format_count_line(count_line, sizeof(count_line), "FAIL",
                                      tests.data[i].name,
                                      protocol.assertions,
                                      protocol.failures))
                    (void)fputs(count_line, stdout);
                print_protocol_failures(&result.protocol);
            } else {
                (void)printf("FAIL %-36s (%s)\n",
                             tests.data[i].name, reason);
            }
            print_capture("stdout", &result.out);
            if (stdout_mismatch)
                print_capture("expected stdout", &expected_stdout);
            print_capture("stderr", &result.err);
            if (!protocol.valid)
                print_capture("protocol", &result.protocol);
            if (sandbox != NULL)
                (void)printf("  sandbox preserved: %s\n", sandbox);
            suite_failures++;
        }
        free(sandbox);
        bytes_free(&result.out);
        bytes_free(&result.err);
        bytes_free(&result.protocol);
        bytes_free(&expected_stdout);
        (void)fflush(stdout);
    }
    if (filter == NULL && (selected < 40U || suite_assertions < 400U)) {
        (void)printf("FAIL script_corpus_floor                  "
                     "(need >= 40 tests and >= 400 assertions; "
                     "got %zu and %zu)\n",
                     selected, suite_assertions);
        suite_failures++;
    }
    (void)printf("script: %zu tests, %zu assertions, %zu failure%s, "
                 "%zu skipped\n",
                 selected, suite_assertions, suite_failures,
                 suite_failures == 1U ? "" : "s", suite_skipped);
    (void)fflush(stdout);
    free(yew);
    free(fakelsp);
    free(root);
    free(script_dir);
    free(fixtures);
    list_free(&tests);
    return suite_failures == 0U ? 0 : 1;
}
