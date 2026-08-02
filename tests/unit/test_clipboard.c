#define _POSIX_C_SOURCE 200809L

#include "harness.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "term/render.h"
#include "text/clipboard.h"
#include "text/register.h"
#include "util/buf.h"

typedef struct ClipEnv {
    const char *name;
    char *old;
    bool had;
} ClipEnv;

typedef struct ClipFixture {
    char dir[PATH_MAX];
    char fake[PATH_MAX];
    char output[PATH_MAX];
    char input[PATH_MAX];
    char pidlog[PATH_MAX];
} ClipFixture;

static void clip_env_save(ClipEnv *save, const char *name)
{
    const char *value = getenv(name);

    save->name = name;
    save->had = value != NULL;
    save->old = value != NULL ? strdup(value) : NULL;
    if (value != NULL && save->old == NULL)
        SAG_BUG("clipboard test environment allocation failed");
}

static void clip_env_restore(ClipEnv *save)
{
    if (save->had)
        SAG_ASSERT_EQ_I64(setenv(save->name, save->old, 1), 0);
    else
        SAG_ASSERT_EQ_I64(unsetenv(save->name), 0);
    free(save->old);
}

static void clip_clear_environment(void)
{
    SAG_ASSERT_EQ_I64(unsetenv("SAG_CLIPBOARD"), 0);
    SAG_ASSERT_EQ_I64(unsetenv("SAG_OSC52"), 0);
    SAG_ASSERT_EQ_I64(unsetenv("WAYLAND_DISPLAY"), 0);
    SAG_ASSERT_EQ_I64(unsetenv("DISPLAY"), 0);
    SAG_ASSERT_EQ_I64(unsetenv("SSH_TTY"), 0);
    SAG_ASSERT_EQ_I64(unsetenv("SSH_CONNECTION"), 0);
}

static void clip_path(char *out, size_t cap, const char *dir,
                      const char *base)
{
    int n = snprintf(out, cap, "%s/%s", dir, base);

    if (n < 0 || (size_t)n >= cap)
        SAG_BUG("clipboard test path overflow");
}

static void clip_fixture_init(ClipFixture *f)
{
    const char *program = sag_test_program_path();
    const char *slash = strrchr(program, '/');
    size_t dir_len = slash != NULL ? (size_t)(slash - program) : 1U;
    char template[] = "/tmp/sagitta-clip-XXXXXX";
    char candidate[PATH_MAX];

    if (slash != NULL) {
        if (dir_len + sizeof("/fakeclip") > sizeof(candidate))
            SAG_BUG("clipboard fake path overflow");
        (void)memcpy(candidate, program, dir_len);
        (void)memcpy(candidate + dir_len, "/fakeclip", sizeof("/fakeclip"));
    } else {
        (void)snprintf(candidate, sizeof(candidate), "./fakeclip");
    }
    if (candidate[0] == '/') {
        (void)snprintf(f->fake, sizeof(f->fake), "%s", candidate);
    } else {
        char cwd[PATH_MAX];
        int n;

        if (getcwd(cwd, sizeof(cwd)) == NULL)
            SAG_BUG("clipboard test getcwd failed: %s", strerror(errno));
        n = snprintf(f->fake, sizeof(f->fake), "%s/%s", cwd, candidate);
        if (n < 0 || (size_t)n >= sizeof(f->fake))
            SAG_BUG("clipboard absolute fake path overflow");
    }
    if (access(f->fake, X_OK) != 0)
        SAG_BUG("clipboard fake tool missing: %s", strerror(errno));
    if (mkdtemp(template) == NULL)
        SAG_BUG("clipboard test mkdtemp failed: %s", strerror(errno));
    (void)snprintf(f->dir, sizeof(f->dir), "%s", template);
    clip_path(f->output, sizeof(f->output), f->dir, "out.bin");
    clip_path(f->input, sizeof(f->input), f->dir, "in.bin");
    clip_path(f->pidlog, sizeof(f->pidlog), f->dir, "pids.txt");
}

static void clip_unlink_name(const ClipFixture *f, const char *name)
{
    char path[PATH_MAX];

    clip_path(path, sizeof(path), f->dir, name);
    if (unlink(path) != 0 && errno != ENOENT)
        SAG_BUG("clipboard test unlink failed: %s", strerror(errno));
}

static void clip_fixture_free(ClipFixture *f)
{
    clip_unlink_name(f, "wl-copy");
    clip_unlink_name(f, "wl-paste");
    clip_unlink_name(f, "xclip");
    clip_unlink_name(f, "xsel");
    clip_unlink_name(f, "pbcopy");
    clip_unlink_name(f, "pbpaste");
    clip_unlink_name(f, "out.bin");
    clip_unlink_name(f, "out.bin.argv");
    clip_unlink_name(f, "in.bin");
    clip_unlink_name(f, "pids.txt");
    if (rmdir(f->dir) != 0)
        SAG_BUG("clipboard test rmdir failed: %s", strerror(errno));
}

static void clip_link(const ClipFixture *f, const char *name)
{
    char path[PATH_MAX];

    clip_path(path, sizeof(path), f->dir, name);
    SAG_ASSERT_EQ_I64(symlink(f->fake, path), 0);
}

static void clip_write_file(const char *path, const u8 *bytes, size_t len)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    size_t off = 0U;

    if (fd < 0)
        SAG_BUG("clipboard test open for write failed: %s", strerror(errno));
    while (off < len) {
        ssize_t n = write(fd, bytes + off, len - off);

        if (n > 0)
            off += (size_t)n;
        else if (n < 0 && errno == EINTR)
            continue;
        else
            SAG_BUG("clipboard test write failed: %s", strerror(errno));
    }
    (void)close(fd);
}

static bool clip_read_file(const char *path, Bytebuf *out)
{
    int fd = open(path, O_RDONLY);

    out->len = 0U;
    if (fd < 0)
        return false;
    for (;;) {
        u8 chunk[4096];
        ssize_t n = read(fd, chunk, sizeof(chunk));

        if (n > 0)
            bytebuf_append(out, chunk, (size_t)n);
        else if (n == 0)
            break;
        else if (errno != EINTR) {
            (void)close(fd);
            return false;
        }
    }
    (void)close(fd);
    return true;
}

static bool clip_wait_file(const char *path, size_t min_len, Bytebuf *out)
{
    struct timespec pause = {0, 1000000L};
    u32 i;

    for (i = 0U; i < 2000U; i++) {
        if (clip_read_file(path, out) && out->len >= min_len)
            return true;
        (void)nanosleep(&pause, NULL);
    }
    return false;
}

static i64 clip_test_now_ms(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        SAG_BUG("clipboard test monotonic clock failed: %s", strerror(errno));
    return (i64)ts.tv_sec * 1000 + (i64)(ts.tv_nsec / 1000000L);
}

static u64 clip_test_now_ns(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        SAG_BUG("clipboard test monotonic clock failed: %s", strerror(errno));
    return (u64)ts.tv_sec * UINT64_C(1000000000) + (u64)ts.tv_nsec;
}

static void clip_value(RegVal *v, const u8 *bytes, size_t len)
{
    sag_regval_init(v);
    v->type = SAG_REG_CHARWISE;
    bytebuf_append(&v->bytes, bytes, len);
}

static void clip_set_custom(const ClipFixture *f, const char *write_mode,
                            bool with_read)
{
    char value[PATH_MAX * 4U];
    int n;

    if (with_read) {
        n = snprintf(value, sizeof(value), "cmd:%s %s %s alpha beta|%s %s read",
                     f->fake, f->output, write_mode, f->fake, f->input);
    } else {
        n = snprintf(value, sizeof(value), "cmd:%s %s %s",
                     f->fake, f->output, write_mode);
    }
    if (n < 0 || (size_t)n >= sizeof(value))
        SAG_BUG("clipboard custom command overflow");
    SAG_ASSERT_EQ_I64(setenv("SAG_CLIPBOARD", value, 1), 0);
    sag_clip_reset();
}

static void clip_pump_until_idle(void)
{
    u32 i;

    for (i = 0U; i < 2000U && sag_clip_busy(); i++) {
        int fd = sag_clip_write_fd();

        if (fd >= 0) {
            struct pollfd pfd = {fd, POLLOUT, 0};
            (void)poll(&pfd, 1U, 1);
        }
        sag_clip_pump(clip_test_now_ms());
    }
    SAG_ASSERT(!sag_clip_busy());
}

void test_clipboard_detection_matrix(void)
{
    static const char *const names[] = {
        "SAG_CLIPBOARD", "SAG_OSC52", "WAYLAND_DISPLAY", "DISPLAY",
        "SSH_TTY", "SSH_CONNECTION", "PATH"
    };
    ClipEnv saved[SAG_ARRAY_LEN(names)];
    ClipFixture f;
    size_t i;

    for (i = 0U; i < SAG_ARRAY_LEN(names); i++)
        clip_env_save(&saved[i], names[i]);
    clip_fixture_init(&f);
    clip_clear_environment();
    SAG_ASSERT_EQ_I64(setenv("PATH", f.dir, 1), 0);
    sag_clip_reset();
    SAG_ASSERT_EQ_U64(sag_clip_detect(), SAG_CLIP_NONE);

    clip_link(&f, "wl-copy");
    SAG_ASSERT_EQ_I64(setenv("WAYLAND_DISPLAY", "wayland-1", 1), 0);
    sag_clip_reset();
    SAG_ASSERT_EQ_U64(sag_clip_detect(), SAG_CLIP_WL);

    SAG_ASSERT_EQ_I64(unsetenv("WAYLAND_DISPLAY"), 0);
    clip_link(&f, "xclip");
    SAG_ASSERT_EQ_I64(setenv("DISPLAY", ":1", 1), 0);
    sag_clip_reset();
    SAG_ASSERT_EQ_U64(sag_clip_detect(), SAG_CLIP_XCLIP);

    clip_link(&f, "xsel");
    sag_clip_reset();
    SAG_ASSERT_EQ_U64(sag_clip_detect(), SAG_CLIP_XCLIP);
    clip_unlink_name(&f, "xclip");
    sag_clip_reset();
    SAG_ASSERT_EQ_U64(sag_clip_detect(), SAG_CLIP_XSEL);

    SAG_ASSERT_EQ_I64(setenv("WAYLAND_DISPLAY", "wayland-1", 1), 0);
    sag_clip_reset();
    SAG_ASSERT_EQ_U64(sag_clip_detect(), SAG_CLIP_WL);
    clip_unlink_name(&f, "wl-copy");
    sag_clip_reset();
    SAG_ASSERT_EQ_U64(sag_clip_detect(), SAG_CLIP_XSEL);

    SAG_ASSERT_EQ_I64(setenv("SSH_TTY", "/dev/pts/9", 1), 0);
    sag_clip_reset();
    SAG_ASSERT_EQ_U64(sag_clip_detect(), SAG_CLIP_NONE);
    SAG_ASSERT_EQ_U64(sag_clip_detect_read(), SAG_CLIP_XSEL);
    SAG_ASSERT_EQ_I64(unsetenv("SSH_TTY"), 0);
    SAG_ASSERT_EQ_I64(setenv("SSH_CONNECTION", "1 2 3 4", 1), 0);
    sag_clip_reset();
    SAG_ASSERT_EQ_U64(sag_clip_detect(), SAG_CLIP_NONE);
    SAG_ASSERT_EQ_U64(sag_clip_detect_read(), SAG_CLIP_XSEL);

    SAG_ASSERT_EQ_I64(unsetenv("SSH_CONNECTION"), 0);
    SAG_ASSERT_EQ_I64(unsetenv("DISPLAY"), 0);
    sag_clip_reset();
    SAG_ASSERT_EQ_U64(sag_clip_detect_read(), SAG_CLIP_NONE);
    SAG_ASSERT_EQ_I64(setenv("SAG_CLIPBOARD", "auto", 1), 0);
    sag_clip_reset();
    SAG_ASSERT_EQ_U64(sag_clip_detect(), SAG_CLIP_NONE);
    SAG_ASSERT_EQ_U64(sag_clip_detect_read(), SAG_CLIP_NONE);
    sag_clip_shutdown();
    clip_fixture_free(&f);
    for (i = SAG_ARRAY_LEN(names); i != 0U; i--)
        clip_env_restore(&saved[i - 1U]);
}

void test_clipboard_override_matrix(void)
{
    static const struct {
        const char *value;
        SagClipBackend write_backend;
        SagClipBackend read_backend;
    } cases[] = {
        {"none", SAG_CLIP_NONE, SAG_CLIP_NONE},
        {"osc52", SAG_CLIP_OSC52, SAG_CLIP_NONE},
        {"wl", SAG_CLIP_WL, SAG_CLIP_WL},
        {"xclip", SAG_CLIP_XCLIP, SAG_CLIP_XCLIP},
        {"xsel", SAG_CLIP_XSEL, SAG_CLIP_XSEL},
        {"pb", SAG_CLIP_PB, SAG_CLIP_PB},
        {"cmd:/bin/false", SAG_CLIP_CUSTOM, SAG_CLIP_NONE},
        {"cmd:/bin/false|/bin/false", SAG_CLIP_CUSTOM, SAG_CLIP_CUSTOM}
    };
    ClipEnv clipboard;
    size_t i;

    clip_env_save(&clipboard, "SAG_CLIPBOARD");
    for (i = 0U; i < SAG_ARRAY_LEN(cases); i++) {
        SAG_ASSERT_EQ_I64(setenv("SAG_CLIPBOARD", cases[i].value, 1), 0);
        sag_clip_reset();
        SAG_ASSERT_EQ_U64(sag_clip_detect(), cases[i].write_backend);
        SAG_ASSERT_EQ_U64(sag_clip_detect_read(), cases[i].read_backend);
    }
    sag_clip_shutdown();
    clip_env_restore(&clipboard);
}

void test_clipboard_write_defers_backend_detection(void)
{
    ClipEnv clipboard;
    RegVal value;
    Bytebuf terminal;

    clip_env_save(&clipboard, "SAG_CLIPBOARD");
    SAG_ASSERT_EQ_I64(setenv("SAG_CLIPBOARD", "none", 1), 0);
    sag_clip_reset();
    clip_value(&value, (const u8 *)"queued", 6U);
    bytebuf_init(&terminal);
    sag_test_capture_log();

    SAG_ASSERT(sag_clip_write(&value, '+'));
    SAG_ASSERT(sag_clip_pending());
    SAG_ASSERT(!sag_test_log_contains(SAG_LOG_WARN, "no writable"));
    sag_clip_after_render(&terminal, clip_test_now_ms());
    SAG_ASSERT(!sag_clip_pending());
    SAG_ASSERT(sag_test_log_contains(SAG_LOG_WARN, "no writable"));

    bytebuf_free(&terminal);
    sag_regval_free(&value);
    sag_clip_shutdown();
    clip_env_restore(&clipboard);
}

void test_clipboard_custom_write_coalesces_binary(void)
{
    static const u8 first[] = {(u8)'o', (u8)'l', (u8)'d'};
    static const u8 latest[] = {0U, 0xffU, (u8)'n', (u8)'e', (u8)'w'};
    ClipEnv clipboard;
    ClipFixture f;
    RegVal a;
    RegVal b;
    Bytebuf got;
    Bytebuf argv;
    char argv_path[PATH_MAX];
    Bytebuf terminal;

    clip_env_save(&clipboard, "SAG_CLIPBOARD");
    clip_fixture_init(&f);
    clip_set_custom(&f, "write", true);
    clip_value(&a, first, sizeof(first));
    clip_value(&b, latest, sizeof(latest));
    bytebuf_init(&got);
    bytebuf_init(&argv);
    bytebuf_init(&terminal);
    SAG_ASSERT(sag_clip_write(&a, '+'));
    SAG_ASSERT(sag_clip_pending());
    SAG_ASSERT(sag_clip_write(&b, '*'));
    sag_clip_after_render(&terminal, clip_test_now_ms());
    clip_pump_until_idle();
    SAG_ASSERT_EQ_U64(terminal.len, 0U);
    SAG_ASSERT(clip_wait_file(f.output, sizeof(latest), &got));
    SAG_ASSERT_EQ_U64(got.len, sizeof(latest));
    SAG_ASSERT_EQ_MEM(got.data, latest, sizeof(latest));
    clip_path(argv_path, sizeof(argv_path), f.dir, "out.bin.argv");
    SAG_ASSERT(clip_wait_file(argv_path, 1U, &argv));
    bytebuf_push_u8(&argv, 0U);
    SAG_ASSERT(strstr((const char *)argv.data, "write\nalpha\nbeta\n") !=
               NULL);
    SAG_ASSERT(!sag_clip_pending());
    sag_regval_free(&b);
    sag_regval_free(&a);
    bytebuf_free(&terminal);
    bytebuf_free(&argv);
    bytebuf_free(&got);
    sag_clip_shutdown();
    clip_fixture_free(&f);
    clip_env_restore(&clipboard);
}

void test_clipboard_custom_read_binary_and_cap(void)
{
    static const u8 payload[] = {0U, 1U, 2U, 0xffU, (u8)'x'};
    ClipEnv clipboard;
    ClipFixture f;
    RegVal out;

    clip_env_save(&clipboard, "SAG_CLIPBOARD");
    clip_fixture_init(&f);
    clip_write_file(f.input, payload, sizeof(payload));
    clip_set_custom(&f, "write", true);
    sag_regval_init(&out);
    SAG_ASSERT(sag_clip_read(&out, '+'));
    SAG_ASSERT_EQ_U64(out.type, SAG_REG_CHARWISE);
    SAG_ASSERT_EQ_U64(out.bytes.len, sizeof(payload));
    SAG_ASSERT_EQ_MEM(out.bytes.data, payload, sizeof(payload));
    sag_clip_set_read_max(sizeof(payload) - 1U);
    sag_test_capture_log();
    SAG_ASSERT(!sag_clip_read(&out, '*'));
    SAG_ASSERT(sag_test_log_contains(SAG_LOG_WARN, "exceeds"));
    SAG_ASSERT_EQ_U64(out.bytes.len, sizeof(payload));
    sag_regval_free(&out);
    sag_clip_shutdown();
    clip_fixture_free(&f);
    clip_env_restore(&clipboard);
}

void test_clipboard_reader_reaped_before_pipe_eof(void)
{
    static const u8 payload[] = {(u8)'h', 0U, (u8)'i'};
    ClipEnv clipboard;
    ClipFixture f;
    RegVal out;
    char command[PATH_MAX * 4U];
    int n;

    clip_env_save(&clipboard, "SAG_CLIPBOARD");
    clip_fixture_init(&f);
    clip_write_file(f.input, payload, sizeof(payload));
    n = snprintf(command, sizeof(command),
                 "cmd:%s %s write|%s %s read-hold",
                 f.fake, f.output, f.fake, f.input);
    if (n < 0 || (size_t)n >= sizeof(command))
        SAG_BUG("clipboard held reader command overflow");
    SAG_ASSERT_EQ_I64(setenv("SAG_CLIPBOARD", command, 1), 0);
    sag_clip_reset();
    sag_regval_init(&out);

    SAG_ASSERT(sag_clip_read(&out, '+'));
    SAG_ASSERT_EQ_U64(out.bytes.len, sizeof(payload));
    SAG_ASSERT_EQ_MEM(out.bytes.data, payload, sizeof(payload));

    sag_regval_free(&out);
    sag_clip_shutdown();
    clip_fixture_free(&f);
    clip_env_restore(&clipboard);
}

void test_clipboard_read_failures_are_reported(void)
{
    static const u8 keep[] = {(u8)'k', 0U, (u8)'p'};
    ClipEnv clipboard;
    ClipEnv timeout;
    ClipFixture f;
    RegVal out;
    char command[PATH_MAX * 4U];
    int n;

    clip_env_save(&clipboard, "SAG_CLIPBOARD");
    clip_env_save(&timeout, "SAG_CLIPBOARD_TIMEOUT_MS");
    clip_fixture_init(&f);
    clip_value(&out, keep, sizeof(keep));

    SAG_ASSERT_EQ_I64(setenv("SAG_CLIPBOARD", "none", 1), 0);
    sag_clip_reset();
    sag_test_capture_log();
    SAG_ASSERT(!sag_clip_read(&out, '+'));
    SAG_ASSERT(sag_test_log_contains(SAG_LOG_WARN, "no readable"));
    SAG_ASSERT_EQ_MEM(out.bytes.data, keep, sizeof(keep));

    n = snprintf(command, sizeof(command), "cmd:%s %s write|%s %s exit",
                 f.fake, f.output, f.fake, f.input);
    if (n < 0 || (size_t)n >= sizeof(command))
        SAG_BUG("clipboard exit reader command overflow");
    SAG_ASSERT_EQ_I64(setenv("SAG_CLIPBOARD", command, 1), 0);
    sag_clip_reset();
    sag_test_capture_log();
    SAG_ASSERT(!sag_clip_read(&out, '+'));
    SAG_ASSERT(sag_test_log_contains(SAG_LOG_WARN, "status 17"));
    SAG_ASSERT_EQ_MEM(out.bytes.data, keep, sizeof(keep));

    n = snprintf(command, sizeof(command), "cmd:%s %s write|%s %s hang",
                 f.fake, f.output, f.fake, f.input);
    if (n < 0 || (size_t)n >= sizeof(command))
        SAG_BUG("clipboard hanging reader command overflow");
    SAG_ASSERT_EQ_I64(setenv("SAG_CLIPBOARD", command, 1), 0);
    SAG_ASSERT_EQ_I64(setenv("SAG_CLIPBOARD_TIMEOUT_MS", "10", 1), 0);
    sag_clip_reset();
    sag_test_capture_log();
    SAG_ASSERT(!sag_clip_read(&out, '+'));
    SAG_ASSERT(sag_test_log_contains(SAG_LOG_WARN, "timed out"));
    SAG_ASSERT_EQ_MEM(out.bytes.data, keep, sizeof(keep));

    sag_regval_free(&out);
    sag_clip_shutdown();
    clip_fixture_free(&f);
    clip_env_restore(&timeout);
    clip_env_restore(&clipboard);
}

void test_clipboard_osc52_flushes_after_esu(void)
{
    static const char prefix[] = "frame\033[?2026l";
    static const u8 payload[] = {(u8)'f', (u8)'o', (u8)'o'};
    static const char suffix[] = "\033]52;c;Zm9v\033\\";
    ClipEnv clipboard;
    ClipEnv osc52;
    RegVal value;
    Bytebuf terminal;

    clip_env_save(&clipboard, "SAG_CLIPBOARD");
    clip_env_save(&osc52, "SAG_OSC52");
    SAG_ASSERT_EQ_I64(setenv("SAG_CLIPBOARD", "osc52", 1), 0);
    SAG_ASSERT_EQ_I64(setenv("SAG_OSC52", "plain", 1), 0);
    sag_clip_reset();
    sag_term_oob_clear();
    clip_value(&value, payload, sizeof(payload));
    bytebuf_init(&terminal);
    bytebuf_append(&terminal, prefix, sizeof(prefix) - 1U);
    SAG_ASSERT(sag_clip_write(&value, '+'));
    sag_clip_after_render(&terminal, clip_test_now_ms());
    SAG_ASSERT_EQ_U64(sag_term_oob_pending(), 0U);
    SAG_ASSERT_EQ_U64(terminal.len,
                      sizeof(prefix) - 1U + sizeof(suffix) - 1U);
    SAG_ASSERT_EQ_MEM(terminal.data, prefix, sizeof(prefix) - 1U);
    SAG_ASSERT_EQ_MEM(terminal.data + sizeof(prefix) - 1U,
                      suffix, sizeof(suffix) - 1U);
    bytebuf_free(&terminal);
    sag_regval_free(&value);
    sag_term_oob_clear();
    sag_clip_shutdown();
    clip_env_restore(&osc52);
    clip_env_restore(&clipboard);
}

static u32 clip_parse_pids(const Bytebuf *log, pid_t *out, u32 max)
{
    u32 count = 0U;
    size_t at = 0U;

    while (at < log->len && count < max) {
        long value = 0;
        bool any = false;

        while (at < log->len && log->data[at] >= '0' &&
               log->data[at] <= '9') {
            value = value * 10 + (long)(log->data[at] - '0');
            at++;
            any = true;
        }
        while (at < log->len && log->data[at] != '\n')
            at++;
        if (at < log->len)
            at++;
        if (any)
            out[count++] = (pid_t)value;
    }
    return count;
}

static bool clip_wait_pids_gone(const pid_t *pids, u32 count)
{
    struct timespec pause = {0, 1000000L};
    u32 attempt;

    for (attempt = 0U; attempt < 2000U; attempt++) {
        bool any_live = false;
        u32 i;

        for (i = 0U; i < count; i++) {
            errno = 0;
            if (kill(pids[i], 0) == 0 || errno != ESRCH) {
                any_live = true;
                break;
            }
        }
        if (!any_live)
            return true;
        (void)nanosleep(&pause, NULL);
    }
    return false;
}

void test_clipboard_nonexit_100_writes_are_nonblocking(void)
{
    ClipEnv clipboard;
    ClipFixture f;
    RegVal value;
    Bytebuf log;
    Bytebuf terminal;
    char command[PATH_MAX * 4U];
    pid_t pids[100];
    u64 slowest = 0U;
    u64 budget_ns;
    u32 count;
    u32 i;
    int n;

    clip_env_save(&clipboard, "SAG_CLIPBOARD");
    clip_fixture_init(&f);
    n = snprintf(command, sizeof(command), "cmd:%s %s stay %s",
                 f.fake, f.output, f.pidlog);
    if (n < 0 || (size_t)n >= sizeof(command))
        SAG_BUG("clipboard stay command overflow");
    SAG_ASSERT_EQ_I64(setenv("SAG_CLIPBOARD", command, 1), 0);
    sag_clip_reset();
    clip_value(&value, (const u8 *)"x", 1U);
    bytebuf_init(&log);
    bytebuf_init(&terminal);
    /* Preserve the production budget while allowing instrumentation overhead. */
    budget_ns = getenv("SAG_TEST_INSTRUMENTED") != NULL ?
                    UINT64_C(100000000) : UINT64_C(2000000);
    for (i = 0U; i < 100U; i++) {
        u64 start = clip_test_now_ns();
        u64 elapsed;

        SAG_ASSERT(sag_clip_write(&value, '+'));
        sag_clip_after_render(&terminal, clip_test_now_ms());
        elapsed = clip_test_now_ns() - start;
        if (elapsed > slowest)
            slowest = elapsed;
        clip_pump_until_idle();
    }
    SAG_ASSERT(slowest < budget_ns);
    count = 0U;
    for (i = 0U; i < 2000U && count < 100U; i++) {
        struct timespec pause = {0, 1000000L};

        if (clip_read_file(f.pidlog, &log))
            count = clip_parse_pids(&log, pids, SAG_ARRAY_LEN(pids));
        if (count < 100U)
            (void)nanosleep(&pause, NULL);
    }
    SAG_ASSERT_EQ_U64(count, 100U);
    for (i = 0U; i < count; i++) {
        int status;

        errno = 0;
        SAG_ASSERT_EQ_I64(waitpid(pids[i], &status, WNOHANG), -1);
        SAG_ASSERT_EQ_I64(errno, ECHILD);
        SAG_ASSERT_EQ_I64(kill(pids[i], SIGTERM), 0);
    }
    SAG_ASSERT(clip_wait_pids_gone(pids, count));
    for (i = 0U; i < 200U && sag_clip_owned_children() != 0U; i++) {
        struct timespec pause = {0, 1000000L};
        sag_clip_reap();
        (void)nanosleep(&pause, NULL);
    }
    SAG_ASSERT_EQ_U64(sag_clip_owned_children(), 0U);
    bytebuf_free(&terminal);
    bytebuf_free(&log);
    sag_regval_free(&value);
    sag_clip_shutdown();
    clip_fixture_free(&f);
    clip_env_restore(&clipboard);
}

void test_clipboard_epipe_demotes_failed_backend(void)
{
    ClipEnv clipboard;
    ClipFixture f;
    RegVal value;
    Bytebuf terminal;
    u32 i;

    clip_env_save(&clipboard, "SAG_CLIPBOARD");
    clip_fixture_init(&f);
    clip_set_custom(&f, "exit", false);
    sag_test_capture_log();
    sag_regval_init(&value);
    bytebuf_reserve(&value.bytes, 1024U * 1024U);
    (void)memset(value.bytes.data, 'x', 1024U * 1024U);
    value.bytes.len = 1024U * 1024U;
    bytebuf_init(&terminal);
    SAG_ASSERT(sag_clip_write(&value, '+'));
    sag_clip_after_render(&terminal, clip_test_now_ms());
    for (i = 0U; i < 2000U && sag_clip_busy(); i++) {
        struct pollfd pfd = {sag_clip_write_fd(), POLLOUT, 0};
        (void)poll(&pfd, 1U, 1);
        sag_clip_pump(clip_test_now_ms());
    }
    SAG_ASSERT(!sag_clip_busy());
    SAG_ASSERT(!sag_clip_pending());
    SAG_ASSERT_EQ_U64(sag_clip_detect(), SAG_CLIP_NONE);
    SAG_ASSERT(sag_test_log_contains(SAG_LOG_WARN, "demoting backend"));
    bytebuf_free(&terminal);
    sag_regval_free(&value);
    sag_clip_shutdown();
    clip_fixture_free(&f);
    clip_env_restore(&clipboard);
}

void test_clipboard_exec_failure_demotes_small_write(void)
{
    ClipEnv clipboard;
    RegVal value;
    Bytebuf terminal;

    clip_env_save(&clipboard, "SAG_CLIPBOARD");
    SAG_ASSERT_EQ_I64(
        setenv("SAG_CLIPBOARD",
               "cmd:/definitely/not/a/sagitta-clipboard-tool", 1), 0);
    sag_clip_reset();
    clip_value(&value, (const u8 *)"x", 1U);
    bytebuf_init(&terminal);
    sag_test_capture_log();
    SAG_ASSERT(sag_clip_write(&value, '+'));
    sag_clip_after_render(&terminal, clip_test_now_ms());
    clip_pump_until_idle();
    SAG_ASSERT_EQ_U64(sag_clip_detect(), SAG_CLIP_NONE);
    SAG_ASSERT(sag_test_log_contains(SAG_LOG_WARN, "exec failed"));
    SAG_ASSERT(!sag_clip_pending());
    bytebuf_free(&terminal);
    sag_regval_free(&value);
    sag_clip_shutdown();
    clip_env_restore(&clipboard);
}

void test_clipboard_osc52_over_limit_falls_back_to_subprocess(void)
{
    static const char *const names[] = {
        "SAG_CLIPBOARD", "SAG_OSC52", "SAG_OSC52_MAX",
        "SAG_FAKECLIP_OUTPUT", "WAYLAND_DISPLAY", "SSH_TTY", "PATH"
    };
    ClipEnv saved[SAG_ARRAY_LEN(names)];
    ClipFixture f;
    RegVal value;
    Bytebuf terminal;
    Bytebuf got;
    size_t i;

    for (i = 0U; i < SAG_ARRAY_LEN(names); i++)
        clip_env_save(&saved[i], names[i]);
    clip_fixture_init(&f);
    clip_link(&f, "wl-copy");
    SAG_ASSERT_EQ_I64(setenv("SAG_CLIPBOARD", "osc52", 1), 0);
    SAG_ASSERT_EQ_I64(setenv("SAG_OSC52", "plain", 1), 0);
    SAG_ASSERT_EQ_I64(setenv("SAG_OSC52_MAX", "1", 1), 0);
    SAG_ASSERT_EQ_I64(setenv("SAG_FAKECLIP_OUTPUT", f.output, 1), 0);
    SAG_ASSERT_EQ_I64(setenv("WAYLAND_DISPLAY", "wayland-1", 1), 0);
    SAG_ASSERT_EQ_I64(setenv("PATH", f.dir, 1), 0);
    sag_clip_reset();
    clip_value(&value, (const u8 *)"fallback", 8U);
    bytebuf_init(&terminal);
    bytebuf_init(&got);
    SAG_ASSERT(sag_clip_write(&value, '+'));
    sag_clip_after_render(&terminal, clip_test_now_ms());
    clip_pump_until_idle();
    SAG_ASSERT_EQ_U64(terminal.len, 0U);
    SAG_ASSERT(clip_wait_file(f.output, 8U, &got));
    SAG_ASSERT_EQ_MEM(got.data, "fallback", 8U);
    sag_clip_shutdown();
    clip_unlink_name(&f, "out.bin");
    SAG_ASSERT_EQ_I64(setenv("SSH_TTY", "/dev/pts/9", 1), 0);
    sag_clip_reset();
    SAG_ASSERT(sag_clip_write(&value, '+'));
    sag_clip_after_render(&terminal, clip_test_now_ms());
    SAG_ASSERT(!sag_clip_busy());
    SAG_ASSERT(!clip_read_file(f.output, &got));
    bytebuf_free(&got);
    bytebuf_free(&terminal);
    sag_regval_free(&value);
    sag_clip_shutdown();
    clip_fixture_free(&f);
    for (i = SAG_ARRAY_LEN(names); i != 0U; i--)
        clip_env_restore(&saved[i - 1U]);
}

void test_clipboard_sync_default_yank_not_delete(void)
{
    ClipEnv clipboard;
    ClipFixture f;
    Registers registers;
    RegVal value;
    Bytebuf got;
    Bytebuf terminal;

    clip_env_save(&clipboard, "SAG_CLIPBOARD");
    clip_fixture_init(&f);
    clip_set_custom(&f, "write", false);
    sag_reg_init(&registers);
    clip_value(&value, (const u8 *)"sync", 4U);
    bytebuf_init(&got);
    bytebuf_init(&terminal);
    SAG_ASSERT_EQ_U64(registers.clipboard_sync, SAG_CLIP_SYNC_YANK);
    sag_reg_delete(&registers, 0U, &value);
    SAG_ASSERT(!sag_clip_pending());
    SAG_ASSERT_EQ_U64(registers.system.bytes.len, 0U);
    sag_reg_yank(&registers, 0U, &value);
    SAG_ASSERT(sag_clip_pending());
    SAG_ASSERT_EQ_U64(registers.system.bytes.len, 4U);
    sag_clip_after_render(&terminal, clip_test_now_ms());
    clip_pump_until_idle();
    SAG_ASSERT(clip_wait_file(f.output, 4U, &got));
    SAG_ASSERT_EQ_MEM(got.data, "sync", 4U);
    bytebuf_free(&terminal);
    bytebuf_free(&got);
    sag_regval_free(&value);
    sag_reg_free(&registers);
    sag_clip_shutdown();
    clip_fixture_free(&f);
    clip_env_restore(&clipboard);
}

void test_clipboard_sync_modes_route_only_documented_writes(void)
{
    ClipEnv clipboard;
    ClipFixture f;
    Registers registers;
    RegVal value;

    clip_env_save(&clipboard, "SAG_CLIPBOARD");
    clip_fixture_init(&f);
    clip_set_custom(&f, "write", false);
    sag_reg_init(&registers);
    clip_value(&value, (const u8 *)"modes", 5U);

    registers.clipboard_sync = SAG_CLIP_SYNC_OFF;
    sag_reg_yank(&registers, 0U, &value);
    SAG_ASSERT(!sag_clip_pending());
    sag_reg_yank(&registers, '+', &value);
    SAG_ASSERT(sag_clip_pending());
    sag_clip_reset();

    registers.clipboard_sync = SAG_CLIP_SYNC_YANK;
    sag_reg_delete(&registers, 0U, &value);
    SAG_ASSERT(!sag_clip_pending());
    sag_reg_yank(&registers, 0U, &value);
    SAG_ASSERT(sag_clip_pending());
    sag_clip_reset();

    registers.clipboard_sync = SAG_CLIP_SYNC_ALL;
    sag_reg_delete(&registers, 0U, &value);
    SAG_ASSERT(sag_clip_pending());
    sag_clip_reset();

    registers.clipboard_sync = SAG_CLIP_SYNC_UNNAMED;
    sag_reg_delete(&registers, 0U, &value);
    SAG_ASSERT(sag_clip_pending());
    SAG_ASSERT_EQ_MEM(registers.system.bytes.data, "modes", 5U);

    sag_regval_free(&value);
    sag_reg_free(&registers);
    sag_clip_shutdown();
    clip_fixture_free(&f);
    clip_env_restore(&clipboard);
}

static const char *clip_captured_read_failure(void)
{
    if (sag_test_log_contains(SAG_LOG_WARN, "no readable"))
        return "clipboard read failed: no readable backend";
    if (sag_test_log_contains(SAG_LOG_WARN, "no read command"))
        return "clipboard read failed: backend has no read command";
    if (sag_test_log_contains(SAG_LOG_WARN, "cannot start reader"))
        return "clipboard read failed: cannot start reader";
    if (sag_test_log_contains(SAG_LOG_WARN, "timed out"))
        return "clipboard read failed: reader timed out";
    if (sag_test_log_contains(SAG_LOG_WARN, "poll failed"))
        return "clipboard read failed: poll failed";
    if (sag_test_log_contains(SAG_LOG_WARN, "read exceeds"))
        return "clipboard read failed: byte limit exceeded";
    if (sag_test_log_contains(SAG_LOG_WARN, "pipe failed"))
        return "clipboard read failed: pipe read failed";
    if (sag_test_log_contains(SAG_LOG_WARN, "wait failed"))
        return "clipboard read failed: waitpid failed";
    if (sag_test_log_contains(SAG_LOG_WARN, "exited with status"))
        return "clipboard read failed: reader exited nonzero";
    if (sag_test_log_contains(SAG_LOG_WARN, "terminated by signal"))
        return "clipboard read failed: reader was signaled";
    return "clipboard read failed without a diagnostic";
}

void test_clipboard_unnamed_paste_reads_subprocess(void)
{
    static const u8 payload[] = {0U, (u8)'Q'};
    ClipEnv clipboard;
    ClipFixture f;
    Registers registers;
    TextBuf *tb;
    CursorSet cursors;
    Cursor cursor = {BYTEOFF(0U), {0U}, BYTEOFF(0U)};
    UndoTree *undo;
    EditCtx edit;
    TextIter iter;
    const u8 *bytes;
    u64 len;
    Bytebuf materialized;
    const char *paste_failure = NULL;

    clip_env_save(&clipboard, "SAG_CLIPBOARD");
    clip_fixture_init(&f);
    clip_write_file(f.input, payload, sizeof(payload));
    clip_set_custom(&f, "write", true);
    sag_reg_init(&registers);
    registers.clipboard_sync = SAG_CLIP_SYNC_UNNAMED;
    tb = sag_textbuf_from_bytes((const u8 *)"a", 1U);
    sag_cset_init(&cursors, cursor);
    undo = sag_undo_new(tb);
    edit = (EditCtx){tb, NULL, &cursors, 7U, NULL, undo, NULL};
    bytebuf_init(&materialized);

    sag_test_capture_log();
    sag_test_count_assertion();
    if (!sag_reg_paste(&registers, &edit, '"', true, 8U)) {
        paste_failure = clip_captured_read_failure();
        goto cleanup;
    }
    SAG_ASSERT_EQ_U64(sag_textbuf_len(tb), 3U);
    SAG_ASSERT(sag_textiter_begin(&iter, tb, BYTEOFF(0U)));
    while (materialized.len < 3U) {
        SAG_ASSERT(sag_textiter_chunk(&iter, tb, &bytes, &len));
        bytebuf_append(&materialized, bytes, (size_t)len);
        if (materialized.len < 3U)
            SAG_ASSERT(sag_textiter_advance(&iter, tb));
    }
    SAG_ASSERT_EQ_MEM(materialized.data, "\0Qa", 3U);
    SAG_ASSERT_EQ_U64(registers.system.bytes.len, sizeof(payload));
    SAG_ASSERT_EQ_MEM(registers.system.bytes.data, payload, sizeof(payload));
    SAG_ASSERT_EQ_MEM(registers.unnamed.bytes.data, payload, sizeof(payload));
    SAG_ASSERT_EQ_U64(registers.ring_len, 1U);
    SAG_ASSERT_EQ_U64(undo->nodes.len, 2U);

cleanup:
    bytebuf_free(&materialized);
    sag_undo_free(undo);
    sag_cset_free(&cursors);
    sag_textbuf_free(tb);
    sag_reg_free(&registers);
    sag_clip_shutdown();
    clip_fixture_free(&f);
    clip_env_restore(&clipboard);
    if (paste_failure != NULL)
        sag_test_fail(__FILE__, __LINE__, paste_failure);
}
