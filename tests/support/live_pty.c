#define _XOPEN_SOURCE 700
#define _POSIX_C_SOURCE 200809L

#include "support/live_pty.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static bool suffix(const YewLivePty *pty, const char *text)
{
    size_t len = strlen(text);

    return len <= pty->ntail &&
           memcmp(pty->tail + pty->ntail - len, text, len) == 0;
}

i64 yew_live_pty_now_ns(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return -1;
    return (i64)ts.tv_sec * INT64_C(1000000000) + ts.tv_nsec;
}

static int timeout_ms(i64 deadline_ns)
{
    i64 now = yew_live_pty_now_ns();
    i64 left;

    if (now < 0 || now >= deadline_ns)
        return 0;
    left = deadline_ns - now;
    if (left > INT64_C(1000000000))
        return 1000;
    return (int)((left + INT64_C(999999)) / INT64_C(1000000));
}

bool yew_live_pty_open(YewLivePty *pty, char *slave, size_t slave_cap,
                       u16 rows, u16 cols)
{
    const char *name;
    size_t len;
    struct winsize ws;
    int fd;

    if (pty == NULL || slave == NULL || slave_cap == 0U ||
        rows == 0U || cols == 0U)
        return false;
    (void)memset(pty, 0, sizeof(*pty));
    pty->master = -1;
    pty->pid = -1;
    fd = posix_openpt(O_RDWR | O_NOCTTY | O_CLOEXEC);
    if (fd < 0 || grantpt(fd) != 0 || unlockpt(fd) != 0)
        goto fail;
    name = ptsname(fd);
    if (name == NULL)
        goto fail;
    len = strlen(name);
    if (len + 1U > slave_cap)
        goto fail;
    (void)memcpy(slave, name, len + 1U);
    (void)memset(&ws, 0, sizeof(ws));
    ws.ws_row = rows;
    ws.ws_col = cols;
    if (ioctl(fd, TIOCSWINSZ, &ws) != 0)
        goto fail;
    if (fcntl(fd, F_SETFL, O_NONBLOCK) != 0)
        goto fail;
    pty->master = fd;
    return true;

fail:
    if (fd >= 0)
        (void)close(fd);
    return false;
}

bool yew_live_pty_attach(const YewLivePty *pty, const char *slave,
                         u16 rows, u16 cols)
{
    struct winsize ws;
    sigset_t empty;
    int fd;

    if (pty == NULL || pty->master < 0 || slave == NULL)
        return false;
    if (sigemptyset(&empty) != 0 ||
        sigprocmask(SIG_SETMASK, &empty, NULL) != 0 || setsid() < 0)
        return false;
    fd = open(slave, O_RDWR);
    if (fd < 0)
        return false;
#ifdef TIOCSCTTY
    if (ioctl(fd, TIOCSCTTY, 0) != 0) {
        (void)close(fd);
        return false;
    }
#endif
    (void)memset(&ws, 0, sizeof(ws));
    ws.ws_row = rows;
    ws.ws_col = cols;
    if (ioctl(fd, TIOCSWINSZ, &ws) != 0 ||
        dup2(fd, STDIN_FILENO) < 0 || dup2(fd, STDOUT_FILENO) < 0 ||
        dup2(fd, STDERR_FILENO) < 0) {
        (void)close(fd);
        return false;
    }
    if (fd > STDERR_FILENO)
        (void)close(fd);
    (void)close(pty->master);
    return true;
}

void yew_live_pty_exec(const char *binary, const char *path)
{
    if (getenv("YEW_TEST_VALGRIND") != NULL) {
        execlp("valgrind", "valgrind", "--quiet", "--error-exitcode=99",
               "--leak-check=full", "--errors-for-leak-kinds=definite",
               "--track-fds=yes", binary, path, (char *)NULL);
    } else {
        execl(binary, binary, path, (char *)NULL);
    }
    _exit(126);
}

bool yew_live_pty_spawn(YewLivePty *pty, const char *binary,
                        const char *path, const char *state_dir,
                        u16 rows, u16 cols)
{
    char slave[128];
    pid_t pid;

    if (!yew_live_pty_open(pty, slave, sizeof(slave), rows, cols))
        return false;
    pid = fork();
    if (pid < 0) {
        yew_live_pty_close(pty);
        return false;
    }
    if (pid == 0) {
        if (setenv("TERM", "xterm-256color", 1) != 0 ||
            setenv("COLORTERM", "truecolor", 1) != 0 ||
            setenv("YEW_COLORS", "truecolor", 1) != 0 ||
            setenv("YEW_TTY_PROBE", "1", 1) != 0 ||
            setenv("YEW_PROBE_TIMEOUT_MS", "500", 1) != 0 ||
            setenv("YEW_ESC_TIMEOUT_MS", "25", 1) != 0 ||
            setenv("LANG", "C.UTF-8", 1) != 0 ||
            setenv("LC_ALL", "C.UTF-8", 1) != 0 ||
            setenv("XDG_STATE_HOME", state_dir, 1) != 0 ||
            setenv("YEW_LOG", "/dev/null", 1) != 0 ||
            !yew_live_pty_attach(pty, slave, rows, cols))
            _exit(126);
        yew_live_pty_exec(binary, path);
    }
    pty->pid = pid;
    return true;
}

bool yew_live_pty_write(YewLivePty *pty, const void *data, size_t len,
                        i64 deadline_ns)
{
    const u8 *bytes = data;

    while (len != 0U) {
        ssize_t n = write(pty->master, bytes, len);

        if (n > 0) {
            bytes += (size_t)n;
            len -= (size_t)n;
        } else if (n < 0 && errno == EINTR) {
            continue;
        } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            struct pollfd fd = {pty->master, POLLOUT, 0};

            if (poll(&fd, 1U, timeout_ms(deadline_ns)) <= 0)
                return false;
        } else {
            return false;
        }
        if (yew_live_pty_now_ns() >= deadline_ns)
            return false;
    }
    return true;
}

static bool reply(YewLivePty *pty, const char *bytes, size_t len,
                  i64 deadline_ns)
{
    return yew_live_pty_write(pty, bytes, len, deadline_ns);
}

static bool consume(YewLivePty *pty, const u8 *bytes, size_t len,
                    i64 deadline_ns)
{
    u8 responses[32];
    size_t nresponses = 0U;
    size_t i;

    for (i = 0U; i < len; i++) {
        if (pty->ntail == sizeof(pty->tail)) {
            (void)memmove(pty->tail, pty->tail + 1U,
                          sizeof(pty->tail) - 1U);
            pty->ntail--;
        }
        pty->tail[pty->ntail++] = bytes[i];
        if (!pty->kitty_replied && suffix(pty, "\033[?u")) {
            static const char response[] = "\033[?0u";

            /* Legacy remains the default.  Focused callers can opt into
             * Kitty CSI-u without changing every live-PTY performance lane. */
            pty->kitty_replied = true;
            if (pty->kitty_supported) {
                (void)memcpy(responses + nresponses, response,
                             sizeof(response) - 1U);
                nresponses += sizeof(response) - 1U;
            }
        }
        if (suffix(pty, "\033[>21u"))
            pty->kitty_enabled = true;
        if (!pty->sync_replied && suffix(pty, "\033[?2026$p")) {
            static const char response[] = "\033[?2026;2$y";

            pty->sync_replied = true;
            (void)memcpy(responses + nresponses, response,
                         sizeof(response) - 1U);
            nresponses += sizeof(response) - 1U;
        }
        if (!pty->da_replied && suffix(pty, "\033[c")) {
            static const char response[] = "\033[?62;22c";

            pty->da_replied = true;
            (void)memcpy(responses + nresponses, response,
                         sizeof(response) - 1U);
            nresponses += sizeof(response) - 1U;
        }
        if (suffix(pty, "\033[?2026h"))
            pty->in_sync_frame = true;
        if (pty->frames == 0U && !pty->in_sync_frame &&
            suffix(pty, "\033[?25h"))
            pty->frames++;
        if (suffix(pty, "\033[?2026l")) {
            pty->frames++;
            pty->in_sync_frame = false;
        }
    }
    return nresponses == 0U ||
           reply(pty, (const char *)responses, nresponses, deadline_ns);
}

bool yew_live_pty_wait_frame(YewLivePty *pty, u64 after, i64 deadline_ns,
                             i64 *completed_ns)
{
    while (pty->frames <= after) {
        struct pollfd fd = {pty->master, POLLIN | POLLHUP, 0};
        u8 bytes[8192];
        int result = poll(&fd, 1U, timeout_ms(deadline_ns));
        ssize_t n;

        if (result < 0 && errno == EINTR)
            continue;
        if (result == 0 && yew_live_pty_now_ns() < deadline_ns)
            continue;
        if (result <= 0) {
            size_t i;

            (void)fprintf(stderr,
                          "live PTY: frame timeout (frames=%llu bytes=%zu "
                          "replies=%d/%d/%d)\n",
                          (unsigned long long)pty->frames, pty->ntail,
                          pty->kitty_replied, pty->sync_replied,
                          pty->da_replied);
            (void)fputs("live PTY: tail", stderr);
            for (i = 0U; i < pty->ntail; i++)
                (void)fprintf(stderr, " %02x", pty->tail[i]);
            (void)fputc('\n', stderr);
            return false;
        }
        n = read(pty->master, bytes, sizeof(bytes));
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0) {
            int status;
            pid_t got = waitpid(pty->pid, &status, WNOHANG);

            if (got == pty->pid) {
                int code = WIFEXITED(status) ? WEXITSTATUS(status) :
                           WIFSIGNALED(status) ? 128 + WTERMSIG(status) : 255;

                (void)fprintf(stderr,
                              "live PTY: child exited before frame (status %d)\n",
                              code);
                pty->pid = -1;
            } else {
                (void)fprintf(stderr,
                              "live PTY: read before frame failed: %s\n",
                              strerror(errno));
            }
            return false;
        }
        if (!consume(pty, bytes, (size_t)n, deadline_ns))
            return false;
    }
    if (completed_ns != NULL)
        *completed_ns = yew_live_pty_now_ns();
    return true;
}

bool yew_live_pty_wait_quiet(YewLivePty *pty, i64 quiet_ns,
                             i64 deadline_ns)
{
    i64 quiet_deadline;

    if (pty == NULL || pty->master < 0 || quiet_ns <= 0)
        return false;
    quiet_deadline = yew_live_pty_now_ns();
    if (quiet_deadline < 0 || quiet_deadline >= deadline_ns ||
        quiet_ns > deadline_ns - quiet_deadline)
        return false;
    quiet_deadline += quiet_ns;
    for (;;) {
        struct pollfd fd = {pty->master, POLLIN | POLLHUP, 0};
        i64 now = yew_live_pty_now_ns();
        i64 wait_until;
        u8 bytes[8192];
        int result;
        ssize_t n;

        if (now < 0 || now >= deadline_ns)
            return false;
        if (now >= quiet_deadline)
            return true;
        wait_until = quiet_deadline < deadline_ns ? quiet_deadline :
                     deadline_ns;
        result = poll(&fd, 1U, timeout_ms(wait_until));
        if (result < 0 && errno == EINTR)
            continue;
        if (result == 0)
            continue;
        if (result < 0)
            return false;
        n = read(pty->master, bytes, sizeof(bytes));
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0 || !consume(pty, bytes, (size_t)n, deadline_ns))
            return false;
        now = yew_live_pty_now_ns();
        if (now < 0 || quiet_ns > deadline_ns - now)
            return false;
        quiet_deadline = now + quiet_ns;
    }
}

bool yew_live_pty_wait_exit(YewLivePty *pty, i64 deadline_ns, int *code)
{
    for (;;) {
        struct pollfd fd = {pty->master, POLLIN | POLLHUP, 0};
        int status;
        pid_t got = waitpid(pty->pid, &status, WNOHANG);

        if (got == pty->pid) {
            if (code != NULL)
                *code = WIFEXITED(status) ? WEXITSTATUS(status) :
                        WIFSIGNALED(status) ? 128 + WTERMSIG(status) : 255;
            pty->pid = -1;
            return true;
        }
        if (got < 0 && errno != EINTR)
            return false;
        if (yew_live_pty_now_ns() >= deadline_ns)
            return false;
        /* A child can keep painting while it processes the quit.  Drain
         * the master so a full PTY cannot block it in terminal output
         * before it gets back to input and exits. */
        if (poll(&fd, 1U, 1) < 0) {
            if (errno == EINTR)
                continue;
            return false;
        }
        if ((fd.revents & POLLIN) != 0) {
            u8 bytes[8192];
            ssize_t n;

            do {
                n = read(pty->master, bytes, sizeof(bytes));
            } while (n < 0 && errno == EINTR);
            if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK &&
                errno != EIO)
                return false;
        }
    }
}

void yew_live_pty_close(YewLivePty *pty)
{
    if (pty == NULL)
        return;
    if (pty->pid > 0) {
        (void)kill(pty->pid, SIGKILL);
        (void)waitpid(pty->pid, NULL, 0);
    }
    if (pty->master >= 0)
        (void)close(pty->master);
    pty->master = -1;
    pty->pid = -1;
}
