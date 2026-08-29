/*
 * Sprint 43 performance and structural gates for passive shadow text.
 *
 * The timed frame path is the real draw/diff pipeline at 200x50.  The
 * edit path measures only the fixed post-edit consumer on a matching
 * byte.  Debounce is a count gate: 100 rapid arms may retain at most one
 * live timer and must fan out to no more than one request per provider.
 */
#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "edit/ed.h"
#include "edit/shadow.h"
#include "term/grid.h"
#include "term/render.h"
#include "text/edit.h"
#include "ui/draw.h"
#include "ui/shadowdraw.h"

enum {
    SHADOW_PERF_TRIALS = 3,
    SHADOW_FRAME_SAMPLES = 101,
    SHADOW_EDIT_SAMPLES = 1001,
    SHADOW_ROWS = 50,
    SHADOW_COLS = 200,
    SHADOW_GHOST_LINES = 8,
    SHADOW_GHOST_COLS = 160,
    SHADOW_LARGE_LINES = 100000,
    SHADOW_FRAME_P99_BUDGET_NS = 250000,
    SHADOW_EDIT_P99_BUDGET_NS = 5000
};

typedef struct PerfResult {
    const char *name;
    i64 median_ns;
    i64 p99_ns;
    i64 baseline_median_ns;
    i64 baseline_p99_ns;
    i64 budget_p99_ns;
} PerfResult;

static volatile u64 perf_sink;
static u32 provider_requests[YEW_SHADOW_NPROV];

static bool perf_advisory(void)
{
    const char *value = getenv("YEW_PERF_ADVISORY");

    return value != NULL && strcmp(value, "0") != 0;
}

#if defined(__linux__)
static bool allocation_probe_active;
static u64 allocation_calls;

void *__real_malloc(size_t size);
void *__real_calloc(size_t count, size_t size);
void *__real_realloc(void *ptr, size_t size);
void __real_free(void *ptr);

void *__wrap_malloc(size_t size)
{
    if (allocation_probe_active)
        allocation_calls++;
    return __real_malloc(size);
}

void *__wrap_calloc(size_t count, size_t size)
{
    if (allocation_probe_active)
        allocation_calls++;
    return __real_calloc(count, size);
}

void *__wrap_realloc(void *ptr, size_t size)
{
    if (allocation_probe_active)
        allocation_calls++;
    return __real_realloc(ptr, size);
}

void __wrap_free(void *ptr)
{
    __real_free(ptr);
}

static void allocation_probe_begin(void)
{
    allocation_calls = 0U;
    allocation_probe_active = true;
}

static bool allocation_probe_end(void)
{
    allocation_probe_active = false;
    return allocation_calls == 0U;
}
#else
static void allocation_probe_begin(void) { }
static bool allocation_probe_end(void) { return true; }
#endif

static i64 now_ns(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        (void)fprintf(stderr, "perf_shadow: clock_gettime: %s\n",
                      strerror(errno));
        return -1;
    }
    return (i64)ts.tv_sec * INT64_C(1000000000) + (i64)ts.tv_nsec;
}

static void sort_i64(i64 *values, size_t len)
{
    size_t i;

    for (i = 1U; i < len; i++) {
        i64 value = values[i];
        size_t at = i;

        while (at != 0U && values[at - 1U] > value) {
            values[at] = values[at - 1U];
            at--;
        }
        values[at] = value;
    }
}

static void reduce_trials(i64 medians[SHADOW_PERF_TRIALS],
                          i64 p99s[SHADOW_PERF_TRIALS], PerfResult *result)
{
    sort_i64(medians, SHADOW_PERF_TRIALS);
    sort_i64(p99s, SHADOW_PERF_TRIALS);
    result->median_ns = medians[SHADOW_PERF_TRIALS / 2U];
    result->p99_ns = p99s[SHADOW_PERF_TRIALS / 2U];
}

static bool fake_request(Ed *ed, const ShadowReq *request)
{
    (void)ed;
    if (request == NULL || request->prov >= (u8)YEW_SHADOW_NPROV)
        return false;
    provider_requests[request->prov]++;
    return true;
}

static void fake_cancel(Ed *ed, u32 buf_id, u32 up_to)
{
    (void)ed;
    (void)buf_id;
    (void)up_to;
}

static void register_providers(void)
{
    static const ShadowProvider providers[YEW_SHADOW_NPROV] = {
        {"index", YEW_SHADOW_INDEX, 0U, fake_request, NULL},
        {"lsp", YEW_SHADOW_LSP, 120U, fake_request, fake_cancel},
        {"ai", YEW_SHADOW_AI, 350U, fake_request, fake_cancel},
    };
    u32 i;

    for (i = 0U; i < (u32)YEW_SHADOW_NPROV; i++)
        yew_shadow_register(&providers[i]);
}

static bool open_editor(Ed *ed, const u8 *bytes, size_t len,
                        bool with_grid)
{
    TtyCaps caps;

    yew_ed_init(ed);
    if (!yew_ed_open_memory(ed, bytes, len, "shadow-perf"))
        return false;
    ed->mode = YEW_MODE_I;
    ed->prev_unit = YEW_MODE_I;
    if (!with_grid)
        return true;
    (void)memset(&caps, 0, sizeof(caps));
    if (!yew_grid_init(&ed->grid, &ed->interner,
                       SHADOW_ROWS, SHADOW_COLS))
        return false;
    ed->grid_ready = true;
    yew_render_init(&ed->render, &caps, NULL);
    ed->render_ready = true;
    yew_ed_layout(ed);
    return true;
}

static size_t make_ghost(u8 *out, size_t cap, u8 fill)
{
    size_t at = 0U;
    u32 line;

    for (line = 0U; line < SHADOW_GHOST_LINES; line++) {
        u32 col;

        for (col = 0U; col < SHADOW_GHOST_COLS; col++) {
            if (at == cap)
                return 0U;
            out[at++] = (u8)(fill + col % 7U);
        }
        if (line + 1U < SHADOW_GHOST_LINES) {
            if (at == cap)
                return 0U;
            out[at++] = '\n';
        }
    }
    return at;
}

static void install_ghost_at(Ed *ed, ByteOff pos, const u8 *text,
                             size_t len)
{
    Shadow *shadow = &ed->win->shadow;

    shadow->live = true;
    shadow->suppressed = false;
    shadow->sug.seq = 1U;
    shadow->sug.prov = YEW_SHADOW_AI;
    shadow->sug.buf_id = ed->win->buf->id;
    shadow->sug.buf_gen = ed->win->buf->tb->gen;
    shadow->sug.pos = pos;
    shadow->sug.text = text;
    shadow->sug.len = (u32)len;
    shadow->sug.consumed = 0U;
    shadow->sug.scratch = NULL;
    shadow->owned_text = NULL;
}

static void install_ghost(Ed *ed, const u8 *text, size_t len)
{
    install_ghost_at(ed, BYTEOFF(0U), text, len);
}

static bool frame_once(Ed *ed, const u8 *ghost, size_t ghost_len,
                       i64 *elapsed_out)
{
    LineNo top = yew_win_view_top(ed->win);
    CCol left = ed->win->vp.left;
    u32 top_sub = ed->win->vp.top_sub;
    i64 start;
    i64 elapsed;
    size_t emitted;

    install_ghost(ed, ghost, ghost_len);
    start = now_ns();
    if (start < 0)
        return false;
    yew_draw_panes(ed);
    yew_grid_mark_all(&ed->grid);
    yew_draw_footer(ed, ed->win);
    yew_draw_cursor(ed, ed->win);
    yew_shadow_draw_panes(ed);
    ed->frame.len = 0U;
    emitted = yew_render_frame(&ed->render, &ed->grid, &ed->frame);
    yew_grid_flip(&ed->grid);
    elapsed = now_ns() - start;
    if (elapsed < 0 || emitted == 0U || emitted != ed->frame.len ||
        ed->win->shadow.vrows != SHADOW_GHOST_LINES ||
        yew_win_view_top(ed->win).v != top.v ||
        ed->win->vp.left.v != left.v || ed->win->vp.top_sub != top_sub)
        return false;
    perf_sink ^= (u64)emitted + ed->grid.cur_col + ed->grid.cur_row;
    *elapsed_out = elapsed;
    return true;
}

static bool measure_frame(PerfResult *result)
{
    u8 ghost_a[SHADOW_GHOST_LINES * (SHADOW_GHOST_COLS + 1U)];
    u8 ghost_b[SHADOW_GHOST_LINES * (SHADOW_GHOST_COLS + 1U)];
    size_t ghost_len_a = make_ghost(ghost_a, sizeof(ghost_a), (u8)'a');
    size_t ghost_len_b = make_ghost(ghost_b, sizeof(ghost_b), (u8)'k');
    i64 medians[SHADOW_PERF_TRIALS];
    i64 p99s[SHADOW_PERF_TRIALS];
    u32 trial;

    if (ghost_len_a == 0U || ghost_len_a != ghost_len_b)
        return false;
    for (trial = 0U; trial < SHADOW_PERF_TRIALS; trial++) {
        Ed ed;
        i64 samples[SHADOW_FRAME_SAMPLES];
        u32 sample;

        if (!open_editor(&ed, NULL, 0U, true))
            return false;
        if (!frame_once(&ed, ghost_a, ghost_len_a, &samples[0])) {
            yew_ed_free(&ed);
            return false;
        }
        for (sample = 0U; sample < SHADOW_FRAME_SAMPLES; sample++) {
            const u8 *ghost = (sample & 1U) == 0U ? ghost_b : ghost_a;

            if (!frame_once(&ed, ghost, ghost_len_a, &samples[sample])) {
                yew_ed_free(&ed);
                return false;
            }
        }
        yew_ed_free(&ed);
        sort_i64(samples, SHADOW_FRAME_SAMPLES);
        medians[trial] = samples[SHADOW_FRAME_SAMPLES / 2U];
        p99s[trial] = samples[99U];
    }
    reduce_trials(medians, p99s, result);
    return true;
}

static bool measure_midline_large(PerfResult *result)
{
    static const u8 line[] = "int value = suffix_value;\n";
    static const u8 ghost[] = "ghost_";
    const size_t line_len = sizeof(line) - 1U;
    const size_t fixture_len = line_len * SHADOW_LARGE_LINES;
    u8 *fixture = malloc(fixture_len);
    i64 medians[SHADOW_PERF_TRIALS];
    i64 p99s[SHADOW_PERF_TRIALS];
    u32 trial;

    if (fixture == NULL)
        return false;
    for (size_t at = 0U; at < fixture_len; at += line_len)
        (void)memcpy(fixture + at, line, line_len);
    for (trial = 0U; trial < SHADOW_PERF_TRIALS; trial++) {
        Ed ed;
        Cursor *cursor;
        i64 samples[SHADOW_FRAME_SAMPLES];
        u32 sample;

        if (!open_editor(&ed, fixture, fixture_len, true)) {
            free(fixture);
            return false;
        }
        cursor = &ed.win->cs.curs.data[ed.win->cs.primary];
        cursor->pos = BYTEOFF(12U);
        cursor->anchor = BYTEOFF(12U);
        yew_draw_document_rows(&ed, ed.win, 0U, 1U);
        for (sample = 0U; sample < SHADOW_FRAME_SAMPLES; sample++) {
            ShadowLayout layout;
            i64 start;
            i64 elapsed;

            yew_draw_document_rows(&ed, ed.win, 0U, 1U);
            install_ghost_at(&ed, cursor->pos, ghost,
                             sizeof(ghost) - 1U);
            yew_region_frame_begin();
            allocation_probe_begin();
            start = now_ns();
            if (start < 0) {
                (void)allocation_probe_end();
                yew_ed_free(&ed);
                free(fixture);
                return false;
            }
            yew_shadow_layout(ed.win, &ed.win->shadow, &layout);
            yew_shadow_draw(&ed, ed.win, &layout, &ed.grid);
            elapsed = now_ns() - start;
            if (!allocation_probe_end() || elapsed < 0 ||
                layout.nlines != 1U ||
                ed.grid.back[(size_t)layout.inline_run.y * ed.grid.cols +
                             layout.inline_run.x].utf8[0] != (u8)'g' ||
                ed.grid.back[(size_t)layout.inline_run.y * ed.grid.cols +
                             layout.inline_run.x + sizeof(ghost) - 1U]
                        .utf8[0] != (u8)'s') {
                yew_ed_free(&ed);
                free(fixture);
                return false;
            }
            samples[sample] = elapsed;
            perf_sink ^= (u64)layout.inline_run.x + (u64)elapsed;
        }
        yew_ed_free(&ed);
        sort_i64(samples, SHADOW_FRAME_SAMPLES);
        medians[trial] = samples[SHADOW_FRAME_SAMPLES / 2U];
        p99s[trial] = samples[99U];
    }
    free(fixture);
    reduce_trials(medians, p99s, result);
    (void)printf("shadow.compose_midline_100kloc allocations=0 ok\n");
    return true;
}

static void reset_edit_ghost(Ed *ed, const u8 *text, size_t len)
{
    install_ghost(ed, text, len);
    ed->win->shadow.timer = YEW_TIMER_NONE;
    ed->full_damage = false;
}

static bool measure_edit(PerfResult *result)
{
    u8 suggestion[1024];
    i64 medians[SHADOW_PERF_TRIALS];
    i64 p99s[SHADOW_PERF_TRIALS];
    u32 trial;

    (void)memset(suggestion, 'x', sizeof(suggestion));
    for (trial = 0U; trial < SHADOW_PERF_TRIALS; trial++) {
        static const u8 inserted = 'x';
        Ed ed;
        EditCtx edit;
        i64 samples[SHADOW_EDIT_SAMPLES];
        u32 sample;

        if (!open_editor(&ed, &inserted, 1U, false))
            return false;
        edit = yew_ed_edit_ctx(&ed);
        for (sample = 0U; sample < SHADOW_EDIT_SAMPLES; sample++) {
            i64 start;
            i64 elapsed;

            reset_edit_ghost(&ed, suggestion, sizeof(suggestion));
            start = now_ns();
            if (start < 0) {
                yew_ed_free(&ed);
                return false;
            }
            yew_shadow_on_edit(&edit, YEW_JOURNAL_INS, BYTEOFF(0U), 1U);
            elapsed = now_ns() - start;
            if (elapsed < 0 || !ed.win->shadow.live ||
                ed.win->shadow.sug.consumed != 1U ||
                ed.win->shadow.sug.buf_gen != ed.win->buf->tb->gen ||
                ed.win->shadow.timer != YEW_TIMER_NONE) {
                yew_ed_free(&ed);
                return false;
            }
            samples[sample] = elapsed;
            perf_sink ^= (u64)ed.win->shadow.sug.consumed;
        }
        yew_ed_free(&ed);
        sort_i64(samples, SHADOW_EDIT_SAMPLES);
        medians[trial] = samples[SHADOW_EDIT_SAMPLES / 2U];
        p99s[trial] = samples[990U];
    }
    reduce_trials(medians, p99s, result);
    return true;
}

static bool check_debounce(void)
{
    Ed ed;
    size_t max_active = 0U;
    u32 i;

    (void)memset(provider_requests, 0, sizeof(provider_requests));
    if (!open_editor(&ed, NULL, 0U, false))
        return false;
    for (i = 0U; i < 100U; i++) {
        ed.now_ms = 2000 + (i64)i;
        yew_shadow_arm(&ed, ed.win);
        if (ed.timers.len > max_active)
            max_active = ed.timers.len;
        if (ed.timers.len != 1U) {
            yew_ed_free(&ed);
            return false;
        }
    }
    ed.now_ms = 2099;
    yew_timers_fire(&ed.timers, &ed, ed.now_ms);
    ed.now_ms = 2219;
    yew_timers_fire(&ed.timers, &ed, ed.now_ms);
    ed.now_ms = 2449;
    yew_timers_fire(&ed.timers, &ed, ed.now_ms);
    for (i = 0U; i < (u32)YEW_SHADOW_NPROV; i++) {
        if (provider_requests[i] != 1U) {
            yew_ed_free(&ed);
            return false;
        }
    }
    if (max_active != 1U || ed.timers.len != 0U ||
        ed.shadow_stats.requests != YEW_SHADOW_NPROV ||
        ed.shadow_stats.dropped_stale != 0U) {
        yew_ed_free(&ed);
        return false;
    }
    (void)printf("shadow.debounce_100 max_active=%zu requests=%llu "
                 "dropped_stale=%llu ok\n", max_active,
                 (unsigned long long)ed.shadow_stats.requests,
                 (unsigned long long)ed.shadow_stats.dropped_stale);
    yew_ed_free(&ed);
    return true;
}

static bool load_baselines(PerfResult *results, size_t count)
{
    FILE *file = fopen("tests/perf/baselines/shadow.txt", "r");
    char line[256];

    if (file == NULL) {
        (void)fprintf(stderr, "perf_shadow: cannot read baseline: %s\n",
                      strerror(errno));
        return false;
    }
    while (fgets(line, sizeof(line), file) != NULL) {
        char name[80];
        long long median;
        long long p99;
        size_t i;

        if (sscanf(line, "%79s %lld %lld", name, &median, &p99) != 3 ||
            median <= 0 || p99 <= 0)
            continue;
        for (i = 0U; i < count; i++) {
            if (strcmp(name, results[i].name) == 0) {
                results[i].baseline_median_ns = (i64)median;
                results[i].baseline_p99_ns = (i64)p99;
            }
        }
    }
    if (ferror(file) || fclose(file) != 0)
        return false;
    for (size_t i = 0U; i < count; i++) {
        if (results[i].baseline_median_ns <= 0 ||
            results[i].baseline_p99_ns <= 0) {
            (void)fprintf(stderr, "perf_shadow: missing baseline for %s\n",
                          results[i].name);
            return false;
        }
    }
    return true;
}

int main(int argc, char **argv)
{
    PerfResult results[] = {
        {"frame_8line_200x50", 0, 0, 0, 0,
         SHADOW_FRAME_P99_BUDGET_NS},
        {"compose_midline_100kloc", 0, 0, 0, 0,
         SHADOW_FRAME_P99_BUDGET_NS},
        {"on_edit_match", 0, 0, 0, 0, SHADOW_EDIT_P99_BUDGET_NS},
    };
    bool measure_only = argc == 2 && strcmp(argv[1], "--measure") == 0;
    bool advisory = perf_advisory();
    size_t i;
    int status = 0;

    if (argc > 2 || (argc == 2 && !measure_only)) {
        (void)fprintf(stderr, "usage: %s [--measure]\n", argv[0]);
        return 2;
    }
    if (setenv("YEW_SHADOW_TEST", "0", 1) != 0) {
        (void)fprintf(stderr, "perf_shadow: setenv: %s\n", strerror(errno));
        return 2;
    }
    register_providers();
    if (!measure_frame(&results[0])) {
        (void)fprintf(stderr, "perf_shadow: frame invariant failed\n");
        return 2;
    }
    if (!measure_midline_large(&results[1])) {
        (void)fprintf(stderr, "perf_shadow: large composition invariant failed\n");
        return 2;
    }
    if (!measure_edit(&results[2])) {
        (void)fprintf(stderr, "perf_shadow: edit invariant failed\n");
        return 2;
    }
    if (!check_debounce()) {
        (void)fprintf(stderr, "perf_shadow: debounce invariant failed\n");
        return 2;
    }
    if (!measure_only && !load_baselines(results, YEW_ARRAY_LEN(results)))
        return 2;
    for (i = 0U; i < YEW_ARRAY_LEN(results); i++) {
        bool over_absolute = results[i].p99_ns > results[i].budget_p99_ns;
        bool over_relative = !measure_only &&
            results[i].p99_ns > results[i].baseline_p99_ns +
                                results[i].baseline_p99_ns / 5;
        bool regression = over_absolute || over_relative;
        bool broken = results[i].p99_ns <= 0 ||
            results[i].p99_ns > results[i].budget_p99_ns * 100;
        const char *verdict = broken ? " BROKEN" :
            regression ? (advisory ? " ADVISORY" : " REGRESSION") :
            " ok";

        (void)printf("shadow.%s median_ns=%lld p99_ns=%lld%s\n",
                     results[i].name, (long long)results[i].median_ns,
                     (long long)results[i].p99_ns, verdict);
        if (measure_only)
            (void)printf("%s %lld %lld\n", results[i].name,
                         (long long)results[i].median_ns,
                         (long long)results[i].p99_ns);
        if (broken || (regression && !advisory))
            status = 1;
    }
    return status;
}
