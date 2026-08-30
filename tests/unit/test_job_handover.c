/* Sprint 52 §11: the one synchronous child inherits fd 0/1/2 and reports
 * exit, signal, and exec failure without entering the asynchronous table. */
#define _XOPEN_SOURCE 700

#include "harness.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#include "edit/ed.h"
#include "edit/job.h"

static size_t handover_read_all(int fd, char *out, size_t cap)
{
    size_t len = 0U;

    while (len + 1U < cap) {
        ssize_t got = read(fd, out + len, cap - len - 1U);

        if (got > 0) {
            len += (size_t)got;
        } else if (got < 0 && errno == EINTR) {
            continue;
        } else {
            break;
        }
    }
    out[len] = '\0';
    return len;
}

static int handover_open_pty(char *slave, size_t slave_cap,
                             struct termios *initial)
{
    const char *name;
    size_t name_len;
    int master;
    int slave_fd;

    master = posix_openpt(O_RDWR | O_NOCTTY);
    if (master < 0 || grantpt(master) != 0 || unlockpt(master) != 0)
        goto fail;
    name = ptsname(master);
    if (name == NULL)
        goto fail;
    name_len = strlen(name);
    if (name_len + 1U > slave_cap) {
        errno = ENAMETOOLONG;
        goto fail;
    }
    (void)memcpy(slave, name, name_len + 1U);
    slave_fd = open(slave, O_RDWR | O_NOCTTY);
    if (slave_fd < 0)
        goto fail;
    if (tcgetattr(slave_fd, initial) != 0) {
        int saved_errno = errno;

        (void)close(slave_fd);
        errno = saved_errno;
        goto fail;
    }
    (void)close(slave_fd);
    return master;

fail:
    if (master >= 0)
        (void)close(master);
    return -1;
}

static bool handover_termios_equal(const struct termios *left,
                                    const struct termios *right)
{
    /* tcgetattr need not initialize implementation padding. */
    return left->c_iflag == right->c_iflag &&
           left->c_oflag == right->c_oflag &&
           left->c_cflag == right->c_cflag &&
           left->c_lflag == right->c_lflag &&
           memcmp(left->c_cc, right->c_cc, sizeof(left->c_cc)) == 0 &&
           cfgetispeed(left) == cfgetispeed(right) &&
           cfgetospeed(left) == cfgetospeed(right);
}

static pid_t handover_wait_pty(pid_t child, int master, int *status)
{
    struct pollfd ready = {master, POLLIN, 0};
    char discard[256];
    int flags;
    int saved_errno;
    unsigned tick;

    flags = fcntl(master, F_GETFL);
    if (flags < 0 || fcntl(master, F_SETFL, flags | O_NONBLOCK) < 0)
        goto fail;
    for (tick = 0U; tick < 100U; tick++) {
        pid_t waited = waitpid(child, status, WNOHANG);

        if (waited == child)
            return waited;
        if (waited < 0 && errno != EINTR)
            goto fail;
        ready.revents = 0;
        if (poll(&ready, 1U, 50) < 0 && errno != EINTR)
            goto fail;
        if ((ready.revents & (POLLIN | POLLHUP | POLLERR)) != 0) {
            ssize_t got;

            do {
                got = read(master, discard, sizeof(discard));
            } while (got > 0 || (got < 0 && errno == EINTR));
            if ((ready.revents & POLLHUP) != 0)
                (void)poll(NULL, 0U, 50);
        }
    }
    errno = ETIMEDOUT;

fail:
    saved_errno = errno;
    (void)kill(child, SIGKILL);
    while (waitpid(child, status, 0) < 0 && errno == EINTR) {}
    errno = saved_errno;
    return -1;
}

static bool handover_attach_slave(const char *path)
{
    int slave;

    if (setsid() < 0)
        return false;
    slave = open(path, O_RDWR);
    if (slave < 0)
        return false;
#ifdef TIOCSCTTY
    if (ioctl(slave, TIOCSCTTY, 0) != 0) {
        (void)close(slave);
        return false;
    }
#endif
    if (dup2(slave, STDIN_FILENO) < 0 ||
        dup2(slave, STDOUT_FILENO) < 0 ||
        dup2(slave, STDERR_FILENO) < 0) {
        (void)close(slave);
        return false;
    }
    if (slave > STDERR_FILENO)
        (void)close(slave);
    return true;
}

static void handover_tty_child(const char *slave_path,
                               bool fatal_mid_handover)
{
    Ed ed;
    YewJobSpec spec = {0};
    YewJobWait result;
    struct termios actual;
    struct termios initial;
    struct termios raw;
    struct rlimit no_more_fds;
    char err[192];
    char *signal_argv[] = {
        (char *)"/bin/sh", (char *)"-c", (char *)"kill -TERM $$", NULL
    };
    char *exit_argv[] = {
        (char *)"/bin/sh", (char *)"-c", (char *)"exit 23", NULL
    };
    char *missing_argv[] = {(char *)"/definitely/not/yew-s52-tty", NULL};

    if (!handover_attach_slave(slave_path))
        _exit(101);
    /* The contract is to restore the terminal state yew inherited.  Linux
     * is allowed to normalize a newly attached controlling terminal, and
     * musl CI has observed that normalization between the parent's
     * pre-setsid sample and this point.  Sample after attachment, exactly
     * where yew_tty_open() does, so the test does not mistake kernel setup
     * for an editor mutation. */
    if (tcgetattr(STDIN_FILENO, &initial) != 0)
        _exit(119);
    yew_ed_init(&ed);
    if (!yew_tty_open(&ed.tty))
        _exit(102);
    ed.tty_ready = true;
    if (!yew_tty_raw(&ed.tty))
        _exit(103);
    yew_tty_altscreen(&ed.tty, true);
    if (!ed.tty.alt || !yew_tty_handover_begin(&ed.tty))
        _exit(104);
    if (tcgetattr(STDIN_FILENO, &actual) != 0 ||
        !handover_termios_equal(&actual, &initial))
        _exit(105);
    if (fatal_mid_handover) {
        (void)raise(SIGTERM);
        _exit(106);
    }
    if (!yew_tty_handover_end(&ed.tty))
        _exit(107);
    raw = initial;
    yew_tty_rawios(&raw);
    if (tcgetattr(STDIN_FILENO, &actual) != 0 ||
        !handover_termios_equal(&actual, &raw))
        _exit(108);

    spec.sink = YEW_SINK_DISCARD;
    spec.inherit_tty = true;
    spec.argv = missing_argv;
    if (!yew_job_run_sync(&ed, &spec, &result, err, sizeof(err)) ||
        result.state != YEW_JOB_EXECFAIL || result.exec_errno != ENOENT)
        _exit(109);
    if (tcgetattr(STDIN_FILENO, &actual) != 0 ||
        !handover_termios_equal(&actual, &raw))
        _exit(110);
    spec.argv = exit_argv;
    if (!yew_job_run_sync(&ed, &spec, &result, err, sizeof(err)) ||
        result.state != YEW_JOB_EXITED || result.exit_code != 23)
        _exit(111);
    if (tcgetattr(STDIN_FILENO, &actual) != 0 ||
        !handover_termios_equal(&actual, &raw))
        _exit(112);
    spec.argv = signal_argv;
    if (!yew_job_run_sync(&ed, &spec, &result, err, sizeof(err)) ||
        result.state != YEW_JOB_SIGNALED || result.termsig != SIGTERM)
        _exit(113);
    if (tcgetattr(STDIN_FILENO, &actual) != 0 ||
        !handover_termios_equal(&actual, &raw))
        _exit(114);

    /* Force the one fallible setup step that occurs after tty release.
     * Its failure must still pass through the common resume epilogue. */
    if (getenv("YEW_TEST_INSTRUMENTED") == NULL) {
        no_more_fds.rlim_cur = 3;
        no_more_fds.rlim_max = 3;
        if (setrlimit(RLIMIT_NOFILE, &no_more_fds) != 0)
            _exit(115);
        spec.argv = exit_argv;
        if (yew_job_run_sync(&ed, &spec, &result, err, sizeof(err)) ||
            strstr(err, "cannot create pipe") == NULL)
            _exit(116);
        if (tcgetattr(STDIN_FILENO, &actual) != 0 ||
            !handover_termios_equal(&actual, &raw))
            _exit(117);
    }
    yew_ed_free(&ed);
    if (tcgetattr(STDIN_FILENO, &actual) != 0 ||
        !handover_termios_equal(&actual, &initial))
        _exit(118);
    _exit(0);
}

static void handover_assert_tty_case(bool fatal_mid_handover)
{
    char slave[128];
    struct termios initial;
    struct termios after;
    pid_t child;
    pid_t waited;
    int master;
    int slave_fd;
    int status = 0;

    (void)memset(&initial, 0, sizeof(initial));
    master = handover_open_pty(slave, sizeof(slave), &initial);
    YEW_ASSERT(master >= 0);
    if (master < 0)
        return;
    child = fork();
    YEW_ASSERT(child >= 0);
    if (child == 0) {
        (void)close(master);
        handover_tty_child(slave, fatal_mid_handover);
    }
    if (child < 0) {
        (void)close(master);
        return;
    }
    waited = handover_wait_pty(child, master, &status);
    YEW_ASSERT_EQ_I64(waited, child);
    if (fatal_mid_handover) {
        YEW_ASSERT(WIFSIGNALED(status));
        if (WIFSIGNALED(status))
            YEW_ASSERT_EQ_I64(WTERMSIG(status), SIGTERM);
    } else {
        YEW_ASSERT(WIFEXITED(status));
        if (WIFEXITED(status))
            YEW_ASSERT_EQ_I64(WEXITSTATUS(status), 0);
    }
    (void)memset(&after, 0, sizeof(after));
    slave_fd = open(slave, O_RDWR | O_NOCTTY);
    YEW_ASSERT(slave_fd >= 0);
    YEW_ASSERT_EQ_I64(tcgetattr(slave_fd, &after), 0);
    YEW_ASSERT(handover_termios_equal(&after, &initial));
    YEW_ASSERT_EQ_I64(close(slave_fd), 0);
    YEW_ASSERT_EQ_I64(close(master), 0);
}

void test_job_handover_inherits_stdio_and_reports_all_outcomes(void)
{
    Ed ed;
    YewJobSpec spec = {0};
    YewJobWait result;
    char err[192];
    char out[64];
    char diag[64];
    char *stdio_argv[] = {
        (char *)"/bin/sh", (char *)"-c",
        (char *)"IFS= read -r line; printf 'out:%s' \"$line\"; "
                "printf 'err:%s' \"$line\" >&2",
        NULL
    };
    char *missing_argv[] = {(char *)"/definitely/not/yew-s52", NULL};
    char *exit_argv[] = {
        (char *)"/bin/sh", (char *)"-c", (char *)"exit 23", NULL
    };
    char *signal_argv[] = {
        (char *)"/bin/sh", (char *)"-c", (char *)"kill -TERM $$", NULL
    };
    int input[2];
    int output[2];
    int errors[2];
    int saved[3];
    bool ran;

    yew_ed_init(&ed);
    YEW_ASSERT(pipe(input) == 0);
    YEW_ASSERT(pipe(output) == 0);
    YEW_ASSERT(pipe(errors) == 0);
    YEW_ASSERT(write(input[1], "hello\n", 6U) == 6);
    (void)close(input[1]);
    saved[0] = dup(STDIN_FILENO);
    saved[1] = dup(STDOUT_FILENO);
    saved[2] = dup(STDERR_FILENO);
    YEW_ASSERT(saved[0] >= 0 && saved[1] >= 0 && saved[2] >= 0);
    YEW_ASSERT(dup2(input[0], STDIN_FILENO) == STDIN_FILENO);
    YEW_ASSERT(dup2(output[1], STDOUT_FILENO) == STDOUT_FILENO);
    YEW_ASSERT(dup2(errors[1], STDERR_FILENO) == STDERR_FILENO);
    (void)close(input[0]);
    (void)close(output[1]);
    (void)close(errors[1]);

    spec.argv = stdio_argv;
    spec.sink = YEW_SINK_DISCARD;
    spec.inherit_tty = true;
    ran = yew_job_run_sync(&ed, &spec, &result, err, sizeof(err));

    YEW_ASSERT(dup2(saved[0], STDIN_FILENO) == STDIN_FILENO);
    YEW_ASSERT(dup2(saved[1], STDOUT_FILENO) == STDOUT_FILENO);
    YEW_ASSERT(dup2(saved[2], STDERR_FILENO) == STDERR_FILENO);
    (void)close(saved[0]);
    (void)close(saved[1]);
    (void)close(saved[2]);
    (void)handover_read_all(output[0], out, sizeof(out));
    (void)handover_read_all(errors[0], diag, sizeof(diag));
    (void)close(output[0]);
    (void)close(errors[0]);

    YEW_ASSERT(ran);
    YEW_ASSERT_EQ_U64(result.state, YEW_JOB_EXITED);
    YEW_ASSERT_EQ_U64(result.exit_code, 0U);
    YEW_ASSERT_EQ_STR(out, "out:hello");
    YEW_ASSERT_EQ_STR(diag, "err:hello");
    YEW_ASSERT_EQ_U64(ed.jobs.len, 0U);

    spec.argv = missing_argv;
    YEW_ASSERT(yew_job_run_sync(&ed, &spec, &result, err, sizeof(err)));
    YEW_ASSERT_EQ_U64(result.state, YEW_JOB_EXECFAIL);
    YEW_ASSERT_EQ_U64(result.exec_errno, ENOENT);
    YEW_ASSERT_EQ_U64(result.exit_code, 127U);
    YEW_ASSERT_EQ_U64(ed.jobs.len, 0U);

    spec.argv = exit_argv;
    YEW_ASSERT(yew_job_run_sync(&ed, &spec, &result, err, sizeof(err)));
    YEW_ASSERT_EQ_U64(result.state, YEW_JOB_EXITED);
    YEW_ASSERT_EQ_U64(result.exit_code, 23U);
    YEW_ASSERT_EQ_U64(ed.jobs.len, 0U);

    spec.argv = signal_argv;
    YEW_ASSERT(yew_job_run_sync(&ed, &spec, &result, err, sizeof(err)));
    YEW_ASSERT_EQ_U64(result.state, YEW_JOB_SIGNALED);
    YEW_ASSERT_EQ_U64(result.termsig, SIGTERM);
    YEW_ASSERT_EQ_U64(ed.jobs.len, 0U);

    YEW_ASSERT_EQ_U64(yew_job_spawn(&ed, &spec, err, sizeof(err)), 0U);
    YEW_ASSERT(strstr(err, "synchronous runner") != NULL);
    yew_ed_free(&ed);

    handover_assert_tty_case(false);
    handover_assert_tty_case(true);
}
