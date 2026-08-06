/*
 * Sprint 25 §9: the workspace-state latency gates.
 *
 * THREE BUDGETS, and one reason each:
 *
 *   emit  <= 5 ms   on a 512-tab / 16-leaf document.  Emission runs on
 *                   the debounce timer, inside the event loop, on the
 *                   thread that paints.  A slow emit is a visible hitch
 *                   while typing, and it happens on the largest
 *                   workspace — the one whose owner already has the
 *                   most to lose from a stutter.
 *   parse <= 10 ms  on the same document.  Parsing runs at STARTUP,
 *                   inside the 20 ms cold-start budget, before the
 *                   first paint.
 *   restore <= 20 ms on a 40-tab workspace: the whole cold-start budget,
 *                   because a restore that eats it has eaten the thing
 *                   it was meant to make pleasant.
 *
 * The 40-tab restore is the one that would regress silently.  It is
 * fast only because tabs come back DEFERRED — one file read, not forty
 * (s24 D3).  If that ever quietly stops being true this gate is what
 * notices, because forty reads is tens of milliseconds on a warm cache
 * and seconds on a cold or networked one.  So the read COUNT is
 * asserted here too, not just the time: a machine with a fast disk
 * would otherwise pass this while doing forty times the I/O.
 */
#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "edit/cmd.h"
#include "edit/ed.h"
#include "text/file.h"
#include "ui/layout.h"
#include "ui/tabs.h"
#include "util/arena.h"
#include "util/buf.h"
#include "ws/fllit.h"
#include "ws/state.h"

enum {
    PERF_STATE_TABS = 512,
    PERF_STATE_LEAVES = 16,
    PERF_STATE_RESTORE_TABS = 40,
    PERF_STATE_TRIALS = 11,
    PERF_STATE_WARMUPS = 3,
    PERF_STATE_EMIT_BUDGET_NS = 5000000,
    PERF_STATE_PARSE_BUDGET_NS = 10000000,
    PERF_STATE_RESTORE_BUDGET_NS = 20000000
};

static i64 now_ns(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        (void)fprintf(stderr, "perf_state: clock_gettime: %s\n",
                      strerror(errno));
        return -1;
    }
    return (i64)ts.tv_sec * 1000000000 + (i64)ts.tv_nsec;
}

/* Insertion sort: eleven samples, and raw qsort is banned. */
static void sort_i64(i64 *v, size_t n)
{
    size_t i;

    for (i = 1U; i < n; i++) {
        i64 key = v[i];
        size_t k = i;

        while (k > 0U && v[k - 1U] > key) {
            v[k] = v[k - 1U];
            k--;
        }
        v[k] = key;
    }
}

static i64 median_of(i64 *v, size_t n)
{
    sort_i64(v, n);
    return v[n / 2U];
}

/* ---------------------------------------------------------------- */
/* Fixtures                                                         */
/* ---------------------------------------------------------------- */

static void ed_open(Ed *ed)
{
    sag_cmd_shutdown();
    sag_cmd_init();
    sag_ed_init(ed);
    ed->ws.dir = arena_strdup(&ed->arena, "/w");
    if (!sag_ed_open_scratch(ed)) {
        (void)fprintf(stderr, "perf_state: cannot open a scratch buffer\n");
        exit(2);
    }
    sag_layout_compute(ed->pane_root, (Rect){0U, 0U, 240U, 96U});
}

/* 512 tabs, the first of them split to 16 leaves — the §9 shape. */
static void build_maximal(Ed *ed)
{
    u32 i;

    ed_open(ed);
    for (i = 0U; i < (u32)PERF_STATE_TABS; i++) {
        char path[64];

        (void)snprintf(path, sizeof(path), "/w/max/f%03u.txt",
                       (unsigned)i);
        if (sag_tab_open(ed, path) < 0)
            break;
    }
    sag_tab_switch(ed, 1);
    sag_layout_compute(ed->pane_root, (Rect){0U, 0U, 240U, 96U});
    for (i = 1U; i < (u32)PERF_STATE_LEAVES; i++) {
        Pane *nu = sag_pane_split(ed, ed->focus,
                                  (i % 2U) == 0U ? SAG_SPLIT_H
                                                 : SAG_SPLIT_V);

        if (nu == NULL)
            break;
        ed->focus = nu;
        sag_tab_at(ed, 1)->focus = nu;
        sag_layout_compute(ed->pane_root, (Rect){0U, 0U, 240U, 96U});
    }
}

/* ---------------------------------------------------------------- */

static int measure_emit(const Ed *ed, i64 *out_ns, u64 *out_bytes)
{
    i64 samples[PERF_STATE_TRIALS];
    u32 t;

    for (t = 0U; t < (u32)(PERF_STATE_TRIALS + PERF_STATE_WARMUPS); t++) {
        Bytebuf doc;
        i64 start;
        i64 end;

        bytebuf_init(&doc);
        start = now_ns();
        sag_state_emit(ed, &doc);
        end = now_ns();
        if (start < 0 || end < 0)
            return 2;
        if (t >= (u32)PERF_STATE_WARMUPS)
            samples[t - PERF_STATE_WARMUPS] = end - start;
        *out_bytes = doc.len;
        bytebuf_free(&doc);
    }
    *out_ns = median_of(samples, PERF_STATE_TRIALS);
    return 0;
}

static int measure_parse(const Bytebuf *doc, i64 *out_ns)
{
    i64 samples[PERF_STATE_TRIALS];
    u32 t;

    for (t = 0U; t < (u32)(PERF_STATE_TRIALS + PERF_STATE_WARMUPS); t++) {
        Arena a;
        FlParseErr err;
        FlLit *lit;
        i64 start;
        i64 end;

        arena_init(&a);
        start = now_ns();
        lit = sag_fl_parse(&a, doc->data, doc->len, &err);
        end = now_ns();
        if (lit == NULL) {
            (void)fprintf(stderr, "perf_state: corpus parse failed\n");
            arena_free_all(&a);
            return 2;
        }
        if (start < 0 || end < 0) {
            arena_free_all(&a);
            return 2;
        }
        if (t >= (u32)PERF_STATE_WARMUPS)
            samples[t - PERF_STATE_WARMUPS] = end - start;
        arena_free_all(&a);
    }
    *out_ns = median_of(samples, PERF_STATE_TRIALS);
    return 0;
}

/*
 * A 40-tab restore, timed AND counted.
 *
 * The count is the load-bearing half.  A restore is inside the cold
 * start budget only because it performs one read; if deferral broke,
 * this would still pass on a warm SSD and fail on every laptop with a
 * network mount.
 */
static int measure_restore(const Bytebuf *doc, i64 *out_ns, u64 *out_reads)
{
    i64 samples[PERF_STATE_TRIALS];
    u32 t;

    for (t = 0U; t < (u32)(PERF_STATE_TRIALS + PERF_STATE_WARMUPS); t++) {
        Ed ed;
        i64 start;
        i64 end;

        ed_open(&ed);
        sag_file_load_count_reset();
        start = now_ns();
        (void)sag_state_apply(&ed, doc->data, doc->len);
        sag_ed_layout(&ed);
        end = now_ns();
        if (start < 0 || end < 0) {
            sag_ed_free(&ed);
            return 2;
        }
        if (t >= (u32)PERF_STATE_WARMUPS) {
            samples[t - PERF_STATE_WARMUPS] = end - start;
            *out_reads = sag_file_load_count();
        }
        sag_ed_free(&ed);
    }
    *out_ns = median_of(samples, PERF_STATE_TRIALS);
    return 0;
}

int main(void)
{
    Ed big;
    Ed forty;
    Bytebuf big_doc;
    Bytebuf forty_doc;
    i64 emit_ns = 0;
    i64 parse_ns = 0;
    i64 restore_ns = 0;
    u64 bytes = 0U;
    u64 reads = 0U;
    u32 i;
    int status = 0;
    int rc;

    build_maximal(&big);
    bytebuf_init(&big_doc);
    rc = measure_emit(&big, &emit_ns, &bytes);
    if (rc != 0) {
        sag_ed_free(&big);
        return rc;
    }
    sag_state_emit(&big, &big_doc);
    sag_ed_free(&big);

    rc = measure_parse(&big_doc, &parse_ns);
    if (rc != 0) {
        bytebuf_free(&big_doc);
        return rc;
    }

    /* A separate, realistic document for the restore: 40 tabs is the
     * contract's number and a plausible working set, where 512 is the
     * cap. */
    ed_open(&forty);
    for (i = 0U; i < (u32)PERF_STATE_RESTORE_TABS; i++) {
        char path[64];

        (void)snprintf(path, sizeof(path), "/w/r/f%02u.txt", (unsigned)i);
        if (sag_tab_open(&forty, path) < 0)
            break;
    }
    bytebuf_init(&forty_doc);
    sag_state_emit(&forty, &forty_doc);
    sag_ed_free(&forty);

    rc = measure_restore(&forty_doc, &restore_ns, &reads);
    if (rc != 0) {
        bytebuf_free(&big_doc);
        bytebuf_free(&forty_doc);
        return rc;
    }

    (void)printf("perf-state: tabs=%u leaves=%u bytes=%llu "
                 "emit_ms=%.3f (budget %.3f) parse_ms=%.3f (budget %.3f)%s\n",
                 (unsigned)PERF_STATE_TABS, (unsigned)PERF_STATE_LEAVES,
                 (unsigned long long)bytes,
                 (double)emit_ns / 1000000.0,
                 (double)PERF_STATE_EMIT_BUDGET_NS / 1000000.0,
                 (double)parse_ns / 1000000.0,
                 (double)PERF_STATE_PARSE_BUDGET_NS / 1000000.0,
                 emit_ns <= PERF_STATE_EMIT_BUDGET_NS &&
                         parse_ns <= PERF_STATE_PARSE_BUDGET_NS
                     ? " ok"
                     : " FAIL");
    /*
     * The read count is printed on its own line and always, because it
     * is the number that explains the time — and the one a reviewer
     * should look at first when this gate moves.
     */
    (void)printf("perf-state: restore_tabs=%u restore_ms=%.3f "
                 "(budget %.3f) file_reads=%llu (expected 1)%s\n",
                 (unsigned)PERF_STATE_RESTORE_TABS,
                 (double)restore_ns / 1000000.0,
                 (double)PERF_STATE_RESTORE_BUDGET_NS / 1000000.0,
                 (unsigned long long)reads,
                 restore_ns <= PERF_STATE_RESTORE_BUDGET_NS && reads <= 1U
                     ? " ok"
                     : " FAIL");
    if (emit_ns > PERF_STATE_EMIT_BUDGET_NS ||
        parse_ns > PERF_STATE_PARSE_BUDGET_NS ||
        restore_ns > PERF_STATE_RESTORE_BUDGET_NS || reads > 1U)
        status = 1;

    bytebuf_free(&big_doc);
    bytebuf_free(&forty_doc);
    sag_cmd_shutdown();
    return status;
}
