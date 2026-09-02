#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700

/* Sprint 55: filesystem-only package integrity and lockfile budgets. */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "mod/plug/pkg.h"
#include "perf_policy.h"
#include "util/sort.h"
#include "util/xdg.h"

enum {
    PKG_PERF_PLUGINS = 20,
    PKG_PERF_PLUGIN_BYTES = 24 * 1024,
    PKG_PERF_LOCK_ENTRIES = 100,
    /* With 1001 observations, nearest-rank p99 permits ten scheduler
     * outliers while still failing when one percent of operations miss the
     * budget.  The larger window keeps this sub-millisecond filesystem gate
     * from depending on two noisy samples from a short hosted-CI run. */
    PKG_PERF_SAMPLES = 1001,
    PKG_HASH_BUDGET_NS = 3000000,
    PKG_LOCK_BUDGET_NS = 1000000
};

typedef struct PkgPerfFix {
    char root[256];
    char data[320];
    char plugins[384];
    char *old_data;
} PkgPerfFix;

typedef struct PkgTiming {
    const char *name;
    u64 median_ns;
    u64 p99_ns;
    u64 budget_ns;
} PkgTiming;

static u64 now_ns(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        (void)fprintf(stderr, "perf_pkg: clock_gettime: %s\n",
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

static bool write_payload(const char *path, size_t len, unsigned seed)
{
    u8 block[4096];
    size_t off = 0U;
    int fd;

    for (size_t i = 0U; i < sizeof(block); i++)
        block[i] = (u8)('a' + (unsigned)(i + seed) % 26U);
    fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0)
        return false;
    while (off < len) {
        size_t want = len - off < sizeof(block) ? len - off : sizeof(block);
        ssize_t wrote = write(fd, block, want);

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

static bool make_plugins(PkgPerfFix *f)
{
    unsigned i;

    for (i = 0U; i < PKG_PERF_PLUGINS; i++) {
        char dir[448];
        char payload[480];
        int n;

        n = snprintf(dir, sizeof(dir), "%s/plug-%02u", f->plugins, i);
        if (n <= 0 || (size_t)n >= sizeof(dir) || !yew_mkdirs(dir, 0700U))
            return false;
        n = snprintf(payload, sizeof(payload), "%s/payload.fl", dir);
        if (n <= 0 || (size_t)n >= sizeof(payload) ||
            !write_payload(payload, PKG_PERF_PLUGIN_BYTES, i))
            return false;
    }
    return true;
}

static bool lock_fill(PkgLock *lock)
{
    size_t i;

    lock->v.data = calloc(PKG_PERF_LOCK_ENTRIES, sizeof(*lock->v.data));
    if (lock->v.data == NULL)
        return false;
    lock->v.len = PKG_PERF_LOCK_ENTRIES;
    lock->v.cap = PKG_PERF_LOCK_ENTRIES;
    for (i = 0U; i < PKG_PERF_LOCK_ENTRIES; i++) {
        PkgEntry *entry = &lock->v.data[i];
        unsigned id = (unsigned)(PKG_PERF_LOCK_ENTRIES - i - 1U);
        char name[32];
        char url[96];
        char rev[YEW_PKG_REV_HEX + 1U];
        char tree[YEW_PKG_TREE_HEX + 1U];
        int n;
        size_t k;

        n = snprintf(name, sizeof(name), "perf-plugin-%03u", id);
        if (n <= 0 || (size_t)n >= sizeof(name))
            return false;
        n = snprintf(url, sizeof(url), "file:///fixture/perf-plugin-%03u", id);
        if (n <= 0 || (size_t)n >= sizeof(url))
            return false;
        for (k = 0U; k < YEW_PKG_REV_HEX; k++)
            rev[k] = "0123456789abcdef"[(k + id) & 15U];
        rev[YEW_PKG_REV_HEX] = '\0';
        for (k = 0U; k < YEW_PKG_TREE_HEX; k++)
            tree[k] = "fedcba9876543210"[(k + id) & 15U];
        tree[YEW_PKG_TREE_HEX] = '\0';
        entry->name = arena_strdup(&lock->a, name);
        entry->url = arena_strdup(&lock->a, url);
        entry->shorthand = arena_strdup(&lock->a, "");
        entry->pin = arena_strdup(&lock->a, "head");
        (void)memcpy(entry->rev, rev, sizeof(entry->rev));
        (void)memcpy(entry->tree, tree, sizeof(entry->tree));
        entry->installed_at = 1000000 + (i64)id;
        entry->updated_at = 1000000 + (i64)id;
        entry->extra = FL_OBJ_V(FL_MAP, fl_map_new(&lock->vm));
    }
    return true;
}

static bool fix_init(PkgPerfFix *f)
{
    PkgLock lock;
    int n;
    bool ok;

    (void)memset(f, 0, sizeof(*f));
    (void)memcpy(f->root, "/tmp/yew-perf-pkg-XXXXXX",
                 sizeof("/tmp/yew-perf-pkg-XXXXXX"));
    if (mkdtemp(f->root) == NULL)
        return false;
    n = snprintf(f->data, sizeof(f->data), "%s/data", f->root);
    if (n <= 0 || (size_t)n >= sizeof(f->data))
        return false;
    n = snprintf(f->plugins, sizeof(f->plugins), "%s/yew/plugins", f->data);
    if (n <= 0 || (size_t)n >= sizeof(f->plugins) ||
        !yew_mkdirs(f->plugins, 0700U))
        return false;
    f->old_data = copy_env("XDG_DATA_HOME");
    if (setenv("XDG_DATA_HOME", f->data, 1) != 0 || !make_plugins(f))
        return false;
    (void)memset(&lock, 0, sizeof(lock));
    yew_pkg_lock_init(&lock);
    ok = lock_fill(&lock) && yew_pkg_lock_save(&lock, NULL);
    yew_pkg_lock_free(&lock);
    return ok;
}

static bool fix_done(PkgPerfFix *f)
{
    bool ok = restore_env("XDG_DATA_HOME", f->old_data);

    free(f->old_data);
    if (!yew_rmtree(f->data, f->root, NULL))
        ok = false;
    if (rmdir(f->root) != 0)
        ok = false;
    return ok;
}

static bool measure_hash(const PkgPerfFix *f, u64 *samples)
{
    unsigned sample;

    for (sample = 0U; sample < PKG_PERF_SAMPLES; sample++) {
        u64 started = now_ns();
        unsigned i;

        for (i = 0U; i < PKG_PERF_PLUGINS; i++) {
            char dir[448];
            char hash[YEW_PKG_TREE_HEX + 1U];
            int n = snprintf(dir, sizeof(dir), "%s/plug-%02u", f->plugins,
                             i);

            if (n <= 0 || (size_t)n >= sizeof(dir) ||
                !yew_pkg_tree_hash(dir, hash, NULL) ||
                strlen(hash) != YEW_PKG_TREE_HEX)
                return false;
        }
        samples[sample] = now_ns() - started;
    }
    return true;
}

static bool measure_lock(u64 *samples)
{
    unsigned sample;

    {
        PkgLock dirty;
        PkgLock verify;
        i64 expected;

        (void)memset(&dirty, 0, sizeof(dirty));
        (void)memset(&verify, 0, sizeof(verify));
        yew_pkg_lock_init(&dirty);
        if (!yew_pkg_lock_load(&dirty, NULL) ||
            dirty.v.len != PKG_PERF_LOCK_ENTRIES) {
            yew_pkg_lock_free(&dirty);
            return false;
        }
        expected = dirty.v.data[0].updated_at + 1;
        dirty.v.data[0].updated_at = expected;
        if (!yew_pkg_lock_save(&dirty, NULL)) {
            yew_pkg_lock_free(&dirty);
            return false;
        }
        yew_pkg_lock_free(&dirty);
        yew_pkg_lock_init(&verify);
        if (!yew_pkg_lock_load(&verify, NULL) ||
            verify.v.len != PKG_PERF_LOCK_ENTRIES ||
            verify.v.data[0].updated_at != expected) {
            yew_pkg_lock_free(&verify);
            return false;
        }
        yew_pkg_lock_free(&verify);
    }

    for (sample = 0U; sample < PKG_PERF_SAMPLES; sample++) {
        PkgLock lock;
        u64 started;

        (void)memset(&lock, 0, sizeof(lock));
        yew_pkg_lock_init(&lock);
        started = now_ns();
        if (!yew_pkg_lock_load(&lock, NULL) ||
            lock.v.len != PKG_PERF_LOCK_ENTRIES ||
            !yew_pkg_lock_save(&lock, NULL)) {
            yew_pkg_lock_free(&lock);
            return false;
        }
        samples[sample] = now_ns() - started;
        yew_pkg_lock_free(&lock);
    }
    return true;
}

static int cmp_u64(const void *left, const void *right, void *ctx)
{
    u64 a = *(const u64 *)left;
    u64 b = *(const u64 *)right;

    (void)ctx;
    return a < b ? -1 : a > b ? 1 : 0;
}

static void summarize(u64 *samples, PkgTiming *timing)
{
    yew_sort_stable(samples, PKG_PERF_SAMPLES, sizeof(*samples), cmp_u64,
                    NULL);
    timing->median_ns = samples[PKG_PERF_SAMPLES / 2U];
    timing->p99_ns = samples[(PKG_PERF_SAMPLES * 99U) / 100U];
}

int main(int argc, char **argv)
{
    PkgPerfFix fix;
    PkgTiming rows[] = {
        {"tree_hash_20", 0U, 0U, PKG_HASH_BUDGET_NS},
        {"lock_load_save_100", 0U, 0U, PKG_LOCK_BUDGET_NS}
    };
    u64 hash_samples[PKG_PERF_SAMPLES];
    u64 lock_samples[PKG_PERF_SAMPLES];
    bool measure = argc == 2 && strcmp(argv[1], "--measure") == 0;
    bool advisory = yew_perf_advisory();
    int status = 0;
    size_t i;

    if (argc > 2 || (argc == 2 && !measure)) {
        (void)fprintf(stderr, "usage: %s [--measure]\n", argv[0]);
        return 2;
    }
    if (!fix_init(&fix)) {
        (void)fprintf(stderr, "perf_pkg: fixture setup failed\n");
        return 2;
    }
    if (!measure_hash(&fix, hash_samples) || !measure_lock(lock_samples)) {
        (void)fprintf(stderr, "perf_pkg: measurement invariant failed\n");
        (void)fix_done(&fix);
        return 2;
    }
    if (!fix_done(&fix)) {
        (void)fprintf(stderr, "perf_pkg: fixture cleanup failed\n");
        return 2;
    }
    summarize(hash_samples, &rows[0]);
    summarize(lock_samples, &rows[1]);
    for (i = 0U; i < YEW_ARRAY_LEN(rows); i++) {
        (void)printf("pkg.%s median_ns=%llu p99_ns=%llu budget_ns=%llu%s\n",
                     rows[i].name,
                     (unsigned long long)rows[i].median_ns,
                     (unsigned long long)rows[i].p99_ns,
                     (unsigned long long)rows[i].budget_ns,
                     yew_perf_timing_verdict(rows[i].p99_ns,
                                             rows[i].budget_ns, advisory));
        if (measure)
            (void)printf("%s %llu %llu\n", rows[i].name,
                         (unsigned long long)rows[i].median_ns,
                         (unsigned long long)rows[i].p99_ns);
        if (yew_perf_timing_failed(rows[i].p99_ns, rows[i].budget_ns,
                                   advisory))
            status = 1;
    }
    return status;
}
