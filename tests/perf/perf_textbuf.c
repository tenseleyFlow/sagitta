#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>

#include "text/edit.h"
#include "util/base.h"

enum {
    PERF_RUNS = 3,
    OPEN_SAMPLES = 3,
    OPEN_WARMUPS = 3,
    QUERY_SAMPLES = 100000,
    INSERT_SAMPLES = 10000
};

typedef struct {
    u64 p50;
    u64 p99;
    u64 max;
} Timing;

typedef struct {
    const char *name;
    u64 p50;
    u64 p99;
    u64 max;
    u64 rss;
    u64 abs_p99;
    u64 abs_rss;
    bool informational;
} Result;

typedef struct {
    char name[80];
    u64 p50;
    u64 p99;
    u64 max;
    u64 rss;
    char why[192];
    bool scalar;
} Baseline;

typedef struct {
    Baseline *data;
    size_t len;
    size_t cap;
    char runner_id[80];
    u32 scale_permille;
    u64 calib_c1;
    u64 calib_c2;
    u64 calib_c3;
    bool saw_v2_header;
    bool saw_calib;
} Baselines;

static bool now_ns(u64 *out)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return false;
    *out = (u64)ts.tv_sec * UINT64_C(1000000000) + (u64)ts.tv_nsec;
    return true;
}

static u64 random_next(u64 *state)
{
    u64 x = *state;

    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    *state = x;
    return x * UINT64_C(2685821657736338717);
}

static void sort_u64(u64 *values, size_t len)
{
    size_t i;

    for (i = 1U; i < len; i++) {
        u64 value = values[i];
        size_t at = i;

        while (at != 0U && values[at - 1U] > value) {
            values[at] = values[at - 1U];
            at--;
        }
        values[at] = value;
    }
}

static Timing timing_of(u64 *samples, size_t len)
{
    Timing result;
    size_t p99_at;

    sort_u64(samples, len);
    p99_at = (len * 99U + 99U) / 100U;
    if (p99_at != 0U)
        p99_at--;
    result.p50 = samples[(len - 1U) / 2U];
    result.p99 = samples[p99_at];
    result.max = samples[len - 1U];
    return result;
}

/* Normalize resident bytes in this one host-specific function. */
static bool rss_bytes(u64 *out)
{
#if defined(__linux__)
    FILE *file = fopen("/proc/self/statm", "r");
    unsigned long long pages;
    unsigned long long resident;
    long page_size;

    if (file != NULL) {
        int count = fscanf(file, "%llu %llu", &pages, &resident);

        (void)pages;
        (void)fclose(file);
        page_size = sysconf(_SC_PAGESIZE);
        if (count == 2 && page_size > 0) {
            *out = (u64)resident * (u64)page_size;
            return true;
        }
    }
#endif
    {
        struct rusage usage;

        if (getrusage(RUSAGE_SELF, &usage) != 0 || usage.ru_maxrss < 0)
            return false;
#if defined(__APPLE__)
        *out = (u64)usage.ru_maxrss;
#else
        *out = (u64)usage.ru_maxrss * 1024U;
#endif
    }
    return true;
}

static char *fixture_path(const char *dir, const char *name)
{
    size_t len = strlen(dir) + strlen(name) + 6U;
    char *path = malloc(len);

    if (path != NULL)
        (void)snprintf(path, len, "%s/%s.bin", dir, name);
    return path;
}

static bool load_once(const char *path, u64 *elapsed, u64 *rss_growth)
{
    FileMeta meta;
    TextBuf *tb = NULL;
    u64 before;
    u64 after;
    u64 start;
    u64 end;
    YewLoadErr error;

    yew_filemeta_init(&meta);
    if (!rss_bytes(&before) || !now_ns(&start))
        return false;
    error = yew_file_load(path, &tb, &meta);
    if (error == YEW_LOAD_OK && getenv("YEW_PERF_INJECT_OPEN_DELAY") != NULL) {
        struct timespec delay = {0, 200000000L};
        struct timespec left;

        while (nanosleep(&delay, &left) != 0) {
            if (errno != EINTR)
                break;
            delay = left;
        }
    }
    if (!now_ns(&end) || error != YEW_LOAD_OK || !rss_bytes(&after)) {
        yew_textbuf_free(tb);
        yew_filemeta_dispose(&meta);
        return false;
    }
    yew_textbuf_check(tb);
    *elapsed = end - start;
    *rss_growth = after > before ? after - before : 0U;
    yew_textbuf_free(tb);
    yew_filemeta_dispose(&meta);
    return true;
}

static bool measure_open(const char *dir, const char *profile, Result *out,
                         u64 abs_p99, u64 abs_rss, bool informational)
{
    char *path = fixture_path(dir, profile);
    u64 run_p50[PERF_RUNS];
    u64 run_p99[PERF_RUNS];
    u64 run_max[PERF_RUNS];
    u64 max_rss = 0U;
    size_t warm;
    size_t run;

    if (path == NULL)
        return false;
    for (warm = 0U; warm < OPEN_WARMUPS; warm++) {
        u64 elapsed;
        u64 rss;

        if (!load_once(path, &elapsed, &rss)) {
            (void)fprintf(stderr, "perf_textbuf: cannot load %s: %s\n",
                          path, strerror(errno));
            free(path);
            return false;
        }
        if (rss > max_rss)
            max_rss = rss;
    }
    for (run = 0U; run < PERF_RUNS; run++) {
        u64 samples[OPEN_SAMPLES];
        size_t sample;
        Timing timing;

        for (sample = 0U; sample < OPEN_SAMPLES; sample++) {
            u64 rss;

            if (!load_once(path, &samples[sample], &rss)) {
                free(path);
                return false;
            }
            if (rss > max_rss)
                max_rss = rss;
        }
        timing = timing_of(samples, OPEN_SAMPLES);
        run_p50[run] = timing.p50;
        run_p99[run] = timing.p99;
        run_max[run] = timing.max;
    }
    sort_u64(run_p50, PERF_RUNS);
    sort_u64(run_p99, PERF_RUNS);
    sort_u64(run_max, PERF_RUNS);
    out->name = profile;
    out->p50 = run_p50[1];
    out->p99 = run_p99[1];
    out->max = run_max[1];
    out->rss = max_rss;
    out->abs_p99 = abs_p99;
    out->abs_rss = abs_rss;
    out->informational = informational;
    free(path);
    return true;
}

static bool baselines_push(Baselines *set, const Baseline *value)
{
    if (set->len == set->cap) {
        size_t cap = set->cap == 0U ? 16U : set->cap * 2U;
        Baseline *data;

        if (cap > SIZE_MAX / sizeof(*data))
            return false;
        data = realloc(set->data, cap * sizeof(*data));
        if (data == NULL)
            return false;
        set->data = data;
        set->cap = cap;
    }
    set->data[set->len++] = *value;
    return true;
}

static bool baseline_why_valid(const char *why)
{
    return why != NULL && why[0] != '\0' &&
           strlen(why) < sizeof(((Baseline *)0)->why) &&
           strstr(why, "PLACEHOLDER") == NULL;
}

static bool parse_u32_decimal(const char *text, u32 *out)
{
    char *end;
    unsigned long value;

    if (text == NULL || text[0] == '\0' || out == NULL)
        return false;
    errno = 0;
    value = strtoul(text, &end, 10);
    if (errno != 0 || *end != '\0' || value > UINT32_MAX)
        return false;
    *out = (u32)value;
    return true;
}

static bool parse_u64_decimal(const char *text, u64 *out)
{
    char *end;
    unsigned long long value;

    if (text == NULL || text[0] == '\0' || out == NULL)
        return false;
    errno = 0;
    value = strtoull(text, &end, 10);
    if (errno != 0 || *end != '\0')
        return false;
    *out = (u64)value;
    return true;
}

static bool calib_complete(const Baselines *set)
{
    return set->saw_calib && set->scale_permille != 0U &&
           set->calib_c1 != 0U && set->calib_c2 != 0U &&
           set->calib_c3 != 0U;
}

static bool scale_materially_differs(u32 baseline, u32 measured)
{
    u64 difference;

    if (baseline == 0U || measured == 0U)
        return true;
    difference = baseline > measured ? (u64)baseline - measured
                                     : (u64)measured - baseline;
    return difference * 100U > (u64)baseline * 15U;
}

static void trim_reason(char *text)
{
    size_t len;

    while (*text == ' ' || *text == '\t')
        (void)memmove(text, text + 1, strlen(text));
    len = strlen(text);
    while (len != 0U && (text[len - 1U] == '\n' ||
                         text[len - 1U] == '\r' ||
                         text[len - 1U] == ' ' ||
                         text[len - 1U] == '\t'))
        text[--len] = '\0';
}

static bool baselines_read_file(FILE *file, Baselines *set)
{
    char line[512];
    size_t line_no = 0U;

    while (fgets(line, sizeof(line), file) != NULL) {
        Baseline value;
        unsigned long long p50;
        unsigned long long p99;
        unsigned long long max;
        unsigned long long rss;
        unsigned scale;
        int consumed = 0;
        int count;

        line_no++;
        if (line[0] == '#') {
            if (strncmp(line, "# yew perf baseline v2  runner=", 31U) ==
                0) {
                if (sscanf(line + 31, "%79s", set->runner_id) != 1)
                    return false;
                set->saw_v2_header = true;
            } else if (strncmp(line, "# calib ", 8U) == 0) {
                count = sscanf(line,
                               "# calib scale_permille=%u c1=%llu c2=%llu "
                               "c3=%llu %n",
                               &scale, &p50, &p99, &max, &consumed);
                if (count != 4)
                    return false;
                set->scale_permille = (u32)scale;
                set->calib_c1 = (u64)p50;
                set->calib_c2 = (u64)p99;
                set->calib_c3 = (u64)max;
                set->saw_calib = true;
            }
            continue;
        }
        if (line[0] == '\n')
            continue;
        (void)memset(&value, 0, sizeof(value));
        count = sscanf(line, "%79s %llu %llu %llu %llu %n", value.name,
                       &p50, &p99, &max, &rss, &consumed);
        if (count == 5) {
            value.p50 = (u64)p50;
            value.p99 = (u64)p99;
            value.max = (u64)max;
            value.rss = (u64)rss;
        } else {
            consumed = 0;
            count = sscanf(line, "%79s %llu %n", value.name, &p50,
                           &consumed);
        }
        if (count == 2) {
            value.p50 = (u64)p50;
            value.scalar = true;
        } else if (count != 5) {
            (void)fprintf(stderr,
                          "perf_textbuf: malformed baseline at line %zu\n",
                          line_no);
            return false;
        }
        if (strlen(line + consumed) >= sizeof(value.why)) {
            (void)fprintf(stderr,
                          "perf_textbuf: baseline why too long at line %zu\n",
                          line_no);
            return false;
        }
        (void)snprintf(value.why, sizeof(value.why), "%s", line + consumed);
        trim_reason(value.why);
        if (!baseline_why_valid(value.why)) {
            (void)fprintf(stderr,
                          "perf_textbuf: baseline why missing at line %zu\n",
                          line_no);
            return false;
        }
        if (!baselines_push(set, &value)) {
            return false;
        }
    }
    return !ferror(file) && set->saw_v2_header && set->saw_calib &&
           set->runner_id[0] != '\0';
}

static bool baselines_read(const char *path, Baselines *set)
{
    FILE *file = fopen(path, "r");
    bool ok;

    if (file == NULL)
        return false;
    ok = baselines_read_file(file, set);
    if (fclose(file) != 0)
        ok = false;
    return ok;
}

static const Baseline *baseline_find(const Baselines *set, const char *name)
{
    size_t i;

    for (i = 0U; i < set->len; i++) {
        if (strcmp(set->data[i].name, name) == 0)
            return &set->data[i];
    }
    return NULL;
}

static Baseline *baseline_find_mut(Baselines *set, const char *name)
{
    size_t i;

    for (i = 0U; i < set->len; i++) {
        if (strcmp(set->data[i].name, name) == 0)
            return &set->data[i];
    }
    return NULL;
}

static bool baselines_apply_results(Baselines *set, const Result *results,
                                    size_t result_count, const char *why)
{
    size_t i;

    for (i = 0U; i < result_count; i++) {
        const Result *result = &results[i];
        Baseline *baseline = baseline_find_mut(set, result->name);

        if (baseline == NULL) {
            Baseline added;

            (void)memset(&added, 0, sizeof(added));
            (void)snprintf(added.name, sizeof(added.name), "%s",
                           result->name);
            if (!baselines_push(set, &added))
                return false;
            baseline = &set->data[set->len - 1U];
        }
        baseline->p50 = result->p50;
        baseline->p99 = result->p99;
        baseline->max = result->max;
        baseline->rss = result->rss;
        baseline->scalar = false;
        (void)snprintf(baseline->why, sizeof(baseline->why), "%s", why);
    }
    return true;
}

static bool baselines_write_file(FILE *file, const char *runner_id,
                                 const Baselines *set)
{
    size_t i;

    if (fprintf(file, "# yew perf baseline v2  runner=%s\n", runner_id) <
            0 ||
        fprintf(file,
                "# calib scale_permille=%u c1=%llu c2=%llu c3=%llu\n",
                set->scale_permille,
                (unsigned long long)set->calib_c1,
                (unsigned long long)set->calib_c2,
                (unsigned long long)set->calib_c3) < 0 ||
        fprintf(file,
                "# metric                         p50_ns        p99_ns"
                "        max_ns     rss_bytes  why\n") < 0)
        goto fail;
    for (i = 0U; i < set->len; i++) {
        const Baseline *baseline = &set->data[i];

        if (strcmp(baseline->name, "rss.100m-allnl") == 0 &&
            fprintf(file,
                    "# Informational: all-newline input is exempt from the "
                    "1.6x RSS gate (s07).\n") < 0)
            goto fail;
        if (baseline->scalar) {
            if (fprintf(file, "%-36s %14llu  %s\n", baseline->name,
                        (unsigned long long)baseline->p50,
                        baseline->why) < 0)
                goto fail;
        } else if (fprintf(file,
                           "%-28s %14llu %14llu %14llu %14llu  %s\n",
                           baseline->name,
                           (unsigned long long)baseline->p50,
                           (unsigned long long)baseline->p99,
                           (unsigned long long)baseline->max,
                           (unsigned long long)baseline->rss,
                           baseline->why) < 0) {
            goto fail;
        }
    }
    return !ferror(file);

fail:
    return false;
}

static bool baselines_update(const char *path, const char *runner_id,
                             Baselines *set, const Result *results,
                             size_t result_count, const char *why)
{
    FILE *file;
    bool ok;

    if (!baselines_apply_results(set, results, result_count, why))
        return false;
    file = fopen(path, "wb");
    if (file == NULL)
        return false;
    ok = baselines_write_file(file, runner_id, set);
    if (fclose(file) != 0)
        ok = false;
    return ok;
}

static bool update_why_valid(const char *why)
{
    return baseline_why_valid(why) && strstr(why, "s11 initial") == NULL &&
           strstr(why, "s33 initial") == NULL;
}

static bool baselines_set_update_calib(Baselines *set)
{
    u32 scale;
    u64 c1;
    u64 c2;
    u64 c3;

    if (!parse_u32_decimal(getenv("YEW_CALIB_SCALE_PERMILLE"), &scale) ||
        !parse_u64_decimal(getenv("YEW_CALIB_C1_NS"), &c1) ||
        !parse_u64_decimal(getenv("YEW_CALIB_C2_NS"), &c2) ||
        !parse_u64_decimal(getenv("YEW_CALIB_C3_NS"), &c3) || scale == 0U ||
        c1 == 0U || c2 == 0U || c3 == 0U)
        return false;
    set->scale_permille = scale;
    set->calib_c1 = c1;
    set->calib_c2 = c2;
    set->calib_c3 = c3;
    set->saw_calib = true;
    return true;
}

static bool load_buffer(const char *dir, const char *profile, TextBuf **tb,
                        FileMeta *meta)
{
    char *path = fixture_path(dir, profile);
    YewLoadErr error;

    if (path == NULL)
        return false;
    yew_filemeta_init(meta);
    error = yew_file_load(path, tb, meta);
    free(path);
    return error == YEW_LOAD_OK;
}

static bool measure_queries(const char *dir, const char *profile,
                            Result *line_start, Result *line_of)
{
    TextBuf *tb = NULL;
    FileMeta meta;
    u64 *start_samples = malloc(sizeof(*start_samples) * QUERY_SAMPLES);
    u64 *of_samples = malloc(sizeof(*of_samples) * QUERY_SAMPLES);
    u64 rng = UINT64_C(0x9e3779b97f4a7c15);
    u64 lines;
    u64 len;
    u64 start_p50[PERF_RUNS];
    u64 start_p99[PERF_RUNS];
    u64 start_max[PERF_RUNS];
    u64 of_p50[PERF_RUNS];
    u64 of_p99[PERF_RUNS];
    u64 of_max[PERF_RUNS];
    size_t i;
    size_t run;
    Timing timing;
    volatile u64 sink = 0U;

    if (start_samples == NULL || of_samples == NULL ||
        !load_buffer(dir, profile, &tb, &meta)) {
        free(start_samples);
        free(of_samples);
        return false;
    }
    lines = yew_textbuf_line_count(tb);
    len = yew_textbuf_len(tb);
    for (i = 0U; i < 3U; i++) {
        sink += yew_textbuf_line_start(tb, LINENO(i % lines)).v;
        sink += yew_textbuf_line_of(tb, BYTEOFF(i % (len + 1U))).v;
    }
    for (run = 0U; run < PERF_RUNS; run++) {
        for (i = 0U; i < QUERY_SAMPLES; i++) {
            u64 begin;
            u64 end;

            if (!now_ns(&begin))
                goto fail;
            sink += yew_textbuf_line_start(tb,
                LINENO(random_next(&rng) % lines)).v;
            if (!now_ns(&end))
                goto fail;
            start_samples[i] = end - begin;
            if (!now_ns(&begin))
                goto fail;
            sink += yew_textbuf_line_of(tb,
                BYTEOFF(random_next(&rng) % (len + 1U))).v;
            if (!now_ns(&end))
                goto fail;
            of_samples[i] = end - begin;
        }
        timing = timing_of(start_samples, QUERY_SAMPLES);
        start_p50[run] = timing.p50;
        start_p99[run] = timing.p99;
        start_max[run] = timing.max;
        timing = timing_of(of_samples, QUERY_SAMPLES);
        of_p50[run] = timing.p50;
        of_p99[run] = timing.p99;
        of_max[run] = timing.max;
    }
    sort_u64(start_p50, PERF_RUNS);
    sort_u64(start_p99, PERF_RUNS);
    sort_u64(start_max, PERF_RUNS);
    sort_u64(of_p50, PERF_RUNS);
    sort_u64(of_p99, PERF_RUNS);
    sort_u64(of_max, PERF_RUNS);
    *line_start = (Result){"line_start", start_p50[1], start_p99[1],
                           start_max[1], 0U, 5000U, 0U, false};
    *line_of = (Result){"line_of", of_p50[1], of_p99[1], of_max[1], 0U,
                        5000U, 0U, false};
    if (sink == UINT64_MAX)
        (void)fprintf(stderr, "perf_textbuf: query sink=%llu\n",
                      (unsigned long long)sink);
    free(start_samples);
    free(of_samples);
    yew_textbuf_free(tb);
    yew_filemeta_dispose(&meta);
    return true;

fail:
    free(start_samples);
    free(of_samples);
    yew_textbuf_free(tb);
    yew_filemeta_dispose(&meta);
    return false;
}

static bool measure_insert(const char *dir, const char *name, bool head,
                           Result *out)
{
    TextBuf *tb = NULL;
    FileMeta meta;
    u64 *samples = malloc(sizeof(*samples) * INSERT_SAMPLES);
    u64 p50[PERF_RUNS];
    u64 p99[PERF_RUNS];
    u64 max[PERF_RUNS];
    size_t i;
    size_t run;
    Timing timing;
    static const u8 byte = 'x';

    if (samples == NULL || !load_buffer(dir, "1g-code", &tb, &meta)) {
        free(samples);
        return false;
    }
    for (i = 0U; i < 3U; i++) {
        ByteOff at = head ? BYTEOFF(0U) : BYTEOFF(yew_textbuf_len(tb));

        yew_textbuf_insert(tb, at, &byte, 1U);
        yew_textbuf_delete(tb, (Span){at.v, at.v + 1U});
    }
    for (run = 0U; run < PERF_RUNS; run++) {
        for (i = 0U; i < INSERT_SAMPLES; i++) {
            u64 begin;
            u64 end;
            ByteOff at = head ? BYTEOFF(0U) : BYTEOFF(yew_textbuf_len(tb));

            if (!now_ns(&begin))
                goto fail;
            yew_textbuf_insert(tb, at, &byte, 1U);
            if (!now_ns(&end))
                goto fail;
            samples[i] = end - begin;
            yew_textbuf_delete(tb, (Span){at.v, at.v + 1U});
        }
        timing = timing_of(samples, INSERT_SAMPLES);
        p50[run] = timing.p50;
        p99[run] = timing.p99;
        max[run] = timing.max;
    }
    sort_u64(p50, PERF_RUNS);
    sort_u64(p99, PERF_RUNS);
    sort_u64(max, PERF_RUNS);
    *out = (Result){name, p50[1], p99[1], max[1], 0U,
                    50000U, 0U, false};
    free(samples);
    yew_textbuf_free(tb);
    yew_filemeta_dispose(&meta);
    return true;

fail:
    free(samples);
    yew_textbuf_free(tb);
    yew_filemeta_dispose(&meta);
    return false;
}

static bool measure_materialize(const char *dir, Result *out)
{
    TextBuf *tb = NULL;
    FileMeta meta;
    u8 *bytes;
    u64 len;
    u64 begin;
    u64 end;
    u64 samples[PERF_RUNS];
    TextIter it;
    u64 at;
    size_t run;
    Timing timing;

    if (!load_buffer(dir, "1g-code", &tb, &meta))
        return false;
    len = yew_textbuf_len(tb);
    if (len > SIZE_MAX) {
        yew_textbuf_free(tb);
        yew_filemeta_dispose(&meta);
        return false;
    }
    bytes = malloc((size_t)(len == 0U ? 1U : len));
    if (bytes == NULL)
        goto fail;
    for (run = 0U; run < 3U + PERF_RUNS; run++) {
        at = 0U;
        if (!now_ns(&begin) || !yew_textiter_begin(&it, tb, BYTEOFF(0U)))
            goto fail;
        for (;;) {
            const u8 *chunk;
            u64 chunk_len;

            if (!yew_textiter_chunk(&it, tb, &chunk, &chunk_len))
                break;
            (void)memcpy(bytes + at, chunk, (size_t)chunk_len);
            at += chunk_len;
            if (!yew_textiter_advance(&it, tb))
                break;
        }
        if (!now_ns(&end) || at != len)
            goto fail;
        if (run >= 3U)
            samples[run - 3U] = end - begin;
    }
    timing = timing_of(samples, PERF_RUNS);
    *out = (Result){"materialize.1g-code", timing.p50, timing.p99,
                    timing.max, 0U, UINT64_C(1000000000), 0U, false};
    free(bytes);
    yew_textbuf_free(tb);
    yew_filemeta_dispose(&meta);
    return true;

fail:
    free(bytes);
    yew_textbuf_free(tb);
    yew_filemeta_dispose(&meta);
    return false;
}

static bool textbuf_bytes(const TextBuf *tb, u8 **out, u64 *out_len)
{
    u64 len = yew_textbuf_len(tb);
    u8 *bytes;
    TextIter it;
    u64 at = 0U;

    if (len > SIZE_MAX)
        return false;
    bytes = malloc((size_t)(len == 0U ? 1U : len));
    if (bytes == NULL || !yew_textiter_begin(&it, tb, BYTEOFF(0U))) {
        free(bytes);
        return false;
    }
    for (;;) {
        const u8 *chunk;
        u64 chunk_len;

        if (!yew_textiter_chunk(&it, tb, &chunk, &chunk_len))
            break;
        (void)memcpy(bytes + at, chunk, (size_t)chunk_len);
        at += chunk_len;
        if (!yew_textiter_advance(&it, tb))
            break;
    }
    if (at != len) {
        free(bytes);
        return false;
    }
    *out = bytes;
    *out_len = len;
    return true;
}

static bool measure_atomic_save(const char *dir, Result *out)
{
    TextBuf *source = NULL;
    TextBuf *tb = NULL;
    FileMeta source_meta;
    FileMeta meta;
    u8 *bytes = NULL;
    u64 len;
    u64 samples[PERF_RUNS];
    char *path;
    size_t path_len = strlen(dir) + 80U;
    size_t i;
    static const u8 dirty = 's';
    bool ok = false;
    Timing timing;

    path = malloc(path_len);
    if (path == NULL || !load_buffer(dir, "1g-code", &source, &source_meta)) {
        free(path);
        return false;
    }
    (void)snprintf(path, path_len, "%s/.yew-perf-save-%ld.bin", dir,
                   (long)getpid());
    if (!textbuf_bytes(source, &bytes, &len) ||
        yew_file_write_atomic(path, bytes, (size_t)len, 0600) != YEW_SAVE_OK)
        goto done_source;
    free(bytes);
    bytes = NULL;
    yew_textbuf_free(source);
    source = NULL;
    yew_filemeta_dispose(&source_meta);
    yew_filemeta_init(&meta);
    if (yew_file_load(path, &tb, &meta) != YEW_LOAD_OK)
        goto done;
    for (i = 0U; i < 3U + PERF_RUNS; i++) {
        u64 begin;
        u64 end;

        yew_textbuf_insert(tb, BYTEOFF(yew_textbuf_len(tb)), &dirty, 1U);
        if (!now_ns(&begin) || yew_file_save(tb, &meta, path) != YEW_SAVE_OK ||
            !now_ns(&end))
            goto done;
        if (i >= 3U)
            samples[i - 3U] = end - begin;
        if (i + 1U < 3U + PERF_RUNS) {
            yew_textbuf_free(tb);
            tb = NULL;
            yew_filemeta_dispose(&meta);
            yew_filemeta_init(&meta);
            if (yew_file_load(path, &tb, &meta) != YEW_LOAD_OK)
                goto done;
        }
    }
    timing = timing_of(samples, PERF_RUNS);
    *out = (Result){"save.atomic.1g-code", timing.p50, timing.p99,
                    timing.max, 0U, UINT64_C(6000000000), 0U, false};
    ok = true;
done:
    yew_textbuf_free(tb);
    yew_filemeta_dispose(&meta);
    (void)unlink(path);
    free(path);
    return ok;

done_source:
    free(bytes);
    yew_textbuf_free(source);
    yew_filemeta_dispose(&source_meta);
    (void)unlink(path);
    free(path);
    return false;
}

static bool measure_both_ends(const char *dir, Result *out)
{
    TextBuf *tb = NULL;
    FileMeta meta;
    static const u8 byte = 'x';
    u64 rss_before;
    u64 rss_after;
    u64 begin;
    u64 end;
    size_t i;

    if (!load_buffer(dir, "1g-code", &tb, &meta) ||
        !rss_bytes(&rss_before) || !now_ns(&begin))
        return false;
    for (i = 0U; i < INSERT_SAMPLES; i++) {
        ByteOff at = (i & 1U) == 0U ? BYTEOFF(0U)
                                         : BYTEOFF(yew_textbuf_len(tb));

        yew_textbuf_insert(tb, at, &byte, 1U);
    }
    if (!now_ns(&end) || !rss_bytes(&rss_after)) {
        yew_textbuf_free(tb);
        yew_filemeta_dispose(&meta);
        return false;
    }
    *out = (Result){"edit.ends.1g-code", end - begin, end - begin,
                    end - begin,
                    rss_after > rss_before ? rss_after - rss_before : 0U,
                    UINT64_C(1000000000), UINT64_C(8) * 1024U * 1024U,
                    false};
    yew_textbuf_free(tb);
    yew_filemeta_dispose(&meta);
    return true;
}

static bool measure_undo(const char *dir, Result *out)
{
    TextBuf *tb = NULL;
    FileMeta meta;
    UndoTree *undo;
    EditCtx edit;
    static const u8 byte = 'u';
    u64 begin;
    u64 end;
    size_t i;
    bool ok = false;

    if (!load_buffer(dir, "100m-code", &tb, &meta))
        return false;
    undo = yew_undo_new(tb);
    if (undo == NULL) {
        yew_textbuf_free(tb);
        yew_filemeta_dispose(&meta);
        return false;
    }
    /* Measure the undo engine, not the file-backed crash journal. */
    edit = (EditCtx){tb, NULL, NULL, 0U, NULL, undo, NULL, NULL, NULL, 0,
                     NULL, NULL, {0}, 0U};
    if (!now_ns(&begin))
        goto done;
    for (i = 0U; i < INSERT_SAMPLES; i++) {
        yew_undo_begin(&edit, YEW_TXN_TYPE);
        yew_edit_insert(&edit, BYTEOFF(yew_textbuf_len(tb)), &byte, 1U);
        yew_undo_end(&edit);
        yew_undo_boundary(undo);
    }
    for (i = 0U; i < INSERT_SAMPLES; i++) {
        if (!yew_undo(&edit))
            goto done;
    }
    if (!now_ns(&end))
        goto done;
    *out = (Result){"undo.100m-code", end - begin, end - begin,
                    end - begin, undo->bytes_live + undo->bytes_dead,
                    UINT64_C(400000000), undo->bytes_max, false};
    ok = true;
done:
    yew_undo_free(undo);
    yew_textbuf_free(tb);
    yew_filemeta_dispose(&meta);
    return ok;
}

static int assess(const Result *result, const Baselines *baselines,
                  bool advisory, bool compare_baseline)
{
    const Baseline *baseline = compare_baseline
                                   ? baseline_find(baselines, result->name)
                                   : NULL;
    bool absolute = false;
    bool relative = false;

    if (!result->informational) {
        absolute = (result->abs_p99 != 0U &&
                    result->p99 > result->abs_p99) ||
                   (result->abs_rss != 0U &&
                    result->rss > result->abs_rss);
    }
    if (baseline != NULL && baseline->p99 != 0U)
        relative = result->p99 > baseline->p99 + baseline->p99 / 10U;
    (void)printf("%-28s p50_ns=%llu p99_ns=%llu max_ns=%llu rss_bytes=%llu",
                 result->name, (unsigned long long)result->p50,
                 (unsigned long long)result->p99,
                 (unsigned long long)result->max,
                 (unsigned long long)result->rss);
    if (result->informational)
        (void)printf(" informational");
    if (absolute)
        (void)printf(" ABSOLUTE-FAIL");
    if (relative)
        (void)printf(advisory ? " relative-warning" : " RELATIVE-FAIL");
    if (baseline == NULL)
        (void)printf(" no-baseline");
    else if (result->p99 * 5U < baseline->p99 * 4U)
        (void)printf(" rebaseline-me");
    (void)putchar('\n');
    return absolute || (relative && !advisory) ? 1 : 0;
}

static bool baseline_selftest_parse(const char *text, Baselines *set)
{
    FILE *file = tmpfile();
    bool ok;

    if (file == NULL)
        return false;
    if (fputs(text, file) == EOF || fflush(file) != 0 ||
        fseek(file, 0L, SEEK_SET) != 0) {
        (void)fclose(file);
        return false;
    }
    ok = baselines_read_file(file, set);
    if (fclose(file) != 0)
        ok = false;
    return ok;
}

static int baseline_selftest(void)
{
    static const char valid[] =
        "# yew perf baseline v2  runner=perf-test\n"
        "# calib scale_permille=1000 c1=11 c2=22 c3=33\n"
        "metric.full 1 2 3 4 measured on the pinned test runner\n"
        "metric.scalar 9 ratio bound inherited unchanged\n";
    static const char empty_why[] =
        "# yew perf baseline v2  runner=perf-test\n"
        "# calib scale_permille=1000 c1=11 c2=22 c3=33\n"
        "metric.full 1 2 3 4\n";
    static const char legacy[] =
        "# yew perf baseline v1  runner=perf-test\n"
        "metric.full 1 2 3 4 old row\n";
    static const char placeholder[] =
        "# yew perf baseline v2  runner=perf-test\n"
        "# calib scale_permille=0 c1=0 c2=0 c3=0\n"
        "metric.full 1 2 3 4 PLACEHOLDER\n";
    Baselines parsed = {0};
    Baselines invalid = {0};
    Baselines roundtrip = {0};
    const Result update_result = {
        "metric.scalar", 10U, 11U, 12U, 13U, 0U, 0U, false
    };
    FILE *written = NULL;
    int failed = 0;

#define BASELINE_CHECK(condition, label)                                      \
    do {                                                                       \
        if (!(condition)) {                                                    \
            (void)fprintf(stderr, "baseline-v2-selftest: FAIL %s\n", label); \
            failed = 1;                                                        \
        }                                                                      \
    } while (0)

    BASELINE_CHECK(baseline_selftest_parse(valid, &parsed),
                   "accepts a complete v2 document");
    BASELINE_CHECK(strcmp(parsed.runner_id, "perf-test") == 0,
                   "preserves runner id");
    BASELINE_CHECK(calib_complete(&parsed),
                   "recognizes complete calibration metadata");
    BASELINE_CHECK(parsed.len == 2U && !parsed.data[0].scalar &&
                       parsed.data[1].scalar,
                   "parses metric and scalar rows");
    BASELINE_CHECK(parsed.len == 2U &&
                       strcmp(parsed.data[0].why,
                              "measured on the pinned test runner") == 0,
                   "preserves the mandatory why");
    BASELINE_CHECK(baselines_apply_results(&parsed, &update_result, 1U,
                                           "measured after parser change") &&
                       !parsed.data[1].scalar && parsed.data[1].p99 == 11U &&
                       strcmp(parsed.data[1].why,
                              "measured after parser change") == 0,
                   "updates a row and its why together");
    written = tmpfile();
    BASELINE_CHECK(written != NULL &&
                       baselines_write_file(written, parsed.runner_id,
                                            &parsed) &&
                       fflush(written) == 0 &&
                       fseek(written, 0L, SEEK_SET) == 0 &&
                       baselines_read_file(written, &roundtrip),
                   "writes a parseable v2 document");
    BASELINE_CHECK(roundtrip.len == parsed.len &&
                       roundtrip.scale_permille == parsed.scale_permille &&
                       strcmp(roundtrip.data[1].why,
                              parsed.data[1].why) == 0,
                   "round-trips calibration and why metadata");
    BASELINE_CHECK(!baseline_selftest_parse(empty_why, &invalid),
                   "rejects an empty why");
    free(invalid.data);
    invalid = (Baselines){0};
    BASELINE_CHECK(!baseline_selftest_parse(legacy, &invalid),
                   "rejects a legacy v1 header");
    free(invalid.data);
    invalid = (Baselines){0};
    BASELINE_CHECK(!baseline_selftest_parse(placeholder, &invalid),
                   "rejects a placeholder why");
    BASELINE_CHECK(!scale_materially_differs(1000U, 1150U) &&
                       scale_materially_differs(1000U, 1151U),
                   "uses the 15 percent calibration-drift boundary");
    BASELINE_CHECK(update_why_valid("allocator fix on pinned runner") &&
                       !update_why_valid("") &&
                       !update_why_valid("s11 initial"),
                   "requires an explicit non-initial update why");
    free(invalid.data);
    if (written != NULL)
        (void)fclose(written);
    free(roundtrip.data);
    free(parsed.data);
#undef BASELINE_CHECK
    if (failed == 0)
        (void)printf("baseline-v2-selftest: 13 checks passed\n");
    return failed;
}

static void usage(FILE *out)
{
    (void)fprintf(out,
        "usage: perf_textbuf --fixtures DIR --baseline FILE --runner-id ID "
        "[--advisory] [--huge] [--update] | --baseline-selftest\n");
}

int main(int argc, char **argv)
{
    const char *fixtures = NULL;
    const char *baseline_path = NULL;
    const char *runner_id = NULL;
    Baselines baselines = {0};
    Result results[20];
    size_t result_count = 0U;
    bool advisory = false;
    bool huge = false;
    bool update = false;
    bool baseline_loaded;
    bool compare_baseline = false;
    u32 current_scale = 0U;
    const char *update_why = NULL;
    int failed = 0;
    int i;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--baseline-selftest") == 0)
            return argc == 2 ? baseline_selftest() : 2;
        if (i + 1 < argc && strcmp(argv[i], "--fixtures") == 0)
            fixtures = argv[++i];
        else if (i + 1 < argc && strcmp(argv[i], "--baseline") == 0)
            baseline_path = argv[++i];
        else if (i + 1 < argc && strcmp(argv[i], "--runner-id") == 0)
            runner_id = argv[++i];
        else if (strcmp(argv[i], "--advisory") == 0)
            advisory = true;
        else if (strcmp(argv[i], "--huge") == 0)
            huge = true;
        else if (strcmp(argv[i], "--update") == 0)
            update = true;
        else {
            usage(stderr);
            return 2;
        }
    }
    if (fixtures == NULL || baseline_path == NULL || runner_id == NULL) {
        usage(stderr);
        return 2;
    }
    baseline_loaded = baselines_read(baseline_path, &baselines);
    if (!baseline_loaded) {
        (void)fprintf(stderr,
                      "perf_textbuf: baseline unavailable; advisory mode\n");
        advisory = true;
    }
    if (getenv("YEW_PERF_ADVISORY") != NULL &&
        strcmp(getenv("YEW_PERF_ADVISORY"), "0") != 0)
        advisory = true;
    if (baselines.runner_id[0] == '\0' ||
        strcmp(baselines.runner_id, runner_id) != 0)
        advisory = true;
    (void)parse_u32_decimal(getenv("YEW_CALIB_SCALE_PERMILLE"),
                            &current_scale);
    if (baseline_loaded && strcmp(baselines.runner_id, runner_id) == 0 &&
        current_scale != 0U) {
        if (!calib_complete(&baselines)) {
            if (!advisory) {
                (void)fprintf(stderr,
                              "perf: baseline has no measured calibration; "
                              "no verdict\n");
                free(baselines.data);
                return 75;
            }
        } else if (scale_materially_differs(baselines.scale_permille,
                                            current_scale)) {
            (void)fprintf(stderr,
                          "perf: calibration drift %u -> %u; rebaseline or "
                          "fix the runner\n",
                          baselines.scale_permille, current_scale);
            free(baselines.data);
            return 75;
        } else {
            compare_baseline = true;
        }
    }
    if (update) {
        update_why = getenv("YEW_PERF_UPDATE_WHY");
        if (!baseline_loaded || !update_why_valid(update_why) ||
            !baselines_set_update_calib(&baselines)) {
            (void)fprintf(stderr,
                          "perf_textbuf: --update requires YEW_PERF_UPDATE_WHY "
                          "and nonzero YEW_CALIB_{SCALE_PERMILLE,C1_NS,C2_NS,"
                          "C3_NS}\n");
            free(baselines.data);
            return 2;
        }
        compare_baseline = calib_complete(&baselines);
    }
    (void)printf("perf-textbuf: runner-id=%s mode=%s suite=%s\n", runner_id,
                 advisory ? "advisory" : "gating", huge ? "huge" : "quick");
    if (!measure_open(fixtures, "100m-code", &results[result_count],
                      UINT64_C(150000000), UINT64_C(160) * 1024U * 1024U,
                      false))
        goto error;
    results[result_count++].name = "open.100m-code";
    if (!measure_open(fixtures, "100m-utf8", &results[result_count],
                      UINT64_C(150000000), 0U, false))
        goto error;
    results[result_count++].name = "open.100m-utf8";
    if (!measure_open(fixtures, "100m-allnl", &results[result_count],
                      0U, 0U, true))
        goto error;
    results[result_count++].name = "rss.100m-allnl";
    if (!measure_undo(fixtures, &results[result_count++]))
        goto error;
    if (huge) {
        Result query_start;
        Result query_of;

        if (!measure_open(fixtures, "1g-code", &results[result_count],
                          UINT64_C(2000000000),
                          (UINT64_C(16) * 1024U * 1024U * 1024U) / 10U,
                          false))
            goto error;
        results[result_count++].name = "open.1g-code";
        if (!measure_open(fixtures, "1g-noline", &results[result_count],
                          UINT64_C(2000000000), 0U, false))
            goto error;
        results[result_count++].name = "open.1g-noline";
        if (!measure_insert(fixtures, "insert.head.1g-code", true,
                            &results[result_count++]) ||
            !measure_insert(fixtures, "insert.eof.1g-code", false,
                            &results[result_count++]) ||
            !measure_queries(fixtures, "1g-code", &query_start, &query_of))
            goto error;
        query_start.name = "line_start.1g-code";
        results[result_count++] = query_start;
        query_of.name = "line_of.1g-code";
        results[result_count++] = query_of;
        if (!measure_queries(fixtures, "1g-noline", &query_start, &query_of))
            goto error;
        query_start.name = "line_start.1g-noline";
        results[result_count++] = query_start;
        if (!measure_materialize(fixtures, &results[result_count++]) ||
            !measure_both_ends(fixtures, &results[result_count++]) ||
            !measure_atomic_save(fixtures, &results[result_count++]))
            goto error;
    }
    for (i = 0; (size_t)i < result_count; i++)
        failed |= assess(&results[i], &baselines, advisory,
                         compare_baseline);
    if (!failed && update &&
        !baselines_update(baseline_path, runner_id, &baselines, results,
                          result_count, update_why))
        goto error;
    if (!failed && update)
        (void)printf("perf-textbuf: updated %s\n", baseline_path);
    free(baselines.data);
    return failed;

error:
    (void)fprintf(stderr, "perf_textbuf: measurement failed: %s\n",
                  strerror(errno));
    free(baselines.data);
    return 2;
}
