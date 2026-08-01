#include "harness.h"

#include <stdio.h>
#include <string.h>
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
    SAG_ASSERT_EQ_I64(pipe(pipefd), 0);
    t->wfd = pipefd[1];
    bytebuf_init(&t->pending);
    set_test_env(NULL, 0U);
    sag_tty_probe_config(t, now_ms, test_getenv);
}

static void tty_fixture_free(Tty *t, int pipefd[2])
{
    bytebuf_free(&t->pending);
    SAG_ASSERT_EQ_I64(close(pipefd[0]), 0);
    SAG_ASSERT_EQ_I64(close(pipefd[1]), 0);
}

static void assert_caps(const Tty *t, const ProbeFixture *fixture)
{
    SAG_ASSERT(t->caps.probed);
    SAG_ASSERT(t->caps.kitty_kbd == fixture->kitty_kbd);
    SAG_ASSERT_EQ_U64(t->caps.kitty_flags, fixture->kitty_flags);
    SAG_ASSERT(t->caps.sync_output == fixture->sync_output);
    SAG_ASSERT(t->caps.da1_seen == fixture->da1_seen);
}

static void run_probe_fixture(const ProbeFixture *fixture, size_t split,
                              Tty *t, int pipefd[2])
{
    tty_fixture_init(t, pipefd, 1000);
    (void)sag_tty_probe_feed(t, fixture->stream, split);
    (void)sag_tty_probe_feed(t, fixture->stream + split,
                             fixture->len - split);
}

void test_tty_raw_input_flags(void)
{
    struct termios io;

    memset(&io, 0, sizeof(io));
    io.c_iflag = IXON | ICRNL | INLCR | IGNCR | BRKINT | IGNBRK | PARMRK |
                 ISTRIP | INPCK;
    sag_tty_rawios(&io);
    SAG_ASSERT((io.c_iflag & IXON) == 0);
    SAG_ASSERT((io.c_iflag & ICRNL) == 0);
    SAG_ASSERT((io.c_iflag & INLCR) == 0);
    SAG_ASSERT((io.c_iflag & IGNCR) == 0);
    SAG_ASSERT((io.c_iflag & BRKINT) == 0);
    SAG_ASSERT((io.c_iflag & IGNBRK) == 0);
    SAG_ASSERT((io.c_iflag & PARMRK) == 0);
    SAG_ASSERT((io.c_iflag & ISTRIP) == 0);
    SAG_ASSERT((io.c_iflag & INPCK) == 0);
}

void test_tty_raw_output_flags(void)
{
    struct termios io;

    memset(&io, 0, sizeof(io));
    io.c_oflag = OPOST;
    sag_tty_rawios(&io);
    SAG_ASSERT((io.c_oflag & OPOST) == 0);
}

void test_tty_raw_local_flags(void)
{
    struct termios io;

    memset(&io, 0, sizeof(io));
    io.c_lflag = ECHO | ECHONL | ICANON | ISIG | IEXTEN;
    sag_tty_rawios(&io);
    SAG_ASSERT((io.c_lflag & ECHO) == 0);
    SAG_ASSERT((io.c_lflag & ECHONL) == 0);
    SAG_ASSERT((io.c_lflag & ICANON) == 0);
    SAG_ASSERT((io.c_lflag & ISIG) == 0);
    SAG_ASSERT((io.c_lflag & IEXTEN) == 0);
}

void test_tty_raw_control_flags(void)
{
    struct termios io;
    struct termios once;

    memset(&io, 0, sizeof(io));
    io.c_cflag = CSIZE | PARENB | CLOCAL;
    io.c_cc[VMIN] = 9;
    io.c_cc[VTIME] = 9;
    sag_tty_rawios(&io);
    SAG_ASSERT((io.c_cflag & CSIZE) == CS8);
    SAG_ASSERT((io.c_cflag & PARENB) == 0);
    SAG_ASSERT((io.c_cflag & CLOCAL) != 0);
    SAG_ASSERT_EQ_U64(io.c_cc[VMIN], 0U);
    SAG_ASSERT_EQ_U64(io.c_cc[VTIME], 0U);
    once = io;
    sag_tty_rawios(&io);
    SAG_ASSERT_EQ_MEM(&io, &once, sizeof(io));
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
        "\x1b[?1049l"
        "\x1b[?25h";
    const u8 *actual;
    size_t len;

    actual = sag_tty_restore_blob(&len);
    SAG_ASSERT_NOT_NULL(actual);
    SAG_ASSERT_EQ_U64(len, sizeof(expected) - 1U);
    SAG_ASSERT_EQ_MEM(actual, expected, sizeof(expected) - 1U);
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
    SAG_ASSERT_EQ_U64(t.pending.len, 0U);
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
    SAG_ASSERT(!sag_tty_probe_done(&t));
    SAG_ASSERT_EQ_I64(sag_tty_probe_deadline(&t, 700), 50);
    SAG_ASSERT_EQ_I64(sag_tty_probe_deadline(&t, 749), 1);
    SAG_ASSERT_EQ_I64(sag_tty_probe_deadline(&t, 751), 0);
    sag_tty_probe_tick(&t, 749);
    SAG_ASSERT(!sag_tty_probe_done(&t));
    sag_tty_probe_tick(&t, 750);
    SAG_ASSERT(sag_tty_probe_done(&t));
    SAG_ASSERT(t.caps.probed);
    SAG_ASSERT(!t.caps.kitty_kbd);
    SAG_ASSERT(!t.caps.sync_output);
    SAG_ASSERT(!t.caps.da1_seen);
    tty_fixture_free(&t, pipefd);
}

void test_tty_probe_decrpm_states(void)
{
    static const bool expected[] = {false, true, true, true, false};
    u32 ps;

    for (ps = 0U; ps < SAG_ARRAY_LEN(expected); ps++) {
        char stream[32];
        int n;
        Tty t;
        int pipefd[2];

        n = snprintf(stream, sizeof(stream), "\x1b[?2026;%u$y\x1b[?1c", ps);
        SAG_ASSERT(n > 0 && (size_t)n < sizeof(stream));
        tty_fixture_init(&t, pipefd, 0);
        (void)sag_tty_probe_feed(&t, (const u8 *)stream, (size_t)n);
        SAG_ASSERT(t.caps.sync_output == expected[ps]);
        SAG_ASSERT(t.caps.da1_seen);
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

    for (f = 0U; f < SAG_ARRAY_LEN(fixtures); f++) {
        size_t split;

        for (split = 0U; split <= fixtures[f].len; split++) {
            Tty t;
            int pipefd[2];

            run_probe_fixture(&fixtures[f], split, &t, pipefd);
            assert_caps(&t, &fixtures[f]);
            SAG_ASSERT_EQ_U64(t.pending.len, 0U);
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

    for (i = 0U; i < SAG_ARRAY_LEN(fixtures); i++) {
        Tty t;
        int pipefd[2];

        tty_fixture_init(&t, pipefd, 100);
        (void)sag_tty_probe_feed(&t, fixtures[i].probe.stream,
                                 fixtures[i].probe.len);
        if (fixtures[i].expire) {
            SAG_ASSERT(!sag_tty_probe_done(&t));
            sag_tty_probe_tick(&t, 150);
        }
        assert_caps(&t, &fixtures[i].probe);
        SAG_ASSERT_EQ_U64(t.pending.len, sizeof(expected) - 1U);
        SAG_ASSERT_EQ_MEM(t.pending.data, expected, sizeof(expected) - 1U);
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
    (void)sag_tty_probe_feed(&t, prefix, sizeof(prefix) - 1U);
    SAG_ASSERT_EQ_U64(t.pending.len, 0U);
    (void)sag_tty_probe_feed(&t, mismatch, sizeof(mismatch) - 1U);
    SAG_ASSERT_EQ_U64(t.pending.len, sizeof(expected) - 1U);
    SAG_ASSERT_EQ_MEM(t.pending.data, expected, sizeof(expected) - 1U);
    tty_fixture_free(&t, pipefd);
}

void test_tty_resume_failure_visible(void)
{
    static const u8 cont_note = (u8)'C';
    Tty t;
    int pipefd[2];
    bool cont = false;

    memset(&t, 0, sizeof(t));
    SAG_ASSERT_EQ_I64(pipe(pipefd), 0);
    t.sigpipe[0] = pipefd[0];
    t.sigpipe[1] = -1;
    t.raw = true;
    SAG_ASSERT_EQ_I64(write(pipefd[1], &cont_note, 1U), 1);
    SAG_ASSERT_EQ_I64(close(pipefd[1]), 0);
    sag_tty_drain_signals(&t, NULL, &cont, NULL);
    SAG_ASSERT(cont);
    SAG_ASSERT(!t.raw);
    SAG_ASSERT_EQ_I64(close(pipefd[0]), 0);
}

void test_tty_probe_config(void)
{
    static const TestEnvEntry disabled[] = {{"SAG_TTY_PROBE", "0"}};
    static const TestEnvEntry timeout[] = {
        {"SAG_TTY_PROBE", "1"}, {"SAG_PROBE_TIMEOUT_MS", "173"}
    };
    TtyProbeConfig config;

    set_test_env(NULL, 0U);
    config = sag_tty_probe_read_config(test_getenv);
    SAG_ASSERT(config.enabled);
    SAG_ASSERT_EQ_I64(config.timeout_ms, 50);

    set_test_env(disabled, SAG_ARRAY_LEN(disabled));
    config = sag_tty_probe_read_config(test_getenv);
    SAG_ASSERT(!config.enabled);

    set_test_env(timeout, SAG_ARRAY_LEN(timeout));
    config = sag_tty_probe_read_config(test_getenv);
    SAG_ASSERT(config.enabled);
    SAG_ASSERT_EQ_I64(config.timeout_ms, 173);
}

void test_tty_truecolor(void)
{
    static const TestEnvEntry forced_off[] = {
        {"SAG_TRUECOLOR", "0"}, {"COLORTERM", "truecolor"}
    };
    static const TestEnvEntry forced_on[] = {{"SAG_TRUECOLOR", "1"}};
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
        {forced_off, SAG_ARRAY_LEN(forced_off), false},
        {forced_on, SAG_ARRAY_LEN(forced_on), true},
        {colorterm_truecolor, SAG_ARRAY_LEN(colorterm_truecolor), true},
        {colorterm_24bit, SAG_ARRAY_LEN(colorterm_24bit), true},
        {term_direct, SAG_ARRAY_LEN(term_direct), true},
        {term_truecolor, SAG_ARRAY_LEN(term_truecolor), true},
        {term_kitty, SAG_ARRAY_LEN(term_kitty), true},
        {term_foot, SAG_ARRAY_LEN(term_foot), true},
        {term_wezterm, SAG_ARRAY_LEN(term_wezterm), true},
        {term_alacritty, SAG_ARRAY_LEN(term_alacritty), true},
        {term_ghostty, SAG_ARRAY_LEN(term_ghostty), true},
        {program_iterm, SAG_ARRAY_LEN(program_iterm), true},
        {program_vscode, SAG_ARRAY_LEN(program_vscode), true},
        {vte_old, SAG_ARRAY_LEN(vte_old), false},
        {vte_new, SAG_ARRAY_LEN(vte_new), true},
        {konsole, SAG_ARRAY_LEN(konsole), true},
        {unknown, SAG_ARRAY_LEN(unknown), false}
    };
    size_t i;

    for (i = 0U; i < SAG_ARRAY_LEN(cases); i++) {
        set_test_env(cases[i].entries, cases[i].len);
        SAG_ASSERT(sag_tty_detect_truecolor(test_getenv) == cases[i].expected);
    }
}
