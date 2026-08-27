#define _POSIX_C_SOURCE 200809L

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

enum {
    SESSION_KEYS = 10000,
    OUTPUT_CAP = 16384,
    ARG_CAP = 20,
    PROF_RING = 16384,
    OVERHEAD_LIMIT_PERMILLE = 20,
    CROSSCHECK_LIMIT_PERMILLE = 250
};

typedef struct Options {
    const char *runner;
    const char *yew;
    const char *session;
    const char *fixture;
    const char *path;
    const char *state;
    const char *many_dir;
} Options;

typedef struct RunResult {
    uint64_t external_p99_ns;
    char output[OUTPUT_CAP];
} RunResult;

static bool regular_file(const char *path)
{
    struct stat st;

    return path != NULL && stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

static bool write_all(int fd, const void *data, size_t len)
{
    const unsigned char *bytes = data;

    while (len != 0U) {
        ssize_t n = write(fd, bytes, len);

        if (n > 0) {
            bytes += (size_t)n;
            len -= (size_t)n;
        } else if (n < 0 && errno == EINTR) {
            continue;
        } else {
            return false;
        }
    }
    return true;
}

static bool write_token(int fd, const char *token, size_t len)
{
    return write_all(fd, token, len) && write_all(fd, "\n", 1U);
}

static bool write_text_tokens(int fd, const char *text)
{
    const unsigned char *p = (const unsigned char *)text;

    while (*p != '\0') {
        char token[2];

        if (*p == ' ') {
            if (!write_token(fd, "space", sizeof("space") - 1U))
                return false;
        } else if (*p >= 0x21U && *p <= 0x7eU) {
            token[0] = (char)*p;
            token[1] = '\0';
            if (!write_token(fd, token, 1U))
                return false;
        } else {
            return false;
        }
        p++;
    }
    return true;
}

static bool load_tokens(const char *path, char **tokens, size_t *count)
{
    FILE *fp = fopen(path, "r");
    char *line = NULL;
    size_t cap = 0U;
    bool ok = fp != NULL;

    *count = 0U;
    while (ok && getline(&line, &cap, fp) >= 0) {
        char *start = line;
        char *end;
        char *copy;
        char *p;

        while (*start != '\0' && isspace((unsigned char)*start))
            start++;
        if (*start == '\0' || *start == '#')
            continue;
        end = start + strlen(start);
        while (end > start && isspace((unsigned char)end[-1]))
            end--;
        for (p = start; p < end && !isspace((unsigned char)*p); p++)
            ;
        if (end == start || p != end || *count == SESSION_KEYS) {
            ok = false;
            break;
        }
        copy = malloc((size_t)(end - start) + 1U);
        if (copy == NULL) {
            ok = false;
            break;
        }
        (void)memcpy(copy, start, (size_t)(end - start));
        copy[end - start] = '\0';
        tokens[(*count)++] = copy;
    }
    if (fp != NULL && ferror(fp))
        ok = false;
    free(line);
    if (fp != NULL)
        (void)fclose(fp);
    return ok && *count == SESSION_KEYS;
}

static void free_tokens(char **tokens, size_t count)
{
    size_t i;

    for (i = 0U; i < count; i++)
        free(tokens[i]);
}

static size_t command_keys(const char *command)
{
    return 2U + strlen(command) + 1U;
}

static bool write_command(int fd, const char *command)
{
    return write_token(fd, "esc", sizeof("esc") - 1U) &&
           write_token(fd, ":", 1U) && write_text_tokens(fd, command) &&
           write_token(fd, "enter", sizeof("enter") - 1U);
}

static bool make_profile_session(const char *source, const char *dump_path,
                                 const char *dir, char *session,
                                 size_t session_cap)
{
    char *tokens[SESSION_KEYS];
    char dump_command[1024];
    const char *base;
    size_t count = 0U;
    size_t prefix;
    size_t suffix;
    size_t i;
    int fd = -1;
    int n;
    bool ok = false;

    base = strrchr(source, '/');
    base = base == NULL ? source : base + 1;
    n = snprintf(session, session_cap, "%s/%s", dir, base);
    if (n <= 0 || (size_t)n >= session_cap)
        goto done;
    n = snprintf(dump_command, sizeof(dump_command), "prof dump %s",
                 dump_path);
    if (n <= 0 || (size_t)n >= sizeof(dump_command) ||
        !load_tokens(source, tokens, &count))
        goto done;
    prefix = command_keys("prof reset");
    suffix = command_keys(dump_command);
    if (prefix + suffix >= SESSION_KEYS)
        goto done;
    fd = open(session, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd < 0 || !write_command(fd, "prof reset"))
        goto done;
    for (i = prefix; i < SESSION_KEYS - suffix; i++) {
        if (!write_token(fd, tokens[i], strlen(tokens[i])))
            goto done;
    }
    if (!write_command(fd, dump_command) || close(fd) != 0)
        goto done;
    fd = -1;
    ok = true;

done:
    if (fd >= 0)
        (void)close(fd);
    free_tokens(tokens, count);
    return ok;
}

static bool parse_external_p99(const char *output, uint64_t *value)
{
    const char *line = output;

    while (*line != '\0') {
        const char *end = strchr(line, '\n');
        const char *p99 = strstr(line, ".p99 ");
        unsigned long long parsed;

        if (p99 != NULL && (end == NULL || p99 < end) &&
            sscanf(p99 + sizeof(".p99 ") - 1U, "%llu ns", &parsed) == 1) {
            *value = (uint64_t)parsed;
            return *value != 0U;
        }
        if (end == NULL)
            break;
        line = end + 1;
    }
    return false;
}

static bool parse_prof_total(const char *path, uint64_t *value)
{
    FILE *fp = fopen(path, "r");
    char *line = NULL;
    size_t cap = 0U;
    bool ok = false;

    if (fp == NULL)
        return false;
    while (getline(&line, &cap, fp) >= 0) {
        unsigned long p50;
        unsigned long p90;
        unsigned long p99;
        unsigned long max;

        if (sscanf(line, "TOTAL %lu %lu %lu %lu", &p50, &p90, &p99,
                   &max) == 4) {
            (void)p50;
            (void)p90;
            (void)max;
            *value = (uint64_t)p99;
            ok = *value != 0U;
            break;
        }
    }
    free(line);
    (void)fclose(fp);
    return ok;
}

static bool read_child_output(int fd, char *out, size_t cap)
{
    size_t len = 0U;

    while (len + 1U < cap) {
        ssize_t n = read(fd, out + len, cap - len - 1U);

        if (n > 0)
            len += (size_t)n;
        else if (n < 0 && errno == EINTR)
            continue;
        else if (n == 0)
            break;
        else
            return false;
    }
    out[len] = '\0';
    if (len + 1U == cap) {
        char discard[1024];

        while (read(fd, discard, sizeof(discard)) > 0)
            ;
        return false;
    }
    return true;
}

static bool run_latency(const Options *opts, const char *session,
                        bool prof_on, RunResult *result)
{
    char *argv[ARG_CAP];
    char ring[32];
    int pipefd[2];
    size_t narg = 0U;
    pid_t pid;
    int status;
    bool read_ok;

    argv[narg++] = (char *)opts->runner;
    argv[narg++] = (char *)"--yew";
    argv[narg++] = (char *)opts->yew;
    argv[narg++] = (char *)"--session";
    argv[narg++] = (char *)session;
    argv[narg++] = (char *)"--fixture";
    argv[narg++] = (char *)opts->fixture;
    argv[narg++] = (char *)"--path";
    argv[narg++] = (char *)opts->path;
    argv[narg++] = (char *)"--state";
    argv[narg++] = (char *)opts->state;
    if (opts->many_dir != NULL) {
        argv[narg++] = (char *)"--many-dir";
        argv[narg++] = (char *)opts->many_dir;
    }
    argv[narg] = NULL;
    if (narg + 1U >= ARG_CAP || pipe(pipefd) != 0)
        return false;
    pid = fork();
    if (pid < 0) {
        (void)close(pipefd[0]);
        (void)close(pipefd[1]);
        return false;
    }
    if (pid == 0) {
        (void)close(pipefd[0]);
        if (dup2(pipefd[1], STDOUT_FILENO) < 0)
            _exit(126);
        (void)close(pipefd[1]);
        if (prof_on) {
            (void)snprintf(ring, sizeof(ring), "%u", PROF_RING);
            if (setenv("YEW_PROF", "1", 1) != 0 ||
                setenv("YEW_PROF_RING", ring, 1) != 0)
                _exit(126);
        } else {
            if (unsetenv("YEW_PROF") != 0 || unsetenv("YEW_PROF_RING") != 0)
                _exit(126);
        }
        execv(opts->runner, argv);
        _exit(126);
    }
    (void)close(pipefd[1]);
    read_ok = read_child_output(pipefd[0], result->output,
                                sizeof(result->output));
    (void)close(pipefd[0]);
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR)
            return false;
    }
    return read_ok && WIFEXITED(status) && WEXITSTATUS(status) == 0 &&
           parse_external_p99(result->output, &result->external_p99_ns);
}

static uint64_t delta_permille(uint64_t a, uint64_t b, uint64_t denominator)
{
    uint64_t delta = a > b ? a - b : b - a;

    if (denominator == 0U)
        return UINT64_MAX;
    if (delta > UINT64_MAX / 1000U)
        return UINT64_MAX;
    return delta * 1000U / denominator;
}

static bool gating(void)
{
    const char *gate = getenv("PERF_GATE");
    const char *advisory = getenv("YEW_PERF_ADVISORY");

    return gate != NULL && strcmp(gate, "1") == 0 &&
           (advisory == NULL || strcmp(advisory, "0") == 0);
}

static void usage(const char *arg0)
{
    (void)fprintf(stderr,
        "usage: %s --runner PATH --yew PATH --session FILE "
        "--fixture CLASS --path FILE [--state DIR] [--many-dir DIR]\n",
        arg0);
}

int main(int argc, char **argv)
{
    static const char temp_pattern[] = "/tmp/yew-prof-crosscheck-XXXXXX";
    Options opts = {NULL, NULL, NULL, NULL, NULL, "/tmp", NULL};
    RunResult off;
    RunResult on;
    char temp_dir[128] = "";
    char prof_session[1024] = "";
    char dump_path[1024] = "";
    uint64_t prof_p99_ns;
    uint64_t overhead_pm;
    uint64_t crosscheck_pm;
    bool gate;
    int i;
    int status = 0;

    for (i = 1; i < argc; i++) {
        if (i + 1 < argc && strcmp(argv[i], "--runner") == 0)
            opts.runner = argv[++i];
        else if (i + 1 < argc && strcmp(argv[i], "--yew") == 0)
            opts.yew = argv[++i];
        else if (i + 1 < argc && strcmp(argv[i], "--session") == 0)
            opts.session = argv[++i];
        else if (i + 1 < argc && strcmp(argv[i], "--fixture") == 0)
            opts.fixture = argv[++i];
        else if (i + 1 < argc && strcmp(argv[i], "--path") == 0)
            opts.path = argv[++i];
        else if (i + 1 < argc && strcmp(argv[i], "--state") == 0)
            opts.state = argv[++i];
        else if (i + 1 < argc && strcmp(argv[i], "--many-dir") == 0)
            opts.many_dir = argv[++i];
        else {
            usage(argv[0]);
            return 2;
        }
    }
    if (!regular_file(opts.runner) || !regular_file(opts.yew) ||
        !regular_file(opts.session) || opts.fixture == NULL ||
        opts.path == NULL) {
        usage(argv[0]);
        return 2;
    }
    if (sizeof(temp_pattern) > sizeof(temp_dir)) {
        (void)fprintf(stderr, "perf_prof_crosscheck: cannot prepare session\n");
        return 2;
    }
    (void)memcpy(temp_dir, temp_pattern, sizeof(temp_pattern));
    if (mkdtemp(temp_dir) == NULL) {
        (void)fprintf(stderr, "perf_prof_crosscheck: cannot prepare session\n");
        return 2;
    }
    i = snprintf(dump_path, sizeof(dump_path), "%s/report.txt", temp_dir);
    if (i <= 0 || (size_t)i >= sizeof(dump_path) ||
        !make_profile_session(opts.session, dump_path, temp_dir, prof_session,
                              sizeof(prof_session))) {
        (void)fprintf(stderr, "perf_prof_crosscheck: cannot prepare session\n");
        status = 2;
        goto done;
    }
    if (!run_latency(&opts, opts.session, false, &off) ||
        !run_latency(&opts, prof_session, true, &on) ||
        !parse_prof_total(dump_path, &prof_p99_ns)) {
        (void)fprintf(stderr,
                      "perf_prof_crosscheck: measurement or profiler dump failed\n");
        status = 2;
        goto done;
    }
    overhead_pm = on.external_p99_ns > off.external_p99_ns ?
        delta_permille(on.external_p99_ns, off.external_p99_ns,
                       off.external_p99_ns) : 0U;
    crosscheck_pm = delta_permille(on.external_p99_ns, prof_p99_ns,
                                   on.external_p99_ns);
    gate = gating();
    (void)printf("latency.prof.off.p99 %llu ns\n",
                 (unsigned long long)off.external_p99_ns);
    (void)printf("latency.prof.on.p99 %llu ns\n",
                 (unsigned long long)on.external_p99_ns);
    (void)printf("latency.prof.total.p99 %llu ns\n",
                 (unsigned long long)prof_p99_ns);
    (void)printf("latency.prof_overhead %llu permille %s\n",
                 (unsigned long long)overhead_pm,
                 overhead_pm <= OVERHEAD_LIMIT_PERMILLE ? "OK" :
                 gate ? "FAIL" : "ADVISORY");
    (void)printf("latency.prof_external_delta %llu permille %s\n",
                 (unsigned long long)crosscheck_pm,
                 crosscheck_pm <= CROSSCHECK_LIMIT_PERMILLE ? "OK" :
                 gate ? "FAIL" : "ADVISORY");
    if (gate && (overhead_pm > OVERHEAD_LIMIT_PERMILLE ||
                 crosscheck_pm > CROSSCHECK_LIMIT_PERMILLE))
        status = 1;

done:
    (void)unlink(dump_path);
    (void)unlink(prof_session);
    (void)rmdir(temp_dir);
    return status;
}
