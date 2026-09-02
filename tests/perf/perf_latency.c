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
#include <time.h>
#include <unistd.h>

enum {
    SESSION_KEYS = 10000,
    SCREEN_ROWS = 50,
    SCREEN_COLS = 200,
    /* This is a hang ceiling, not a latency allowance.  Shared runners can
     * deschedule both processes for longer than the old 600 ms deadline;
     * wait long enough to observe the frame, then let BROKEN_RUN_NS reject
     * it as latency instead of mislabeling scheduler contention as a PTY
     * transport failure. */
    KEY_TIMEOUT_MS = 5000,
    CONTROL_TIMEOUT_MS = 5000,
    /* Serializing and atomically writing a full profiler ring is control
     * traffic after measurement.  It must tolerate a descheduled hosted
     * runner without weakening any key-to-paint latency verdict. */
    PROF_DUMP_TIMEOUT_MS = 30000,
    ADVISORY_ATTEMPTS = 3,
    FLOOR_SAMPLES = 1001,
    MANY_BUFFER_COUNT = 50,
    MANY_BUFFER_HYDRATED = 20
};

#define BROKEN_RUN_NS INT64_C(500000000)

_Static_assert((i64)KEY_TIMEOUT_MS * INT64_C(1000000) > BROKEN_RUN_NS,
               "PTY hang ceiling must outlive the broken-run latency gate");

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
    bool no_paint;
    i64 completed_ns;
    u64 frames;
} FrameRead;

typedef enum FrameScanResult {
    FRAME_SCAN_MORE,
    FRAME_SCAN_PAINT,
    FRAME_SCAN_NO_PAINT,
    FRAME_SCAN_INVALID
} FrameScanResult;

typedef struct FrameScan {
    size_t frame_matched;
    size_t tag_matched;
    u32 tag_keys;
    u32 tag_visible;
    u16 frame_keys;
    u8 tag_field;
    bool tag_digit;
    bool frame_tagged;
} FrameScan;

typedef struct AssistRun {
    VtScreen screen;
    pid_t ai_pid;
    bool screen_on;
} AssistRun;

typedef struct DelayedObserver {
    i64 started_ns;
    i64 finished_ns;
} DelayedObserver;

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

static void delayed_observer(void *ctx, const u8 *bytes, size_t len)
{
    DelayedObserver *observer = ctx;
    struct timespec delay = {0, 20000000L};
    struct timespec remaining;

    (void)bytes;
    (void)len;
    observer->started_ns = yew_live_pty_now_ns();
    while (nanosleep(&delay, &remaining) != 0 && errno == EINTR)
        delay = remaining;
    observer->finished_ns = yew_live_pty_now_ns();
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

static bool named_key(const char *text, size_t len, u32 *kitty,
                      const char **legacy, unsigned *tilde)
{
    static const struct {
        const char *name;
        u32 kitty;
        const char *legacy;
        unsigned tilde;
    } names[] = {
        {"esc", 27U, "\033", 0U}, {"enter", 13U, "\r", 0U},
        {"tab", 9U, "\t", 0U}, {"space", 32U, " ", 0U},
        {"backspace", 127U, "\177", 0U},
        {"insert", 57348U, NULL, 2U}, {"delete", 57349U, NULL, 3U},
        {"left", 57350U, "\033[D", 0U},
        {"right", 57351U, "\033[C", 0U},
        {"up", 57352U, "\033[A", 0U},
        {"down", 57353U, "\033[B", 0U},
        {"pageup", 57354U, NULL, 5U},
        {"pagedown", 57355U, NULL, 6U},
        {"home", 57356U, "\033[H", 0U},
        {"end", 57357U, "\033[F", 0U}
    };
    size_t i;

    for (i = 0U; i < YEW_ARRAY_LEN(names); i++) {
        if (same_ci(text, len, names[i].name)) {
            *kitty = names[i].kitty;
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
    u32 kitty = 0U;
    u8 scalar = 0U;
    int n;

    while ((plus = memchr(part, '+', (size_t)(end - part))) != NULL) {
        if (!modifier(part, (size_t)(plus - part), &mods))
            return false;
        part = plus + 1;
    }
    if (end - part == 1 && (unsigned char)*part >= 0x20U &&
        (unsigned char)*part <= 0x7eU) {
        scalar = (u8)*part;
        kitty = scalar;
    } else if (!named_key(part, (size_t)(end - part), &kitty, &legacy,
                          &tilde)) {
        return false;
    }

    /* Kitty flag 21 reports printable keys and the disambiguated controls as
     * CSI u, while arrows and navigation retain their legacy CSI forms. */
    if (scalar != 0U || kitty == 32U || kitty == 27U || kitty == 13U ||
        kitty == 9U || kitty == 127U || (legacy == NULL && tilde == 0U)) {
        n = snprintf((char *)out->bytes, sizeof(out->bytes), "\033[%u;%uu",
                     (unsigned)kitty, (unsigned)mods + 1U);
    } else if (tilde != 0U) {
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

static bool encoding_is(const char *token, const char *expected)
{
    KeyStroke key = {{0}, 0U};
    size_t len = strlen(token);
    size_t expected_len = strlen(expected);

    return encode_key(token, len, &key) && key.len == expected_len &&
           memcmp(key.bytes, expected, expected_len) == 0;
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

static bool tag_value_push(u32 *value, u8 byte)
{
    u32 digit;

    if (byte < (u8)'0' || byte > (u8)'9')
        return false;
    digit = (u32)(byte - (u8)'0');
    if (*value > (UINT32_MAX - digit) / 10U)
        return false;
    *value = *value * 10U + digit;
    return true;
}

static FrameScanResult scan_frame_bytes(FrameScan *scan, const u8 *data,
                                        size_t len, FrameRead *out)
{
    static const u8 frame_end[] = "\033[?2026l";
    static const u8 tag[] = "\033]777;yew-key;";
    FrameScanResult result = FRAME_SCAN_MORE;
    size_t i;

    for (i = 0U; i < len; i++) {
        u8 byte = data[i];

        if (scan->tag_field == 0U) {
            if (byte == tag[scan->tag_matched]) {
                scan->tag_matched++;
                if (scan->tag_matched == sizeof(tag) - 1U) {
                    scan->tag_matched = 0U;
                    scan->tag_field = 1U;
                    scan->tag_keys = 0U;
                    scan->tag_visible = 0U;
                    scan->tag_digit = false;
                }
            } else {
                scan->tag_matched = byte == tag[0] ? 1U : 0U;
            }
        } else if (scan->tag_field == 1U) {
            if (byte == (u8)';' && scan->tag_digit &&
                scan->tag_keys <= UINT16_MAX) {
                scan->tag_field = 2U;
                scan->tag_digit = false;
            } else if (tag_value_push(&scan->tag_keys, byte)) {
                scan->tag_digit = true;
            } else {
                return FRAME_SCAN_INVALID;
            }
        } else if (byte == (u8)'\a' && scan->tag_digit) {
            scan->tag_field = 0U;
            scan->frame_keys = (u16)scan->tag_keys;
            scan->frame_tagged = scan->tag_visible != 0U;
            if (scan->frame_keys != 0U && scan->tag_visible == 0U) {
                if (result == FRAME_SCAN_MORE)
                    result = FRAME_SCAN_NO_PAINT;
                scan->frame_keys = 0U;
            }
        } else if (tag_value_push(&scan->tag_visible, byte)) {
            scan->tag_digit = true;
        } else {
            return FRAME_SCAN_INVALID;
        }

        if (byte == frame_end[scan->frame_matched]) {
            scan->frame_matched++;
            if (scan->frame_matched == sizeof(frame_end) - 1U) {
                out->frames++;
                scan->frame_matched = 0U;
                if (scan->frame_tagged) {
                    bool key_frame = scan->frame_keys != 0U;

                    scan->frame_tagged = false;
                    scan->frame_keys = 0U;
                    if (key_frame && result == FRAME_SCAN_MORE)
                        result = FRAME_SCAN_PAINT;
                }
            }
        } else {
            scan->frame_matched = byte == frame_end[0] ? 1U : 0U;
        }
    }
    return result;
}

static bool read_frames(YewLivePty *pty, i64 deadline, FrameRead *out)
{
    FrameScan scan = {0};

    (void)memset(out, 0, sizeof(*out));
    while (yew_live_pty_now_ns() < deadline) {
        struct pollfd fd = {pty->master, POLLIN | POLLHUP, 0};
        u8 data[8192];
        int result = poll(&fd, 1U, poll_timeout(deadline));
        i64 read_ns;
        ssize_t n;

        if (result < 0 && errno == EINTR)
            continue;
        if (result <= 0)
            return false;
        n = read(pty->master, data, sizeof(data));
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            continue;
        if (n <= 0)
            return false;
        read_ns = yew_live_pty_now_ns();
        if (read_ns < 0)
            return false;
        yew_live_pty_observe_output(pty, data, (size_t)n);
        {
            FrameScanResult scan_result =
                scan_frame_bytes(&scan, data, (size_t)n, out);

            if (scan_result == FRAME_SCAN_INVALID)
                return false;
            if (scan_result == FRAME_SCAN_MORE)
                continue;
            out->painted = scan_result == FRAME_SCAN_PAINT;
            out->no_paint = scan_result == FRAME_SCAN_NO_PAINT;
            out->completed_ns = read_ns;
            return true;
        }
    }
    return false;
}

static bool stop_editor(YewLivePty *pty)
{
    static const char quit[] = "\033[27u:q!\r";
    i64 deadline = yew_live_pty_now_ns() + INT64_C(5000000000);
    int code;

    return yew_live_pty_write(pty, quit, sizeof(quit) - 1U, deadline) &&
           yew_live_pty_wait_exit(pty, deadline, &code) && code == 0;
}

static bool send_editor_command(YewLivePty *pty, const char *command,
                                int timeout_ms)
{
    char wire[1400];
    i64 deadline = yew_live_pty_now_ns() +
                   (i64)timeout_ms * INT64_C(1000000);
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
    const char *tmp = getenv("TMPDIR");
    int n;

    if (tmp == NULL || tmp[0] == '\0')
        tmp = "/tmp";
    n = snprintf(out, cap, "%s/yew-perf-workspace-XXXXXX", tmp);
    return n > 0 && (size_t)n < cap && mkdtemp(out) != NULL;
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
    if (setenv("YEW_PERF_FRAME_TAGS", "1", 1) != 0) {
        (void)fprintf(stderr, "perf_latency: cannot enable frame tags\n");
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
    /* The child has not been pumped yet, so this opt-in is set before the
     * terminal capability probe can be observed and answered. */
    pty.kitty_supported = true;
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
    if (!pty.kitty_enabled) {
        (void)fprintf(stderr,
                      "perf_latency: editor did not enable Kitty input\n");
        yew_live_pty_close(&pty);
        stop_mock_ai(assist.ai_pid);
        if (assist.screen_on)
            vt_free(&assist.screen);
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
        (!send_editor_command(&pty, "prof reset", CONTROL_TIMEOUT_MS) ||
         /* Opening a second command clears reset's four-second info
          * message and cancels its timer.  Otherwise that unrelated paint
          * can satisfy a key wait mid-session and merge the displaced key
          * into the following profiler frame.  nop itself is one multi-key
          * command frame and is excluded from KEYPAINT. */
         !send_editor_command(&pty, "nop", CONTROL_TIMEOUT_MS) ||
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
        if (read.painted) {
            i64 elapsed = read.completed_ns - start;

            samples[nsamples++] = elapsed;
        }
        else if (read.no_paint)
            no_paint++;
        else {
            (void)fprintf(stderr,
                          "perf_latency: key %zu had no terminal frame result\n",
                          i + 1U);
            status = 2;
            break;
        }
    }
    if (prof_dump != NULL && status == 0) {
        char command[1200];
        int n = snprintf(command, sizeof(command), "prof dump %s", prof_dump);

        if (n <= 0 || (size_t)n >= sizeof(command)) {
            (void)fprintf(stderr,
                          "perf_latency: cannot format profiler dump command\n");
            status = 2;
        } else if (!send_editor_command(&pty, command,
                                        PROF_DUMP_TIMEOUT_MS)) {
            (void)fprintf(stderr,
                          "perf_latency: profiler dump command timed out\n");
            status = 2;
        } else if (!regular_file(prof_dump)) {
            (void)fprintf(stderr,
                          "perf_latency: profiler dump file is missing\n");
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
        if ((over && gate) || p99 <= 0 || p99 > BROKEN_RUN_NS ||
            frames > session.len)
            status = 1;
    }
    return status;
}

static bool perf_advisory(void)
{
    const char *value = getenv("YEW_PERF_ADVISORY");

    return value != NULL && strcmp(value, "0") != 0;
}

static bool retry_transport(bool advisory, bool single_attempt, int status,
                            unsigned attempt)
{
    return advisory && !single_attempt && status == 2 &&
           attempt < ADVISORY_ATTEMPTS;
}

static int run_session_with_retry(const char *binary, const char *script,
                                  const char *fixture, const char *path,
                                  const char *state, const char *many_dir,
                                  const char *fakelsp, const char *mockai,
                                  const char *ai_script,
                                  const char *prof_dump,
                                  bool single_attempt)
{
    bool advisory = perf_advisory();
    unsigned attempt;

    for (attempt = 1U; ; attempt++) {
        int status = run_session(binary, script, fixture, path, state,
                                 many_dir, fakelsp, mockai, ai_script,
                                 prof_dump);

        if (!retry_transport(advisory, single_attempt, status, attempt))
            return status;
        (void)fprintf(stderr,
                      "perf_latency: retrying %s/%s after transport failure "
                      "(attempt %u/%u)\n",
                      script, fixture, attempt, ADVISORY_ATTEMPTS);
    }
}

static int check_retry_policy(void)
{
    if (retry_transport(false, false, 2, 1U) ||
        retry_transport(true, false, 0, 1U) ||
        retry_transport(true, false, 1, 1U) ||
        retry_transport(true, true, 2, 1U) ||
        !retry_transport(true, false, 2, 1U) ||
        !retry_transport(true, false, 2, 2U) ||
        retry_transport(true, false, 2, 3U)) {
        (void)fprintf(stderr,
                      "perf_latency: advisory retry policy failed\n");
        return 1;
    }
    (void)printf("latency_advisory_retry_policy OK\n");
    return 0;
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

    if (!encoding_is("esc", "\033[27;1u") ||
        !encoding_is("ctrl+r", "\033[114;5u") ||
        !encoding_is("space", "\033[32;1u") ||
        !encoding_is("left", "\033[D") ||
        !encoding_is("ctrl+left", "\033[1;5D") ||
        !encoding_is("insert", "\033[2~")) {
        (void)fprintf(stderr,
                      "perf_latency: modern key encoding self-check failed\n");
        return 1;
    }
    (void)printf("latency_modern_keys OK\n");

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

static int check_frame_tags(void)
{
    static const u8 background[] =
        "\033[?2026hbackground\033[?2026l";
    static const u8 no_paint[] = "\033]777;yew-key;1;0\a";
    static const u8 painted_a[] = "\033]777;yew-key;1;3\a\033[?2026hab";
    static const u8 painted_b[] =
        "c\033[?2026l"
        "\033[?2026hbackground\033[?2026l";
    static const u8 trailing[] =
        "\033]777;yew-key;1;0\a"
        "\033[?2026hbackground\033[?2026l"
        "\033[?2026hbackground\033[?2026l";
    static const u8 observed[] =
        "\033]777;yew-key;1;3\a\033[?2026habc\033[?2026l";
    DelayedObserver observer = {0};
    FrameScan scan = {0};
    FrameRead read = {0};
    YewLivePty pty = {0};
    int pipefd[2] = {-1, -1};
    bool ok;

    ok = scan_frame_bytes(&scan, background, sizeof(background) - 1U,
                          &read) == FRAME_SCAN_MORE &&
         read.frames == 1U && !read.painted;
    (void)memset(&scan, 0, sizeof(scan));
    (void)memset(&read, 0, sizeof(read));
    ok = ok && scan_frame_bytes(&scan, no_paint,
                                sizeof(no_paint) - 1U, &read) ==
                   FRAME_SCAN_NO_PAINT &&
         !read.painted && read.frames == 0U;
    (void)memset(&scan, 0, sizeof(scan));
    (void)memset(&read, 0, sizeof(read));
    ok = ok && scan_frame_bytes(&scan, painted_a,
                                sizeof(painted_a) - 1U, &read) ==
                   FRAME_SCAN_MORE &&
         scan_frame_bytes(&scan, painted_b, sizeof(painted_b) - 1U,
                          &read) == FRAME_SCAN_PAINT &&
         read.frames == 2U;
    (void)memset(&scan, 0, sizeof(scan));
    (void)memset(&read, 0, sizeof(read));
    ok = ok && scan_frame_bytes(&scan, trailing, sizeof(trailing) - 1U,
                                &read) == FRAME_SCAN_NO_PAINT &&
         read.frames == 2U;
    if (pipe(pipefd) != 0) {
        ok = false;
    } else {
        pty.master = pipefd[0];
        ok = ok && !read_frames(&pty, yew_live_pty_now_ns() +
                                      INT64_C(1000000), &read);
        (void)close(pipefd[0]);
        (void)close(pipefd[1]);
    }
    pipefd[0] = -1;
    pipefd[1] = -1;
    (void)memset(&read, 0, sizeof(read));
    if (pipe(pipefd) != 0) {
        ok = false;
    } else {
        ssize_t written;

        pty.master = pipefd[0];
        yew_live_pty_set_output(&pty, delayed_observer, &observer);
        written = write(pipefd[1], observed, sizeof(observed) - 1U);
        ok = ok && written == (ssize_t)(sizeof(observed) - 1U) &&
             read_frames(&pty, yew_live_pty_now_ns() + INT64_C(1000000000),
                         &read) &&
             read.painted && observer.started_ns >= read.completed_ns &&
             observer.finished_ns - observer.started_ns >= INT64_C(10000000) &&
             observer.finished_ns - read.completed_ns >= INT64_C(10000000);
        (void)close(pipefd[0]);
        (void)close(pipefd[1]);
    }
    (void)printf("latency_frame_tags %s\n", ok ? "OK" : "FAIL");
    return ok ? 0 : 1;
}

static void usage(const char *arg0)
{
    (void)fprintf(stderr,
        "usage:\n"
        "  %s --floor --echo PATH\n"
        "  %s --check-scripts DIR\n"
        "  %s --check-assist-vt\n"
        "  %s --check-frame-tags\n"
        "  %s --selftest-retry\n"
        "  %s --yew PATH --session FILE --fixture CLASS --path FILE "
        "[--state DIR] [--many-dir DIR] [--fakelsp PATH --mockai PATH "
        "--ai-script PATH] [--prof-dump PATH] [--single-attempt]\n",
        arg0, arg0, arg0, arg0, arg0, arg0);
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
    bool frame_tags = false;
    bool retry_selftest = false;
    bool single_attempt = false;
    int i;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--floor") == 0)
            floor = true;
        else if (strcmp(argv[i], "--check-assist-vt") == 0)
            assist_vt = true;
        else if (strcmp(argv[i], "--check-frame-tags") == 0)
            frame_tags = true;
        else if (strcmp(argv[i], "--selftest-retry") == 0)
            retry_selftest = true;
        else if (strcmp(argv[i], "--single-attempt") == 0)
            single_attempt = true;
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
    if (retry_selftest && !assist_vt && !frame_tags && !floor &&
        check == NULL && yew == NULL)
        return check_retry_policy();
    if (assist_vt && !retry_selftest && !frame_tags && !floor &&
        check == NULL && yew == NULL)
        return check_assist_vt();
    if (frame_tags && !retry_selftest && !assist_vt && !floor &&
        check == NULL && yew == NULL)
        return check_frame_tags();
    if (check != NULL && !retry_selftest && !floor && !assist_vt &&
        !frame_tags && yew == NULL)
        return check_scripts(check);
    if (floor && echo != NULL && check == NULL && !retry_selftest &&
        !assist_vt && !frame_tags && yew == NULL)
        return run_floor(echo);
    if (!floor && !retry_selftest && !assist_vt && !frame_tags &&
        check == NULL && yew != NULL && script != NULL && fixture != NULL &&
        path != NULL)
        return run_session_with_retry(yew, script, fixture, path, state,
                                      many_dir, fakelsp, mockai, ai_script,
                                      prof_dump, single_attempt);
    usage(argv[0]);
    return 2;
}
