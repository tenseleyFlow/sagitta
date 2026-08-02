#define _POSIX_C_SOURCE 200809L

/*
 * Public controls are SAG_FAULT_AT, SAG_FAULT_SHORT, SAG_FAULT_SEED and
 * SAG_FAULT_LOG.  The harness also uses SAG_FAULT_ENABLE=0 as its durable-
 * journal barrier, SAG_FAULT_RENAME_EXDEV=1 for decision-table row 5,
 * SAG_FAULT_FCHOWN_EPERM=1 for row 6, and SAG_FAULT_EINTR_AT=N to return
 * EINTR once at intercepted call N.
 */

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

typedef ssize_t (*WriteFn)(int, const void *, size_t);
typedef ssize_t (*PwriteFn)(int, const void *, size_t, off_t);
typedef int (*FsyncFn)(int);
typedef int (*RenameFn)(const char *, const char *);
typedef int (*FtruncateFn)(int, off_t);
typedef int (*FchownFn)(int, uid_t, gid_t);
typedef int (*CloseFn)(int);

static WriteFn real_write_fn;
static PwriteFn real_pwrite_fn;
static FsyncFn real_fsync_fn;
static FsyncFn real_fdatasync_fn;
static RenameFn real_rename_fn;
static FtruncateFn real_ftruncate_fn;
static FchownFn real_fchown_fn;
static CloseFn real_close_fn;
static long fault_at = -1;
static unsigned long long call_no;
static uint64_t rng_state;
static int short_writes;
static int log_fd = -1;
static int initialized;
static int resolving;
static int rename_exdev_done;
static int link_done;
static volatile sig_atomic_t signal_enabled;

static void enable_faults(int sig)
{
    (void)sig;
    signal_enabled = 1;
}

static void close_log(void)
{
    if (log_fd >= 0 && real_close_fn != NULL) {
        (void)real_close_fn(log_fd);
        log_fd = -1;
    }
}

static void load_symbol(void *dst, size_t dst_size, const char *name)
{
    void *sym = dlsym(RTLD_NEXT, name);

    if (sym == NULL || dst_size != sizeof(sym))
        _exit(126);
    memcpy(dst, &sym, sizeof(sym));
}

static unsigned long long parse_ull(const char *s, unsigned long long fallback)
{
    char *end;
    unsigned long long value;

    if (s == NULL || *s == '\0')
        return fallback;
    errno = 0;
    value = strtoull(s, &end, 10);
    if (errno != 0 || *end != '\0')
        return fallback;
    return value;
}

static int env_is_one(const char *name)
{
    const char *value = getenv(name);

    return value != NULL && strcmp(value, "1") == 0;
}

static void initialize(void)
{
    const char *at;
    const char *log_path;
    unsigned long long parsed;

    if (initialized || resolving)
        return;
    resolving = 1;
    load_symbol(&real_write_fn, sizeof(real_write_fn), "write");
    load_symbol(&real_pwrite_fn, sizeof(real_pwrite_fn), "pwrite");
    load_symbol(&real_fsync_fn, sizeof(real_fsync_fn), "fsync");
    load_symbol(&real_fdatasync_fn, sizeof(real_fdatasync_fn), "fdatasync");
    load_symbol(&real_rename_fn, sizeof(real_rename_fn), "rename");
    load_symbol(&real_ftruncate_fn, sizeof(real_ftruncate_fn), "ftruncate");
    load_symbol(&real_fchown_fn, sizeof(real_fchown_fn), "fchown");
    load_symbol(&real_close_fn, sizeof(real_close_fn), "close");

    at = getenv("SAG_FAULT_AT");
    parsed = parse_ull(at, UINT64_MAX);
    if (parsed <= (unsigned long long)LONG_MAX)
        fault_at = (long)parsed;
    rng_state = (uint64_t)parse_ull(getenv("SAG_FAULT_SEED"), 1U);
    if (rng_state == 0U)
        rng_state = UINT64_C(0x9e3779b97f4a7c15);
    short_writes = env_is_one("SAG_FAULT_SHORT");
    log_path = getenv("SAG_FAULT_LOG");
    if (log_path != NULL && *log_path != '\0') {
        log_fd = open(log_path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC,
                      0600);
        if (log_fd >= 0 && atexit(close_log) != 0)
            _exit(126);
    }
    if (env_is_one("SAG_FAULT_SIGNAL_ENABLE")) {
        struct sigaction action;

        (void)memset(&action, 0, sizeof(action));
        action.sa_handler = enable_faults;
        (void)sigemptyset(&action.sa_mask);
        if (sigaction(SIGUSR2, &action, NULL) != 0)
            _exit(126);
    }
    resolving = 0;
    initialized = 1;
}

static int faults_enabled(void)
{
    const char *enabled;

    if (env_is_one("SAG_FAULT_SIGNAL_ENABLE"))
        return signal_enabled != 0;
    enabled = getenv("SAG_FAULT_ENABLE");
    return enabled == NULL || strcmp(enabled, "0") != 0;
}

static uint64_t next_random(void)
{
    uint64_t x = rng_state;

    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    rng_state = x;
    return x;
}

static void log_call(const char *name, const char *action)
{
    char line[160];
    int n;

    if (log_fd < 0 || real_write_fn == NULL)
        return;
    n = snprintf(line, sizeof(line), "%llu %s %s\n", call_no, name, action);
    if (n > 0 && (size_t)n < sizeof(line))
        (void)real_write_fn(log_fd, line, (size_t)n);
}

static void before_call(const char *name)
{
    unsigned long long delay_us;

    initialize();
    if (!faults_enabled())
        return;
    delay_us = parse_ull(getenv("SAG_FAULT_DELAY_US"), 0U);
    if (delay_us != 0U) {
        struct timespec delay;

        delay.tv_sec = (time_t)(delay_us / UINT64_C(1000000));
        delay.tv_nsec = (long)((delay_us % UINT64_C(1000000)) * 1000U);
        while (nanosleep(&delay, &delay) != 0 && errno == EINTR)
            ;
    }
    if (fault_at >= 0 && call_no == (unsigned long long)fault_at) {
        log_call(name, "kill");
        _exit(137);
    }
    log_call(name, "pass");
    call_no++;
}

static size_t maybe_short(size_t count, const char *name)
{
    if (!faults_enabled())
        return count;
    if (!short_writes || count < 2U || (next_random() & 1U) == 0U)
        return count;
    log_call(name, "short");
    return count / 2U;
}

static int inject_eintr(const char *name)
{
    unsigned long long at;

    if (!faults_enabled())
        return 0;
    at = parse_ull(getenv("SAG_FAULT_EINTR_AT"), UINT64_MAX);
    if (call_no == 0U || at != call_no - 1U)
        return 0;
    log_call(name, "errno=EINTR");
    errno = EINTR;
    return 1;
}

static const char *sync_name(int fd, const char *file_name,
                             const char *dir_name)
{
    struct stat st;

    if (fstat(fd, &st) == 0 && S_ISDIR(st.st_mode))
        return dir_name;
    return file_name;
}

ssize_t write(int fd, const void *buf, size_t count)
{
    before_call("write");
    if (inject_eintr("write"))
        return -1;
    return real_write_fn(fd, buf, maybe_short(count, "write"));
}

ssize_t pwrite(int fd, const void *buf, size_t count, off_t offset)
{
    before_call("pwrite");
    if (inject_eintr("pwrite"))
        return -1;
    return real_pwrite_fn(fd, buf, maybe_short(count, "pwrite"), offset);
}

int fsync(int fd)
{
    const char *name = sync_name(fd, "fsync-file", "fsync-dir");

    before_call(name);
    if (inject_eintr(name))
        return -1;
    return real_fsync_fn(fd);
}

int fdatasync(int fd)
{
    const char *name = sync_name(fd, "fdatasync-file", "fdatasync-dir");

    before_call(name);
    if (inject_eintr(name))
        return -1;
    return real_fdatasync_fn(fd);
}

int rename(const char *old_path, const char *new_path)
{
    before_call("rename");
    if (env_is_one("SAG_FAULT_RENAME_EXDEV") && !rename_exdev_done) {
        rename_exdev_done = 1;
        log_call("rename", "errno=EXDEV");
        errno = EXDEV;
        return -1;
    }
    if (inject_eintr("rename"))
        return -1;
    return real_rename_fn(old_path, new_path);
}

int ftruncate(int fd, off_t length)
{
    before_call("ftruncate");
    if (inject_eintr("ftruncate"))
        return -1;
    return real_ftruncate_fn(fd, length);
}

int fchown(int fd, uid_t owner, gid_t group)
{
    before_call("fchown");
    if (env_is_one("SAG_FAULT_FCHOWN_EPERM")) {
        log_call("fchown", "errno=EPERM");
        errno = EPERM;
        return -1;
    }
    if (inject_eintr("fchown"))
        return -1;
    return real_fchown_fn(fd, owner, group);
}

int close(int fd)
{
    const char *source;
    const char *twin;

    initialize();
    if (fd == log_fd)
        return real_close_fn(fd);
    before_call("close");
    source = getenv("SAG_FAULT_LINK_SOURCE");
    twin = getenv("SAG_FAULT_LINK_TWIN");
    if (!link_done && source != NULL && twin != NULL) {
        link_done = 1;
        if (link(source, twin) != 0)
            _exit(125);
        log_call("close", "late-hardlink");
    }
    if (inject_eintr("close"))
        return -1;
    return real_close_fn(fd);
}
