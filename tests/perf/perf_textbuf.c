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
} Baseline;

typedef struct {
    Baseline *data;
    size_t len;
    size_t cap;
    char runner_id[80];
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

static bool baselines_read(const char *path, Baselines *set)
{
    FILE *file = fopen(path, "r");
    char line[512];

    if (file == NULL)
        return false;
    while (fgets(line, sizeof(line), file) != NULL) {
        Baseline value;
        unsigned long long p50;
        unsigned long long p99;
        unsigned long long max;
        unsigned long long rss;
        int count;

        if (line[0] == '#') {
            const char *runner = strstr(line, "runner=");

            if (runner != NULL)
                (void)sscanf(runner + 7, "%79s", set->runner_id);
            continue;
        }
        if (line[0] == '\n')
            continue;
        (void)memset(&value, 0, sizeof(value));
        count = sscanf(line, "%79s %llu %llu %llu %llu", value.name,
                       &p50, &p99, &max, &rss);
        value.p50 = (u64)p50;
        value.p99 = (u64)p99;
        value.max = (u64)max;
        value.rss = (u64)rss;
        if (count == 4) {
            value.rss = value.max;
            value.max = value.p99;
        } else if (count != 5) {
            (void)fprintf(stderr, "perf_textbuf: malformed baseline: %s",
                          line);
            (void)fclose(file);
            return false;
        }
        if (!baselines_push(set, &value)) {
            (void)fclose(file);
            return false;
        }
    }
    return fclose(file) == 0;
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

static bool baselines_update(const char *path, const char *runner_id,
                             Baselines *set, const Result *results,
                             size_t result_count)
{
    FILE *file;
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
    }
    file = fopen(path, "wb");
    if (file == NULL)
        return false;
    if (fprintf(file, "# yew perf baseline v1  runner=%s\n", runner_id) <
            0 ||
        fprintf(file,
                "# metric                         p50_ns        p99_ns"
                "        max_ns     rss_bytes\n") < 0)
        goto fail;
    for (i = 0U; i < set->len; i++) {
        const Baseline *baseline = &set->data[i];

        if (strcmp(baseline->name, "rss.100m-allnl") == 0 &&
            fprintf(file,
                    "# Informational: all-newline input is exempt from the "
                    "1.6x RSS gate (s07).\n") < 0)
            goto fail;
        if (fprintf(file, "%-28s %14llu %14llu %14llu %14llu\n",
                    baseline->name, (unsigned long long)baseline->p50,
                    (unsigned long long)baseline->p99,
                    (unsigned long long)baseline->max,
                    (unsigned long long)baseline->rss) < 0)
            goto fail;
    }
    return fclose(file) == 0;

fail:
    (void)fclose(file);
    return false;
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
                  bool advisory)
{
    const Baseline *baseline = baseline_find(baselines, result->name);
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

static void usage(FILE *out)
{
    (void)fprintf(out,
        "usage: perf_textbuf --fixtures DIR --baseline FILE --runner-id ID "
        "[--advisory] [--huge] [--update]\n");
}

int main(int argc, char **argv)
{
    const char *fixtures = NULL;
    const char *baseline_path = NULL;
    const char *runner_id = NULL;
    Baselines baselines = {NULL, 0U, 0U, {0}};
    Result results[20];
    size_t result_count = 0U;
    bool advisory = false;
    bool huge = false;
    bool update = false;
    int failed = 0;
    int i;

    for (i = 1; i < argc; i++) {
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
    if (!baselines_read(baseline_path, &baselines)) {
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
        failed |= assess(&results[i], &baselines, advisory);
    if (!failed && update &&
        !baselines_update(baseline_path, runner_id, &baselines, results,
                          result_count))
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
