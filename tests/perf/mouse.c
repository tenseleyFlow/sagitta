/*
 * Sprint 27 DoD 13: the motion-burst gate.
 *
 * THE NUMBER THAT MATTERS.  A pointer dragged across the screen emits
 * motion reports as fast as the terminal can write them — hundreds in
 * the time between two frames — and how many arrive depends on the
 * terminal, the machine's load, and nothing the user can see.  So the
 * router has to be cheap enough that a burst is absorbed inside one
 * frame, and it must not render: rendering per report is how a drag
 * turns into a slideshow on exactly the machines that emit the most
 * reports.
 *
 *   1 000-event burst   <= 5 ms   invariant 4's keypress budget, so a
 *                                 burst never costs more than a key
 *   renders during it   0         the LOOP renders, once, afterwards
 *   allocations         0         checked structurally, below
 *
 * The allocation claim is checked by reading the router's source rather
 * than by counting mallocs at runtime: glibc removed the malloc hooks,
 * and an interposed allocator would measure the test harness as much as
 * the router.  "This file contains no allocation call" is the stronger
 * statement anyway — it cannot regress under a code path the burst
 * happened not to take.
 */
#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "edit/ed.h"
#include "ui/layout.h"
#include "ui/mouse.h"
#include "ui/region.h"
#include "ui/tabs.h"
#include "util/base.h"

enum {
    PERF_MOUSE_EVENTS = 1000,
    PERF_MOUSE_TRIALS = 11,
    PERF_MOUSE_BUDGET_NS = 5000000
};

static i64 now_ns(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        (void)fprintf(stderr, "perf_mouse: clock_gettime: %s\n",
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

static Key motion_at(u16 x, u16 y)
{
    Key k;

    (void)memset(&k, 0, sizeof(k));
    k.kind = (u16)YEW_EV_MOUSE;
    k.button = (u8)YEW_MB_LEFT;
    k.ev = (u8)YEW_KEY_REPEAT;
    k.col = x;
    k.row = y;
    return k;
}

/*
 * A live drag over the tab strip: the most expensive motion path there
 * is, because it re-resolves the drop slot and runs the dwell against
 * the pre-drag table on every event.
 */
static i64 measure_burst(Ed *ed, u64 *renders)
{
    Key press;
    i64 start;
    i64 elapsed;
    int i;

    yew_region_frame_begin();
    yew_tab_strip_draw(ed, ed->tab_strip_rect);
    press = motion_at(2U, 0U);
    press.ev = (u8)YEW_KEY_PRESS;
    yew_mouse_event(ed, &press);

    *renders = ed->render.frames;
    start = now_ns();
    for (i = 0; i < PERF_MOUSE_EVENTS; i++) {
        Key m = motion_at((u16)(1U + (i % 70)), 0U);

        yew_mouse_event(ed, &m);
    }
    elapsed = now_ns() - start;
    *renders = ed->render.frames - *renders;
    yew_mouse_cancel(ed);
    return elapsed;
}

/* DoD 13's second half, read out of the source it is a claim about. */
static bool router_allocates(const char **what)
{
    static const char *const banned[] = {
        "malloc(", "calloc(", "realloc(", "strdup(", "yew_xmalloc",
        "yew_xcalloc", "yew_xrealloc", "arena_alloc"
    };
    /* YEW_PERF_MOUSE_SRC is a test seam, and it earns its keep: a
     * check that can only ever pass proves nothing, so pointing it at a
     * file that DOES allocate is how this gate is shown to bite. */
    const char *src = getenv("YEW_PERF_MOUSE_SRC");
    FILE *f = fopen(src != NULL ? src : "src/ui/mouse.c", "r");
    char line[512];
    bool found = false;

    if (f == NULL) {
        *what = "the router's source could not be read";
        return true; /* an unreadable claim is a failed claim */
    }
    while (!found && fgets(line, sizeof(line), f) != NULL) {
        size_t i;

        /* Comments mention none of these, and a line that did would be
         * worth rewording rather than exempting. */
        for (i = 0U; i < YEW_ARRAY_LEN(banned); i++) {
            if (strstr(line, banned[i]) != NULL) {
                *what = banned[i];
                found = true;
                break;
            }
        }
    }
    (void)fclose(f);
    return found;
}

int main(void)
{
    Ed ed;
    i64 samples[PERF_MOUSE_TRIALS];
    u64 renders = 0U;
    u64 worst_renders = 0U;
    i64 median;
    const char *offender = NULL;
    bool allocates;
    int i;
    int status = 0;

    yew_cmd_init();
    yew_ed_init(&ed);
    if (!yew_ed_open_scratch(&ed) ||
        !yew_grid_init(&ed.grid, &ed.interner, 24U, 80U)) {
        (void)fprintf(stderr, "perf_mouse: could not build an editor\n");
        return 1;
    }
    ed.grid_ready = true;
    for (i = 0; i < 8; i++) {
        char path[64];

        (void)snprintf(path, sizeof(path), "/tmp/yew-perfmouse-%d.txt", i);
        (void)yew_tab_open(&ed, path);
    }
    yew_tab_switch(&ed, 0);
    yew_ed_layout(&ed);
    ed.now_ms = 1000;

    for (i = 0; i < PERF_MOUSE_TRIALS; i++) {
        samples[i] = measure_burst(&ed, &renders);
        if (renders > worst_renders)
            worst_renders = renders;
    }
    sort_i64(samples, (size_t)PERF_MOUSE_TRIALS);
    median = samples[PERF_MOUSE_TRIALS / 2];

    allocates = router_allocates(&offender);
    (void)printf("perf-mouse: burst=%d events_ms=%.3f (budget %.3f) "
                 "renders=%llu (budget 0)%s\n",
                 PERF_MOUSE_EVENTS, (double)median / 1000000.0,
                 (double)PERF_MOUSE_BUDGET_NS / 1000000.0,
                 (unsigned long long)worst_renders,
                 median <= PERF_MOUSE_BUDGET_NS && worst_renders == 0U
                     ? " ok"
                     : " FAIL");
    (void)printf("perf-mouse: router_allocations=%s%s\n",
                 allocates ? offender : "none",
                 allocates ? " FAIL" : " ok");
    if (median > PERF_MOUSE_BUDGET_NS || worst_renders != 0U || allocates)
        status = 1;
    yew_ed_free(&ed);
    return status;
}
