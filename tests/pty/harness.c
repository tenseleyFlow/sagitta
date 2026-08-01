#define _XOPEN_SOURCE 700
#include <fcntl.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "harness.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>

#include "snapshot.h"
#include "term/input.h"
#include "term/tty.h"

enum {
    PTC_DEFAULT_BUDGET_MS = 5000,
    PTC_DEFAULT_QUIET_MS = 50,
    PTC_KILL_BUDGET_MS = 1000,
    PTC_LIVE_MAX = 64
};

static Pty *live_children[PTC_LIVE_MAX];
static size_t nlive;

i64 ptc_now_ms(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return 0;
    return (i64)ts.tv_sec * 1000 + (i64)ts.tv_nsec / 1000000;
}

static void ptc_fail(PtyCtx *c, const char *fmt, ...)
{
    va_list ap;

    if (c == NULL || c->failed)
        return;
    c->failed = true;
    va_start(ap, fmt);
    (void)vsnprintf(c->failure, sizeof(c->failure), fmt, ap);
    va_end(ap);
}

static i64 add_ms(i64 start, i64 delta)
{
    if (delta < 0 || start > INT64_MAX - delta)
        return INT64_MAX;
    return start + delta;
}

static i64 case_deadline(const PtyCtx *c)
{
    i64 deadline = add_ms(c->pty.started_ms, c->budget_ms);

    if (c->global_deadline_ms > 0 && c->global_deadline_ms < deadline)
        deadline = c->global_deadline_ms;
    return deadline;
}

static void live_add(Pty *p)
{
    live_children[nlive++] = p;
}

static void live_remove(Pty *p)
{
    size_t i;

    for (i = 0U; i < nlive; i++) {
        if (live_children[i] != p)
            continue;
        live_children[i] = live_children[nlive - 1U];
        nlive--;
        return;
    }
}

bool sag_pty_spawn(Pty *p, const PtySpec *sp)
{
    char sname[128];
    int m;
    pid_t pid;

    if (p == NULL || sp == NULL || sp->path == NULL || sp->argv == NULL ||
        sp->envp == NULL || sp->rows == 0U || sp->cols == 0U) {
        errno = EINVAL;
        return false;
    }
    m = posix_openpt(O_RDWR | O_NOCTTY);
    if (m < 0)
        return false;
    if (fcntl(m, F_SETFD, FD_CLOEXEC) < 0)
        goto fail;
    if (grantpt(m) < 0 || unlockpt(m) < 0)
        goto fail;
    {
        const char *s = ptsname(m);
        size_t n;

        if (s == NULL)
            goto fail;
        n = strlen(s);
        if (n + 1U > sizeof(sname))
            goto fail;
        (void)memcpy(sname, s, n + 1U);
    }
    if (nlive >= PTC_LIVE_MAX) {
        errno = EAGAIN;
        goto fail;
    }
    if (fcntl(m, F_SETFL, O_NONBLOCK) < 0)
        goto fail;
    pid = fork();
    if (pid < 0)
        goto fail;
    if (pid == 0) {
        struct winsize ws;
        int s;

        if (setsid() < 0)
            _exit(127);
        s = open(sname, O_RDWR);
        if (s < 0)
            _exit(127);
#ifdef TIOCSCTTY
        (void)ioctl(s, TIOCSCTTY, 0);
#endif
        (void)memset(&ws, 0, sizeof(ws));
        ws.ws_row = sp->rows;
        ws.ws_col = sp->cols;
        (void)ioctl(s, TIOCSWINSZ, &ws);
        if (dup2(s, STDIN_FILENO) < 0 || dup2(s, STDOUT_FILENO) < 0 ||
            dup2(s, STDERR_FILENO) < 0)
            _exit(127);
        if (s > STDERR_FILENO)
            (void)close(s);
        (void)close(m);
        execve(sp->path, sp->argv, sp->envp);
        _exit(127);
    }
    p->master = m;
    p->pid = pid;
    p->rows = sp->rows;
    p->cols = sp->cols;
    p->reaped = false;
    p->status = -1;
    p->started_ms = ptc_now_ms();
    live_add(p);
    return true;

fail:
    (void)close(m);
    return false;
}

static bool write_master(PtyCtx *c, const u8 *bytes, size_t len)
{
    i64 deadline = case_deadline(c);

    while (len != 0U && !c->failed) {
        ssize_t n = write(c->pty.master, bytes, len);

        if (n > 0) {
            bytes += (size_t)n;
            len -= (size_t)n;
        } else if (n < 0 && errno == EINTR) {
            continue;
        } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            struct pollfd fd = {c->pty.master, POLLOUT, 0};
            i64 left = deadline - ptc_now_ms();
            int timeout = left <= 0 ? 0 : left > 1000 ? 1000 : (int)left;

            if (poll(&fd, 1U, timeout) < 0 && errno != EINTR) {
                ptc_fail(c, "poll for pty write: %s", strerror(errno));
                return false;
            }
        } else {
            ptc_fail(c, "write to child: %s", strerror(errno));
            return false;
        }
        if (ptc_now_ms() >= deadline) {
            c->timed_out = true;
            ptc_fail(c, "case timed out while writing to child");
        }
    }
    return !c->failed;
}

static void reap_nonblocking(PtyCtx *c)
{
    int status;
    pid_t result;

    if (!c->spawned || c->pty.reaped)
        return;
    do {
        result = waitpid(c->pty.pid, &status, WNOHANG);
    } while (result < 0 && errno == EINTR);
    if (result == c->pty.pid) {
        c->pty.reaped = true;
        c->pty.status = status;
        live_remove(&c->pty);
    } else if (result < 0 && errno != ECHILD) {
        ptc_fail(c, "waitpid: %s", strerror(errno));
    }
}

static bool read_available(PtyCtx *c, bool *activity)
{
    u8 bytes[8192];

    for (;;) {
        ssize_t n = read(c->pty.master, bytes, sizeof(bytes));

        if (n > 0) {
            Bytebuf replies;

            *activity = true;
            bytebuf_append(&c->raw, bytes, (size_t)n);
            vt_feed(&c->vt, bytes, (size_t)n);
            bytebuf_init(&replies);
            vt_take_replies(&c->vt, &replies);
            if (replies.len != 0U)
                (void)write_master(c, replies.data, replies.len);
            bytebuf_free(&replies);
            if (c->vt.alt)
                c->ready = true;
        } else if (n == 0 || (n < 0 && errno == EIO)) {
            c->eof = true;
            return true;
        } else if (n < 0 && errno == EINTR) {
            continue;
        } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return true;
        } else {
            ptc_fail(c, "read from child: %s", strerror(errno));
            return false;
        }
    }
}

static void validate_vt(PtyCtx *c)
{
    const char *errors = c->vt.errors.data == NULL
                             ? "" : (char *)c->vt.errors.data;

    if (c->vt.nerrors != 0U)
        ptc_fail(c, "VT rejected output: %.*s",
                 (int)c->vt.errors.len, errors);
}

static void pump_quiet(PtyCtx *c, i64 quiet_ms, bool need_ready)
{
    i64 quiet_deadline;
    i64 deadline;

    if (c == NULL || !c->spawned || c->failed)
        return;
    if (quiet_ms <= 0)
        quiet_ms = PTC_DEFAULT_QUIET_MS;
    deadline = case_deadline(c);
    quiet_deadline = add_ms(ptc_now_ms(), quiet_ms);
    while (!c->failed) {
        struct pollfd fd = {c->pty.master, POLLIN | POLLHUP, 0};
        i64 now = ptc_now_ms();
        i64 until = (need_ready && !c->ready && !c->eof)
                        ? deadline : quiet_deadline;
        i64 left;
        int timeout;
        int result;
        bool activity = false;

        reap_nonblocking(c);
        if (now >= deadline) {
            i64 effective_ms = deadline > c->pty.started_ms
                                   ? deadline - c->pty.started_ms : 0;

            c->timed_out = true;
            ptc_fail(c, "case timed out after %lld ms",
                     (long long)effective_ms);
            break;
        }
        if ((!need_ready || c->ready || c->eof) && now >= quiet_deadline)
            break;
        left = until - now;
        timeout = left <= 0 ? 0 : left > 250 ? 250 : (int)left;
        result = poll(&fd, 1U, timeout);
        if (result < 0 && errno == EINTR)
            continue;
        if (result < 0) {
            ptc_fail(c, "poll pty: %s", strerror(errno));
            break;
        }
        if (result > 0 && (fd.revents & (POLLIN | POLLHUP | POLLERR)) != 0)
            (void)read_available(c, &activity);
        if (activity)
            quiet_deadline = add_ms(ptc_now_ms(), quiet_ms);
        if (c->eof && c->pty.reaped)
            break;
    }
}

static char *copy_string(const char *s)
{
    size_t n = strlen(s) + 1U;
    char *copy = malloc(n);

    if (copy != NULL)
        (void)memcpy(copy, s, n);
    return copy;
}

static char *env_pair(const char *key, const char *value)
{
    size_t nk = strlen(key);
    size_t nv = strlen(value);
    char *pair;

    if (nk > SIZE_MAX - nv - 2U)
        return NULL;
    pair = malloc(nk + nv + 2U);
    if (pair == NULL)
        return NULL;
    (void)memcpy(pair, key, nk);
    pair[nk] = '=';
    (void)memcpy(pair + nk + 1U, value, nv + 1U);
    return pair;
}

static const char *color_tier(const PtyCtx *c)
{
    if (strstr(c->test->name, "colors_256") != NULL)
        return "256";
    if (strstr(c->test->name, "colors_16") != NULL ||
        strcmp(c->test->profile, "dumb") == 0)
        return "16";
    return "truecolor";
}

bool ptc_env_build(char **envp, const char *colors, const char *state_dir)
{
    static const char *const keys[] = {
        "TERM", "SAG_COLORS", "SAG_TTY_PROBE", "SAG_PROBE_TIMEOUT_MS",
        "SAG_ESC_TIMEOUT_MS", "XDG_STATE_HOME", "LANG", "LC_ALL",
        "SAG_LOG_LEVEL"
    };
    const char *values[] = {
        "xterm-256color", colors, "1", "500", "25", state_dir,
        "C.UTF-8", "C.UTF-8", "debug"
    };
    size_t i;

    if (envp == NULL || colors == NULL || state_dir == NULL)
        return false;
    for (i = 0U; i < SAG_ARRAY_LEN(keys); i++) {
        envp[i] = env_pair(keys[i], values[i]);
        if (envp[i] == NULL) {
            while (i != 0U)
                free(envp[--i]);
            return false;
        }
    }
    envp[SAG_ARRAY_LEN(keys)] = NULL;
    return true;
}

void ptc_env_free(char **envp)
{
    size_t i;

    if (envp == NULL)
        return;
    for (i = 0U; i < SAG_PTY_ENV_COUNT; i++) {
        free(envp[i]);
        envp[i] = NULL;
    }
    envp[SAG_PTY_ENV_COUNT] = NULL;
}

static void strv_free(char **v)
{
    size_t i;

    if (v == NULL)
        return;
    for (i = 0U; v[i] != NULL; i++)
        free(v[i]);
    free(v);
}

void ptc_spawn(PtyCtx *c, const char *bin, ...)
{
    char **argv;
    char *envp[10];
    PtySpec spec;
    va_list ap;
    va_list count_ap;
    const char *arg;
    size_t argc = 1U;
    size_t i;

    if (c == NULL || bin == NULL || c->spawned || c->failed)
        return;
    va_start(ap, bin);
    va_copy(count_ap, ap);
    while (va_arg(count_ap, const char *) != NULL)
        argc++;
    va_end(count_ap);
    argv = calloc(argc + 1U, sizeof(*argv));
    if (argv == NULL) {
        va_end(ap);
        ptc_fail(c, "allocating argv");
        return;
    }
    argv[0] = copy_string(bin);
    for (i = 1U; i < argc; i++) {
        arg = va_arg(ap, const char *);
        argv[i] = copy_string(arg);
    }
    (void)va_arg(ap, const char *);
    va_end(ap);
    for (i = 0U; i < argc; i++) {
        if (argv[i] == NULL) {
            strv_free(argv);
            ptc_fail(c, "allocating argv strings");
            return;
        }
    }
    if (!ptc_env_build(envp, color_tier(c), c->state_dir)) {
        strv_free(argv);
        ptc_fail(c, "allocating pinned environment");
        return;
    }
    spec.path = bin;
    spec.argv = argv;
    spec.envp = envp;
    spec.rows = c->test->rows;
    spec.cols = c->test->cols;
    spec.budget_ms = c->budget_ms;
    if (!sag_pty_spawn(&c->pty, &spec))
        ptc_fail(c, "spawn %s: %s", bin, strerror(errno));
    else
        c->spawned = true;
    ptc_env_free(envp);
    strv_free(argv);
}

void ptc_settle(PtyCtx *c, i64 quiet_ms)
{
    pump_quiet(c, quiet_ms, !c->ready);
}

void ptc_bytes(PtyCtx *c, const char *lit)
{
    if (c == NULL || lit == NULL || !c->spawned || c->failed)
        return;
    (void)write_master(c, (const u8 *)lit, strlen(lit));
}

typedef struct KeyName {
    const char *name;
    u32 kitty;
    const char *legacy;
    unsigned tilde;
} KeyName;

static const KeyName key_names[] = {
    {"esc", 27U, "\x1b", 0U}, {"enter", 13U, "\r", 0U},
    {"tab", 9U, "\t", 0U}, {"backspace", 127U, "\x7f", 0U},
    {"insert", 57348U, NULL, 2U}, {"delete", 57349U, NULL, 3U},
    {"left", 57350U, "\x1b[D", 0U},
    {"right", 57351U, "\x1b[C", 0U},
    {"up", 57352U, "\x1b[A", 0U}, {"down", 57353U, "\x1b[B", 0U},
    {"pageup", 57354U, NULL, 5U}, {"pagedown", 57355U, NULL, 6U},
    {"home", 57356U, "\x1b[H", 0U}, {"end", 57357U, "\x1b[F", 0U}
};

static bool name_equal(const char *a, size_t na, const char *b)
{
    size_t i;

    if (strlen(b) != na)
        return false;
    for (i = 0U; i < na; i++) {
        if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i]))
            return false;
    }
    return true;
}

static bool token_modifier(const char *s, size_t n, u16 *mods)
{
    if (name_equal(s, n, "shift"))
        *mods = (u16)(*mods | SAG_MOD_SHIFT);
    else if (name_equal(s, n, "alt"))
        *mods = (u16)(*mods | SAG_MOD_ALT);
    else if (name_equal(s, n, "ctrl"))
        *mods = (u16)(*mods | SAG_MOD_CTRL);
    else if (name_equal(s, n, "super"))
        *mods = (u16)(*mods | SAG_MOD_SUPER);
    else if (name_equal(s, n, "hyper"))
        *mods = (u16)(*mods | SAG_MOD_HYPER);
    else if (name_equal(s, n, "meta"))
        *mods = (u16)(*mods | SAG_MOD_META);
    else
        return false;
    return true;
}

static bool key_parse(const char *token, size_t len, u16 *mods,
                      u32 *kitty, const char **legacy, unsigned *tilde,
                      u8 *scalar)
{
    const char *part = token;
    const char *end = token + len;
    const char *plus;
    size_t i;

    *mods = 0U;
    *kitty = 0U;
    *legacy = NULL;
    *tilde = 0U;
    *scalar = 0U;
    while ((plus = memchr(part, '+', (size_t)(end - part))) != NULL) {
        if (!token_modifier(part, (size_t)(plus - part), mods))
            return false;
        part = plus + 1;
    }
    if (end - part == 1 && (unsigned char)*part >= 0x20U &&
        (unsigned char)*part <= 0x7eU) {
        *scalar = (u8)*part;
        *kitty = *scalar;
        return true;
    }
    for (i = 0U; i < SAG_ARRAY_LEN(key_names); i++) {
        if (name_equal(part, (size_t)(end - part), key_names[i].name)) {
            *kitty = key_names[i].kitty;
            *legacy = key_names[i].legacy;
            *tilde = key_names[i].tilde;
            return true;
        }
    }
    if (end - part >= 2 && (part[0] == 'F' || part[0] == 'f')) {
        unsigned f = 0U;
        const char *p;

        for (p = part + 1; p < end; p++) {
            if (*p < '0' || *p > '9')
                return false;
            f = f * 10U + (unsigned)(*p - '0');
        }
        if (f == 0U || f > 35U)
            return false;
        if (f <= 12U) {
            static const unsigned kitty_f[] = {
                57364U, 57365U, 57366U, 57367U, 57368U, 57369U,
                57370U, 57371U, 57372U, 57373U, 57374U, 57375U
            };
            static const unsigned tilde_f[] = {
                11U, 12U, 13U, 14U, 15U, 17U,
                18U, 19U, 20U, 21U, 23U, 24U
            };

            *kitty = kitty_f[f - 1U];
            *tilde = tilde_f[f - 1U];
        } else {
            *kitty = 57376U + f - 13U;
        }
        return true;
    }
    return false;
}

static void emit_key(PtyCtx *c, const char *token, size_t len)
{
    char sequence[64];
    const char *legacy;
    u32 kitty;
    unsigned tilde;
    u16 mods;
    u8 scalar;
    bool modern;
    int n;

    if (!key_parse(token, len, &mods, &kitty, &legacy, &tilde, &scalar)) {
        ptc_fail(c, "unknown key spelling: %.*s", (int)len, token);
        return;
    }
    modern = strcmp(c->test->profile, "modern") == 0 ||
             strcmp(c->test->profile, "nosync") == 0;
    if (modern) {
        n = snprintf(sequence, sizeof(sequence), "\x1b[%u;%uu",
                     (unsigned)kitty, (unsigned)mods + 1U);
        if (n < 0 || (size_t)n >= sizeof(sequence)) {
            ptc_fail(c, "key encoding overflow");
            return;
        }
        (void)write_master(c, (const u8 *)sequence, (size_t)n);
        return;
    }
    if (scalar != 0U) {
        u8 bytes[2];
        size_t nb = 0U;

        if ((mods & SAG_MOD_ALT) != 0U)
            bytes[nb++] = 0x1bU;
        if ((mods & SAG_MOD_CTRL) != 0U &&
            ((scalar >= 'a' && scalar <= 'z') ||
             (scalar >= 'A' && scalar <= 'Z')))
            bytes[nb++] = (u8)((tolower(scalar) - 'a') + 1);
        else
            bytes[nb++] = scalar;
        (void)write_master(c, bytes, nb);
    } else if (tilde != 0U) {
        n = mods == 0U
                ? snprintf(sequence, sizeof(sequence), "\x1b[%u~", tilde)
                : snprintf(sequence, sizeof(sequence), "\x1b[%u;%u~", tilde,
                           (unsigned)mods + 1U);
        if (n > 0 && (size_t)n < sizeof(sequence))
            (void)write_master(c, (const u8 *)sequence, (size_t)n);
        else
            ptc_fail(c, "key encoding overflow");
    } else if (legacy != NULL && mods == 0U) {
        (void)write_master(c, (const u8 *)legacy, strlen(legacy));
    } else if (legacy != NULL && strlen(legacy) == 3U && legacy[0] == '\x1b' &&
               legacy[1] == '[') {
        n = snprintf(sequence, sizeof(sequence), "\x1b[1;%u%c",
                     (unsigned)mods + 1U, legacy[2]);
        if (n > 0 && (size_t)n < sizeof(sequence))
            (void)write_master(c, (const u8 *)sequence, (size_t)n);
        else
            ptc_fail(c, "key encoding overflow");
    } else {
        ptc_fail(c, "legacy terminal cannot encode modified key: %.*s",
                 (int)len, token);
    }
}

void ptc_keys(PtyCtx *c, const char *spec)
{
    const char *p;

    if (c == NULL || spec == NULL || !c->spawned || c->failed)
        return;
    p = spec;
    while (*p != '\0') {
        const char *start;

        while (*p == ' ' || *p == '\t' || *p == ',')
            p++;
        if (*p == '\0')
            break;
        start = p;
        while (*p != '\0' && *p != ' ' && *p != '\t' && *p != ',')
            p++;
        emit_key(c, start, (size_t)(p - start));
    }
}

void ptc_resize(PtyCtx *c, u16 rows, u16 cols)
{
    struct winsize ws;

    if (c == NULL || !c->spawned || c->failed || rows == 0U || cols == 0U)
        return;
    (void)memset(&ws, 0, sizeof(ws));
    ws.ws_row = rows;
    ws.ws_col = cols;
    if (ioctl(c->pty.master, TIOCSWINSZ, &ws) != 0 ||
        kill(c->pty.pid, SIGWINCH) != 0) {
        ptc_fail(c, "resize child: %s", strerror(errno));
        return;
    }
    c->pty.rows = rows;
    c->pty.cols = cols;
    vt_resize(&c->vt, rows, cols);
}

void ptc_snapshot(PtyCtx *c, const char *golden_name)
{
    if (c == NULL || golden_name == NULL || c->failed)
        return;
    if (c->snapshot_taken) {
        ptc_fail(c, "case took more than one snapshot per execution");
        return;
    }
    validate_vt(c);
    if (c->failed)
        return;
    c->golden_name = copy_string(golden_name);
    if (c->golden_name == NULL) {
        ptc_fail(c, "allocating golden name");
        return;
    }
    snapshot_write(&c->vt, &c->snapshot);
    c->snapshot_taken = true;
}

static void wait_for_exit(PtyCtx *c)
{
    i64 deadline = case_deadline(c);

    while (!c->pty.reaped && !c->failed && ptc_now_ms() < deadline) {
        pump_quiet(c, 10, false);
        reap_nonblocking(c);
        if (!c->pty.reaped) {
            struct pollfd fd = {c->pty.master, POLLIN | POLLHUP, 0};
            (void)poll(&fd, 1U, 10);
        }
    }
    if (!c->pty.reaped && !c->failed) {
        c->timed_out = true;
        ptc_fail(c, "child did not exit within case budget");
    }
    if (c->pty.reaped && !c->eof) {
        bool activity = false;

        (void)read_available(c, &activity);
    }
}

void ptc_expect_exit(PtyCtx *c, int code)
{
    if (c == NULL || !c->spawned || c->failed)
        return;
    wait_for_exit(c);
    if (c->failed)
        return;
    if (WIFSIGNALED(c->pty.status)) {
        ptc_fail(c, "child died on signal %d", WTERMSIG(c->pty.status));
    } else if (!WIFEXITED(c->pty.status)) {
        ptc_fail(c, "child did not report an exit status");
    } else if (WEXITSTATUS(c->pty.status) != code) {
        ptc_fail(c, "child exit %d, expected %d",
                 WEXITSTATUS(c->pty.status), code);
    }
}

void ptc_expect_signal(PtyCtx *c, int sig)
{
    if (c == NULL || !c->spawned || c->failed)
        return;
    wait_for_exit(c);
    if (c->failed)
        return;
    if (!WIFSIGNALED(c->pty.status)) {
        ptc_fail(c, "child exited %d, expected signal %d",
                 WIFEXITED(c->pty.status) ? WEXITSTATUS(c->pty.status) : -1,
                 sig);
    } else if (WTERMSIG(c->pty.status) != sig) {
        ptc_fail(c, "child died on signal %d, expected %d",
                 WTERMSIG(c->pty.status), sig);
    }
}

static const u8 *find_bytes(const u8 *hay, size_t nhay,
                            const u8 *needle, size_t nneedle)
{
    size_t i;

    if (nneedle == 0U)
        return hay;
    if (nneedle > nhay)
        return NULL;
    for (i = 0U; i <= nhay - nneedle; i++) {
        if (memcmp(hay + i, needle, nneedle) == 0)
            return hay + i;
    }
    return NULL;
}

void ptc_expect_output(PtyCtx *c, const void *bytes, size_t len)
{
    if (c == NULL || bytes == NULL || c->failed)
        return;
    if (find_bytes(c->raw.data, c->raw.len, bytes, len) == NULL)
        ptc_fail(c, "expected output bytes were not observed");
}

void ptc_reject_output(PtyCtx *c, const void *bytes, size_t len)
{
    if (c == NULL || bytes == NULL || c->failed)
        return;
    if (find_bytes(c->raw.data, c->raw.len, bytes, len) != NULL)
        ptc_fail(c, "forbidden output bytes were observed");
}

void ptc_expect_tail(PtyCtx *c, const void *bytes, size_t len)
{
    if (c == NULL || bytes == NULL || c->failed)
        return;
    if (len > c->raw.len || memcmp(c->raw.data + c->raw.len - len,
                                   bytes, len) != 0)
        ptc_fail(c, "child output did not end in expected restore bytes");
}

void ptc_allow_primary(PtyCtx *c)
{
    if (c == NULL || c->spawned)
        return;
    c->allow_primary = true;
    vt_set_primary_policy(&c->vt, true);
}

void ptc_allow_restore(PtyCtx *c)
{
    if (c != NULL)
        vt_set_restore_policy(&c->vt, true);
}

void ptc_check(PtyCtx *c, bool condition, const char *message)
{
    if (!condition)
        ptc_fail(c, "%s", message);
}

void ptc_suspend_resume(PtyCtx *c)
{
    i64 deadline;
    int status = 0;
    pid_t result = 0;
    size_t raw_before;
    size_t restore_len;
    const u8 *restore;
    bool fallback_sent = false;
    bool stopped = false;

    if (c == NULL || !c->spawned || c->failed)
        return;
    ptc_allow_restore(c);
    raw_before = c->raw.len;
    restore = sag_tty_restore_blob(&restore_len);
    if (kill(c->pty.pid, SIGTSTP) != 0) {
        ptc_fail(c, "SIGTSTP child: %s", strerror(errno));
        return;
    }
    deadline = case_deadline(c);
    while (ptc_now_ms() < deadline) {
        bool activity = false;

        (void)read_available(c, &activity);
        if (!fallback_sent && c->raw.len >= raw_before &&
            find_bytes(c->raw.data + raw_before, c->raw.len - raw_before,
                       restore, restore_len) != NULL) {
            /* A setsid() fixture is an orphaned process group, so its
             * job-control stop may be discarded. Normalize both outcomes
             * with SIGSTOP before accepting the observed stopped state. */
            if (kill(c->pty.pid, SIGSTOP) != 0) {
                ptc_fail(c, "SIGSTOP child after restore: %s",
                         strerror(errno));
                return;
            }
            fallback_sent = true;
        }
        do {
            result = waitpid(c->pty.pid, &status, WNOHANG | WUNTRACED);
        } while (result < 0 && errno == EINTR);
        if (result == c->pty.pid && WIFSTOPPED(status))
            stopped = true;
        if (fallback_sent && stopped)
            break;
        {
            struct pollfd fd = {c->pty.master, POLLIN | POLLHUP, 0};
            (void)poll(&fd, 1U, 10);
        }
    }
    if (!fallback_sent || !stopped) {
        ptc_fail(c, "child did not stop after SIGTSTP");
        return;
    }
    if (find_bytes(c->raw.data + raw_before, c->raw.len - raw_before,
                   restore, restore_len) == NULL) {
        ptc_fail(c, "suspend did not emit the terminal restore blob");
        return;
    }
    c->ready = false;
    if (kill(c->pty.pid, SIGCONT) != 0) {
        ptc_fail(c, "SIGCONT child: %s", strerror(errno));
        return;
    }
    pump_quiet(c, PTC_DEFAULT_QUIET_MS, true);
}

const char *ptc_demo_bin(const PtyCtx *c)
{
    return c == NULL ? NULL : c->demo_bin;
}

const char *ptc_sagitta_bin(const PtyCtx *c)
{
    return c == NULL ? NULL : c->sagitta_bin;
}

void ptc_init(PtyCtx *c, const PtyCase *test, const char *state_dir,
              const char *demo_bin, const char *sagitta_bin,
              i64 budget_ms, i64 global_deadline_ms)
{
    VtProfile profile;

    (void)memset(c, 0, sizeof(*c));
    c->test = test;
    c->pty.master = -1;
    c->pty.pid = -1;
    c->pty.status = -1;
    c->state_dir = copy_string(state_dir);
    c->demo_bin = demo_bin;
    c->sagitta_bin = sagitta_bin;
    c->budget_ms = budget_ms > 0 ? budget_ms : PTC_DEFAULT_BUDGET_MS;
    c->global_deadline_ms = global_deadline_ms;
    bytebuf_init(&c->raw);
    bytebuf_init(&c->snapshot);
    vt_init(&c->vt, test->rows, test->cols);
    if (!vt_profile_from_name(test->profile, &profile))
        ptc_fail(c, "unknown terminal profile: %s", test->profile);
    else
        vt_set_profile(&c->vt, profile);
    if (c->state_dir == NULL)
        ptc_fail(c, "allocating state path");
}

void ptc_cleanup(PtyCtx *c)
{
    i64 deadline;
    bool activity = false;

    if (c == NULL || !c->spawned)
        return;
    reap_nonblocking(c);
    if (!c->pty.reaped) {
        (void)kill(c->pty.pid, SIGKILL);
        deadline = add_ms(ptc_now_ms(), PTC_KILL_BUDGET_MS);
        while (!c->pty.reaped && ptc_now_ms() < deadline) {
            struct pollfd fd = {c->pty.master, POLLIN | POLLHUP, 0};
            bool activity = false;

            (void)poll(&fd, 1U, 10);
            (void)read_available(c, &activity);
            reap_nonblocking(c);
        }
        if (!c->pty.reaped)
            ptc_fail(c, "SIGKILLed child could not be reaped within 1 second");
    }
    if (c->pty.master >= 0 && !c->eof)
        (void)read_available(c, &activity);
    validate_vt(c);
    if (c->pty.master >= 0) {
        (void)close(c->pty.master);
        c->pty.master = -1;
    }
    if (c->pty.reaped)
        live_remove(&c->pty);
}

void ptc_dispose(PtyCtx *c)
{
    if (c == NULL)
        return;
    ptc_cleanup(c);
    vt_free(&c->vt);
    bytebuf_free(&c->raw);
    bytebuf_free(&c->snapshot);
    free(c->state_dir);
    free(c->golden_name);
    c->state_dir = NULL;
    c->golden_name = NULL;
}

bool ptc_sweep_all(void)
{
    i64 deadline = add_ms(ptc_now_ms(), PTC_KILL_BUDGET_MS);

    while (nlive != 0U) {
        Pty *p = live_children[0];
        int status;
        pid_t result;

        if (!p->reaped)
            (void)kill(p->pid, SIGKILL);
        do {
            result = waitpid(p->pid, &status, WNOHANG);
        } while (result < 0 && errno == EINTR);
        if (result == p->pid) {
            p->reaped = true;
            p->status = status;
        }
        if (p->master >= 0) {
            (void)close(p->master);
            p->master = -1;
        }
        if (p->reaped) {
            live_remove(p);
            continue;
        }
        if (ptc_now_ms() >= deadline)
            return false;
        {
            struct pollfd fd = {-1, 0, 0};
            (void)poll(&fd, 0U, 10);
        }
    }
    return nlive == 0U;
}

bool ptc_fd_hygiene(Bytebuf *msg)
{
#if defined(__linux__)
    DIR *dir = opendir("/proc/self/fd");
    struct dirent *entry;

    if (dir == NULL)
        return true;
    while ((entry = readdir(dir)) != NULL) {
        char linkpath[64];
        char target[256];
        char *end;
        long fdno;
        ssize_t n;

        errno = 0;
        fdno = strtol(entry->d_name, &end, 10);
        if (errno != 0 || *entry->d_name == '\0' || *end != '\0' || fdno <= 2)
            continue;
        (void)snprintf(linkpath, sizeof(linkpath), "/proc/self/fd/%ld", fdno);
        n = readlink(linkpath, target, sizeof(target) - 1U);
        if (n < 0)
            continue;
        target[n] = '\0';
        if (strncmp(target, "/dev/pts/", 9U) == 0 &&
            strcmp(target, "/dev/pts/ptmx") != 0) {
            bytebuf_printf(msg, "leaked pty fd %ld -> %s", fdno, target);
            (void)closedir(dir);
            return false;
        }
    }
    (void)closedir(dir);
#else
    (void)msg;
#endif
    return true;
}
