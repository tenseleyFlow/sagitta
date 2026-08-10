/*
 * Sprint 37 batch-startup gate.
 *
 * Measure the product surface, not an internal bootstrap helper: every
 * sample forks and execs `yew --clean --batch empty.fl small.txt`, then
 * waits for the process to exit successfully.  The median of 101 warm-cache
 * runs is gated at 8 ms.  A p95 is reported as useful scheduling-noise
 * evidence but is not gated: subprocess tail latency belongs to the host
 * scheduler, while the median makes a stable regression gate for editor
 * startup itself.
 *
 * YEW_BATCH_INJECT_NS delays the child before exec.  It exists only so the
 * Makefile selftest can prove that the gate rejects a known regression.
 */
#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <dirent.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

enum {
    BATCH_WARMUPS = 5,
    BATCH_SAMPLES = 101,
    BATCH_MIN_SAMPLES = 100,
    BATCH_MAX_SAMPLES = 1001
};

#define BATCH_BUDGET_NS INT64_C(8000000)

static int64_t now_ns(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return -1;
    return (int64_t)ts.tv_sec * INT64_C(1000000000) +
           (int64_t)ts.tv_nsec;
}

static void delay_ns(int64_t ns)
{
    struct timespec delay;

    if (ns <= 0)
        return;
    delay.tv_sec = (time_t)(ns / INT64_C(1000000000));
    delay.tv_nsec = (long)(ns % INT64_C(1000000000));
    while (nanosleep(&delay, &delay) != 0 && errno == EINTR)
        ;
}

static bool write_all(int fd, const void *data, size_t len)
{
    const unsigned char *bytes = data;

    while (len != 0U) {
        ssize_t wrote = write(fd, bytes, len);

        if (wrote > 0) {
            bytes += (size_t)wrote;
            len -= (size_t)wrote;
        } else if (wrote < 0 && errno == EINTR) {
            continue;
        } else {
            return false;
        }
    }
    return true;
}

static bool write_fixture(const char *path, const void *data, size_t len)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
    bool ok;

    if (fd < 0)
        return false;
    ok = write_all(fd, data, len);
    if (close(fd) != 0)
        ok = false;
    return ok;
}

static bool fixture_open(char *root, size_t root_cap,
                         char *script, size_t script_cap,
                         char *file, size_t file_cap)
{
    static const char small[] =
        "int main(void) {\n"
        "    return 0;\n"
        "}\n";
    char template[] = "/tmp/yew-batch-perf-XXXXXX";
    char *made = mkdtemp(template);
    int n;

    if (made == NULL || strlen(made) + 1U > root_cap)
        return false;
    (void)strcpy(root, made);
    n = snprintf(script, script_cap, "%s/empty.fl", root);
    if (n < 0 || (size_t)n >= script_cap)
        return false;
    n = snprintf(file, file_cap, "%s/small.c", root);
    if (n < 0 || (size_t)n >= file_cap)
        return false;
    if (write_fixture(script, "", 0U) &&
        write_fixture(file, small, sizeof(small) - 1U))
        return true;
    (void)unlink(script);
    (void)unlink(file);
    (void)rmdir(root);
    root[0] = '\0';
    return false;
}

static bool remove_tree(const char *path)
{
    struct stat st;

    if (lstat(path, &st) != 0)
        return errno == ENOENT;
    if (S_ISDIR(st.st_mode)) {
        DIR *dir = opendir(path);
        struct dirent *entry;

        if (dir == NULL)
            return false;
        while ((entry = readdir(dir)) != NULL) {
            char child[1200];
            int n;

            if (strcmp(entry->d_name, ".") == 0 ||
                strcmp(entry->d_name, "..") == 0)
                continue;
            n = snprintf(child, sizeof(child), "%s/%s", path,
                         entry->d_name);
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

static int64_t env_i64(const char *name, int64_t fallback)
{
    const char *text = getenv(name);
    char *end;
    long long value;

    if (text == NULL || *text == '\0')
        return fallback;
    errno = 0;
    value = strtoll(text, &end, 10);
    if (errno != 0 || *end != '\0' || value < 0)
        return -1;
    return (int64_t)value;
}

static size_t sample_count(void)
{
    int64_t count = env_i64("YEW_BATCH_RUNS", BATCH_SAMPLES);

    if (count < BATCH_MIN_SAMPLES || count > BATCH_MAX_SAMPLES)
        return 0U;
    return (size_t)count;
}

static bool run_once(const char *binary, const char *script,
                     const char *file, const char *state,
                     int64_t inject_ns, int64_t *elapsed)
{
    int64_t start = now_ns();
    pid_t pid;
    pid_t waited;
    int status;

    if (start < 0)
        return false;
    pid = fork();
    if (pid < 0)
        return false;
    if (pid == 0) {
        int nullfd = open("/dev/null", O_RDWR);

        if (nullfd < 0 || dup2(nullfd, STDIN_FILENO) < 0 ||
            dup2(nullfd, STDOUT_FILENO) < 0 ||
            dup2(nullfd, STDERR_FILENO) < 0)
            _exit(126);
        if (nullfd > STDERR_FILENO)
            (void)close(nullfd);
        if (setenv("XDG_STATE_HOME", state, 1) != 0 ||
            setenv("XDG_CONFIG_HOME", state, 1) != 0 ||
            setenv("LC_ALL", "C", 1) != 0)
            _exit(126);
        delay_ns(inject_ns);
        execl(binary, binary, "--clean", "--batch", script, file,
              (char *)NULL);
        _exit(127);
    }
    do {
        waited = waitpid(pid, &status, 0);
    } while (waited < 0 && errno == EINTR);
    if (waited != pid)
        return false;
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
        return false;
    *elapsed = now_ns() - start;
    return *elapsed >= 0;
}

/* Insertion sort is deliberate: raw qsort is banned, and n is only 101. */
static void sort_i64(int64_t *values, size_t len)
{
    size_t i;

    for (i = 1U; i < len; i++) {
        int64_t value = values[i];
        size_t at = i;

        while (at != 0U && values[at - 1U] > value) {
            values[at] = values[at - 1U];
            at--;
        }
        values[at] = value;
    }
}

int main(int argc, char **argv)
{
    char root[1024] = "";
    char script[1024] = "";
    char file[1024] = "";
    const char *binary;
    bool gate = false;
    size_t count = sample_count();
    int64_t inject_ns = env_i64("YEW_BATCH_INJECT_NS", 0);
    int64_t *samples;
    int64_t median;
    int64_t p95;
    size_t i;
    int result = 2;

    if ((argc != 3 && argc != 4) || strcmp(argv[1], "--yew") != 0 ||
        (argc == 4 && strcmp(argv[3], "--gate") != 0)) {
        (void)fprintf(stderr,
                      "usage: %s --yew PATH [--gate]\n", argv[0]);
        return 2;
    }
    binary = argv[2];
    gate = argc == 4;
    if (count == 0U || inject_ns < 0) {
        (void)fprintf(stderr,
                      "perf-batch: YEW_BATCH_RUNS must be 100..1001 and "
                      "YEW_BATCH_INJECT_NS must be nonnegative\n");
        return 2;
    }
    samples = malloc(count * sizeof(*samples));
    if (samples == NULL) {
        (void)fprintf(stderr, "perf-batch: cannot allocate samples\n");
        return 2;
    }
    if (!fixture_open(root, sizeof(root), script, sizeof(script), file,
                      sizeof(file))) {
        (void)fprintf(stderr, "perf-batch: cannot create fixture: %s\n",
                      strerror(errno));
        goto done;
    }
    for (i = 0U; i < (size_t)BATCH_WARMUPS; i++) {
        int64_t ignored;

        if (!run_once(binary, script, file, root, inject_ns, &ignored)) {
            (void)fprintf(stderr, "perf-batch: warmup %zu failed\n", i + 1U);
            goto done_fixture;
        }
    }
    for (i = 0U; i < count; i++) {
        if (!run_once(binary, script, file, root, inject_ns, &samples[i])) {
            (void)fprintf(stderr, "perf-batch: sample %zu failed\n", i + 1U);
            goto done_fixture;
        }
    }
    sort_i64(samples, count);
    median = samples[count / 2U];
    p95 = samples[(count * 95U + 99U) / 100U - 1U];
    (void)printf("batch_startup_median_ns %lld limit=%lld runs=%zu%s\n",
                 (long long)median, (long long)BATCH_BUDGET_NS, count,
                 !gate || median <= BATCH_BUDGET_NS ? " ok" : " FAIL");
    (void)printf("batch_startup_p95_ns %lld informational\n",
                 (long long)p95);
    result = gate && median > BATCH_BUDGET_NS ? 1 : 0;

done_fixture:
    if (!remove_tree(root)) {
        (void)fprintf(stderr, "perf-batch: cannot remove fixture: %s\n",
                      strerror(errno));
        result = 2;
    }
done:
    free(samples);
    return result;
}
