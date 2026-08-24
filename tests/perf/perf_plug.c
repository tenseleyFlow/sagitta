#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "edit/ed.h"
#include "mod/plug/plug.h"
#include "util/sort.h"
#include "util/xdg.h"

enum {
    PLUG_DISCOVERY_COUNT = 20,
    PLUG_ENABLE_COUNT = 10,
    PLUG_PERF_SAMPLES = 31,
    PLUG_DISCOVERY_BUDGET_NS = 2000000,
    PLUG_ENABLE_BUDGET_NS = 5000000
};

typedef struct PlugPerfFix {
    char root[256];
    char data[320];
    char enabled_data[320];
    char empty_data[320];
    char config[320];
    char state[320];
    char workspace[320];
    char plugins[384];
    char enabled_plugins[384];
    char *old_data;
    char *old_config;
    char *old_state;
} PlugPerfFix;

typedef struct PlugTiming {
    const char *name;
    u64 median_ns;
    u64 p99_ns;
    u64 baseline_median_ns;
    u64 baseline_p99_ns;
    u64 budget_ns;
} PlugTiming;

static u64 now_ns(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        (void)fprintf(stderr, "perf_plug: clock_gettime: %s\n",
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
        ssize_t n = write(fd, text + off, len - off);

        if (n <= 0) {
            (void)close(fd);
            return false;
        }
        off += (size_t)n;
    }
    return close(fd) == 0;
}

static bool path_format(char *out, size_t cap, const char *fmt,
                        const char *a, unsigned i)
{
    int n = snprintf(out, cap, fmt, a, i);

    return n > 0 && (size_t)n < cap;
}

static bool make_plugins(const char *plugins, unsigned count)
{
    unsigned i;

    for (i = 0U; i < count; i++) {
        char src[480];
        char manifest[512];
        char entry[512];
        char body[512];
        int n;

        if (!path_format(src, sizeof(src), "%s/plug-%02u/src", plugins,
                         i) ||
            !path_format(manifest, sizeof(manifest),
                         "%s/plug-%02u/plugin.fl", plugins, i) ||
            !path_format(entry, sizeof(entry),
                         "%s/plug-%02u/src/main.fl", plugins, i) ||
            !yew_mkdirs(src, 0700U))
            return false;
        n = snprintf(body, sizeof(body),
                     "{name: \"plug-%02u\", version: \"1.0.0\", api: 1, "
                     "entry: \"src/main.fl\", capabilities: [], events: [], "
                     "description: \"performance fixture\"}\n", i);
        if (n <= 0 || (size_t)n >= sizeof(body) ||
            !write_all(manifest, body) ||
            !write_all(entry, "fn init(ctx) { nil }\n"))
            return false;
    }
    return true;
}

static bool fix_init(PlugPerfFix *f)
{
    int n;

    (void)memset(f, 0, sizeof(*f));
    (void)memcpy(f->root, "/tmp/yew-perf-plug-XXXXXX",
                 sizeof("/tmp/yew-perf-plug-XXXXXX"));
    if (mkdtemp(f->root) == NULL)
        return false;
    n = snprintf(f->data, sizeof(f->data), "%s/data", f->root);
    if (n <= 0 || (size_t)n >= sizeof(f->data))
        return false;
    n = snprintf(f->config, sizeof(f->config), "%s/config", f->root);
    if (n <= 0 || (size_t)n >= sizeof(f->config))
        return false;
    n = snprintf(f->state, sizeof(f->state), "%s/state", f->root);
    if (n <= 0 || (size_t)n >= sizeof(f->state))
        return false;
    n = snprintf(f->workspace, sizeof(f->workspace), "%s/work", f->root);
    if (n <= 0 || (size_t)n >= sizeof(f->workspace))
        return false;
    n = snprintf(f->plugins, sizeof(f->plugins), "%s/yew/plugins", f->data);
    if (n <= 0 || (size_t)n >= sizeof(f->plugins))
        return false;
    n = snprintf(f->enabled_data, sizeof(f->enabled_data), "%s/enabled-data",
                 f->root);
    if (n <= 0 || (size_t)n >= sizeof(f->enabled_data))
        return false;
    n = snprintf(f->empty_data, sizeof(f->empty_data), "%s/empty-data",
                 f->root);
    if (n <= 0 || (size_t)n >= sizeof(f->empty_data))
        return false;
    n = snprintf(f->enabled_plugins, sizeof(f->enabled_plugins),
                 "%s/yew/plugins", f->enabled_data);
    if (n <= 0 || (size_t)n >= sizeof(f->enabled_plugins) ||
        !yew_mkdirs(f->plugins, 0700U) ||
        !yew_mkdirs(f->enabled_plugins, 0700U) ||
        !yew_mkdirs(f->empty_data, 0700U) ||
        !yew_mkdirs(f->config, 0700U) ||
        !yew_mkdirs(f->state, 0700U) ||
        !yew_mkdirs(f->workspace, 0700U))
        return false;
    f->old_data = copy_env("XDG_DATA_HOME");
    f->old_config = copy_env("XDG_CONFIG_HOME");
    f->old_state = copy_env("XDG_STATE_HOME");
    if (setenv("XDG_DATA_HOME", f->data, 1) != 0 ||
        setenv("XDG_CONFIG_HOME", f->config, 1) != 0 ||
        setenv("XDG_STATE_HOME", f->state, 1) != 0)
        return false;
    return make_plugins(f->plugins, PLUG_DISCOVERY_COUNT) &&
           make_plugins(f->enabled_plugins, PLUG_ENABLE_COUNT);
}

static bool remove_plugin_root(const char *plugins, unsigned count)
{
    unsigned i;
    bool ok = true;

    for (i = 0U; i < count; i++) {
        char dir[448];
        char src[480];
        char manifest[512];
        char entry[512];

        if (!path_format(dir, sizeof(dir), "%s/plug-%02u", plugins, i) ||
            !path_format(src, sizeof(src), "%s/plug-%02u/src", plugins,
                         i) ||
            !path_format(manifest, sizeof(manifest),
                         "%s/plug-%02u/plugin.fl", plugins, i) ||
            !path_format(entry, sizeof(entry),
                         "%s/plug-%02u/src/main.fl", plugins, i))
            return false;
        ok = unlink(entry) == 0 && ok;
        ok = unlink(manifest) == 0 && ok;
        ok = rmdir(src) == 0 && ok;
        ok = rmdir(dir) == 0 && ok;
    }
    ok = rmdir(plugins) == 0 && ok;
    {
        char yew[352];
        size_t len = strlen(plugins);

        if (len < sizeof("/plugins") - 1U)
            return false;
        len -= sizeof("/plugins") - 1U;
        if (len >= sizeof(yew))
            return false;
        (void)memcpy(yew, plugins, len);
        yew[len] = '\0';

        ok = rmdir(yew) == 0 && ok;
    }
    return ok;
}

static bool remove_plugins(const PlugPerfFix *f)
{
    bool ok = remove_plugin_root(f->plugins, PLUG_DISCOVERY_COUNT);

    ok = remove_plugin_root(f->enabled_plugins, PLUG_ENABLE_COUNT) && ok;
    ok = rmdir(f->data) == 0 && ok;
    ok = rmdir(f->enabled_data) == 0 && ok;
    ok = rmdir(f->empty_data) == 0 && ok;
    ok = rmdir(f->config) == 0 && ok;
    ok = rmdir(f->state) == 0 && ok;
    ok = rmdir(f->workspace) == 0 && ok;
    ok = rmdir(f->root) == 0 && ok;
    return ok;
}

static bool fix_done(PlugPerfFix *f)
{
    bool ok = restore_env("XDG_DATA_HOME", f->old_data) &&
              restore_env("XDG_CONFIG_HOME", f->old_config) &&
              restore_env("XDG_STATE_HOME", f->old_state);

    free(f->old_data);
    free(f->old_config);
    free(f->old_state);
    return remove_plugins(f) && ok;
}

static bool prepare_ed(Ed *ed, const PlugPerfFix *f)
{
    yew_ed_init(ed);
    ed->ws.dir = arena_strdup(&ed->arena, f->workspace);
    return yew_ed_open_scratch(ed);
}

static bool measure_discovery(const PlugPerfFix *f, u64 *samples)
{
    unsigned i;

    for (i = 0U; i < PLUG_PERF_SAMPLES; i++) {
        Ed ed;
        u64 started;

        if (!prepare_ed(&ed, f))
            return false;
        started = now_ns();
        if (!yew_plug_discover(&ed, NULL)) {
            yew_ed_free(&ed);
            return false;
        }
        samples[i] = now_ns() - started;
        if (yew_plug_count(&ed) != PLUG_DISCOVERY_COUNT) {
            yew_ed_free(&ed);
            return false;
        }
        yew_ed_free(&ed);
    }
    return true;
}

static bool cold_start(const PlugPerfFix *f, const char *data,
                       unsigned expected, u64 *elapsed)
{
    Ed ed;
    u64 started;
    unsigned i;

    if (setenv("XDG_DATA_HOME", data, 1) != 0)
        return false;
    started = now_ns();
    if (!prepare_ed(&ed, f) || !yew_plug_boot(&ed)) {
        yew_ed_free(&ed);
        return false;
    }
    *elapsed = now_ns() - started;
    if (yew_plug_count(&ed) != expected) {
        yew_ed_free(&ed);
        return false;
    }
    for (i = 0U; i < expected; i++) {
        Plug *plug = yew_plug_at(&ed, i);

        if (plug == NULL || plug->st != PLUG_ENABLED) {
            yew_ed_free(&ed);
            return false;
        }
    }
    yew_ed_free(&ed);
    return true;
}

static bool measure_enable(const PlugPerfFix *f, u64 *samples)
{
    unsigned i;

    for (i = 0U; i < PLUG_PERF_SAMPLES; i++) {
        u64 empty_ns;
        u64 enabled_ns;

        if (!cold_start(f, f->empty_data, 0U, &empty_ns) ||
            !cold_start(f, f->enabled_data, PLUG_ENABLE_COUNT,
                        &enabled_ns))
            return false;
        samples[i] = enabled_ns > empty_ns ? enabled_ns - empty_ns : 0U;
    }
    if (setenv("XDG_DATA_HOME", f->data, 1) != 0)
        return false;
    return true;
}

static int cmp_u64(const void *left, const void *right, void *ctx)
{
    const u64 a = *(const u64 *)left;
    const u64 b = *(const u64 *)right;

    (void)ctx;
    return a < b ? -1 : a > b ? 1 : 0;
}

static void summarize(u64 *samples, PlugTiming *timing)
{
    yew_sort_stable(samples, PLUG_PERF_SAMPLES, sizeof(*samples),
                    cmp_u64, NULL);
    timing->median_ns = samples[PLUG_PERF_SAMPLES / 2U];
    timing->p99_ns = samples[(PLUG_PERF_SAMPLES * 99U) / 100U];
}

static bool load_baselines(PlugTiming *rows, size_t count)
{
    FILE *fp = fopen("tests/perf/baselines/plug.txt", "r");
    char line[160];

    if (fp == NULL)
        return false;
    while (fgets(line, sizeof(line), fp) != NULL) {
        char name[64];
        unsigned long long median;
        unsigned long long p99;
        size_t i;

        if (sscanf(line, "%63s %llu %llu", name, &median, &p99) != 3)
            continue;
        for (i = 0U; i < count; i++) {
            if (strcmp(rows[i].name, name) == 0) {
                rows[i].baseline_median_ns = (u64)median;
                rows[i].baseline_p99_ns = (u64)p99;
            }
        }
    }
    if (ferror(fp) || fclose(fp) != 0)
        return false;
    for (size_t i = 0U; i < count; i++)
        if (rows[i].baseline_median_ns == 0U ||
            rows[i].baseline_p99_ns == 0U)
            return false;
    return true;
}

int main(int argc, char **argv)
{
    PlugPerfFix fix;
    PlugTiming rows[] = {
        {"discover_parse_20", 0U, 0U, 0U, 0U,
         PLUG_DISCOVERY_BUDGET_NS},
        {"enable_noop_10", 0U, 0U, 0U, 0U,
         PLUG_ENABLE_BUDGET_NS}
    };
    u64 discovery[PLUG_PERF_SAMPLES];
    u64 enable[PLUG_PERF_SAMPLES];
    bool measure = argc == 2 && strcmp(argv[1], "--measure") == 0;
    int status = 0;
    size_t i;

    if (argc > 2 || (argc == 2 && !measure)) {
        (void)fprintf(stderr, "usage: %s [--measure]\n", argv[0]);
        return 2;
    }
    if (!fix_init(&fix)) {
        (void)fprintf(stderr, "perf_plug: fixture setup failed\n");
        return 2;
    }
    if (!measure_discovery(&fix, discovery) ||
        !measure_enable(&fix, enable)) {
        (void)fprintf(stderr, "perf_plug: measurement invariant failed\n");
        (void)fix_done(&fix);
        return 2;
    }
    if (!fix_done(&fix)) {
        (void)fprintf(stderr, "perf_plug: fixture cleanup failed\n");
        return 2;
    }
    summarize(discovery, &rows[0]);
    summarize(enable, &rows[1]);
    if (!measure && !load_baselines(rows, YEW_ARRAY_LEN(rows))) {
        (void)fprintf(stderr, "perf_plug: missing or invalid baseline\n");
        return 2;
    }
    for (i = 0U; i < YEW_ARRAY_LEN(rows); i++) {
        bool regression = rows[i].p99_ns > rows[i].budget_ns;

        (void)printf("plug.%s median_ns=%llu p99_ns=%llu budget_ns=%llu%s\n",
                     rows[i].name,
                     (unsigned long long)rows[i].median_ns,
                     (unsigned long long)rows[i].p99_ns,
                     (unsigned long long)rows[i].budget_ns,
                     regression ? " REGRESSION" : " ok");
        if (measure)
            (void)printf("%s %llu %llu\n", rows[i].name,
                         (unsigned long long)rows[i].median_ns,
                         (unsigned long long)rows[i].p99_ns);
        if (regression)
            status = 1;
    }
    return status;
}
