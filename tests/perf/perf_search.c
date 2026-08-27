#define _POSIX_C_SOURCE 200809L

/*
 * Sprint 56 §5.5: whole-file search gates and the real incremental-search
 * keystroke path.  The whole-file rows use read-only mappings so the harness
 * does not copy a 1 GiB fixture merely to search it.  The mapping is released
 * before the editor-backed rows load the code fixture, keeping only one
 * file-sized resident representation at a time.
 *
 * `make perf-search-s56-smoke` generates small fixtures of the same profiles.
 * The full `perf-search-s56` target always uses the manifest's 1 GiB files.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "edit/ed.h"
#include "search/regex.h"
#include "search/searchui.h"
#include "term/grid.h"
#include "term/render.h"
#include "text/piece.h"
#include "ui/cmdline.h"
#include "ui/win.h"
#include "util/arena.h"
#include "util/base.h"

enum {
    RUNS = 3,
    HOSTILE_BYTES = 64U * 1024U,
    INCREMENTAL_MIN_OFFSET = 2U * 1024U * 1024U + 32U
};

typedef struct Options {
    const char *budgets;
    const char *code;
    const char *noline;
    u64 scale;
} Options;

typedef struct Mapping {
    const u8 *bytes;
    u64 len;
} Mapping;

static void usage(void)
{
    (void)fputs(
        "usage: perf_search --budgets PATH --fixture-code PATH "
        "--fixture-noline PATH [--scale-permille N]\n", stderr);
}

static bool parse_u64(const char *text, u64 *out)
{
    char *end;
    unsigned long long value;

    if (text == NULL || text[0] == '\0' || text[0] == '-')
        return false;
    errno = 0;
    value = strtoull(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || value == 0ULL ||
        (unsigned long long)(u64)value != value)
        return false;
    *out = (u64)value;
    return true;
}

static bool parse_options(int argc, char **argv, Options *out)
{
    int i;

    (void)memset(out, 0, sizeof(*out));
    out->scale = 1000U;
    for (i = 1; i < argc; i += 2) {
        const char *name;
        const char *value;

        if (i + 1 >= argc)
            return false;
        name = argv[i];
        value = argv[i + 1];
        if (strcmp(name, "--budgets") == 0 && out->budgets == NULL)
            out->budgets = value;
        else if (strcmp(name, "--fixture-code") == 0 && out->code == NULL)
            out->code = value;
        else if (strcmp(name, "--fixture-noline") == 0 &&
                 out->noline == NULL)
            out->noline = value;
        else if (strcmp(name, "--scale-permille") == 0 &&
                 out->scale == 1000U) {
            if (!parse_u64(value, &out->scale))
                return false;
        } else {
            return false;
        }
    }
    return out->budgets != NULL && out->code != NULL &&
           out->noline != NULL && out->scale >= 500U && out->scale <= 3000U;
}

static bool budget(const char *path, const char *wanted, u64 *limit)
{
    FILE *file = fopen(path, "r");
    char line[512];

    if (file == NULL)
        return false;
    while (fgets(line, sizeof(line), file) != NULL) {
        char metric[128];
        char comparison[16];
        char value[32];

        if (line[0] == '#' ||
            sscanf(line, "%127s %15s %31s", metric, comparison, value) != 3 ||
            strcmp(metric, wanted) != 0)
            continue;
        if (strcmp(comparison, "le") != 0 || !parse_u64(value, limit)) {
            (void)fclose(file);
            return false;
        }
        return fclose(file) == 0;
    }
    (void)fclose(file);
    return false;
}

static i64 now_ns(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return -1;
    return (i64)ts.tv_sec * INT64_C(1000000000) + ts.tv_nsec;
}

static bool map_file(const char *path, Mapping *out)
{
    struct stat st;
    void *bytes;
    int fd = open(path, O_RDONLY);

    (void)memset(out, 0, sizeof(*out));
    if (fd < 0 || fstat(fd, &st) != 0 || st.st_size <= 0) {
        if (fd >= 0)
            (void)close(fd);
        return false;
    }
    bytes = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    (void)close(fd);
    if (bytes == MAP_FAILED)
        return false;
    out->bytes = bytes;
    out->len = (u64)st.st_size;
    return true;
}

static void unmap_file(Mapping *mapping)
{
    if (mapping->bytes != NULL)
        (void)munmap((void *)mapping->bytes, (size_t)mapping->len);
    (void)memset(mapping, 0, sizeof(*mapping));
}

static void sort_i64(i64 *values, size_t n)
{
    size_t i;

    for (i = 1U; i < n; i++) {
        i64 value = values[i];
        size_t at = i;

        while (at > 0U && values[at - 1U] > value) {
            values[at] = values[at - 1U];
            at--;
        }
        values[at] = value;
    }
}

static bool measure_raw(const u8 *bytes, u64 len, const char *pattern,
                        i64 *elapsed, bool expect_match)
{
    i64 samples[RUNS];
    size_t runs = getenv("YEW_PERF_SMOKE") != NULL ? 1U : RUNS;
    size_t i;

    for (i = 0U; i < runs; i++) {
        Arena arena;
        YewReInput input = yew_re_input_bytes(bytes, len);
        YewReMatch match;
        YewRe *re;
        i64 begin;
        i64 end;
        bool found;

        arena_init(&arena);
        re = yew_re_compile(&arena, pattern, strlen(pattern), 0U, NULL);
        if (re == NULL) {
            arena_free_all(&arena);
            return false;
        }
        (void)memset(&match, 0, sizeof(match));
        begin = now_ns();
        found = yew_re_search(re, &input, BYTEOFF(0U), &match);
        end = now_ns();
        arena_free_all(&arena);
        if (begin < 0 || end < begin || found != expect_match)
            return false;
        samples[i] = end - begin;
    }
    sort_i64(samples, runs);
    *elapsed = samples[runs / 2U];
    return true;
}

static bool measure_early(Ed *ed, u64 inserted_at, i64 *elapsed)
{
    i64 samples[RUNS];
    size_t runs = getenv("YEW_PERF_SMOKE") != NULL ? 1U : RUNS;
    size_t i;

    for (i = 0U; i < runs; i++) {
        Arena arena;
        YewReInput input = yew_re_input_textbuf(ed->win->buf->tb);
        YewReMatch match;
        YewRe *re;
        i64 begin;
        i64 end;

        arena_init(&arena);
        re = yew_re_compile(&arena, "needle", 6U, 0U, NULL);
        if (re == NULL) {
            arena_free_all(&arena);
            return false;
        }
        (void)memset(&match, 0, sizeof(match));
        begin = now_ns();
        if (!yew_re_search(re, &input, BYTEOFF(0U), &match)) {
            arena_free_all(&arena);
            return false;
        }
        end = now_ns();
        arena_free_all(&arena);
        if (begin < 0 || end < begin || match.g[0].lo != inserted_at)
            return false;
        samples[i] = end - begin;
    }
    sort_i64(samples, runs);
    *elapsed = samples[runs / 2U];
    return true;
}

static Key text_key(u8 byte)
{
    Key key = {0};

    key.code = byte;
    key.kind = YEW_EV_KEY;
    key.ev = YEW_KEY_PRESS;
    key.ntext = 1U;
    key.text[0] = byte;
    return key;
}

static bool render_incremental(Ed *ed, u64 inserted_at, bool require_hit)
{
    u16 row;

    yew_ed_render(ed);
    if (ed->quit)
        return false;
    if (!require_hit)
        return true;
    return yew_ed_cursor(ed)->pos.v == inserted_at &&
           yew_win_view_row(ed->win,
                            yew_textbuf_line_of(ed->win->buf->tb,
                                                BYTEOFF(inserted_at)),
                            &row);
}

static bool settle_incremental(Ed *ed, u64 inserted_at, bool require_hit,
                               i64 started_ns, i64 *elapsed)
{
    size_t slices = 0U;
    i64 end;

    while (yew_search_preview_queued(ed)) {
        if (++slices > 2048U)
            return false;
        yew_search_preview_tick(ed);
        if (!render_incremental(ed, inserted_at, false))
            return false;
    }
    if (yew_search_preview_queued(ed) ||
        !render_incremental(ed, inserted_at, require_hit))
        return false;
    end = now_ns();
    if (started_ns < 0 || end < started_ns)
        return false;
    *elapsed = end - started_ns;
    return true;
}

static bool measure_incremental(Ed *ed, u64 inserted_at, i64 *elapsed)
{
    static const u8 prefix[] = {'n', 'e', 'e'};
    static const u8 suffix[] = {'d', 'l', 'e'};
    i64 samples[RUNS];
    size_t runs = getenv("YEW_PERF_SMOKE") != NULL ? 1U : RUNS;
    size_t run;

    ed->win->rect.w = 120U;
    ed->win->rect.h = 50U;
    for (run = 0U; run < runs; run++) {
        i64 worst = 0;
        size_t i;

        /* Start at the file origin.  The planted match is deliberately
         * beyond one runtime slice even in the reduced smoke fixture, so a
         * passing row proves both bounded key work and idle-turn
         * continuation. */
        yew_ed_cursor(ed)->pos = BYTEOFF(0U);
        yew_win_follow_cursor(ed->win);
        yew_search_open(ed, ed->win, false);
        /* Establish the prescribed `/nee` state through the same cmdline
         * path as the measured `/need` and `/needle` keystrokes.  Besides
         * being faithful to a real prompt, this keeps one-time widget
         * initialization out of a transition that occurs only after three
         * earlier keys in the product. */
        for (i = 0U; i < sizeof(prefix); i++) {
            Key key = text_key(prefix[i]);
            i64 begin = now_ns();
            i64 prefix_elapsed;

            if (!yew_cmdline_key(ed, &key)) {
                yew_search_cancel(ed, ed->win);
                return false;
            }
            if (!render_incremental(ed, inserted_at, false)) {
                yew_search_cancel(ed, ed->win);
                return false;
            }
            if (!settle_incremental(ed, inserted_at, false, begin,
                                    &prefix_elapsed)) {
                yew_search_cancel(ed, ed->win);
                return false;
            }
        }
        for (i = 0U; i < sizeof(suffix); i++) {
            Key key = text_key(suffix[i]);
            i64 begin = now_ns();
            i64 key_elapsed;

            if (!yew_cmdline_key(ed, &key)) {
                yew_search_cancel(ed, ed->win);
                return false;
            }
            if (!render_incremental(ed, inserted_at, false)) {
                yew_search_cancel(ed, ed->win);
                return false;
            }
            if (!settle_incremental(ed, inserted_at, true, begin,
                                    &key_elapsed)) {
                yew_search_cancel(ed, ed->win);
                return false;
            }
            if (key_elapsed > worst)
                worst = key_elapsed;
        }
        yew_search_cancel(ed, ed->win);
        if (!render_incremental(ed, inserted_at, false))
            return false;
        samples[run] = worst;
    }
    sort_i64(samples, runs);
    *elapsed = samples[runs / 2U];
    return true;
}

static bool report(const char *metric, i64 value, u64 limit, bool gate)
{
    bool broken = value < 0 || (limit <= UINT64_MAX / UINT64_C(100) &&
                                (u64)value > limit * UINT64_C(100));
    bool failed = gate && !broken && (u64)value > limit;
    const char *verdict = broken ? "BROKEN" : failed ? "FAIL" :
                          gate ? "PASS" : "ADVISORY";

    (void)printf("%s value_ns=%lld budget_ns=%llu verdict=%s\n", metric,
                 (long long)value, (unsigned long long)limit, verdict);
    return !broken && !failed;
}

static u64 scaled(u64 limit, u64 scale)
{
    return limit > UINT64_MAX / scale ? UINT64_MAX :
           limit * scale / UINT64_C(1000);
}

int main(int argc, char **argv)
{
    static const char *const metrics[] = {
        "search.literal_early.1g_code",
        "search.literal_absent.1g_code",
        "search.literal.1g_noline",
        "search.regex_anchored.1g_code",
        "search.regex_alternation.1g_code",
        "search.regex_hostile.64k",
        "search.incremental.1g_code"
    };
    Options opt;
    Mapping code = {0};
    Mapping noline = {0};
    i64 values[YEW_ARRAY_LEN(metrics)];
    u64 limits[YEW_ARRAY_LEN(metrics)];
    u8 hostile[HOSTILE_BYTES];
    Ed ed;
    EditCtx edit;
    TtyCaps caps = {0};
    u64 inserted_at;
    int sink = -1;
    bool gate;
    bool ok = true;
    size_t i;

    if (!parse_options(argc, argv, &opt)) {
        usage();
        return 2;
    }
    for (i = 0U; i < YEW_ARRAY_LEN(metrics); i++) {
        if (!budget(opt.budgets, metrics[i], &limits[i])) {
            (void)fprintf(stderr, "perf_search: missing budget %s\n",
                          metrics[i]);
            return 2;
        }
        limits[i] = scaled(limits[i], opt.scale);
    }
    gate = getenv("PERF_GATE") != NULL &&
           strcmp(getenv("PERF_GATE"), "1") == 0 &&
           !(getenv("YEW_PERF_ADVISORY") != NULL &&
             strcmp(getenv("YEW_PERF_ADVISORY"), "0") != 0);

    if (!map_file(opt.code, &code) || !map_file(opt.noline, &noline)) {
        (void)fputs("perf_search: cannot map fixtures\n", stderr);
        unmap_file(&code);
        unmap_file(&noline);
        return 2;
    }
    if (!measure_raw(code.bytes, code.len, "zzqqxx", &values[1], false) ||
        !measure_raw(noline.bytes, noline.len, "needle", &values[2], false) ||
        !measure_raw(code.bytes, code.len, "^static ", &values[3], false) ||
        !measure_raw(code.bytes, code.len,
                     "\\b(foo|bar|baz)_[0-9]+\\b", &values[4], false)) {
        (void)fputs("perf_search: whole-file measurement failed\n", stderr);
        unmap_file(&code);
        unmap_file(&noline);
        return 1;
    }
    unmap_file(&code);
    unmap_file(&noline);

    (void)memset(hostile, 'a', sizeof(hostile));
    if (!measure_raw(hostile, sizeof(hostile), "(a+)+b", &values[5], false)) {
        (void)fputs("perf_search: hostile-regex measurement failed\n", stderr);
        return 1;
    }

    yew_ed_init(&ed);
    if (yew_ed_open(&ed, opt.code) != YEW_LOAD_OK) {
        (void)fputs("perf_search: cannot load code fixture in editor\n",
                    stderr);
        yew_ed_free(&ed);
        return 2;
    }
    inserted_at = yew_textbuf_len(ed.win->buf->tb) / UINT64_C(100);
    if (inserted_at < INCREMENTAL_MIN_OFFSET)
        inserted_at = INCREMENTAL_MIN_OFFSET;
    edit = yew_ed_edit_ctx(&ed);
    if (!yew_edit_insert(&edit, BYTEOFF(inserted_at),
                         (const u8 *)"needle", 6U) ||
        !yew_grid_init(&ed.grid, &ed.interner, 50U, 120U)) {
        (void)fputs("perf_search: cannot initialize editor fixture\n",
                    stderr);
        yew_ed_free(&ed);
        return 1;
    }
    ed.grid_ready = true;
    yew_render_init(&ed.render, &caps, NULL);
    ed.render_ready = true;
    yew_ed_layout(&ed);
    sink = open("/dev/null", O_WRONLY);
    if (sink < 0) {
        (void)fputs("perf_search: cannot open render sink\n", stderr);
        yew_ed_free(&ed);
        return 2;
    }
    ed.tty.wfd = sink;
    if (!render_incremental(&ed, inserted_at, false) ||
        !measure_early(&ed, inserted_at, &values[0]) ||
        !measure_incremental(&ed, inserted_at, &values[6])) {
        (void)fputs("perf_search: editor-backed measurement failed\n", stderr);
        ed.tty.wfd = -1;
        (void)close(sink);
        yew_ed_free(&ed);
        return 1;
    }
    ed.tty.wfd = -1;
    if (close(sink) != 0) {
        (void)fputs("perf_search: cannot close render sink\n", stderr);
        yew_ed_free(&ed);
        return 2;
    }
    yew_ed_free(&ed);

    for (i = 0U; i < YEW_ARRAY_LEN(metrics); i++)
        ok = report(metrics[i], values[i], limits[i], gate) && ok;
    return ok ? 0 : 1;
}
