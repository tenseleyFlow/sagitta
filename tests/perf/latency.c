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
    ARROW_KEYS = 2000,
    LATENCY_LINES = 10000,
    COLD_RUNS = 9,
    SCREEN_ROWS = 24,
    SCREEN_COLS = 80
};

typedef struct LatencyLimits {
    i64 key_p99_ns;
    i64 arrow_p99_ns;
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
                         char *wolf, size_t wolf_cap,
                         char *state, size_t state_cap)
{
    static const char line[] = "x\n";
    static const char hello_lu[] =
        "fn main() -> !int {\n"
        "    print(\"hello, wolf\")\n"
        "    greet(\"reader\")\n"
        "    0\n"
        "}\n"
        "\n"
        "fn greet(name: str) -> !int {\n"
        "    print(\"hello, {name}\")\n"
        "    0\n"
        "}";
    _Static_assert(sizeof(hello_lu) - 1U == 138U,
                   "Wolf latency fixture must match hello.lu");
    char template[] = "/tmp/yew-latency-XXXXXX";
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
    n = snprintf(wolf, wolf_cap, "%s/hello.lu", root);
    if (n < 0 || (size_t)n >= wolf_cap)
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
    if (close(fd) != 0)
        return false;
    fd = open(wolf, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd < 0)
        return false;
    if (!write_all(fd, hello_lu, sizeof(hello_lu) - 1U)) {
        (void)close(fd);
        return false;
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

static int selftest_exit_drain(void)
{
    YewLivePty pty = {.master = -1, .pid = -1};
    char slave[128];
    pid_t pid;
    int code = -1;

    if (!yew_live_pty_open(&pty, slave, sizeof(slave),
                           SCREEN_ROWS, SCREEN_COLS))
        return 1;
    pid = fork();
    if (pid < 0) {
        yew_live_pty_close(&pty);
        return 1;
    }
    if (pid == 0) {
        u8 bytes[4096];
        size_t i;

        if (!yew_live_pty_attach(&pty, slave, SCREEN_ROWS, SCREEN_COLS))
            _exit(126);
        (void)memset(bytes, 'x', sizeof(bytes));
        for (i = 0U; i < 256U; i++) {
            if (!write_all(STDOUT_FILENO, bytes, sizeof(bytes)))
                _exit(125);
        }
        _exit(0);
    }
    pty.pid = pid;
    if (!yew_live_pty_wait_exit(
            &pty, yew_live_pty_now_ns() + INT64_C(5000000000), &code) ||
        code != 0) {
        yew_live_pty_close(&pty);
        return 1;
    }
    yew_live_pty_close(&pty);
    (void)printf("perf-latency-selftest: exit drain ok\n");
    return 0;
}

static int selftest_quiet_drain(void)
{
    static const char frame[] = "\033[?2026h.\033[?2026l";
    YewLivePty pty = {.master = -1, .pid = -1};
    char slave[128];
    pid_t pid;
    i64 deadline;

    if (!yew_live_pty_open(&pty, slave, sizeof(slave),
                           SCREEN_ROWS, SCREEN_COLS))
        return 1;
    pid = fork();
    if (pid < 0) {
        yew_live_pty_close(&pty);
        return 1;
    }
    if (pid == 0) {
        struct timespec delayed = {0, 50000000L};
        struct timespec linger = {0, 300000000L};

        if (!yew_live_pty_attach(&pty, slave, SCREEN_ROWS, SCREEN_COLS) ||
            !write_all(STDOUT_FILENO, frame, sizeof(frame) - 1U))
            _exit(126);
        while (nanosleep(&delayed, &delayed) != 0 && errno == EINTR)
            ;
        if (!write_all(STDOUT_FILENO, frame, sizeof(frame) - 1U))
            _exit(125);
        while (nanosleep(&linger, &linger) != 0 && errno == EINTR)
            ;
        _exit(0);
    }
    pty.pid = pid;
    deadline = yew_live_pty_now_ns() + INT64_C(1000000000);
    if (!yew_live_pty_wait_frame(&pty, 0U, deadline, NULL) ||
        !yew_live_pty_wait_quiet(&pty, INT64_C(100000000), deadline) ||
        pty.frames != 2U) {
        yew_live_pty_close(&pty);
        return 1;
    }
    yew_live_pty_close(&pty);
    (void)printf("perf-latency-selftest: quiet drain ok\n");
    return 0;
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
        else if (strcmp(name, "cursor_arrow_to_paint_p99") == 0)
            limits->arrow_p99_ns = (i64)value;
        else if (strcmp(name, "cold_open_to_first_paint") == 0)
            limits->cold_ns = (i64)value;
    }
    if (ferror(file) || fclose(file) != 0)
        return false;
    return limits->key_p99_ns > 0 && limits->arrow_p99_ns > 0 &&
           limits->cold_ns > 0;
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

/*
 * Why the quit failed, not just that it did.  This lane has flaked
 * three times with "did not quit" and no way to tell a runner stall
 * from a non-zero exit status -- which are different bugs.
 */
static const char *g_stop_why = "?";

static bool stop_editor(YewLivePty *pty)
{
    static const char quit[] = "\033[27u:q!\r";
    /*
     * A LIVENESS bound, not a measured quantity.
     *
     * The sample is taken before this function runs, so how long the
     * editor takes to shut down cannot move any number this gate
     * reports — which means a tight deadline here buys nothing and
     * costs a flake.
     */
    i64 deadline = yew_live_pty_now_ns() + INT64_C(20000000000);
    int code;

    /*
     * THE QUIT IS ENCODED, not timed.
     *
     * A raw ESC followed later by command text is still one buffered read
     * when a loaded runner deschedules the editor between writes.  The
     * decoder then correctly treats ESC plus the next byte as an Alt
     * chord, so a wall-clock sleep cannot make this protocol reliable.
     * CSI-u Escape is unambiguous however the read is split, and `:q!`
     * remains valid after the default `q` key became macro recording.
     *
     * Retrying cannot hide a real hang.  An editor that is genuinely
     * stuck ignores every encoded command and still fails at the same
     * 20 s bound, with the same message.
     */
    while (yew_live_pty_now_ns() < deadline) {
        i64 give_up;

        g_stop_why = "quit write timed out";
        if (!yew_live_pty_write(pty, quit, sizeof(quit) - 1U, deadline))
            return false;

        /* Short per-attempt wait, long overall bound. */
        give_up = yew_live_pty_now_ns() + INT64_C(2000000000);
        if (give_up > deadline)
            give_up = deadline;
        if (yew_live_pty_wait_exit(pty, give_up, &code)) {
            if (code != 0) {
                static char why[64];

                (void)snprintf(why, sizeof(why),
                               "editor exited %d, wanted 0", code);
                g_stop_why = why;
                return false;
            }
            return true;
        }
    }
    g_stop_why = "editor did not exit within 20 s";
    return false;
}

static bool measure_cold(const char *binary, const char *path,
                         const char *state, i64 *median_out)
{
    i64 samples[COLD_RUNS];
    i64 work[COLD_RUNS];
    size_t run;

    for (run = 0U; run < COLD_RUNS; run++) {
        YewLivePty pty = {.master = -1, .pid = -1};
        i64 start = yew_live_pty_now_ns();
        i64 completed;
        i64 deadline = start + INT64_C(2000000000);

        if (start < 0 ||
            !yew_live_pty_spawn(&pty, binary, path, state,
                                SCREEN_ROWS, SCREEN_COLS) ||
            !yew_live_pty_wait_frame(&pty, 0U, deadline, &completed)) {
            (void)fprintf(stderr, "latency: cold run %zu did not paint\n",
                          run + 1U);
            yew_live_pty_close(&pty);
            return false;
        }
        samples[run] = completed - start;
        if (samples[run] < 0 || !stop_editor(&pty)) {
            (void)fprintf(stderr, "latency: cold run %zu did not quit: %s\n",
                          run + 1U, g_stop_why);
            yew_live_pty_close(&pty);
            return false;
        }
        yew_live_pty_close(&pty);
    }
    merge_sort_i64(samples, work, COLD_RUNS);
    *median_out = samples[COLD_RUNS / 2U];
    return true;
}

static size_t key_count(void)
{
    const char *env = getenv("YEW_LATENCY_KEYS");
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
    const char *env = getenv("YEW_LATENCY_INJECT_NS");
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

static bool settle_editor(YewLivePty *pty)
{
    /* Startup jobs may repaint after the first frame.  Drain them before
     * associating one subsequent frame with one measured input. */
    return yew_live_pty_wait_quiet(
        pty, INT64_C(250000000),
        yew_live_pty_now_ns() + INT64_C(2000000000));
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
    YewLivePty pty = {.master = -1, .pid = -1};
    i64 deadline;
    size_t i;
    bool ok = false;

    if (count == 0U || inject < 0)
        return false;
    samples = malloc(count * sizeof(*samples));
    work = malloc(count * sizeof(*work));
    if (samples == NULL || work == NULL)
        goto done_alloc;
    deadline = yew_live_pty_now_ns() + INT64_C(2000000000);
    if (!yew_live_pty_spawn(&pty, binary, path, state,
                            SCREEN_ROWS, SCREEN_COLS) ||
        !yew_live_pty_wait_frame(&pty, 0U, deadline, NULL) ||
        !settle_editor(&pty)) {
        (void)fprintf(stderr, "latency: key run did not paint initially\n");
        goto done_pty;
    }
    {
        static const char enter_insert = 'i';
        u64 frame = pty.frames;

        deadline = yew_live_pty_now_ns() + INT64_C(2000000000);
        if (!yew_live_pty_write(&pty, &enter_insert, 1U, deadline) ||
            !yew_live_pty_wait_frame(&pty, frame, deadline, NULL) ||
            !settle_editor(&pty)) {
            (void)fprintf(stderr, "latency: insert mode did not paint\n");
            goto done_pty;
        }
    }
    for (i = 0U; i < count; i++) {
        char key = (i & 1U) != 0U ? 'a' : 'b';
        u64 frame = pty.frames;
        i64 start = yew_live_pty_now_ns();
        i64 completed;

        deadline = start + INT64_C(1000000000);
        if (start < 0 ||
            !yew_live_pty_write(&pty, &key, 1U, deadline))
            goto done_pty;
        delay_ns(inject);
        if (!yew_live_pty_wait_frame(&pty, frame, deadline, &completed)) {
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
    yew_live_pty_close(&pty);
done_alloc:
    free(work);
    free(samples);
    return ok;
}

static bool measure_arrows(const char *binary, const char *path,
                           const char *state, i64 *p99_out)
{
    size_t configured = key_count();
    size_t count = configured < ARROW_KEYS ? configured : ARROW_KEYS;
    i64 inject = injected_delay();
    i64 *samples;
    i64 *work;
    YewLivePty pty = {.master = -1, .pid = -1};
    i64 deadline;
    size_t i;
    bool ok = false;

    if (count == 0U || inject < 0)
        return false;
    samples = malloc(count * sizeof(*samples));
    work = malloc(count * sizeof(*work));
    if (samples == NULL || work == NULL)
        goto done_alloc;
    deadline = yew_live_pty_now_ns() + INT64_C(2000000000);
    if (!yew_live_pty_spawn(&pty, binary, path, state,
                            SCREEN_ROWS, SCREEN_COLS) ||
        !yew_live_pty_wait_frame(&pty, 0U, deadline, NULL) ||
        !settle_editor(&pty)) {
        (void)fprintf(stderr, "latency: arrow run did not paint initially\n");
        goto done_pty;
    }
    for (i = 0U; i < count; i++) {
        static const char up[] = "\033[A";
        static const char down[] = "\033[B";
        const char *key = (i & 1U) != 0U ? up : down;
        u64 frame = pty.frames;
        i64 start = yew_live_pty_now_ns();
        i64 completed;

        deadline = start + INT64_C(1000000000);
        if (start < 0 || !yew_live_pty_write(&pty, key, sizeof(up) - 1U,
                                              deadline))
            goto done_pty;
        delay_ns(inject);
        if (!yew_live_pty_wait_frame(&pty, frame, deadline, &completed)) {
            (void)fprintf(stderr, "latency: arrow %zu did not paint\n", i + 1U);
            goto done_pty;
        }
        samples[i] = completed - start;
    }
    if (!stop_editor(&pty)) {
        (void)fprintf(stderr, "latency: arrow run did not quit\n");
        goto done_pty;
    }
    merge_sort_i64(samples, work, count);
    *p99_out = samples[(count * 99U + 99U) / 100U - 1U];
    ok = true;

done_pty:
    yew_live_pty_close(&pty);
done_alloc:
    free(work);
    free(samples);
    return ok;
}

int main(int argc, char **argv)
{
    char root[1024];
    char fixture[1024];
    char wolf[1024];
    char state[1024];
    LatencyLimits limits;
    i64 key_p99;
    i64 arrow_p99;
    i64 cold;
    const char *wolf_path;
    int status = 0;

    if (argc == 2 && strcmp(argv[1], "--selftest-exit-drain") == 0)
        return selftest_exit_drain();
    if (argc == 2 && strcmp(argv[1], "--selftest-quiet-drain") == 0)
        return selftest_quiet_drain();
    if (argc != 5 || strcmp(argv[1], "--yew") != 0 ||
        strcmp(argv[3], "--baseline") != 0) {
        (void)fprintf(stderr,
                      "usage: %s --yew PATH --baseline PATH\n", argv[0]);
        return 2;
    }
    if (!load_limits(argv[4], &limits)) {
        (void)fprintf(stderr, "latency: invalid baseline %s\n", argv[4]);
        return 2;
    }
    if (!make_fixture(root, sizeof(root), fixture, sizeof(fixture),
                      wolf, sizeof(wolf),
                      state, sizeof(state))) {
        (void)fprintf(stderr, "latency: cannot create fixtures\n");
        return 2;
    }
    wolf_path = getenv("YEW_LATENCY_WOLF_PATH");
    if (wolf_path == NULL || *wolf_path == '\0') {
        wolf_path = wolf;
    } else {
        struct stat wolf_st;

        if (stat(wolf_path, &wolf_st) != 0 || !S_ISREG(wolf_st.st_mode)) {
            (void)fprintf(stderr,
                          "latency: invalid Wolf fixture %s\n", wolf_path);
            (void)remove_tree(root);
            return 2;
        }
    }
    if (!measure_cold(argv[2], fixture, state, &cold) ||
        !measure_keys(argv[2], fixture, state, &key_p99) ||
        !measure_arrows(argv[2], wolf_path, state, &arrow_p99)) {
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
    (void)printf("cursor_arrow_to_paint_p99 %lld limit=%lld%s\n",
                 (long long)arrow_p99, (long long)limits.arrow_p99_ns,
                 arrow_p99 <= limits.arrow_p99_ns ? " ok" : " FAIL");
    (void)printf("cold_open_to_first_paint %lld limit=%lld%s\n",
                 (long long)cold, (long long)limits.cold_ns,
                 cold <= limits.cold_ns ? " ok" : " FAIL");
    if (key_p99 > limits.key_p99_ns ||
        arrow_p99 > limits.arrow_p99_ns || cold > limits.cold_ns)
        status = 1;
    return status;
}
