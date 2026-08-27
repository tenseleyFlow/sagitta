#define _POSIX_C_SOURCE 200809L

#include "support/live_pty.h"

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

enum { ROWS = 24, COLS = 80, DEFAULT_RUNS = 3 };

typedef struct Options {
    const char *yew;
    const char *state;
    const char *budgets;
    const char *code;
    const char *utf8;
    const char *allnl;
    u64 scale;
} Options;

typedef struct Sample {
    i64 open_ns;
    i64 key_ns;
} Sample;

static void usage(void)
{
    (void)fputs(
        "usage: perf_open --yew PATH --state DIR --budgets PATH "
        "--fixture-code PATH [--fixture-utf8 PATH] [--fixture-allnl PATH] "
        "[--scale-permille N]\n", stderr);
}

static bool parse_u64(const char *text, u64 *out)
{
    char *end;
    unsigned long long value;

    errno = 0;
    value = strtoull(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || value == 0U ||
        (unsigned long long)(u64)value != value)
        return false;
    *out = (u64)value;
    return true;
}

static bool parse_options(int argc, char **argv, Options *out)
{
    int i;

    (void)memset(out, 0, sizeof(*out));
    out->scale = 1000U;
    for (i = 1; i < argc; i += 2) {
        const char *name;
        const char *value;

        if (i + 1 >= argc)
            return false;
        name = argv[i];
        value = argv[i + 1];
        if (strcmp(name, "--yew") == 0 && out->yew == NULL)
            out->yew = value;
        else if (strcmp(name, "--state") == 0 && out->state == NULL)
            out->state = value;
        else if (strcmp(name, "--budgets") == 0 && out->budgets == NULL)
            out->budgets = value;
        else if (strcmp(name, "--fixture-code") == 0 && out->code == NULL)
            out->code = value;
        else if (strcmp(name, "--fixture-utf8") == 0 && out->utf8 == NULL)
            out->utf8 = value;
        else if (strcmp(name, "--fixture-allnl") == 0 && out->allnl == NULL)
            out->allnl = value;
        else if (strcmp(name, "--scale-permille") == 0 &&
                 out->scale == 1000U) {
            if (!parse_u64(value, &out->scale))
                return false;
        } else {
            return false;
        }
    }
    return out->yew != NULL && out->state != NULL && out->budgets != NULL &&
           out->code != NULL && out->scale >= 500U && out->scale <= 3000U;
}

static bool budget(const char *path, const char *wanted, u64 *limit)
{
    FILE *file = fopen(path, "r");
    char line[512];

    if (file == NULL)
        return false;
    while (fgets(line, sizeof(line), file) != NULL) {
        char metric[128];
        char comparison[16];
        char value[32];

        if (line[0] == '#' ||
            sscanf(line, "%127s %15s %31s", metric, comparison, value) != 3 ||
            strcmp(metric, wanted) != 0)
            continue;
        if (strcmp(comparison, "record") == 0 && strcmp(value, "-") == 0) {
            *limit = 0U;
            return fclose(file) == 0;
        }
        if (strcmp(comparison, "le") != 0 || !parse_u64(value, limit)) {
            (void)fclose(file);
            return false;
        }
        return fclose(file) == 0;
    }
    (void)fclose(file);
    return false;
}

static bool spawn_clean(YewLivePty *pty, const Options *opt,
                        const char *fixture, const char *log_env,
                        i64 *started)
{
    char slave[128];
    const char *log = getenv(log_env);
    pid_t pid;

    if (log == NULL)
        log = getenv("YEW_PERF_LOG");

    if (!yew_live_pty_open(pty, slave, sizeof(slave), ROWS, COLS))
        return false;
    *started = yew_live_pty_now_ns();
    pid = *started < 0 ? -1 : fork();
    if (pid < 0) {
        yew_live_pty_close(pty);
        return false;
    }
    if (pid == 0) {
        if (setenv("TERM", "xterm-256color", 1) != 0 ||
            setenv("COLORTERM", "truecolor", 1) != 0 ||
            setenv("YEW_TTY_PROBE", "1", 1) != 0 ||
            setenv("YEW_PROBE_TIMEOUT_MS", "500", 1) != 0 ||
            setenv("YEW_ESC_TIMEOUT_MS", "25", 1) != 0 ||
            setenv("LANG", "C.UTF-8", 1) != 0 ||
            setenv("LC_ALL", "C.UTF-8", 1) != 0 ||
            setenv("XDG_STATE_HOME", opt->state, 1) != 0 ||
            setenv("YEW_LOG", log != NULL ? log : "/dev/null", 1) != 0 ||
            !yew_live_pty_attach(pty, slave, ROWS, COLS))
            _exit(126);
        (void)execl(opt->yew, opt->yew, "--clean", fixture, (char *)NULL);
        _exit(126);
    }
    pty->pid = pid;
    return true;
}

static bool stop_editor(YewLivePty *pty)
{
    static const char quit[] = "\033[27u:q!\r";
    i64 deadline = yew_live_pty_now_ns() + INT64_C(10000000000);
    int code;

    return yew_live_pty_write(pty, quit, sizeof(quit) - 1U, deadline) &&
           yew_live_pty_wait_exit(pty, deadline, &code) && code == 0;
}

static bool one_sample(const Options *opt, const char *fixture,
                       const char *log_env, bool key_sample, Sample *out)
{
    static const char down[] = "\033[B";
    YewLivePty pty = {.master = -1, .pid = -1};
    i64 started;
    i64 completed;
    i64 deadline;
    u64 frame;
    bool ok;

    if (!spawn_clean(&pty, opt, fixture, log_env, &started))
        return false;
    deadline = started + INT64_C(30000000000);
    if (!yew_live_pty_wait_frame(&pty, 0U, deadline, &completed)) {
        yew_live_pty_close(&pty);
        return false;
    }
    out->open_ns = completed - started;
    out->key_ns = 0;
    ok = out->open_ns > 0;
    if (ok && key_sample) {
        i64 key_started;

        deadline = yew_live_pty_now_ns() + INT64_C(5000000000);
        ok = yew_live_pty_wait_quiet(&pty, INT64_C(1000000), deadline);
        frame = pty.frames;
        key_started = yew_live_pty_now_ns();
        deadline = key_started + INT64_C(5000000000);
        ok = ok && key_started >= 0 &&
             yew_live_pty_write(&pty, down, sizeof(down) - 1U, deadline) &&
             yew_live_pty_wait_frame(&pty, frame, deadline, &completed);
        if (ok)
            out->key_ns = completed - key_started;
        ok = ok && out->key_ns > 0;
    }
    if (ok)
        ok = stop_editor(&pty);
    yew_live_pty_close(&pty);
    return ok;
}

static void sort_i64(i64 *values, size_t n)
{
    size_t i;

    for (i = 1U; i < n; i++) {
        i64 value = values[i];
        size_t at = i;

        while (at > 0U && values[at - 1U] > value) {
            values[at] = values[at - 1U];
            at--;
        }
        values[at] = value;
    }
}

static bool measure(const Options *opt, const char *fixture,
                    const char *log_env, bool key,
                    i64 *open_ns, i64 *key_ns)
{
    i64 opens[DEFAULT_RUNS];
    i64 keys[DEFAULT_RUNS];
    size_t runs = getenv("YEW_PERF_SMOKE") != NULL ? 1U : DEFAULT_RUNS;
    size_t i;

    for (i = 0U; i < runs; i++) {
        Sample sample;

        if (!one_sample(opt, fixture, log_env, key, &sample))
            return false;
        opens[i] = sample.open_ns;
        keys[i] = sample.key_ns;
    }
    sort_i64(opens, runs);
    sort_i64(keys, runs);
    *open_ns = opens[runs / 2U];
    *key_ns = keys[runs / 2U];
    return true;
}

static bool report(const char *metric, i64 value, u64 limit,
                   bool designated, bool informational)
{
    bool broken = value <= 0 || (!informational &&
                                  limit <= UINT64_MAX / UINT64_C(100) &&
                                  (u64)value > limit * UINT64_C(100));
    bool failed = designated && !informational && (u64)value > limit;
    const char *verdict = broken ? "BROKEN" : informational ? "RECORDED" :
                          failed ? "FAIL" : designated ? "PASS" : "ADVISORY";

    (void)printf("%s value_ns=%lld", metric, (long long)value);
    if (!informational)
        (void)printf(" budget_ns=%llu", (unsigned long long)limit);
    (void)printf(" verdict=%s\n", verdict);
    return !broken && !failed;
}

int main(int argc, char **argv)
{
    Options opt;
    u64 code_limit;
    u64 utf8_limit;
    u64 allnl_limit;
    u64 key_limit;
    i64 open_ns;
    i64 key_ns;
    bool gate = getenv("PERF_GATE") != NULL &&
                strcmp(getenv("PERF_GATE"), "1") == 0 &&
                !(getenv("YEW_PERF_ADVISORY") != NULL &&
                  strcmp(getenv("YEW_PERF_ADVISORY"), "0") != 0);
    bool ok = true;

    if (!parse_options(argc, argv, &opt)) {
        usage();
        return 2;
    }
    if (!budget(opt.budgets, "open.100m_code.e2e", &code_limit) ||
        !budget(opt.budgets, "open.100m_utf8.e2e", &utf8_limit) ||
        !budget(opt.budgets, "open.100m_allnl.e2e", &allnl_limit) ||
        !budget(opt.budgets, "open.100m_code.first_key_paint", &key_limit)) {
        (void)fputs("perf_open: malformed or incomplete budgets\n", stderr);
        return 2;
    }
    if (!measure(&opt, opt.code, "YEW_PERF_LOG_CODE", true,
                 &open_ns, &key_ns)) {
        (void)fputs("perf_open: 100m-code measurement failed\n", stderr);
        return 1;
    }
    ok = report("open.100m_code.e2e", open_ns, code_limit, gate, false) && ok;
    key_limit = key_limit > UINT64_MAX / opt.scale ? UINT64_MAX :
                key_limit * opt.scale / UINT64_C(1000);
    ok = report("open.100m_code.first_key_paint", key_ns, key_limit, gate,
                false) && ok;
    if (opt.utf8 != NULL) {
        if (!measure(&opt, opt.utf8, "YEW_PERF_LOG_UTF8", false,
                     &open_ns, &key_ns))
            return 1;
        ok = report("open.100m_utf8.e2e", open_ns, utf8_limit, gate, false) &&
             ok;
    } else {
        (void)puts("open.100m_utf8.e2e verdict=UNSUPPORTED "
                   "reason=fixture_not_supplied");
    }
    if (opt.allnl != NULL) {
        if (!measure(&opt, opt.allnl, "YEW_PERF_LOG_ALLNL", false,
                     &open_ns, &key_ns))
            return 1;
        ok = report("open.100m_allnl.e2e", open_ns, allnl_limit, gate, true) &&
             ok;
    } else {
        (void)puts("open.100m_allnl.e2e verdict=UNSUPPORTED "
                   "reason=fixture_not_supplied");
    }
    return ok ? 0 : 1;
}
