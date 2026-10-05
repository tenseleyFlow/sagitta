/*
 * Sprint 19 DoD 13: streamed job output must not starve the input loop.
 *
 * A job can emit faster than any terminal can show it (`yes` produces
 * gigabytes per second).  YEW_JOB_READ_BUDGET caps the bytes drained per
 * job per loop iteration precisely so keystrokes keep their turn; this
 * gate is what makes that cap load-bearing rather than decorative.
 *
 * What it measures: the wall time from writing a key to the next painted
 * frame, while a job floods the editor.  That is a responsiveness proxy,
 * not a causal keypress→its-own-paint measurement — under streaming the
 * editor paints continuously, so a frame may reflect job output rather
 * than the key.  The proxy is still the right alarm: if the loop stalls
 * inside an unbounded drain, no frame arrives at all until it finishes,
 * and the sample blows the budget.  The selftest below proves the gate
 * reacts by injecting a delay.
 */
#define _POSIX_C_SOURCE 200809L

#include "support/live_pty.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "perf_policy.h"
#include "perf_runtime.h"

enum {
    STREAM_KEYS = 2000,
    SCREEN_ROWS = 24,
    SCREEN_COLS = 80,
    FIXTURE_LINES = 200
};

/* ~50 MiB in 64 KiB blocks: big enough to span thousands of read budgets
 * (256 KiB each), short enough to keep the gate inside a CI minute. */
#define STREAM_BYTES_DEFAULT 52428800

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

static bool make_fixture(char *root, size_t root_cap, char *path,
                         size_t path_cap, char *state, size_t state_cap)
{
    static const char line[] = "streaming fixture line\n";
    char template[] = "/tmp/yew-jobstream-XXXXXX";
    char *made;
    int fd;
    int i;

    made = mkdtemp(template);
    if (made == NULL ||
        (size_t)snprintf(root, root_cap, "%s", made) >= root_cap ||
        (size_t)snprintf(path, path_cap, "%s/fixture.txt", made) >=
            path_cap ||
        (size_t)snprintf(state, state_cap, "%s/state", made) >= state_cap)
        return false;
    if (mkdir(state, 0700) != 0)
        return false;
    fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        return false;
    for (i = 0; i < FIXTURE_LINES; i++) {
        if (!write_all(fd, line, sizeof(line) - 1U)) {
            (void)close(fd);
            return false;
        }
    }
    return close(fd) == 0;
}

static void remove_tree(const char *root, const char *path,
                        const char *state)
{
    (void)unlink(path);
    (void)rmdir(state);
    (void)rmdir(root);
}

static void merge_sort_i64(i64 *values, i64 *work, size_t len)
{
    size_t width;

    for (width = 1U; width < len; width *= 2U) {
        size_t at;

        for (at = 0U; at < len; at += width * 2U) {
            size_t mid = at + width < len ? at + width : len;
            size_t end = at + width * 2U < len ? at + width * 2U : len;
            size_t l = at;
            size_t r = mid;
            size_t o = at;

            while (l < mid && r < end)
                work[o++] = values[l] <= values[r] ? values[l++] :
                                                     values[r++];
            while (l < mid)
                work[o++] = values[l++];
            while (r < end)
                work[o++] = values[r++];
        }
        (void)memcpy(values, work, len * sizeof(*values));
    }
}

static void delay_ns(i64 ns)
{
    struct timespec req;

    if (ns <= 0)
        return;
    req.tv_sec = (time_t)(ns / INT64_C(1000000000));
    req.tv_nsec = (long)(ns % INT64_C(1000000000));
    while (nanosleep(&req, &req) != 0 && errno == EINTR)
        continue;
}

static i64 env_i64(const char *name, i64 fallback)
{
    const char *value = getenv(name);
    char *end = NULL;
    long long parsed;

    if (value == NULL || value[0] == '\0')
        return fallback;
    parsed = strtoll(value, &end, 10);
    if (end == NULL || *end != '\0' || parsed < 0)
        return fallback;
    return (i64)parsed;
}

/* Line-oriented like latency.c's loader: the baseline carries comment
 * headers, and a token-stream parser stops dead on the first one. */
static bool read_limit(const char *path, i64 *limit_out)
{
    FILE *file = fopen(path, "r");
    char line[256];

    if (file == NULL)
        return false;
    *limit_out = 0;
    while (fgets(line, sizeof(line), file) != NULL) {
        char name[96];
        long long value;

        if (sscanf(line, "%95s %lld", name, &value) != 2 || value <= 0)
            continue;
        if (strcmp(name, "keypress_to_paint_p99_streaming") == 0)
            *limit_out = (i64)value;
    }
    if (ferror(file) || fclose(file) != 0)
        return false;
    return *limit_out > 0;
}

static bool stop_editor(YewLivePty *pty)
{
    /* CSI-u Escape cannot merge with `:` into an Alt chord when a loaded
     * runner drains the whole quit sequence in one read. */
    static const char quit[] = "\x1b[27u:q!\r";
    i64 deadline = yew_live_pty_now_ns() + INT64_C(5000000000);
    int code = 0;

    /* Quitting kills the job's process group; the editor must not block
     * waiting for a child that is still writing. */
    if (!yew_live_pty_write(pty, quit, sizeof(quit) - 1U, deadline))
        return false;
    return yew_live_pty_wait_exit(pty, deadline, &code);
}

static bool measure(const char *binary, const char *path, const char *state,
                    size_t count, i64 inject, i64 *p99_out)
{
    YewLivePty pty = {0};
    i64 *samples = NULL;
    i64 *work = NULL;
    i64 deadline;
    size_t i;
    bool ok = false;

    samples = malloc(count * sizeof(*samples));
    work = malloc(count * sizeof(*work));
    if (samples == NULL || work == NULL)
        goto done_alloc;
    deadline = yew_live_pty_now_ns() + INT64_C(5000000000);
    if (!yew_live_pty_spawn(&pty, binary, path, state, SCREEN_ROWS,
                            SCREEN_COLS) ||
        !yew_live_pty_wait_frame(&pty, 0U, deadline, NULL)) {
        (void)fprintf(stderr, "jobstream: editor did not paint\n");
        goto done_pty;
    }
    /* Start the flood, then keep typing through it. */
    {
    char command[256];
    int n = snprintf(command, sizeof(command),
                     ":!yes 0123456789abcdefghijklmnopqrstuvwxyz"
                     "0123456789abcdefghijklmn | head -c %lld\r",
                     (long long)env_i64("YEW_JOBSTREAM_BYTES",
                                        STREAM_BYTES_DEFAULT));
    deadline = yew_live_pty_now_ns() + INT64_C(5000000000);
    if (n <= 0 || !yew_live_pty_write(&pty, command, (size_t)n, deadline)) {
        (void)fprintf(stderr, "jobstream: could not start the job\n");
        goto done_pty;
    }
    }
    for (i = 0U; i < count; i++) {
        /* Down/up arrows: pure movement, so the flooded job buffer is
         * never edited and the samples measure dispatch plus paint. */
        const char *key = (i & 1U) != 0U ? "\x1b[B" : "\x1b[A";
        u64 frame = pty.frames;
        i64 start = yew_live_pty_now_ns();
        i64 completed;

        deadline = start + INT64_C(2000000000);
        if (start < 0 || !yew_live_pty_write(&pty, key, 3U, deadline))
            goto done_pty;
        delay_ns(inject);
        if (!yew_live_pty_wait_frame(&pty, frame, deadline, &completed)) {
            (void)fprintf(stderr,
                          "jobstream: key %zu never reached a frame\n",
                          i + 1U);
            goto done_pty;
        }
        samples[i] = completed - start;
    }
    if (!stop_editor(&pty)) {
        (void)fprintf(stderr, "jobstream: editor did not quit\n");
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

/* Strict by default; under YEW_PERF_ADVISORY (shared hosted runners) the
 * p99 fails only when zero or beyond 100x the baseline limit.  A stalled
 * or crashed editor is a measurement failure and exits 2 in both modes. */
static bool p99_failed(i64 p99, i64 limit, bool advisory)
{
    return yew_perf_timing_failed_i64(p99, limit, advisory);
}

static int selftest_policy(void)
{
    const i64 limit = 5000000;
    const i64 ceiling = limit * YEW_PERF_ADVISORY_SANITY_MULTIPLIER;

    if (p99_failed(limit, limit, false) || !p99_failed(limit + 1, limit, false) ||
        p99_failed(limit + 1, limit, true) || p99_failed(ceiling, limit, true) ||
        !p99_failed(ceiling + 1, limit, true) || !p99_failed(0, limit, true) ||
        !p99_failed(-1, limit, false)) {
        (void)fprintf(stderr, "jobstream: policy selftest failed\n");
        return 1;
    }
    (void)printf("perf-jobstream-policy: strict/advisory/sanity ok\n");
    return 0;
}

int main(int argc, char **argv)
{
    char root[1024];
    char fixture[1024];
    char state[1024];
    i64 limit = 0;
    i64 p99 = 0;
    size_t keys = (size_t)env_i64("YEW_JOBSTREAM_KEYS", STREAM_KEYS);
    i64 inject = env_i64("YEW_JOBSTREAM_INJECT_NS", 0);
    int status = 0;
    bool advisory = yew_perf_advisory();

    if (!yew_perf_runtime_check("perf_jobstream"))
        return 2;

    if (argc == 2 && strcmp(argv[1], "--selftest-policy") == 0)
        return selftest_policy();
    if (argc != 5 || strcmp(argv[1], "--yew") != 0 ||
        strcmp(argv[3], "--baseline") != 0) {
        (void)fprintf(stderr,
                      "usage: perf_jobstream --yew BIN --baseline FILE\n");
        return 2;
    }
    if (!read_limit(argv[4], &limit)) {
        (void)fprintf(stderr, "jobstream: cannot read baseline %s\n",
                      argv[4]);
        return 2;
    }
    if (keys == 0U)
        keys = STREAM_KEYS;
    if (!make_fixture(root, sizeof(root), fixture, sizeof(fixture), state,
                      sizeof(state))) {
        (void)fprintf(stderr, "jobstream: cannot create fixture\n");
        return 2;
    }
    if (!measure(argv[2], fixture, state, keys, inject, &p99)) {
        remove_tree(root, fixture, state);
        return 2;
    }
    (void)printf("jobstream keypress_to_paint_p99_streaming %lld ns "
                 "(limit %lld ns, %zu keys under a 50 MiB stream) "
                 "mode=%s%s\n",
                 (long long)p99, (long long)limit, keys,
                 yew_perf_mode(advisory),
                 yew_perf_timing_verdict_i64(p99, limit, advisory));
    if (p99_failed(p99, limit, advisory)) {
        (void)fprintf(stderr,
                      "jobstream: p99 %lld ns exceeds limit %lld ns (%s)\n",
                      (long long)p99, (long long)limit,
                      yew_perf_mode(advisory));
        status = 1;
    }
    remove_tree(root, fixture, state);
    return status;
}
