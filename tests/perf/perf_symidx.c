#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "edit/ed.h"
#include "text/edit.h"
#include "util/sort.h"
#include "ws/symidx.h"
#include "ws/symwalk.h"

enum {
    SYMIDX_PUMP_SAMPLES = 10000,
    SYMIDX_QUERY_SAMPLES = 1001,
    SYMIDX_QUERY_SYMBOLS = 15000,
    SYMIDX_WALK_FILES = 1000,
    SYMIDX_WALK_LINES_PER_FILE = 100,
    SYMIDX_PUMP_P99_BUDGET_NS = 500000,
    SYMIDX_QUERY_P99_BUDGET_NS = 400000,
    SYMIDX_TYPING_P99_BUDGET_NS = 5000000
};

typedef struct Timing {
    const char *name;
    u64 median_ns;
    u64 p99_ns;
    u64 maximum_ns;
    u64 baseline_median_ns;
    u64 baseline_p99_ns;
    u64 budget_p99_ns;
    bool gate_maximum;
} Timing;

static volatile u64 symidx_perf_sink;

static u64 now_ns(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        (void)fprintf(stderr, "perf_symidx: clock_gettime: %s\n",
                      strerror(errno));
        return 0U;
    }
    return (u64)ts.tv_sec * UINT64_C(1000000000) + (u64)ts.tv_nsec;
}

static int cmp_u64(const void *left, const void *right, void *ctx)
{
    const u64 a = *(const u64 *)left;
    const u64 b = *(const u64 *)right;

    (void)ctx;
    return a < b ? -1 : a > b ? 1 : 0;
}

static void summarize(u64 *samples, size_t len, Timing *out)
{
    yew_sort_stable(samples, len, sizeof(*samples), cmp_u64, NULL);
    out->median_ns = samples[len / 2U];
    out->p99_ns = samples[(len * 99U) / 100U];
    out->maximum_ns = samples[len - 1U];
}

static bool drain_index(Ed *ed)
{
    u32 guard = 0U;

    while (yew_symidx_pending(ed) && guard++ < 100000U)
        yew_symidx_pump(ed, INT64_MAX);
    return !yew_symidx_pending(ed);
}

static u8 *make_lines(size_t lines, bool distinct, size_t *len_out)
{
    const size_t line_cap = 40U;
    u8 *bytes = malloc(lines * line_cap);
    size_t at = 0U;
    size_t i;

    if (bytes == NULL)
        return NULL;
    for (i = 0U; i < lines; i++) {
        int n;

        if (distinct)
            n = snprintf((char *)bytes + at, line_cap,
                         "int symbol_%05zu(void);\n", i);
        else
            n = snprintf((char *)bytes + at, line_cap,
                         "int perf_symbol(void);\n");

        if (n <= 0 || (size_t)n >= line_cap) {
            free(bytes);
            return NULL;
        }
        at += (size_t)n;
    }
    *len_out = at;
    return bytes;
}

static bool insert_byte(Ed *ed, ByteOff at)
{
    static const u8 byte = (u8)'x';
    EditCtx edit = yew_ed_edit_ctx(ed);

    if (!yew_edit_insert(&edit, at, &byte, 1U))
        return false;
    yew_ed_finish_edit(ed, &edit);
    return true;
}

static bool measure_pump(Timing *out)
{
    size_t len;
    u8 *bytes = make_lines(SYMIDX_PUMP_SAMPLES, false, &len);
    u64 *samples = malloc(SYMIDX_PUMP_SAMPLES * sizeof(*samples));
    Ed ed;
    u32 i;
    bool ok = false;

    if (bytes == NULL || samples == NULL)
        goto done_alloc;
    yew_ed_init(&ed);
    if (!yew_ed_open_memory(&ed, bytes, len, "perf_symidx.c"))
        goto done_ed;
    if (!drain_index(&ed))
        goto done_ed;
    for (i = 0U; i < SYMIDX_PUMP_SAMPLES; i++) {
        Span line = yew_textbuf_line_span(ed.buffer.tb, LINENO(i));
        u64 started;

        if (!insert_byte(&ed, BYTEOFF(line.lo)))
            goto done_ed;
        started = now_ns();
        yew_symidx_pump(&ed, YEW_SYMIDX_BURST_US);
        samples[i] = now_ns() - started;
        symidx_perf_sink ^= samples[i] + ed.ws.sym_buf.len;
    }
    if (!drain_index(&ed))
        goto done_ed;
    summarize(samples, SYMIDX_PUMP_SAMPLES, out);
    ok = true;
done_ed:
    yew_ed_free(&ed);
done_alloc:
    free(samples);
    free(bytes);
    return ok;
}

static bool measure_query(Timing *out)
{
    size_t len;
    u8 *bytes = make_lines(SYMIDX_QUERY_SYMBOLS, true, &len);
    u64 *samples = malloc(SYMIDX_QUERY_SAMPLES * sizeof(*samples));
    SymHit hits[YEW_SYM_QUERY_MAX];
    SymQuery query = {"sym149", 6U, 0U, BYTEOFF(0U),
                      YEW_SYM_QUERY_MAX, true};
    Ed ed;
    u32 i;
    bool ok = false;

    if (bytes == NULL || samples == NULL)
        goto done_alloc;
    yew_ed_init(&ed);
    if (!yew_ed_open_memory(&ed, bytes, len, "perf_query.txt"))
        goto done_ed;
    if (!drain_index(&ed))
        goto done_ed;
    query.buf_id = ed.buffer.id;
    for (i = 0U; i < SYMIDX_QUERY_SAMPLES; i++) {
        u64 started = now_ns();
        u32 n = yew_symidx_query(&ed.ws, &query, hits,
                                 YEW_ARRAY_LEN(hits));

        samples[i] = now_ns() - started;
        if (n == 0U)
            goto done_ed;
        symidx_perf_sink ^= samples[i] + hits[0].name + n;
    }
    summarize(samples, SYMIDX_QUERY_SAMPLES, out);
    ok = true;
done_ed:
    yew_ed_free(&ed);
done_alloc:
    free(samples);
    free(bytes);
    return ok;
}

static bool write_walk_fixture(const char *root)
{
    u32 file;

    for (file = 0U; file < SYMIDX_WALK_FILES; file++) {
        char path[256];
        FILE *fp;
        u32 line;
        int n = snprintf(path, sizeof(path), "%s/file_%04u.c", root, file);

        if (n <= 0 || (size_t)n >= sizeof(path))
            return false;
        fp = fopen(path, "wb");
        if (fp == NULL)
            return false;
        for (line = 0U; line < SYMIDX_WALK_LINES_PER_FILE; line++) {
            if (fprintf(fp, "int workspace_%04u(void);\n", file) < 0) {
                (void)fclose(fp);
                return false;
            }
        }
        if (fclose(fp) != 0)
            return false;
    }
    return true;
}

static void remove_walk_fixture(const char *root)
{
    u32 file;

    for (file = 0U; file < SYMIDX_WALK_FILES; file++) {
        char path[256];
        int n = snprintf(path, sizeof(path), "%s/file_%04u.c", root, file);

        if (n > 0 && (size_t)n < sizeof(path))
            (void)unlink(path);
    }
    (void)rmdir(root);
}

static bool measure_walk_typing(Timing *typing_out, Timing *pump_out)
{
    char root[] = "/tmp/yew-perf-symidx-XXXXXX";
    size_t len;
    u8 *bytes = make_lines(SYMIDX_WALK_FILES, false, &len);
    u64 *samples = malloc(SYMIDX_WALK_FILES * sizeof(*samples));
    u64 *pump_samples = malloc(SYMIDX_WALK_FILES * sizeof(*pump_samples));
    Ed ed;
    u32 count = 0U;
    bool opened = false;
    bool ok = false;

    if (bytes == NULL || samples == NULL || pump_samples == NULL ||
        mkdtemp(root) == NULL)
        goto done;
    if (!write_walk_fixture(root))
        goto done_fixture;
    yew_ed_init(&ed);
    opened = true;
    if (!yew_ed_open_memory(&ed, bytes, len, "perf_walk_typing.c"))
        goto done_ed;
    ed.ws.dir = arena_strdup(&ed.arena, root);
    if (!drain_index(&ed))
        goto done_ed;
    yew_symwalk_start(&ed);
    while (ed.ws.sym_walk.running && count < SYMIDX_WALK_FILES) {
        Span line = yew_textbuf_line_span(ed.buffer.tb, LINENO(count));
        u64 started;
        u64 walk_started;

        if (!insert_byte(&ed, BYTEOFF(line.lo)))
            goto done_ed;
        started = now_ns();
        yew_symidx_pump(&ed, YEW_SYMIDX_BURST_US);
        walk_started = now_ns();
        yew_symwalk_pump(&ed, YEW_SYMWALK_BUDGET_US);
        pump_samples[count] = now_ns() - walk_started;
        samples[count] = now_ns() - started;
        symidx_perf_sink ^= samples[count] + ed.ws.sym_walk.files_done;
        count++;
    }
    while (ed.ws.sym_walk.running) {
        u64 started = now_ns();

        yew_symwalk_pump(&ed, YEW_SYMWALK_BUDGET_US);
        if (now_ns() - started > (u64)YEW_SYMWALK_BUDGET_US * 1000U)
            goto done_ed;
    }
    if (count == 0U || ed.ws.sym_walk.files_done != SYMIDX_WALK_FILES ||
        ed.ws.sym_walk.bytes_read == 0U || !drain_index(&ed))
        goto done_ed;
    summarize(samples, count, typing_out);
    summarize(pump_samples, count, pump_out);
    ok = true;
done_ed:
    if (opened)
        yew_ed_free(&ed);
done_fixture:
    remove_walk_fixture(root);
done:
    free(samples);
    free(pump_samples);
    free(bytes);
    return ok;
}

static bool load_baselines(Timing *rows, size_t count)
{
    FILE *fp = fopen("tests/perf/baselines/symidx.txt", "r");
    char line[160];

    if (fp == NULL)
        return false;
    while (fgets(line, sizeof(line), fp) != NULL) {
        char name[64];
        unsigned long long median;
        unsigned long long p99;
        size_t i;

        if (sscanf(line, "%63s %llu %llu", name, &median, &p99) != 3)
            continue;
        for (i = 0U; i < count; i++) {
            if (strcmp(rows[i].name, name) == 0) {
                rows[i].baseline_median_ns = (u64)median;
                rows[i].baseline_p99_ns = (u64)p99;
            }
        }
    }
    if (ferror(fp) || fclose(fp) != 0)
        return false;
    for (size_t i = 0U; i < count; i++) {
        if (rows[i].baseline_median_ns == 0U ||
            rows[i].baseline_p99_ns == 0U)
            return false;
    }
    return true;
}

int main(int argc, char **argv)
{
    Timing rows[] = {
        {"pump_insert_10kloc", 0U, 0U, 0U, 0U, 0U,
         SYMIDX_PUMP_P99_BUDGET_NS, false},
        {"walk_pump_100kloc", 0U, 0U, 0U, 0U, 0U,
         YEW_SYMWALK_BUDGET_US * 1000U, true},
        {"walk_typing_100kloc", 0U, 0U, 0U, 0U, 0U,
         SYMIDX_TYPING_P99_BUDGET_NS, false},
        {"query_15k", 0U, 0U, 0U, 0U, 0U,
         SYMIDX_QUERY_P99_BUDGET_NS, false}
    };
    bool measure = argc == 2 && strcmp(argv[1], "--measure") == 0;
    size_t i;
    int status = 0;

    if (argc > 2 || (argc == 2 && !measure)) {
        (void)fprintf(stderr, "usage: %s [--measure]\n", argv[0]);
        return 2;
    }
    if (!measure_pump(&rows[0]) ||
        !measure_walk_typing(&rows[2], &rows[1]) ||
        !measure_query(&rows[3])) {
        (void)fprintf(stderr, "perf_symidx: measurement invariant failed\n");
        return 2;
    }
    if (!measure && !load_baselines(rows, YEW_ARRAY_LEN(rows))) {
        (void)fprintf(stderr, "perf_symidx: missing or invalid baseline\n");
        return 2;
    }
    for (i = 0U; i < YEW_ARRAY_LEN(rows); i++) {
        u64 gated = rows[i].gate_maximum ? rows[i].maximum_ns
                                         : rows[i].p99_ns;
        bool regression = gated > rows[i].budget_p99_ns;

        (void)printf("symidx.%s median_ns=%llu p99_ns=%llu max_ns=%llu "
                     "budget_ns=%llu%s\n",
                     rows[i].name,
                     (unsigned long long)rows[i].median_ns,
                     (unsigned long long)rows[i].p99_ns,
                     (unsigned long long)rows[i].maximum_ns,
                     (unsigned long long)rows[i].budget_p99_ns,
                     regression ? " REGRESSION" : " ok");
        if (measure)
            (void)printf("%s %llu %llu\n", rows[i].name,
                         (unsigned long long)rows[i].median_ns,
                         (unsigned long long)rows[i].p99_ns);
        if (regression)
            status = 1;
    }
    return status;
}
