#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "edit/ed.h"
#include "term/grid.h"
#include "ui/cmdcomp.h"
#include "ui/cmdline.h"

enum {
    PERF_COMP_ENTRIES = 10000,
    PERF_COMP_SAMPLES = 101,
    PERF_COMP_WARMUPS = 5,
    PERF_COMP_ROWS = 24,
    PERF_COMP_COLS = 100,
    PERF_COMP_BUDGET_NS = 5000000
};

static volatile u64 perf_comp_sink;

static i64 now_ns(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        (void)fprintf(stderr, "perf_cmdcomp: clock_gettime: %s\n",
                      strerror(errno));
        return -1;
    }
    return (i64)ts.tv_sec * INT64_C(1000000000) + ts.tv_nsec;
}

static void stable_sort_i64(i64 *values, size_t len)
{
    size_t i;

    for (i = 1U; i < len; i++) {
        i64 value = values[i];
        size_t j = i;

        while (j != 0U && values[j - 1U] > value) {
            values[j] = values[j - 1U];
            j--;
        }
        values[j] = value;
    }
}

static bool fixture_name(char *path, size_t cap, const char *root, u32 index)
{
    int n = snprintf(path, cap, "%s/entry%05u", root, (unsigned)index);

    return n >= 0 && (size_t)n < cap;
}

static bool fixture_create(char root[64])
{
    u32 i;

    (void)strcpy(root, "/tmp/sagitta-perf-cmdcomp-XXXXXX");
    if (mkdtemp(root) == NULL) {
        (void)fprintf(stderr, "perf_cmdcomp: mkdtemp: %s\n",
                      strerror(errno));
        root[0] = '\0';
        return false;
    }
    for (i = 0U; i < PERF_COMP_ENTRIES; i++) {
        char path[128];
        int fd;

        if (!fixture_name(path, sizeof(path), root, i))
            return false;
        fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
        if (fd < 0) {
            (void)fprintf(stderr, "perf_cmdcomp: create %s: %s\n", path,
                          strerror(errno));
            return false;
        }
        if (close(fd) != 0) {
            (void)fprintf(stderr, "perf_cmdcomp: close %s: %s\n", path,
                          strerror(errno));
            return false;
        }
    }
    return true;
}

static bool fixture_remove(const char *root)
{
    u32 i;
    bool ok = true;

    for (i = 0U; i < PERF_COMP_ENTRIES; i++) {
        char path[128];

        if (!fixture_name(path, sizeof(path), root, i) || unlink(path) != 0)
            ok = false;
    }
    if (rmdir(root) != 0)
        ok = false;
    return ok;
}

static bool measure_once(const char *root, i64 *elapsed_out)
{
    static const u8 prompt[] = "file.open entry";
    Ed ed;
    Vec_CompItem items = {0};
    i64 start;
    i64 elapsed;
    u32 total;
    bool ok = false;

    sag_ed_init(&ed);
    ed.ws.dir = (char *)root;
    if (!sag_grid_init(&ed.grid, &ed.interner,
                       PERF_COMP_ROWS, PERF_COMP_COLS))
        goto done;
    ed.grid_ready = true;
    ed.cmdline.buf = sag_textbuf_from_bytes(prompt, sizeof(prompt) - 1U);
    if (ed.cmdline.buf == NULL)
        goto done;
    ed.cmdline.active = true;
    ed.cmdline.kind = SAG_PROMPT_CMD;
    ed.cmdline.cur = (Cursor){BYTEOFF(sizeof(prompt) - 1U), {0U},
                              BYTEOFF(sizeof(prompt) - 1U)};
    ed.mode = SAG_MODE_E;

    start = now_ns();
    if (start < 0)
        goto done;
    total = sag_comp_enumerate(&ed, SAG_COMP_PATH, "entry", &items);
    ed.cmdline.menu.items = items;
    items = (Vec_CompItem){0};
    ed.cmdline.comp_total = total;
    sag_cmdline_draw(&ed, (Rect){0U, PERF_COMP_ROWS - 1U,
                                 PERF_COMP_COLS, 1U});
    elapsed = now_ns() - start;
    if (elapsed < 0 || total != PERF_COMP_ENTRIES ||
        ed.cmdline.menu.items.len != SAG_COMP_MAX)
        goto done;
    perf_comp_sink ^= (u64)total + ed.grid.cur_col + ed.grid.cur_row;
    *elapsed_out = elapsed;
    ok = true;

done:
    Vec_CompItem_free(&items);
    Vec_CompItem_free(&ed.cmdline.menu.items);
    sag_textbuf_free(ed.cmdline.buf);
    ed.cmdline.buf = NULL;
    ed.cmdline.active = false;
    sag_ed_free(&ed);
    return ok;
}

int main(void)
{
    char root[64];
    i64 samples[PERF_COMP_SAMPLES];
    size_t sample;
    i64 p99;
    i64 median;
    int status = 0;

    if (!fixture_create(root)) {
        if (root[0] != '\0')
            (void)fixture_remove(root);
        sag_cmd_shutdown();
        return 2;
    }
    for (sample = 0U; sample < PERF_COMP_WARMUPS; sample++) {
        i64 ignored;

        if (!measure_once(root, &ignored)) {
            status = 2;
            goto done;
        }
    }
    for (sample = 0U; sample < PERF_COMP_SAMPLES; sample++) {
        if (!measure_once(root, &samples[sample])) {
            status = 2;
            goto done;
        }
    }
    stable_sort_i64(samples, PERF_COMP_SAMPLES);
    median = samples[PERF_COMP_SAMPLES / 2U];
    p99 = samples[(PERF_COMP_SAMPLES * 99U + 99U) / 100U - 1U];
    (void)printf("perf-cmdcomp: entries=%u kept=%u median_ms=%.3f "
                 "p99_ms=%.3f budget_ms=%.3f%s\n",
                 PERF_COMP_ENTRIES, SAG_COMP_MAX,
                 (double)median / 1000000.0, (double)p99 / 1000000.0,
                 (double)PERF_COMP_BUDGET_NS / 1000000.0,
                 p99 <= PERF_COMP_BUDGET_NS ? " ok" : " FAIL");
    if (p99 > PERF_COMP_BUDGET_NS)
        status = 1;

done:
    if (!fixture_remove(root)) {
        (void)fprintf(stderr, "perf_cmdcomp: fixture cleanup failed\n");
        if (status == 0)
            status = 2;
    }
    sag_cmd_shutdown();
    return status;
}
