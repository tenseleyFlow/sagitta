#define _XOPEN_SOURCE 700
#include <fcntl.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "harness.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
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
    /* Generous on purpose: the grid recorded here is compared byte for
     * byte against a second process, so a frame that is 10 ms late
     * would fail the gate rather than slow it down. */
    PTC_RESUME_QUIET_MS = 250,
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

bool yew_pty_spawn(Pty *p, const PtySpec *sp)
{
    char sname[128];
    struct termios initial_termios;
    int pid_pipe[2] = {-1, -1};
    int m;
    int sfd;
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
    (void)memset(&initial_termios, 0, sizeof(initial_termios));
    sfd = open(sname, O_RDWR | O_NOCTTY);
    if (sfd < 0)
        goto fail;
    if (tcgetattr(sfd, &initial_termios) < 0) {
        int saved_errno = errno;

        (void)close(sfd);
        errno = saved_errno;
        goto fail;
    }
    (void)close(sfd);
    if (nlive >= PTC_LIVE_MAX) {
        errno = EAGAIN;
        goto fail;
    }
    if (fcntl(m, F_SETFL, O_NONBLOCK) < 0)
        goto fail;
    if (sp->host_session && pipe(pid_pipe) != 0)
        goto fail;
    pid = fork();
    if (pid < 0)
        goto fail;
    if (pid == 0) {
        struct winsize ws;
        sigset_t empty;
        int s;

        if (sigemptyset(&empty) != 0 ||
            sigprocmask(SIG_SETMASK, &empty, NULL) != 0)
            _exit(127);
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
        if (sp->host_session) {
            pid_t target;
            int status;
            ssize_t written;

            (void)close(pid_pipe[0]);
            target = fork();
            if (target < 0)
                _exit(127);
            if (target != 0) {
                do {
                    written = write(pid_pipe[1], &target, sizeof(target));
                } while (written < 0 && errno == EINTR);
                (void)close(pid_pipe[1]);
                if (written != (ssize_t)sizeof(target))
                    _exit(127);
                while (waitpid(target, &status, 0) < 0) {
                    if (errno != EINTR)
                        _exit(127);
                }
                /* Let yew's orphaned guardian restore the PTY before this
                 * shell-like session leader mirrors yew's status and exits. */
                {
                    struct timespec grace = {0, 250000000L};

                    while (nanosleep(&grace, &grace) != 0 && errno == EINTR)
                        ;
                }
                if (WIFSIGNALED(status)) {
                    int sig = WTERMSIG(status);

                    (void)signal(sig, SIG_DFL);
                    (void)raise(sig);
                    _exit(128 + sig);
                }
                _exit(WIFEXITED(status) ? WEXITSTATUS(status) : 127);
            }
            (void)close(pid_pipe[1]);
        }
        /* Set BEFORE execve, so the editor's workspace root is the
         * fixture rather than whatever directory the runner was
         * started in. */
        if (sp->cwd != NULL && chdir(sp->cwd) != 0)
            _exit(127);
        execve(sp->path, sp->argv, sp->envp);
        _exit(127);
    }
    if (sp->host_session) {
        ssize_t got;

        (void)close(pid_pipe[1]);
        do {
            got = read(pid_pipe[0], &p->target_pid,
                       sizeof(p->target_pid));
        } while (got < 0 && errno == EINTR);
        (void)close(pid_pipe[0]);
        pid_pipe[0] = -1;
        pid_pipe[1] = -1;
        if (got != (ssize_t)sizeof(p->target_pid)) {
            int saved_errno = got < 0 ? errno : EIO;

            (void)kill(pid, SIGKILL);
            while (waitpid(pid, NULL, 0) < 0 && errno == EINTR)
                ;
            errno = saved_errno;
            goto fail;
        }
    } else {
        p->target_pid = pid;
    }
    p->master = m;
    p->pid = pid;
    p->rows = sp->rows;
    p->cols = sp->cols;
    p->reaped = false;
    p->status = -1;
    p->started_ms = ptc_now_ms();
    p->initial_termios = initial_termios;
    p->initial_termios_valid = true;
    live_add(p);
    return true;

fail:
    if (pid_pipe[0] >= 0)
        (void)close(pid_pipe[0]);
    if (pid_pipe[1] >= 0)
        (void)close(pid_pipe[1]);
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
            /* Linux commonly reports EIO after the slave side has closed.
             * Darwin can return either EIO or zero transiently between fork
             * and the child's open of the slave.  Only a reaped child makes
             * either result EOF; before that, keep polling for startup. */
            reap_nonblocking(c);
            if (c->pty.reaped)
                c->eof = true;
            return !c->failed;
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

/*
 * How much longer a "quiet" window has to be on this run.
 *
 * A settle waits for N ms of NO OUTPUT and then decides the editor is
 * done.  That inference holds while the editor is fast: if it had more
 * to draw, it would have drawn it.  Under valgrind it does not — the
 * editor can sit silent for longer than N ms with a frame still to
 * come, and the snapshot catches the screen mid-update.  The symptom is
 * an "unstable snapshot": the runner's two snapshots disagree because a
 * frame landed between them.
 *
 * So the valgrind lane scales every quiet window by one number rather
 * than tuning cases one at a time.  At the default of 1 nothing
 * changes, so no golden can move.
 */
static i64 quiet_scale(void)
{
    static i64 scale;

    if (scale == 0) {
        const char *v = getenv("YEW_PTY_QUIET_SCALE");
        long parsed = v != NULL ? strtol(v, NULL, 10) : 1;

        scale = parsed >= 1 && parsed <= 100 ? (i64)parsed : 1;
    }
    return scale;
}

static void pump_quiet(PtyCtx *c, i64 quiet_ms, bool need_ready)
{
    i64 quiet_deadline;
    i64 deadline;

    if (c == NULL || !c->spawned || c->failed)
        return;
    if (quiet_ms <= 0)
        quiet_ms = PTC_DEFAULT_QUIET_MS;
    quiet_ms *= quiet_scale();
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

static const char *term_for(const PtyCtx *c)
{
    return strstr(c->test->name, "_term_dumb") != NULL
               ? "dumb" : "xterm-256color";
}

/*
 * Sprint 27 §7: the degradation variants are selected by the case NAME.
 *
 * A suffix rather than a per-case field because the golden is named
 * after the case too — so `chrome_tabs_nocolor` names one env, one
 * scene and one golden, and the three cannot drift apart.  NO_COLOR is
 * presence-based, so baseline cases omit it rather than exporting an empty
 * value.
 */
static const char *no_color_for(const PtyCtx *c)
{
    if (strstr(c->test->name, "_nocolor_empty") != NULL)
        return "";
    return strstr(c->test->name, "_nocolor") != NULL ? "1" : NULL;
}

static const char *ascii_for(const PtyCtx *c)
{
    return strstr(c->test->name, "_ascii") != NULL ? "1" : "0";
}

static const char *shadow_test_for(const PtyCtx *c)
{
    return strncmp(c->test->name, "s43_shadow_", 11U) == 0 ? "1" : "0";
}

bool ptc_env_build(char **envp, const char *term, const char *colors,
                   const char *state_dir, const char *no_color, const char *ascii,
                   const char *runtime_dir, const char *shadow_test,
                   const char *prof, const char *log)
{
    static const char *const keys[] = {
        "TERM", "YEW_COLORS", "YEW_TTY_PROBE", "YEW_PROBE_TIMEOUT_MS",
        "YEW_ESC_TIMEOUT_MS", "XDG_STATE_HOME", "XDG_CONFIG_HOME",
        "LANG", "LC_ALL",
        "YEW_LOG_LEVEL", "YEW_JOB_ELAPSED_MS", "SHELL",
        "YEW_PICKERS_NOW",
        /* Sprint 27 §7's degradation variants. */
        "NO_COLOR", "YEW_ASCII", "YEW_RUNTIME_DIR", "XDG_CACHE_HOME",
        /* The hermetic workspace lives below the source repository's
         * ignored build/ directory.  Stop discovery at its state parent so
         * non-Git PTYs cannot inherit the checkout that launched them;
         * cases that create a repository inside the workspace still find it. */
        "YEW_SHADOW_TEST", "YEW_AI_MOCK", "GIT_CEILING_DIRECTORIES",
        /* Sprint 57's constrained-target lane reads yew's own HWM rather
         * than accidentally measuring this runner process. */
        "YEW_PROF", "YEW_LOG"
    };
    const char *values[] = {
        term, colors, "1", "500", "25", state_dir, state_dir,
        "C.UTF-8", "C.UTF-8", "debug",
        /* Pin job elapsed time: it is the only nondeterministic thing a
         * job prints, and goldens are byte-compared (invariant 5). */
        "1240",
        /* Pin the shell too.  Without this the editor falls back to the
         * developer's pw_shell, so a machine running fish or zsh records
         * different goldens than one running bash — the tests would encode
         * whoever generated them. */
        "/bin/sh",
        /* Sprint 26: pins the undo picker's relative timestamps. */
        "1700000000",
        no_color, ascii, runtime_dir, state_dir, shadow_test, "1", state_dir,
        prof, log
    };
    size_t i;
    size_t out_i = 0U;

    _Static_assert(YEW_ARRAY_LEN(keys) == YEW_PTY_ENV_COUNT,
                   "YEW_PTY_ENV_COUNT must match the key table");
    if (envp == NULL || term == NULL || colors == NULL || state_dir == NULL ||
        ascii == NULL || runtime_dir == NULL || shadow_test == NULL)
        return false;
    for (i = 0U; i <= YEW_PTY_ENV_COUNT; i++)
        envp[i] = NULL;
    for (i = 0U; i < YEW_ARRAY_LEN(keys); i++) {
        if (values[i] == NULL)
            continue;
        envp[out_i] = env_pair(keys[i], values[i]);
        if (envp[out_i] == NULL) {
            while (out_i != 0U) {
                out_i--;
                free(envp[out_i]);
                envp[out_i] = NULL;
            }
            return false;
        }
        out_i++;
    }
    envp[out_i] = NULL;
    return true;
}

void ptc_env_free(char **envp)
{
    size_t i;

    if (envp == NULL)
        return;
    for (i = 0U; i < YEW_PTY_ENV_COUNT; i++) {
        free(envp[i]);
        envp[i] = NULL;
    }
    envp[YEW_PTY_ENV_COUNT] = NULL;
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

static void env_remove(char **envp, const char *name)
{
    size_t name_len;
    size_t i;

    if (envp == NULL || name == NULL)
        return;
    name_len = strlen(name);
    for (i = 0U; envp[i] != NULL; i++) {
        if (strncmp(envp[i], name, name_len) != 0 ||
            envp[i][name_len] != '=')
            continue;
        free(envp[i]);
        do {
            envp[i] = envp[i + 1U];
            i++;
        } while (envp[i - 1U] != NULL);
        return;
    }
}

void ptc_spawn(PtyCtx *c, const char *bin, ...)
{
    char **argv;
    char *envp[YEW_PTY_ENV_COUNT + 1U];
    char *runtime_dir;
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
    /* A child may chdir into an isolated workspace.  Pin the checked-in
     * runtime by absolute path so config-bearing cases remain hermetic. */
    runtime_dir = realpath("runtime", NULL);
    if (!ptc_env_build(envp, term_for(c),
                       color_tier(c), c->state_dir,
                       no_color_for(c), ascii_for(c),
                       runtime_dir == NULL ? "" : runtime_dir,
                       shadow_test_for(c), getenv("YEW_PROF"),
                       getenv("YEW_LOG"))) {
        free(runtime_dir);
        strv_free(argv);
        ptc_fail(c, "allocating pinned environment");
        return;
    }
    /* Sprint 50's remote status badge and cloud opt-in need a non-loopback
     * URL to exercise their privacy surfaces.  The local-error cases select
     * that backend only to establish error state.  Ordinary PTY children
     * retain the transport guard; these named cases use only a local mock
     * listener or make no request. */
    if (strncmp(c->test->name, "ai_badge_remote_", 16U) == 0 ||
        strncmp(c->test->name, "ai_badge_local_error_", 21U) == 0 ||
        strncmp(c->test->name, "ai_optin_cloud_", 15U) == 0)
        env_remove(envp, "YEW_AI_MOCK");
    free(runtime_dir);
    (void)memset(&spec, 0, sizeof(spec));
    spec.path = bin;
    spec.argv = argv;
    spec.envp = envp;
    spec.cwd = c->cwd;
    spec.host_session = c->host_session;
    /*
     * The runner is invoked with a RELATIVE binary path
     * (`--yew build/yew`), and the child chdirs before
     * execve — so the path has to be resolved here, while we are still
     * in the directory it is relative to.  Without this the child
     * exec'd nothing and exited 127, which surfaced as "child exited
     * before kitty keyboard push" and said nothing about why.
     */
    if (c->cwd != NULL) {
        char *resolved = realpath(bin, NULL);

        if (resolved == NULL) {
            strv_free(argv);
            ptc_env_free(envp);
            ptc_fail(c, "cannot resolve %s: %s", bin, strerror(errno));
            return;
        }
        free(c->resolved_bin);
        c->resolved_bin = resolved;
        spec.path = resolved;
        /* argv[0] too, so the child's own idea of its path is real. */
        free(argv[0]);
        argv[0] = copy_string(resolved);
        if (argv[0] == NULL) {
            strv_free(argv);
            ptc_env_free(envp);
            ptc_fail(c, "allocating argv[0]");
            return;
        }
    }
    spec.rows = c->test->rows;
    spec.cols = c->test->cols;
    spec.budget_ms = c->budget_ms;
    if (!yew_pty_spawn(&c->pty, &spec))
        ptc_fail(c, "spawn %s: %s", bin, strerror(errno));
    else
        c->spawned = true;
    c->host_session = false;
    ptc_env_free(envp);
    strv_free(argv);
}

void ptc_no_altscreen(PtyCtx *c)
{
    if (c != NULL)
        c->ready = true;
}

void ptc_host_session(PtyCtx *c)
{
    if (c != NULL && !c->spawned)
        c->host_session = true;
}

static const u8 *find_bytes(const u8 *hay, size_t nhay,
                            const u8 *needle, size_t nneedle);
static void wait_for_exit(PtyCtx *c);

void ptc_wait_output_since(PtyCtx *c, size_t at,
                           const void *bytes, size_t len)
{
    i64 deadline;

    if (c == NULL || bytes == NULL || !c->spawned || c->failed)
        return;
    if (at > c->raw.len) {
        ptc_fail(c, "output checkpoint is past the raw log");
        return;
    }
    deadline = case_deadline(c);
    while (find_bytes(at == 0U ? c->raw.data : c->raw.data + at,
                      c->raw.len - at,
                      bytes, len) == NULL) {
        if (c->failed)
            return;
        if (ptc_now_ms() >= deadline) {
            c->timed_out = true;
            ptc_fail(c, "timed out waiting for expected output");
            return;
        }
        if (c->eof) {
            wait_for_exit(c);
            if (c->failed)
                return;
            if (c->pty.reaped && WIFSIGNALED(c->pty.status))
                ptc_fail(c, "child died on signal %d before the expected output",
                         WTERMSIG(c->pty.status));
            else if (c->pty.reaped && WIFEXITED(c->pty.status))
                ptc_fail(c, "child exited %d before the expected output",
                         WEXITSTATUS(c->pty.status));
            else
                ptc_fail(c, "child ended before the expected output");
            return;
        }
        pump_quiet(c, 20, false);
    }
}

void ptc_wait_output(PtyCtx *c, const void *bytes, size_t len)
{
    ptc_wait_output_since(c, 0U, bytes, len);
}

void ptc_settle(PtyCtx *c, i64 quiet_ms)
{
    pump_quiet(c, quiet_ms, !c->ready);
}

void ptc_wait_kitty_push(PtyCtx *c, u32 flags)
{
    i64 deadline;

    if (c == NULL || !c->spawned || c->failed)
        return;
    deadline = case_deadline(c);
    while ((c->vt.ksp == 0 || c->vt.kitty[c->vt.ksp - 1] != flags) &&
           !c->failed) {
        struct pollfd fd = {c->pty.master, POLLIN | POLLHUP, 0};
        i64 left = deadline - ptc_now_ms();
        int timeout = left <= 0 ? 0 : left > 250 ? 250 : (int)left;
        int result;
        bool activity = false;

        reap_nonblocking(c);
        if (left <= 0) {
            c->timed_out = true;
            ptc_fail(c, "timed out waiting for kitty keyboard push");
            break;
        }
        result = poll(&fd, 1U, timeout);
        if (result < 0 && errno == EINTR)
            continue;
        if (result < 0) {
            ptc_fail(c, "poll for kitty keyboard push: %s",
                     strerror(errno));
            break;
        }
        if (result > 0 &&
            (fd.revents & (POLLIN | POLLHUP | POLLERR)) != 0)
            (void)read_available(c, &activity);
        if (c->eof && c->pty.reaped &&
            (c->vt.ksp == 0 || c->vt.kitty[c->vt.ksp - 1] != flags))
            ptc_fail(c, "child exited before kitty keyboard push");
    }
}

void ptc_wait_sync_pairs(PtyCtx *c, u32 count)
{
    i64 deadline;

    if (c == NULL || !c->spawned || c->failed)
        return;
    deadline = case_deadline(c);
    while (c->vt.nsync_pairs < count && !c->failed) {
        struct pollfd fd = {c->pty.master, POLLIN | POLLHUP, 0};
        i64 left = deadline - ptc_now_ms();
        int timeout = left <= 0 ? 0 : left > 250 ? 250 : (int)left;
        int result;
        bool activity = false;

        reap_nonblocking(c);
        if (left <= 0) {
            c->timed_out = true;
            ptc_fail(c, "timed out waiting for synchronized frame");
            break;
        }
        result = poll(&fd, 1U, timeout);
        if (result < 0 && errno == EINTR)
            continue;
        if (result < 0) {
            ptc_fail(c, "poll for synchronized frame: %s",
                     strerror(errno));
            break;
        }
        if (result > 0 &&
            (fd.revents & (POLLIN | POLLHUP | POLLERR)) != 0)
            (void)read_available(c, &activity);
        if (c->eof && c->pty.reaped && c->vt.nsync_pairs < count)
            ptc_fail(c, "child exited before synchronized frame");
    }
}

void ptc_wait_until(PtyCtx *c, PtcWaitPredicate done, const void *arg,
                    const char *failure)
{
    i64 deadline;

    if (c == NULL || done == NULL || failure == NULL || !c->spawned ||
        c->failed)
        return;
    deadline = case_deadline(c);
    while (!done(c, arg) && !c->failed) {
        struct pollfd fd = {c->pty.master, POLLIN | POLLHUP, 0};
        i64 left = deadline - ptc_now_ms();
        int timeout = left <= 0 ? 0 : left > 250 ? 250 : (int)left;
        int result;
        bool activity = false;

        reap_nonblocking(c);
        if (left <= 0) {
            c->timed_out = true;
            ptc_fail(c, "%s", failure);
            break;
        }
        result = poll(&fd, 1U, timeout);
        if (result < 0 && errno == EINTR)
            continue;
        if (result < 0) {
            ptc_fail(c, "poll for semantic PTY state: %s",
                     strerror(errno));
            break;
        }
        if (result > 0 &&
            (fd.revents & (POLLIN | POLLHUP | POLLERR)) != 0)
            (void)read_available(c, &activity);
        if (c->eof && c->pty.reaped && !done(c, arg))
            ptc_fail(c, "%s (child exited)", failure);
    }
}

void ptc_bytes(PtyCtx *c, const char *lit)
{
    size_t len;

    if (c == NULL || lit == NULL || !c->spawned || c->failed)
        return;
    len = strlen(lit);
    /*
     * A PTY preserves byte order, not the read boundary of this write.
     * Once a raw write contains more than one byte the harness cannot
     * prove that it denotes one input event without duplicating the
     * editor's incremental UTF-8 / escape-sequence decoder.  The child may
     * therefore drain it in one loop iteration or several and paint an
     * identical final grid with a different cumulative frame count.
     *
     * Keep counting pairs: explicit ptc_wait_sync_pairs / ptc_check gates
     * still observe nsync_pairs.  Only the snapshot header stops treating
     * the scheduler-dependent total as terminal state.
     */
    if (len > 1U)
        c->vt.sync_pairs_unstable = true;
    (void)write_master(c, (const u8 *)lit, len);
}

typedef struct KeyName {
    const char *name;
    u32 kitty;
    const char *legacy;
    unsigned tilde;
} KeyName;

static const KeyName key_names[] = {
    {"esc", 27U, "\x1b", 0U}, {"enter", 13U, "\r", 0U},
    {"tab", 9U, "\t", 0U}, {"space", 32U, " ", 0U},
    {"backspace", 127U, "\x7f", 0U},
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
        *mods = (u16)(*mods | YEW_MOD_SHIFT);
    else if (name_equal(s, n, "alt"))
        *mods = (u16)(*mods | YEW_MOD_ALT);
    else if (name_equal(s, n, "ctrl"))
        *mods = (u16)(*mods | YEW_MOD_CTRL);
    else if (name_equal(s, n, "super"))
        *mods = (u16)(*mods | YEW_MOD_SUPER);
    else if (name_equal(s, n, "hyper"))
        *mods = (u16)(*mods | YEW_MOD_HYPER);
    else if (name_equal(s, n, "meta"))
        *mods = (u16)(*mods | YEW_MOD_META);
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
    for (i = 0U; i < YEW_ARRAY_LEN(key_names); i++) {
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

static void emit_key(PtyCtx *c, Bytebuf *burst,
                     const char *token, size_t len)
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
    /* Kitty flag 21 does not request "report all keys".  Printable keys,
     * the four disambiguated controls, and Kitty-only functional codes use
     * CSI u; arrows/navigation keep their legacy CSI finals, with modifiers
     * in the legacy parameter.  This mirrors the terminal contract and the
     * real sequences a flag-21 terminal sends. */
    if (modern &&
        (scalar != 0U || kitty == 32U || kitty == 27U || kitty == 13U ||
         kitty == 9U ||
         kitty == 127U || (legacy == NULL && tilde == 0U))) {
        n = snprintf(sequence, sizeof(sequence), "\x1b[%u;%uu",
                     (unsigned)kitty, (unsigned)mods + 1U);
        if (n < 0 || (size_t)n >= sizeof(sequence)) {
            ptc_fail(c, "key encoding overflow");
            return;
        }
        bytebuf_append(burst, sequence, (size_t)n);
        return;
    }
    if (scalar != 0U) {
        u8 bytes[2];
        size_t nb = 0U;

        if ((mods & YEW_MOD_ALT) != 0U)
            bytes[nb++] = 0x1bU;
        if ((mods & YEW_MOD_CTRL) != 0U &&
            ((scalar >= 'a' && scalar <= 'z') ||
             (scalar >= 'A' && scalar <= 'Z')))
            bytes[nb++] = (u8)((tolower(scalar) - 'a') + 1);
        else
            bytes[nb++] = scalar;
        bytebuf_append(burst, bytes, nb);
    } else if (tilde != 0U) {
        n = mods == 0U
                ? snprintf(sequence, sizeof(sequence), "\x1b[%u~", tilde)
                : snprintf(sequence, sizeof(sequence), "\x1b[%u;%u~", tilde,
                           (unsigned)mods + 1U);
        if (n > 0 && (size_t)n < sizeof(sequence))
            bytebuf_append(burst, sequence, (size_t)n);
        else
            ptc_fail(c, "key encoding overflow");
    } else if (legacy != NULL && mods == 0U) {
        bytebuf_append(burst, legacy, strlen(legacy));
    } else if (legacy != NULL && strlen(legacy) == 3U && legacy[0] == '\x1b' &&
               legacy[1] == '[') {
        n = snprintf(sequence, sizeof(sequence), "\x1b[1;%u%c",
                     (unsigned)mods + 1U, legacy[2]);
        if (n > 0 && (size_t)n < sizeof(sequence))
            bytebuf_append(burst, sequence, (size_t)n);
        else
            ptc_fail(c, "key encoding overflow");
    } else {
        ptc_fail(c, "legacy terminal cannot encode modified key: %.*s",
                 (int)len, token);
    }
}

void ptc_keys(PtyCtx *c, const char *spec)
{
    Bytebuf burst;
    const char *p;
    size_t nevents = 0U;

    if (c == NULL || spec == NULL || !c->spawned || c->failed)
        return;
    bytebuf_init(&burst);
    p = spec;
    while (*p != '\0' && !c->failed) {
        const char *start;

        while (*p == ' ' || *p == '\t' || *p == ',')
            p++;
        if (*p == '\0')
            break;
        start = p;
        while (*p != '\0' && *p != ' ' && *p != '\t' && *p != ',')
            p++;
        emit_key(c, &burst, start, (size_t)(p - start));
        nevents++;
    }
    if (!c->failed && burst.len != 0U) {
        /* One token is one decoded key even when its wire encoding is a
         * multi-byte CSI sequence.  Two tokens are a burst whose split
         * across child reads is scheduler-dependent, just like ptc_bytes. */
        if (nevents > 1U)
            c->vt.sync_pairs_unstable = true;
        (void)write_master(c, burst.data, burst.len);
    }
    bytebuf_free(&burst);
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

void ptc_snapshot_sgr(PtyCtx *c, const char *golden_name)
{
    size_t i;

    ptc_snapshot(c, golden_name);
    if (c == NULL || c->failed)
        return;
    bytebuf_append(&c->snapshot, "--- sgr\n", 8U);
    for (i = 0U; i + 2U < c->raw.len; i++) {
        size_t end;

        if (c->raw.data[i] != 0x1bU || c->raw.data[i + 1U] != '[')
            continue;
        end = i + 2U;
        while (end < c->raw.len &&
               !(c->raw.data[end] >= 0x40U && c->raw.data[end] <= 0x7eU))
            end++;
        if (end == c->raw.len)
            break;
        if (c->raw.data[end] == 'm') {
            bytebuf_append(&c->snapshot, "\\x1b[", 5U);
            bytebuf_append(&c->snapshot, c->raw.data + i + 2U,
                           end - (i + 2U) + 1U);
            bytebuf_append(&c->snapshot, "\n", 1U);
        }
        i = end;
    }
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

void ptc_check_termios_unchanged(PtyCtx *c)
{
    struct termios current;

    if (c == NULL || c->failed)
        return;
    if (!c->spawned || !c->pty.initial_termios_valid) {
        ptc_fail(c, "terminal state was not captured at spawn");
        return;
    }
    (void)memset(&current, 0, sizeof(current));
    if (tcgetattr(c->pty.master, &current) < 0) {
        ptc_fail(c, "reading terminal state after child exit: %s",
                 strerror(errno));
        return;
    }
    if (memcmp(&c->pty.initial_termios, &current, sizeof(current)) != 0)
        ptc_fail(c, "child changed terminal state bytes");
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
    if (find_bytes(c->raw.data, c->raw.len, bytes, len) == NULL) {
        /* Name the run.  "bytes were not observed" over seven
         * assertions in one case says nothing about which. */
        char shown[64];
        size_t i;
        size_t n = len < sizeof(shown) / 4U ? len : sizeof(shown) / 4U - 1U;
        size_t at = 0U;

        for (i = 0U; i < n; i++) {
            u8 b = ((const u8 *)bytes)[i];

            if (b >= 0x20U && b < 0x7FU)
                shown[at++] = (char)b;
            else
                at += (size_t)snprintf(shown + at, sizeof(shown) - at,
                                       "\\x%02x", (unsigned)b);
        }
        shown[at] = '\0';
        ptc_fail(c, "expected output |%s| was not observed", shown);
    }
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

static bool enter_suspend(PtyCtx *c, bool through_command)
{
    i64 deadline;
    int status = 0;
    pid_t result = 0;
    size_t raw_before;
    size_t restore_len;
    const u8 *restore;
    bool restored = false;
    bool stopped = false;

    if (c == NULL || !c->spawned || c->failed)
        return false;
    ptc_allow_restore(c);
    raw_before = c->raw.len;
    restore = yew_tty_restore_blob(&restore_len);
    if (through_command) {
        ptc_keys(c, "ctrl+z");
    } else if (kill(c->pty.pid, SIGTSTP) != 0) {
        ptc_fail(c, "SIGTSTP child: %s", strerror(errno));
        return false;
    }
    deadline = case_deadline(c);
    while (!restored && !c->failed && ptc_now_ms() < deadline) {
        bool activity = false;

        (void)read_available(c, &activity);
        restored = c->raw.len >= raw_before &&
                   find_bytes(c->raw.data + raw_before,
                              c->raw.len - raw_before,
                              restore, restore_len) != NULL;
        if (restored)
            break;
        {
            struct pollfd fd = {c->pty.master, POLLIN | POLLHUP, 0};
            (void)poll(&fd, 1U, 10);
        }
    }
    if (!restored) {
        ptc_fail(c, "suspend did not emit the terminal restore blob");
        return false;
    }
    c->ready = false;
    if (through_command) {
        pump_quiet(c, PTC_DEFAULT_QUIET_MS, false);
        return !c->failed;
    }
    /* setsid() makes this fixture's process group orphaned, so the
     * job-control SIGTSTP may be discarded. SIGSTOP gives the harness a
     * portable stopped state after the real handler has restored the tty. */
    if (kill(c->pty.pid, SIGSTOP) != 0) {
        ptc_fail(c, "SIGSTOP child after restore: %s", strerror(errno));
        return false;
    }
    while (!stopped && ptc_now_ms() < deadline) {
        do {
            result = waitpid(c->pty.pid, &status, WNOHANG | WUNTRACED);
        } while (result < 0 && errno == EINTR);
        if (result == c->pty.pid) {
            if (WIFSTOPPED(status)) {
                stopped = true;
            } else if (WIFEXITED(status) || WIFSIGNALED(status)) {
                c->pty.reaped = true;
                c->pty.status = status;
                live_remove(&c->pty);
                ptc_fail(c, "child exited while entering suspend state");
                return false;
            }
        } else if (result < 0) {
            ptc_fail(c, "waitpid for suspend: %s", strerror(errno));
            return false;
        }
        if (stopped)
            break;
        {
            struct pollfd fd = {-1, 0, 0};
            (void)poll(&fd, 0U, 10);
        }
    }
    if (!stopped) {
        ptc_fail(c, "child did not stop after SIGTSTP");
        return false;
    }
    return true;
}

static void resume_suspended(PtyCtx *c)
{
    i64 deadline;
    i64 retry_at;
    int status = 0;
    pid_t result = 0;

    if (c == NULL || c->failed)
        return;
    deadline = case_deadline(c);
    if (kill(c->pty.pid, SIGCONT) != 0) {
        ptc_fail(c, "SIGCONT child: %s", strerror(errno));
        return;
    }
    retry_at = add_ms(ptc_now_ms(), PTC_DEFAULT_QUIET_MS);
    while (!c->ready && !c->failed && ptc_now_ms() < deadline) {
        struct pollfd fd = {c->pty.master, POLLIN | POLLHUP, 0};
        bool activity = false;
        i64 now;

        (void)read_available(c, &activity);
        if (c->ready)
            break;
        do {
            result = waitpid(c->pty.pid, &status, WNOHANG | WUNTRACED);
        } while (result < 0 && errno == EINTR);
        if (result == c->pty.pid) {
            if (WIFSTOPPED(status)) {
                if (kill(c->pty.pid, SIGCONT) != 0) {
                    ptc_fail(c, "repeat SIGCONT child: %s",
                             strerror(errno));
                    break;
                }
                retry_at = add_ms(ptc_now_ms(), PTC_DEFAULT_QUIET_MS);
            } else if (WIFEXITED(status) || WIFSIGNALED(status)) {
                c->pty.reaped = true;
                c->pty.status = status;
                live_remove(&c->pty);
                if (WIFEXITED(status))
                    ptc_fail(c,
                             "child exited %d before repaint after SIGCONT",
                             WEXITSTATUS(status));
                else
                    ptc_fail(c,
                             "child died on signal %d before repaint after SIGCONT",
                             WTERMSIG(status));
                break;
            }
        } else if (result < 0) {
            ptc_fail(c, "waitpid for resume: %s", strerror(errno));
            break;
        }
        now = ptc_now_ms();
        if (now >= retry_at) {
            if (kill(c->pty.pid, SIGCONT) != 0) {
                ptc_fail(c, "retry SIGCONT child: %s", strerror(errno));
                break;
            }
            retry_at = add_ms(now, PTC_DEFAULT_QUIET_MS);
        }
        (void)poll(&fd, 1U, 10);
    }
    if (!c->ready && !c->failed)
        ptc_fail(c, "child did not repaint after SIGCONT");
    if (!c->failed)
        pump_quiet(c, PTC_DEFAULT_QUIET_MS, false);
}

void ptc_suspend_resume(PtyCtx *c)
{
    if (enter_suspend(c, false))
        resume_suspended(c);
}

void ptc_command_suspend_resume(PtyCtx *c)
{
    if (enter_suspend(c, true))
        resume_suspended(c);
}

const char *ptc_demo_bin(const PtyCtx *c)
{
    return c == NULL ? NULL : c->demo_bin;
}

const char *ptc_yew_bin(const PtyCtx *c)
{
    return c == NULL ? NULL : c->yew_bin;
}

void ptc_init(PtyCtx *c, const PtyCase *test, const char *state_dir,
              const char *demo_bin, const char *yew_bin,
              i64 budget_ms, i64 global_deadline_ms)
{
    VtProfile profile;

    (void)memset(c, 0, sizeof(*c));
    c->test = test;
    c->pty.master = -1;
    c->pty.pid = -1;
    c->pty.target_pid = -1;
    c->pty.status = -1;
    /*
     * ABSOLUTE, always.
     *
     * The runner builds this as `build/pty-<case>-N.XXXX`, relative to
     * where it was started.  A child that chdirs (Sprint 26's finder
     * cases, so the walk has a fixed root) would then resolve
     * XDG_STATE_HOME against ITS cwd — and the editor wrote its whole
     * state directory inside the fixture the finder was about to walk,
     * which grew the file list by five entries per run and made the
     * golden unstable in a way that pointed nowhere near the cause.
     */
    if (state_dir != NULL && state_dir[0] != '/') {
        char abs[PATH_MAX];
        char cwd[PATH_MAX];

        if (getcwd(cwd, sizeof(cwd)) != NULL &&
            snprintf(abs, sizeof(abs), "%s/%s", cwd, state_dir) <
                (int)sizeof(abs))
            c->state_dir = copy_string(abs);
        else
            c->state_dir = copy_string(state_dir);
    } else {
        c->state_dir = copy_string(state_dir);
    }
    if (c->state_dir != NULL) {
        struct stat state_st;

        if (stat(c->state_dir, &state_st) != 0) {
            /* Unit drills use synthetic processes and historically pass
             * an absent state path.  They need no workspace isolation. */
            if (errno != ENOENT)
                ptc_fail(c, "inspecting state directory: %s",
                         strerror(errno));
        } else if (!S_ISDIR(state_st.st_mode)) {
            ptc_fail(c, "state path is not a directory");
        } else {
            size_t len = strlen(c->state_dir);
            static const char *const links[] = {"build", "tests"};
            size_t i;

            c->workspace_dir = malloc(len + sizeof("/workspace"));
            if (c->workspace_dir != NULL) {
                (void)memcpy(c->workspace_dir, c->state_dir, len);
                (void)memcpy(c->workspace_dir + len, "/workspace",
                             sizeof("/workspace"));
                if (mkdir(c->workspace_dir, 0700) != 0)
                    ptc_fail(c, "creating isolated workspace: %s",
                             strerror(errno));
            }
            for (i = 0U; c->workspace_dir != NULL &&
                         i < YEW_ARRAY_LEN(links); i++) {
                size_t name_len = strlen(links[i]);
                size_t path_len = strlen(c->workspace_dir) + name_len + 2U;
                char *path = malloc(path_len);
                char *target = realpath(links[i], NULL);

                if (path == NULL || target == NULL) {
                    ptc_fail(c, "preparing isolated workspace");
                } else {
                    (void)snprintf(path, path_len, "%s/%s",
                                   c->workspace_dir, links[i]);
                    if (symlink(target, path) != 0)
                        ptc_fail(c, "linking isolated workspace: %s",
                                 strerror(errno));
                }
                free(target);
                free(path);
            }
            c->cwd = c->workspace_dir;
        }
    }
    c->demo_bin = demo_bin;
    c->yew_bin = yew_bin;
    c->budget_ms = budget_ms > 0 ? budget_ms : PTC_DEFAULT_BUDGET_MS;
    c->global_deadline_ms = global_deadline_ms;
    bytebuf_init(&c->raw);
    bytebuf_init(&c->snapshot);
    bytebuf_init(&c->pre_resume);
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
        if (c->pty.target_pid > 0 && c->pty.target_pid != c->pty.pid)
            (void)kill(c->pty.target_pid, SIGKILL);
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

/*
 * Sprint 25 DoD 2: quit, reopen, and compare.
 *
 * Everything the first process left behind is discarded on purpose.  A
 * resumed editor repaints from an empty terminal, so comparing against
 * a model that still held the first one's output would pass whenever
 * the second process painted nothing at all — which is exactly the
 * regression this gate exists to catch.
 */
void ptc_set_cwd(PtyCtx *c, const char *dir)
{
    if (c != NULL)
        c->cwd = dir;
}

void ptc_mark_resume(PtyCtx *c)
{
    if (c == NULL || c->failed)
        return;
    /*
     * Captured while the first editor is still RUNNING.
     *
     * Taking it after the quit records a terminal that has been torn
     * down — alt screen off, modes reset, cursor homed — and every one
     * of those differs from a live second process, so the comparison
     * fails on the teardown rather than on the state.  The text and
     * style grids were identical the whole time; only the header was
     * wrong, which is the most misleading way for a gate to fail.
     */
    /*
     * Settle FIRST.  A grid captured mid-frame has the cursor hidden at
     * 0,0 — the editor hides it while painting and places it after —
     * and the resumed process, settled properly, does not.  The text
     * matched all along; the comparison failed on a cursor that was
     * simply not drawn yet.
     */
    pump_quiet(c, PTC_RESUME_QUIET_MS, false);
    if (c->failed)
        return;
    snapshot_write(&c->vt, &c->pre_resume);
    if (c->pre_resume.len == 0U) {
        ptc_fail(c, "nothing on screen to resume from");
        return;
    }
    /*
     * The frame COUNT is not editor state (s19).  It records how the
     * kernel scheduled a scripted session's writes, and the second
     * process paints once where the first painted twenty-five times —
     * both from the same document.  Invariant 5 is about the grid.
     */
    c->vt.sync_pairs_unstable = true;
    c->pre_resume.len = 0U;
    snapshot_write(&c->vt, &c->pre_resume);
    c->marked_resume = true;
}

void ptc_resume(PtyCtx *c, const char *bin, ...)
{
    va_list ap;
    const char *args[8];
    size_t n = 0U;
    VtProfile profile;

    if (c == NULL || bin == NULL || c->failed)
        return;
    if (c->resumed) {
        ptc_fail(c, "case resumed more than once");
        return;
    }
    if (!c->marked_resume) {
        ptc_fail(c, "ptc_resume without ptc_mark_resume");
        return;
    }
    /* The child must be GONE, not merely asked to leave: the clean-quit
     * save happens on its way out, and spawning the reader before the
     * writer has finished writing is the flakiest race available. */
    ptc_cleanup(c);
    if (c->failed)
        return;
    vt_free(&c->vt);
    vt_init(&c->vt, c->test->rows, c->test->cols);
    if (!vt_profile_from_name(c->test->profile, &profile)) {
        ptc_fail(c, "unknown terminal profile: %s", c->test->profile);
        return;
    }
    vt_set_profile(&c->vt, profile);
    /* The fresh VtScreen must carry the same frame-count policy, or the
     * two snapshots disagree about a field neither of them is about. */
    c->vt.sync_pairs_unstable = true;
    if (c->allow_primary)
        vt_set_primary_policy(&c->vt, true);
    c->raw.len = 0U;
    c->eof = false;
    c->ready = false;
    c->spawned = false;
    c->resumed = true;
    c->pty.master = -1;
    c->pty.pid = -1;
    c->pty.target_pid = -1;
    c->pty.status = -1;
    c->pty.reaped = false;

    va_start(ap, bin);
    while (n < YEW_ARRAY_LEN(args) - 1U) {
        const char *arg = va_arg(ap, const char *);

        if (arg == NULL)
            break;
        args[n++] = arg;
    }
    va_end(ap);
    args[n] = NULL;
    ptc_spawn(c, bin, args[0], args[1], args[2], args[3], args[4], args[5],
              args[6], NULL);
}

void ptc_check_resume_exact(PtyCtx *c)
{
    Bytebuf now;

    if (c == NULL || c->failed)
        return;
    if (!c->resumed) {
        ptc_fail(c, "resume exactness checked without a resume");
        return;
    }
    /* And the resumed process gets the same settle, for the same
     * reason. */
    pump_quiet(c, PTC_RESUME_QUIET_MS, false);
    if (c->failed)
        return;
    bytebuf_init(&now);
    snapshot_write(&c->vt, &now);
    if (now.len != c->pre_resume.len ||
        memcmp(now.data, c->pre_resume.data, now.len) != 0) {
        /*
         * The two snapshots are dumped in full rather than diffed to a
         * line: a resume that is one cell wrong is the interesting
         * case, and a summary hides it.
         */
        (void)fprintf(stderr,
                      "--- resume mismatch: before quit ---\n%.*s\n"
                      "--- after reopen ---\n%.*s\n",
                      (int)c->pre_resume.len, (const char *)c->pre_resume.data,
                      (int)now.len, (const char *)now.data);
        ptc_fail(c, "grid differs after quit and reopen");
    }
    bytebuf_free(&now);
}

void ptc_dispose(PtyCtx *c)
{
    if (c == NULL)
        return;
    ptc_cleanup(c);
    vt_free(&c->vt);
    bytebuf_free(&c->raw);
    bytebuf_free(&c->snapshot);
    bytebuf_free(&c->pre_resume);
    free(c->state_dir);
    free(c->workspace_dir);
    free(c->resolved_bin);
    c->resolved_bin = NULL;
    free(c->golden_name);
    c->state_dir = NULL;
    c->workspace_dir = NULL;
    c->golden_name = NULL;
}

bool ptc_sweep_all(void)
{
    i64 deadline = add_ms(ptc_now_ms(), PTC_KILL_BUDGET_MS);

    while (nlive != 0U) {
        Pty *p = live_children[0];
        int status;
        pid_t result;

        if (!p->reaped) {
            if (p->target_pid > 0 && p->target_pid != p->pid)
                (void)kill(p->target_pid, SIGKILL);
        }
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
