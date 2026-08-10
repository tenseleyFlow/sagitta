#define _POSIX_C_SOURCE 200809L

#include "text/clipboard.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "term/osc52.h"
#include "term/render.h"
#include "term/tty.h"
#include "util/buf.h"
#include "util/log.h"

#define YEW_CLIP_REAP_MAX 512U
#define YEW_CLIP_BACKEND_COUNT 7U

typedef struct YewClipCmd {
    char **argv;
    char *storage;
} YewClipCmd;

typedef struct YewClipboardState {
    RegVal queued;
    RegVal active;
    bool initialized;
    bool queued_valid;
    bool active_valid;
    bool write_cached;
    bool read_cached;
    YewClipBackend write_backend;
    YewClipBackend read_backend;
    YewClipBackend active_backend;
    u32 demoted;
    u8 queued_target;
    u8 active_target;
    int write_fd;
    int pid_fd;
    int exec_fd;
    pid_t tool_pid;
    pid_t reap[YEW_CLIP_REAP_MAX];
    u32 reap_len;
    size_t written;
    i64 deadline_ms;
    i64 timeout_ms;
    u64 read_max;
    bool exec_ready;
} YewClipboardState;

static YewClipboardState yew_clip;

static void clip_value_clear(RegVal *v)
{
    v->type = YEW_REG_CHARWISE;
    v->ragged = false;
    v->width = 0U;
    v->bytes.len = 0U;
    v->rows.len = 0U;
    v->t_wall = 0;
}

static i64 clip_now_ms(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        YEW_BUG("clipboard: monotonic clock failed");
    if ((i64)ts.tv_sec > (INT64_MAX - (i64)(ts.tv_nsec / 1000000L)) /
                          1000)
        return INT64_MAX;
    return (i64)ts.tv_sec * 1000 + (i64)(ts.tv_nsec / 1000000L);
}

static i64 clip_parse_positive_ms(const char *name, i64 fallback)
{
    const char *text = getenv(name);
    char *end = NULL;
    long long value;

    if (text == NULL || text[0] == '\0')
        return fallback;
    errno = 0;
    value = strtoll(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || value <= 0) {
        yew_log(YEW_LOG_WARN, "clipboard: ignoring invalid %s", name);
        return fallback;
    }
    return value > INT64_MAX ? INT64_MAX : (i64)value;
}

static void clip_ignore_sigpipe(void)
{
    struct sigaction action;

    (void)memset(&action, 0, sizeof(action));
    action.sa_handler = SIG_IGN;
    (void)sigemptyset(&action.sa_mask);
    if (sigaction(SIGPIPE, &action, NULL) != 0)
        yew_log(YEW_LOG_WARN, "clipboard: cannot ignore SIGPIPE: %s",
                strerror(errno));
}

static void clip_init(void)
{
    if (yew_clip.initialized)
        return;
    (void)memset(&yew_clip, 0, sizeof(yew_clip));
    yew_regval_init(&yew_clip.queued);
    yew_regval_init(&yew_clip.active);
    yew_clip.write_fd = -1;
    yew_clip.pid_fd = -1;
    yew_clip.exec_fd = -1;
    yew_clip.timeout_ms = clip_parse_positive_ms(
        "YEW_CLIPBOARD_TIMEOUT_MS", YEW_CLIPBOARD_TIMEOUT_DEFAULT_MS);
    yew_clip.read_max = YEW_CLIPBOARD_READ_MAX_DEFAULT;
    yew_clip.initialized = true;
    clip_ignore_sigpipe();
}

static bool clip_nonempty_env(const char *name)
{
    const char *value = getenv(name);

    return value != NULL && value[0] != '\0';
}

static bool clip_remote(void)
{
    return clip_nonempty_env("SSH_TTY") ||
           clip_nonempty_env("SSH_CONNECTION");
}

static bool clip_path_exec(const char *name)
{
    const char *path = getenv("PATH");
    const char *at;
    size_t name_len;

    if (name == NULL || name[0] == '\0')
        return false;
    if (strchr(name, '/') != NULL) {
        struct stat st;

        return access(name, X_OK) == 0 && stat(name, &st) == 0 &&
               S_ISREG(st.st_mode);
    }
    if (path == NULL)
        path = "/usr/local/bin:/usr/bin:/bin";
    name_len = strlen(name);
    at = path;
    for (;;) {
        const char *colon = strchr(at, ':');
        size_t dir_len = colon != NULL ? (size_t)(colon - at) : strlen(at);
        size_t need = (dir_len == 0U ? 1U : dir_len) + 1U + name_len + 1U;
        char *candidate = yew_xmalloc(need);
        size_t used = 0U;
        bool found;
        struct stat st;

        if (dir_len == 0U) {
            candidate[used++] = '.';
        } else {
            (void)memcpy(candidate, at, dir_len);
            used = dir_len;
        }
        candidate[used++] = '/';
        (void)memcpy(candidate + used, name, name_len + 1U);
        found = access(candidate, X_OK) == 0 && stat(candidate, &st) == 0 &&
                S_ISREG(st.st_mode);
        free(candidate);
        if (found)
            return true;
        if (colon == NULL)
            return false;
        at = colon + 1;
    }
}

static u32 clip_backend_bit(YewClipBackend backend)
{
    return backend < 32 ? UINT32_C(1) << (u32)backend : 0U;
}

static bool clip_available(YewClipBackend backend)
{
    if ((yew_clip.demoted & clip_backend_bit(backend)) != 0U)
        return false;
    switch (backend) {
    case YEW_CLIP_WL:
        return clip_nonempty_env("WAYLAND_DISPLAY") &&
               clip_path_exec("wl-copy");
    case YEW_CLIP_XCLIP:
        return clip_nonempty_env("DISPLAY") && clip_path_exec("xclip");
    case YEW_CLIP_XSEL:
        return clip_nonempty_env("DISPLAY") && clip_path_exec("xsel");
    case YEW_CLIP_PB:
#if defined(__APPLE__) && (defined(__aarch64__) || defined(__arm64__))
        return clip_path_exec("pbcopy");
#else
        return false;
#endif
    case YEW_CLIP_OSC52:
        return yew_tty_fd_is_terminal(STDOUT_FILENO) &&
               yew_osc52_mode(NULL) != YEW_OSC52_OFF;
    case YEW_CLIP_CUSTOM:
    case YEW_CLIP_NONE:
        return false;
    }
    return false;
}

static YewClipBackend clip_named_backend(const char *value, bool read_side,
                                         bool *recognized)
{
    const char *bar;

    *recognized = true;
    if (strcmp(value, "none") == 0)
        return YEW_CLIP_NONE;
    if (strcmp(value, "osc52") == 0)
        return read_side || yew_osc52_mode(NULL) == YEW_OSC52_OFF ?
               YEW_CLIP_NONE : YEW_CLIP_OSC52;
    if (strcmp(value, "wl") == 0)
        return YEW_CLIP_WL;
    if (strcmp(value, "xclip") == 0)
        return YEW_CLIP_XCLIP;
    if (strcmp(value, "xsel") == 0)
        return YEW_CLIP_XSEL;
    if (strcmp(value, "pb") == 0)
        return YEW_CLIP_PB;
    if (strncmp(value, "cmd:", 4U) == 0) {
        bar = strchr(value + 4U, '|');
        if (read_side)
            return bar != NULL && bar[1] != '\0' ? YEW_CLIP_CUSTOM :
                                                   YEW_CLIP_NONE;
        return value[4] != '\0' && bar != value + 4U ? YEW_CLIP_CUSTOM :
                                                       YEW_CLIP_NONE;
    }
    *recognized = false;
    return YEW_CLIP_NONE;
}

static YewClipBackend clip_detect_direction(bool read_side)
{
    const char *override = getenv("YEW_CLIPBOARD");
    bool recognized;

    if (override != NULL && override[0] != '\0' &&
        strcmp(override, "auto") != 0) {
        YewClipBackend forced = clip_named_backend(override, read_side,
                                                   &recognized);

        if (recognized) {
            if ((yew_clip.demoted & clip_backend_bit(forced)) != 0U)
                return YEW_CLIP_NONE;
            return forced;
        }
        yew_log(YEW_LOG_WARN,
                "clipboard: ignoring invalid YEW_CLIPBOARD override");
    }
    if (!read_side && clip_remote())
        return clip_available(YEW_CLIP_OSC52) ? YEW_CLIP_OSC52 :
                                               YEW_CLIP_NONE;
    if (clip_available(YEW_CLIP_WL))
        return YEW_CLIP_WL;
    if (clip_available(YEW_CLIP_XCLIP))
        return YEW_CLIP_XCLIP;
    if (clip_available(YEW_CLIP_XSEL))
        return YEW_CLIP_XSEL;
    if (clip_available(YEW_CLIP_PB))
        return YEW_CLIP_PB;
    if (!read_side && clip_available(YEW_CLIP_OSC52))
        return YEW_CLIP_OSC52;
    return YEW_CLIP_NONE;
}

static YewClipBackend clip_detect_subprocess_write(void)
{
    if (clip_available(YEW_CLIP_WL))
        return YEW_CLIP_WL;
    if (clip_available(YEW_CLIP_XCLIP))
        return YEW_CLIP_XCLIP;
    if (clip_available(YEW_CLIP_XSEL))
        return YEW_CLIP_XSEL;
    if (clip_available(YEW_CLIP_PB))
        return YEW_CLIP_PB;
    return YEW_CLIP_NONE;
}

YewClipBackend yew_clip_detect(void)
{
    clip_init();
    if (!yew_clip.write_cached) {
        yew_clip.write_backend = clip_detect_direction(false);
        yew_clip.write_cached = true;
    }
    return yew_clip.write_backend;
}

YewClipBackend yew_clip_detect_read(void)
{
    clip_init();
    if (!yew_clip.read_cached) {
        yew_clip.read_backend = clip_detect_direction(true);
        yew_clip.read_cached = true;
    }
    return yew_clip.read_backend;
}

static void clip_cmd_free(YewClipCmd *cmd)
{
    free(cmd->argv);
    free(cmd->storage);
    cmd->argv = NULL;
    cmd->storage = NULL;
}

static bool clip_cmd_copy(YewClipCmd *cmd, const char *const *words,
                          size_t count)
{
    size_t i;
    size_t total = 0U;
    char *at;

    (void)memset(cmd, 0, sizeof(*cmd));
    for (i = 0U; i < count; i++) {
        size_t len = strlen(words[i]);
        if (len > SIZE_MAX - total - 1U)
            YEW_BUG("clipboard command size overflow");
        total += len + 1U;
    }
    cmd->argv = yew_xcalloc(count + 1U, sizeof(*cmd->argv));
    cmd->storage = yew_xmalloc(total == 0U ? 1U : total);
    at = cmd->storage;
    for (i = 0U; i < count; i++) {
        size_t len = strlen(words[i]);
        cmd->argv[i] = at;
        (void)memcpy(at, words[i], len + 1U);
        at += len + 1U;
    }
    return count != 0U;
}

static bool clip_cmd_parse(YewClipCmd *cmd, const char *begin, size_t len)
{
    size_t count = 0U;
    size_t i = 0U;
    char *at;

    (void)memset(cmd, 0, sizeof(*cmd));
    while (i < len) {
        while (i < len && begin[i] == ' ')
            i++;
        if (i == len)
            break;
        count++;
        while (i < len && begin[i] != ' ')
            i++;
    }
    if (count == 0U)
        return false;
    cmd->storage = yew_xmalloc(len + 1U);
    (void)memcpy(cmd->storage, begin, len);
    cmd->storage[len] = '\0';
    cmd->argv = yew_xcalloc(count + 1U, sizeof(*cmd->argv));
    count = 0U;
    at = cmd->storage;
    while (*at != '\0') {
        while (*at == ' ')
            *at++ = '\0';
        if (*at == '\0')
            break;
        cmd->argv[count++] = at;
        while (*at != '\0' && *at != ' ')
            at++;
    }
    return true;
}

static bool clip_custom_cmd(YewClipCmd *cmd, bool read_side)
{
    const char *value = getenv("YEW_CLIPBOARD");
    const char *body;
    const char *bar;

    if (value == NULL || strncmp(value, "cmd:", 4U) != 0)
        return false;
    body = value + 4U;
    bar = strchr(body, '|');
    if (read_side) {
        if (bar == NULL)
            return false;
        return clip_cmd_parse(cmd, bar + 1U, strlen(bar + 1U));
    }
    return clip_cmd_parse(cmd, body,
                          bar != NULL ? (size_t)(bar - body) : strlen(body));
}

static bool clip_command(YewClipCmd *cmd, YewClipBackend backend,
                         bool read_side, u8 target)
{
    static const char *const wl_write[] = {
        "wl-copy", "--type", "text/plain;charset=utf-8"
    };
    static const char *const wl_write_primary[] = {
        "wl-copy", "--type", "text/plain;charset=utf-8", "--primary"
    };
    static const char *const wl_read[] = {"wl-paste", "--no-newline"};
    static const char *const wl_read_primary[] = {
        "wl-paste", "--primary", "--no-newline"
    };
    static const char *const xclip_write[] = {
        "xclip", "-selection", "clipboard", "-in"
    };
    static const char *const xclip_write_primary[] = {
        "xclip", "-selection", "primary", "-in"
    };
    static const char *const xclip_read[] = {
        "xclip", "-selection", "clipboard", "-out"
    };
    static const char *const xclip_read_primary[] = {
        "xclip", "-selection", "primary", "-out"
    };
    static const char *const xsel_write[] = {
        "xsel", "--clipboard", "--input"
    };
    static const char *const xsel_write_primary[] = {
        "xsel", "--primary", "--input"
    };
    static const char *const xsel_read[] = {
        "xsel", "--clipboard", "--output"
    };
    static const char *const xsel_read_primary[] = {
        "xsel", "--primary", "--output"
    };
    static const char *const pb_write[] = {"pbcopy"};
    static const char *const pb_read[] = {"pbpaste"};

#define CLIP_COPY_WORDS(words) \
    clip_cmd_copy(cmd, words, YEW_ARRAY_LEN(words))
    switch (backend) {
    case YEW_CLIP_WL:
        if (read_side)
            return target == '*' ? CLIP_COPY_WORDS(wl_read_primary) :
                                   CLIP_COPY_WORDS(wl_read);
        return target == '*' ? CLIP_COPY_WORDS(wl_write_primary) :
                               CLIP_COPY_WORDS(wl_write);
    case YEW_CLIP_XCLIP:
        if (read_side)
            return target == '*' ? CLIP_COPY_WORDS(xclip_read_primary) :
                                   CLIP_COPY_WORDS(xclip_read);
        return target == '*' ? CLIP_COPY_WORDS(xclip_write_primary) :
                               CLIP_COPY_WORDS(xclip_write);
    case YEW_CLIP_XSEL:
        if (read_side)
            return target == '*' ? CLIP_COPY_WORDS(xsel_read_primary) :
                                   CLIP_COPY_WORDS(xsel_read);
        return target == '*' ? CLIP_COPY_WORDS(xsel_write_primary) :
                               CLIP_COPY_WORDS(xsel_write);
    case YEW_CLIP_PB:
        return read_side ? CLIP_COPY_WORDS(pb_read) :
                           CLIP_COPY_WORDS(pb_write);
    case YEW_CLIP_CUSTOM:
        return clip_custom_cmd(cmd, read_side);
    case YEW_CLIP_NONE:
    case YEW_CLIP_OSC52:
        break;
    }
#undef CLIP_COPY_WORDS
    (void)memset(cmd, 0, sizeof(*cmd));
    return false;
}

static bool clip_pipe(int fds[2])
{
    int i;

    if (!yew_pipe_cloexec(fds))
        return false;
    for (i = 0; i < 2; i++) {
        int flags = fcntl(fds[i], F_GETFD);

        if (flags < 0 || fcntl(fds[i], F_SETFD, flags | FD_CLOEXEC) != 0) {
            (void)close(fds[0]);
            (void)close(fds[1]);
            return false;
        }
    }
    return true;
}

static bool clip_nonblock(int fd)
{
    int flags = fcntl(fd, F_GETFL);

    return flags >= 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

static void clip_child_sigpipe_default(void)
{
    struct sigaction action;

    (void)memset(&action, 0, sizeof(action));
    action.sa_handler = SIG_DFL;
    (void)sigemptyset(&action.sa_mask);
    (void)sigaction(SIGPIPE, &action, NULL);
}

static void clip_child_exec_fail(int fd, int error)
{
    (void)!write(fd, &error, sizeof(error));
    _exit(126);
}

static bool clip_reap_track(pid_t pid)
{
    if (yew_clip.reap_len >= YEW_CLIP_REAP_MAX) {
        yew_clip_reap();
        if (yew_clip.reap_len >= YEW_CLIP_REAP_MAX)
            return false;
    }
    yew_clip.reap[yew_clip.reap_len++] = pid;
    return true;
}

void yew_clip_reap(void)
{
    u32 out = 0U;
    u32 i;

    if (!yew_clip.initialized)
        return;
    for (i = 0U; i < yew_clip.reap_len; i++) {
        pid_t result = waitpid(yew_clip.reap[i], NULL, WNOHANG);

        if (result == 0 || (result < 0 && errno == EINTR))
            yew_clip.reap[out++] = yew_clip.reap[i];
        else if (result < 0 && errno != ECHILD)
            yew_log(YEW_LOG_WARN, "clipboard: waitpid failed: %s",
                    strerror(errno));
    }
    yew_clip.reap_len = out;
}

u32 yew_clip_owned_children(void)
{
    return yew_clip.initialized ? yew_clip.reap_len : 0U;
}

static bool clip_spawn_writer(YewClipBackend backend, u8 target, i64 now_ms)
{
    YewClipCmd cmd;
    int data[2] = {-1, -1};
    int pidpipe[2] = {-1, -1};
    int execpipe[2] = {-1, -1};
    pid_t middle;

    if (!clip_command(&cmd, backend, false, target))
        return false;
    if (!clip_pipe(data) || !clip_pipe(pidpipe) || !clip_pipe(execpipe)) {
        if (data[0] >= 0) {
            (void)close(data[0]);
            (void)close(data[1]);
        }
        if (pidpipe[0] >= 0) {
            (void)close(pidpipe[0]);
            (void)close(pidpipe[1]);
        }
        clip_cmd_free(&cmd);
        return false;
    }
    yew_clip_reap();
    if (yew_clip.reap_len >= YEW_CLIP_REAP_MAX ||
        !clip_nonblock(data[1]) || !clip_nonblock(pidpipe[0]) ||
        !clip_nonblock(execpipe[0])) {
        (void)close(data[0]);
        (void)close(data[1]);
        (void)close(pidpipe[0]);
        (void)close(pidpipe[1]);
        (void)close(execpipe[0]);
        (void)close(execpipe[1]);
        clip_cmd_free(&cmd);
        return false;
    }
    middle = fork();
    if (middle < 0) {
        (void)close(data[0]);
        (void)close(data[1]);
        (void)close(pidpipe[0]);
        (void)close(pidpipe[1]);
        (void)close(execpipe[0]);
        (void)close(execpipe[1]);
        clip_cmd_free(&cmd);
        return false;
    }
    if (middle == 0) {
        pid_t tool;
        int tool_errno;

        (void)close(data[1]);
        (void)close(pidpipe[0]);
        (void)close(execpipe[0]);
        tool = fork();
        tool_errno = errno;
        if (tool == 0) {
            pid_t self = getpid();
            int nullfd;
            int exec_errno;

            (void)!write(pidpipe[1], &self, sizeof(self));
            (void)close(pidpipe[1]);
            if (dup2(data[0], STDIN_FILENO) < 0)
                clip_child_exec_fail(execpipe[1], errno);
            (void)close(data[0]);
            nullfd = open("/dev/null", O_WRONLY);
            if (nullfd < 0 || dup2(nullfd, STDOUT_FILENO) < 0 ||
                dup2(nullfd, STDERR_FILENO) < 0)
                clip_child_exec_fail(execpipe[1], errno);
            if (nullfd != STDOUT_FILENO && nullfd != STDERR_FILENO)
                (void)close(nullfd);
            clip_child_sigpipe_default();
            execvp(cmd.argv[0], cmd.argv);
            exec_errno = errno;
            (void)!write(execpipe[1], &exec_errno, sizeof(exec_errno));
            _exit(127);
        }
        (void)close(data[0]);
        (void)close(pidpipe[1]);
        if (tool < 0)
            clip_child_exec_fail(execpipe[1], tool_errno);
        (void)close(execpipe[1]);
        _exit(0);
    }
    (void)close(data[0]);
    (void)close(pidpipe[1]);
    (void)close(execpipe[1]);
    clip_cmd_free(&cmd);
    if (!clip_reap_track(middle)) {
        (void)close(data[1]);
        (void)close(pidpipe[0]);
        (void)close(execpipe[0]);
        return false;
    }
    yew_clip.write_fd = data[1];
    yew_clip.pid_fd = pidpipe[0];
    yew_clip.exec_fd = execpipe[0];
    yew_clip.tool_pid = 0;
    yew_clip.written = 0U;
    yew_clip.exec_ready = false;
    yew_clip.deadline_ms = now_ms > INT64_MAX - yew_clip.timeout_ms ?
                           INT64_MAX : now_ms + yew_clip.timeout_ms;
    return true;
}

static void clip_close_active(void)
{
    if (yew_clip.write_fd >= 0)
        (void)close(yew_clip.write_fd);
    if (yew_clip.pid_fd >= 0)
        (void)close(yew_clip.pid_fd);
    if (yew_clip.exec_fd >= 0)
        (void)close(yew_clip.exec_fd);
    yew_clip.write_fd = -1;
    yew_clip.pid_fd = -1;
    yew_clip.exec_fd = -1;
    yew_clip.tool_pid = 0;
    yew_clip.written = 0U;
    yew_clip.deadline_ms = 0;
    yew_clip.exec_ready = false;
    yew_clip.active_valid = false;
    clip_value_clear(&yew_clip.active);
}

static void clip_retry_active(void)
{
    if (!yew_clip.queued_valid) {
        yew_regval_copy(&yew_clip.queued, &yew_clip.active);
        yew_clip.queued_target = yew_clip.active_target;
        yew_clip.queued_valid = true;
    }
    yew_clip.demoted |= clip_backend_bit(yew_clip.active_backend);
    yew_clip.write_cached = false;
    clip_close_active();
}

static void clip_read_tool_pid(void)
{
    pid_t pid;
    ssize_t got;

    if (yew_clip.pid_fd < 0 || yew_clip.tool_pid > 0)
        return;
    got = read(yew_clip.pid_fd, &pid, sizeof(pid));
    if (got == (ssize_t)sizeof(pid)) {
        yew_clip.tool_pid = pid;
        (void)close(yew_clip.pid_fd);
        yew_clip.pid_fd = -1;
    } else if (got == 0) {
        (void)close(yew_clip.pid_fd);
        yew_clip.pid_fd = -1;
    }
}

static bool clip_read_exec_status(void)
{
    int exec_errno;
    ssize_t got;

    if (yew_clip.exec_fd < 0 || yew_clip.exec_ready)
        return true;
    got = read(yew_clip.exec_fd, &exec_errno, sizeof(exec_errno));
    if (got == 0) {
        (void)close(yew_clip.exec_fd);
        yew_clip.exec_fd = -1;
        yew_clip.exec_ready = true;
        return true;
    }
    if (got == (ssize_t)sizeof(exec_errno)) {
        yew_log(YEW_LOG_WARN,
                "clipboard: writer exec failed: %s; demoting backend",
                strerror(exec_errno));
        return false;
    }
    if (got < 0 && (errno == EAGAIN || errno == EWOULDBLOCK ||
                    errno == EINTR))
        return true;
    yew_log(YEW_LOG_WARN,
            "clipboard: writer exec status failed; demoting backend");
    return false;
}

bool yew_clip_write(const RegVal *v, u8 target)
{
    if (v == NULL)
        return false;
    clip_init();
    if (target != '*')
        target = '+';
    yew_regval_copy(&yew_clip.queued, v);
    yew_clip.queued_target = target;
    yew_clip.queued_valid = true;
    return true;
}

static void clip_start_queued(i64 now_ms)
{
    Bytebuf frame;
    YewClipBackend backend;

    if (!yew_clip.queued_valid || yew_clip.active_valid)
        return;
    backend = yew_clip_detect();
    if (backend == YEW_CLIP_NONE) {
        yew_log(YEW_LOG_WARN,
                "clipboard: no writable system clipboard backend");
        yew_clip.queued_valid = false;
        clip_value_clear(&yew_clip.queued);
        return;
    }
    yew_regval_copy(&yew_clip.active, &yew_clip.queued);
    yew_clip.active_target = yew_clip.queued_target;
    yew_clip.active_backend = backend;
    yew_clip.active_valid = true;
    yew_clip.queued_valid = false;
    clip_value_clear(&yew_clip.queued);

    if (backend == YEW_CLIP_OSC52) {
        bytebuf_init(&frame);
        if (yew_osc52_build_env(&frame, yew_clip.active.bytes.data,
                                yew_clip.active.bytes.len, NULL)) {
            yew_term_oob_queue(frame.data, frame.len);
            bytebuf_free(&frame);
            clip_close_active();
            return;
        }
        bytebuf_free(&frame);
        while (!clip_remote()) {
            YewClipBackend fallback = clip_detect_subprocess_write();

            if (fallback == YEW_CLIP_NONE)
                break;
            yew_clip.active_backend = fallback;
            if (clip_spawn_writer(fallback, yew_clip.active_target, now_ms))
                return;
            yew_log(YEW_LOG_WARN,
                    "clipboard: cannot start fallback writer backend");
            yew_clip.demoted |= clip_backend_bit(fallback);
            yew_clip.write_cached = false;
        }
        yew_log(YEW_LOG_WARN,
                "clipboard: payload exceeds OSC 52 limit; register + retains it");
        clip_close_active();
        return;
    }
    if (!clip_spawn_writer(backend, yew_clip.active_target, now_ms)) {
        yew_log(YEW_LOG_WARN, "clipboard: cannot start writer backend");
        clip_retry_active();
    }
}

static void clip_start_available(i64 now_ms)
{
    u32 attempts = 0U;

    while (yew_clip.queued_valid && !yew_clip.active_valid &&
           attempts++ < YEW_CLIP_BACKEND_COUNT)
        clip_start_queued(now_ms);
}

void yew_clip_after_render(Bytebuf *terminal_out, i64 now_ms)
{
    clip_init();
    yew_clip_reap();
    yew_clip_pump(now_ms);
    clip_start_available(now_ms);
    yew_clip_pump(now_ms);
    if (terminal_out != NULL)
        (void)yew_term_oob_flush(terminal_out);
}

void yew_clip_pump(i64 now_ms)
{
    u32 fallbacks = 0U;

    if (!yew_clip.initialized)
        return;
    yew_clip_reap();
    for (;;) {
        if (!yew_clip.active_valid)
            return;
        clip_read_tool_pid();
        if (!clip_read_exec_status()) {
            clip_retry_active();
        } else if (now_ms >= yew_clip.deadline_ms) {
            if (yew_clip.tool_pid > 0)
                (void)kill(yew_clip.tool_pid, SIGKILL);
            yew_log(YEW_LOG_WARN, "clipboard: writer timed out");
            clip_retry_active();
        } else {
            while (yew_clip.written < yew_clip.active.bytes.len) {
                ssize_t n = write(
                    yew_clip.write_fd,
                    yew_clip.active.bytes.data + yew_clip.written,
                    yew_clip.active.bytes.len - yew_clip.written);

                if (n > 0) {
                    yew_clip.written += (size_t)n;
                    continue;
                }
                if (n < 0 && errno == EINTR)
                    continue;
                if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
                    return;
                if (n < 0 && errno == EPIPE) {
                    yew_log(
                        YEW_LOG_WARN,
                        "clipboard: writer closed its pipe; demoting backend");
                } else {
                    yew_log(YEW_LOG_WARN, "clipboard: write failed: %s",
                            strerror(errno));
                }
                clip_retry_active();
                break;
            }
            if (yew_clip.active_valid &&
                yew_clip.written == yew_clip.active.bytes.len) {
                if (yew_clip.write_fd >= 0) {
                    (void)close(yew_clip.write_fd);
                    yew_clip.write_fd = -1;
                }
                if (!clip_read_exec_status()) {
                    clip_retry_active();
                } else if (yew_clip.exec_ready) {
                    clip_close_active();
                    return;
                } else {
                    return;
                }
            }
        }
        if (++fallbacks >= YEW_CLIP_BACKEND_COUNT)
            return;
        clip_start_available(now_ms);
    }
}

int yew_clip_write_fd(void)
{
    if (!yew_clip.initialized)
        return -1;
    return yew_clip.write_fd >= 0 ? yew_clip.write_fd : yew_clip.exec_fd;
}

i64 yew_clip_deadline(void)
{
    return yew_clip.initialized && yew_clip.active_valid ?
           yew_clip.deadline_ms : -1;
}

bool yew_clip_busy(void)
{
    return yew_clip.initialized && yew_clip.active_valid;
}

bool yew_clip_pending(void)
{
    return yew_clip.initialized && yew_clip.queued_valid;
}

static bool clip_spawn_reader(const YewClipCmd *cmd, int *fd, pid_t *pid)
{
    int output[2];
    pid_t child;

    if (!clip_pipe(output))
        return false;
    child = fork();
    if (child < 0) {
        (void)close(output[0]);
        (void)close(output[1]);
        return false;
    }
    if (child == 0) {
        int nullfd;

        (void)close(output[0]);
        nullfd = open("/dev/null", O_RDWR);
        if (nullfd < 0 || dup2(nullfd, STDIN_FILENO) < 0 ||
            dup2(output[1], STDOUT_FILENO) < 0 ||
            dup2(nullfd, STDERR_FILENO) < 0)
            _exit(126);
        if (nullfd != STDIN_FILENO && nullfd != STDERR_FILENO)
            (void)close(nullfd);
        if (output[1] != STDOUT_FILENO)
            (void)close(output[1]);
        clip_child_sigpipe_default();
        execvp(cmd->argv[0], cmd->argv);
        _exit(127);
    }
    (void)close(output[1]);
    if (!clip_nonblock(output[0])) {
        (void)close(output[0]);
        (void)kill(child, SIGKILL);
        (void)waitpid(child, NULL, 0);
        return false;
    }
    *fd = output[0];
    *pid = child;
    return true;
}

bool yew_clip_read(RegVal *out, u8 target)
{
    YewClipBackend backend;
    YewClipCmd cmd;
    Bytebuf bytes;
    int fd = -1;
    pid_t pid = -1;
    i64 deadline;
    bool eof = false;
    bool failed = false;
    int status = 0;
    bool reaped = false;

    if (out == NULL)
        return false;
    clip_init();
    backend = yew_clip_detect_read();
    if (backend == YEW_CLIP_NONE || backend == YEW_CLIP_OSC52) {
        yew_log(YEW_LOG_WARN,
                "clipboard: no readable system clipboard backend");
        return false;
    }
    if (!clip_command(&cmd, backend, true, target == '*' ? '*' : '+')) {
        yew_log(YEW_LOG_WARN,
                "clipboard: selected backend has no read command");
        return false;
    }
    if (!clip_spawn_reader(&cmd, &fd, &pid)) {
        yew_log(YEW_LOG_WARN, "clipboard: cannot start reader: %s",
                strerror(errno));
        clip_cmd_free(&cmd);
        return false;
    }
    clip_cmd_free(&cmd);
    bytebuf_init(&bytes);
    deadline = clip_now_ms();
    deadline = deadline > INT64_MAX - yew_clip.timeout_ms ? INT64_MAX :
                                                                deadline + yew_clip.timeout_ms;
    while ((!reaped || !eof) && !failed) {
        struct pollfd pfd;
        i64 now = clip_now_ms();
        int timeout;

        if (now >= deadline) {
            yew_log(YEW_LOG_WARN,
                    "clipboard: reader timed out after %lld ms",
                    (long long)yew_clip.timeout_ms);
            failed = true;
            break;
        }
        timeout = (int)(deadline - now > 20 ? 20 : deadline - now);
        pfd.fd = fd;
        pfd.events = POLLIN | POLLHUP;
        pfd.revents = 0;
        {
            int ready = poll(&pfd, 1U, timeout);

            if (ready < 0 && errno != EINTR) {
                yew_log(YEW_LOG_WARN, "clipboard: reader poll failed: %s",
                        strerror(errno));
                failed = true;
                break;
            }
            if (!eof && ready > 0 &&
                (pfd.revents & (POLLIN | POLLHUP)) != 0) {
                for (;;) {
                    u8 chunk[8192];
                    ssize_t n = read(fd, chunk, sizeof(chunk));

                    if (n > 0) {
                        if ((u64)n > yew_clip.read_max - bytes.len) {
                            yew_log(YEW_LOG_WARN,
                                    "clipboard: read exceeds %llu-byte limit",
                                    (unsigned long long)yew_clip.read_max);
                            failed = true;
                            break;
                        }
                        bytebuf_append(&bytes, chunk, (size_t)n);
                    } else if (n == 0) {
                        eof = true;
                        (void)close(fd);
                        fd = -1;
                        break;
                    } else if (errno == EINTR) {
                        continue;
                    } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
                        break;
                    } else {
                        yew_log(YEW_LOG_WARN,
                                "clipboard: reader pipe failed: %s",
                                strerror(errno));
                        failed = true;
                        break;
                    }
                }
            }
        }
        if (!reaped) {
            pid_t got = waitpid(pid, &status, WNOHANG);

            if (got == pid)
                reaped = true;
            else if (got < 0 && errno != EINTR) {
                yew_log(YEW_LOG_WARN, "clipboard: reader wait failed: %s",
                        strerror(errno));
                failed = true;
                reaped = errno == ECHILD;
            }
        }
    }
    if (fd >= 0)
        (void)close(fd);
    if (!reaped) {
        (void)kill(pid, SIGKILL);
        while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
            ;
    }
    if (!failed && WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        clip_value_clear(out);
        bytebuf_append(&out->bytes, bytes.data, bytes.len);
        out->t_wall = (i64)time(NULL);
        bytebuf_free(&bytes);
        return true;
    }
    if (!failed) {
        if (WIFEXITED(status))
            yew_log(YEW_LOG_WARN,
                    "clipboard: reader exited with status %d",
                    WEXITSTATUS(status));
        else if (WIFSIGNALED(status))
            yew_log(YEW_LOG_WARN,
                    "clipboard: reader terminated by signal %d",
                    WTERMSIG(status));
        else
            yew_log(YEW_LOG_WARN,
                    "clipboard: reader did not exit cleanly");
    }
    bytebuf_free(&bytes);
    return false;
}

void yew_clip_set_read_max(u64 max_bytes)
{
    clip_init();
    yew_clip.read_max = max_bytes;
}

void yew_clip_shutdown(void)
{
    u32 i;

    if (!yew_clip.initialized)
        return;
    clip_read_tool_pid();
    if (yew_clip.tool_pid > 0)
        (void)kill(yew_clip.tool_pid, SIGKILL);
    if (yew_clip.write_fd >= 0)
        (void)close(yew_clip.write_fd);
    if (yew_clip.pid_fd >= 0)
        (void)close(yew_clip.pid_fd);
    if (yew_clip.exec_fd >= 0)
        (void)close(yew_clip.exec_fd);
    for (i = 0U; i < yew_clip.reap_len; i++) {
        while (waitpid(yew_clip.reap[i], NULL, 0) < 0 && errno == EINTR)
            ;
    }
    yew_regval_free(&yew_clip.queued);
    yew_regval_free(&yew_clip.active);
    (void)memset(&yew_clip, 0, sizeof(yew_clip));
}

void yew_clip_reset(void)
{
    yew_clip_shutdown();
    clip_init();
}
