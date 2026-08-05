#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700

#include <dirent.h>
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

static const unsigned char old_bytes[] =
    "Sagitta torture fixture: before save\nline two\n";
static const unsigned char post_bytes[] =
    "Sagitta torture fixture: AFTER save\n"
    "a substantially longer replacement line that forces short writes\n"
    "final line without a newline";

static void case_paths(char *dst, size_t dst_n, char *old, size_t old_n,
                       char *post, size_t post_n, char *log, size_t log_n,
                       const char *root, const char *lane,
                       unsigned long long serial);

static void die(const char *what)
{
    perror(what);
    exit(2);
}

static bool remove_tree(const char *path)
{
    struct stat st;

    if (lstat(path, &st) != 0)
        return errno == ENOENT;
    if (S_ISDIR(st.st_mode)) {
        struct dirent *ent;
        DIR *dir = opendir(path);

        if (dir == NULL)
            return false;
        while ((ent = readdir(dir)) != NULL) {
            char child[1024];
            int n;

            if (strcmp(ent->d_name, ".") == 0 ||
                strcmp(ent->d_name, "..") == 0)
                continue;
            n = snprintf(child, sizeof(child), "%s/%s", path, ent->d_name);
            if (n < 0 || (size_t)n >= sizeof(child) ||
                !remove_tree(child)) {
                (void)closedir(dir);
                return false;
            }
        }
        if (closedir(dir) != 0)
            return false;
        return rmdir(path) == 0;
    }
    return unlink(path) == 0;
}

static void write_all(int fd, const void *bytes, size_t len)
{
    const unsigned char *p = bytes;

    while (len != 0U) {
        ssize_t n = write(fd, p, len);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            die("write");
        p += (size_t)n;
        len -= (size_t)n;
    }
}

static void make_file(const char *path, const unsigned char *bytes, size_t len)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);

    if (fd < 0)
        die(path);
    write_all(fd, bytes, len);
    if (close(fd) != 0)
        die("close fixture");
}

static bool read_file(const char *path, unsigned char **out, size_t *out_len)
{
    struct stat st;
    unsigned char *bytes;
    size_t off = 0U;
    int fd = open(path, O_RDONLY);

    *out = NULL;
    *out_len = 0U;
    if (fd < 0 || fstat(fd, &st) != 0 || st.st_size < 0 ||
        (uintmax_t)st.st_size > SIZE_MAX) {
        if (fd >= 0)
            (void)close(fd);
        return false;
    }
    bytes = malloc(st.st_size == 0 ? 1U : (size_t)st.st_size);
    if (bytes == NULL) {
        (void)close(fd);
        return false;
    }
    while (off < (size_t)st.st_size) {
        ssize_t n = read(fd, bytes + off, (size_t)st.st_size - off);

        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0) {
            free(bytes);
            (void)close(fd);
            return false;
        }
        off += (size_t)n;
    }
    if (close(fd) != 0) {
        free(bytes);
        return false;
    }
    *out = bytes;
    *out_len = off;
    return true;
}

static int wait_child(pid_t pid)
{
    int status;

    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR)
            die("waitpid");
    }
    if (WIFEXITED(status))
        return WEXITSTATUS(status);
    if (WIFSIGNALED(status))
        return 128 + WTERMSIG(status);
    return 255;
}

static int wait_child_status(pid_t pid, int *status)
{
    pid_t waited;

    do {
        waited = waitpid(pid, status, 0);
    } while (waited < 0 && errno == EINTR);
    if (waited < 0)
        die("waitpid");
    return WIFEXITED(*status) ? WEXITSTATUS(*status)
                              : WIFSIGNALED(*status)
                                    ? 128 + WTERMSIG(*status)
                                    : 255;
}

static void set_num_env(const char *name, unsigned long long value)
{
    char buf[32];

    (void)snprintf(buf, sizeof(buf), "%llu", value);
    if (setenv(name, buf, 1) != 0)
        die("setenv");
}

static pid_t start_save(const char *driver, const char *shim,
                        const char *dst, const char *post,
                        const char *log, unsigned long long seed,
                        long fault_at, int ready_fd)
{
    pid_t pid = fork();

    if (pid < 0)
        die("fork");
    if (pid != 0)
        return pid;
    if (getenv("SAG_TORTURE_NO_SHIM") == NULL) {
        const char *prefix = getenv("SAG_TORTURE_PRELOAD_PREFIX");
        char *preload = NULL;

#ifdef SAG_ASAN_RUNTIME
        /* Under ASan the runtime has to be preloaded before the shim, or
         * it refuses to install interceptors and the shim never fires.
         * An explicit prefix in the environment still wins. */
        if (prefix == NULL || *prefix == '\0')
            prefix = SAG_ASAN_RUNTIME;
#endif

        if (prefix != NULL && *prefix != '\0') {
            size_t n = strlen(prefix) + 1U + strlen(shim) + 1U;

            preload = malloc(n);
            if (preload == NULL)
                _exit(126);
            (void)snprintf(preload, n, "%s:%s", prefix, shim);
        }
#if defined(__APPLE__)
        if (setenv("DYLD_INSERT_LIBRARIES",
                   preload != NULL ? preload : shim, 1) != 0 ||
            setenv("DYLD_FORCE_FLAT_NAMESPACE", "1", 1) != 0)
#else
        if (setenv("LD_PRELOAD", preload != NULL ? preload : shim, 1) != 0)
#endif
            _exit(126);
        free(preload);
    }
    if (
        setenv("SAG_FAULT_ENABLE", "0", 1) != 0 ||
        setenv("SAG_FAULT_SHORT", "1", 1) != 0 ||
        setenv("SAG_FAULT_LOG", log, 1) != 0)
        _exit(126);
    set_num_env("SAG_FAULT_SEED", seed);
    if (fault_at >= 0)
        set_num_env("SAG_FAULT_AT", (unsigned long long)fault_at);
    else
        (void)unsetenv("SAG_FAULT_AT");
    if (ready_fd >= 0)
        set_num_env("SAG_TORTURE_READY_FD", (unsigned long long)ready_fd);
    else
        (void)unsetenv("SAG_TORTURE_READY_FD");
    execl(driver, driver, "--save", dst, post, (char *)NULL);
    _exit(126);
}

static bool run_check(const char *driver, const char *dst,
                      const char *old, const char *post)
{
    pid_t pid = fork();

    if (pid < 0)
        die("fork checker");
    if (pid == 0) {
        (void)unsetenv("LD_PRELOAD");
        (void)unsetenv("DYLD_INSERT_LIBRARIES");
        (void)unsetenv("DYLD_FORCE_FLAT_NAMESPACE");
        (void)unsetenv("SAG_FAULT_AT");
        (void)unsetenv("SAG_FAULT_SHORT");
        (void)unsetenv("SAG_FAULT_ENABLE");
        execl(driver, driver, "--check", dst, old, post, (char *)NULL);
        _exit(126);
    }
    return wait_child(pid) == 0;
}

static bool file_equals_bytes(const char *path, const unsigned char *want,
                              size_t want_len)
{
    unsigned char buf[4096];
    size_t off = 0U;
    int fd = open(path, O_RDONLY);

    if (fd < 0)
        return false;
    for (;;) {
        ssize_t n = read(fd, buf, sizeof(buf));
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0 || off + (size_t)n > want_len ||
            (n > 0 && memcmp(buf, want + off, (size_t)n) != 0)) {
            (void)close(fd);
            return false;
        }
        if (n == 0)
            break;
        off += (size_t)n;
    }
    (void)close(fd);
    return off == want_len;
}

static bool files_equal(const char *a, const char *b)
{
    struct stat sa;
    struct stat sb;
    unsigned char ba[4096];
    unsigned char bb[4096];
    int fa;
    int fb;
    bool equal = true;

    if (stat(a, &sa) != 0 || stat(b, &sb) != 0 || sa.st_size != sb.st_size)
        return false;
    fa = open(a, O_RDONLY);
    fb = open(b, O_RDONLY);
    if (fa < 0 || fb < 0) {
        if (fa >= 0)
            (void)close(fa);
        if (fb >= 0)
            (void)close(fb);
        return false;
    }
    for (;;) {
        ssize_t na = read(fa, ba, sizeof(ba));
        ssize_t nb = read(fb, bb, sizeof(bb));

        if (na < 0 && errno == EINTR)
            continue;
        if (na != nb || na < 0 ||
            (na > 0 && memcmp(ba, bb, (size_t)na) != 0)) {
            equal = false;
            break;
        }
        if (na == 0)
            break;
    }
    (void)close(fa);
    (void)close(fb);
    return equal;
}

static bool atomic_log_order(const char *path)
{
    unsigned char *bytes;
    size_t len;
    char *text;
    char *line;
    char *save = NULL;
    unsigned state = 0U;
    bool ok = true;

    if (!read_file(path, &bytes, &len))
        return false;
    text = malloc(len + 1U);
    if (text == NULL) {
        free(bytes);
        return false;
    }
    (void)memcpy(text, bytes, len);
    text[len] = '\0';
    for (line = strtok_r(text, "\n", &save); line != NULL && state < 4U;
         line = strtok_r(NULL, "\n", &save)) {
        unsigned long long call;
        char name[32];
        char action[32];

        if (sscanf(line, "%llu %31s %31s", &call, name, action) != 3) {
            ok = false;
            break;
        }
        (void)call;
        if (strcmp(action, "short") == 0)
            continue;
        if (strcmp(action, "pass") != 0) {
            ok = false;
            break;
        }
        if (state == 0U) {
            if (strcmp(name, "write") != 0) {
                ok = false;
                break;
            }
            state = 1U;
        } else if (state == 1U) {
            if (strcmp(name, "write") == 0)
                continue;
            if (strcmp(name, "fsync-file") != 0) {
                ok = false;
                break;
            }
            state = 2U;
        } else if (state == 2U) {
            if (strcmp(name, "fchown") == 0 || strcmp(name, "close") == 0)
                continue;
            if (strcmp(name, "rename") != 0) {
                ok = false;
                break;
            }
            state = 3U;
        } else {
            if (strcmp(name, "fsync-dir") != 0) {
                ok = false;
                break;
            }
            state = 4U;
        }
    }
    ok = ok && state == 4U;
    free(text);
    free(bytes);
    return ok;
}

static uint64_t path_hash(const char *text)
{
    uint64_t hash = UINT64_C(14695981039346656037);

    while (*text != '\0') {
        hash ^= (unsigned char)*text++;
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

static bool backup_matches(const char *state, const char *dst)
{
    char *resolved;
    char path[1536];
    struct stat st;
    int n;
    bool matches;

    resolved = realpath(dst, NULL);
    if (resolved == NULL)
        return false;
    n = snprintf(path, sizeof(path), "%s/sagitta/backup/%016llx.bak",
                 state, (unsigned long long)path_hash(resolved));
    free(resolved);
    matches = n > 0 && (size_t)n < sizeof(path) &&
              lstat(path, &st) == 0 && S_ISREG(st.st_mode) &&
              file_equals_bytes(path, old_bytes, sizeof(old_bytes) - 1U);
    return matches;
}

static bool same_inode(const char *a, const char *b)
{
    struct stat sa;
    struct stat sb;

    return stat(a, &sa) == 0 && stat(b, &sb) == 0 &&
           sa.st_dev == sb.st_dev && sa.st_ino == sb.st_ino;
}

static void hardlink_sweep(const char *driver, const char *shim,
                           const char *root, const char *state,
                           unsigned long long *serial)
{
    unsigned long long at;

    for (at = 0U; at < 4096U; at++) {
        char dst[512], old[512], post[512], log[512], twin[768];
        bool old_or_new;
        int code;

        case_paths(dst, sizeof(dst), old, sizeof(old), post, sizeof(post),
                   log, sizeof(log), root, "hardlink", (*serial)++);
        (void)snprintf(twin, sizeof(twin), "%s.twin", dst);
        make_file(dst, old_bytes, sizeof(old_bytes) - 1U);
        if (link(dst, twin) != 0)
            die("link hardlink fixture");
        make_file(old, old_bytes, sizeof(old_bytes) - 1U);
        make_file(post, post_bytes, sizeof(post_bytes) - 1U);
        code = wait_child(start_save(driver, shim, dst, post, log, 42U,
                                     (long)at, -1));
        old_or_new = run_check(driver, dst, old, post);
        if (!same_inode(dst, twin) ||
            (!old_or_new && !backup_matches(state, dst))) {
            (void)fprintf(stderr,
                          "torture: hardlink invariant failed at=%llu\n", at);
            exit(1);
        }
        if (code == 0) {
            if (!file_equals_bytes(dst, post_bytes, sizeof(post_bytes) - 1U)) {
                (void)fprintf(stderr,
                              "torture: successful hardlink save is not new\n");
                exit(1);
            }
            (void)printf("hardlink syscalls=%llu ok\n", at);
            return;
        }
        if (code != 137) {
            (void)fprintf(stderr, "torture: hardlink child exit %d at=%llu\n",
                          code, at);
            exit(1);
        }
    }
    (void)fprintf(stderr, "torture: hardlink sweep exceeded safety bound\n");
    exit(1);
}

static void determinism_check(const char *driver, const char *shim,
                              const char *root, unsigned long long *serial)
{
    char first_log[512];
    unsigned run;

    first_log[0] = '\0';
    for (run = 0U; run < 2U; run++) {
        char dst[512], old[512], post[512], log[512];

        case_paths(dst, sizeof(dst), old, sizeof(old), post, sizeof(post),
                   log, sizeof(log), root, "determinism", (*serial)++);
        make_file(dst, old_bytes, sizeof(old_bytes) - 1U);
        make_file(old, old_bytes, sizeof(old_bytes) - 1U);
        make_file(post, post_bytes, sizeof(post_bytes) - 1U);
        if (wait_child(start_save(driver, shim, dst, post, log, 424242U,
                                  -1, -1)) != 0 ||
            !run_check(driver, dst, old, post) || !atomic_log_order(log)) {
            (void)fprintf(stderr, "torture: determinism run failed\n");
            exit(1);
        }
        if (run == 0U) {
            (void)snprintf(first_log, sizeof(first_log), "%s", log);
        } else if (!files_equal(first_log, log)) {
            (void)fprintf(stderr,
                          "torture: identical seed produced different log\n");
            exit(1);
        }
    }
    (void)printf("determinism seed=424242 ok\n");
}

static void injected_fallback(const char *driver, const char *shim,
                              const char *root, const char *state,
                              const char *kind, unsigned long long *serial)
{
    char dst[512], old[512], post[512], log[512];
    bool chown_case = strcmp(kind, "fchown") == 0;
    int code;

    case_paths(dst, sizeof(dst), old, sizeof(old), post, sizeof(post),
               log, sizeof(log), root, kind, (*serial)++);
    make_file(dst, old_bytes, sizeof(old_bytes) - 1U);
    make_file(old, old_bytes, sizeof(old_bytes) - 1U);
    make_file(post, post_bytes, sizeof(post_bytes) - 1U);
    if (setenv(chown_case ? "SAG_FAULT_FCHOWN_EPERM"
                          : "SAG_FAULT_RENAME_EXDEV", "1", 1) != 0)
        die("setenv fallback fault");
    if (chown_case && setenv("SAG_TORTURE_FOREIGN_OWNER", "1", 1) != 0)
        die("setenv foreign owner");
    code = wait_child(start_save(driver, shim, dst, post, log, 99U, -1, -1));
    (void)unsetenv("SAG_FAULT_FCHOWN_EPERM");
    (void)unsetenv("SAG_FAULT_RENAME_EXDEV");
    (void)unsetenv("SAG_TORTURE_FOREIGN_OWNER");
    if (code != 0 || !run_check(driver, dst, old, post) ||
        !backup_matches(state, dst)) {
        (void)fprintf(stderr, "torture: injected %s fallback failed\n", kind);
        exit(1);
    }
    (void)printf("fallback %s ok\n", kind);
}

static void late_hardlink_fallback(const char *driver, const char *shim,
                                   const char *root, const char *state,
                                   unsigned long long *serial)
{
    char dst[512], old[512], post[512], log[512], twin[768];
    int code;

    case_paths(dst, sizeof(dst), old, sizeof(old), post, sizeof(post),
               log, sizeof(log), root, "late-hardlink", (*serial)++);
    (void)snprintf(twin, sizeof(twin), "%s.twin", dst);
    make_file(dst, old_bytes, sizeof(old_bytes) - 1U);
    make_file(old, old_bytes, sizeof(old_bytes) - 1U);
    make_file(post, post_bytes, sizeof(post_bytes) - 1U);
    if (setenv("SAG_FAULT_LINK_SOURCE", dst, 1) != 0 ||
        setenv("SAG_FAULT_LINK_TWIN", twin, 1) != 0)
        die("setenv late hardlink");
    code = wait_child(start_save(driver, shim, dst, post, log, 314159U,
                                 -1, -1));
    (void)unsetenv("SAG_FAULT_LINK_SOURCE");
    (void)unsetenv("SAG_FAULT_LINK_TWIN");
    if (code != 0 || !same_inode(dst, twin) ||
        !file_equals_bytes(dst, post_bytes, sizeof(post_bytes) - 1U) ||
        !file_equals_bytes(twin, post_bytes, sizeof(post_bytes) - 1U) ||
        !backup_matches(state, dst)) {
        (void)fprintf(stderr, "torture: late hardlink fallback failed\n");
        exit(1);
    }
    (void)printf("fallback late-hardlink ok\n");
}

static void injected_eintr(const char *driver, const char *shim,
                           const char *root, unsigned long long *serial)
{
    char dst[512], old[512], post[512], log[512];
    int code;

    case_paths(dst, sizeof(dst), old, sizeof(old), post, sizeof(post),
               log, sizeof(log), root, "eintr", (*serial)++);
    make_file(dst, old_bytes, sizeof(old_bytes) - 1U);
    make_file(old, old_bytes, sizeof(old_bytes) - 1U);
    make_file(post, post_bytes, sizeof(post_bytes) - 1U);
    if (setenv("SAG_FAULT_EINTR_AT", "0", 1) != 0)
        die("setenv EINTR fault");
    code = wait_child(start_save(driver, shim, dst, post, log, 17U, -1, -1));
    (void)unsetenv("SAG_FAULT_EINTR_AT");
    if (code != 0 || !run_check(driver, dst, old, post)) {
        (void)fprintf(stderr, "torture: EINTR retry lane failed\n");
        exit(1);
    }
    (void)printf("retry EINTR ok\n");
}

static void clean_child_check(const char *driver, const char *shim,
                              const char *root, unsigned long long *serial)
{
    char dst[512], old[512], post[512], log[512];
    int code;

    case_paths(dst, sizeof(dst), old, sizeof(old), post, sizeof(post),
               log, sizeof(log), root, "clean", (*serial)++);
    make_file(dst, old_bytes, sizeof(old_bytes) - 1U);
    make_file(old, old_bytes, sizeof(old_bytes) - 1U);
    make_file(post, post_bytes, sizeof(post_bytes) - 1U);
    if (setenv("SAG_TORTURE_NO_SHIM", "1", 1) != 0)
        die("setenv no shim");
    code = wait_child(start_save(driver, shim, dst, post, log, 1U, -1, -1));
    (void)unsetenv("SAG_TORTURE_NO_SHIM");
    {
        bool oracle = run_check(driver, dst, old, post);
        bool exact = file_equals_bytes(dst, post_bytes,
                                       sizeof(post_bytes) - 1U);

        if (code != 0 || !oracle || !exact) {
            (void)fprintf(stderr,
                          "torture: clean child check failed "
                          "(exit=%d oracle=%d exact=%d)\n",
                          code, oracle, exact);
            exit(1);
        }
    }
    (void)printf("clean child save+check ok\n");
}

static void case_paths(char *dst, size_t dst_n, char *old, size_t old_n,
                       char *post, size_t post_n, char *log, size_t log_n,
                       const char *root, const char *lane,
                       unsigned long long serial)
{
    (void)snprintf(dst, dst_n, "%s/%s-%llu.txt", root, lane, serial);
    (void)snprintf(old, old_n, "%s/%s-%llu.old", root, lane, serial);
    (void)snprintf(post, post_n, "%s/%s-%llu.new", root, lane, serial);
    (void)snprintf(log, log_n, "%s/%s-%llu.log", root, lane, serial);
}

static unsigned long long atomic_sweep(const char *driver, const char *shim,
                                       const char *root,
                                       unsigned long long seed,
                                       unsigned long long *serial)
{
    unsigned long long at;

    for (at = 0U; at < 4096U; at++) {
        char dst[512], old[512], post[512], log[512];
        int code;

        case_paths(dst, sizeof(dst), old, sizeof(old), post, sizeof(post),
                   log, sizeof(log), root, "atomic", (*serial)++);
        make_file(dst, old_bytes, sizeof(old_bytes) - 1U);
        make_file(old, old_bytes, sizeof(old_bytes) - 1U);
        make_file(post, post_bytes, sizeof(post_bytes) - 1U);
        code = wait_child(start_save(driver, shim, dst, post, log, seed,
                                     (long)at, -1));
        if (!run_check(driver, dst, old, post)) {
            (void)fprintf(stderr,
                          "torture: atomic invariant failed seed=%llu at=%llu\n",
                          seed, at);
            exit(1);
        }
        if (code == 0)
            return at;
        if (code != 137) {
            (void)fprintf(stderr,
                          "torture: child exit %d seed=%llu at=%llu\n",
                          code, seed, at);
            exit(1);
        }
    }
    (void)fprintf(stderr, "torture: syscall sweep exceeded safety bound\n");
    exit(1);
}

static uint64_t random_next(uint64_t *state)
{
    uint64_t x = *state;

    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    *state = x;
    return x;
}

static void external_kills(const char *driver, const char *shim,
                           const char *root, unsigned long long count,
                           unsigned long long *serial)
{
    uint64_t rng = UINT64_C(0x7361676974746108);
    unsigned long long killed = 0U;
    unsigned long long attempts = 0U;
    unsigned long long safety = count > (UINT64_MAX - 100U) / 10U
                                    ? UINT64_MAX
                                    : count * 10U + 100U;

    while (killed < count && attempts < safety) {
        char dst[512], old[512], post[512], log[512];
        char ready;
        int pipefd[2];
        int status;
        pid_t pid;
        struct timespec delay;
        ssize_t n;

        attempts++;
        case_paths(dst, sizeof(dst), old, sizeof(old), post, sizeof(post),
                   log, sizeof(log), root, "signal", (*serial)++);
        make_file(dst, old_bytes, sizeof(old_bytes) - 1U);
        make_file(old, old_bytes, sizeof(old_bytes) - 1U);
        make_file(post, post_bytes, sizeof(post_bytes) - 1U);
        if (pipe(pipefd) != 0)
            die("pipe");
        if (setenv("SAG_FAULT_DELAY_US", "5000", 1) != 0)
            die("setenv fault delay");
        pid = start_save(driver, shim, dst, post, log, random_next(&rng),
                         -1, pipefd[1]);
        (void)unsetenv("SAG_FAULT_DELAY_US");
        (void)close(pipefd[1]);
        do {
            n = read(pipefd[0], &ready, 1U);
        } while (n < 0 && errno == EINTR);
        (void)close(pipefd[0]);
        if (n == 1) {
            delay.tv_sec = 0;
            delay.tv_nsec = (long)(random_next(&rng) % UINT64_C(20000001));
            while (nanosleep(&delay, &delay) != 0 && errno == EINTR)
                ;
            if (kill(pid, SIGKILL) != 0 && errno != ESRCH)
                die("kill");
        }
        (void)wait_child_status(pid, &status);
        if (!run_check(driver, dst, old, post)) {
            (void)fprintf(stderr,
                          "torture: external SIGKILL invariant failed at=%llu\n",
                          killed);
            exit(1);
        }
        if (WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL)
            killed++;
    }
    if (killed != count) {
        (void)fprintf(stderr,
                      "torture: only %llu/%llu children received SIGKILL\n",
                      killed, count);
        exit(1);
    }
    (void)printf("external-sigkill iterations=%llu attempts=%llu ok\n",
                 count, attempts);
}

int main(int argc, char **argv)
{
    static const unsigned long long seeds[] = {1U, 7U, 42U, 8675309U};
    char root_template[] = "/tmp/sagitta-torture-XXXXXX";
    char state[512];
    char *root;
    const char *iterations_env;
    unsigned long long signal_iterations = 500U;
    unsigned long long serial = 0U;
    bool fault_boundaries = true;
    bool live_editor = false;
    size_t i;

    if (argc != 3) {
        (void)fprintf(stderr, "usage: %s SAG_TORTURE FAULTSHIM_SO\n", argv[0]);
        return 2;
    }
    live_editor = getenv("SAG_TORTURE_LANE") != NULL &&
                  strcmp(getenv("SAG_TORTURE_LANE"), "live-editor") == 0;
    root = mkdtemp(root_template);
    if (root == NULL)
        die("mkdtemp");
    (void)snprintf(state, sizeof(state), "%s/state", root);
    if (mkdir(state, 0700) != 0)
        die("mkdir state");
    if (setenv("XDG_STATE_HOME", state, 1) != 0)
        die("setenv XDG_STATE_HOME");
    iterations_env = getenv("SAG_TORTURE_SIGKILL_ITERS");
    if (iterations_env != NULL)
        signal_iterations = strtoull(iterations_env, NULL, 10);
    if (getenv("SAG_TORTURE_CLEAN_ONLY") != NULL &&
        strcmp(getenv("SAG_TORTURE_CLEAN_ONLY"), "1") == 0) {
        clean_child_check(argv[1], argv[2], root, &serial);
        if (!remove_tree(root))
            die("remove torture root");
        return 0;
    }
    if (getenv("SAG_TORTURE_FAULT_BOUNDARIES") != NULL &&
        strcmp(getenv("SAG_TORTURE_FAULT_BOUNDARIES"), "0") == 0)
        fault_boundaries = false;

    if (fault_boundaries) {
        for (i = 0U; i < sizeof(seeds) / sizeof(seeds[0]); i++) {
            unsigned long long calls = atomic_sweep(argv[1], argv[2], root,
                                                    seeds[i], &serial);
            (void)printf("atomic seed=%llu syscalls=%llu ok\n", seeds[i],
                         calls);
        }
        hardlink_sweep(argv[1], argv[2], root, state, &serial);
    }
    /* These decision-table fallbacks need synthetic metadata or a fault at a
     * helper-internal close boundary. Keep them in the API lane; the live
     * editor lane exercises the atomic sweep, retries, determinism, and
     * external SIGKILL against the real process. */
    if (!live_editor) {
        late_hardlink_fallback(argv[1], argv[2], root, state, &serial);
        injected_fallback(argv[1], argv[2], root, state, "rename-exdev",
                          &serial);
        injected_fallback(argv[1], argv[2], root, state, "fchown", &serial);
    }
    injected_eintr(argv[1], argv[2], root, &serial);
    determinism_check(argv[1], argv[2], root, &serial);
    external_kills(argv[1], argv[2], root, signal_iterations, &serial);
    if (getenv("SAG_TORTURE_KEEP") != NULL)
        (void)printf("torture root retained for inspection: %s\n", root);
    else if (!remove_tree(root))
        die("remove torture root");
    return 0;
}
