#define _POSIX_C_SOURCE 200809L

/* Sprint 55: shipped cloud-preset idle conflict-scan budget. */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "edit/ed.h"
#include "edit/loop.h"
#include "fl/flruntime.h"
#include "fl/gc.h"
#include "fl/module.h"
#include "fl/vm.h"
#include "util/buf.h"
#include "util/sort.h"

enum {
    CLOUD_PERF_BUFFERS = 20,
    /* 101 samples make nearest-rank p99 the second-slowest observation.
     * A 31-sample "p99" is just the maximum and turns one involuntary
     * deschedule into a false scan-cost regression. */
    CLOUD_PERF_SAMPLES = 101,
    CLOUD_SCAN_BUDGET_NS = 2000000
};

typedef struct CloudPerfFix {
    char root[256];
    char *old_runtime;
    Ed ed;
    FlValue scan;
} CloudPerfFix;

static volatile u64 cloud_perf_sink;

static u64 now_ns(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        (void)fprintf(stderr, "perf_cloud: clock_gettime: %s\n",
                      strerror(errno));
        return 0U;
    }
    return (u64)ts.tv_sec * UINT64_C(1000000000) + (u64)ts.tv_nsec;
}

static char *copy_env(const char *name)
{
    const char *value = getenv(name);
    size_t len;
    char *copy;

    if (value == NULL)
        return NULL;
    len = strlen(value);
    copy = malloc(len + 1U);
    if (copy != NULL)
        (void)memcpy(copy, value, len + 1U);
    return copy;
}

static bool restore_env(const char *name, char *value)
{
    return value == NULL ? unsetenv(name) == 0 : setenv(name, value, 1) == 0;
}

static bool write_all(const char *path, const char *text)
{
    size_t len = strlen(text);
    size_t off = 0U;
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);

    if (fd < 0)
        return false;
    while (off < len) {
        ssize_t wrote = write(fd, text + off, len - off);

        if (wrote < 0 && errno == EINTR)
            continue;
        if (wrote <= 0) {
            (void)close(fd);
            return false;
        }
        off += (size_t)wrote;
    }
    return close(fd) == 0;
}

static bool make_files(CloudPerfFix *f, Bytebuf *setup)
{
    unsigned i;

    bytebuf_append(setup,
                   "import buf\nimport io\n"
                   "import \"preset.cloud.fl\" as cloud\n",
                   sizeof("import buf\nimport io\n"
                          "import \"preset.cloud.fl\" as cloud\n") - 1U);
    bytebuf_append(setup,
                   "on(\"ed.idle\", fn() {\n"
                   "  return io.write(\"",
                   sizeof("on(\"ed.idle\", fn() {\n"
                          "  return io.write(\"") - 1U);
    bytebuf_append(setup, f->root, strlen(f->root));
    bytebuf_append(setup,
                   "/idle-count\", io.read(\"",
                   sizeof("/idle-count\", io.read(\"") - 1U);
    bytebuf_append(setup, f->root, strlen(f->root));
    bytebuf_append(setup,
                   "/idle-count\") + \"x\")\n"
                   "})\n",
                   sizeof("/idle-count\") + \"x\")\n"
                          "})\n") - 1U);
    {
        char idle_count[320];
        int n = snprintf(idle_count, sizeof(idle_count), "%s/idle-count",
                         f->root);

        if (n <= 0 || (size_t)n >= sizeof(idle_count) ||
            !write_all(idle_count, ""))
            return false;
    }
    bytebuf_append(setup, "cloud.watch(\"", sizeof("cloud.watch(\"") - 1U);
    bytebuf_append(setup, f->root, strlen(f->root));
    bytebuf_append(setup, "\")\n", sizeof("\")\n") - 1U);
    for (i = 0U; i < CLOUD_PERF_BUFFERS; i++) {
        char dir[320];
        char path[384];
        char twin[416];
        int n;

        n = snprintf(dir, sizeof(dir), "%s/watched-%02u", f->root, i);
        if (n <= 0 || (size_t)n >= sizeof(dir) || mkdir(dir, 0700) != 0)
            return false;
        n = snprintf(path, sizeof(path), "%s/file-%02u.txt", dir, i);
        if (n <= 0 || (size_t)n >= sizeof(path) || !write_all(path, "base\n"))
            return false;
        n = snprintf(twin, sizeof(twin),
                     "%s/file-%02u.sync-conflict-peer.txt", dir, i);
        if (n <= 0 || (size_t)n >= sizeof(twin) ||
            !write_all(twin, "remote\n"))
            return false;
        bytebuf_append(setup, "buf.open(\"", sizeof("buf.open(\"") - 1U);
        bytebuf_append(setup, path, strlen(path));
        bytebuf_append(setup, "\")\n", sizeof("\")\n") - 1U);
    }
    return true;
}

static bool global_get(FlVm *vm, FlMap *map, const char *name, FlValue *out)
{
    FlStr *key = fl_str_new(vm, name, (u32)strlen(name));

    return key != NULL &&
           fl_map_get(map, FL_OBJ_V(FL_STR, key), out);
}

static bool find_scan(CloudPerfFix *f)
{
    FlVm *vm = yew_fl_vm(&f->ed);
    u32 i;

    if (vm == NULL)
        return false;
    for (i = 0U; i < vm->mods.n; i++) {
        FlModule *module = &vm->mods.v[i];

        if (module->state == (u8)FL_MOD_READY && module->exports != NULL &&
            global_get(vm, module->exports, "scan", &f->scan) &&
            (f->scan.t == (u8)FL_CLOSURE ||
             f->scan.t == (u8)FL_NATIVE))
            return true;
    }
    return false;
}

static bool run_scan(CloudPerfFix *f, u32 *found)
{
    FlValue out = FL_NIL_V;
    FlList *list;

    if (!fl_call_value_args(f->ed.fl, f->scan, NULL, 0U, YEW_SRC_FLETCH,
                            &out) ||
        out.t != (u8)FL_LIST)
        return false;
    list = (FlList *)out.as.o;
    *found = list->n;
    cloud_perf_sink += list->n;
    return true;
}

static const char *disable_probe(const char *name)
{
    return strcmp(name, "YEW_TTY_PROBE") == 0 ? "0" : NULL;
}

static void stop_loop(Ed *ed, void *ctx)
{
    (void)ctx;
    ed->quit = true;
}

static bool idle_runs_once(CloudPerfFix *f)
{
    char path[320];
    char count[2] = {0};
    i64 now = yew_now_ms();
    ssize_t nread;
    int fd;
    int status;

    if (yew_fl_vm(&f->ed) == NULL ||
        snprintf(path, sizeof(path), "%s/idle-count", f->root) <= 0)
        return false;
    f->ed.headless = true;
    f->ed.tty.rfd = -1;
    f->ed.tty.sigpipe[0] = -1;
    yew_tty_probe_config(&f->ed.tty, now, disable_probe);
    f->ed.probe_seeded = true;
    f->ed.layout_dirty = false;
    f->ed.fl_idle_since_ms = now - 500;
    f->ed.fl_idle_fired = false;
    /* Keep the loop alive for repeated iterations in the same idle period. */
    (void)yew_timer_add(&f->ed.timers, now + 10, stop_loop, NULL);
    status = yew_loop_run(&f->ed);
    fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return false;
    nread = read(fd, count, sizeof(count));
    (void)close(fd);
    return status == YEW_EXIT_OK && f->ed.fl_idle_fired && nread == 1 &&
           count[0] == 'x';
}

static bool fix_init(CloudPerfFix *f)
{
    Bytebuf setup;
    char *runtime;
    u32 found = 0U;
    bool ok;

    (void)memset(f, 0, sizeof(*f));
    (void)memcpy(f->root, "/tmp/yew-perf-cloud-XXXXXX",
                 sizeof("/tmp/yew-perf-cloud-XXXXXX"));
    if (mkdtemp(f->root) == NULL)
        return false;
    runtime = realpath("runtime", NULL);
    f->old_runtime = copy_env("YEW_RUNTIME_DIR");
    if (runtime == NULL || setenv("YEW_RUNTIME_DIR", runtime, 1) != 0) {
        free(runtime);
        return false;
    }
    free(runtime);
    bytebuf_init(&setup);
    ok = make_files(f, &setup);
    if (!ok)
        (void)fprintf(stderr, "perf_cloud: cannot create watched files\n");
    yew_ed_init(&f->ed);
    f->ed.headless = true;
    if (ok && !yew_ed_open_scratch(&f->ed)) {
        (void)fprintf(stderr, "perf_cloud: cannot open scratch buffer\n");
        ok = false;
    }
    if (ok && yew_fl_eval(&f->ed, (const char *)setup.data,
                          (u32)setup.len) != YEW_CMD_OK) {
        (void)fprintf(stderr, "perf_cloud: preset setup did not evaluate: %s\n",
                      f->ed.msg.text);
        ok = false;
    }
    if (ok && f->ed.ws.nbufs != CLOUD_PERF_BUFFERS + 1U) {
        (void)fprintf(stderr, "perf_cloud: opened %u buffers, expected %u\n",
                      f->ed.ws.nbufs, CLOUD_PERF_BUFFERS + 1U);
        ok = false;
    }
    if (ok && !find_scan(f)) {
        (void)fprintf(stderr, "perf_cloud: cloud.scan is not callable\n");
        ok = false;
    }
    if (ok && (!run_scan(f, &found) || found != CLOUD_PERF_BUFFERS)) {
        (void)fprintf(stderr, "perf_cloud: warm scan found %u, expected %u\n",
                      found, CLOUD_PERF_BUFFERS);
        ok = false;
    }
    bytebuf_free(&setup);
    return ok;
}

static bool fix_done(CloudPerfFix *f)
{
    bool ok = true;
    unsigned i;

    yew_ed_free(&f->ed);
    {
        char idle_count[320];
        int n = snprintf(idle_count, sizeof(idle_count), "%s/idle-count",
                         f->root);

        if (n <= 0 || (size_t)n >= sizeof(idle_count))
            return false;
        ok = unlink(idle_count) == 0 && ok;
    }
    for (i = 0U; i < CLOUD_PERF_BUFFERS; i++) {
        char dir[320];
        char path[384];
        char twin[416];
        int n;

        n = snprintf(dir, sizeof(dir), "%s/watched-%02u", f->root, i);
        if (n <= 0 || (size_t)n >= sizeof(dir))
            return false;
        n = snprintf(path, sizeof(path), "%s/file-%02u.txt", dir, i);
        if (n <= 0 || (size_t)n >= sizeof(path))
            return false;
        n = snprintf(twin, sizeof(twin),
                     "%s/file-%02u.sync-conflict-peer.txt", dir, i);
        if (n <= 0 || (size_t)n >= sizeof(twin))
            return false;
        ok = unlink(path) == 0 && ok;
        ok = unlink(twin) == 0 && ok;
        ok = rmdir(dir) == 0 && ok;
    }
    ok = rmdir(f->root) == 0 && ok;
    ok = restore_env("YEW_RUNTIME_DIR", f->old_runtime) && ok;
    free(f->old_runtime);
    return ok;
}

static int cmp_u64(const void *left, const void *right, void *ctx)
{
    const u64 a = *(const u64 *)left;
    const u64 b = *(const u64 *)right;

    (void)ctx;
    return a < b ? -1 : a > b ? 1 : 0;
}

int main(int argc, char **argv)
{
    CloudPerfFix fix;
    u64 samples[CLOUD_PERF_SAMPLES];
    u64 median;
    u64 p99;
    unsigned i;
    bool measure = argc == 2 && strcmp(argv[1], "--measure") == 0;

    if (argc > 2 || (argc == 2 && !measure)) {
        (void)fprintf(stderr, "usage: %s [--measure]\n", argv[0]);
        return 2;
    }
    if (!fix_init(&fix)) {
        (void)fprintf(stderr, "perf_cloud: fixture setup failed\n");
        return 2;
    }
    for (i = 0U; i < CLOUD_PERF_SAMPLES; i++) {
        u32 found = 0U;
        u64 started = now_ns();

        if (!run_scan(&fix, &found) || found != CLOUD_PERF_BUFFERS) {
            (void)fprintf(stderr, "perf_cloud: scan invariant failed\n");
            (void)fix_done(&fix);
            return 2;
        }
        samples[i] = now_ns() - started;
    }
    if (!idle_runs_once(&fix)) {
        (void)fprintf(stderr,
                      "perf_cloud: idle hook did not run exactly once\n");
        (void)fix_done(&fix);
        return 2;
    }
    if (!fix_done(&fix)) {
        (void)fprintf(stderr, "perf_cloud: fixture cleanup failed\n");
        return 2;
    }
    yew_sort_stable(samples, CLOUD_PERF_SAMPLES, sizeof(*samples), cmp_u64,
                    NULL);
    median = samples[CLOUD_PERF_SAMPLES / 2U];
    p99 = samples[(CLOUD_PERF_SAMPLES * 99U) / 100U];
    (void)printf("cloud.idle_scan_20 median_ns=%llu p99_ns=%llu "
                 "budget_ns=%u%s\n",
                 (unsigned long long)median, (unsigned long long)p99,
                 CLOUD_SCAN_BUDGET_NS,
                 p99 > CLOUD_SCAN_BUDGET_NS ? " REGRESSION" : " ok");
    if (measure)
        (void)printf("idle_scan_20 %llu %llu\n",
                     (unsigned long long)median, (unsigned long long)p99);
    return p99 > CLOUD_SCAN_BUDGET_NS ? 1 : 0;
}
