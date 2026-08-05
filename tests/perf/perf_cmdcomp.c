/*
 * Sprint 18.5 §4/§10: the per-KEYSTROKE completion gate.
 *
 * Before this sprint the menu was built once, on Tab, and measuring one
 * enumerate-and-draw was the right gate.  Now the menu is live: every
 * character the user types re-runs the filter and repaints the rows, so
 * the budget that matters is invariant 4's keypress->paint p99 <= 5 ms,
 * applied to EACH key of a realistic prefix -- including the one key that
 * pays for the cold opendir of a 10,000-entry directory.
 *
 * Timing sag_cmdline_key + sag_cmdline_draw is deliberate: those are the
 * two calls the event loop makes per key.  Timing the filter alone would
 * pass while the draw blew the budget.
 */

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

/*
 * The typed prefix walks through three regimes on purpose: the command
 * name (a small static set), the space (which switches source to PATH and
 * pays the cold opendir), and then digits that narrow 10,000 entries down
 * to one -- the narrowing keystrokes are the ones §4's cache exists for.
 */
#define PERF_COMP_PREFIX "file.open entry000"

enum {
    PERF_COMP_ENTRIES = 10000,
    PERF_COMP_TRIALS = 11,
    PERF_COMP_KEYS = sizeof(PERF_COMP_PREFIX) - 1U,
    PERF_COMP_SAMPLES = PERF_COMP_TRIALS * PERF_COMP_KEYS,
    PERF_COMP_WARMUPS = 3,
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

static Key perf_key(char c)
{
    Key key = {0};

    key.code = (u32)(u8)c;
    key.kind = SAG_EV_KEY;
    key.ev = SAG_KEY_PRESS;
    key.ntext = 1U;
    key.text[0] = (u8)c;
    return key;
}

/*
 * One trial types the whole prefix, timing each key.  `samples` receives
 * PERF_COMP_KEYS values; `worst_key` reports which position was slowest,
 * which is the difference between "the cold opendir is the cost" and
 * "narrowing regressed".
 */
static bool measure_trial(const char *root, i64 *samples)
{
    Ed ed;
    Rect rect;
    u32 i;
    bool ok = false;

    sag_ed_init(&ed);
    if (!sag_ed_open_scratch(&ed))
        goto done;
    ed.ws.dir = (char *)root;
    if (!sag_grid_init(&ed.grid, &ed.interner,
                       PERF_COMP_ROWS, PERF_COMP_COLS))
        goto done;
    ed.grid_ready = true;
    ed.mode = SAG_MODE_E;
    rect.x = 0U;
    rect.y = PERF_COMP_ROWS - 1U;
    rect.w = PERF_COMP_COLS;
    rect.h = 1U;
    sag_cmdline_open(&ed, SAG_PROMPT_CMD, NULL);

    for (i = 0U; i < PERF_COMP_KEYS; i++) {
        Key key = perf_key(PERF_COMP_PREFIX[i]);
        i64 start = now_ns();
        i64 elapsed;

        if (start < 0)
            goto done;
        (void)sag_cmdline_key(&ed, &key);
        sag_cmdline_draw(&ed, rect);
        elapsed = now_ns() - start;
        if (elapsed < 0)
            goto done;
        samples[i] = elapsed;
        perf_comp_sink ^= (u64)ed.grid.cur_col + ed.grid.cur_row;
    }
    /* A gate that measures an empty menu measures nothing.  The last key
     * of the prefix leaves "entry000" selecting entry00000..entry00099. */
    if (ed.cmdline.menu.items.len == 0U) {
        (void)fprintf(stderr, "perf_cmdcomp: menu was empty\n");
        goto done;
    }
    ok = true;

done:
    sag_cmdline_dispose(&ed);
    ed.ws.dir = NULL;
    sag_ed_free(&ed);
    return ok;
}

int main(void)
{
    char root[64];
    i64 samples[PERF_COMP_SAMPLES];
    i64 worst_by_key[PERF_COMP_KEYS];
    size_t trial;
    size_t i;
    size_t worst_key = 0U;
    i64 p99;
    i64 median;
    int status = 0;

    if (!fixture_create(root)) {
        if (root[0] != '\0')
            (void)fixture_remove(root);
        sag_cmd_shutdown();
        return 2;
    }
    for (i = 0U; i < PERF_COMP_KEYS; i++)
        worst_by_key[i] = 0;
    for (trial = 0U; trial < PERF_COMP_WARMUPS; trial++) {
        i64 ignored[PERF_COMP_KEYS];

        if (!measure_trial(root, ignored)) {
            status = 2;
            goto done;
        }
    }
    for (trial = 0U; trial < PERF_COMP_TRIALS; trial++) {
        if (!measure_trial(root, &samples[trial * PERF_COMP_KEYS])) {
            status = 2;
            goto done;
        }
        for (i = 0U; i < PERF_COMP_KEYS; i++) {
            i64 value = samples[trial * PERF_COMP_KEYS + i];

            if (value > worst_by_key[i])
                worst_by_key[i] = value;
        }
    }
    for (i = 0U; i < PERF_COMP_KEYS; i++) {
        if (worst_by_key[i] > worst_by_key[worst_key])
            worst_key = i;
    }
    stable_sort_i64(samples, PERF_COMP_SAMPLES);
    median = samples[PERF_COMP_SAMPLES / 2U];
    p99 = samples[(PERF_COMP_SAMPLES * 99U + 99U) / 100U - 1U];
    (void)printf("perf-cmdcomp: entries=%u keys=%u samples=%u "
                 "median_ms=%.3f p99_ms=%.3f max_ms=%.3f "
                 "slowest_key=%u('%c') budget_ms=%.3f%s\n",
                 PERF_COMP_ENTRIES, (unsigned)PERF_COMP_KEYS,
                 (unsigned)PERF_COMP_SAMPLES,
                 (double)median / 1000000.0, (double)p99 / 1000000.0,
                 (double)samples[PERF_COMP_SAMPLES - 1U] / 1000000.0,
                 (unsigned)worst_key, PERF_COMP_PREFIX[worst_key],
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
