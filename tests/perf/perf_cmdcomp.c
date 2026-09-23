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
#include "ui/cmdhist.h"
#include "ui/cmdline.h"
#include "ui/compspec.h"
#include "ui/shctx.h"
#include "util/arena.h"
#include "util/buf.h"

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
    PERF_EXEC_SAMPLES = PERF_COMP_TRIALS * PERF_EXEC_KEYS,
    /*
     * Sprint 57.23: the shell context lexer runs on EVERY keystroke of an
     * open `:!` menu, over the whole body up to the caret.  A 4 KiB body
     * with the caret at its end is the worst case a prompt realistically
     * holds; 1000 lexes of it, p99 under 200 us.
     */
    PERF_SHCTX_BYTES = 4096,
    PERF_SHCTX_ITERS = 1000,
    PERF_SHCTX_WARMUPS = 50,
    PERF_SHCTX_BUDGET_NS = 200000,
    /*
     * Sprint 57.24: resolving a caret against a spec runs on every
     * keystroke of an open `:!` menu too -- the spec lookup, the walk and
     * the routing.  `git -C x remote add o` against the SHIPPED git.fl,
     * warm (the one-time parse is not a keystroke's), p99 under 150 us.
     */
    PERF_SPEC_ITERS = 1000,
    PERF_SPEC_WARMUPS = 50,
    PERF_SPEC_BUDGET_NS = 150000,
    /*
     * Sprint 57.26 §3: the history ghost is computed on every keystroke
     * of a `:!` body -- a prefix memcmp over the snapshot, newest first.
     * The worst case is a full 20 000-entry snapshot whose ONLY match is
     * the oldest entry, so every entry is compared; p99 under 300 us.
     */
    PERF_HIST_ITERS = 1000,
    PERF_HIST_WARMUPS = 50,
    PERF_HIST_BUDGET_NS = 300000,
    /*
     * The snapshot is taken on the keystroke that first makes the line a
     * bang body, once per prompt: parsing a full-size history (20 000
     * fish entries, ~1 MB) must fit the keypress budget on its own.
     */
    PERF_HIST_LOADS = 21,
    PERF_HIST_LOAD_BUDGET_NS = 5000000
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

/*
 * The body mixes every construct the lexer tracks -- pipes, quotes, a
 * substitution, a variable, a redirection, assignments, a wrapper -- so
 * no fast path can skip most of it.
 */
static int measure_shctx(void)
{
    static const char unit[] =
        "FOO=1 sudo -u root git log --grep \"fix $(date +%s)\" "
        "'a b' $HOME/x 2>err.log | grep -v x && ";
    static i64 samples[PERF_SHCTX_ITERS];
    char *body = malloc(PERF_SHCTX_BYTES + 1U);
    size_t len = 0U;
    size_t ulen = sizeof(unit) - 1U;
    Arena arena;
    u32 i;
    i64 p99;
    i64 median;
    bool failed;

    if (body == NULL)
        return 2;
    while (len + ulen <= PERF_SHCTX_BYTES) {
        (void)memcpy(body + len, unit, ulen);
        len += ulen;
    }
    while (len < PERF_SHCTX_BYTES)
        body[len++] = 'x';
    body[len] = '\0';
    arena_init(&arena);
    for (i = 0U; i < PERF_SHCTX_WARMUPS + PERF_SHCTX_ITERS; i++) {
        YewShCtx ctx;
        i64 start = now_ns();
        i64 end;

        if (!yew_shctx_at(body, len, len, &arena, &ctx)) {
            (void)fprintf(stderr, "perf_cmdcomp: shctx refused its body\n");
            arena_free_all(&arena);
            free(body);
            return 2;
        }
        end = now_ns();
        perf_comp_sink += (u64)ctx.pos + ctx.argc;
        arena_free_all(&arena);
        if (i >= PERF_SHCTX_WARMUPS)
            samples[i - PERF_SHCTX_WARMUPS] = end - start;
    }
    free(body);
    stable_sort_i64(samples, PERF_SHCTX_ITERS);
    median = samples[PERF_SHCTX_ITERS / 2U];
    p99 = samples[(PERF_SHCTX_ITERS * 99U + 99U) / 100U - 1U];
    failed = yew_perf_timing_failed((uint64_t)p99,
                                    (uint64_t)PERF_SHCTX_BUDGET_NS,
                                    yew_perf_advisory());
    (void)printf("perf-cmdcomp-shctx: bytes=%u iters=%u median_us=%.1f "
                 "p99_us=%.1f max_us=%.1f budget_us=%.1f%s\n",
                 (unsigned)PERF_SHCTX_BYTES, (unsigned)PERF_SHCTX_ITERS,
                 (double)median / 1000.0, (double)p99 / 1000.0,
                 (double)samples[PERF_SHCTX_ITERS - 1U] / 1000.0,
                 (double)PERF_SHCTX_BUDGET_NS / 1000.0,
                 yew_perf_timing_verdict((uint64_t)p99,
                                         (uint64_t)PERF_SHCTX_BUDGET_NS,
                                         yew_perf_advisory()));
    return failed ? 1 : 0;
}

static int measure_spec(void)
{
    static const char body[] = "git -C x remote add o";
    static i64 samples[PERF_SPEC_ITERS];
    const char *runtime = getenv("YEW_RUNTIME_DIR");
    char *cwd;
    Arena arena;
    u32 i;
    i64 p99;
    i64 median;
    bool failed;

    /* The checked-in runtime, never an installed one. */
    if (runtime == NULL || runtime[0] == '\0') {
        size_t n;
        char *dir;

        cwd = yew_xgetcwd();
        n = strlen(cwd) + sizeof("/runtime");
        dir = malloc(n);
        if (dir == NULL) {
            yew_xfree(cwd);
            return 2;
        }
        (void)snprintf(dir, n, "%s/runtime", cwd);
        yew_xfree(cwd);
        if (setenv("YEW_RUNTIME_DIR", dir, 1) != 0) {
            free(dir);
            return 2;
        }
        free(dir);
    }
    yew_compspec_invalidate_all();
    if (yew_compspec_get(NULL, "git") == NULL) {
        (void)fprintf(stderr, "perf_cmdcomp: the shipped git.fl did not "
                              "load\n");
        return 2;
    }
    arena_init(&arena);
    for (i = 0U; i < PERF_SPEC_WARMUPS + PERF_SPEC_ITERS; i++) {
        YewShCtx ctx;
        char *described;
        i64 start = now_ns();
        i64 end;

        if (!yew_shctx_at_with(body, sizeof(body) - 1U, sizeof(body) - 1U,
                               &arena, yew_compspec_wrapper, NULL, &ctx)) {
            arena_free_all(&arena);
            return 2;
        }
        described = yew_comp_shell_describe(NULL, &ctx, &arena);
        end = now_ns();
        /* `remote add <name> <url>`: `o` is the new remote's NAME, free
         * text.  Anything else means the walk went wrong. */
        if (described == NULL || strcmp(described, "none") != 0) {
            (void)fprintf(stderr, "perf_cmdcomp: `%s` routed to %s\n", body,
                          described == NULL ? "(null)" : described);
            arena_free_all(&arena);
            return 2;
        }
        perf_comp_sink += (u64)ctx.argc + strlen(described);
        arena_free_all(&arena);
        if (i >= PERF_SPEC_WARMUPS)
            samples[i - PERF_SPEC_WARMUPS] = end - start;
    }
    stable_sort_i64(samples, PERF_SPEC_ITERS);
    median = samples[PERF_SPEC_ITERS / 2U];
    p99 = samples[(PERF_SPEC_ITERS * 99U + 99U) / 100U - 1U];
    failed = yew_perf_timing_failed((uint64_t)p99,
                                    (uint64_t)PERF_SPEC_BUDGET_NS,
                                    yew_perf_advisory());
    (void)printf("perf-cmdcomp-spec: line=\"%s\" iters=%u median_us=%.1f "
                 "p99_us=%.1f max_us=%.1f budget_us=%.1f%s\n",
                 body, (unsigned)PERF_SPEC_ITERS, (double)median / 1000.0,
                 (double)p99 / 1000.0,
                 (double)samples[PERF_SPEC_ITERS - 1U] / 1000.0,
                 (double)PERF_SPEC_BUDGET_NS / 1000.0,
                 yew_perf_timing_verdict((uint64_t)p99,
                                         (uint64_t)PERF_SPEC_BUDGET_NS,
                                         yew_perf_advisory()));
    return failed ? 1 : 0;
}

static int measure_history(void)
{
    static i64 samples[PERF_HIST_ITERS];
    static const char body[] = "git log --oneline --graph --decor";
    YewHistSuggest snap;
    char line[96];
    u32 i;
    i64 p99;
    i64 median;
    bool failed;

    yew_hist_suggest_init(&snap);
    /* Newest first: 19 999 near-misses sharing a long prefix (so each
     * memcmp does real work), then the one match, the oldest. */
    for (i = 0U; i + 1U < YEW_HIST_SUGGEST_MAX; i++) {
        (void)snprintf(line, sizeof(line),
                       "git log --oneline --graph --dec%05u", (unsigned)i);
        (void)yew_hist_suggest_add(&snap, line, strlen(line));
    }
    (void)yew_hist_suggest_add(&snap,
                               "git log --oneline --graph --decorate --all",
                               42U);
    if (snap.n != YEW_HIST_SUGGEST_MAX) {
        (void)fprintf(stderr, "perf_cmdcomp: history snapshot holds %u\n",
                      (unsigned)snap.n);
        yew_hist_suggest_free(&snap);
        return 2;
    }
    for (i = 0U; i < PERF_HIST_WARMUPS + PERF_HIST_ITERS; i++) {
        size_t rest = 0U;
        const char *ghost;
        i64 start = now_ns();
        i64 end;

        ghost = yew_hist_suggest_match(&snap, body, sizeof(body) - 1U,
                                       &rest);
        end = now_ns();
        if (ghost == NULL || rest != 9U || memcmp(ghost, "ate --all", 9U)) {
            (void)fprintf(stderr, "perf_cmdcomp: history scan missed\n");
            yew_hist_suggest_free(&snap);
            return 2;
        }
        perf_comp_sink += (u64)rest;
        if (i >= PERF_HIST_WARMUPS)
            samples[i - PERF_HIST_WARMUPS] = end - start;
    }
    yew_hist_suggest_free(&snap);
    {
        static i64 loads[PERF_HIST_LOADS];
        Bytebuf file;
        i64 load_p99;
        bool load_failed;

        bytebuf_init(&file);
        for (i = 0U; i < YEW_HIST_SUGGEST_MAX; i++)
            bytebuf_printf(&file,
                           "- cmd: make -C src/module%05u test-unit "
                           "V=1 && ./build/run --verbose\n"
                           "  when: %u\n",
                           (unsigned)i, 1700000000U + (unsigned)i);
        for (i = 0U; i < PERF_HIST_LOADS; i++) {
            i64 start = now_ns();

            yew_hist_suggest_init(&snap);
            yew_hist_parse_fish(&snap, (const char *)file.data, file.len);
            loads[i] = now_ns() - start;
            perf_comp_sink += snap.n;
            yew_hist_suggest_free(&snap);
        }
        stable_sort_i64(loads, PERF_HIST_LOADS);
        load_p99 = loads[PERF_HIST_LOADS - 1U];
        load_failed = yew_perf_timing_failed(
            (uint64_t)load_p99, (uint64_t)PERF_HIST_LOAD_BUDGET_NS,
            yew_perf_advisory());
        (void)printf("perf-cmdcomp-history-load: entries=%u bytes=%zu "
                     "median_ms=%.3f max_ms=%.3f budget_ms=%.3f%s\n",
                     (unsigned)YEW_HIST_SUGGEST_MAX, file.len,
                     (double)loads[PERF_HIST_LOADS / 2U] / 1000000.0,
                     (double)load_p99 / 1000000.0,
                     (double)PERF_HIST_LOAD_BUDGET_NS / 1000000.0,
                     yew_perf_timing_verdict(
                         (uint64_t)load_p99,
                         (uint64_t)PERF_HIST_LOAD_BUDGET_NS,
                         yew_perf_advisory()));
        bytebuf_free(&file);
        if (load_failed)
            return 1;
    }
    stable_sort_i64(samples, PERF_HIST_ITERS);
    median = samples[PERF_HIST_ITERS / 2U];
    p99 = samples[(PERF_HIST_ITERS * 99U + 99U) / 100U - 1U];
    failed = yew_perf_timing_failed((uint64_t)p99,
                                    (uint64_t)PERF_HIST_BUDGET_NS,
                                    yew_perf_advisory());
    (void)printf("perf-cmdcomp-history: entries=%u iters=%u median_us=%.1f "
                 "p99_us=%.1f max_us=%.1f budget_us=%.1f%s\n",
                 (unsigned)YEW_HIST_SUGGEST_MAX, (unsigned)PERF_HIST_ITERS,
                 (double)median / 1000.0, (double)p99 / 1000.0,
                 (double)samples[PERF_HIST_ITERS - 1U] / 1000.0,
                 (double)PERF_HIST_BUDGET_NS / 1000.0,
                 yew_perf_timing_verdict((uint64_t)p99,
                                         (uint64_t)PERF_HIST_BUDGET_NS,
                                         yew_perf_advisory()));
    return failed ? 1 : 0;
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
    /*
     * Sprint 57.26: a `:!` keystroke may take the history snapshot, which
     * reads the shells' history files.  The gate measures yew, not the
     * size of whoever runs it's history: HOME is the fixture root, and
     * neither XDG_DATA_HOME nor HISTFILE names anything.
     */
    if (setenv("HOME", root, 1) != 0 || unsetenv("XDG_DATA_HOME") != 0 ||
        unsetenv("HISTFILE") != 0) {
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
        int shctx_status = measure_shctx();
        int spec_status = measure_spec();
        int history_status = measure_history();

        if (exec_status != 0 && status == 0)
            status = exec_status;
        if (shctx_status != 0 && status == 0)
            status = shctx_status;
        if (spec_status != 0 && status == 0)
            status = spec_status;
        if (history_status != 0 && status == 0)
            status = history_status;
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
