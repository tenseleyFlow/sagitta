#define _POSIX_C_SOURCE 200809L

#include "edit/ed.h"

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

typedef struct {
    i64 key_p99_ns;
    i64 cold_ns;
} LatencyLimits;

static volatile u64 latency_sink;

static i64 now_ns(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return -1;
    return (i64)ts.tv_sec * INT64_C(1000000000) + ts.tv_nsec;
}

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

static bool make_fixture(char *path, size_t path_len,
                         char *state, size_t state_len)
{
    static const char line[] =
        "int sagitta_latency_fixture(void) { return 14; }\n";
    char root[] = "/tmp/sagitta-latency-XXXXXX";
    int fd;
    int n;
    size_t i;

    if (mkdtemp(root) == NULL)
        return false;
    n = snprintf(path, path_len, "%s/fixture.c", root);
    if (n < 0 || (size_t)n >= path_len)
        return false;
    n = snprintf(state, state_len, "%s/state", root);
    if (n < 0 || (size_t)n >= state_len || mkdir(state, 0700) != 0)
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

static void remove_fixture(const char *path, const char *state)
{
    char root[1024];
    char *slash;

    (void)unlink(path);
    (void)rmdir(state);
    if (strlen(path) >= sizeof(root))
        return;
    (void)strcpy(root, path);
    slash = strrchr(root, '/');
    if (slash != NULL) {
        *slash = '\0';
        (void)rmdir(root);
    }
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

static bool editor_ready(Ed *ed, const char *path, int sink_fd)
{
    sag_ed_init(ed);
    if (sag_ed_open(ed, path) != SAG_LOAD_OK) {
        sag_ed_free(ed);
        return false;
    }
    ed->tty.wfd = sink_fd;
    if (!sag_grid_init(&ed->grid, &ed->interner,
                       SCREEN_ROWS, SCREEN_COLS)) {
        sag_ed_free(ed);
        return false;
    }
    ed->grid_ready = true;
    sag_render_init(&ed->render, &ed->tty.caps, NULL);
    ed->render_ready = true;
    sag_ed_layout(ed);
    return true;
}

static Key ascii_key(u8 byte)
{
    Key key = {0};

    key.kind = SAG_EV_KEY;
    key.ev = SAG_KEY_PRESS;
    key.code = byte;
    key.ntext = 1U;
    key.text[0] = byte;
    return key;
}

static bool measure_cold(const char *path, int sink_fd, i64 *median_out)
{
    i64 samples[COLD_RUNS];
    i64 work[COLD_RUNS];
    size_t run;

    for (run = 0U; run < COLD_RUNS; run++) {
        Ed ed;
        i64 start = now_ns();

        if (start < 0 || !editor_ready(&ed, path, sink_fd))
            return false;
        sag_ed_render(&ed);
        samples[run] = now_ns() - start;
        if (samples[run] < 0 || ed.quit) {
            sag_ed_free(&ed);
            return false;
        }
        latency_sink += ed.frame.len;
        sag_ed_free(&ed);
    }
    merge_sort_i64(samples, work, COLD_RUNS);
    *median_out = samples[COLD_RUNS / 2U];
    return true;
}

static bool measure_keys(const char *path, int sink_fd, i64 *p99_out)
{
    i64 *samples = malloc(LATENCY_KEYS * sizeof(*samples));
    i64 *work = malloc(LATENCY_KEYS * sizeof(*work));
    Ed ed;
    Key enter = ascii_key((u8)'i');
    size_t i;
    bool ok = false;

    if (samples == NULL || work == NULL)
        goto done_alloc;
    if (!editor_ready(&ed, path, sink_fd))
        goto done_alloc;
    /* The model is the loaded 10 kLOC fixture. Suppress journal fsync noise:
     * durability has its own torture gate and is not paint latency. */
    ed.buffer.path = NULL;
    sag_ed_handle_key(&ed, enter, 0);
    sag_ed_render(&ed);
    for (i = 0U; i < LATENCY_KEYS; i++) {
        Key key = ascii_key((u8)((i & 1U) != 0U ? 'a' : 'b'));
        i64 start = now_ns();

        sag_ed_handle_key(&ed, key, (i64)i + 1);
        sag_ed_render(&ed);
        samples[i] = now_ns() - start;
        if (samples[i] < 0 || ed.quit) {
            sag_ed_free(&ed);
            goto done_alloc;
        }
        latency_sink += ed.frame.len;
    }
    sag_ed_free(&ed);
    merge_sort_i64(samples, work, LATENCY_KEYS);
    *p99_out = samples[((size_t)LATENCY_KEYS * 99U + 99U) / 100U - 1U];
    ok = true;

done_alloc:
    free(work);
    free(samples);
    return ok;
}

int main(int argc, char **argv)
{
    char fixture[1024];
    char state[1024];
    LatencyLimits limits;
    i64 key_p99;
    i64 cold;
    int sink_fd;
    int status = 0;

    if (argc != 3 || strcmp(argv[1], "--baseline") != 0) {
        (void)fprintf(stderr, "usage: %s --baseline PATH\n", argv[0]);
        return 2;
    }
    if (!load_limits(argv[2], &limits)) {
        (void)fprintf(stderr, "latency: invalid baseline %s\n", argv[2]);
        return 2;
    }
    if (!make_fixture(fixture, sizeof(fixture), state, sizeof(state)) ||
        setenv("XDG_STATE_HOME", state, 1) != 0) {
        (void)fprintf(stderr, "latency: cannot create 10 kLOC fixture\n");
        return 2;
    }
    sink_fd = open("/dev/null", O_WRONLY);
    if (sink_fd < 0 || !measure_cold(fixture, sink_fd, &cold) ||
        !measure_keys(fixture, sink_fd, &key_p99)) {
        (void)fprintf(stderr, "latency: measurement failed\n");
        if (sink_fd >= 0)
            (void)close(sink_fd);
        remove_fixture(fixture, state);
        return 2;
    }
    (void)close(sink_fd);
    remove_fixture(fixture, state);

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
