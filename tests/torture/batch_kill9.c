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

enum {
    DEFAULT_ITERATIONS = 200,
    DEFAULT_MAX_DELAY_US = 50000,
    PAYLOAD_DOUBLINGS = 10,
    SAVE_PASSES = 128
};

static const unsigned char old_bytes[] =
    "Sagitta batch kill9 fixture: old bytes\n";
static const unsigned char payload_seed[] =
    "Sagitta batch kill9 fixture: replacement bytes 0123456789abcdef\n";

static void die(const char *what)
{
    perror(what);
    exit(2);
}

static void fail_root(const char *root, const char *fmt,
                      unsigned long long trial, size_t size)
{
    (void)fprintf(stderr, fmt, trial, size);
    (void)fprintf(stderr, "batch-kill9: retained failure root: %s\n", root);
    exit(1);
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

static void write_all(int fd, const void *data, size_t len)
{
    const unsigned char *p = data;

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

static void make_file(const char *path, const void *data, size_t len)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);

    if (fd < 0)
        die(path);
    write_all(fd, data, len);
    if (fsync(fd) != 0 || close(fd) != 0)
        die("persist fixture");
}

static bool read_file(const char *path, unsigned char **out, size_t *out_len)
{
    struct stat st;
    unsigned char *bytes;
    size_t off = 0U;
    int fd;

    *out = NULL;
    *out_len = 0U;
    fd = open(path, O_RDONLY);
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

static uint64_t random_next(uint64_t *state)
{
    uint64_t x = *state;

    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    *state = x;
    return x * UINT64_C(2685821657736338717);
}

static unsigned long long parse_count(const char *name, const char *text,
                                      unsigned long long min)
{
    unsigned long long value;
    char *end;

    errno = 0;
    value = strtoull(text, &end, 0);
    if (errno != 0 || end == text || *end != '\0' || value < min) {
        (void)fprintf(stderr, "batch-kill9: invalid %s '%s' (minimum %llu)\n",
                      name, text, min);
        exit(2);
    }
    return value;
}

static unsigned char *make_payload(size_t *len)
{
    size_t seed_len = sizeof(payload_seed) - 1U;
    size_t n = seed_len << PAYLOAD_DOUBLINGS;
    unsigned char *bytes = malloc(n);
    size_t written = seed_len;

    if (bytes == NULL)
        die("malloc payload");
    (void)memcpy(bytes, payload_seed, seed_len);
    while (written < n) {
        (void)memcpy(bytes + written, bytes, written);
        written *= 2U;
    }
    *len = n;
    return bytes;
}

static void write_scripts(const char *save_path, const char *open_path)
{
    static const char save_script[] =
        "import buf\n"
        "import span\n"
        "let b = buf.current()\n"
        "let payload = \"Sagitta batch kill9 fixture: replacement bytes "
        "0123456789abcdef\\n\"\n"
        "let d = 0\n"
        "while d < 10 {\n"
        "    payload = payload + payload\n"
        "    d = d + 1\n"
        "}\n"
        "let i = 0\n"
        "while i < 128 {\n"
        "    edit {\n"
        "        let whole = buf.span(b, 0, buf.len(b))\n"
        "        span.replace(whole, payload)\n"
        "    }\n"
        "    buf.save(b, {force: true})\n"
        "    i = i + 1\n"
        "}\n";
    static const char open_script[] =
        "import buf\n"
        "let b = buf.current()\n"
        "if buf.len(b) < 1 { error(\"reopened file is empty\") }\n";

    make_file(save_path, save_script, sizeof(save_script) - 1U);
    make_file(open_path, open_script, sizeof(open_script) - 1U);
}

static void set_child_environment(const char *trial)
{
    char state[1024];
    char config[1024];
    char data[1024];
    char tmp[1024];
    int n;

    n = snprintf(state, sizeof(state), "%s/state", trial);
    if (n < 0 || (size_t)n >= sizeof(state) ||
        (mkdir(state, 0700) != 0 && errno != EEXIST))
        _exit(125);
    n = snprintf(config, sizeof(config), "%s/config", trial);
    if (n < 0 || (size_t)n >= sizeof(config) ||
        (mkdir(config, 0700) != 0 && errno != EEXIST))
        _exit(125);
    n = snprintf(data, sizeof(data), "%s/data", trial);
    if (n < 0 || (size_t)n >= sizeof(data) ||
        (mkdir(data, 0700) != 0 && errno != EEXIST))
        _exit(125);
    n = snprintf(tmp, sizeof(tmp), "%s/tmp", trial);
    if (n < 0 || (size_t)n >= sizeof(tmp) ||
        (mkdir(tmp, 0700) != 0 && errno != EEXIST))
        _exit(125);
    if (setenv("HOME", trial, 1) != 0 ||
        setenv("XDG_STATE_HOME", state, 1) != 0 ||
        setenv("XDG_CONFIG_HOME", config, 1) != 0 ||
        setenv("XDG_DATA_HOME", data, 1) != 0 ||
        setenv("TMPDIR", tmp, 1) != 0 || setenv("LC_ALL", "C", 1) != 0)
        _exit(125);
}

static pid_t spawn_batch(const char *sagitta, const char *script,
                         const char *target, const char *trial)
{
    pid_t pid = fork();

    if (pid < 0)
        die("fork");
    if (pid != 0)
        return pid;
    set_child_environment(trial);
    if (chdir(trial) != 0)
        _exit(125);
    {
        int nullfd = open("/dev/null", O_RDWR);

        if (nullfd < 0 || dup2(nullfd, STDIN_FILENO) < 0 ||
            dup2(nullfd, STDOUT_FILENO) < 0 ||
            dup2(nullfd, STDERR_FILENO) < 0)
            _exit(125);
        if (nullfd > STDERR_FILENO)
            (void)close(nullfd);
    }
    execl(sagitta, sagitta, "--clean", "--batch", script, target,
          (char *)NULL);
    _exit(127);
}

static pid_t spawn_checker(const char *checker, const char *target,
                           const char *old_path, const char *new_path,
                           const char *trial)
{
    pid_t pid = fork();

    if (pid < 0)
        die("fork checker");
    if (pid != 0)
        return pid;
    set_child_environment(trial);
    execl(checker, checker, "--check-batch", target, old_path, new_path,
          (char *)NULL);
    _exit(127);
}

static int wait_status(pid_t pid, int *status)
{
    pid_t waited;

    do {
        waited = waitpid(pid, status, 0);
    } while (waited < 0 && errno == EINTR);
    if (waited < 0)
        die("waitpid");
    if (WIFEXITED(*status))
        return WEXITSTATUS(*status);
    if (WIFSIGNALED(*status))
        return 128 + WTERMSIG(*status);
    return 255;
}

static bool tree_has_journal(const char *path)
{
    struct stat st;

    if (lstat(path, &st) != 0)
        return false;
    if (S_ISREG(st.st_mode)) {
        size_t n = strlen(path);

        return n >= 5U && strcmp(path + n - 5U, ".sagj") == 0 &&
               st.st_size > 0;
    }
    if (S_ISDIR(st.st_mode)) {
        DIR *dir = opendir(path);
        struct dirent *ent;

        if (dir == NULL)
            return false;
        while ((ent = readdir(dir)) != NULL) {
            char child[1024];
            int n;

            if (strcmp(ent->d_name, ".") == 0 ||
                strcmp(ent->d_name, "..") == 0)
                continue;
            n = snprintf(child, sizeof(child), "%s/%s", path, ent->d_name);
            if (n >= 0 && (size_t)n < sizeof(child) &&
                tree_has_journal(child)) {
                (void)closedir(dir);
                return true;
            }
        }
        (void)closedir(dir);
    }
    return false;
}

static bool exact(const unsigned char *got, size_t got_len,
                  const unsigned char *want, size_t want_len)
{
    return got_len == want_len && memcmp(got, want, want_len) == 0;
}

static void usage(const char *argv0)
{
    (void)fprintf(stderr,
                  "usage: %s --sagitta PATH --checker PATH "
                  "[--iterations N] [--seed N] "
                  "[--max-delay-us N]\n",
                  argv0);
}

int main(int argc, char **argv)
{
    char root_template[] = "/tmp/sagitta-batch-kill9-XXXXXX";
    char save_script[1024];
    char open_script[1024];
    char old_path[1024];
    char new_path[1024];
    char *sagitta = NULL;
    char *checker = NULL;
    char *root;
    unsigned char *new_bytes;
    size_t new_len;
    unsigned long long iterations = DEFAULT_ITERATIONS;
    unsigned long long max_delay_us = DEFAULT_MAX_DELAY_US;
    unsigned long long killed = 0U;
    unsigned long long attempts = 0U;
    unsigned long long old_seen = 0U;
    unsigned long long new_seen = 0U;
    unsigned long long journal_seen = 0U;
    uint64_t rng = UINT64_C(0x7361676974746137);
    uint64_t initial_seed;
    int i;

    {
        const char *v = getenv("SAG_BATCH_KILL9_ITERS");
        if (v != NULL)
            iterations = parse_count("SAG_BATCH_KILL9_ITERS", v, 200U);
        v = getenv("SAG_BATCH_KILL9_SEED");
        if (v != NULL)
            rng = (uint64_t)parse_count("SAG_BATCH_KILL9_SEED", v, 1U);
        v = getenv("SAG_BATCH_KILL9_MAX_DELAY_US");
        if (v != NULL)
            max_delay_us = parse_count("SAG_BATCH_KILL9_MAX_DELAY_US", v,
                                       1U);
    }
    for (i = 1; i < argc; i++) {
        const char *value;

        if (i + 1 >= argc) {
            usage(argv[0]);
            return 2;
        }
        value = argv[++i];
        if (strcmp(argv[i - 1], "--sagitta") == 0) {
            free(sagitta);
            sagitta = realpath(value, NULL);
            if (sagitta == NULL)
                die(value);
        } else if (strcmp(argv[i - 1], "--checker") == 0) {
            free(checker);
            checker = realpath(value, NULL);
            if (checker == NULL)
                die(value);
        } else if (strcmp(argv[i - 1], "--iterations") == 0) {
            iterations = parse_count("--iterations", value, 200U);
        } else if (strcmp(argv[i - 1], "--seed") == 0) {
            rng = (uint64_t)parse_count("--seed", value, 1U);
        } else if (strcmp(argv[i - 1], "--max-delay-us") == 0) {
            max_delay_us = parse_count("--max-delay-us", value, 1U);
        } else {
            usage(argv[0]);
            free(sagitta);
            free(checker);
            return 2;
        }
    }
    if (sagitta == NULL || checker == NULL) {
        usage(argv[0]);
        free(sagitta);
        free(checker);
        return 2;
    }
    initial_seed = rng;
    root = mkdtemp(root_template);
    if (root == NULL)
        die("mkdtemp");
    if (snprintf(save_script, sizeof(save_script), "%s/save-heavy.fl", root) <
            0 ||
        snprintf(open_script, sizeof(open_script), "%s/reopen.fl", root) < 0 ||
        snprintf(old_path, sizeof(old_path), "%s/old.txt", root) < 0 ||
        snprintf(new_path, sizeof(new_path), "%s/new.txt", root) < 0)
        die("script path");
    write_scripts(save_script, open_script);
    new_bytes = make_payload(&new_len);
    make_file(old_path, old_bytes, sizeof(old_bytes) - 1U);
    make_file(new_path, new_bytes, new_len);
    (void)printf("batch-kill9 seed=%llu iterations=%llu max-delay-us=%llu\n",
                 (unsigned long long)initial_seed, iterations, max_delay_us);
    (void)fflush(stdout);

    while (killed < iterations && attempts < iterations * 30U + 100U) {
        char trial[1024];
        char target[1024];
        struct timespec delay;
        unsigned char *got;
        size_t got_len;
        uint64_t delay_us;
        pid_t pid;
        int status;
        bool is_old;
        bool is_new;
        bool has_journal;

        attempts++;
        if (snprintf(trial, sizeof(trial), "%s/trial-%08llu", root,
                     attempts) < 0 || mkdir(trial, 0700) != 0)
            die("mkdir trial");
        if (snprintf(target, sizeof(target), "%s/target.txt", trial) < 0)
            die("target path");
        make_file(target, old_bytes, sizeof(old_bytes) - 1U);
        pid = spawn_batch(sagitta, save_script, target, trial);
        delay_us = random_next(&rng) % (max_delay_us + 1U);
        delay.tv_sec = (time_t)(delay_us / UINT64_C(1000000));
        delay.tv_nsec = (long)((delay_us % UINT64_C(1000000)) * 1000U);
        while (nanosleep(&delay, &delay) != 0 && errno == EINTR)
            ;
        if (kill(pid, SIGKILL) != 0 && errno != ESRCH)
            die("kill");
        (void)wait_status(pid, &status);
        if (!WIFSIGNALED(status) || WTERMSIG(status) != SIGKILL) {
            if (!remove_tree(trial))
                die("remove completed trial");
            continue;
        }
        if (!read_file(target, &got, &got_len))
            fail_root(root,
                      "batch-kill9: unreadable target after trial=%llu "
                      "size=%zu\n",
                      killed, 0U);
        is_old = exact(got, got_len, old_bytes, sizeof(old_bytes) - 1U);
        is_new = exact(got, got_len, new_bytes, new_len);
        free(got);
        if (!is_old && !is_new)
            fail_root(root,
                      "batch-kill9: partial/corrupt target after trial=%llu "
                      "size=%zu\n",
                      killed, got_len);
        old_seen += is_old ? 1U : 0U;
        new_seen += is_new ? 1U : 0U;
        has_journal = tree_has_journal(trial);
        journal_seen += has_journal ? 1U : 0U;
        if (has_journal) {
            pid = spawn_checker(checker, target, old_path, new_path, trial);
            if (wait_status(pid, &status) != 0)
                fail_root(root,
                          "batch-kill9: journal replay failed after "
                          "trial=%llu size=%zu\n",
                          killed, got_len);
        }

        pid = spawn_batch(sagitta, open_script, target, trial);
        if (wait_status(pid, &status) != 0)
            fail_root(root,
                      "batch-kill9: batch reopen failed after trial=%llu "
                      "size=%zu\n",
                      killed, got_len);
        killed++;
        if (!remove_tree(trial))
            die("remove killed trial");
    }
    if (killed != iterations)
        fail_root(root,
                  "batch-kill9: only %llu SIGKILL trials completed "
                  "(size=%zu)\n",
                  killed, (size_t)attempts);
    if (new_seen == 0U || journal_seen == 0U)
        fail_root(root,
                  "batch-kill9: campaign missed save/journal windows at "
                  "trial=%llu (journal count=%zu)\n",
                  new_seen, (size_t)journal_seen);

    (void)printf("batch-sigkill iterations=%llu attempts=%llu old=%llu "
                 "new=%llu journals=%llu seed=%llu ok\n",
                 killed, attempts, old_seen, new_seen, journal_seen,
                 (unsigned long long)initial_seed);
    free(new_bytes);
    free(sagitta);
    free(checker);
    if (getenv("SAG_TORTURE_KEEP") != NULL) {
        (void)printf("batch-kill9 root retained: %s\n", root);
    } else if (!remove_tree(root)) {
        die("remove batch-kill9 root");
    }
    return 0;
}
