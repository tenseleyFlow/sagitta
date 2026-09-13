#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700

/*
 * Sprint 58 invariant 6.
 *
 * Exercise the eight fatal signals at four live editor boundaries without
 * adding a test hook to shipping code.  The existing torture interposer
 * stops a write immediately after BSU, or inside the fatal restore write;
 * the other two boundaries are synchronized by a live filter subprocess and
 * a fake LSP server that has received shutdown inside yew's 500 ms budget.
 */

#include "support/live_pty.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

enum { RESTORE_ROWS = 24, RESTORE_COLS = 80 };

static const u8 restore_blob[] =
    "\x1b[<u"
    "\x1b[?2004l"
    "\x1b[?1002l"
    "\x1b[?1006l"
    "\x1b[?1004l"
    "\x1b[?2026l"
    "\x1b[0m"
    "\x1b[0 q"
    "\x1b[?1049l"
    "\x1b[?25h";

typedef struct Capture {
    u8 *data;
    size_t len;
    size_t cap;
    bool failed;
} Capture;

typedef struct Trial {
    YewLivePty pty;
    Capture output;
    struct termios initial;
    char root[256];
    char state[320];
    char workspace[320];
    char source[384];
    char old_file[384];
    char post_file[384];
    char config[384];
    char ready[384];
    char marker[384];
    char armed[384];
    char slave[128];
    pid_t target;
    pid_t filter_pgid;
} Trial;

typedef struct SignalCase {
    int value;
    const char *name;
} SignalCase;

static const SignalCase signals[] = {
    {SIGSEGV, "SIGSEGV"}, {SIGBUS, "SIGBUS"},
    {SIGABRT, "SIGABRT"}, {SIGTERM, "SIGTERM"},
    {SIGINT, "SIGINT"},   {SIGQUIT, "SIGQUIT"},
    {SIGHUP, "SIGHUP"},   {SIGKILL, "SIGKILL"}
};

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

static bool write_file(const char *path, const void *data, size_t len)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    bool ok;

    if (fd < 0)
        return false;
    ok = write_all(fd, data, len);
    if (close(fd) != 0)
        ok = false;
    return ok;
}

static bool make_path(char *out, size_t cap, const char *a, const char *b)
{
    int n = snprintf(out, cap, "%s/%s", a, b);

    return n > 0 && (size_t)n < cap;
}

static bool remove_tree(const char *path)
{
    struct stat st;
    DIR *dir;
    struct dirent *entry;
    bool ok = true;

    if (lstat(path, &st) != 0)
        return errno == ENOENT;
    if (!S_ISDIR(st.st_mode))
        return unlink(path) == 0;
    dir = opendir(path);
    if (dir == NULL)
        return false;
    while ((entry = readdir(dir)) != NULL) {
        char child[1024];

        if (strcmp(entry->d_name, ".") == 0 ||
            strcmp(entry->d_name, "..") == 0)
            continue;
        if (!make_path(child, sizeof(child), path, entry->d_name) ||
            !remove_tree(child))
            ok = false;
    }
    if (closedir(dir) != 0)
        ok = false;
    return rmdir(path) == 0 && ok;
}

static void capture_output(void *opaque, const u8 *bytes, size_t len)
{
    Capture *capture = opaque;
    size_t cap;
    u8 *next;

    if (capture->failed || len == 0U)
        return;
    if (len > SIZE_MAX - capture->len) {
        capture->failed = true;
        return;
    }
    if (capture->len + len > capture->cap) {
        cap = capture->cap == 0U ? 8192U : capture->cap;
        while (cap < capture->len + len) {
            if (cap > SIZE_MAX / 2U) {
                capture->failed = true;
                return;
            }
            cap *= 2U;
        }
        next = realloc(capture->data, cap);
        if (next == NULL) {
            capture->failed = true;
            return;
        }
        capture->data = next;
        capture->cap = cap;
    }
    (void)memcpy(capture->data + capture->len, bytes, len);
    capture->len += len;
}

static bool contains_since(const Capture *capture, size_t at,
                           const void *needle, size_t needle_len)
{
    size_t i;

    if (capture->failed || at > capture->len ||
        needle_len > capture->len - at)
        return false;
    for (i = at; i <= capture->len - needle_len; i++)
        if (memcmp(capture->data + i, needle, needle_len) == 0)
            return true;
    return false;
}

static bool sync_open_since(const Capture *capture, size_t at)
{
    static const u8 bsu[] = "\x1b[?2026h";
    static const u8 esu[] = "\x1b[?2026l";
    bool open = false;
    bool seen = false;
    size_t i;

    if (capture->failed || at > capture->len)
        return false;
    for (i = at; i < capture->len; i++) {
        if (i + sizeof(bsu) - 1U <= capture->len &&
            memcmp(capture->data + i, bsu, sizeof(bsu) - 1U) == 0) {
            open = true;
            seen = true;
        } else if (i + sizeof(esu) - 1U <= capture->len &&
                   memcmp(capture->data + i, esu,
                          sizeof(esu) - 1U) == 0) {
            open = false;
        }
    }
    return seen && open;
}

static bool termios_equal(const struct termios *a, const struct termios *b)
{
    size_t i;

    if (a->c_iflag != b->c_iflag || a->c_oflag != b->c_oflag ||
        a->c_cflag != b->c_cflag || a->c_lflag != b->c_lflag ||
        cfgetispeed(a) != cfgetispeed(b) ||
        cfgetospeed(a) != cfgetospeed(b))
        return false;
    for (i = 0U; i < NCCS; i++)
        if (a->c_cc[i] != b->c_cc[i])
            return false;
    return true;
}

static bool trial_termios_restored(const Trial *trial)
{
    struct termios current;
    int fd = open(trial->slave, O_RDWR | O_NOCTTY);
    bool ok;

    if (fd < 0)
        return false;
    ok = tcgetattr(fd, &current) == 0 &&
         termios_equal(&trial->initial, &current);
    (void)close(fd);
    return ok;
}

static bool trial_prepare(Trial *trial, const char *fakelsp, bool with_lsp)
{
    static const u8 initial[] = "base\n";
    static const u8 post[] = "Xbase\n";
    char git_dir[384];
    char config_text[4096];
    int n;

    (void)memset(trial, 0, sizeof(*trial));
    trial->pty.master = -1;
    trial->pty.pid = -1;
    trial->target = -1;
    trial->filter_pgid = -1;
    (void)snprintf(trial->root, sizeof(trial->root),
                   "/tmp/yew-invariant6-XXXXXX");
    if (mkdtemp(trial->root) == NULL ||
        !make_path(trial->state, sizeof(trial->state), trial->root, "state") ||
        !make_path(trial->workspace, sizeof(trial->workspace), trial->root,
                   "workspace") ||
        mkdir(trial->state, 0700) != 0 || mkdir(trial->workspace, 0700) != 0 ||
        !make_path(git_dir, sizeof(git_dir), trial->workspace, ".git") ||
        mkdir(git_dir, 0700) != 0 ||
        !make_path(trial->source, sizeof(trial->source), trial->workspace,
                   "source.c") ||
        !make_path(trial->old_file, sizeof(trial->old_file), trial->workspace,
                   "old.bin") ||
        !make_path(trial->post_file, sizeof(trial->post_file),
                   trial->workspace, "post.bin") ||
        !make_path(trial->config, sizeof(trial->config), trial->workspace,
                   "config.fl") ||
        !make_path(trial->ready, sizeof(trial->ready), trial->root,
                   "lsp-ready") ||
        !make_path(trial->marker, sizeof(trial->marker), trial->root,
                   "moment-ready") ||
        !make_path(trial->armed, sizeof(trial->armed), trial->root,
                   "signal-armed") ||
        !write_file(trial->source, initial, sizeof(initial) - 1U) ||
        !write_file(trial->old_file, initial, sizeof(initial) - 1U) ||
        !write_file(trial->post_file, post, sizeof(post) - 1U))
        return false;
    if (with_lsp) {
        n = snprintf(config_text, sizeof(config_text),
            "let lsp = {servers: {c: {id: \"fakelsp\", cmd: \"%s\", "
            "args: [\"session-shutdown-delay\", \"%s\", \"%s\"], "
            "roots: [\".git\"], init_options: nil, "
            "init_timeout_ms: 3000}}}\n",
            fakelsp, trial->marker, trial->ready);
    } else {
        n = snprintf(config_text, sizeof(config_text),
                     "let lsp = {servers: {}}\n");
    }
    return n > 0 && (size_t)n < sizeof(config_text) &&
           write_file(trial->config, config_text, (size_t)n);
}

static void child_environment(const Trial *trial, const char *runtime,
                              const char *shim, const char *stop_control)
{
    size_t i;

    for (i = 0U; i < YEW_ARRAY_LEN(signals); i++)
        (void)signal(signals[i].value, SIG_DFL);
    if (setenv("TERM", "xterm-256color", 1) != 0 ||
        setenv("COLORTERM", "truecolor", 1) != 0 ||
        setenv("YEW_COLORS", "truecolor", 1) != 0 ||
        setenv("YEW_TTY_PROBE", "1", 1) != 0 ||
        setenv("YEW_PROBE_TIMEOUT_MS", "500", 1) != 0 ||
        setenv("YEW_ESC_TIMEOUT_MS", "25", 1) != 0 ||
        setenv("SHELL", "/bin/sh", 1) != 0 ||
        setenv("XDG_STATE_HOME", trial->state, 1) != 0 ||
        setenv("YEW_LOG", "/dev/null", 1) != 0 ||
        setenv("YEW_MOUSE", "0", 1) != 0 ||
        setenv("YEW_NO_SYN_CACHE", "1", 1) != 0 ||
        setenv("YEW_RUNTIME_DIR", runtime, 1) != 0)
        _exit(126);
    if (stop_control == NULL)
        return;
#if defined(__APPLE__)
    if (setenv("DYLD_INSERT_LIBRARIES", shim, 1) != 0 ||
        setenv("DYLD_FORCE_FLAT_NAMESPACE", "1", 1) != 0)
#else
    if (setenv("LD_PRELOAD", shim, 1) != 0)
#endif
        _exit(126);
    if (setenv("YEW_FAULT_STORAGE_ONLY", "1", 1) != 0 ||
        setenv("YEW_FAULT_SIGNAL_ENABLE", "1", 1) != 0 ||
        setenv("YEW_FAULT_TTY_ARMED_MARKER", trial->armed, 1) != 0 ||
        setenv("YEW_FAULT_TTY_STOP_MARKER", trial->marker, 1) != 0 ||
        setenv(stop_control, "1", 1) != 0)
        _exit(126);
}

static bool trial_spawn(Trial *trial, const char *yew, const char *runtime,
                        const char *shim, const char *stop_control)
{
    int pid_pipe[2] = {-1, -1};
    int fd;
    pid_t host;
    ssize_t got;

    if (!yew_live_pty_open(&trial->pty, trial->slave, sizeof(trial->slave),
                           RESTORE_ROWS, RESTORE_COLS))
        return false;
    fd = open(trial->slave, O_RDWR | O_NOCTTY);
    if (fd < 0 || tcgetattr(fd, &trial->initial) != 0) {
        if (fd >= 0)
            (void)close(fd);
        return false;
    }
    if (close(fd) != 0)
        return false;
    if (pipe(pid_pipe) != 0)
        return false;
    host = fork();
    if (host < 0) {
        (void)close(pid_pipe[0]);
        (void)close(pid_pipe[1]);
        return false;
    }
    if (host == 0) {
        pid_t target;
        int status;
        ssize_t written;

        (void)close(pid_pipe[0]);
        if (!yew_live_pty_attach(&trial->pty, trial->slave,
                                 RESTORE_ROWS, RESTORE_COLS))
            _exit(126);
        target = fork();
        if (target < 0)
            _exit(126);
        if (target == 0) {
            char *args[] = {(char *)yew, (char *)"--config", trial->config,
                            trial->source, NULL};

            (void)close(pid_pipe[1]);
            child_environment(trial, runtime, shim, stop_control);
            if (chdir(trial->workspace) != 0)
                _exit(126);
            yew_live_pty_exec_argv(yew, args);
        }
        do {
            written = write(pid_pipe[1], &target, sizeof(target));
        } while (written < 0 && errno == EINTR);
        (void)close(pid_pipe[1]);
        if (written != (ssize_t)sizeof(target))
            _exit(126);
        while (waitpid(target, &status, 0) < 0)
            if (errno != EINTR)
                _exit(126);
        {
            struct timespec grace = {0, 250000000L};

            while (nanosleep(&grace, &grace) != 0 && errno == EINTR) {}
        }
        if (WIFSIGNALED(status)) {
            int sig = WTERMSIG(status);

            (void)signal(sig, SIG_DFL);
            (void)raise(sig);
            _exit(128 + sig);
        }
        _exit(WIFEXITED(status) ? WEXITSTATUS(status) : 126);
    }
    (void)close(pid_pipe[1]);
    do {
        got = read(pid_pipe[0], &trial->target, sizeof(trial->target));
    } while (got < 0 && errno == EINTR);
    (void)close(pid_pipe[0]);
    if (got != (ssize_t)sizeof(trial->target)) {
        (void)kill(host, SIGKILL);
        (void)waitpid(host, NULL, 0);
        return false;
    }
    trial->pty.pid = host;
    yew_live_pty_set_output(&trial->pty, capture_output, &trial->output);
    return true;
}

static bool wait_initial_frame(Trial *trial)
{
    i64 deadline = yew_live_pty_now_ns() + INT64_C(5000000000);

    return yew_live_pty_wait_frame(&trial->pty, 0U, deadline, NULL);
}

static bool wait_file(Trial *trial, const char *path, i64 seconds)
{
    i64 deadline = yew_live_pty_now_ns() + seconds * INT64_C(1000000000);

    while (yew_live_pty_now_ns() < deadline) {
        i64 slice = yew_live_pty_now_ns() + INT64_C(50000000);

        if (access(path, F_OK) == 0)
            return true;
        (void)yew_live_pty_wait_quiet(&trial->pty, INT64_C(2000000), slice);
    }
    return access(path, F_OK) == 0;
}

static bool wait_nonempty_file(Trial *trial, const char *path, i64 seconds)
{
    i64 deadline = yew_live_pty_now_ns() + seconds * INT64_C(1000000000);
    struct stat st;

    while (yew_live_pty_now_ns() < deadline) {
        i64 slice = yew_live_pty_now_ns() + INT64_C(50000000);

        if (stat(path, &st) == 0 && st.st_size > 0)
            return true;
        (void)yew_live_pty_wait_quiet(&trial->pty, INT64_C(2000000), slice);
    }
    return stat(path, &st) == 0 && st.st_size > 0;
}

static bool arm_faults(Trial *trial)
{
    return kill(trial->target, SIGUSR2) == 0 &&
           wait_nonempty_file(trial, trial->armed, 3);
}

static bool read_pid_file(const char *path, pid_t *pid)
{
    char text[64];
    char *end;
    long value;
    int fd = open(path, O_RDONLY);
    ssize_t n;

    if (fd < 0)
        return false;
    do {
        n = read(fd, text, sizeof(text) - 1U);
    } while (n < 0 && errno == EINTR);
    (void)close(fd);
    if (n <= 0)
        return false;
    text[n] = '\0';
    errno = 0;
    value = strtol(text, &end, 10);
    if (errno != 0 || end == text || (*end != '\0' && *end != '\n') ||
        value <= 1)
        return false;
    *pid = (pid_t)value;
    return true;
}

static bool wait_stopped(Trial *trial)
{
    i64 deadline;

    if (!wait_file(trial, trial->marker, 5))
        return false;
    deadline = yew_live_pty_now_ns() + INT64_C(100000000);
    (void)yew_live_pty_wait_quiet(&trial->pty, INT64_C(10000000), deadline);
    return true;
}

static bool send_edit(Trial *trial)
{
    static const char insert[] = "iX";
    static const char escape = '\x1b';
    i64 deadline = yew_live_pty_now_ns() + INT64_C(3000000000);
    u64 frame = trial->pty.frames;

    if (!yew_live_pty_write(&trial->pty, insert, sizeof(insert) - 1U,
                            deadline))
        return false;
    if (!yew_live_pty_wait_frame(&trial->pty, frame, deadline, NULL))
        return false;
    if (!yew_live_pty_write(&trial->pty, &escape, 1U, deadline))
        return false;
    /* A bare Escape is deliberately ambiguous in the legacy terminal
     * profile until esc_timeout expires.  Waiting for merely one frame can
     * consume an unrelated git/LSP repaint and leave the editor in I mode. */
    return yew_live_pty_wait_quiet(&trial->pty, INT64_C(50000000), deadline);
}

static bool trigger_resize(Trial *trial)
{
    struct winsize size;

    (void)memset(&size, 0, sizeof(size));
    size.ws_row = RESTORE_ROWS + 1U;
    size.ws_col = RESTORE_COLS;
    return ioctl(trial->pty.master, TIOCSWINSZ, &size) == 0 &&
           kill(trial->target, SIGWINCH) == 0;
}

static bool drain_signal(Trial *trial, int *status_out)
{
    i64 deadline = yew_live_pty_now_ns() + INT64_C(5000000000);
    bool reaped = false;
    bool eof = false;
    int status = 0;

    while (yew_live_pty_now_ns() < deadline && (!reaped || !eof)) {
        struct pollfd fd = {trial->pty.master, POLLIN | POLLHUP, 0};
        int result;

        if (!reaped) {
            pid_t got = waitpid(trial->pty.pid, &status, WNOHANG);

            if (got == trial->pty.pid) {
                reaped = true;
                trial->pty.pid = -1;
            } else if (got < 0 && errno != EINTR) {
                return false;
            }
        }
        result = poll(&fd, 1U, 10);
        if (result < 0 && errno == EINTR)
            continue;
        if (result < 0)
            return false;
        if (result > 0 && (fd.revents & (POLLIN | POLLHUP)) != 0) {
            u8 bytes[8192];
            ssize_t n;

            do {
                n = read(trial->pty.master, bytes, sizeof(bytes));
            } while (n < 0 && errno == EINTR);
            if (n > 0)
                capture_output(&trial->output, bytes, (size_t)n);
            else if (n == 0 || (n < 0 && errno == EIO))
                eof = true;
            else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)
                return false;
        }
    }
    if (!reaped || !eof || trial->output.failed)
        return false;
    /* The host reaps target before mirroring its status, so neither PID is
     * live now.  Clear target before cleanup can accidentally signal a PID
     * that the system has already reused. */
    trial->target = -1;
    *status_out = status;
    return true;
}

static bool signal_and_finish(Trial *trial, int sig, bool stopped,
                              int *status)
{
    if (kill(trial->target, sig) != 0)
        return false;
    if (stopped && sig != SIGKILL && kill(trial->target, SIGCONT) != 0)
        return false;
    return drain_signal(trial, status);
}

static bool run_reset(Trial *trial)
{
    i64 deadline = yew_live_pty_now_ns() + INT64_C(3000000000);
    pid_t pid = fork();
    int status = 0;
    bool reaped = false;

    if (pid < 0)
        return false;
    if (pid == 0) {
        if (setenv("TERM", "xterm-256color", 1) != 0 ||
            !yew_live_pty_attach(&trial->pty, trial->slave,
                                 RESTORE_ROWS, RESTORE_COLS))
            _exit(126);
        execlp("reset", "reset", (char *)NULL);
        _exit(126);
    }
    while (yew_live_pty_now_ns() < deadline && !reaped) {
        struct pollfd fd = {trial->pty.master, POLLIN | POLLHUP, 0};
        pid_t got = waitpid(pid, &status, WNOHANG);

        if (got == pid) {
            reaped = true;
            break;
        }
        if (got < 0 && errno != EINTR)
            break;
        if (poll(&fd, 1U, 10) > 0 && (fd.revents & POLLIN) != 0) {
            u8 bytes[1024];
            ssize_t n = read(trial->pty.master, bytes, sizeof(bytes));

            if (n > 0)
                capture_output(&trial->output, bytes, (size_t)n);
        }
    }
    if (!reaped) {
        (void)kill(pid, SIGKILL);
        (void)waitpid(pid, &status, 0);
        return false;
    }
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

static bool checker_passes(const Trial *trial, const char *checker)
{
    pid_t pid = fork();
    int status;

    if (pid < 0)
        return false;
    if (pid == 0) {
        if (setenv("XDG_STATE_HOME", trial->state, 1) != 0)
            _exit(126);
        (void)unsetenv("LD_PRELOAD");
        (void)unsetenv("DYLD_INSERT_LIBRARIES");
        (void)unsetenv("DYLD_FORCE_FLAT_NAMESPACE");
        execl(checker, checker, "--check", trial->source,
              trial->old_file, trial->post_file, (char *)NULL);
        _exit(126);
    }
    while (waitpid(pid, &status, 0) < 0)
        if (errno != EINTR)
            return false;
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

static bool enter_filter_moment(Trial *trial)
{
    char command[768];
    int n = snprintf(command, sizeof(command),
        ":%%!printf '%%d' $$ > %s; sleep 30; cat\r", trial->marker);
    i64 deadline = yew_live_pty_now_ns() + INT64_C(3000000000);
    static const char queued[] = "iQUEUED";
    size_t queued_at;

    if (n <= 0 || (size_t)n >= sizeof(command)) {
        (void)fprintf(stderr, "invariant-6: filter command overflow\n");
        return false;
    }
    if (!yew_live_pty_write(&trial->pty, command, (size_t)n, deadline)) {
        (void)fprintf(stderr, "invariant-6: filter command write failed\n");
        return false;
    }
    if (!wait_nonempty_file(trial, trial->marker, 3)) {
        (void)fprintf(stderr, "invariant-6: filter marker timeout\n");
        return false;
    }
    if (!read_pid_file(trial->marker, &trial->filter_pgid)) {
        (void)fprintf(stderr, "invariant-6: filter pid marker invalid\n");
        return false;
    }
    queued_at = trial->output.len;
    if (!yew_live_pty_write(&trial->pty, queued, sizeof(queued) - 1U,
                            deadline)) {
        (void)fprintf(stderr, "invariant-6: filter typeahead write failed\n");
        return false;
    }
    return yew_live_pty_wait_quiet(&trial->pty, INT64_C(50000000), deadline) &&
           !contains_since(&trial->output, queued_at, "QUEUED", 6U);
}

static bool enter_lsp_shutdown_moment(Trial *trial)
{
    static const char quit[] = ":ed.quit_force\r";
    i64 deadline = yew_live_pty_now_ns() + INT64_C(3000000000);

    return wait_file(trial, trial->ready, 5) &&
           yew_live_pty_write(&trial->pty, quit, sizeof(quit) - 1U,
                              deadline) &&
           wait_file(trial, trial->marker, 3);
}

static void trial_dispose(Trial *trial)
{
    if (trial->filter_pgid > 1)
        (void)kill(-trial->filter_pgid, SIGKILL);
    if (trial->target > 1)
        (void)kill(trial->target, SIGKILL);
    yew_live_pty_close(&trial->pty);
    free(trial->output.data);
    if (trial->root[0] != '\0')
        (void)remove_tree(trial->root);
}

static bool run_trial(const char *moment, const SignalCase *sig,
                      const char *yew, const char *shim,
                      const char *fakelsp, const char *checker,
                      const char *runtime)
{
    Trial trial;
    const char *stop = NULL;
    bool with_lsp = strcmp(moment, "lsp-shutdown") == 0;
    bool stopped = false;
    bool ok = false;
    size_t checkpoint = 0U;
    size_t moment_checkpoint = 0U;
    int status = 0;
    const char *stage = "prepare";

    if (strcmp(moment, "mid-render") == 0)
        stop = "YEW_FAULT_TTY_STOP_AFTER_BSU";
    else if (strcmp(moment, "second-signal") == 0)
        stop = "YEW_FAULT_TTY_STOP_IN_RESTORE";
    if (!trial_prepare(&trial, fakelsp, with_lsp))
        goto done;
    stage = "spawn";
    if (!trial_spawn(&trial, yew, runtime, shim, stop))
        goto done;
    stage = "initial-frame";
    if (!wait_initial_frame(&trial))
        goto done;

    stage = "edit";
    if (!send_edit(&trial))
        goto done;
    checkpoint = trial.output.len;

    if (strcmp(moment, "mid-render") == 0) {
        stage = "mid-render-stop";
        moment_checkpoint = trial.output.len;
        if (!arm_faults(&trial) ||
            !trigger_resize(&trial) || !wait_stopped(&trial))
            goto done;
        if (!sync_open_since(&trial.output, moment_checkpoint)) {
            stage = "mid-render-boundary";
            goto done;
        }
        stopped = true;
        checkpoint = trial.output.len;
    } else {
        if (strcmp(moment, "second-signal") == 0) {
            stage = "restore-stop";
            if (!arm_faults(&trial) ||
                kill(trial.target, SIGTERM) != 0 ||
                !wait_stopped(&trial))
                goto done;
            if (!contains_since(&trial.output, checkpoint, "\x1b[<u", 4U) ||
                contains_since(&trial.output, checkpoint, restore_blob,
                               sizeof(restore_blob) - 1U)) {
                stage = "restore-boundary";
                goto done;
            }
            stopped = true;
        } else if (strcmp(moment, "filter-typeahead") == 0) {
            stage = "filter-moment";
            if (!enter_filter_moment(&trial))
                goto done;
            checkpoint = trial.output.len;
        } else if (strcmp(moment, "lsp-shutdown") == 0) {
            stage = "lsp-shutdown-moment";
            if (!enter_lsp_shutdown_moment(&trial))
                goto done;
            checkpoint = trial.output.len;
        } else {
            goto done;
        }
    }

    stage = "signal-finish";
    if (!signal_and_finish(&trial, sig->value, stopped, &status))
        goto done;
    stage = "signal-status";
    if (!WIFSIGNALED(status) ||
        (strcmp(moment, "second-signal") != 0 &&
         WTERMSIG(status) != sig->value) ||
        (strcmp(moment, "second-signal") == 0 &&
         WTERMSIG(status) != sig->value && WTERMSIG(status) != SIGTERM))
        goto done;
    stage = "restore-bytes";
    if (!contains_since(&trial.output, checkpoint, restore_blob,
                        sizeof(restore_blob) - 1U))
        goto done;
    stage = "termios";
    if (!trial_termios_restored(&trial))
        goto done;
    if (sig->value == SIGKILL) {
        stage = "reset";
        if (!run_reset(&trial))
            goto done;
        stage = "journal";
        if (!checker_passes(&trial, checker))
            goto done;
    }
    ok = true;

done:
    if (ok)
        (void)printf("invariant-6: %s %s ok\n", moment, sig->name);
    else
    {
        size_t i;
        size_t begin = trial.output.len > 800U ? trial.output.len - 800U : 0U;

        (void)fprintf(stderr,
            "invariant-6: %s %s FAILED at %s status=%d output=%zu checkpoint=%zu\n",
            moment, sig->name, stage, status, trial.output.len, checkpoint);
        (void)fputs("invariant-6: output tail: ", stderr);
        for (i = begin; i < trial.output.len; i++) {
            u8 byte = trial.output.data[i];

            (void)fputc(byte >= 0x20U && byte < 0x7fU ? byte : '.', stderr);
        }
        (void)fputc('\n', stderr);
    }
    trial_dispose(&trial);
    return ok;
}

static const char *arg_value(int argc, char **argv, const char *name)
{
    int i;

    for (i = 1; i + 1 < argc; i++)
        if (strcmp(argv[i], name) == 0)
            return argv[i + 1];
    return NULL;
}

int main(int argc, char **argv)
{
    static const char *moments[] = {
        "mid-render", "second-signal", "filter-typeahead", "lsp-shutdown"
    };
    const char *yew = arg_value(argc, argv, "--yew");
    const char *shim = arg_value(argc, argv, "--shim");
    const char *fakelsp = arg_value(argc, argv, "--fakelsp");
    const char *checker = arg_value(argc, argv, "--checker");
    const char *runtime = arg_value(argc, argv, "--runtime");
    const char *only_moment = arg_value(argc, argv, "--moment");
    const char *only_signal = arg_value(argc, argv, "--signal");
    size_t moment;
    size_t signal_index;
    size_t trials = 0U;

    if (yew == NULL || shim == NULL || fakelsp == NULL || checker == NULL ||
        runtime == NULL) {
        (void)fprintf(stderr,
            "usage: %s --yew PATH --shim PATH --fakelsp PATH "
            "--checker PATH --runtime DIR\n", argv[0]);
        return 2;
    }
    for (moment = 0U; moment < YEW_ARRAY_LEN(moments); moment++) {
        if (only_moment != NULL && strcmp(only_moment, moments[moment]) != 0)
            continue;
        for (signal_index = 0U; signal_index < YEW_ARRAY_LEN(signals);
             signal_index++) {
            if (only_signal != NULL &&
                strcmp(only_signal, signals[signal_index].name) != 0)
                continue;
            if (!run_trial(moments[moment], &signals[signal_index], yew,
                           shim, fakelsp, checker, runtime))
                return 1;
            trials++;
        }
    }
    (void)printf("invariant-6: %zu signal/moment trials passed\n", trials);
    return 0;
}
