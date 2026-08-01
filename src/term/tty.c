#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "term/tty.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "util/log.h"

enum {
    SAG_TTY_PROBE_IDLE,
    SAG_TTY_PROBE_AWAIT,
    SAG_TTY_PROBE_DONE
};

static const i64 SAG_TTY_PROBE_DEFAULT_MS = 50;
static const char SAG_TTY_PROBE_QUERY[] =
    "\x1b[?u"
    "\x1b[?2026$p"
    "\x1b[c";
static const char SAG_TTY_ALT_ON[] = "\x1b" "7\x1b[?1049h";
static const char SAG_TTY_ALT_OFF[] = "\x1b[?1049l\x1b" "8";

static struct termios g_saved;
static struct termios g_rawios;
static volatile sig_atomic_t g_raw;
static volatile sig_atomic_t g_resume_raw;
static volatile sig_atomic_t g_winch_pending;
static volatile sig_atomic_t g_cont_pending;
static volatile sig_atomic_t g_chld_pending;
static Tty *g_owner;
static int g_tfd = -1;
static int g_wfd = -1;
static int g_sigpipe_w = -1;
static bool g_atexit_armed;

static const char SAG_TTY_RESTORE_BLOB[] =
    "\x1b[<u"
    "\x1b[?2004l"
    "\x1b[?1002l"
    "\x1b[?1006l"
    "\x1b[?1004l"
    "\x1b[?2026l"
    "\x1b[0m"
    "\x1b[?1049l"
    "\x1b[?25h";

/* BEGIN ASYNC-SIGNAL-SAFE — allowlist: write tcsetattr sigaction signal
 * raise kill. */
void sag_tty_restore(void)
{
    if (!g_raw)
        return;
    g_raw = 0;
    (void)!write(g_wfd, SAG_TTY_RESTORE_BLOB,
                 sizeof(SAG_TTY_RESTORE_BLOB) - 1U);
    (void)tcsetattr(g_tfd, TCSAFLUSH, &g_saved);
}

static void sag_tty_fatal(int sig)
{
    static const char note[] =
        "sagitta: fatal signal, terminal restored\r\n";

    sag_tty_restore();
    (void)!write(STDERR_FILENO, note, sizeof(note) - 1U);
    (void)signal(sig, SIG_DFL);
    (void)raise(sig);
}

static void sag_tty_signote(int sig)
{
    int saved_errno = errno;
    u8 byte = sig == SIGWINCH ? (u8)'W' :
              sig == SIGCONT ? (u8)'C' : (u8)'H';

    if (sig == SIGWINCH)
        g_winch_pending = 1;
    else if (sig == SIGCONT)
        g_cont_pending = 1;
    else
        g_chld_pending = 1;
    (void)!write(g_sigpipe_w, &byte, 1U);
    errno = saved_errno;
}

static void sag_tty_tstp(int sig)
{
    (void)sig;
    sag_tty_restore();
    (void)signal(SIGTSTP, SIG_DFL);
    (void)raise(SIGTSTP);
}

static void sag_tty_cont(int sig)
{
    if (g_resume_raw && tcsetattr(g_tfd, TCSANOW, &g_rawios) == 0)
        g_raw = 1;
    (void)signal(SIGTSTP, sag_tty_tstp);
    sag_tty_signote(sig);
}
/* END ASYNC-SIGNAL-SAFE */

static bool sag_tty_write_all(int fd, const void *data, size_t len)
{
    const u8 *bytes = data;

    while (len != 0U) {
        ssize_t written = write(fd, bytes, len);

        if (written > 0) {
            bytes += (size_t)written;
            len -= (size_t)written;
        } else if (written < 0 && errno == EINTR) {
            continue;
        } else {
            return false;
        }
    }
    return true;
}

typedef struct {
    int sig;
    void (*handler)(int);
    struct sigaction saved;
} SagTtySignal;

static SagTtySignal g_signals[] = {
    {.sig = SIGSEGV, .handler = sag_tty_fatal},
    {.sig = SIGBUS, .handler = sag_tty_fatal},
    {.sig = SIGABRT, .handler = sag_tty_fatal},
    {.sig = SIGTERM, .handler = sag_tty_fatal},
    {.sig = SIGWINCH, .handler = sag_tty_signote},
    {.sig = SIGCHLD, .handler = sag_tty_signote},
    {.sig = SIGCONT, .handler = sag_tty_cont},
    {.sig = SIGTSTP, .handler = sag_tty_tstp}
};
static bool g_handlers_installed;

static bool sag_tty_handlers_install(void)
{
    struct sigaction action;
    size_t i;

    if (g_handlers_installed)
        return true;
    for (i = 0U; i < SAG_ARRAY_LEN(g_signals); i++) {
        (void)memset(&action, 0, sizeof(action));
        action.sa_handler = g_signals[i].handler;
        (void)sigemptyset(&action.sa_mask);
        if (sigaction(g_signals[i].sig, &action,
                      &g_signals[i].saved) != 0) {
            while (i != 0U) {
                i--;
                (void)sigaction(g_signals[i].sig,
                                &g_signals[i].saved, NULL);
            }
            return false;
        }
    }
    g_handlers_installed = true;
    return true;
}

static void sag_tty_handlers_restore(void)
{
    size_t i;

    if (!g_handlers_installed)
        return;
    for (i = 0U; i < SAG_ARRAY_LEN(g_signals); i++)
        (void)sigaction(g_signals[i].sig, &g_signals[i].saved, NULL);
    g_handlers_installed = false;
}

static bool sag_tty_pipe_open(int fds[2])
{
#if defined(__linux__)
    return pipe2(fds, O_NONBLOCK | O_CLOEXEC) == 0;
#else
    int i;

    if (pipe(fds) != 0)
        return false;
    for (i = 0; i < 2; ++i) {
        int flags = fcntl(fds[i], F_GETFL);
        int fdflags = fcntl(fds[i], F_GETFD);

        if (flags < 0 || fdflags < 0 ||
            fcntl(fds[i], F_SETFL, flags | O_NONBLOCK) != 0 ||
            fcntl(fds[i], F_SETFD, fdflags | FD_CLOEXEC) != 0) {
            (void)close(fds[0]);
            (void)close(fds[1]);
            fds[0] = -1;
            fds[1] = -1;
            return false;
        }
    }
    return true;
#endif
}

static bool sag_tty_not_tty(void)
{
    static const char message[] =
        "sagitta: error: stdin is not a terminal (--batch lands in Sprint 37)\n";

    (void)!write(STDERR_FILENO, message, sizeof(message) - 1U);
    errno = ENOTTY;
    return false;
}

static const char *sag_tty_getenv(const char *name)
{
    return getenv(name);
}

bool sag_tty_open(Tty *t)
{
    if (t == NULL || g_owner != NULL) {
        errno = t == NULL ? EINVAL : EBUSY;
        return false;
    }
    (void)memset(t, 0, sizeof(*t));
    t->rfd = STDIN_FILENO;
    t->wfd = STDOUT_FILENO;
    t->sigpipe[0] = -1;
    t->sigpipe[1] = -1;
    bytebuf_init(&t->pending);

    if (!isatty(t->rfd) || !isatty(t->wfd))
        return sag_tty_not_tty();
    if (tcgetattr(t->rfd, &t->saved) != 0)
        return false;
    if (!sag_tty_pipe_open(t->sigpipe))
        return false;
    t->caps.truecolor = sag_tty_detect_truecolor(sag_tty_getenv);
    (void)sag_tty_winsize(t);
    g_owner = t;
    return true;
}

void sag_tty_rawios(struct termios *io)
{
    const tcflag_t input_clear = IXON | ICRNL | INLCR | IGNCR | BRKINT |
                                 IGNBRK | PARMRK | ISTRIP | INPCK;
    const tcflag_t local_clear = ECHO | ECHONL | ICANON | ISIG | IEXTEN;

    io->c_iflag &= ~input_clear;
    io->c_oflag &= ~OPOST;
    io->c_lflag &= ~local_clear;
    io->c_cflag &= ~(CSIZE | PARENB);
    io->c_cflag |= CS8;
    io->c_cc[VMIN] = 0;
    io->c_cc[VTIME] = 0;
}

static bool sag_tty_rawios_equal(const struct termios *left,
                                 const struct termios *right)
{
    const tcflag_t input = IXON | ICRNL | INLCR | IGNCR | BRKINT | IGNBRK |
                           PARMRK | ISTRIP | INPCK;
    const tcflag_t local = ECHO | ECHONL | ICANON | ISIG | IEXTEN;
    const tcflag_t control = CSIZE | PARENB;

    return (left->c_iflag & input) == (right->c_iflag & input) &&
           (left->c_oflag & OPOST) == (right->c_oflag & OPOST) &&
           (left->c_lflag & local) == (right->c_lflag & local) &&
           (left->c_cflag & control) == (right->c_cflag & control) &&
           left->c_cc[VMIN] == right->c_cc[VMIN] &&
           left->c_cc[VTIME] == right->c_cc[VTIME];
}

static bool sag_tty_broken(void)
{
    static const char message[] =
        "sagitta: error: terminal is broken: raw mode was only partially applied\n";

    sag_tty_restore();
    (void)!write(STDERR_FILENO, message, sizeof(message) - 1U);
    errno = EIO;
    return false;
}

bool sag_tty_raw(Tty *t)
{
    struct termios actual;

    if (t == NULL || t != g_owner) {
        errno = EINVAL;
        return false;
    }
    if (t->raw && g_raw)
        return true;
    if (!g_atexit_armed) {
        if (atexit(sag_tty_restore) != 0)
            return false;
        g_atexit_armed = true;
    }
    if (!sag_tty_handlers_install())
        return false;

    g_saved = t->saved;
    g_rawios = t->saved;
    sag_tty_rawios(&g_rawios);
    g_tfd = t->rfd;
    g_wfd = t->wfd;
    g_sigpipe_w = t->sigpipe[1];
    g_resume_raw = 1;
    g_winch_pending = 0;
    g_cont_pending = 0;
    g_chld_pending = 0;
    sag_bug_set_prehook(sag_tty_restore);

    g_raw = 1;
    if (tcsetattr(t->rfd, TCSANOW, &g_rawios) != 0) {
        g_raw = 0;
        g_resume_raw = 0;
        return false;
    }
    if (tcgetattr(t->rfd, &actual) != 0 ||
        !sag_tty_rawios_equal(&actual, &g_rawios))
        return sag_tty_broken();
    t->raw = true;
    return true;
}

void sag_tty_close(Tty *t)
{
    if (t == NULL)
        return;
    if (g_owner == t) {
        g_resume_raw = 0;
        sag_tty_restore();
        sag_bug_set_prehook(NULL);
        sag_tty_handlers_restore();
        g_owner = NULL;
        g_tfd = -1;
        g_wfd = -1;
        g_sigpipe_w = -1;
        g_winch_pending = 0;
        g_cont_pending = 0;
        g_chld_pending = 0;
    }
    if (t->sigpipe[0] >= 0)
        (void)close(t->sigpipe[0]);
    if (t->sigpipe[1] >= 0)
        (void)close(t->sigpipe[1]);
    t->sigpipe[0] = -1;
    t->sigpipe[1] = -1;
    t->raw = false;
    t->alt = false;
    bytebuf_free(&t->pending);
}

bool sag_tty_winsize(Tty *t)
{
    struct winsize size;

    if (t == NULL || ioctl(t->wfd, TIOCGWINSZ, &size) != 0 ||
        size.ws_row == 0U || size.ws_col == 0U)
        return false;
    t->rows = (int)size.ws_row;
    t->cols = (int)size.ws_col;
    return true;
}

void sag_tty_altscreen(Tty *t, bool on)
{
    if (t == NULL || t != g_owner || !t->raw || t->alt == on)
        return;
    if (on) {
        if (!sag_tty_write_all(t->wfd, SAG_TTY_ALT_ON,
                               sizeof(SAG_TTY_ALT_ON) - 1U))
            return;
    } else if (!sag_tty_write_all(t->wfd, SAG_TTY_ALT_OFF,
                                  sizeof(SAG_TTY_ALT_OFF) - 1U)) {
        return;
    }
    t->alt = on;
}

int sag_tty_signal_fd(const Tty *t)
{
    return t == NULL ? -1 : t->sigpipe[0];
}

void sag_tty_drain_signals(Tty *t, bool *winch, bool *cont, bool *chld)
{
    u8 bytes[64];
    sigset_t blocked;
    sigset_t saved;
    bool masked;

    if (winch != NULL)
        *winch = false;
    if (cont != NULL)
        *cont = false;
    if (chld != NULL)
        *chld = false;
    if (t == NULL || t->sigpipe[0] < 0)
        return;

    (void)sigemptyset(&blocked);
    (void)sigaddset(&blocked, SIGWINCH);
    (void)sigaddset(&blocked, SIGCONT);
    (void)sigaddset(&blocked, SIGCHLD);
    masked = sigprocmask(SIG_BLOCK, &blocked, &saved) == 0;
    if (g_winch_pending && winch != NULL)
        *winch = true;
    if (g_cont_pending) {
        if (cont != NULL)
            *cont = true;
        t->raw = g_raw != 0;
        t->alt = false;
    }
    if (g_chld_pending && chld != NULL)
        *chld = true;
    g_winch_pending = 0;
    g_cont_pending = 0;
    g_chld_pending = 0;

    for (;;) {
        ssize_t count = read(t->sigpipe[0], bytes, sizeof(bytes));
        size_t i;

        if (count > 0) {
            for (i = 0; i < (size_t)count; ++i) {
                if (bytes[i] == (u8)'W' && winch != NULL)
                    *winch = true;
                else if (bytes[i] == (u8)'C') {
                    if (cont != NULL)
                        *cont = true;
                    t->raw = true;
                    t->alt = false;
                } else if (bytes[i] == (u8)'H' && chld != NULL) {
                    *chld = true;
                }
            }
        } else if (count < 0 && errno == EINTR) {
            continue;
        } else {
            break;
        }
    }
    if (masked)
        (void)sigprocmask(SIG_SETMASK, &saved, NULL);
}

void sag_tty_suspend(Tty *t)
{
    (void)t;
    sag_tty_restore();
    (void)kill(0, SIGTSTP);
}

static bool sag_tty_streq(const char *left, const char *right)
{
    return left != NULL && strcmp(left, right) == 0;
}

static bool sag_tty_starts(const char *value, const char *prefix)
{
    size_t len;

    if (value == NULL)
        return false;
    len = strlen(prefix);
    return strncmp(value, prefix, len) == 0;
}

static bool sag_tty_contains(const char *value, const char *needle)
{
    return value != NULL && strstr(value, needle) != NULL;
}

static bool sag_tty_decimal_at_least(const char *value, unsigned long floor)
{
    unsigned long parsed = 0;
    const unsigned char *p = (const unsigned char *)value;

    if (value == NULL || *value == '\0')
        return false;
    while (*p != '\0') {
        unsigned digit;

        if (*p < (unsigned char)'0' || *p > (unsigned char)'9')
            return false;
        digit = (unsigned)(*p - (unsigned char)'0');
        if (parsed > (ULONG_MAX - digit) / 10UL)
            parsed = ULONG_MAX;
        else
            parsed = parsed * 10UL + digit;
        ++p;
    }
    return parsed >= floor;
}

bool sag_tty_detect_truecolor(const char *(*getv)(const char *))
{
    static const char *const term_prefixes[] = {
        "xterm-kitty", "kitty", "foot", "wezterm", "alacritty", "ghostty"
    };
    static const char *const programs[] = {
        "iTerm.app", "WezTerm", "ghostty", "vscode"
    };
    const char *value;
    size_t i;

    if (getv == NULL)
        return false;
    value = getv("SAG_TRUECOLOR");
    if (sag_tty_streq(value, "0"))
        return false;
    if (sag_tty_streq(value, "1"))
        return true;
    value = getv("COLORTERM");
    if (sag_tty_streq(value, "truecolor") || sag_tty_streq(value, "24bit"))
        return true;
    value = getv("TERM");
    if (sag_tty_contains(value, "direct") ||
        sag_tty_contains(value, "truecolor"))
        return true;
    for (i = 0; i < SAG_ARRAY_LEN(term_prefixes); ++i) {
        if (sag_tty_starts(value, term_prefixes[i]))
            return true;
    }
    value = getv("TERM_PROGRAM");
    for (i = 0; i < SAG_ARRAY_LEN(programs); ++i) {
        if (sag_tty_streq(value, programs[i]))
            return true;
    }
    if (sag_tty_decimal_at_least(getv("VTE_VERSION"), 3600UL))
        return true;
    value = getv("KONSOLE_VERSION");
    return value != NULL;
}

TtyProbeConfig sag_tty_probe_read_config(
    const char *(*getv)(const char *))
{
    TtyProbeConfig config;
    const char *value;

    config.enabled = true;
    config.timeout_ms = SAG_TTY_PROBE_DEFAULT_MS;
    if (getv == NULL)
        return config;
    value = getv("SAG_TTY_PROBE");
    if (sag_tty_streq(value, "0"))
        config.enabled = false;
    value = getv("SAG_PROBE_TIMEOUT_MS");
    if (value != NULL && *value != '\0') {
        i64 parsed = 0;
        const unsigned char *p = (const unsigned char *)value;
        bool valid = true;

        while (*p != '\0') {
            unsigned digit;

            if (*p < (unsigned char)'0' || *p > (unsigned char)'9') {
                valid = false;
                break;
            }
            digit = (unsigned)(*p - (unsigned char)'0');
            if (parsed > (INT64_MAX - (i64)digit) / 10) {
                valid = false;
                break;
            }
            parsed = parsed * 10 + (i64)digit;
            ++p;
        }
        if (valid && parsed > 0)
            config.timeout_ms = parsed;
    }
    return config;
}

static void sag_tty_probe_flush(Tty *t)
{
    if (t->probe_prefix_len != 0U) {
        bytebuf_append(&t->pending, t->probe_prefix, t->probe_prefix_len);
        t->probe_prefix_len = 0U;
    }
}

static bool sag_tty_prefix_fixed(const u8 *bytes, size_t len,
                                 const char *fixed)
{
    size_t fixed_len = strlen(fixed);
    size_t shared = len < fixed_len ? len : fixed_len;

    return memcmp(bytes, fixed, shared) == 0;
}

static bool sag_tty_probe_kitty(const u8 *bytes, size_t len, bool *complete,
                                u32 *flags)
{
    size_t i;
    u32 value = 0;

    *complete = false;
    if (len <= 3U)
        return sag_tty_prefix_fixed(bytes, len, "\x1b[?");
    if (memcmp(bytes, "\x1b[?", 3U) != 0)
        return false;
    for (i = 3U; i < len; ++i) {
        unsigned digit;

        if (bytes[i] == (u8)'u' && i + 1U == len && i > 3U) {
            *complete = true;
            *flags = value;
            return true;
        }
        if (bytes[i] < (u8)'0' || bytes[i] > (u8)'9')
            return false;
        digit = (unsigned)(bytes[i] - (u8)'0');
        if (value > (UINT32_MAX - digit) / 10U)
            return false;
        value = value * 10U + (u32)digit;
    }
    return true;
}

static bool sag_tty_probe_da1(const u8 *bytes, size_t len, bool *complete)
{
    size_t i;
    bool digit_seen = false;

    *complete = false;
    if (len <= 3U)
        return sag_tty_prefix_fixed(bytes, len, "\x1b[?");
    if (memcmp(bytes, "\x1b[?", 3U) != 0)
        return false;
    for (i = 3U; i < len; ++i) {
        if (bytes[i] >= (u8)'0' && bytes[i] <= (u8)'9') {
            digit_seen = true;
        } else if (bytes[i] == (u8)';' && digit_seen) {
            digit_seen = false;
        } else if (bytes[i] == (u8)'c' && digit_seen && i + 1U == len) {
            *complete = true;
            return true;
        } else {
            return false;
        }
    }
    return true;
}

static bool sag_tty_probe_sync(const u8 *bytes, size_t len, bool *complete,
                               bool *supported)
{
    static const char prefix[] = "\x1b[?2026;";
    size_t prefix_len = sizeof(prefix) - 1U;
    u8 status;

    *complete = false;
    if (len <= prefix_len)
        return sag_tty_prefix_fixed(bytes, len, prefix);
    if (memcmp(bytes, prefix, prefix_len) != 0)
        return false;
    status = bytes[prefix_len];
    if (status < (u8)'0' || status > (u8)'4')
        return false;
    if (len == prefix_len + 1U)
        return true;
    if (bytes[prefix_len + 1U] != (u8)'$')
        return false;
    if (len == prefix_len + 2U)
        return true;
    if (bytes[prefix_len + 2U] != (u8)'y' ||
        len != prefix_len + 3U)
        return false;
    *complete = true;
    *supported = status == (u8)'1' || status == (u8)'2' ||
                 status == (u8)'3';
    return true;
}

static bool sag_tty_probe_candidate(Tty *t)
{
    bool kitty_complete;
    bool da1_complete;
    bool sync_complete;
    bool sync_supported = false;
    u32 kitty_flags = 0;
    bool kitty = sag_tty_probe_kitty(t->probe_prefix, t->probe_prefix_len,
                                    &kitty_complete, &kitty_flags);
    bool da1 = sag_tty_probe_da1(t->probe_prefix, t->probe_prefix_len,
                                &da1_complete);
    bool sync = sag_tty_probe_sync(t->probe_prefix, t->probe_prefix_len,
                                  &sync_complete, &sync_supported);

    if (kitty_complete) {
        t->caps.kitty_kbd = true;
        t->caps.kitty_flags = kitty_flags;
        t->probe_prefix_len = 0U;
        return true;
    }
    if (sync_complete) {
        t->caps.sync_output = sync_supported;
        t->probe_prefix_len = 0U;
        return true;
    }
    if (da1_complete) {
        t->caps.da1_seen = true;
        t->caps.probed = true;
        t->pstate = SAG_TTY_PROBE_DONE;
        t->pdeadline = 0;
        t->probe_prefix_len = 0U;
        return true;
    }
    return kitty || da1 || sync;
}

static void sag_tty_probe_byte(Tty *t, u8 byte)
{
    if (t->probe_prefix_len == 0U) {
        if (byte == 0x1bU)
            t->probe_prefix[t->probe_prefix_len++] = byte;
        else
            bytebuf_push_u8(&t->pending, byte);
        return;
    }
    if (t->probe_prefix_len == sizeof(t->probe_prefix)) {
        sag_tty_probe_flush(t);
        sag_tty_probe_byte(t, byte);
        return;
    }
    t->probe_prefix[t->probe_prefix_len++] = byte;
    if (sag_tty_probe_candidate(t))
        return;

    if (byte == 0x1bU) {
        bytebuf_append(&t->pending, t->probe_prefix,
                       t->probe_prefix_len - 1U);
        t->probe_prefix[0] = byte;
        t->probe_prefix_len = 1U;
    } else {
        sag_tty_probe_flush(t);
    }
}

void sag_tty_probe_config(Tty *t, i64 now_ms,
                          const char *(*getv)(const char *))
{
    TtyProbeConfig config;

    if (t == NULL)
        return;
    config = sag_tty_probe_read_config(getv);
    t->caps.probed = false;
    t->caps.truecolor = sag_tty_detect_truecolor(getv);
    t->caps.kitty_kbd = false;
    t->caps.kitty_flags = 0;
    t->caps.sync_output = false;
    t->caps.da1_seen = false;
    t->probe_prefix_len = 0U;
    if (!config.enabled) {
        t->pstate = SAG_TTY_PROBE_DONE;
        t->pdeadline = 0;
        t->caps.probed = true;
        return;
    }
    t->pstate = SAG_TTY_PROBE_AWAIT;
    if (now_ms > INT64_MAX - config.timeout_ms)
        t->pdeadline = INT64_MAX;
    else
        t->pdeadline = now_ms + config.timeout_ms;
    (void)sag_tty_write_all(t->wfd, SAG_TTY_PROBE_QUERY,
                            sizeof(SAG_TTY_PROBE_QUERY) - 1U);
}

void sag_tty_probe_start(Tty *t, i64 now_ms)
{
    sag_tty_probe_config(t, now_ms, sag_tty_getenv);
}

size_t sag_tty_probe_feed(Tty *t, const u8 *b, size_t n)
{
    size_t i;

    if (t == NULL || (b == NULL && n != 0U))
        return 0U;
    if (t->pstate != SAG_TTY_PROBE_AWAIT) {
        bytebuf_append(&t->pending, b, n);
        return n;
    }
    for (i = 0U; i < n; ++i) {
        if (t->pstate == SAG_TTY_PROBE_AWAIT)
            sag_tty_probe_byte(t, b[i]);
        else
            bytebuf_push_u8(&t->pending, b[i]);
    }
    return n;
}

void sag_tty_probe_tick(Tty *t, i64 now_ms)
{
    if (t == NULL || t->pstate != SAG_TTY_PROBE_AWAIT ||
        now_ms < t->pdeadline)
        return;
    sag_tty_probe_flush(t);
    t->pstate = SAG_TTY_PROBE_DONE;
    t->pdeadline = 0;
    t->caps.probed = true;
}

bool sag_tty_probe_done(const Tty *t)
{
    return t != NULL && t->pstate == SAG_TTY_PROBE_DONE;
}

i64 sag_tty_probe_deadline(const Tty *t, i64 now_ms)
{
    if (t == NULL || t->pstate != SAG_TTY_PROBE_AWAIT)
        return -1;
    if (now_ms >= t->pdeadline)
        return 0;
    return t->pdeadline - now_ms;
}

const u8 *sag_tty_restore_blob(size_t *len)
{
    if (len != NULL)
        *len = sizeof(SAG_TTY_RESTORE_BLOB) - 1U;
    return (const u8 *)SAG_TTY_RESTORE_BLOB;
}
