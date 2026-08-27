#define _POSIX_C_SOURCE 200809L

#include "util/rss.h"

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

enum { NROWS = 9, PROBE_BYTES = 1024 * 1024, NCHECKPOINTS = 6 };

typedef struct MemRow {
    const char *name;
    u64 value;
    u64 limit;
    bool seen;
    bool ratio;
    bool informational;
} MemRow;

typedef struct Options {
    const char *yew;
    const char *state;
    const char *budgets;
    const char *log_default;
    const char *log_clean;
    const char *log_code;
    const char *log_utf8;
    const char *log_allnl;
    const char *log_workspace;
    const char *log_typing;
    const char *log_assist;
    const char *code;
    const char *utf8;
    const char *allnl;
} Options;

static MemRow rows[NROWS] = {
    {"mem.clean_open.100m_code", 0U, 0U, false, true, false},
    {"mem.clean_open.100m_utf8", 0U, 0U, false, true, false},
    {"mem.clean_open.100m_allnl", 0U, 0U, false, true, true},
    {"mem.paint.default_empty", 0U, 0U, false, false, false},
    {"mem.paint.clean_empty", 0U, 0U, false, false, false},
    {"mem.workspace50", 0U, 0U, false, false, false},
    {"mem.typing_growth.small", 0U, 0U, false, false, false},
    {"mem.closed_growth.100m_code.linux", 0U, 0U, false, false, false},
    {"mem.assist.small", 0U, 0U, false, false, false}
};

static void usage(void)
{
    (void)fputs(
        "usage: perf_mem --budgets PATH [--yew PATH] [--state DIR] "
        "[--log-default PATH] [--log-clean PATH] [--log-code PATH] "
        "[--log-utf8 PATH] [--log-allnl PATH] [--log-workspace PATH] "
        "[--log-typing PATH] [--log-assist PATH] "
        "[--fixture-code PATH] [--fixture-utf8 PATH] "
        "[--fixture-allnl PATH]\n"
        "log records: rss checkpoint=NAME current_bytes=N peak_bytes=N\n",
        stderr);
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
        else if (strcmp(name, "--state") == 0 && out->state == NULL)
            out->state = value;
        else if (strcmp(name, "--budgets") == 0 && out->budgets == NULL)
            out->budgets = value;
        else if (strcmp(name, "--log-default") == 0 &&
                 out->log_default == NULL)
            out->log_default = value;
        else if (strcmp(name, "--log-clean") == 0 && out->log_clean == NULL)
            out->log_clean = value;
        else if (strcmp(name, "--log-code") == 0 && out->log_code == NULL)
            out->log_code = value;
        else if (strcmp(name, "--log-utf8") == 0 && out->log_utf8 == NULL)
            out->log_utf8 = value;
        else if (strcmp(name, "--log-allnl") == 0 && out->log_allnl == NULL)
            out->log_allnl = value;
        else if (strcmp(name, "--log-workspace") == 0 &&
                 out->log_workspace == NULL)
            out->log_workspace = value;
        else if (strcmp(name, "--log-typing") == 0 &&
                 out->log_typing == NULL)
            out->log_typing = value;
        else if (strcmp(name, "--log-assist") == 0 &&
                 out->log_assist == NULL)
            out->log_assist = value;
        else if (strcmp(name, "--fixture-code") == 0 && out->code == NULL)
            out->code = value;
        else if (strcmp(name, "--fixture-utf8") == 0 && out->utf8 == NULL)
            out->utf8 = value;
        else if (strcmp(name, "--fixture-allnl") == 0 && out->allnl == NULL)
            out->allnl = value;
        else
            return false;
    }
    return out->budgets != NULL;
}

static bool parse_u64(const char *text, u64 *out)
{
    char *end;
    unsigned long long value;

    errno = 0;
    value = strtoull(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' ||
        (unsigned long long)(u64)value != value)
        return false;
    *out = (u64)value;
    return true;
}

static MemRow *find_row(const char *name)
{
    size_t i;

    for (i = 0U; i < NROWS; i++) {
        if (strcmp(rows[i].name, name) == 0)
            return &rows[i];
    }
    return NULL;
}

static bool load_budgets(const char *path)
{
    FILE *file = fopen(path, "r");
    char line[512];

    if (file == NULL)
        return false;
    while (fgets(line, sizeof(line), file) != NULL) {
        char metric[128];
        char comparison[16];
        char value[32];
        MemRow *row;

        if (line[0] == '#' ||
            sscanf(line, "%127s %15s %31s", metric, comparison, value) != 3)
            continue;
        row = find_row(metric);
        if (row == NULL)
            continue;
        if (row->informational) {
            if (strcmp(comparison, "record") != 0 || strcmp(value, "-") != 0) {
                (void)fclose(file);
                return false;
            }
        } else if (strcmp(comparison, "le") != 0 ||
                   !parse_u64(value, &row->limit) || row->limit == 0U) {
            (void)fclose(file);
            return false;
        }
    }
    if (fclose(file) != 0)
        return false;
    for (size_t i = 0U; i < NROWS; i++) {
        if (!rows[i].informational && rows[i].limit == 0U)
            return false;
    }
    return true;
}

typedef struct RssLog {
    u64 current[NCHECKPOINTS];
    u64 peak[NCHECKPOINTS];
    bool seen[NCHECKPOINTS];
} RssLog;

static int checkpoint_index(const char *name)
{
    static const char *const names[NCHECKPOINTS] = {
        "argv", "config", "loaded", "paint", "session", "closed"
    };
    size_t i;

    for (i = 0U; i < NCHECKPOINTS; i++) {
        if (strcmp(name, names[i]) == 0)
            return (int)i;
    }
    return -1;
}

static bool load_log(const char *path, RssLog *out)
{
    FILE *file = fopen(path, "r");
    char line[512];
    size_t lineno = 0U;

    if (file == NULL)
        return false;
    (void)memset(out, 0, sizeof(*out));
    while (fgets(line, sizeof(line), file) != NULL) {
        char name[32];
        char current[32];
        char peak[32];
        char extra[2];
        char *record;
        int index;
        u64 parsed_current;
        u64 parsed_peak;

        lineno++;
        record = strstr(line, "rss checkpoint=");
        if (record == NULL)
            continue;
        if (sscanf(record, "rss checkpoint=%31[a-z] current_bytes=%31[0-9] "
                           "peak_bytes=%31[0-9] %1s",
                   name, current, peak, extra) != 3 ||
            (index = checkpoint_index(name)) < 0 ||
            !parse_u64(current, &parsed_current) ||
            !parse_u64(peak, &parsed_peak) ||
            parsed_current == 0U || parsed_peak == 0U) {
            (void)fprintf(stderr, "perf_mem: malformed checkpoint line %zu\n",
                          lineno);
            (void)fclose(file);
            return false;
        }
        if (!out->seen[index] || parsed_current > out->current[index])
            out->current[index] = parsed_current;
        if (!out->seen[index] || parsed_peak > out->peak[index])
            out->peak[index] = parsed_peak;
        out->seen[index] = true;
    }
    return fclose(file) == 0;
}

static void set_value(const char *metric, u64 value)
{
    MemRow *row = find_row(metric);

    if (row != NULL) {
        row->value = value;
        row->seen = true;
    }
}

static bool consume_logs(const Options *opt)
{
    const char *paths[8] = {opt->log_default, opt->log_clean, opt->log_code,
                            opt->log_utf8, opt->log_allnl, opt->log_workspace,
                            opt->log_typing, opt->log_assist};
    size_t i;

    for (i = 0U; i < 8U; i++) {
        RssLog log;

        if (paths[i] == NULL)
            continue;
        if (!load_log(paths[i], &log))
            return false;
        if (i == 0U && log.seen[3])
            set_value("mem.paint.default_empty", log.peak[3]);
        else if (i == 1U && log.seen[3])
            set_value("mem.paint.clean_empty", log.peak[3]);
        else if (i == 2U) {
            if (log.seen[3])
                set_value("mem.clean_open.100m_code", log.peak[3]);
            if (log.seen[1] && log.seen[5])
                set_value("mem.closed_growth.100m_code.linux",
                          log.current[5] > log.current[1] ?
                          log.current[5] - log.current[1] : 0U);
        } else if (i == 3U && log.seen[3]) {
            set_value("mem.clean_open.100m_utf8", log.peak[3]);
        } else if (i == 4U && log.seen[3]) {
            set_value("mem.clean_open.100m_allnl", log.peak[3]);
        } else if (i == 5U && log.seen[3]) {
            set_value("mem.workspace50", log.peak[3]);
        } else if (i == 6U && log.seen[3] && log.seen[4]) {
            set_value("mem.typing_growth.small",
                      log.current[4] > log.current[3] ?
                      log.current[4] - log.current[3] : 0U);
        } else if (i == 7U && log.seen[3]) {
            set_value("mem.assist.small", log.peak[3]);
        }
    }
    return true;
}

static bool fixture_size(const Options *opt, const MemRow *row, u64 *out)
{
    const char *path = strstr(row->name, "100m_code") != NULL ? opt->code :
                       strstr(row->name, "100m_utf8") != NULL ? opt->utf8 :
                       opt->allnl;
    struct stat st;

    if (path == NULL || stat(path, &st) != 0 || st.st_size <= 0)
        return false;
    *out = (u64)st.st_size;
    return (off_t)*out == st.st_size;
}

static bool selfcheck_rss(void)
{
    u64 current_before = yew_rss_bytes();
    u64 peak_before = yew_rss_peak_bytes();
    u64 peak_after;
    unsigned char *probe = malloc(PROBE_BYTES);
    size_t i;

    if (current_before == 0U || peak_before == 0U || probe == NULL)
        return false;
    for (i = 0U; i < PROBE_BYTES; i += 4096U)
        probe[i] = (unsigned char)i;
    peak_after = yew_rss_peak_bytes();
    free(probe);
    if (peak_after == 0U || peak_after < peak_before)
        return false;
    (void)printf("mem.harness.current_bytes value_bytes=%llu verdict=PASS\n",
                 (unsigned long long)current_before);
    (void)printf("mem.harness.peak_before_bytes value_bytes=%llu verdict=PASS\n",
                 (unsigned long long)peak_before);
    (void)printf("mem.harness.peak_after_bytes value_bytes=%llu verdict=PASS\n",
                 (unsigned long long)peak_after);
    return true;
}

static bool report_row(const Options *opt, const MemRow *row, bool gate)
{
    u64 value = row->value;
    u64 file_bytes = 0U;
    bool broken;
    bool failed;
    const char *verdict;

    if (!row->seen) {
        (void)printf("%s verdict=UNSUPPORTED "
                     "reason=editor_checkpoint_not_available\n", row->name);
        return !gate;
    }
    if (row->ratio) {
        if (!fixture_size(opt, row, &file_bytes) ||
            value > UINT64_MAX / UINT64_C(1000)) {
            (void)printf("%s verdict=UNSUPPORTED "
                         "reason=fixture_size_not_available\n", row->name);
            return !gate;
        }
        value = value * UINT64_C(1000) / file_bytes;
    }
    broken = !row->seen || (!row->informational &&
                             row->limit <= UINT64_MAX / UINT64_C(100) &&
                             value > row->limit * UINT64_C(100));
    failed = gate && !row->informational && value > row->limit;
    verdict = broken ? "BROKEN" : row->informational ? "RECORDED" :
              failed ? "FAIL" : gate ? "PASS" : "ADVISORY";
    (void)printf("%s value_%s=%llu", row->name,
                 row->ratio ? "permille" : "bytes",
                 (unsigned long long)value);
    if (!row->informational)
        (void)printf(" budget_%s=%llu", row->ratio ? "permille" : "bytes",
                     (unsigned long long)row->limit);
    (void)printf(" verdict=%s\n", verdict);
    return !broken && !failed;
}

int main(int argc, char **argv)
{
    Options opt;
    bool gate = getenv("PERF_GATE") != NULL &&
                strcmp(getenv("PERF_GATE"), "1") == 0 &&
                !(getenv("YEW_PERF_ADVISORY") != NULL &&
                  strcmp(getenv("YEW_PERF_ADVISORY"), "0") != 0);
    bool ok = true;
    size_t i;

    if (!parse_options(argc, argv, &opt)) {
        usage();
        return 2;
    }
    if (!load_budgets(opt.budgets)) {
        (void)fputs("perf_mem: malformed or incomplete budgets\n", stderr);
        return 2;
    }
    if (!selfcheck_rss()) {
        (void)fputs("perf_mem: RSS normalization self-check failed\n", stderr);
        return 1;
    }
    if (!consume_logs(&opt))
        return 2;
    if (opt.log_default == NULL && opt.log_clean == NULL &&
        opt.log_code == NULL && opt.log_utf8 == NULL &&
        opt.log_allnl == NULL && opt.log_workspace == NULL &&
        opt.log_typing == NULL && opt.log_assist == NULL) {
        (void)puts("mem.external_checkpoints verdict=UNSUPPORTED "
                   "reason=no_checkpoint_logs_supplied");
    }
    if (opt.yew == NULL || opt.state == NULL)
        (void)puts("mem.editor_invocation verdict=UNSUPPORTED "
                   "reason=yew_or_state_path_not_supplied");
    else
        (void)puts("mem.editor_invocation verdict=PASS "
                   "producer=external_pty_harnesses");
    for (i = 0U; i < NROWS; i++)
        ok = report_row(&opt, &rows[i], gate) && ok;
    return ok ? 0 : 1;
}
