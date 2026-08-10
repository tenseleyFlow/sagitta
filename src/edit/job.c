/* Sprint 19: child processes, nonblocking and budgeted.
 *
 * Every descriptor here is close-on-exec at creation via
 * yew_pipe_cloexec(), so an fd cannot leak into a concurrently spawned
 * job (or, once LSP and AI land, into a long-lived server). */
#define _POSIX_C_SOURCE 200809L

#include "edit/job.h"

#include <errno.h>
#include <fcntl.h>
#include <pwd.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "edit/ed.h"
#include "edit/loop.h"
#include "text/piece.h"
#include "unicode/coords.h"
#include "unicode/grapheme.h"
#include "unicode/utf8.h"
#include "util/log.h"

extern char **environ;

/* ------------------------------------------------------------------ */
/* Byte-boundary safety (§3)                                          */
/* ------------------------------------------------------------------ */

/* Bytes needed by the sequence a lead byte opens; 0 when not a lead. */
static u64 utf8_seq_len(u8 b)
{
    if ((b & 0x80U) == 0x00U)
        return 1U;
    if ((b & 0xE0U) == 0xC0U)
        return 2U;
    if ((b & 0xF0U) == 0xE0U)
        return 3U;
    if ((b & 0xF8U) == 0xF0U)
        return 4U;
    return 0U;
}

static bool utf8_is_cont(u8 b)
{
    return (b & 0xC0U) == 0x80U;
}

/* Offset of the last codepoint start at or before `n`, or n when none is
 * within reach (a continuation run longer than a legal sequence is
 * garbage, and garbage is flushed rather than held). */
static u64 last_lead(const u8 *b, u64 n)
{
    u64 back = 0U;

    while (back < YEW_UTF8_MAX && back < n) {
        u64 at = n - 1U - back;

        if (!utf8_is_cont(b[at]))
            return at;
        back++;
    }
    return n;
}

u64 yew_job_safe_prefix(const u8 *b, u64 n, bool at_eof)
{
    u64 cut;
    u64 lead;
    u64 need;

    if (b == NULL || n == 0U)
        return 0U;
    /* Rule 4: at EOF everything goes, including an invalid tail.  Invalid
     * bytes are stored verbatim — the escape policy governs rendering. */
    if (at_eof)
        return n;

    /* Rule 1: hold back a trailing incomplete UTF-8 sequence. */
    cut = n;
    lead = last_lead(b, n);
    if (lead < n) {
        need = utf8_seq_len(b[lead]);
        if (need > 1U && lead + need > n)
            cut = lead;
    }

    /* Rule 2: a read may also split a grapheme cluster — the next read
     * could bring a combining mark, a ZWJ continuation, or the second
     * regional indicator.  A trailing newline is a guaranteed cluster
     * terminator, so line-oriented output (the common case) holds nothing. */
    if (cut == n && n != 0U && b[n - 1U] == (u8)'\n')
        return n;
    if (cut != 0U) {
        u64 prev = (u64)yew_gb_prev_bytes(b, (size_t)cut, (size_t)cut);

        if (prev < cut)
            cut = prev;
    }

    /* Rule 3: never hold more than YEW_JOB_HOLD_MAX.  A pathological
     * stream of combining marks with no boundary must not grow `hold`
     * without bound; at the cap we flush verbatim. */
    if (n - cut > (u64)YEW_JOB_HOLD_MAX)
        return n;
    return cut;
}

/* ------------------------------------------------------------------ */
/* yew_shell_quote (§9)                                               */
/* ------------------------------------------------------------------ */

void yew_shell_quote(Bytebuf *out, const u8 *s, size_t n)
{
    size_t i;

    if (out == NULL)
        return;
    /* Inside '...' POSIX sh treats every other byte literally, including
     * newline, $, backtick and backslash — so a single quote is the only
     * byte needing work, and it closes/escapes/reopens. */
    bytebuf_push_u8(out, (u8)'\'');
    for (i = 0U; i < n; i++) {
        if (s[i] == (u8)'\'')
            bytebuf_append(out, "'\\''", 4U);
        else
            bytebuf_push_u8(out, s[i]);
    }
    bytebuf_push_u8(out, (u8)'\'');
}

/* ------------------------------------------------------------------ */
/* Table plumbing                                                     */
/* ------------------------------------------------------------------ */

void yew_jobs_init(JobTable *jt)
{
    if (jt == NULL)
        return;
    (void)memset(jt, 0, sizeof(*jt));
    jt->next_id = 1U;
}

static void job_close(int *fd)
{
    if (*fd >= 0) {
        (void)close(*fd);
        *fd = -1;
    }
}

static void job_dispose(YewJob *j)
{
    job_close(&j->in_fd);
    job_close(&j->out_fd);
    job_close(&j->err_fd);
    job_close(&j->exec_fd);
    bytebuf_free(&j->hold);
    bytebuf_free(&j->collect);
    free(j->label);
    (void)memset(j, 0, sizeof(*j));
    j->in_fd = j->out_fd = j->err_fd = j->exec_fd = -1;
}

void yew_jobs_free(Ed *ed)
{
    u32 i;

    if (ed == NULL)
        return;
    for (i = 0U; i < ed->jobs.len; i++) {
        YewJob *j = &ed->jobs.v[i];

        /* Editor teardown must not leave orphans holding the terminal. */
        if (j->state == YEW_JOB_RUNNING && j->pgid > 0)
            (void)kill(-j->pgid, SIGKILL);
        if (j->pid > 0 && !j->reaped)
            (void)waitpid(j->pid, NULL, 0);
        job_dispose(j);
    }
    ed->jobs.len = 0U;
}

YewJob *yew_job_find(Ed *ed, u32 id)
{
    u32 i;

    if (ed == NULL || id == 0U)
        return NULL;
    for (i = 0U; i < ed->jobs.len; i++) {
        if (ed->jobs.v[i].id == id)
            return &ed->jobs.v[i];
    }
    return NULL;
}

void yew_job_release(Ed *ed, YewJob *j)
{
    u32 idx;

    if (ed == NULL || j == NULL)
        return;
    idx = (u32)(j - ed->jobs.v);
    if (idx >= ed->jobs.len)
        return;
    /* Never drop a live child: that would orphan it with no way back. */
    if (j->state == YEW_JOB_RUNNING)
        return;
    if (j->pid > 0 && !j->reaped)
        (void)waitpid(j->pid, NULL, WNOHANG);
    job_dispose(j);
    (void)memmove(&ed->jobs.v[idx], &ed->jobs.v[idx + 1U],
                  (size_t)(ed->jobs.len - idx - 1U) * sizeof(*ed->jobs.v));
    ed->jobs.len--;
    ed->jobs.dirty = true;
}

u32 yew_job_running_count(const Ed *ed)
{
    u32 i;
    u32 n = 0U;

    if (ed == NULL)
        return 0U;
    for (i = 0U; i < ed->jobs.len; i++) {
        if (ed->jobs.v[i].state == YEW_JOB_RUNNING)
            n++;
    }
    return n;
}

const char *yew_job_state_name(YewJobState state)
{
    switch (state) {
    case YEW_JOB_RUNNING:
        return "running";
    case YEW_JOB_EXITED:
        return "exited";
    case YEW_JOB_SIGNALED:
        return "signaled";
    case YEW_JOB_EXECFAIL:
        return "exec-fail";
    case YEW_JOB_TIMEOUT:
        return "timeout";
    case YEW_JOB_CANCELLED:
        return "cancelled";
    default:
        break;
    }
    return "?";
}

/* ------------------------------------------------------------------ */
/* Spawn (§2)                                                         */
/* ------------------------------------------------------------------ */

const char *yew_job_shell(void)
{
    const char *sh = getenv("SHELL");
    struct passwd *pw;

    if (sh != NULL && sh[0] != '\0')
        return sh;
    pw = getpwuid(getuid());
    if (pw != NULL && pw->pw_shell != NULL && pw->pw_shell[0] != '\0')
        return pw->pw_shell;
    return "/bin/sh";
}

/* Builds the child environment: a copy of environ with the §7 rows applied.
 * The parent's environment is never mutated — putenv() there would leak
 * into every later job and into the editor itself. */
static char **job_build_env(Ed *ed, Arena *a)
{
    static const char *const drop[] = {"COLUMNS=", "LINES=", "YEW_FILE=",
                                       "YEW_LINE=", "YEW_COL=",
                                       "YEW_WORKSPACE=", "YEW_JOB=",
                                       "PAGER=", "GIT_PAGER="};
    size_t n = 0U;
    size_t i;
    size_t out = 0U;
    char **env;
    const Buffer *buf = ed->win != NULL ? ed->win->buf : NULL;
    const char *path = buf != NULL && buf->path != NULL ? buf->path : "";
    LineNo line = LINENO(0U);
    GCol col = {0U};
    char tmp[512];

    while (environ[n] != NULL)
        n++;
    /* environ + 7 added rows + NULL */
    env = arena_alloc(a, (n + 9U) * sizeof(*env), sizeof(void *));
    for (i = 0U; i < n; i++) {
        bool skip = false;
        size_t d;

        for (d = 0U; d < YEW_ARRAY_LEN(drop); d++) {
            size_t dl = strlen(drop[d]);

            if (strncmp(environ[i], drop[d], dl) == 0) {
                skip = true;
                break;
            }
        }
        if (!skip)
            env[out++] = environ[i];
    }
    if (ed->win != NULL && buf != NULL && buf->tb != NULL &&
        ed->win->cs.curs.len != 0U) {
        const Cursor *cur = &ed->win->cs.curs.data[ed->win->cs.primary];

        line = yew_textbuf_line_of(buf->tb, cur->pos);
        col = yew_off_to_gcol(buf->tb,
                              yew_textbuf_line_span(buf->tb, line),
                              cur->pos);
    }
    (void)snprintf(tmp, sizeof(tmp), "YEW_FILE=%s", path);
    env[out++] = arena_strdup(a, tmp);
    (void)snprintf(tmp, sizeof(tmp), "YEW_LINE=%llu",
                   (unsigned long long)(line.v + 1U));
    env[out++] = arena_strdup(a, tmp);
    /* Grapheme columns, not bytes and not cells: the number the statusline
     * shows, so scripts and the UI agree. */
    (void)snprintf(tmp, sizeof(tmp), "YEW_COL=%llu",
                   (unsigned long long)((u64)col.v + 1U));
    env[out++] = arena_strdup(a, tmp);
    (void)snprintf(tmp, sizeof(tmp), "YEW_WORKSPACE=%s", yew_ws_root(ed));
    env[out++] = arena_strdup(a, tmp);
    env[out++] = arena_strdup(a, "YEW_JOB=1");
    /* A job that spawns `less` would wait forever on a stdin it does not
     * own: no output, no exit, no clue. */
    env[out++] = arena_strdup(a, "PAGER=cat");
    env[out++] = arena_strdup(a, "GIT_PAGER=cat");
    env[out] = NULL;
    return env;
}

static void set_nonblock(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);

    if (flags >= 0)
        (void)fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static bool make_pipe(int p[2], char *err, size_t errsz)
{
    if (!yew_pipe_cloexec(p)) {
        (void)snprintf(err, errsz, "cannot create pipe: %s",
                       strerror(errno));
        p[0] = p[1] = -1;
        return false;
    }
    return true;
}

/* Everything between fork() and execve() must be async-signal-safe: malloc
 * in a forked child can deadlock on an allocator lock the parent held at
 * fork time, and yew_log formats through stdio and opens files.  The
 * exec-status pipe is the only diagnostic channel in this window. */
static void job_child(char **argv, char **envp, const char *cwd,
                      int in_p[2], int out_p[2], int err_p[2],
                      int exec_p[2])
{
    /* Every signal the editor installs a handler for or ignores. */
    static const int reset_signals[] = {SIGPIPE, SIGINT,  SIGTERM, SIGHUP,
                                        SIGQUIT, SIGWINCH, SIGCHLD,
                                        SIGCONT, SIGTSTP, SIGTTIN, SIGTTOU};
    int e;
    int sig;
    sigset_t empty;

    (void)setpgid(0, 0);
    if (dup2(in_p[0], STDIN_FILENO) < 0 ||
        dup2(out_p[1], STDOUT_FILENO) < 0 ||
        dup2(err_p[1], STDERR_FILENO) < 0)
        goto fail;
    /* dup2 clears CLOEXEC on exactly 0/1/2; every other end must go. */
    (void)close(in_p[0]);
    (void)close(in_p[1]);
    (void)close(out_p[0]);
    (void)close(out_p[1]);
    (void)close(err_p[0]);
    (void)close(exec_p[0]);
    /* Reset exactly the dispositions the editor changes.  NSIG is not in
     * the POSIX namespace, and blindly walking 1..NSIG would also touch
     * realtime signals we never install.
     *
     * SIGPIPE matters most: the parent ignores it so a write to a dead
     * child returns EPIPE instead of killing the editor, but a child that
     * inherits SIG_IGN never dies from a closed pipe, so `head`-style
     * pipelines inside the command run forever. */
    for (sig = 0; sig < (int)YEW_ARRAY_LEN(reset_signals); sig++)
        (void)signal(reset_signals[sig], SIG_DFL);
    (void)sigemptyset(&empty);
    (void)sigprocmask(SIG_SETMASK, &empty, NULL);
    if (cwd != NULL && chdir(cwd) != 0)
        goto fail;
    (void)execve(argv[0], argv, envp);
fail:
    e = errno;
    /*
     * Best-effort: we are in the forked child with nothing left to do
     * but report why exec failed, and the parent learns it from the
     * exit status either way.
     *
     * Assigned rather than cast to void because glibc marks write
     * warn_unused_result and several gcc versions do NOT accept a
     * `(void)` cast as acknowledgement — the local compiler did, CI's
     * did not, and -Werror turned that into a build failure on every
     * gcc lane.
     */
    {
        ssize_t reported = write(exec_p[1], &e, sizeof e);

        (void)reported;
    }
    _exit(127);
}

u32 yew_job_spawn(Ed *ed, const YewJobSpec *spec, char *err, size_t errsz)
{
    Arena scratch;
    char **argv;
    char **envp;
    char *shell_argv[4];
    int in_p[2] = {-1, -1};
    int out_p[2] = {-1, -1};
    int err_p[2] = {-1, -1};
    int exec_p[2] = {-1, -1};
    pid_t pid;
    YewJob *j;
    const char *cwd;
    const char *display;
    bool want_stdin;

    if (err != NULL && errsz != 0U)
        err[0] = '\0';
    if (ed == NULL || spec == NULL)
        return 0U;
    if (ed->jobs.len >= YEW_JOB_MAX) {
        (void)snprintf(err, errsz,
                       "too many jobs (%d); finish or kill one first",
                       YEW_JOB_MAX);
        return 0U;
    }
    if (spec->argv == NULL && spec->cmdline == NULL) {
        (void)snprintf(err, errsz, "no command");
        return 0U;
    }

    /* Everything that allocates happens BEFORE fork. */
    arena_init(&scratch);
    if (spec->argv != NULL) {
        argv = spec->argv;
    } else {
        shell_argv[0] = (char *)yew_job_shell();
        shell_argv[1] = (char *)"-c";
        shell_argv[2] = (char *)spec->cmdline;
        shell_argv[3] = NULL;
        argv = shell_argv;
    }
    envp = job_build_env(ed, &scratch);
    cwd = spec->cwd != NULL ? spec->cwd : yew_ws_root(ed);
    want_stdin = spec->in_buf != NULL &&
                 spec->in_span.hi > spec->in_span.lo;

    if (!make_pipe(in_p, err, errsz) || !make_pipe(out_p, err, errsz) ||
        !make_pipe(err_p, err, errsz) || !make_pipe(exec_p, err, errsz))
        goto fail;

    pid = fork();
    if (pid < 0) {
        (void)snprintf(err, errsz, "cannot fork: %s", strerror(errno));
        goto fail;
    }
    if (pid == 0)
        job_child(argv, envp, cwd, in_p, out_p, err_p, exec_p);

    /* Both sides call setpgid so whichever runs first wins the race. */
    (void)setpgid(pid, pid);
    (void)close(in_p[0]);
    in_p[0] = -1;
    (void)close(out_p[1]);
    out_p[1] = -1;
    (void)close(err_p[1]);
    err_p[1] = -1;
    (void)close(exec_p[1]);
    exec_p[1] = -1;
    set_nonblock(in_p[1]);
    set_nonblock(out_p[0]);
    set_nonblock(err_p[0]);
    set_nonblock(exec_p[0]);

    j = &ed->jobs.v[ed->jobs.len++];
    (void)memset(j, 0, sizeof(*j));
    j->id = ed->jobs.next_id++;
    j->pid = pid;
    j->pgid = pid;
    j->in_fd = in_p[1];
    j->out_fd = out_p[0];
    j->err_fd = err_p[0];
    j->exec_fd = exec_p[0];
    j->state = YEW_JOB_RUNNING;
    j->sink = spec->sink;
    j->in_buf = spec->in_buf;
    j->in_span = spec->in_span;
    j->timeout_ms = spec->timeout_ms;
    j->start_ms = yew_now_ms();
    j->follow_tail = true;
    bytebuf_init(&j->hold);
    bytebuf_init(&j->collect);
    display = spec->display != NULL ? spec->display :
              (spec->cmdline != NULL ? spec->cmdline : argv[0]);
    {
        size_t dlen = strlen(display);

        j->label = yew_xmalloc(dlen + 1U);
        (void)memcpy(j->label, display, dlen + 1U);
    }
    /* No stdin to write means closing immediately: a child whose read()
     * never sees EOF is this subsystem's signature hang, and it presents
     * as "that command is slow", not as a deadlock. */
    if (!want_stdin)
        job_close(&j->in_fd);
    ed->jobs.dirty = true;
    arena_free_all(&scratch);
    return j->id;

fail:
    job_close(&in_p[0]);
    job_close(&in_p[1]);
    job_close(&out_p[0]);
    job_close(&out_p[1]);
    job_close(&err_p[0]);
    job_close(&err_p[1]);
    job_close(&exec_p[0]);
    job_close(&exec_p[1]);
    arena_free_all(&scratch);
    return 0U;
}

bool yew_job_signal(Ed *ed, u32 id, int sig)
{
    YewJob *j = yew_job_find(ed, id);

    if (j == NULL || j->pgid <= 0 || j->state != YEW_JOB_RUNNING)
        return false;
    if (kill(-j->pgid, sig) != 0)
        return false;
    if (sig == SIGTERM)
        j->kill_at_ms = yew_now_ms() + YEW_JOB_TERM_GRACE_MS;
    return true;
}

/* ------------------------------------------------------------------ */
/* Reading (§3)                                                       */
/* ------------------------------------------------------------------ */

u32 yew_job_pollfd_count(const Ed *ed)
{
    /* stdout, stderr, exec-status, stdin: four per job at most. */
    return ed == NULL ? 0U : ed->jobs.len * 4U;
}

static bool job_wants_stdin(const YewJob *j)
{
    return j->in_fd >= 0 && j->in_buf != NULL &&
           j->in_off < j->in_span.hi - j->in_span.lo;
}

void yew_job_collect_fds(Ed *ed, struct pollfd *pfd, u32 *n)
{
    u32 i;

    if (ed == NULL || pfd == NULL || n == NULL)
        return;
    for (i = 0U; i < ed->jobs.len; i++) {
        YewJob *j = &ed->jobs.v[i];

        if (j->out_fd >= 0) {
            pfd[*n].fd = j->out_fd;
            pfd[*n].events = POLLIN;
            pfd[(*n)++].revents = 0;
        }
        if (j->err_fd >= 0) {
            pfd[*n].fd = j->err_fd;
            pfd[*n].events = POLLIN;
            pfd[(*n)++].revents = 0;
        }
        if (j->exec_fd >= 0) {
            pfd[*n].fd = j->exec_fd;
            pfd[*n].events = POLLIN;
            pfd[(*n)++].revents = 0;
        }
        if (job_wants_stdin(j)) {
            pfd[*n].fd = j->in_fd;
            pfd[*n].events = POLLOUT;
            pfd[(*n)++].revents = 0;
        }
    }
}

static short revents_for(const struct pollfd *pfd, u32 n, int fd)
{
    u32 i;

    if (fd < 0)
        return 0;
    for (i = 0U; i < n; i++) {
        if (pfd[i].fd == fd)
            return pfd[i].revents;
    }
    return 0;
}

/* Appends `bytes` to wherever this job's sink points. */
static void job_deliver(Ed *ed, YewJob *j, const u8 *bytes, u64 len,
                        bool is_err)
{
    if (len == 0U)
        return;
    switch (j->sink) {
    case YEW_SINK_COLLECT:
        if (j->collect.len + len > YEW_JOB_COLLECT_MAX) {
            if (!j->collect_capped) {
                j->collect_capped = true;
                (void)yew_job_signal(ed, j->id, SIGTERM);
            }
            return;
        }
        bytebuf_append(&j->collect, bytes, (size_t)len);
        break;
    case YEW_SINK_BUFFER:
        yew_job_buffer_append(ed, j, bytes, len, is_err);
        break;
    case YEW_SINK_DISCARD:
    default:
        break;
    }
}

/* Drains one fd until EAGAIN or the read budget, whichever comes first.
 * Returns true when the fd reached EOF. */
static bool job_drain(Ed *ed, YewJob *j, int *fd, bool is_err)
{
    u64 drained = 0U;

    while (drained < YEW_JOB_READ_BUDGET) {
        u8 chunk[16384];
        ssize_t got = read(*fd, chunk, sizeof(chunk));
        u64 safe;
        u64 total;
        const u8 *view;

        if (got < 0) {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                return false;
            job_close(fd);
            return true;
        }
        drained += (u64)got;
        if (is_err)
            j->bytes_err += (u64)got;
        else
            j->bytes_out += (u64)got;
        /* Held bytes from the previous read lead the window. */
        if (j->hold.len != 0U) {
            bytebuf_append(&j->hold, chunk, (size_t)got);
            view = j->hold.data;
            total = j->hold.len;
        } else {
            view = chunk;
            total = (u64)got;
        }
        safe = yew_job_safe_prefix(view, total, got == 0);
        job_deliver(ed, j, view, safe, is_err);
        if (safe < total) {
            u64 rest = total - safe;
            u8 tail[YEW_JOB_HOLD_MAX + 16];

            (void)memcpy(tail, view + safe, (size_t)rest);
            j->hold.len = 0U;
            bytebuf_append(&j->hold, tail, (size_t)rest);
        } else {
            j->hold.len = 0U;
        }
        if (got == 0) {
            job_close(fd);
            return true;
        }
    }
    /* Budget hit: stay in the poll set and continue next iteration. */
    return false;
}

static void job_read_exec_status(Ed *ed, YewJob *j)
{
    int e = 0;
    ssize_t got = read(j->exec_fd, &e, sizeof e);

    (void)ed;
    if (got == (ssize_t)sizeof e) {
        /* Without this channel a missing shell and a command exiting 127
         * are indistinguishable. */
        j->exec_errno = e;
        j->state = YEW_JOB_EXECFAIL;
        ed->jobs.dirty = true;
        job_close(&j->exec_fd);
        return;
    }
    if (got == 0 || (got < 0 && errno != EAGAIN && errno != EWOULDBLOCK &&
                     errno != EINTR))
        job_close(&j->exec_fd); /* closing empty IS the success signal */
}

static void job_write_stdin(YewJob *j)
{
    TextIter it;
    u64 base = j->in_span.lo;
    u64 total = j->in_span.hi - base;

    while (j->in_off < total) {
        const u8 *chunk = NULL;
        size_t chunk_len = 0U;
        ssize_t wrote;

        if (!yew_textiter_begin(&it, j->in_buf, BYTEOFF(base + j->in_off)))
            break;
        if (!yew_textiter_chunk(&it, j->in_buf, &chunk, &chunk_len) ||
            chunk_len == 0U)
            break;
        if ((u64)chunk_len > total - j->in_off)
            chunk_len = (size_t)(total - j->in_off);
        wrote = write(j->in_fd, chunk, chunk_len);
        if (wrote < 0) {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                return;
            /* EPIPE: `head -1` exited early.  Not an error — stop writing,
             * close stdin, keep draining output. */
            job_close(&j->in_fd);
            return;
        }
        j->in_off += (u64)wrote;
    }
    /* Close only once the whole region is written.  `sort`, `sha256sum`
     * and every accumulating filter emit nothing until stdin EOF, so
     * closing early truncates their input; closing never is §2's hang. */
    if (j->in_off >= total)
        job_close(&j->in_fd);
}

void yew_job_pump(Ed *ed, const struct pollfd *pfd, u32 n)
{
    u32 i;

    if (ed == NULL || pfd == NULL)
        return;
    for (i = 0U; i < ed->jobs.len; i++) {
        YewJob *j = &ed->jobs.v[i];
        short re;

        re = revents_for(pfd, n, j->exec_fd);
        if (re != 0 && j->exec_fd >= 0)
            job_read_exec_status(ed, j);
        re = revents_for(pfd, n, j->out_fd);
        if (re != 0 && j->out_fd >= 0)
            (void)job_drain(ed, j, &j->out_fd, false);
        re = revents_for(pfd, n, j->err_fd);
        if (re != 0 && j->err_fd >= 0)
            (void)job_drain(ed, j, &j->err_fd, true);
        re = revents_for(pfd, n, j->in_fd);
        if ((re & (POLLOUT | POLLERR | POLLHUP)) != 0 && j->in_fd >= 0)
            job_write_stdin(j);
    }
}

/* ------------------------------------------------------------------ */
/* Reaping and deadlines                                              */
/* ------------------------------------------------------------------ */

void yew_job_reap(Ed *ed)
{
    for (;;) {
        int status = 0;
        pid_t pid = waitpid(-1, &status, WNOHANG);
        u32 i;

        if (pid <= 0)
            return;
        for (i = 0U; i < ed->jobs.len; i++) {
            YewJob *j = &ed->jobs.v[i];

            if (j->pid != pid)
                continue;
            j->reaped = true;
            j->end_ms = yew_now_ms();
            /* A timeout/cancel verdict already recorded why it died; the
             * wait status would only say SIGTERM and lose that. */
            if (j->state == YEW_JOB_RUNNING) {
                if (WIFEXITED(status)) {
                    j->state = YEW_JOB_EXITED;
                    j->exit_code = WEXITSTATUS(status);
                } else if (WIFSIGNALED(status)) {
                    j->state = YEW_JOB_SIGNALED;
                    j->termsig = WTERMSIG(status);
                }
            } else if (j->state == YEW_JOB_EXECFAIL) {
                j->exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : 127;
            }
            ed->jobs.dirty = true;
            /*
             * NOT finished yet.  The child exiting says nothing about
             * whether we have read everything it wrote — a pipe can hold
             * a bufferful past exit, and completing here truncated the
             * output and appended the footer above bytes that had not
             * arrived.  yew_job_settle finishes it once the pipes EOF.
             */
            break;
        }
    }
}

bool yew_job_pending(const YewJob *j)
{
    if (j == NULL)
        return false;
    return !j->reaped || j->out_fd >= 0 || j->err_fd >= 0;
}

void yew_job_settle(Ed *ed)
{
    u32 i;

    if (ed == NULL)
        return;
    for (i = 0U; i < ed->jobs.len; i++) {
        YewJob *j = &ed->jobs.v[i];

        if (j->drained || yew_job_pending(j))
            continue;
        j->drained = true;
        yew_job_finish(ed, j);
    }
}

i64 yew_job_deadline(const Ed *ed, i64 now_ms)
{
    i64 best = -1;
    u32 i;

    if (ed == NULL)
        return -1;
    for (i = 0U; i < ed->jobs.len; i++) {
        const YewJob *j = &ed->jobs.v[i];
        i64 at = -1;

        if (j->state != YEW_JOB_RUNNING)
            continue;
        if (j->kill_at_ms != 0)
            at = j->kill_at_ms;
        else if (j->timeout_ms > 0)
            at = j->start_ms + j->timeout_ms;
        if (at < 0)
            continue;
        if (at <= now_ms)
            return 0;
        if (best < 0 || at - now_ms < best)
            best = at - now_ms;
    }
    return best;
}

void yew_job_tick(Ed *ed, i64 now_ms)
{
    u32 i;

    if (ed == NULL)
        return;
    for (i = 0U; i < ed->jobs.len; i++) {
        YewJob *j = &ed->jobs.v[i];

        /* Escalation is checked BEFORE the RUNNING guard on purpose: a
         * timed-out or cancelled job has already left RUNNING but is
         * still alive until SIGKILL lands.  Guarding first would strand a
         * child that ignores SIGTERM (`trap '' TERM`) forever. */
        if (j->kill_at_ms != 0 && !j->reaped && now_ms >= j->kill_at_ms) {
            (void)kill(-j->pgid, SIGKILL);
            j->kill_at_ms = 0;
            continue;
        }
        if (j->state != YEW_JOB_RUNNING)
            continue;
        if (j->timeout_ms > 0 && now_ms - j->start_ms >= j->timeout_ms) {
            j->state = YEW_JOB_TIMEOUT;
            ed->jobs.dirty = true;
            (void)kill(-j->pgid, SIGTERM);
            j->kill_at_ms = now_ms + YEW_JOB_TERM_GRACE_MS;
        }
    }
}
