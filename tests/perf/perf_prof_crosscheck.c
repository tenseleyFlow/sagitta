#define _POSIX_C_SOURCE 200809L

#include <errno.h>
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
    ARG_CAP = 32,
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
    const char *fakelsp;
    const char *mockai;
    const char *ai_script;
} Options;

typedef struct RunResult {
    uint64_t external_p99_ns;
    uint32_t painted;
    char output[OUTPUT_CAP];
} RunResult;

static bool regular_file(const char *path)
{
    struct stat st;

    return path != NULL && stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

static bool parse_external(const char *output, RunResult *result)
{
    const char *line = output;
    uint32_t no_paint = UINT32_MAX;

    while (*line != '\0') {
        const char *end = strchr(line, '\n');
        const char *p99 = strstr(line, ".p99 ");
        const char *no_paint_row = strstr(line, ".no_paint ");
        unsigned long long parsed;

        if (p99 != NULL && (end == NULL || p99 < end) &&
            sscanf(p99 + sizeof(".p99 ") - 1U, "%llu ns", &parsed) == 1)
            result->external_p99_ns = (uint64_t)parsed;
        if (no_paint_row != NULL && (end == NULL || no_paint_row < end) &&
            sscanf(no_paint_row + sizeof(".no_paint ") - 1U,
                   "%llu", &parsed) == 1 && parsed <= SESSION_KEYS)
            no_paint = (uint32_t)parsed;
        if (end == NULL)
            break;
        line = end + 1;
    }
    if (result->external_p99_ns == 0U || no_paint == UINT32_MAX)
        return false;
    result->painted = SESSION_KEYS - no_paint;
    return result->painted != 0U;
}

static bool parse_prof_keypaint(const char *path, uint64_t *value,
                                uint32_t *calls)
{
    FILE *fp = fopen(path, "r");
    char *line = NULL;
    size_t cap = 0U;
    bool ok = false;

    if (fp == NULL)
        return false;
    while (getline(&line, &cap, fp) >= 0) {
        unsigned long long p50;
        unsigned long long p90;
        unsigned long long p99;
        unsigned long long max;
        unsigned long long parsed_calls;

        if (sscanf(line, "KEYPAINT %llu %llu %llu %llu calls=%llu",
                   &p50, &p90, &p99, &max, &parsed_calls) == 5 &&
            parsed_calls <= UINT32_MAX) {
            (void)p50;
            (void)p90;
            (void)max;
            *value = (uint64_t)p99;
            *calls = (uint32_t)parsed_calls;
            ok = *value != 0U && *calls != 0U;
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

static bool run_latency(const Options *opts, bool prof_on,
                        const char *dump_path, RunResult *result)
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
    argv[narg++] = (char *)opts->session;
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
    if (opts->fakelsp != NULL) {
        argv[narg++] = (char *)"--fakelsp";
        argv[narg++] = (char *)opts->fakelsp;
        argv[narg++] = (char *)"--mockai";
        argv[narg++] = (char *)opts->mockai;
        argv[narg++] = (char *)"--ai-script";
        argv[narg++] = (char *)opts->ai_script;
    }
    if (prof_on) {
        argv[narg++] = (char *)"--prof-dump";
        argv[narg++] = (char *)dump_path;
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
           parse_external(result->output, result);
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

static bool metric_prefix(const Options *opts, char *out, size_t cap)
{
    const char *base = strrchr(opts->session, '/');
    const char *dot;
    size_t len;
    int n;

    base = base == NULL ? opts->session : base + 1;
    dot = strrchr(base, '.');
    len = dot != NULL && strcmp(dot, ".keys") == 0 ?
          (size_t)(dot - base) : strlen(base);
    n = snprintf(out, cap, "latency.prof.%.*s.%s", (int)len, base,
                 opts->fixture);
    return len != 0U && n > 0 && (size_t)n < cap;
}

static void usage(const char *arg0)
{
    (void)fprintf(stderr,
        "usage: %s --runner PATH --yew PATH --session FILE "
        "--fixture CLASS --path FILE [--state DIR] [--many-dir DIR] "
        "[--fakelsp PATH --mockai PATH --ai-script PATH]\n",
        arg0);
}

int main(int argc, char **argv)
{
    static const char temp_pattern[] = "/tmp/yew-prof-crosscheck-XXXXXX";
    Options opts = {0};
    RunResult off;
    RunResult on;
    char temp_dir[128] = "";
    char dump_path[1024] = "";
    char prefix[160] = "";
    uint64_t prof_p99_ns;
    uint32_t prof_calls;
    uint64_t overhead_pm;
    uint64_t crosscheck_pm;
    bool gate;
    int i;
    int status = 0;

    opts.state = "/tmp";

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
        else if (i + 1 < argc && strcmp(argv[i], "--fakelsp") == 0)
            opts.fakelsp = argv[++i];
        else if (i + 1 < argc && strcmp(argv[i], "--mockai") == 0)
            opts.mockai = argv[++i];
        else if (i + 1 < argc && strcmp(argv[i], "--ai-script") == 0)
            opts.ai_script = argv[++i];
        else {
            usage(argv[0]);
            return 2;
        }
    }
    if (!regular_file(opts.runner) || !regular_file(opts.yew) ||
        !regular_file(opts.session) || opts.fixture == NULL ||
        opts.path == NULL ||
        (strcmp(opts.fixture, "assist") == 0 &&
         (!regular_file(opts.fakelsp) || !regular_file(opts.mockai) ||
          !regular_file(opts.ai_script))) ||
        !metric_prefix(&opts, prefix, sizeof(prefix))) {
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
    if (i <= 0 || (size_t)i >= sizeof(dump_path)) {
        (void)fprintf(stderr, "perf_prof_crosscheck: cannot prepare session\n");
        status = 2;
        goto done;
    }
    if (!run_latency(&opts, false, NULL, &off) ||
        !run_latency(&opts, true, dump_path, &on) ||
        !parse_prof_keypaint(dump_path, &prof_p99_ns, &prof_calls)) {
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
    (void)printf("%s.off.p99 %llu ns\n", prefix,
                 (unsigned long long)off.external_p99_ns);
    (void)printf("%s.on.p99 %llu ns\n", prefix,
                 (unsigned long long)on.external_p99_ns);
    (void)printf("%s.keypaint.p99 %llu ns\n", prefix,
                 (unsigned long long)prof_p99_ns);
    (void)printf("%s.samples external=%u internal=%u %s\n", prefix,
                 on.painted, prof_calls,
                 on.painted == prof_calls ? "OK" : "FAIL");
    (void)printf("%s.overhead %llu permille %s\n", prefix,
                 (unsigned long long)overhead_pm,
                 overhead_pm <= OVERHEAD_LIMIT_PERMILLE ? "OK" :
                 gate ? "FAIL" : "ADVISORY");
    (void)printf("%s.external_delta %llu permille %s\n", prefix,
                 (unsigned long long)crosscheck_pm,
                 crosscheck_pm <= CROSSCHECK_LIMIT_PERMILLE ? "OK" :
                 gate ? "FAIL" : "ADVISORY");
    if (on.painted != prof_calls ||
        (gate && (overhead_pm > OVERHEAD_LIMIT_PERMILLE ||
                  crosscheck_pm > CROSSCHECK_LIMIT_PERMILLE)))
        status = 1;

done:
    (void)unlink(dump_path);
    (void)rmdir(temp_dir);
    return status;
}
