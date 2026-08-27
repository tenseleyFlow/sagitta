#define _POSIX_C_SOURCE 200809L

#include "support/live_pty.h"

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

static const MetricSpec metrics[] = {
    {"typing", "small", "latency.typing.small.p99", true, NULL},
    {"typing", "huge", "latency.typing.huge.p99", true, NULL},
    {"edit", "syntax", "latency.edit.syntax.p99", true, NULL},
    {"multicursor", "small", "latency.multicursor.small.p99", true, NULL},
    {"navigate", "many-buffers", "latency.navigate.many_buffers.p99", true,
     NULL},
    {"search", "huge", "latency.search.huge.p99", true, NULL},
    {"typing", "assist", "latency.typing.assist.p99", false,
     "the public live-PTY API does not expose the VT screen needed to "
     "assert that the LSP/AI ghost is visible"}
};

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

static bool spawn_many_buffers(YewLivePty *pty, const char *binary,
                               const char *dir, const char *state,
                               const char *workspace)
{
    char paths[MANY_BUFFER_COUNT][1024];
    char *argv[MANY_BUFFER_COUNT + 4U];
    size_t i;

    if (!regular_dir(dir) || !regular_dir(workspace))
        return false;
    argv[0] = (char *)binary;
    argv[1] = (char *)"--workspace";
    argv[2] = (char *)workspace;
    for (i = 0U; i < MANY_BUFFER_COUNT; i++) {
        int n = snprintf(paths[i], sizeof(paths[i]), "%s/buf-%02zu.c", dir,
                         i);

        if (n <= 0 || (size_t)n >= sizeof(paths[i]) ||
            !regular_file(paths[i]))
            return false;
        argv[i + 3U] = paths[i];
    }
    argv[MANY_BUFFER_COUNT + 3U] = NULL;
    return yew_live_pty_spawn_argv(pty, binary, argv, state, SCREEN_ROWS,
                                   SCREEN_COLS);
}

static bool spawn_file(YewLivePty *pty, const char *binary,
                       const char *path, const char *state,
                       const char *workspace)
{
    char *argv[5];

    if (!regular_dir(workspace))
        return false;
    argv[0] = (char *)binary;
    argv[1] = (char *)"--workspace";
    argv[2] = (char *)workspace;
    argv[3] = (char *)path;
    argv[4] = NULL;
    return yew_live_pty_spawn_argv(pty, binary, argv, state, SCREEN_ROWS,
                                   SCREEN_COLS);
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
    return true;
}

static int run_session(const char *binary, const char *script,
                       const char *fixture, const char *path,
                       const char *state, const char *many_dir)
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
    int status = 0;

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
    if (!make_run_state(state, run_state, sizeof(run_state))) {
        (void)fprintf(stderr, "perf_latency: cannot isolate run state\n");
        return 2;
    }
    if (!make_run_workspace(run_workspace, sizeof(run_workspace))) {
        (void)fprintf(stderr, "perf_latency: cannot isolate workspace\n");
        (void)remove_tree(run_state);
        return 2;
    }
    if (!(strcmp(fixture, "many-buffers") == 0 ?
          spawn_many_buffers(&pty, binary, many_dir, run_state,
                             run_workspace) :
          spawn_file(&pty, binary, path, run_state, run_workspace))) {
        (void)fprintf(stderr, "perf_latency: cannot spawn editor\n");
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
    if (!stop_editor(&pty) && status == 0)
        status = 2;
    yew_live_pty_close(&pty);
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

static void usage(const char *arg0)
{
    (void)fprintf(stderr,
        "usage:\n"
        "  %s --floor --echo PATH\n"
        "  %s --check-scripts DIR\n"
        "  %s --yew PATH --session FILE --fixture CLASS --path FILE "
        "[--state DIR] [--many-dir DIR]\n", arg0, arg0, arg0);
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
    bool floor = false;
    int i;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--floor") == 0)
            floor = true;
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
        else {
            usage(argv[0]);
            return 2;
        }
    }
    if (check != NULL && !floor && yew == NULL)
        return check_scripts(check);
    if (floor && echo != NULL && check == NULL && yew == NULL)
        return run_floor(echo);
    if (!floor && check == NULL && yew != NULL && script != NULL &&
        fixture != NULL && path != NULL)
        return run_session(yew, script, fixture, path, state, many_dir);
    usage(argv[0]);
    return 2;
}
