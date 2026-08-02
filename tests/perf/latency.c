#define _POSIX_C_SOURCE 200809L

#include "support/live_pty.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

enum {
    LATENCY_KEYS = 10000,
    LATENCY_LINES = 10000,
    COLD_RUNS = 9,
    SCREEN_ROWS = 24,
    SCREEN_COLS = 80
};

typedef struct LatencyLimits {
    i64 key_p99_ns;
    i64 cold_ns;
} LatencyLimits;

static bool write_all(int fd, const void *data, size_t len)
{
    const u8 *bytes = data;

    while (len != 0U) {
        ssize_t n = write(fd, bytes, len);

        if (n > 0) {
            bytes += (size_t)n;
            len -= (size_t)n;
        } else if (n < 0 && errno == EINTR) {
            continue;
        } else {
            return false;
        }
    }
    return true;
}

static bool make_fixture(char *root, size_t root_cap,
                         char *path, size_t path_cap,
                         char *state, size_t state_cap)
{
    static const char line[] = "x\n";
    char template[] = "/tmp/sagitta-latency-XXXXXX";
    char *made;
    int fd;
    int n;
    size_t i;

    made = mkdtemp(template);
    if (made == NULL || strlen(made) + 1U > root_cap)
        return false;
    (void)strcpy(root, made);
    n = snprintf(path, path_cap, "%s/fixture.c", root);
    if (n < 0 || (size_t)n >= path_cap)
        return false;
    n = snprintf(state, state_cap, "%s/state", root);
    if (n < 0 || (size_t)n >= state_cap || mkdir(state, 0700) != 0)
        return false;
    fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd < 0)
        return false;
    for (i = 0U; i < LATENCY_LINES; i++) {
        if (!write_all(fd, line, sizeof(line) - 1U)) {
            (void)close(fd);
            return false;
        }
    }
    return close(fd) == 0;
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

static bool load_limits(const char *path, LatencyLimits *limits)
{
    FILE *file = fopen(path, "r");
    char line[256];

    if (file == NULL)
        return false;
    (void)memset(limits, 0, sizeof(*limits));
    while (fgets(line, sizeof(line), file) != NULL) {
        char name[96];
        long long value;

        if (sscanf(line, "%95s %lld", name, &value) != 2 || value <= 0)
            continue;
        if (strcmp(name, "keypress_to_paint_p99") == 0)
            limits->key_p99_ns = (i64)value;
        else if (strcmp(name, "cold_open_to_first_paint") == 0)
            limits->cold_ns = (i64)value;
    }
    if (ferror(file) || fclose(file) != 0)
        return false;
    return limits->key_p99_ns > 0 && limits->cold_ns > 0;
}

static void merge_sort_i64(i64 *values, i64 *work, size_t len)
{
    size_t width;

    for (width = 1U; width < len; width *= 2U) {
        size_t lo;

        for (lo = 0U; lo < len; lo += width * 2U) {
            size_t mid = lo + width < len ? lo + width : len;
            size_t hi = mid + width < len ? mid + width : len;
            size_t a = lo;
            size_t b = mid;
            size_t out = lo;

            while (a < mid || b < hi) {
                if (b == hi || (a < mid && values[a] <= values[b]))
                    work[out++] = values[a++];
                else
                    work[out++] = values[b++];
            }
        }
        (void)memcpy(values, work, len * sizeof(*values));
        if (width > len / 2U)
            break;
    }
}

static bool stop_editor(SagLivePty *pty)
{
    static const char escape = '\033';
    static const char quit[] = "q!";
    struct timespec settle = {0, 50000000};
    i64 deadline = sag_live_pty_now_ns() + INT64_C(2000000000);
    int code;

    if (!sag_live_pty_write(pty, &escape, 1U, deadline))
        return false;
    while (nanosleep(&settle, &settle) != 0 && errno == EINTR)
        ;
    if (!sag_live_pty_write(pty, quit, sizeof(quit) - 1U, deadline) ||
        !sag_live_pty_wait_exit(pty, deadline, &code))
        return false;
    return code == 0;
}

static bool measure_cold(const char *binary, const char *path,
                         const char *state, i64 *median_out)
{
    i64 samples[COLD_RUNS];
    i64 work[COLD_RUNS];
    size_t run;

    for (run = 0U; run < COLD_RUNS; run++) {
        SagLivePty pty = {.master = -1, .pid = -1};
        i64 start = sag_live_pty_now_ns();
        i64 completed;
        i64 deadline = start + INT64_C(2000000000);

        if (start < 0 ||
            !sag_live_pty_spawn(&pty, binary, path, state,
                                SCREEN_ROWS, SCREEN_COLS) ||
            !sag_live_pty_wait_frame(&pty, 0U, deadline, &completed)) {
            (void)fprintf(stderr, "latency: cold run %zu did not paint\n",
                          run + 1U);
            sag_live_pty_close(&pty);
            return false;
        }
        samples[run] = completed - start;
        if (samples[run] < 0 || !stop_editor(&pty)) {
            (void)fprintf(stderr, "latency: cold run %zu did not quit\n",
                          run + 1U);
            sag_live_pty_close(&pty);
            return false;
        }
        sag_live_pty_close(&pty);
    }
    merge_sort_i64(samples, work, COLD_RUNS);
    *median_out = samples[COLD_RUNS / 2U];
    return true;
}

static size_t key_count(void)
{
    const char *env = getenv("SAG_LATENCY_KEYS");
    char *end;
    unsigned long value;

    if (env == NULL || *env == '\0')
        return LATENCY_KEYS;
    errno = 0;
    value = strtoul(env, &end, 10);
    if (errno != 0 || *end != '\0' || value < 100U || value > LATENCY_KEYS)
        return 0U;
    return (size_t)value;
}

static i64 injected_delay(void)
{
    const char *env = getenv("SAG_LATENCY_INJECT_NS");
    char *end;
    long long value;

    if (env == NULL || *env == '\0')
        return 0;
    errno = 0;
    value = strtoll(env, &end, 10);
    if (errno != 0 || *end != '\0' || value < 0)
        return -1;
    return (i64)value;
}

static void delay_ns(i64 ns)
{
    struct timespec delay;

    if (ns <= 0)
        return;
    delay.tv_sec = (time_t)(ns / INT64_C(1000000000));
    delay.tv_nsec = (long)(ns % INT64_C(1000000000));
    while (nanosleep(&delay, &delay) != 0 && errno == EINTR)
        ;
}

static bool measure_keys(const char *binary, const char *path,
                         const char *state, i64 *p99_out)
{
    size_t count = key_count();
    i64 inject = injected_delay();
    i64 *samples;
    i64 *work;
    SagLivePty pty = {.master = -1, .pid = -1};
    i64 deadline;
    size_t i;
    bool ok = false;

    if (count == 0U || inject < 0)
        return false;
    samples = malloc(count * sizeof(*samples));
    work = malloc(count * sizeof(*work));
    if (samples == NULL || work == NULL)
        goto done_alloc;
    deadline = sag_live_pty_now_ns() + INT64_C(2000000000);
    if (!sag_live_pty_spawn(&pty, binary, path, state,
                            SCREEN_ROWS, SCREEN_COLS) ||
        !sag_live_pty_wait_frame(&pty, 0U, deadline, NULL)) {
        (void)fprintf(stderr, "latency: key run did not paint initially\n");
        goto done_pty;
    }
    {
        static const char enter_insert = 'i';
        u64 frame = pty.frames;

        deadline = sag_live_pty_now_ns() + INT64_C(2000000000);
        if (!sag_live_pty_write(&pty, &enter_insert, 1U, deadline) ||
            !sag_live_pty_wait_frame(&pty, frame, deadline, NULL)) {
            (void)fprintf(stderr, "latency: insert mode did not paint\n");
            goto done_pty;
        }
    }
    for (i = 0U; i < count; i++) {
        char key = (i & 1U) != 0U ? 'a' : 'b';
        u64 frame = pty.frames;
        i64 start = sag_live_pty_now_ns();
        i64 completed;

        deadline = start + INT64_C(1000000000);
        if (start < 0 ||
            !sag_live_pty_write(&pty, &key, 1U, deadline))
            goto done_pty;
        delay_ns(inject);
        if (!sag_live_pty_wait_frame(&pty, frame, deadline, &completed)) {
            (void)fprintf(stderr, "latency: key %zu did not paint\n", i + 1U);
            goto done_pty;
        }
        samples[i] = completed - start;
    }
    if (!stop_editor(&pty)) {
        (void)fprintf(stderr, "latency: key run did not quit\n");
        goto done_pty;
    }
    merge_sort_i64(samples, work, count);
    *p99_out = samples[(count * 99U + 99U) / 100U - 1U];
    ok = true;

done_pty:
    sag_live_pty_close(&pty);
done_alloc:
    free(work);
    free(samples);
    return ok;
}

int main(int argc, char **argv)
{
    char root[1024];
    char fixture[1024];
    char state[1024];
    LatencyLimits limits;
    i64 key_p99;
    i64 cold;
    int status = 0;

    if (argc != 5 || strcmp(argv[1], "--sagitta") != 0 ||
        strcmp(argv[3], "--baseline") != 0) {
        (void)fprintf(stderr,
                      "usage: %s --sagitta PATH --baseline PATH\n", argv[0]);
        return 2;
    }
    if (!load_limits(argv[4], &limits)) {
        (void)fprintf(stderr, "latency: invalid baseline %s\n", argv[4]);
        return 2;
    }
    if (!make_fixture(root, sizeof(root), fixture, sizeof(fixture),
                      state, sizeof(state))) {
        (void)fprintf(stderr, "latency: cannot create 10 kLOC fixture\n");
        return 2;
    }
    if (!measure_cold(argv[2], fixture, state, &cold) ||
        !measure_keys(argv[2], fixture, state, &key_p99)) {
        (void)fprintf(stderr, "latency: PTY measurement failed\n");
        (void)remove_tree(root);
        return 2;
    }
    if (!remove_tree(root)) {
        (void)fprintf(stderr, "latency: cannot remove fixture tree\n");
        return 2;
    }

    (void)printf("keypress_to_paint_p99 %lld limit=%lld%s\n",
                 (long long)key_p99, (long long)limits.key_p99_ns,
                 key_p99 <= limits.key_p99_ns ? " ok" : " FAIL");
    (void)printf("cold_open_to_first_paint %lld limit=%lld%s\n",
                 (long long)cold, (long long)limits.cold_ns,
                 cold <= limits.cold_ns ? " ok" : " FAIL");
    if (key_p99 > limits.key_p99_ns || cold > limits.cold_ns)
        status = 1;
    return status;
}
