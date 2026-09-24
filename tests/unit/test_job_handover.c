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
#include <sys/stat.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#include "edit/ed.h"
#include "edit/job.h"
#include "edit/shell.h"

static int handover_tcgetattr(int fd, struct termios *out);

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
    if (handover_tcgetattr(slave_fd, initial) != 0) {
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

static bool handover_cc_equal(const struct termios *left,
                              const struct termios *right)
{
#define HANDOVER_CC_EQ(name)                                                \
    do {                                                                    \
        if (left->c_cc[name] != right->c_cc[name])                          \
            return false;                                                   \
    } while (0)

    HANDOVER_CC_EQ(VEOF);
    HANDOVER_CC_EQ(VEOL);
    HANDOVER_CC_EQ(VERASE);
    HANDOVER_CC_EQ(VINTR);
    HANDOVER_CC_EQ(VKILL);
    HANDOVER_CC_EQ(VMIN);
    HANDOVER_CC_EQ(VQUIT);
    HANDOVER_CC_EQ(VSTART);
    HANDOVER_CC_EQ(VSTOP);
    HANDOVER_CC_EQ(VSUSP);
    HANDOVER_CC_EQ(VTIME);
#ifdef VDISCARD
    HANDOVER_CC_EQ(VDISCARD);
#endif
#ifdef VDSUSP
    HANDOVER_CC_EQ(VDSUSP);
#endif
#ifdef VEOL2
    HANDOVER_CC_EQ(VEOL2);
#endif
#ifdef VLNEXT
    HANDOVER_CC_EQ(VLNEXT);
#endif
#ifdef VREPRINT
    HANDOVER_CC_EQ(VREPRINT);
#endif
#ifdef VSTATUS
    HANDOVER_CC_EQ(VSTATUS);
#endif
#ifdef VSWTC
    HANDOVER_CC_EQ(VSWTC);
#endif
#ifdef VWERASE
    HANDOVER_CC_EQ(VWERASE);
#endif

#undef HANDOVER_CC_EQ
    return true;
}

static bool handover_termios_equal(const struct termios *left,
                                    const struct termios *right)
{
    /* Linux's termios ABI has fewer kernel control-character slots than
     * musl exposes in c_cc.  tcgetattr leaves that unused tail untouched,
     * so compare every named semantic slot rather than indeterminate bytes. */
    return left->c_iflag == right->c_iflag &&
           left->c_oflag == right->c_oflag &&
           left->c_cflag == right->c_cflag &&
           left->c_lflag == right->c_lflag &&
           handover_cc_equal(left, right) &&
           cfgetispeed(left) == cfgetispeed(right) &&
           cfgetospeed(left) == cfgetospeed(right);
}

static int handover_tcgetattr(int fd, struct termios *out)
{
    /* Keep the ABI tail visibly nonzero so this test cannot accidentally
     * depend on stack contents that tcgetattr is not required to replace. */
    (void)memset(out, 0xa5, sizeof(*out));
    return tcgetattr(fd, out);
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
    yew_ed_init(&ed);
    if (!yew_tty_open(&ed.tty))
        _exit(102);
    /* The handover contract is to restore the exact state yew inherited.
     * Use the snapshot taken by yew_tty_open itself: Linux may normalize a
     * newly attached controlling terminal between two successful tcgetattr
     * calls, particularly in Alpine's static-PIE environment. */
    initial = ed.tty.saved;
    ed.tty_ready = true;
    if (!yew_tty_raw(&ed.tty))
        _exit(103);
    yew_tty_altscreen(&ed.tty, true);
    if (!ed.tty.alt || !yew_tty_handover_begin(&ed.tty))
        _exit(104);
    if (handover_tcgetattr(STDIN_FILENO, &actual) != 0 ||
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
    if (handover_tcgetattr(STDIN_FILENO, &actual) != 0 ||
        !handover_termios_equal(&actual, &raw))
        _exit(108);

    spec.sink = YEW_SINK_DISCARD;
    spec.inherit_tty = true;
    spec.argv = missing_argv;
    if (!yew_job_run_sync(&ed, &spec, &result, err, sizeof(err)) ||
        result.state != YEW_JOB_EXECFAIL || result.exec_errno != ENOENT)
        _exit(109);
    if (handover_tcgetattr(STDIN_FILENO, &actual) != 0 ||
        !handover_termios_equal(&actual, &raw))
        _exit(110);
    spec.argv = exit_argv;
    if (!yew_job_run_sync(&ed, &spec, &result, err, sizeof(err)) ||
        result.state != YEW_JOB_EXITED || result.exit_code != 23)
        _exit(111);
    if (handover_tcgetattr(STDIN_FILENO, &actual) != 0 ||
        !handover_termios_equal(&actual, &raw))
        _exit(112);
    spec.argv = signal_argv;
    if (!yew_job_run_sync(&ed, &spec, &result, err, sizeof(err)) ||
        result.state != YEW_JOB_SIGNALED || result.termsig != SIGTERM)
        _exit(113);
    if (handover_tcgetattr(STDIN_FILENO, &actual) != 0 ||
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
        if (handover_tcgetattr(STDIN_FILENO, &actual) != 0 ||
            !handover_termios_equal(&actual, &raw))
            _exit(117);
    }
    yew_ed_free(&ed);
    if (handover_tcgetattr(STDIN_FILENO, &actual) != 0 ||
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
    YEW_ASSERT_EQ_I64(handover_tcgetattr(slave_fd, &after), 0);
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

/*
 * Sprint 57.18 §4 / DoD 4: `:!!` gets a real terminal and yew gets it
 * back, on EVERY exit path.
 *
 * Against a real pty in a forked child, because that is the only way to
 * ask the question honestly: the claim is about termios, the alternate
 * screen and the input modes, and a process with no controlling terminal
 * cannot answer it.  The PTY GOLDEN harness cannot cover this -- it
 * drives a whole editor and compares a cell grid, and the child here
 * OWNS the screen while it runs, so there is no grid to compare and
 * nothing deterministic to snapshot.  So the restore is proved by unit
 * test, as the sprint's testing strategy allows, and the goldens cover
 * the completion half.
 *
 * Every case re-asserts equality with yew's RAW termios afterwards, not
 * merely "not the child's": a handover that came back to the inherited
 * cooked state would leave the editor unable to read a keystroke.
 */
static void handover_shell_child(const char *slave_path)
{
    Ed ed;
    YewJobWait wait;
    struct termios actual;
    struct termios initial;
    struct termios raw;
    char err[192];
    char winch[128];

    if (!handover_attach_slave(slave_path))
        _exit(101);
    yew_ed_init(&ed);
    if (!yew_tty_open(&ed.tty))
        _exit(102);
    initial = ed.tty.saved;
    ed.tty_ready = true;
    if (!yew_tty_raw(&ed.tty))
        _exit(103);
    yew_tty_altscreen(&ed.tty, true);
    if (!ed.tty.alt)
        _exit(104);
    raw = initial;
    yew_tty_rawios(&raw);

    /* Normal exit. */
    if (!yew_shell_term_run(&ed, "exit 0", &wait, err, sizeof(err)) ||
        wait.state != YEW_JOB_EXITED || wait.exit_code != 0)
        _exit(105);
    if (handover_tcgetattr(STDIN_FILENO, &actual) != 0 ||
        !handover_termios_equal(&actual, &raw) || !ed.tty.alt)
        _exit(106);

    /* Non-zero exit. */
    if (!yew_shell_term_run(&ed, "exit 23", &wait, err, sizeof(err)) ||
        wait.state != YEW_JOB_EXITED || wait.exit_code != 23)
        _exit(107);
    if (handover_tcgetattr(STDIN_FILENO, &actual) != 0 ||
        !handover_termios_equal(&actual, &raw) || !ed.tty.alt)
        _exit(108);

    /* Signal death of the child. */
    if (!yew_shell_term_run(&ed, "exec /bin/sh -c 'kill -TERM $$'", &wait,
                            err, sizeof(err)) ||
        wait.state != YEW_JOB_SIGNALED || wait.termsig != SIGTERM)
        _exit(109);
    if (handover_tcgetattr(STDIN_FILENO, &actual) != 0 ||
        !handover_termios_equal(&actual, &raw) || !ed.tty.alt)
        _exit(110);

    /* A command the shell cannot exec.  `sh -c` itself started, so this
     * is the shell's 127 rather than YEW_JOB_EXECFAIL -- the true
     * exec failure is the missing-binary case above, which reaches the
     * same resume epilogue. */
    if (!yew_shell_term_run(&ed, "/definitely/not/yew-s5718", &wait, err,
                            sizeof(err)) ||
        wait.state != YEW_JOB_EXITED || wait.exit_code != 127)
        _exit(111);
    if (handover_tcgetattr(STDIN_FILENO, &actual) != 0 ||
        !handover_termios_equal(&actual, &raw) || !ed.tty.alt)
        _exit(112);

    /*
     * SIGWINCH during the run.  The child signals its PARENT -- this
     * process, sitting in waitpid -- which returns EINTR and must loop
     * rather than abandon the wait with the terminal still handed over.
     */
    (void)snprintf(winch, sizeof(winch),
                   "kill -WINCH %ld; exit 7", (long)getpid());
    if (!yew_shell_term_run(&ed, winch, &wait, err, sizeof(err)) ||
        wait.state != YEW_JOB_EXITED || wait.exit_code != 7)
        _exit(113);
    if (handover_tcgetattr(STDIN_FILENO, &actual) != 0 ||
        !handover_termios_equal(&actual, &raw) || !ed.tty.alt)
        _exit(114);

    /* An empty command never reaches the handover at all. */
    if (yew_shell_term_run(&ed, "   ", &wait, err, sizeof(err)) ||
        strstr(err, "needs a command") == NULL)
        _exit(115);
    if (handover_tcgetattr(STDIN_FILENO, &actual) != 0 ||
        !handover_termios_equal(&actual, &raw))
        _exit(116);

    yew_ed_free(&ed);
    /* And teardown leaves the terminal exactly as yew inherited it
     * (invariant 6). */
    if (handover_tcgetattr(STDIN_FILENO, &actual) != 0 ||
        !handover_termios_equal(&actual, &initial))
        _exit(117);
    _exit(0);
}

void test_shell_term_run_restores_the_terminal_on_every_exit(void)
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
        handover_shell_child(slave);
    }
    if (child < 0) {
        (void)close(master);
        return;
    }
    waited = handover_wait_pty(child, master, &status);
    YEW_ASSERT_EQ_I64(waited, child);
    YEW_ASSERT(WIFEXITED(status));
    if (WIFEXITED(status))
        YEW_ASSERT_EQ_I64(WEXITSTATUS(status), 0);
    (void)memset(&after, 0, sizeof(after));
    slave_fd = open(slave, O_RDWR | O_NOCTTY);
    YEW_ASSERT(slave_fd >= 0);
    YEW_ASSERT_EQ_I64(handover_tcgetattr(slave_fd, &after), 0);
    YEW_ASSERT(handover_termios_equal(&after, &initial));
    YEW_ASSERT_EQ_I64(close(slave_fd), 0);
    YEW_ASSERT_EQ_I64(close(master), 0);
}

/*
 * Sprint 57.31 §3: A-h's handover -- the SAME yew_job_run_sync epilogue
 * reached through yew_shell_term_argv -- restores the terminal on every
 * way `man` can end: a page read, no page, the man killed, and an exec
 * that fails outright.  The prompt the key was pressed in is the same
 * prompt afterwards.  A fake `man` first on PATH plays each outcome
 * ($YEW_TEST_MAN_MODE), in a real pty as the `:!!` case above.
 */
static bool handover_man_prompt_same(Ed *ed, u64 gen)
{
    Bytebuf text;
    bool same;

    bytebuf_init(&text);
    yew_cmdline_text(ed, &text);
    same = ed->cmdline.active && ed->cmdline.generation == gen &&
           text.len == 8U && memcmp(text.data, "!git che", 8U) == 0 &&
           ed->cmdline.cur.pos.v == 6U;
    bytebuf_free(&text);
    return same;
}

static void handover_man_child(const char *slave_path)
{
    static const char script[] =
        "#!/bin/sh\n"
        "case \"$YEW_TEST_MAN_MODE\" in\n"
        "ok) exit 0 ;;\n"
        "kill) kill -TERM $$ ;;\n"
        "esac\n"
        "exit 1\n";
    static const char *const modes[] = {"ok", "none", "kill"};
    Ed ed;
    YewJobWait wait;
    struct termios actual;
    struct termios initial;
    struct termios raw;
    CmdCtx cx;
    CmdId id;
    Win *target;
    char dir[] = "/tmp/yew-homan-XXXXXX";
    char man[64];
    char path[4096];
    char err[192];
    char *bad[] = {(char *)"/definitely/not/yew-s5731", NULL};
    const char *old_path = getenv("PATH");
    FILE *fp;
    u64 gen;
    size_t i;
    int n;

    if (!handover_attach_slave(slave_path))
        _exit(131);
    if (mkdtemp(dir) == NULL)
        _exit(132);
    n = snprintf(man, sizeof(man), "%s/man", dir);
    if (n <= 0 || (size_t)n >= sizeof(man))
        _exit(133);
    fp = fopen(man, "wb");
    if (fp == NULL || fwrite(script, 1U, sizeof(script) - 1U, fp) !=
                          sizeof(script) - 1U)
        _exit(134);
    if (fclose(fp) != 0 || chmod(man, 0755) != 0)
        _exit(135);
    n = snprintf(path, sizeof(path), "%s:%s", dir,
                 old_path == NULL ? "/usr/bin:/bin" : old_path);
    if (n <= 0 || (size_t)n >= sizeof(path) ||
        setenv("PATH", path, 1) != 0)
        _exit(136);
    yew_ed_init(&ed);
    if (!yew_ed_open_scratch(&ed) || !yew_tty_open(&ed.tty))
        _exit(137);
    ed.tty_ready = true;
    if (!yew_tty_raw(&ed.tty))
        _exit(138);
    yew_tty_altscreen(&ed.tty, true);
    initial = ed.tty.saved;
    raw = initial;
    yew_tty_rawios(&raw);
    yew_cmdline_open(&ed, YEW_PROMPT_CMD, "!git che");
    target = yew_cmdline_target(&ed);
    if (!ed.cmdline.active || target == NULL)
        _exit(139);
    target->cs.curs.data[target->cs.primary].pos = BYTEOFF(6U);
    target->cs.curs.data[target->cs.primary].anchor = BYTEOFF(6U);
    yew_cmdline_sync(&ed);
    gen = ed.cmdline.generation;
    id = yew_cmd_lookup("ed.cmdline.man_page", 19U);
    if (id.v == 0U)
        _exit(140);
    for (i = 0U; i < YEW_ARRAY_LEN(modes); i++) {
        if (setenv("YEW_TEST_MAN_MODE", modes[i], 1) != 0)
            _exit(141);
        (void)memset(&cx, 0, sizeof(cx));
        cx.win = yew_cmdline_target(&ed);
        cx.count = 1U;
        if (yew_ed_invoke(&ed, id, &cx) != YEW_CMD_OK)
            _exit((int)(142U + i));
        if (handover_tcgetattr(STDIN_FILENO, &actual) != 0 ||
            !handover_termios_equal(&actual, &raw) || !ed.tty.alt)
            _exit((int)(145U + i));
        if (!handover_man_prompt_same(&ed, gen) || !ed.full_damage)
            _exit((int)(148U + i));
        ed.full_damage = false;
    }
    /* The exec itself failing reaches the same epilogue. */
    if (!yew_shell_term_argv(&ed, bad, &wait, err, sizeof(err)) ||
        wait.state != YEW_JOB_EXECFAIL)
        _exit(151);
    if (handover_tcgetattr(STDIN_FILENO, &actual) != 0 ||
        !handover_termios_equal(&actual, &raw) || !ed.tty.alt)
        _exit(152);
    yew_ed_free(&ed);
    (void)unlink(man);
    (void)rmdir(dir);
    if (handover_tcgetattr(STDIN_FILENO, &actual) != 0 ||
        !handover_termios_equal(&actual, &initial))
        _exit(153);
    _exit(0);
}

void test_man_page_restores_the_terminal_on_every_exit(void)
{
    char slave[128];
    struct termios initial;
    struct termios after;
    pid_t child;
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
        handover_man_child(slave);
    }
    if (child < 0) {
        (void)close(master);
        return;
    }
    YEW_ASSERT_EQ_I64(handover_wait_pty(child, master, &status), child);
    YEW_ASSERT(WIFEXITED(status));
    if (WIFEXITED(status))
        YEW_ASSERT_EQ_I64(WEXITSTATUS(status), 0);
    (void)memset(&after, 0, sizeof(after));
    slave_fd = open(slave, O_RDWR | O_NOCTTY);
    YEW_ASSERT(slave_fd >= 0);
    YEW_ASSERT_EQ_I64(handover_tcgetattr(slave_fd, &after), 0);
    YEW_ASSERT(handover_termios_equal(&after, &initial));
    YEW_ASSERT_EQ_I64(close(slave_fd), 0);
    YEW_ASSERT_EQ_I64(close(master), 0);
}
