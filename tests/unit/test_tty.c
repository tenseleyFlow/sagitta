#define _XOPEN_SOURCE 700

#include "harness.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#include "term/tty.h"

typedef struct {
    const char *name;
    const char *value;
} TestEnvEntry;

typedef struct {
    const u8 *stream;
    size_t len;
    bool kitty_kbd;
    u32 kitty_flags;
    bool sync_output;
    bool da1_seen;
} ProbeFixture;

static const TestEnvEntry *test_env;
static size_t test_env_len;

static const char *test_getenv(const char *name)
{
    size_t i;

    for (i = 0U; i < test_env_len; i++) {
        if (strcmp(test_env[i].name, name) == 0)
            return test_env[i].value;
    }
    return NULL;
}

static void set_test_env(const TestEnvEntry *entries, size_t len)
{
    test_env = entries;
    test_env_len = len;
}

static void tty_fixture_init(Tty *t, int pipefd[2], i64 now_ms)
{
    memset(t, 0, sizeof(*t));
    t->rfd = -1;
    YEW_ASSERT_EQ_I64(pipe(pipefd), 0);
    t->wfd = pipefd[1];
    bytebuf_init(&t->pending);
    set_test_env(NULL, 0U);
    yew_tty_probe_config(t, now_ms, test_getenv);
}

static void tty_fixture_free(Tty *t, int pipefd[2])
{
    bytebuf_free(&t->pending);
    YEW_ASSERT_EQ_I64(close(pipefd[0]), 0);
    YEW_ASSERT_EQ_I64(close(pipefd[1]), 0);
}

static void assert_caps(const Tty *t, const ProbeFixture *fixture)
{
    YEW_ASSERT(t->caps.probed);
    YEW_ASSERT(t->caps.kitty_kbd == fixture->kitty_kbd);
    YEW_ASSERT_EQ_U64(t->caps.kitty_flags, fixture->kitty_flags);
    YEW_ASSERT(t->caps.sync_output == fixture->sync_output);
    YEW_ASSERT(t->caps.da1_seen == fixture->da1_seen);
}

static void run_probe_fixture(const ProbeFixture *fixture, size_t split,
                              Tty *t, int pipefd[2])
{
    tty_fixture_init(t, pipefd, 1000);
    (void)yew_tty_probe_feed(t, fixture->stream, split);
    (void)yew_tty_probe_feed(t, fixture->stream + split,
                             fixture->len - split);
}

void test_tty_raw_input_flags(void)
{
    struct termios io;

    memset(&io, 0, sizeof(io));
    io.c_iflag = IXON | ICRNL | INLCR | IGNCR | BRKINT | IGNBRK | PARMRK |
                 ISTRIP | INPCK;
    yew_tty_rawios(&io);
    YEW_ASSERT((io.c_iflag & IXON) == 0);
    YEW_ASSERT((io.c_iflag & ICRNL) == 0);
    YEW_ASSERT((io.c_iflag & INLCR) == 0);
    YEW_ASSERT((io.c_iflag & IGNCR) == 0);
    YEW_ASSERT((io.c_iflag & BRKINT) == 0);
    YEW_ASSERT((io.c_iflag & IGNBRK) == 0);
    YEW_ASSERT((io.c_iflag & PARMRK) == 0);
    YEW_ASSERT((io.c_iflag & ISTRIP) == 0);
    YEW_ASSERT((io.c_iflag & INPCK) == 0);
}

void test_tty_raw_output_flags(void)
{
    struct termios io;

    memset(&io, 0, sizeof(io));
    io.c_oflag = OPOST;
    yew_tty_rawios(&io);
    YEW_ASSERT((io.c_oflag & OPOST) == 0);
}

void test_tty_raw_local_flags(void)
{
    struct termios io;

    memset(&io, 0, sizeof(io));
    io.c_lflag = ECHO | ECHONL | ICANON | ISIG | IEXTEN;
    yew_tty_rawios(&io);
    YEW_ASSERT((io.c_lflag & ECHO) == 0);
    YEW_ASSERT((io.c_lflag & ECHONL) == 0);
    YEW_ASSERT((io.c_lflag & ICANON) == 0);
    YEW_ASSERT((io.c_lflag & ISIG) == 0);
    YEW_ASSERT((io.c_lflag & IEXTEN) == 0);
}

void test_tty_raw_control_flags(void)
{
    struct termios io;
    struct termios once;

    memset(&io, 0, sizeof(io));
    io.c_cflag = CSIZE | PARENB | CLOCAL;
    io.c_cc[VMIN] = 9;
    io.c_cc[VTIME] = 9;
    yew_tty_rawios(&io);
    YEW_ASSERT((io.c_cflag & CSIZE) == CS8);
    YEW_ASSERT((io.c_cflag & PARENB) == 0);
    YEW_ASSERT((io.c_cflag & CLOCAL) != 0);
    YEW_ASSERT_EQ_U64(io.c_cc[VMIN], 1U);
    YEW_ASSERT_EQ_U64(io.c_cc[VTIME], 0U);
    once = io;
    yew_tty_rawios(&io);
    YEW_ASSERT_EQ_MEM(&io, &once, sizeof(io));
}

void test_tty_raw_poll_sleeps_when_idle(void)
{
    struct termios io;
    struct pollfd pfd;
    char bytes[8];
    char *name;
    int master;
    int slave;
    ssize_t n;

    master = posix_openpt(O_RDWR | O_NOCTTY);
    YEW_ASSERT(master >= 0);
    YEW_ASSERT_EQ_I64(grantpt(master), 0);
    YEW_ASSERT_EQ_I64(unlockpt(master), 0);
    name = ptsname(master);
    YEW_ASSERT_NOT_NULL(name);
    slave = open(name, O_RDWR | O_NOCTTY);
    YEW_ASSERT(slave >= 0);
    YEW_ASSERT_EQ_I64(tcgetattr(slave, &io), 0);
    yew_tty_rawios(&io);
    YEW_ASSERT_EQ_I64(tcsetattr(slave, TCSANOW, &io), 0);

    pfd = (struct pollfd){slave, POLLIN | POLLHUP, 0};
    YEW_ASSERT_EQ_I64(poll(&pfd, 1U, 0), 0);
    YEW_ASSERT_EQ_I64(write(master, "abc", 3U), 3);
    pfd.revents = 0;
    YEW_ASSERT_EQ_I64(poll(&pfd, 1U, 100), 1);
    YEW_ASSERT((pfd.revents & POLLIN) != 0);
    n = read(slave, bytes, sizeof(bytes));
    YEW_ASSERT_EQ_I64(n, 3);
    YEW_ASSERT_EQ_MEM(bytes, "abc", 3U);
    pfd.revents = 0;
    YEW_ASSERT_EQ_I64(poll(&pfd, 1U, 0), 0);

    YEW_ASSERT_EQ_I64(close(slave), 0);
    YEW_ASSERT_EQ_I64(close(master), 0);
}

void test_tty_restore_blob(void)
{
    static const u8 expected[] =
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
    const u8 *actual;
    size_t len;

    actual = yew_tty_restore_blob(&len);
    YEW_ASSERT_NOT_NULL(actual);
    YEW_ASSERT_EQ_U64(len, sizeof(expected) - 1U);
    YEW_ASSERT_EQ_MEM(actual, expected, sizeof(expected) - 1U);
}

void test_tty_poison_marks_terminal_unusable(void)
{
    Tty t;

    memset(&t, 0, sizeof(t));
    t.rfd = STDIN_FILENO;
    yew_tty_poison(&t);
    YEW_ASSERT(t.poisoned);
    YEW_ASSERT_EQ_I64(t.rfd, -1);
}

void test_tty_poisoned_access_is_bug(void)
{
    int fds[2];
    pid_t child;
    pid_t waited;
    int status;
    char output[1024];
    ssize_t got;

    YEW_ASSERT_EQ_I64(pipe(fds), 0);
    YEW_ASSERT_EQ_I64(fflush(NULL), 0);
    child = fork();
    YEW_ASSERT(child >= 0);
    if (child == 0) {
        Tty t;

        (void)close(fds[0]);
        if (dup2(fds[1], STDERR_FILENO) < 0)
            _exit(126);
        (void)close(fds[1]);
        memset(&t, 0, sizeof(t));
        yew_tty_poison(&t);
        (void)yew_tty_signal_fd(&t);
        _exit(0);
    }
    YEW_ASSERT_EQ_I64(close(fds[1]), 0);
    do {
        got = read(fds[0], output, sizeof(output) - 1U);
    } while (got < 0 && errno == EINTR);
    YEW_ASSERT(got >= 0);
    output[got < 0 ? 0U : (size_t)got] = '\0';
    YEW_ASSERT_EQ_I64(close(fds[0]), 0);
    do {
        waited = waitpid(child, &status, 0);
    } while (waited < 0 && errno == EINTR);
    YEW_ASSERT_EQ_I64(waited, child);
    YEW_ASSERT(WIFEXITED(status));
    YEW_ASSERT_EQ_I64(WEXITSTATUS(status), YEW_EXIT_BUG);
    YEW_ASSERT(strstr(output, "terminal access in --batch: "
                              "yew_tty_signal_fd") != NULL);
}

void test_tty_probe_modern(void)
{
    static const u8 stream[] =
        "\x1b[?7u\x1b[?2026;1$y\x1b[?1;2c";
    const ProbeFixture fixture = {
        stream, sizeof(stream) - 1U, true, 7U, true, true
    };
    Tty t;
    int pipefd[2];

    run_probe_fixture(&fixture, fixture.len, &t, pipefd);
    assert_caps(&t, &fixture);
    YEW_ASSERT_EQ_U64(t.pending.len, 0U);
    tty_fixture_free(&t, pipefd);
}

void test_tty_probe_kitty_only(void)
{
    static const u8 stream[] = "\x1b[?13u\x1b[?1;2c";
    const ProbeFixture fixture = {
        stream, sizeof(stream) - 1U, true, 13U, false, true
    };
    Tty t;
    int pipefd[2];

    run_probe_fixture(&fixture, fixture.len, &t, pipefd);
    assert_caps(&t, &fixture);
    tty_fixture_free(&t, pipefd);
}

void test_tty_probe_sync_only(void)
{
    static const u8 stream[] = "\x1b[?2026;2$y\x1b[?62;4c";
    const ProbeFixture fixture = {
        stream, sizeof(stream) - 1U, false, 0U, true, true
    };
    Tty t;
    int pipefd[2];

    run_probe_fixture(&fixture, fixture.len, &t, pipefd);
    assert_caps(&t, &fixture);
    tty_fixture_free(&t, pipefd);
}

void test_tty_probe_dumb_deadline(void)
{
    Tty t;
    int pipefd[2];

    tty_fixture_init(&t, pipefd, 700);
    YEW_ASSERT(!yew_tty_probe_done(&t));
    YEW_ASSERT_EQ_I64(yew_tty_probe_deadline(&t, 700), 50);
    YEW_ASSERT_EQ_I64(yew_tty_probe_deadline(&t, 749), 1);
    YEW_ASSERT_EQ_I64(yew_tty_probe_deadline(&t, 751), 0);
    yew_tty_probe_tick(&t, 749);
    YEW_ASSERT(!yew_tty_probe_done(&t));
    yew_tty_probe_tick(&t, 750);
    YEW_ASSERT(yew_tty_probe_done(&t));
    YEW_ASSERT(t.caps.probed);
    YEW_ASSERT(!t.caps.kitty_kbd);
    YEW_ASSERT(!t.caps.sync_output);
    YEW_ASSERT(!t.caps.da1_seen);
    tty_fixture_free(&t, pipefd);
}

void test_tty_probe_decrpm_states(void)
{
    static const bool expected[] = {false, true, true, true, false};
    u32 ps;

    for (ps = 0U; ps < YEW_ARRAY_LEN(expected); ps++) {
        char stream[32];
        int n;
        Tty t;
        int pipefd[2];

        n = snprintf(stream, sizeof(stream), "\x1b[?2026;%u$y\x1b[?1c", ps);
        YEW_ASSERT(n > 0 && (size_t)n < sizeof(stream));
        tty_fixture_init(&t, pipefd, 0);
        (void)yew_tty_probe_feed(&t, (const u8 *)stream, (size_t)n);
        YEW_ASSERT(t.caps.sync_output == expected[ps]);
        YEW_ASSERT(t.caps.da1_seen);
        tty_fixture_free(&t, pipefd);
    }
}

void test_tty_probe_chunking(void)
{
    static const u8 modern[] =
        "\x1b[?7u\x1b[?2026;1$y\x1b[?1;2c";
    static const u8 kitty[] = "\x1b[?13u\x1b[?1;2c";
    static const u8 sync[] = "\x1b[?2026;3$y\x1b[?62;4c";
    static const ProbeFixture fixtures[] = {
        {modern, sizeof(modern) - 1U, true, 7U, true, true},
        {kitty, sizeof(kitty) - 1U, true, 13U, false, true},
        {sync, sizeof(sync) - 1U, false, 0U, true, true}
    };
    size_t f;

    for (f = 0U; f < YEW_ARRAY_LEN(fixtures); f++) {
        size_t split;

        for (split = 0U; split <= fixtures[f].len; split++) {
            Tty t;
            int pipefd[2];

            run_probe_fixture(&fixtures[f], split, &t, pipefd);
            assert_caps(&t, &fixtures[f]);
            YEW_ASSERT_EQ_U64(t.pending.len, 0U);
            tty_fixture_free(&t, pipefd);
        }
    }
}

void test_tty_probe_pending_interleaved(void)
{
    static const u8 modern[] =
        "ihello\x1b[?23u\x1b[A!\x1b[?2026;3$y?\x1b[?1;2c\x1b";
    static const u8 kitty[] =
        "ihello\x1b[?23u\x1b[A!?\x1b[?1;2c\x1b";
    static const u8 sync[] =
        "ihello\x1b[A!\x1b[?2026;3$y?\x1b[?1;2c\x1b";
    static const u8 expected[] = "ihello\x1b[A!?\x1b";
    static const struct {
        ProbeFixture probe;
        bool expire;
    } fixtures[] = {
        {{modern, sizeof(modern) - 1U, true, 23U, true, true}, false},
        {{kitty, sizeof(kitty) - 1U, true, 23U, false, true}, false},
        {{sync, sizeof(sync) - 1U, false, 0U, true, true}, false},
        {{expected, sizeof(expected) - 1U, false, 0U, false, false}, true}
    };
    size_t i;

    for (i = 0U; i < YEW_ARRAY_LEN(fixtures); i++) {
        Tty t;
        int pipefd[2];

        tty_fixture_init(&t, pipefd, 100);
        (void)yew_tty_probe_feed(&t, fixtures[i].probe.stream,
                                 fixtures[i].probe.len);
        if (fixtures[i].expire) {
            YEW_ASSERT(!yew_tty_probe_done(&t));
            yew_tty_probe_tick(&t, 150);
        }
        assert_caps(&t, &fixtures[i].probe);
        YEW_ASSERT_EQ_U64(t.pending.len, sizeof(expected) - 1U);
        YEW_ASSERT_EQ_MEM(t.pending.data, expected, sizeof(expected) - 1U);
        tty_fixture_free(&t, pipefd);
    }
}

void test_tty_probe_ambiguous_prefix(void)
{
    static const u8 prefix[] = "\x1b[?";
    static const u8 mismatch[] = "Z";
    static const u8 expected[] = "\x1b[?Z";
    Tty t;
    int pipefd[2];

    tty_fixture_init(&t, pipefd, 0);
    (void)yew_tty_probe_feed(&t, prefix, sizeof(prefix) - 1U);
    YEW_ASSERT_EQ_U64(t.pending.len, 0U);
    (void)yew_tty_probe_feed(&t, mismatch, sizeof(mismatch) - 1U);
    YEW_ASSERT_EQ_U64(t.pending.len, sizeof(expected) - 1U);
    YEW_ASSERT_EQ_MEM(t.pending.data, expected, sizeof(expected) - 1U);
    tty_fixture_free(&t, pipefd);
}

void test_tty_resume_failure_visible(void)
{
    static const u8 cont_note = (u8)'C';
    Tty t;
    int pipefd[2];
    bool cont = false;

    memset(&t, 0, sizeof(t));
    YEW_ASSERT_EQ_I64(pipe(pipefd), 0);
    t.sigpipe[0] = pipefd[0];
    t.sigpipe[1] = -1;
    t.raw = true;
    YEW_ASSERT_EQ_I64(write(pipefd[1], &cont_note, 1U), 1);
    YEW_ASSERT_EQ_I64(close(pipefd[1]), 0);
    yew_tty_drain_signals(&t, NULL, &cont, NULL);
    YEW_ASSERT(cont);
    YEW_ASSERT(!t.raw);
    YEW_ASSERT_EQ_I64(close(pipefd[0]), 0);
}

void test_tty_probe_config(void)
{
    static const TestEnvEntry disabled[] = {{"YEW_TTY_PROBE", "0"}};
    static const TestEnvEntry timeout[] = {
        {"YEW_TTY_PROBE", "1"}, {"YEW_PROBE_TIMEOUT_MS", "173"}
    };
    TtyProbeConfig config;

    set_test_env(NULL, 0U);
    config = yew_tty_probe_read_config(test_getenv);
    YEW_ASSERT(config.enabled);
    YEW_ASSERT_EQ_I64(config.timeout_ms, 50);

    set_test_env(disabled, YEW_ARRAY_LEN(disabled));
    config = yew_tty_probe_read_config(test_getenv);
    YEW_ASSERT(!config.enabled);

    set_test_env(timeout, YEW_ARRAY_LEN(timeout));
    config = yew_tty_probe_read_config(test_getenv);
    YEW_ASSERT(config.enabled);
    YEW_ASSERT_EQ_I64(config.timeout_ms, 173);
}

static void tty_background_fixture_init(Tty *t, int pipefd[2], i64 now_ms)
{
    memset(t, 0, sizeof(*t));
    t->rfd = -1;
    YEW_ASSERT_EQ_I64(pipe(pipefd), 0);
    t->wfd = pipefd[1];
    bytebuf_init(&t->pending);
    set_test_env(NULL, 0U);
    YEW_ASSERT(yew_tty_probe_background_config(t, now_ms, test_getenv));
}

void test_tty_probe_background_query_and_common_replies(void)
{
    static const u8 query[] = "\x1b]11;?\x07";
    static const u8 light_16[] = "\x1b]11;rgb:ffff/ffff/ffff\x07";
    static const u8 dark_8[] = "\x1b]11;rgb:00/00/00\x1b\\";
    static const u8 light_4[] = "\x1b]11;rgb:F/8/0\x07";
    static const struct {
        const u8 *reply;
        size_t len;
        TtyBackground expected;
    } cases[] = {
        {light_16, sizeof(light_16) - 1U,
         YEW_TTY_BACKGROUND_LIGHT},
        {dark_8, sizeof(dark_8) - 1U,
         YEW_TTY_BACKGROUND_DARK},
        {light_4, sizeof(light_4) - 1U,
         YEW_TTY_BACKGROUND_LIGHT}
    };
    size_t i;

    for (i = 0U; i < YEW_ARRAY_LEN(cases); ++i) {
        Tty t;
        int pipefd[2];
        u8 actual_query[sizeof(query) - 1U];
        ssize_t n;
        size_t split;

        tty_background_fixture_init(&t, pipefd, 1000);
        n = read(pipefd[0], actual_query, sizeof(actual_query));
        YEW_ASSERT_EQ_I64(n, (ssize_t)sizeof(actual_query));
        YEW_ASSERT_EQ_MEM(actual_query, query, sizeof(actual_query));
        split = cases[i].len / 2U;
        (void)yew_tty_probe_feed(&t, cases[i].reply, split);
        YEW_ASSERT(!yew_tty_probe_done(&t));
        (void)yew_tty_probe_feed(&t, cases[i].reply + split,
                                 cases[i].len - split);
        YEW_ASSERT(yew_tty_probe_done(&t));
        YEW_ASSERT(yew_tty_probe_background(&t) == cases[i].expected);
        YEW_ASSERT_EQ_U64(t.pending.len, 0U);
        tty_fixture_free(&t, pipefd);
    }
}

void test_tty_probe_background_preserves_unrelated_input(void)
{
    static const u8 stream[] =
        "hello\x1b[?7u\x1b[A\x1b[?1;2c!"
        "\x1b]11;rgb:1010/2020/3030\x07" "z";
    static const u8 expected[] = "hello\x1b[A!z";
    Tty t;
    int pipefd[2];

    tty_background_fixture_init(&t, pipefd, 50);
    (void)yew_tty_probe_feed(&t, stream, sizeof(stream) - 1U);
    YEW_ASSERT(yew_tty_probe_done(&t));
    YEW_ASSERT(t.caps.kitty_kbd);
    YEW_ASSERT_EQ_U64(t.caps.kitty_flags, 7U);
    YEW_ASSERT(t.caps.da1_seen);
    YEW_ASSERT(yew_tty_probe_background(&t) == YEW_TTY_BACKGROUND_DARK);
    YEW_ASSERT_EQ_U64(t.pending.len, sizeof(expected) - 1U);
    YEW_ASSERT_EQ_MEM(t.pending.data, expected, sizeof(expected) - 1U);
    tty_fixture_free(&t, pipefd);
}

void test_tty_probe_background_waits_for_delayed_capability_reply(void)
{
    static const u8 background[] =
        "\x1b]11;rgb:ffff/ffff/ffff\x07";
    static const u8 da1[] = "\x1b[?1;2c";
    Tty t;
    int pipefd[2];

    memset(&t, 0, sizeof(t));
    t.rfd = -1;
    YEW_ASSERT_EQ_I64(pipe(pipefd), 0);
    t.wfd = pipefd[1];
    bytebuf_init(&t.pending);
    set_test_env(NULL, 0U);
    yew_tty_probe_config(&t, 1000, test_getenv);
    YEW_ASSERT(t.capability_await);
    YEW_ASSERT(yew_tty_probe_background_config(&t, 1000, test_getenv));
    (void)yew_tty_probe_feed(&t, background, sizeof(background) - 1U);
    YEW_ASSERT(!yew_tty_probe_done(&t));
    YEW_ASSERT(t.capability_await);
    YEW_ASSERT(yew_tty_probe_background(&t) ==
               YEW_TTY_BACKGROUND_LIGHT);
    YEW_ASSERT_EQ_U64(t.pending.len, 0U);
    (void)yew_tty_probe_feed(&t, da1, sizeof(da1) - 1U);
    YEW_ASSERT(yew_tty_probe_done(&t));
    YEW_ASSERT(!t.capability_await);
    YEW_ASSERT(t.caps.da1_seen);
    YEW_ASSERT_EQ_U64(t.pending.len, 0U);
    tty_fixture_free(&t, pipefd);
}

void test_tty_probe_background_timeout_and_malformed(void)
{
    static const u8 malformed[] = "\x1b]11;rgb:ffff/nope/0000\x07";
    static const TestEnvEntry disabled[] = {{"YEW_TTY_PROBE", "0"}};
    Tty t;
    int pipefd[2];

    tty_background_fixture_init(&t, pipefd, 700);
    (void)yew_tty_probe_feed(&t, malformed, sizeof(malformed) - 1U);
    YEW_ASSERT(!yew_tty_probe_done(&t));
    yew_tty_probe_tick(&t, 750);
    YEW_ASSERT(yew_tty_probe_done(&t));
    YEW_ASSERT(yew_tty_probe_background(&t) ==
               YEW_TTY_BACKGROUND_UNKNOWN);
    YEW_ASSERT_EQ_U64(t.pending.len, sizeof(malformed) - 1U);
    YEW_ASSERT_EQ_MEM(t.pending.data, malformed, sizeof(malformed) - 1U);
    tty_fixture_free(&t, pipefd);

    memset(&t, 0, sizeof(t));
    t.rfd = -1;
    YEW_ASSERT_EQ_I64(pipe(pipefd), 0);
    t.wfd = pipefd[1];
    bytebuf_init(&t.pending);
    set_test_env(disabled, YEW_ARRAY_LEN(disabled));
    YEW_ASSERT(!yew_tty_probe_background_config(&t, 0, test_getenv));
    YEW_ASSERT(yew_tty_probe_background(&t) ==
               YEW_TTY_BACKGROUND_UNKNOWN);
    tty_fixture_free(&t, pipefd);
}

void test_tty_truecolor(void)
{
    static const TestEnvEntry forced_off[] = {
        {"YEW_TRUECOLOR", "0"}, {"COLORTERM", "truecolor"}
    };
    static const TestEnvEntry forced_on[] = {{"YEW_TRUECOLOR", "1"}};
    static const TestEnvEntry colorterm_truecolor[] = {
        {"COLORTERM", "truecolor"}
    };
    static const TestEnvEntry colorterm_24bit[] = {{"COLORTERM", "24bit"}};
    static const TestEnvEntry term_direct[] = {{"TERM", "xterm-direct"}};
    static const TestEnvEntry term_truecolor[] = {
        {"TERM", "screen-truecolor"}
    };
    static const TestEnvEntry term_kitty[] = {{"TERM", "xterm-kitty"}};
    static const TestEnvEntry term_foot[] = {{"TERM", "foot-extra"}};
    static const TestEnvEntry term_wezterm[] = {{"TERM", "wezterm"}};
    static const TestEnvEntry term_alacritty[] = {{"TERM", "alacritty"}};
    static const TestEnvEntry term_ghostty[] = {{"TERM", "ghostty"}};
    static const TestEnvEntry program_iterm[] = {
        {"TERM_PROGRAM", "iTerm.app"}
    };
    static const TestEnvEntry program_vscode[] = {
        {"TERM_PROGRAM", "vscode"}
    };
    static const TestEnvEntry vte_old[] = {{"VTE_VERSION", "3599"}};
    static const TestEnvEntry vte_new[] = {{"VTE_VERSION", "3600"}};
    static const TestEnvEntry konsole[] = {{"KONSOLE_VERSION", "230801"}};
    static const TestEnvEntry unknown[] = {{"TERM", "vt100"}};
    static const struct {
        const TestEnvEntry *entries;
        size_t len;
        bool expected;
    } cases[] = {
        {NULL, 0U, false},
        {forced_off, YEW_ARRAY_LEN(forced_off), false},
        {forced_on, YEW_ARRAY_LEN(forced_on), true},
        {colorterm_truecolor, YEW_ARRAY_LEN(colorterm_truecolor), true},
        {colorterm_24bit, YEW_ARRAY_LEN(colorterm_24bit), true},
        {term_direct, YEW_ARRAY_LEN(term_direct), true},
        {term_truecolor, YEW_ARRAY_LEN(term_truecolor), true},
        {term_kitty, YEW_ARRAY_LEN(term_kitty), true},
        {term_foot, YEW_ARRAY_LEN(term_foot), true},
        {term_wezterm, YEW_ARRAY_LEN(term_wezterm), true},
        {term_alacritty, YEW_ARRAY_LEN(term_alacritty), true},
        {term_ghostty, YEW_ARRAY_LEN(term_ghostty), true},
        {program_iterm, YEW_ARRAY_LEN(program_iterm), true},
        {program_vscode, YEW_ARRAY_LEN(program_vscode), true},
        {vte_old, YEW_ARRAY_LEN(vte_old), false},
        {vte_new, YEW_ARRAY_LEN(vte_new), true},
        {konsole, YEW_ARRAY_LEN(konsole), true},
        {unknown, YEW_ARRAY_LEN(unknown), false}
    };
    size_t i;

    for (i = 0U; i < YEW_ARRAY_LEN(cases); i++) {
        set_test_env(cases[i].entries, cases[i].len);
        YEW_ASSERT(yew_tty_detect_truecolor(test_getenv) == cases[i].expected);
    }
}
