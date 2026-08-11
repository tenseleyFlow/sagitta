#define _POSIX_C_SOURCE 200809L

#include "support/live_pty.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

enum { LIVE_ROWS = 24, LIVE_COLS = 80 };

static bool read_all(int fd, u8 *bytes, size_t len)
{
    while (len != 0U) {
        ssize_t n = read(fd, bytes, len);

        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return false;
        bytes += (size_t)n;
        len -= (size_t)n;
    }
    return true;
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

static bool wait_hangup(YewLivePty *pty, i64 deadline)
{
    u8 discard[8192];

    while (yew_live_pty_now_ns() < deadline) {
        struct pollfd fd = {pty->master, POLLIN | POLLHUP, 0};
        int result = poll(&fd, 1U, 100);

        if (result < 0 && errno == EINTR)
            continue;
        if (result < 0)
            return false;
        if (result == 0)
            continue;
        if ((fd.revents & POLLIN) != 0) {
            for (;;) {
                ssize_t n = read(pty->master, discard, sizeof(discard));

                if (n > 0)
                    continue;
                if (n == 0 || (n < 0 && errno == EIO))
                    return true;
                if (n < 0 && errno == EINTR)
                    continue;
                if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
                    break;
                return false;
            }
        }
        if ((fd.revents & POLLHUP) != 0)
            return true;
    }
    return false;
}

static bool slurp(const char *path, u8 **out, size_t *out_len)
{
    struct stat st;
    u8 *bytes;
    int fd;
    bool ok;

    *out = NULL;
    *out_len = 0U;
    fd = open(path, O_RDONLY);
    if (fd < 0 || fstat(fd, &st) != 0 || st.st_size < 0 ||
        (uintmax_t)st.st_size > SIZE_MAX) {
        if (fd >= 0)
            (void)close(fd);
        return false;
    }
    bytes = malloc(st.st_size == 0 ? 1U : (size_t)st.st_size);
    if (bytes == NULL) {
        (void)close(fd);
        return false;
    }
    ok = read_all(fd, bytes, (size_t)st.st_size);
    if (close(fd) != 0)
        ok = false;
    if (!ok) {
        free(bytes);
        return false;
    }
    *out = bytes;
    *out_len = (size_t)st.st_size;
    return true;
}

static u64 line_count(const u8 *bytes, size_t len)
{
    u64 lines = 1U;
    size_t i;

    for (i = 0U; i < len; i++) {
        if (bytes[i] == (u8)'\n')
            lines++;
    }
    return lines;
}

static bool feed_replace(YewLivePty *pty, const u8 *old, size_t old_len,
                         const u8 *post, size_t post_len, i64 deadline)
{
    char erase[64];
    int n;
    u64 frame;
    static const char paste_begin[] = "i\033[200~";
    static const char paste_end[] = "\033[201~\033";

    n = snprintf(erase, sizeof(erase), "%lludd",
                 (unsigned long long)line_count(old, old_len));
    if (n <= 0 || (size_t)n >= sizeof(erase))
        return false;
    frame = pty->frames;
    if (!yew_live_pty_write(pty, erase, (size_t)n, deadline) ||
        !yew_live_pty_wait_frame(pty, frame, deadline, NULL))
        return false;
    frame = pty->frames;
    if (!yew_live_pty_write(pty, paste_begin, sizeof(paste_begin) - 1U,
                            deadline) ||
        !yew_live_pty_write(pty, post, post_len, deadline) ||
        !yew_live_pty_write(pty, paste_end, sizeof(paste_end) - 1U,
                            deadline) ||
        !yew_live_pty_wait_frame(pty, frame, deadline, NULL))
        return false;
    return true;
}

static void feeder(YewLivePty *pty, pid_t editor, int ready_fd,
                   const u8 *old, size_t old_len,
                   const u8 *post, size_t post_len)
{
    i64 deadline = yew_live_pty_now_ns() + INT64_C(3000000000);
    struct timespec escape_settle = {0, 50000000};
    static const char save = 's';
    static const char quit[] = ":ed.quit_force\r";

    if (!yew_live_pty_wait_frame(pty, 0U, deadline, NULL) ||
        !feed_replace(pty, old, old_len, post, post_len, deadline))
        _exit(3);
    while (nanosleep(&escape_settle, &escape_settle) != 0 && errno == EINTR)
        ;
    if (getenv("YEW_TORTURE_NO_SHIM") == NULL &&
        kill(editor, SIGUSR2) != 0)
        _exit(3);
    if (!yew_live_pty_write(pty, &save, 1U, deadline))
        _exit(3);
    if (ready_fd >= 0) {
        static const char ready = 'R';

        if (!write_all(ready_fd, &ready, 1U) || close(ready_fd) != 0)
            _exit(3);
    }
    {
        struct timespec settle = {0, 100000000};

        while (nanosleep(&settle, &settle) != 0 && errno == EINTR)
            ;
    }
    deadline = yew_live_pty_now_ns() + INT64_C(2000000000);
    (void)yew_live_pty_write(pty, quit, sizeof(quit) - 1U, deadline);
    deadline = yew_live_pty_now_ns() + INT64_C(3000000000);
    _exit(wait_hangup(pty, deadline) ? 0 : 3);
}

static int live_save(const char *path, const char *post_path)
{
    const char *binary = getenv("YEW_TORTURE_YEW");
    const char *ready_env = getenv("YEW_TORTURE_READY_FD");
    u8 *old = NULL;
    u8 *post = NULL;
    size_t old_len = 0U;
    size_t post_len = 0U;
    YewLivePty pty;
    char slave[128];
    int ready_fd = -1;
    pid_t feeder_pid;

    if (binary == NULL || *binary == '\0' ||
        !slurp(path, &old, &old_len) ||
        !slurp(post_path, &post, &post_len) ||
        !yew_live_pty_open(&pty, slave, sizeof(slave),
                           LIVE_ROWS, LIVE_COLS))
        goto fail;
    if (ready_env != NULL)
        ready_fd = (int)strtol(ready_env, NULL, 10);
    feeder_pid = fork();
    if (feeder_pid < 0)
        goto fail_pty;
    if (feeder_pid == 0)
        feeder(&pty, getppid(), ready_fd, old, old_len, post, post_len);

    free(old);
    free(post);
    old = NULL;
    post = NULL;
    if (ready_fd >= 0) {
        int flags = fcntl(ready_fd, F_GETFD);

        if (flags < 0 || fcntl(ready_fd, F_SETFD, flags | FD_CLOEXEC) != 0)
            goto fail_pty;
    }
    if (setenv("TERM", "xterm-256color", 1) != 0 ||
        setenv("COLORTERM", "truecolor", 1) != 0 ||
        setenv("YEW_LOG", "/dev/null", 1) != 0 ||
        setenv("YEW_FAULT_SIGNAL_ENABLE", "1", 1) != 0 ||
        !yew_live_pty_attach(&pty, slave, LIVE_ROWS, LIVE_COLS))
        _exit(126);
    yew_live_pty_exec(binary, path);

fail_pty:
    yew_live_pty_close(&pty);
fail:
    free(old);
    free(post);
    return 3;
}

static int delegate_check(char **argv)
{
    const char *checker = getenv("YEW_TORTURE_CHECKER");

    if (checker == NULL || *checker == '\0') {
        (void)fprintf(stderr,
                      "yew-live-torture: YEW_TORTURE_CHECKER is required\n");
        return 2;
    }
    execl(checker, checker, "--check", argv[2], argv[3], argv[4],
          (char *)NULL);
    return 126;
}

int main(int argc, char **argv)
{
    if (argc == 4 && strcmp(argv[1], "--save") == 0)
        return live_save(argv[2], argv[3]);
    if (argc == 5 && strcmp(argv[1], "--check") == 0)
        return delegate_check(argv);
    (void)fprintf(stderr,
                  "usage: %s --save PATH POST | --check PATH OLD POST\n",
                  argv[0]);
    return 2;
}
