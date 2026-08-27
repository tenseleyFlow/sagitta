#define _POSIX_C_SOURCE 200809L

#include "support/live_pty.h"

#include <errno.h>
#include <poll.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

enum { ROWS = 24, COLS = 80, DEFAULT_RUNS = 3 };

typedef struct Options {
    const char *yew;
    const char *nullexec;
    const char *fixture;
    const char *state;
    const char *budgets;
    const char *workspace;
    const char *batch_script;
} Options;

static void usage(FILE *out)
{
    (void)fputs(
        "usage: perf_startup --yew PATH --nullexec PATH --fixture PATH "
        "--state DIR --budgets PATH [--workspace DIR] [--batch-script PATH]\n",
        out);
}

static bool parse_options(int argc, char **argv, Options *out)
{
    int i;

    (void)memset(out, 0, sizeof(*out));
    for (i = 1; i < argc; i += 2) {
        const char *name;
        const char *value;

        if (i + 1 >= argc)
            return false;
        name = argv[i];
        value = argv[i + 1];
        if (strcmp(name, "--yew") == 0 && out->yew == NULL)
            out->yew = value;
        else if (strcmp(name, "--nullexec") == 0 && out->nullexec == NULL)
            out->nullexec = value;
        else if (strcmp(name, "--fixture") == 0 && out->fixture == NULL)
            out->fixture = value;
        else if (strcmp(name, "--state") == 0 && out->state == NULL)
            out->state = value;
        else if (strcmp(name, "--budgets") == 0 && out->budgets == NULL)
            out->budgets = value;
        else if (strcmp(name, "--workspace") == 0 && out->workspace == NULL)
            out->workspace = value;
        else if (strcmp(name, "--batch-script") == 0 &&
                 out->batch_script == NULL)
            out->batch_script = value;
        else
            return false;
    }
    return out->yew != NULL && out->nullexec != NULL &&
           out->fixture != NULL && out->state != NULL &&
           out->budgets != NULL;
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
        char *end;
        unsigned long long parsed;

        if (line[0] == '#' ||
            sscanf(line, "%127s %15s %31s", metric, comparison, value) != 3 ||
            strcmp(metric, wanted) != 0)
            continue;
        errno = 0;
        parsed = strtoull(value, &end, 10);
        if (errno != 0 || end == value || *end != '\0' || parsed == 0U ||
            strcmp(comparison, "le") != 0 ||
            (unsigned long long)(u64)parsed != parsed) {
            (void)fclose(file);
            return false;
        }
        *limit = (u64)parsed;
        return fclose(file) == 0;
    }
    (void)fclose(file);
    return false;
}

static bool suffix(const u8 *tail, size_t len, const char *text)
{
    size_t n = strlen(text);

    return n <= len && memcmp(tail + len - n, text, n) == 0;
}

static bool cup_suffix(const u8 *tail, size_t len)
{
    size_t at;

    if (len < 3U || (tail[len - 1U] != 'H' && tail[len - 1U] != 'f'))
        return false;
    at = len - 2U;
    while (at > 0U && ((tail[at] >= '0' && tail[at] <= '9') ||
                       tail[at] == ';'))
        at--;
    return at > 0U && tail[at] == '[' && tail[at - 1U] == 0x1bU;
}

static bool wait_marker(YewLivePty *pty, bool dumb, i64 deadline,
                        i64 *completed)
{
    u8 tail[64];
    size_t ntail = 0U;
    bool kitty = false;
    bool sync = false;
    bool da = false;

    while (yew_live_pty_now_ns() < deadline) {
        struct pollfd fd = {pty->master, POLLIN | POLLHUP, 0};
        u8 bytes[8192];
        ssize_t n;
        size_t i;

        if (poll(&fd, 1U, 25) < 0) {
            if (errno == EINTR)
                continue;
            return false;
        }
        if ((fd.revents & (POLLIN | POLLHUP)) == 0)
            continue;
        n = read(pty->master, bytes, sizeof(bytes));
        if (n < 0 && (errno == EINTR || errno == EAGAIN))
            continue;
        if (n <= 0)
            return false;
        for (i = 0U; i < (size_t)n; i++) {
            if (ntail == sizeof(tail)) {
                (void)memmove(tail, tail + 1U, sizeof(tail) - 1U);
                ntail--;
            }
            tail[ntail++] = bytes[i];
            if (!dumb && !kitty && suffix(tail, ntail, "\033[?u")) {
                static const char response[] = "\033[?0u";

                kitty = true;
                if (!yew_live_pty_write(pty, response, sizeof(response) - 1U,
                                        deadline))
                    return false;
            }
            if (!dumb && !sync && suffix(tail, ntail, "\033[?2026$p")) {
                static const char response[] = "\033[?2026;2$y";

                sync = true;
                if (!yew_live_pty_write(pty, response, sizeof(response) - 1U,
                                        deadline))
                    return false;
            }
            if (!dumb && !da && suffix(tail, ntail, "\033[c")) {
                static const char response[] = "\033[?62;22c";

                da = true;
                if (!yew_live_pty_write(pty, response, sizeof(response) - 1U,
                                        deadline))
                    return false;
            }
            if ((!dumb && suffix(tail, ntail, "\033[?2026h")) ||
                (dumb && cup_suffix(tail, ntail))) {
                *completed = yew_live_pty_now_ns();
                return *completed >= 0;
            }
        }
    }
    return false;
}

static bool child_env(const char *state, bool dumb)
{
    const char *log = getenv("YEW_PERF_LOG");

    return setenv("TERM", dumb ? "dumb" : "xterm-256color", 1) == 0 &&
           setenv("COLORTERM", dumb ? "" : "truecolor", 1) == 0 &&
           setenv("YEW_TTY_PROBE", "1", 1) == 0 &&
           setenv("YEW_PROBE_TIMEOUT_MS", "500", 1) == 0 &&
           setenv("YEW_ESC_TIMEOUT_MS", "25", 1) == 0 &&
           setenv("LANG", "C.UTF-8", 1) == 0 &&
           setenv("LC_ALL", "C.UTF-8", 1) == 0 &&
           setenv("XDG_STATE_HOME", state, 1) == 0 &&
           setenv("YEW_LOG", log != NULL ? log : "/dev/null", 1) == 0;
}

static bool spawn_editor(YewLivePty *pty, const Options *opt, bool clean,
                         bool dumb, bool workspace, i64 *started)
{
    char slave[128];
    pid_t pid;

    if (!yew_live_pty_open(pty, slave, sizeof(slave), ROWS, COLS))
        return false;
    *started = yew_live_pty_now_ns();
    if (*started < 0) {
        yew_live_pty_close(pty);
        return false;
    }
    pid = fork();
    if (pid < 0) {
        yew_live_pty_close(pty);
        return false;
    }
    if (pid == 0) {
        if (!child_env(opt->state, dumb) ||
            !yew_live_pty_attach(pty, slave, ROWS, COLS))
            _exit(126);
        if (workspace)
            (void)execl(opt->yew, opt->yew, "--workspace", opt->workspace,
                        opt->fixture, (char *)NULL);
        else if (clean)
            (void)execl(opt->yew, opt->yew, "--clean", opt->fixture,
                        (char *)NULL);
        else
            (void)execl(opt->yew, opt->yew, opt->fixture, (char *)NULL);
        _exit(126);
    }
    pty->pid = pid;
    return true;
}

static bool stop_editor(YewLivePty *pty)
{
    static const char quit[] = "\033[27u:q!\r";
    i64 deadline = yew_live_pty_now_ns() + INT64_C(5000000000);
    int code;

    if (!yew_live_pty_write(pty, quit, sizeof(quit) - 1U, deadline) ||
        !yew_live_pty_wait_exit(pty, deadline, &code))
        return false;
    return code == 0;
}

static bool one_startup(const Options *opt, bool clean, bool dumb,
                        bool workspace, i64 *sample)
{
    YewLivePty pty = {.master = -1, .pid = -1};
    i64 started;
    i64 painted;
    bool ok;

    if (!spawn_editor(&pty, opt, clean, dumb, workspace, &started))
        return false;
    ok = wait_marker(&pty, dumb, started + INT64_C(3000000000), &painted);
    if (ok)
        *sample = painted - started;
    if (ok)
        ok = stop_editor(&pty);
    yew_live_pty_close(&pty);
    return ok && *sample > 0;
}

static bool one_floor(const Options *opt, i64 *sample)
{
    YewLivePty pty = {.master = -1, .pid = -1};
    char slave[128];
    pid_t pid;
    i64 started;
    i64 deadline;
    char byte;
    int code;

    if (!yew_live_pty_open(&pty, slave, sizeof(slave), ROWS, COLS))
        return false;
    started = yew_live_pty_now_ns();
    pid = started < 0 ? -1 : fork();
    if (pid < 0) {
        yew_live_pty_close(&pty);
        return false;
    }
    if (pid == 0) {
        if (!child_env(opt->state, false) ||
            !yew_live_pty_attach(&pty, slave, ROWS, COLS))
            _exit(126);
        (void)execl(opt->nullexec, opt->nullexec, (char *)NULL);
        _exit(126);
    }
    pty.pid = pid;
    deadline = started + INT64_C(3000000000);
    while (read(pty.master, &byte, 1U) < 0) {
        struct pollfd fd = {pty.master, POLLIN | POLLHUP, 0};

        if (errno != EAGAIN && errno != EINTR) {
            yew_live_pty_close(&pty);
            return false;
        }
        if (poll(&fd, 1U, 25) < 0 && errno != EINTR) {
            yew_live_pty_close(&pty);
            return false;
        }
        if (yew_live_pty_now_ns() >= deadline) {
            yew_live_pty_close(&pty);
            return false;
        }
    }
    *sample = yew_live_pty_now_ns() - started;
    if (!yew_live_pty_wait_exit(&pty, deadline, &code) || code != 0) {
        yew_live_pty_close(&pty);
        return false;
    }
    yew_live_pty_close(&pty);
    return *sample > 0;
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

static bool measure(const Options *opt, bool clean, bool dumb,
                    bool workspace, bool floor, i64 *median)
{
    i64 samples[DEFAULT_RUNS];
    size_t runs = getenv("YEW_PERF_SMOKE") != NULL ? 1U : DEFAULT_RUNS;
    size_t i;

    for (i = 0U; i < runs; i++) {
        if (!(floor ? one_floor(opt, &samples[i]) :
                      one_startup(opt, clean, dumb, workspace, &samples[i])))
            return false;
    }
    sort_i64(samples, runs);
    *median = samples[runs / 2U];
    return true;
}

static bool report_time(const char *metric, i64 value, u64 limit,
                        bool designated)
{
    bool broken = value <= 0 ||
                  (limit <= UINT64_MAX / UINT64_C(100) &&
                   (u64)value > limit * UINT64_C(100));
    bool failed = designated && (u64)value > limit;
    const char *verdict = broken ? "BROKEN" : failed ? "FAIL" :
                          designated ? "PASS" : "ADVISORY";

    (void)printf("%s value_ns=%lld budget_ns=%llu verdict=%s\n", metric,
                 (long long)value, (unsigned long long)limit, verdict);
    return !broken && !failed;
}

int main(int argc, char **argv)
{
    Options opt;
    u64 default_limit;
    u64 clean_limit;
    u64 dumb_limit;
    u64 fraction_limit;
    i64 normal;
    i64 clean;
    i64 dumb;
    i64 floor;
    u64 fraction;
    bool designated = getenv("PERF_GATE") != NULL &&
                      strcmp(getenv("PERF_GATE"), "1") == 0 &&
                      !(getenv("YEW_PERF_ADVISORY") != NULL &&
                        strcmp(getenv("YEW_PERF_ADVISORY"), "0") != 0);
    bool ok = true;

    if (!parse_options(argc, argv, &opt)) {
        usage(stderr);
        return 2;
    }
    if (!budget(opt.budgets, "startup.first_paint.default", &default_limit) ||
        !budget(opt.budgets, "startup.first_paint.clean", &clean_limit) ||
        !budget(opt.budgets, "startup.first_paint.dumb", &dumb_limit) ||
        !budget(opt.budgets, "startup.spawn_floor_fraction", &fraction_limit)) {
        (void)fprintf(stderr, "perf_startup: malformed or incomplete budgets\n");
        return 2;
    }
    if (!measure(&opt, false, false, false, false, &normal) ||
        !measure(&opt, true, false, false, false, &clean) ||
        !measure(&opt, false, true, false, false, &dumb) ||
        !measure(&opt, false, false, false, true, &floor)) {
        (void)fprintf(stderr, "perf_startup: measurement failed\n");
        return 1;
    }
    ok = report_time("startup.first_paint.default", normal, default_limit,
                     designated) && ok;
    ok = report_time("startup.first_paint.clean", clean, clean_limit,
                     designated) && ok;
    ok = report_time("startup.first_paint.dumb", dumb, dumb_limit,
                     designated) && ok;
    (void)printf("startup.config_delta value_ns=%lld verdict=RECORDED\n",
                 (long long)(normal - clean));
    (void)printf("startup.spawn_floor value_ns=%lld verdict=RECORDED\n",
                 (long long)floor);
    fraction = normal > 0 && (u64)floor <= UINT64_MAX / UINT64_C(1000) ?
               (u64)floor * UINT64_C(1000) / (u64)normal : UINT64_MAX;
    (void)printf("startup.spawn_floor_fraction value_permille=%llu "
                 "budget_permille=%llu verdict=%s\n",
                 (unsigned long long)fraction,
                 (unsigned long long)fraction_limit,
                 fraction <= fraction_limit ? "PASS" : "FAIL");
    ok = fraction <= fraction_limit && ok;
    if (opt.workspace != NULL) {
        i64 workspace;
        u64 limit;

        if (!budget(opt.budgets, "startup.first_paint.workspace50", &limit) ||
            !measure(&opt, false, false, true, false, &workspace))
            return 1;
        ok = report_time("startup.first_paint.workspace50", workspace, limit,
                         designated) && ok;
    } else {
        (void)puts("startup.first_paint.workspace50 verdict=UNSUPPORTED "
                   "reason=workspace_fixture_not_supplied");
    }
    if (opt.batch_script != NULL)
        (void)puts("startup.first_paint.batch verdict=UNSUPPORTED "
                   "reason=batch_script_start_marker_not_available");
    else
        (void)puts("startup.first_paint.batch verdict=UNSUPPORTED "
                   "reason=batch_script_not_supplied");
    return ok ? 0 : 1;
}
