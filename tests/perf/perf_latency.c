#define _POSIX_C_SOURCE 200809L

#include "support/live_pty.h"
#include "pty/vt.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

enum {
    SESSION_KEYS = 10000,
    SCREEN_ROWS = 50,
    SCREEN_COLS = 200,
    KEY_TIMEOUT_MS = 100,
    FLOOR_SAMPLES = 1001,
    MANY_BUFFER_COUNT = 50,
    MANY_BUFFER_HYDRATED = 20
};

typedef struct KeyStroke {
    u8 bytes[32];
    u8 len;
} KeyStroke;

typedef struct Session {
    KeyStroke keys[SESSION_KEYS];
    size_t len;
} Session;

typedef struct MetricSpec {
    const char *session;
    const char *fixture;
    const char *metric;
    bool supported;
    const char *why;
} MetricSpec;

typedef struct FrameRead {
    bool painted;
    i64 completed_ns;
    u64 frames;
} FrameRead;

typedef struct AssistRun {
    VtScreen screen;
    pid_t ai_pid;
    bool screen_on;
} AssistRun;

static const MetricSpec metrics[] = {
    {"typing", "small", "latency.typing.small.p99", true, NULL},
    {"typing", "huge", "latency.typing.huge.p99", true, NULL},
    {"edit", "syntax", "latency.edit.syntax.p99", true, NULL},
    {"multicursor", "small", "latency.multicursor.small.p99", true, NULL},
    {"navigate", "many-buffers", "latency.navigate.many_buffers.p99", true,
     NULL},
    {"search", "huge", "latency.search.huge.p99", true, NULL},
    {"typing", "assist", "latency.typing.assist.p99", true, NULL}
};

static void screen_feed(void *ctx, const u8 *bytes, size_t len)
{
    vt_feed(ctx, bytes, len);
}

static bool sort_i64(i64 *values, size_t len)
{
    i64 *scratch;
    size_t width;

    if (len < 2U)
        return true;
    scratch = malloc(len * sizeof(*scratch));
    if (scratch == NULL)
        return false;
    for (width = 1U; width < len;) {
        size_t lo;

        for (lo = 0U; lo < len; lo += width * 2U) {
            size_t mid = lo + width < len ? lo + width : len;
            size_t hi = mid + width < len ? mid + width : len;
            size_t left = lo;
            size_t right = mid;
            size_t out = lo;

            while (left < mid && right < hi) {
                if (values[right] < values[left])
                    scratch[out++] = values[right++];
                else
                    scratch[out++] = values[left++];
            }
            while (left < mid)
                scratch[out++] = values[left++];
            while (right < hi)
                scratch[out++] = values[right++];
        }
        (void)memcpy(values, scratch, len * sizeof(*values));
        if (width > len / 2U)
            break;
        width *= 2U;
    }
    free(scratch);
    return true;
}

static int poll_timeout(i64 deadline)
{
    i64 now = yew_live_pty_now_ns();
    i64 left;

    if (now < 0 || now >= deadline)
        return 0;
    left = deadline - now;
    if (left > INT64_C(1000000000))
        return 1000;
    return (int)((left + INT64_C(999999)) / INT64_C(1000000));
}

static bool same_ci(const char *a, size_t na, const char *b)
{
    size_t i;

    if (strlen(b) != na)
        return false;
    for (i = 0U; i < na; i++) {
        if (tolower((unsigned char)a[i]) !=
            tolower((unsigned char)b[i]))
            return false;
    }
    return true;
}

static bool modifier(const char *text, size_t len, u16 *mods)
{
    if (same_ci(text, len, "shift"))
        *mods = (u16)(*mods | 1U);
    else if (same_ci(text, len, "alt"))
        *mods = (u16)(*mods | 2U);
    else if (same_ci(text, len, "ctrl"))
        *mods = (u16)(*mods | 4U);
    else if (same_ci(text, len, "super"))
        *mods = (u16)(*mods | 8U);
    else if (same_ci(text, len, "hyper"))
        *mods = (u16)(*mods | 16U);
    else if (same_ci(text, len, "meta"))
        *mods = (u16)(*mods | 32U);
    else
        return false;
    return true;
}

static bool named_key(const char *text, size_t len, const char **legacy,
                      unsigned *tilde)
{
    static const struct {
        const char *name;
        const char *legacy;
        unsigned tilde;
    } names[] = {
        {"esc", "\033", 0U}, {"enter", "\r", 0U},
        {"tab", "\t", 0U}, {"space", " ", 0U},
        {"backspace", "\177", 0U}, {"insert", NULL, 2U},
        {"delete", NULL, 3U}, {"left", "\033[D", 0U},
        {"right", "\033[C", 0U}, {"up", "\033[A", 0U},
        {"down", "\033[B", 0U}, {"pageup", NULL, 5U},
        {"pagedown", NULL, 6U}, {"home", "\033[H", 0U},
        {"end", "\033[F", 0U}
    };
    size_t i;

    for (i = 0U; i < YEW_ARRAY_LEN(names); i++) {
        if (same_ci(text, len, names[i].name)) {
            *legacy = names[i].legacy;
            *tilde = names[i].tilde;
            return true;
        }
    }
    return false;
}

static bool encode_key(const char *token, size_t len, KeyStroke *out)
{
    const char *part = token;
    const char *end = token + len;
    const char *plus;
    const char *legacy = NULL;
    unsigned tilde = 0U;
    u16 mods = 0U;
    u8 scalar = 0U;
    int n;

    while ((plus = memchr(part, '+', (size_t)(end - part))) != NULL) {
        if (!modifier(part, (size_t)(plus - part), &mods))
            return false;
        part = plus + 1;
    }
    if (end - part == 1 && (unsigned char)*part >= 0x20U &&
        (unsigned char)*part <= 0x7eU)
        scalar = (u8)*part;
    else if (!named_key(part, (size_t)(end - part), &legacy, &tilde))
        return false;

    if (scalar != 0U) {
        size_t used = 0U;

        if ((mods & 2U) != 0U)
            out->bytes[used++] = 0x1bU;
        if ((mods & 4U) != 0U && isalpha((unsigned char)scalar))
            out->bytes[used++] = (u8)(tolower((unsigned char)scalar) - 'a' + 1);
        else
            out->bytes[used++] = scalar;
        out->len = (u8)used;
        return (mods & ~(u16)(2U | 4U)) == 0U;
    }
    if (tilde != 0U) {
        n = mods == 0U ?
            snprintf((char *)out->bytes, sizeof(out->bytes), "\033[%u~", tilde) :
            snprintf((char *)out->bytes, sizeof(out->bytes), "\033[%u;%u~",
                     tilde, (unsigned)mods + 1U);
    } else if (legacy != NULL && mods == 0U) {
        size_t nbytes = strlen(legacy);

        if (nbytes > sizeof(out->bytes))
            return false;
        (void)memcpy(out->bytes, legacy, nbytes);
        out->len = (u8)nbytes;
        return true;
    } else if (legacy != NULL && strlen(legacy) == 3U &&
               legacy[0] == '\033' && legacy[1] == '[') {
        n = snprintf((char *)out->bytes, sizeof(out->bytes), "\033[1;%u%c",
                     (unsigned)mods + 1U, legacy[2]);
    } else {
        return false;
    }
    if (n <= 0 || (size_t)n >= sizeof(out->bytes))
        return false;
    out->len = (u8)n;
    return true;
}

static bool load_session(const char *path, Session *out)
{
    FILE *fp = fopen(path, "r");
    char *line = NULL;
    size_t cap = 0U;
    unsigned long lineno = 0UL;
    bool ok = true;

    (void)memset(out, 0, sizeof(*out));
    if (fp == NULL) {
        (void)fprintf(stderr, "perf_latency: cannot open %s: %s\n", path,
                      strerror(errno));
        return false;
    }
    while (ok && getline(&line, &cap, fp) >= 0) {
        char *start = line;
        char *end;
        char *p;

        lineno++;
        while (*start != '\0' && isspace((unsigned char)*start))
            start++;
        if (*start == '\0' || *start == '#')
            continue;
        end = start + strlen(start);
        while (end > start && isspace((unsigned char)end[-1]))
            end--;
        for (p = start; p < end; p++) {
            if (isspace((unsigned char)*p)) {
                (void)fprintf(stderr,
                              "perf_latency: %s:%lu has more than one key spelling\n",
                              path, lineno);
                ok = false;
                break;
            }
        }
        if (!ok)
            break;
        if (out->len == SESSION_KEYS) {
            (void)fprintf(stderr, "perf_latency: %s has more than %u keys\n",
                          path, SESSION_KEYS);
            ok = false;
        } else if (!encode_key(start, (size_t)(end - start),
                               &out->keys[out->len])) {
            (void)fprintf(stderr, "perf_latency: %s:%lu unknown key '%.*s'\n",
                          path, lineno, (int)(end - start), start);
            ok = false;
        } else {
            out->len++;
        }
    }
    if (ferror(fp)) {
        (void)fprintf(stderr, "perf_latency: read failed for %s\n", path);
        ok = false;
    }
    free(line);
    (void)fclose(fp);
    if (ok && out->len != SESSION_KEYS) {
        (void)fprintf(stderr, "perf_latency: %s has %zu keys, wanted %u\n",
                      path, out->len, SESSION_KEYS);
        ok = false;
    }
    return ok;
}

static bool scan_frames(const u8 *data, size_t len, size_t *matched,
                        u64 *frames)
{
    static const u8 esu[] = "\033[?2026l";
    size_t i;

    for (i = 0U; i < len; i++) {
        if (data[i] == esu[*matched]) {
            (*matched)++;
            if (*matched == sizeof(esu) - 1U) {
                (*frames)++;
                *matched = 0U;
            }
        } else {
            *matched = data[i] == esu[0] ? 1U : 0U;
        }
    }
    return true;
}

static bool read_frames(YewLivePty *pty, i64 deadline, FrameRead *out)
{
    size_t matched = 0U;

    (void)memset(out, 0, sizeof(*out));
    while (yew_live_pty_now_ns() < deadline) {
        struct pollfd fd = {pty->master, POLLIN | POLLHUP, 0};
        u8 data[8192];
        int result = poll(&fd, 1U, poll_timeout(deadline));
        ssize_t n;

        if (result < 0 && errno == EINTR)
            continue;
        if (result <= 0)
            return result == 0;
        n = read(pty->master, data, sizeof(data));
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            continue;
        if (n <= 0)
            return false;
        yew_live_pty_observe_output(pty, data, (size_t)n);
        (void)scan_frames(data, (size_t)n, &matched, &out->frames);
        if (!out->painted && out->frames != 0U) {
            out->painted = true;
            out->completed_ns = yew_live_pty_now_ns();
            return out->completed_ns >= 0;
        }
    }
    return true;
}

static bool stop_editor(YewLivePty *pty)
{
    static const char quit[] = "\033[27u:q!\r";
    i64 deadline = yew_live_pty_now_ns() + INT64_C(5000000000);
    int code;

    return yew_live_pty_write(pty, quit, sizeof(quit) - 1U, deadline) &&
           yew_live_pty_wait_exit(pty, deadline, &code) && code == 0;
}

static bool send_editor_command(YewLivePty *pty, const char *command)
{
    char wire[1400];
    i64 deadline = yew_live_pty_now_ns() + INT64_C(5000000000);
    int n;

    n = snprintf(wire, sizeof(wire), "\033[27u:%s\r", command);
    return n > 0 && (size_t)n < sizeof(wire) &&
           yew_live_pty_write(pty, wire, (size_t)n, deadline) &&
           yew_live_pty_wait_quiet(pty, INT64_C(100000000), deadline);
}

static const MetricSpec *find_metric(const char *session, const char *fixture)
{
    size_t i;

    for (i = 0U; i < YEW_ARRAY_LEN(metrics); i++) {
        if (strcmp(metrics[i].session, session) == 0 &&
            strcmp(metrics[i].fixture, fixture) == 0)
            return &metrics[i];
    }
    return NULL;
}

static const char *session_name(const char *path, char *out, size_t cap)
{
    const char *base = strrchr(path, '/');
    const char *dot;
    size_t len;

    base = base == NULL ? path : base + 1;
    dot = strrchr(base, '.');
    len = dot != NULL && strcmp(dot, ".keys") == 0 ?
          (size_t)(dot - base) : strlen(base);
    if (len == 0U || len + 1U > cap)
        return NULL;
    (void)memcpy(out, base, len);
    out[len] = '\0';
    return out;
}

static bool regular_file(const char *path)
{
    struct stat st;

    return path != NULL && stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

static bool regular_dir(const char *path)
{
    struct stat st;

    return path != NULL && stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static bool copy_file(const char *from, const char *to)
{
    FILE *in = fopen(from, "rb");
    FILE *out = NULL;
    u8 bytes[8192];
    bool ok = in != NULL;

    if (ok) {
        out = fopen(to, "wb");
        ok = out != NULL;
    }
    while (ok) {
        size_t n = fread(bytes, 1U, sizeof(bytes), in);

        if (n != 0U && fwrite(bytes, 1U, n, out) != n)
            ok = false;
        if (n < sizeof(bytes)) {
            if (ferror(in))
                ok = false;
            break;
        }
    }
    if (out != NULL && fclose(out) != 0)
        ok = false;
    if (in != NULL && fclose(in) != 0)
        ok = false;
    return ok;
}

static bool write_file(const char *path, const char *text, size_t len)
{
    FILE *fp = fopen(path, "wb");
    bool ok = fp != NULL;

    if (ok && fwrite(text, 1U, len, fp) != len)
        ok = false;
    if (fp != NULL && fclose(fp) != 0)
        ok = false;
    return ok;
}

static pid_t start_mock_ai(const char *binary, const char *script, u16 *port)
{
    int output[2];
    pid_t pid;
    FILE *stream;
    unsigned value = 0U;
    bool ready;

    if (!regular_file(binary) || !regular_file(script) || pipe(output) != 0)
        return -1;
    pid = fork();
    if (pid == 0) {
        (void)close(output[0]);
        if (dup2(output[1], STDOUT_FILENO) < 0)
            _exit(126);
        (void)close(output[1]);
        execl(binary, binary, "--port", "0", "--script", script,
              (char *)NULL);
        _exit(127);
    }
    (void)close(output[1]);
    if (pid < 0) {
        (void)close(output[0]);
        return -1;
    }
    stream = fdopen(output[0], "r");
    ready = stream != NULL && fscanf(stream, "port %u", &value) == 1 &&
            value != 0U && value <= 65535U;
    if (stream != NULL && fclose(stream) != 0)
        ready = false;
    if (!ready) {
        (void)kill(pid, SIGKILL);
        while (waitpid(pid, NULL, 0) < 0 && errno == EINTR) {}
        return -1;
    }
    *port = (u16)value;
    return pid;
}

static void stop_mock_ai(pid_t pid)
{
    if (pid <= 0)
        return;
    (void)kill(pid, SIGTERM);
    while (waitpid(pid, NULL, 0) < 0 && errno == EINTR) {}
}

static bool screen_contains(const VtScreen *screen, const char *text)
{
    size_t wanted = strlen(text);
    int row;

    for (row = 0; row < screen->rows; row++) {
        Bytebuf line;
        int col;
        bool found;

        bytebuf_init(&line);
        for (col = 0; col < screen->cols; col++) {
            const VtCell *cell = &screen->cells[
                (size_t)row * (size_t)screen->cols + (size_t)col];
            size_t len = 0U;
            const u8 *bytes = vt_cell_bytes(screen, cell, &len);

            if (cell->w != 0U && bytes != NULL && len != 0U)
                bytebuf_append(&line, bytes, len);
            else if (cell->w != 0U)
                bytebuf_push_u8(&line, (u8)' ');
        }
        found = wanted <= line.len;
        if (found) {
            size_t at;

            found = false;
            for (at = 0U; at + wanted <= line.len; at++) {
                if (memcmp(line.data + at, text, wanted) == 0) {
                    found = true;
                    break;
                }
            }
        }
        bytebuf_free(&line);
        if (found)
            return true;
    }
    return false;
}

static bool remove_tree(const char *path)
{
    struct stat st;

    if (lstat(path, &st) != 0)
        return errno == ENOENT;
    if (S_ISDIR(st.st_mode)) {
        DIR *dir = opendir(path);
        struct dirent *entry;

        if (dir == NULL)
            return false;
        while ((entry = readdir(dir)) != NULL) {
            char child[1200];
            int n;

            if (strcmp(entry->d_name, ".") == 0 ||
                strcmp(entry->d_name, "..") == 0)
                continue;
            n = snprintf(child, sizeof(child), "%s/%s", path,
                         entry->d_name);
            if (n <= 0 || (size_t)n >= sizeof(child) ||
                !remove_tree(child)) {
                (void)closedir(dir);
                return false;
            }
        }
        if (closedir(dir) != 0)
            return false;
        return rmdir(path) == 0;
    }
    return unlink(path) == 0;
}

static bool make_run_state(const char *parent, char *out, size_t cap)
{
    int n;

    if (!regular_dir(parent))
        return false;
    n = snprintf(out, cap, "%s/latency-XXXXXX", parent);
    return n > 0 && (size_t)n < cap && mkdtemp(out) != NULL;
}

static bool make_run_workspace(char *out, size_t cap)
{
    static const char pattern[] = "/tmp/yew-perf-workspace-XXXXXX";

    if (sizeof(pattern) > cap)
        return false;
    (void)memcpy(out, pattern, sizeof(pattern));
    return mkdtemp(out) != NULL;
}

static bool make_hermetic_config(const char *workspace, char *out,
                                 size_t cap)
{
    static const char source[] = "let lsp = {servers: {}}\n";
    int n = snprintf(out, cap, "%s/perf-no-lsp.fl", workspace);

    return n > 0 && (size_t)n < cap &&
           write_file(out, source, sizeof(source) - 1U);
}

static bool spawn_many_buffers(YewLivePty *pty, const char *binary,
                               const char *dir, const char *state,
                               const char *workspace)
{
    char paths[MANY_BUFFER_COUNT][1024];
    char config[1024];
    char *argv[MANY_BUFFER_COUNT + 7U];
    size_t i;

    if (!regular_dir(dir) || !regular_dir(workspace) ||
        !make_hermetic_config(workspace, config, sizeof(config)))
        return false;
    argv[0] = (char *)binary;
    argv[1] = (char *)"--workspace";
    argv[2] = (char *)workspace;
    argv[3] = (char *)"--config";
    argv[4] = config;
    argv[5] = (char *)"--no-workspace-config";
    for (i = 0U; i < MANY_BUFFER_COUNT; i++) {
        int n = snprintf(paths[i], sizeof(paths[i]), "%s/buf-%02zu.c", dir,
                         i);

        if (n <= 0 || (size_t)n >= sizeof(paths[i]) ||
            !regular_file(paths[i]))
            return false;
        argv[i + 6U] = paths[i];
    }
    argv[MANY_BUFFER_COUNT + 6U] = NULL;
    return yew_live_pty_spawn_argv(pty, binary, argv, state, SCREEN_ROWS,
                                   SCREEN_COLS);
}

static bool spawn_file(YewLivePty *pty, const char *binary,
                       const char *path, const char *state,
                       const char *workspace)
{
    char config[1024];
    char *argv[8];

    if (!regular_dir(workspace) ||
        !make_hermetic_config(workspace, config, sizeof(config)))
        return false;
    argv[0] = (char *)binary;
    argv[1] = (char *)"--workspace";
    argv[2] = (char *)workspace;
    argv[3] = (char *)"--config";
    argv[4] = config;
    argv[5] = (char *)"--no-workspace-config";
    argv[6] = (char *)path;
    argv[7] = NULL;
    return yew_live_pty_spawn_argv(pty, binary, argv, state, SCREEN_ROWS,
                                   SCREEN_COLS);
}

static bool spawn_assist(YewLivePty *pty, AssistRun *assist,
                         const char *binary, const char *path,
                         const char *state, const char *workspace,
                         const char *fakelsp, const char *mockai,
                         const char *ai_script)
{
    char git_dir[1024];
    char source_path[1024];
    char config_path[1024];
    char config[4096];
    char *argv[8];
    u16 port = 0U;
    int n;

    if (!regular_file(fakelsp) || !regular_file(mockai) ||
        !regular_file(ai_script))
        return false;
    n = snprintf(git_dir, sizeof(git_dir), "%s/.git", workspace);
    if (n <= 0 || (size_t)n >= sizeof(git_dir) ||
        (mkdir(git_dir, 0700) != 0 && errno != EEXIST))
        return false;
    n = snprintf(source_path, sizeof(source_path), "%s/assist.c", workspace);
    if (n <= 0 || (size_t)n >= sizeof(source_path) ||
        !copy_file(path, source_path))
        return false;
    n = snprintf(config_path, sizeof(config_path), "%s/assist.fl", workspace);
    if (n <= 0 || (size_t)n >= sizeof(config_path))
        return false;
    assist->ai_pid = start_mock_ai(mockai, ai_script, &port);
    if (assist->ai_pid <= 0)
        return false;
    n = snprintf(config, sizeof(config),
        "import ai\n"
        "let lsp = {servers: {c: {id: \"fakelsp\", cmd: \"%s\", "
        "args: [\"session-nosync\"], roots: [\".git\"], "
        "init_options: nil, init_timeout_ms: 2000}}}\n"
        "ai.backend(\"local\", {kind: \"ollama\", transport: \"http\", "
        "url: \"http://127.0.0.1:%u\", model: \"perf-model\"})\n"
        "set({\"ai.enable\": true, \"ai.backend\": \"local\", "
        "\"ai.default_workspace\": \"allow\", \"ai.frame_ms\": 0, "
        "\"shadow.ai_debounce_ms\": 50, \"shadow.providers\": \"ai\"})\n",
        fakelsp, (unsigned)port);
    if (n <= 0 || (size_t)n >= sizeof(config) ||
        !write_file(config_path, config, (size_t)n))
        goto fail;
    argv[0] = (char *)binary;
    argv[1] = (char *)"--workspace";
    argv[2] = (char *)workspace;
    argv[3] = (char *)"--config";
    argv[4] = config_path;
    argv[5] = (char *)"--no-workspace-config";
    argv[6] = source_path;
    argv[7] = NULL;
    if (!yew_live_pty_spawn_argv(pty, binary, argv, state, SCREEN_ROWS,
                                  SCREEN_COLS))
        goto fail;
    vt_init(&assist->screen, SCREEN_ROWS, SCREEN_COLS);
    assist->screen_on = true;
    yew_live_pty_set_output(pty, screen_feed, &assist->screen);
    return true;

fail:
    stop_mock_ai(assist->ai_pid);
    assist->ai_pid = -1;
    return false;
}

static bool assist_ghost_preflight(YewLivePty *pty, AssistRun *assist)
{
    static const char trigger[] = "\033[F" "aX";
    static const char restore[] = "\033\033";
    i64 deadline = yew_live_pty_now_ns() + INT64_C(5000000000);
    size_t tries;

    if (!yew_live_pty_write(pty, trigger, sizeof(trigger) - 1U, deadline))
        return false;
    for (tries = 0U; tries < 64U &&
                       !screen_contains(&assist->screen,
                                        "int answer = 42;"); tries++) {
        u64 before = pty->frames;

        if (!yew_live_pty_wait_frame(pty, before, deadline, NULL))
            return false;
    }
    if (!screen_contains(&assist->screen, "int answer = 42;") ||
        kill(assist->ai_pid, 0) != 0)
        return false;
    if (!yew_live_pty_write(pty, restore, sizeof(restore) - 1U, deadline) ||
        !yew_live_pty_wait_quiet(pty, INT64_C(100000000), deadline))
        return false;
    (void)puts("latency.typing.assist.ghost_visible 1 lsp=fakelsp ai=mockai");
    return true;
}

static bool hydrate_many_buffers(YewLivePty *pty)
{
    static const char next[] = "tn";
    size_t i;

    for (i = 1U; i < MANY_BUFFER_HYDRATED; i++) {
        i64 deadline = yew_live_pty_now_ns() + INT64_C(5000000000);
        u64 before = pty->frames;

        if (!yew_live_pty_write(pty, next, sizeof(next) - 1U, deadline) ||
            !yew_live_pty_wait_frame(pty, before, deadline, NULL) ||
            !yew_live_pty_wait_quiet(pty, INT64_C(1000000), deadline))
            return false;
    }
    /* Opening a buffer can schedule syntax, Git, and symbol work after the
     * navigation frame.  Measurement must start from a genuinely quiet
     * editor; otherwise a deferred startup redraw can be charged to the
     * first measured key and violate the frames <= keys invariant. */
    return yew_live_pty_wait_quiet(
        pty, INT64_C(100000000),
        yew_live_pty_now_ns() + INT64_C(5000000000));
}

static int run_session(const char *binary, const char *script,
                       const char *fixture, const char *path,
                       const char *state, const char *many_dir,
                       const char *fakelsp, const char *mockai,
                       const char *ai_script, const char *prof_dump)
{
    Session session;
    YewLivePty pty;
    char run_state[1024];
    char run_workspace[1024] = "";
    i64 samples[SESSION_KEYS];
    size_t nsamples = 0U;
    size_t no_paint = 0U;
    u64 frames = 0U;
    char name[64];
    const MetricSpec *spec;
    size_t i;
    bool gate;
    bool is_assist;
    AssistRun assist;
    int status = 0;

    (void)memset(&assist, 0, sizeof(assist));
    assist.ai_pid = -1;

    if (session_name(script, name, sizeof(name)) == NULL ||
        (spec = find_metric(name, fixture)) == NULL) {
        (void)fprintf(stderr,
                      "perf_latency: unsupported session/fixture mapping %s/%s\n",
                      script, fixture);
        return 2;
    }
    if (!spec->supported) {
        (void)fprintf(stderr, "perf_latency: unsupported %s: %s\n",
                      spec->metric, spec->why);
        return 2;
    }
    if (!regular_file(binary) ||
        (strcmp(fixture, "many-buffers") == 0 ? !regular_dir(many_dir) :
                                                !regular_file(path)) ||
        !load_session(script, &session)) {
        (void)fprintf(stderr, "perf_latency: invalid binary, fixture, or session\n");
        return 2;
    }
    is_assist = strcmp(fixture, "assist") == 0;
    if (is_assist && (fakelsp == NULL || mockai == NULL ||
                      ai_script == NULL)) {
        (void)fprintf(stderr,
                      "perf_latency: assist requires fakelsp, mockai, and script\n");
        return 2;
    }
    if (!make_run_state(state, run_state, sizeof(run_state))) {
        (void)fprintf(stderr, "perf_latency: cannot isolate run state\n");
        return 2;
    }
    if (!make_run_workspace(run_workspace, sizeof(run_workspace))) {
        (void)fprintf(stderr, "perf_latency: cannot isolate workspace\n");
        (void)remove_tree(run_state);
        return 2;
    }
    if (!(is_assist ?
          spawn_assist(&pty, &assist, binary, path, run_state,
                       run_workspace, fakelsp, mockai, ai_script) :
          strcmp(fixture, "many-buffers") == 0 ?
          spawn_many_buffers(&pty, binary, many_dir, run_state,
                             run_workspace) :
          spawn_file(&pty, binary, path, run_state, run_workspace))) {
        (void)fprintf(stderr, "perf_latency: cannot spawn editor\n");
        stop_mock_ai(assist.ai_pid);
        if (run_workspace[0] != '\0')
            (void)remove_tree(run_workspace);
        (void)remove_tree(run_state);
        return 2;
    }
    if (!yew_live_pty_wait_frame(&pty, 0U,
            yew_live_pty_now_ns() + INT64_C(5000000000), NULL) ||
        !yew_live_pty_wait_quiet(&pty, INT64_C(100000000),
            yew_live_pty_now_ns() + INT64_C(2000000000))) {
        (void)fprintf(stderr, "perf_latency: editor did not settle\n");
        yew_live_pty_close(&pty);
        if (run_workspace[0] != '\0')
            (void)remove_tree(run_workspace);
        (void)remove_tree(run_state);
        return 2;
    }
    if (strcmp(fixture, "many-buffers") == 0 &&
        !hydrate_many_buffers(&pty)) {
        (void)fprintf(stderr,
                      "perf_latency: could not hydrate 20 of 50 buffers\n");
        yew_live_pty_close(&pty);
        if (run_workspace[0] != '\0')
            (void)remove_tree(run_workspace);
        (void)remove_tree(run_state);
        return 2;
    }
    if (is_assist && !assist_ghost_preflight(&pty, &assist)) {
        (void)fprintf(stderr,
                      "perf_latency: assist mocks did not paint a live ghost\n");
        yew_live_pty_close(&pty);
        stop_mock_ai(assist.ai_pid);
        if (assist.screen_on)
            vt_free(&assist.screen);
        (void)remove_tree(run_workspace);
        (void)remove_tree(run_state);
        return 2;
    }
    if (prof_dump != NULL &&
        (!send_editor_command(&pty, "prof reset") ||
         /* Opening a second command clears reset's four-second info
          * message and cancels its timer.  Otherwise that unrelated paint
          * can satisfy a key wait mid-session and merge the displaced key
          * into the following profiler frame.  nop itself is one multi-key
          * command frame and is excluded from KEYPAINT. */
         !send_editor_command(&pty, "nop") ||
         !yew_live_pty_wait_quiet(
             &pty, INT64_C(500000000),
             yew_live_pty_now_ns() + INT64_C(5000000000)))) {
        (void)fprintf(stderr, "perf_latency: cannot reset profiler\n");
        yew_live_pty_close(&pty);
        stop_mock_ai(assist.ai_pid);
        if (assist.screen_on)
            vt_free(&assist.screen);
        (void)remove_tree(run_workspace);
        (void)remove_tree(run_state);
        return 2;
    }
    for (i = 0U; i < session.len; i++) {
        i64 start = yew_live_pty_now_ns();
        i64 deadline = start + (i64)KEY_TIMEOUT_MS * INT64_C(1000000);
        FrameRead read;

        if (start < 0 || !yew_live_pty_write(&pty, session.keys[i].bytes,
                                              session.keys[i].len, deadline) ||
            !read_frames(&pty, deadline, &read)) {
            (void)fprintf(stderr, "perf_latency: key %zu transport failed\n",
                          i + 1U);
            status = 2;
            break;
        }
        frames += read.frames;
        if (read.painted)
            samples[nsamples++] = read.completed_ns - start;
        else
            no_paint++;
    }
    if (prof_dump != NULL && status == 0) {
        char command[1200];
        int n = snprintf(command, sizeof(command), "prof dump %s", prof_dump);

        if (n <= 0 || (size_t)n >= sizeof(command) ||
            !send_editor_command(&pty, command) || !regular_file(prof_dump)) {
            (void)fprintf(stderr, "perf_latency: cannot dump profiler\n");
            status = 2;
        }
    }
    if (!stop_editor(&pty) && status == 0)
        status = 2;
    yew_live_pty_close(&pty);
    if (is_assist && kill(assist.ai_pid, 0) != 0 && status == 0) {
        (void)fprintf(stderr, "perf_latency: mock AI exited during session\n");
        status = 2;
    }
    stop_mock_ai(assist.ai_pid);
    if (assist.screen_on)
        vt_free(&assist.screen);
    if (!remove_tree(run_state) && status == 0) {
        (void)fprintf(stderr, "perf_latency: cannot remove isolated state\n");
        status = 2;
    }
    if (run_workspace[0] != '\0' && !remove_tree(run_workspace) &&
        status == 0) {
        (void)fprintf(stderr,
                      "perf_latency: cannot remove isolated workspace\n");
        status = 2;
    }
    if (status != 0)
        return status;
    if (nsamples == 0U) {
        (void)fprintf(stderr, "perf_latency: session produced no painted frames\n");
        return 1;
    }
    if (!sort_i64(samples, nsamples)) {
        (void)fprintf(stderr, "perf_latency: cannot sort samples\n");
        return 2;
    }
    gate = getenv("PERF_GATE") != NULL &&
           strcmp(getenv("PERF_GATE"), "1") == 0 &&
           !(getenv("YEW_PERF_ADVISORY") != NULL &&
             strcmp(getenv("YEW_PERF_ADVISORY"), "0") != 0);
    {
        i64 p50 = samples[(nsamples - 1U) * 50U / 100U];
        i64 p99 = samples[(nsamples - 1U) * 99U / 100U];
        i64 max = samples[nsamples - 1U];
        u64 no_paint_pm = (u64)no_paint * 1000U / session.len;
        bool over = p99 > INT64_C(5000000);

        (void)printf("%s %lld ns %s\n", spec->metric, (long long)p99,
                     over ? (gate ? "FAIL" : "ADVISORY") : "OK");
        int base_len = (int)(strlen(spec->metric) - strlen(".p99"));

        (void)printf("%.*s.p50 %lld ns\n", base_len, spec->metric,
                     (long long)p50);
        (void)printf("%.*s.max %lld ns\n", base_len, spec->metric,
                     (long long)max);
        (void)printf("%.*s.no_paint %zu permille=%llu\n", base_len,
                     spec->metric,
                     no_paint, (unsigned long long)no_paint_pm);
        (void)printf("%.*s.frames %llu keys=%zu\n", base_len, spec->metric,
                     (unsigned long long)frames, session.len);
        if ((over && gate) || p99 <= 0 || p99 > INT64_C(500000000) ||
            frames > session.len)
            status = 1;
    }
    return status;
}

static bool spawn_echo(YewLivePty *pty, const char *binary)
{
    char slave[128];
    pid_t pid;

    if (!yew_live_pty_open(pty, slave, sizeof(slave), 24U, 80U))
        return false;
    pid = fork();
    if (pid < 0) {
        yew_live_pty_close(pty);
        return false;
    }
    if (pid == 0) {
        if (!yew_live_pty_attach(pty, slave, 24U, 80U))
            _exit(126);
        execl(binary, binary, (char *)NULL);
        _exit(126);
    }
    pty->pid = pid;
    return true;
}

static int run_floor(const char *echo)
{
    YewLivePty pty;
    i64 samples[FLOOR_SAMPLES];
    size_t i;
    int code;

    if (!regular_file(echo) || !spawn_echo(&pty, echo)) {
        (void)fprintf(stderr, "perf_latency: cannot spawn echo child\n");
        return 2;
    }
    if (!yew_live_pty_wait_frame(&pty, 0U,
            yew_live_pty_now_ns() + INT64_C(1000000000), NULL)) {
        (void)fprintf(stderr, "perf_latency: echo child did not become ready\n");
        yew_live_pty_close(&pty);
        return 2;
    }
    for (i = 0U; i < FLOOR_SAMPLES; i++) {
        const u8 key = 'x';
        i64 start = yew_live_pty_now_ns();
        i64 deadline = start + INT64_C(500000000);
        u64 before = pty.frames;
        i64 completed;

        if (start < 0 || !yew_live_pty_write(&pty, &key, 1U, deadline) ||
            !yew_live_pty_wait_frame(&pty, before, deadline, &completed)) {
            (void)fprintf(stderr, "perf_latency: floor sample %zu failed\n", i);
            yew_live_pty_close(&pty);
            return 2;
        }
        samples[i] = completed - start;
    }
    (void)kill(pty.pid, SIGTERM);
    if (!yew_live_pty_wait_exit(&pty,
            yew_live_pty_now_ns() + INT64_C(1000000000), &code)) {
        yew_live_pty_close(&pty);
        return 2;
    }
    yew_live_pty_close(&pty);
    if (!sort_i64(samples, FLOOR_SAMPLES))
        return 2;
    (void)printf("pty_floor_p50 %lld ns\n",
                 (long long)samples[(FLOOR_SAMPLES - 1U) / 2U]);
    return samples[(FLOOR_SAMPLES - 1U) / 2U] < INT64_C(2000) ||
           samples[(FLOOR_SAMPLES - 1U) / 2U] > INT64_C(500000) ? 1 : 0;
}

static int check_scripts(const char *dir)
{
    static const char *names[] = {
        "typing.keys", "navigate.keys", "edit.keys", "multicursor.keys",
        "search.keys"
    };
    size_t i;

    for (i = 0U; i < YEW_ARRAY_LEN(names); i++) {
        char path[1024];
        Session session;
        int n = snprintf(path, sizeof(path), "%s/%s", dir, names[i]);

        if (n <= 0 || (size_t)n >= sizeof(path) ||
            !load_session(path, &session))
            return 1;
        (void)printf("session.%s %zu\n", names[i], session.len);
    }
    return 0;
}

static int check_assist_vt(void)
{
    static const u8 first[] = "\033[?1049h\033[Hghost ";
    static const u8 second[] = "visible";
    YewLivePty pty;
    VtScreen screen;
    bool ok;

    (void)memset(&pty, 0, sizeof(pty));
    vt_init(&screen, 4, 32);
    yew_live_pty_set_output(&pty, screen_feed, &screen);
    yew_live_pty_observe_output(&pty, first, sizeof(first) - 1U);
    yew_live_pty_observe_output(&pty, second, sizeof(second) - 1U);
    ok = screen.nerrors == 0U && screen_contains(&screen, "ghost visible");
    vt_free(&screen);
    (void)printf("assist_vt_observer %s\n", ok ? "OK" : "FAIL");
    return ok ? 0 : 1;
}

static void usage(const char *arg0)
{
    (void)fprintf(stderr,
        "usage:\n"
        "  %s --floor --echo PATH\n"
        "  %s --check-scripts DIR\n"
        "  %s --check-assist-vt\n"
        "  %s --yew PATH --session FILE --fixture CLASS --path FILE "
        "[--state DIR] [--many-dir DIR] [--fakelsp PATH --mockai PATH "
        "--ai-script PATH] [--prof-dump PATH]\n", arg0, arg0, arg0, arg0);
}

int main(int argc, char **argv)
{
    const char *yew = NULL;
    const char *script = NULL;
    const char *fixture = NULL;
    const char *path = NULL;
    const char *state = "/tmp";
    const char *echo = NULL;
    const char *check = NULL;
    const char *many_dir = NULL;
    const char *fakelsp = NULL;
    const char *mockai = NULL;
    const char *ai_script = NULL;
    const char *prof_dump = NULL;
    bool floor = false;
    bool assist_vt = false;
    int i;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--floor") == 0)
            floor = true;
        else if (strcmp(argv[i], "--check-assist-vt") == 0)
            assist_vt = true;
        else if (i + 1 < argc && strcmp(argv[i], "--echo") == 0)
            echo = argv[++i];
        else if (i + 1 < argc && strcmp(argv[i], "--check-scripts") == 0)
            check = argv[++i];
        else if (i + 1 < argc && strcmp(argv[i], "--yew") == 0)
            yew = argv[++i];
        else if (i + 1 < argc && strcmp(argv[i], "--session") == 0)
            script = argv[++i];
        else if (i + 1 < argc && strcmp(argv[i], "--fixture") == 0)
            fixture = argv[++i];
        else if (i + 1 < argc && strcmp(argv[i], "--path") == 0)
            path = argv[++i];
        else if (i + 1 < argc && strcmp(argv[i], "--state") == 0)
            state = argv[++i];
        else if (i + 1 < argc && strcmp(argv[i], "--many-dir") == 0)
            many_dir = argv[++i];
        else if (i + 1 < argc && strcmp(argv[i], "--fakelsp") == 0)
            fakelsp = argv[++i];
        else if (i + 1 < argc && strcmp(argv[i], "--mockai") == 0)
            mockai = argv[++i];
        else if (i + 1 < argc && strcmp(argv[i], "--ai-script") == 0)
            ai_script = argv[++i];
        else if (i + 1 < argc && strcmp(argv[i], "--prof-dump") == 0)
            prof_dump = argv[++i];
        else {
            usage(argv[0]);
            return 2;
        }
    }
    if (assist_vt && !floor && check == NULL && yew == NULL)
        return check_assist_vt();
    if (check != NULL && !floor && !assist_vt && yew == NULL)
        return check_scripts(check);
    if (floor && echo != NULL && check == NULL && !assist_vt && yew == NULL)
        return run_floor(echo);
    if (!floor && !assist_vt && check == NULL && yew != NULL && script != NULL &&
        fixture != NULL && path != NULL)
        return run_session(yew, script, fixture, path, state, many_dir,
                           fakelsp, mockai, ai_script, prof_dump);
    usage(argv[0]);
    return 2;
}
