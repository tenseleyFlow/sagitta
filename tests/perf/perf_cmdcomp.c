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
 * Timing yew_cmdline_key + yew_cmdline_draw is deliberate: those are the
 * two calls the event loop makes per key.  Timing the filter alone would
 * pass while the draw blew the budget.
 */

#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "edit/ed.h"
#include "perf_policy.h"
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

/*
 * Sprint 57.18 §2's PATH-scan case.
 *
 * `!` switches the source to $PATH executables at word 0 and then
 * narrows, which is the same three regimes as above with one difference
 * that matters: $PATH is MANY directories, so the opendir claim is "one
 * per element for the whole prefix" rather than "one".  A $PATH built
 * from a fixed number of fixture directories is what makes that a
 * COUNT and not a guess — the machine's real $PATH would make the
 * expected number a property of whoever ran the gate.
 *
 * The per-entry stat the executable filter pays is deliberately inside
 * the measurement: it is the part of this source that a path scan does
 * not have, and it is the part most likely to blow the budget.
 */
#define PERF_EXEC_PREFIX "!entry000"

enum {
    PERF_COMP_ENTRIES = 10000,
    PERF_COMP_TRIALS = 11,
    PERF_COMP_KEYS = sizeof(PERF_COMP_PREFIX) - 1U,
    PERF_COMP_SAMPLES = PERF_COMP_TRIALS * PERF_COMP_KEYS,
    PERF_COMP_WARMUPS = 3,
    PERF_COMP_ROWS = 24,
    PERF_COMP_COLS = 100,
    PERF_COMP_BUDGET_NS = 5000000,
    /* Two elements, and 2 000 executables between them: enough that a
     * per-keystroke rescan would be visible in both the count and the
     * clock, small enough that creating them is not the gate's runtime. */
    PERF_EXEC_DIRS = 2,
    PERF_EXEC_PER_DIR = 1000,
    PERF_EXEC_KEYS = sizeof(PERF_EXEC_PREFIX) - 1U,
    PERF_EXEC_SAMPLES = PERF_COMP_TRIALS * PERF_EXEC_KEYS
};

static volatile u64 perf_comp_sink;

/*
 * `allowed` is how many opendir calls the case's source legitimately
 * makes for the WHOLE prefix: one for the path source's single
 * directory, one per $PATH element for the exec source.  Passed in
 * rather than hardcoded so the two cases share one policy and one
 * selftest.
 */
static bool result_failed(i64 p99_ns, u64 opendirs, u64 allowed,
                          bool advisory)
{
    return opendirs > allowed ||
           yew_perf_timing_failed((uint64_t)p99_ns,
                                  (uint64_t)PERF_COMP_BUDGET_NS,
                                  advisory);
}

static const char *result_verdict(i64 p99_ns, u64 opendirs, u64 allowed,
                                  bool advisory)
{
    if (opendirs > allowed)
        return " OPENDIR-FAIL";
    return yew_perf_timing_verdict((uint64_t)p99_ns,
                                   (uint64_t)PERF_COMP_BUDGET_NS,
                                   advisory);
}

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

    (void)strcpy(root, "/tmp/yew-perf-cmdcomp-XXXXXX");
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
    key.kind = YEW_EV_KEY;
    key.ev = YEW_KEY_PRESS;
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
static bool measure_trial(const char *root, i64 *samples, u64 *opendirs)
{
    Ed ed;
    u64 before_opendirs;
    Rect rect;
    u32 i;
    bool ok = false;

    yew_ed_init(&ed);
    if (!yew_ed_open_scratch(&ed))
        goto done;
    ed.ws.dir = (char *)root;
    if (!yew_grid_init(&ed.grid, &ed.interner,
                       PERF_COMP_ROWS, PERF_COMP_COLS))
        goto done;
    ed.grid_ready = true;
    ed.mode = YEW_MODE_E;
    rect.x = 0U;
    rect.y = PERF_COMP_ROWS - 1U;
    rect.w = PERF_COMP_COLS;
    rect.h = 1U;
    yew_cmdline_open(&ed, YEW_PROMPT_CMD, NULL);
    before_opendirs = yew_comp_listing_opendirs();

    for (i = 0U; i < PERF_COMP_KEYS; i++) {
        Key key = perf_key(PERF_COMP_PREFIX[i]);
        i64 start = now_ns();
        i64 elapsed;

        if (start < 0)
            goto done;
        (void)yew_cmdline_key(&ed, &key);
        yew_cmdline_draw(&ed, rect);
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
    *opendirs = yew_comp_listing_opendirs() - before_opendirs;
    ok = true;

done:
    yew_cmdline_dispose(&ed);
    ed.ws.dir = NULL;
    yew_ed_free(&ed);
    return ok;
}

/* ---------------------------------------------------------------- */
/* Sprint 57.18 §2: the $PATH-scan case                             */
/* ---------------------------------------------------------------- */

static bool exec_dir_name(char *path, size_t cap, const char *root, u32 d)
{
    int n = snprintf(path, cap, "%s/bin%u", root, (unsigned)d);

    return n >= 0 && (size_t)n < cap;
}

static bool exec_entry_name(char *path, size_t cap, const char *root, u32 d,
                            u32 i)
{
    int n = snprintf(path, cap, "%s/bin%u/entry%05u", root, (unsigned)d,
                     (unsigned)i);

    return n >= 0 && (size_t)n < cap;
}

static bool exec_fixture_create(const char *root, char *path_env,
                                size_t cap)
{
    u32 d;
    u32 i;
    size_t at = 0U;

    for (d = 0U; d < PERF_EXEC_DIRS; d++) {
        char path[160];
        int n;

        if (!exec_dir_name(path, sizeof(path), root, d) ||
            mkdir(path, 0700) != 0) {
            (void)fprintf(stderr, "perf_cmdcomp: mkdir %s: %s\n", path,
                          strerror(errno));
            return false;
        }
        n = snprintf(path_env + at, cap - at, "%s%s", d == 0U ? "" : ":",
                     path);
        if (n < 0 || (size_t)n >= cap - at)
            return false;
        at += (size_t)n;
        for (i = 0U; i < PERF_EXEC_PER_DIR; i++) {
            int fd;

            if (!exec_entry_name(path, sizeof(path), root, d,
                                 d * PERF_EXEC_PER_DIR + i))
                return false;
            fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0700);
            if (fd < 0 || close(fd) != 0) {
                (void)fprintf(stderr, "perf_cmdcomp: create %s: %s\n", path,
                              strerror(errno));
                return false;
            }
            /* open()'s mode is masked by umask and the exec bit is what
             * the source filters on. */
            if (chmod(path, 0700) != 0)
                return false;
        }
    }
    return true;
}

static bool exec_fixture_remove(const char *root)
{
    bool ok = true;
    u32 d;
    u32 i;

    for (d = 0U; d < PERF_EXEC_DIRS; d++) {
        char path[160];

        for (i = 0U; i < PERF_EXEC_PER_DIR; i++) {
            if (!exec_entry_name(path, sizeof(path), root, d,
                                 d * PERF_EXEC_PER_DIR + i) ||
                unlink(path) != 0)
                ok = false;
        }
        if (!exec_dir_name(path, sizeof(path), root, d) || rmdir(path) != 0)
            ok = false;
    }
    return ok;
}

static bool measure_exec_trial(const char *root, i64 *samples, u64 *opendirs)
{
    Ed ed;
    u64 before_opendirs;
    Rect rect;
    u32 i;
    bool ok = false;

    yew_ed_init(&ed);
    if (!yew_ed_open_scratch(&ed))
        goto done;
    ed.ws.dir = (char *)root;
    if (!yew_grid_init(&ed.grid, &ed.interner,
                       PERF_COMP_ROWS, PERF_COMP_COLS))
        goto done;
    ed.grid_ready = true;
    ed.mode = YEW_MODE_E;
    rect.x = 0U;
    rect.y = PERF_COMP_ROWS - 1U;
    rect.w = PERF_COMP_COLS;
    rect.h = 1U;
    yew_cmdline_open(&ed, YEW_PROMPT_CMD, NULL);
    before_opendirs = yew_comp_listing_opendirs();

    for (i = 0U; i < PERF_EXEC_KEYS; i++) {
        Key key = perf_key(PERF_EXEC_PREFIX[i]);
        i64 start = now_ns();
        i64 elapsed;

        if (start < 0)
            goto done;
        (void)yew_cmdline_key(&ed, &key);
        yew_cmdline_draw(&ed, rect);
        elapsed = now_ns() - start;
        if (elapsed < 0)
            goto done;
        samples[i] = elapsed;
        perf_comp_sink ^= (u64)ed.grid.cur_col + ed.grid.cur_row;
        /*
         * The idle path, driven exactly where the event loop drives it:
         * AFTER the keystroke is timed, because that is the point of
         * slicing the scan.  Without it the first $PATH element eats the
         * whole first slice and the second is never opened -- so the
         * opendir count below would be 1 and would prove nothing about
         * a resumed multi-element scan.
         */
        while (yew_cmdline_comp_tick(&ed))
            ;
    }
    if (ed.cmdline.menu.items.len == 0U) {
        (void)fprintf(stderr, "perf_cmdcomp: exec menu was empty\n");
        goto done;
    }
    *opendirs = yew_comp_listing_opendirs() - before_opendirs;
    ok = true;

done:
    yew_cmdline_dispose(&ed);
    ed.ws.dir = NULL;
    yew_ed_free(&ed);
    return ok;
}

/*
 * The gate: every keystroke of `!entry000` inside its 5 ms, and
 * PERF_EXEC_DIRS opendir calls for the whole prefix.  A source that
 * rescanned $PATH per keystroke would report nine times that count and
 * fail here before any latency number was even read.
 */
static int measure_exec(const char *root)
{
    char path_env[512];
    i64 samples[PERF_EXEC_SAMPLES];
    i64 worst_by_key[PERF_EXEC_KEYS];
    size_t trial;
    size_t i;
    u64 worst_opendirs = 0U;
    i64 p99;
    i64 median;
    int status = 0;

    path_env[0] = '\0';
    if (!exec_fixture_create(root, path_env, sizeof(path_env))) {
        (void)exec_fixture_remove(root);
        return 2;
    }
    if (setenv("PATH", path_env, 1) != 0) {
        (void)exec_fixture_remove(root);
        return 2;
    }
    for (i = 0U; i < PERF_EXEC_KEYS; i++)
        worst_by_key[i] = 0;
    for (trial = 0U; trial < PERF_COMP_WARMUPS; trial++) {
        i64 ignored[PERF_EXEC_KEYS];
        u64 ignored_opendirs;

        if (!measure_exec_trial(root, ignored, &ignored_opendirs)) {
            status = 2;
            goto done;
        }
    }
    for (trial = 0U; trial < PERF_COMP_TRIALS; trial++) {
        u64 opendirs = 0U;

        if (!measure_exec_trial(root, &samples[trial * PERF_EXEC_KEYS],
                                &opendirs)) {
            status = 2;
            goto done;
        }
        if (opendirs > worst_opendirs)
            worst_opendirs = opendirs;
        for (i = 0U; i < PERF_EXEC_KEYS; i++) {
            i64 value = samples[trial * PERF_EXEC_KEYS + i];

            if (value > worst_by_key[i])
                worst_by_key[i] = value;
        }
    }
    stable_sort_i64(samples, PERF_EXEC_SAMPLES);
    median = samples[PERF_EXEC_SAMPLES / 2U];
    p99 = samples[(PERF_EXEC_SAMPLES * 99U + 99U) / 100U - 1U];
    (void)printf("perf-cmdcomp-exec: dirs=%u entries=%u keys=%u "
                 "samples=%u median_ms=%.3f p99_ms=%.3f max_ms=%.3f "
                 "opendirs=%u allowed=%u budget_ms=%.3f%s\n",
                 (unsigned)PERF_EXEC_DIRS,
                 (unsigned)(PERF_EXEC_DIRS * PERF_EXEC_PER_DIR),
                 (unsigned)PERF_EXEC_KEYS, (unsigned)PERF_EXEC_SAMPLES,
                 (double)median / 1000000.0, (double)p99 / 1000000.0,
                 (double)samples[PERF_EXEC_SAMPLES - 1U] / 1000000.0,
                 (unsigned)worst_opendirs, (unsigned)PERF_EXEC_DIRS,
                 (double)PERF_COMP_BUDGET_NS / 1000000.0,
                 result_verdict(p99, worst_opendirs, PERF_EXEC_DIRS,
                                yew_perf_advisory()));
    (void)printf("perf-cmdcomp-exec: per_key_max_ms=");
    for (i = 0U; i < PERF_EXEC_KEYS; i++)
        (void)printf("%s%.3f", i == 0U ? "" : ",",
                     (double)worst_by_key[i] / 1000000.0);
    (void)printf("\n");
    if (result_failed(p99, worst_opendirs, PERF_EXEC_DIRS,
                      yew_perf_advisory()))
        status = 1;

done:
    if (!exec_fixture_remove(root)) {
        (void)fprintf(stderr, "perf_cmdcomp: exec fixture cleanup failed\n");
        if (status == 0)
            status = 2;
    }
    return status;
}

static int selftest_policy(void)
{
    const i64 budget = PERF_COMP_BUDGET_NS;

    if (result_failed(budget, 1U, 1U, false) ||
        !result_failed(budget + 1, 1U, 1U, false) ||
        result_failed(budget + 1, 1U, 1U, true) ||
        !result_failed(budget, 2U, 1U, true) ||
        /* An allowance the case declares is still a CEILING: two
         * elements may open two directories and never a third. */
        result_failed(budget, 2U, 2U, true) ||
        !result_failed(budget, 3U, 2U, true) ||
        result_failed(
            budget * YEW_PERF_ADVISORY_SANITY_MULTIPLIER, 1U, 1U, true) ||
        !result_failed(
            budget * YEW_PERF_ADVISORY_SANITY_MULTIPLIER + 1,
            1U, 1U, true) ||
        !result_failed(0, 1U, 1U, true)) {
        (void)fprintf(stderr, "perf-cmdcomp-policy: failed\n");
        return 1;
    }
    (void)printf("perf-cmdcomp-policy: strict/advisory/sanity/opendir ok\n");
    return 0;
}

int main(int argc, char **argv)
{
    char root[64];
    i64 samples[PERF_COMP_SAMPLES];
    i64 worst_by_key[PERF_COMP_KEYS];
    size_t trial;
    size_t i;
    size_t worst_key = 0U;
    u64 worst_opendirs = 0U;
    i64 p99;
    i64 median;
    int status = 0;

    if (argc == 2 && strcmp(argv[1], "--selftest-policy") == 0)
        return selftest_policy();
    if (argc != 1) {
        (void)fprintf(stderr, "usage: %s [--selftest-policy]\n", argv[0]);
        return 2;
    }
    if (!fixture_create(root)) {
        if (root[0] != '\0')
            (void)fixture_remove(root);
        yew_cmd_shutdown();
        return 2;
    }
    for (i = 0U; i < PERF_COMP_KEYS; i++)
        worst_by_key[i] = 0;
    for (trial = 0U; trial < PERF_COMP_WARMUPS; trial++) {
        i64 ignored[PERF_COMP_KEYS];
        u64 ignored_opendirs;

        if (!measure_trial(root, ignored, &ignored_opendirs)) {
            status = 2;
            goto done;
        }
    }
    for (trial = 0U; trial < PERF_COMP_TRIALS; trial++) {
        u64 opendirs = 0U;

        if (!measure_trial(root, &samples[trial * PERF_COMP_KEYS],
                           &opendirs)) {
            status = 2;
            goto done;
        }
        /*
         * DoD 10 asserts a COUNT, not a latency.  One opendir per trial
         * is the whole claim: the directory is scanned when the PATH
         * source is first reached and never again while the menu stays
         * open, however many characters follow.
         */
        if (opendirs > worst_opendirs)
            worst_opendirs = opendirs;
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
                 "slowest_key=%u('%c') opendirs=%u budget_ms=%.3f%s\n",
                 PERF_COMP_ENTRIES, (unsigned)PERF_COMP_KEYS,
                 (unsigned)PERF_COMP_SAMPLES,
                 (double)median / 1000000.0, (double)p99 / 1000000.0,
                 (double)samples[PERF_COMP_SAMPLES - 1U] / 1000000.0,
                 (unsigned)worst_key, PERF_COMP_PREFIX[worst_key],
                 (unsigned)worst_opendirs,
                 (double)PERF_COMP_BUDGET_NS / 1000000.0,
                 result_verdict(p99, worst_opendirs, 1U,
                                yew_perf_advisory()));
    /* Per-key worst, always printed: a single p99 over a prefix whose
     * keys have wildly different costs hides which regime moved.  This is
     * the line that shows one expensive scan followed by cheap re-ranks
     * rather than a uniformly slow prompt. */
    (void)printf("perf-cmdcomp: per_key_max_ms=");
    for (i = 0U; i < PERF_COMP_KEYS; i++)
        (void)printf("%s%.3f", i == 0U ? "" : ",",
                     (double)worst_by_key[i] / 1000000.0);
    (void)printf("\n");
    if (result_failed(p99, worst_opendirs, 1U, yew_perf_advisory()))
        status = 1;
    {
        /* Runs after the path case and inside the same fixture root, so
         * the two share one mkdtemp and one cleanup. */
        int exec_status = measure_exec(root);

        if (exec_status != 0 && status == 0)
            status = exec_status;
    }

done:
    if (!fixture_remove(root)) {
        (void)fprintf(stderr, "perf_cmdcomp: fixture cleanup failed\n");
        if (status == 0)
            status = 2;
    }
    yew_cmd_shutdown();
    return status;
}
