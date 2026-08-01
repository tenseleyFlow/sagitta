#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "term/grid.h"
#include "term/render.h"
#include "util/arena.h"
#include "util/buf.h"
#include "util/intern.h"

enum { PERF_SAMPLES = 101 };

typedef enum {
    PERF_FULL,
    PERF_CELL,
    PERF_ROW,
    PERF_ZERO
} PerfKind;

typedef struct {
    const char *name;
    u16 rows;
    u16 cols;
    PerfKind kind;
    i64 budget_ns;
    size_t byte_limit;
    i64 baseline_ns;
    i64 median_ns;
    i64 p99_ns;
    size_t bytes;
} PerfCase;

static volatile u64 perf_sink;

static i64 now_ns(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        (void)fprintf(stderr, "perf_render: clock_gettime: %s\n",
                      strerror(errno));
        return -1;
    }
    return (i64)ts.tv_sec * INT64_C(1000000000) + ts.tv_nsec;
}

static void stable_sort_i64(i64 *values, size_t len)
{
    size_t i;

    for (i = 1u; i < len; i++) {
        i64 value = values[i];
        size_t j = i;

        while (j > 0u && values[j - 1u] > value) {
            values[j] = values[j - 1u];
            j--;
        }
        values[j] = value;
    }
}

static Cell ascii_cell(const Grid *grid, u8 byte)
{
    Cell cell = grid->blank;

    cell.utf8[0] = byte;
    return cell;
}

static void mutate(Grid *grid, PerfKind kind, u8 byte)
{
    Cell cell = ascii_cell(grid, byte);

    switch (kind) {
    case PERF_FULL:
        for (u16 row = 0u; row < grid->rows; row++)
            sag_grid_fill(grid, row, 0u, grid->cols, cell);
        break;
    case PERF_CELL:
        sag_grid_fill(grid, 25u, 100u, 101u, cell);
        break;
    case PERF_ROW:
        sag_grid_fill(grid, 25u, 0u, grid->cols, cell);
        break;
    case PERF_ZERO:
        break;
    }
}

static bool load_baselines(PerfCase *cases, size_t count)
{
    FILE *file = fopen("tests/perf/baselines/render.txt", "r");
    char line[256];

    if (file == NULL) {
        (void)fprintf(stderr, "perf_render: cannot read render baseline: %s\n",
                      strerror(errno));
        return false;
    }
    while (fgets(line, sizeof(line), file) != NULL) {
        char name[64];
        long long median;
        size_t i;

        if (sscanf(line, "%63s %lld", name, &median) != 2 || median <= 0)
            continue;
        for (i = 0u; i < count; i++) {
            if (strcmp(cases[i].name, name) == 0)
                cases[i].baseline_ns = (i64)median;
        }
    }
    if (ferror(file)) {
        (void)fprintf(stderr, "perf_render: failed reading baseline\n");
        (void)fclose(file);
        return false;
    }
    if (fclose(file) != 0)
        return false;
    for (size_t i = 0u; i < count; i++) {
        if (cases[i].baseline_ns <= 0) {
            (void)fprintf(stderr, "perf_render: missing baseline for %s\n",
                          cases[i].name);
            return false;
        }
    }
    return true;
}

static bool measure(PerfCase *pc)
{
    Arena arena;
    Interner interner;
    Grid grid;
    Render render;
    TtyCaps caps;
    Bytebuf output;
    i64 samples[PERF_SAMPLES];
    size_t expected_bytes = SIZE_MAX;
    size_t sample;
    bool ok = true;

    arena_init(&arena);
    interner_init(&interner, &arena);
    if (!sag_grid_init(&grid, &interner, pc->rows, pc->cols)) {
        interner_free(&interner);
        arena_free_all(&arena);
        return false;
    }
    memset(&caps, 0, sizeof(caps));
    sag_render_init(&render, &caps, NULL);
    bytebuf_init(&output);

    /* Establish a known front buffer before measuring alternating changes. */
    mutate(&grid, pc->kind, (u8)'a');
    (void)sag_render_frame(&render, &grid, &output);
    sag_grid_flip(&grid);
    for (sample = 0u; sample < PERF_SAMPLES; sample++) {
        i64 start;
        i64 elapsed;
        size_t emitted;

        mutate(&grid, pc->kind, (u8)((sample & 1u) != 0u ? 'a' : 'b'));
        output.len = 0u;
        start = now_ns();
        if (start < 0) {
            ok = false;
            break;
        }
        emitted = sag_render_frame(&render, &grid, &output);
        elapsed = now_ns() - start;
        if (elapsed < 0 || emitted != output.len) {
            ok = false;
            break;
        }
        if (expected_bytes == SIZE_MAX)
            expected_bytes = emitted;
        else if (emitted != expected_bytes) {
            (void)fprintf(stderr,
                          "perf_render: %s byte count changed (%zu != %zu)\n",
                          pc->name, emitted, expected_bytes);
            ok = false;
            break;
        }
        samples[sample] = elapsed;
        perf_sink += (u64)emitted;
        sag_grid_flip(&grid);
    }
    if (ok) {
        stable_sort_i64(samples, PERF_SAMPLES);
        pc->median_ns = samples[PERF_SAMPLES / 2u];
        pc->p99_ns = samples[99u];
        pc->bytes = expected_bytes;
    }
    bytebuf_free(&output);
    sag_grid_free(&grid);
    interner_free(&interner);
    arena_free_all(&arena);
    return ok;
}

int main(void)
{
    PerfCase cases[] = {
        {"full_200x50", 50u, 200u, PERF_FULL, 2000000, SIZE_MAX, 0, 0, 0, 0},
        {"full_80x24", 24u, 80u, PERF_FULL, 400000, 2200u, 0, 0, 0, 0},
        {"single_cell_200x50", 50u, 200u, PERF_CELL, 50000, 32u, 0, 0, 0, 0},
        {"single_row_200x50", 50u, 200u, PERF_ROW, 150000, SIZE_MAX, 0, 0, 0, 0},
        {"zero_damage_200x50", 50u, 200u, PERF_ZERO, 2000, 0u, 0, 0, 0, 0}
    };
    size_t i;
    int status = 0;

    if (!load_baselines(cases, SAG_ARRAY_LEN(cases)))
        return 2;
    for (i = 0u; i < SAG_ARRAY_LEN(cases); i++) {
        bool over_budget;
        bool over_baseline;

        if (!measure(&cases[i])) {
            (void)fprintf(stderr, "perf_render: %s measurement failed\n",
                          cases[i].name);
            return 2;
        }
        over_budget = cases[i].median_ns > cases[i].budget_ns;
        over_baseline = cases[i].median_ns >
                        cases[i].baseline_ns + cases[i].baseline_ns / 5;
        (void)printf("%s %lld %lld %zu%s%s\n", cases[i].name,
                     (long long)cases[i].median_ns,
                     (long long)cases[i].p99_ns, cases[i].bytes,
                     over_budget ? " ADVISORY-OVER-BUDGET" : "",
                     over_baseline ? " ADVISORY-REGRESSION" : "");
        if (cases[i].byte_limit != SIZE_MAX &&
            cases[i].bytes > cases[i].byte_limit) {
            (void)fprintf(stderr,
                          "perf_render: %s bytes=%zu exceeds hard limit=%zu\n",
                          cases[i].name, cases[i].bytes,
                          cases[i].byte_limit);
            status = 1;
        }
    }
    return status;
}
