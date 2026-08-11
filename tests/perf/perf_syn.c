#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "edit/block.h"
#include "edit/buf.h"
#include "edit/ed.h"
#include "edit/theme_cmds.h"
#include "search/regex.h"
#include "syn/defs.h"
#include "syn/engine.h"
#include "syn/langs_gen.h"
#include "text/file.h"
#include "text/piece.h"
#include "text/undo.h"
#include "ui/draw.h"
#include "ui/layout.h"
#include "ui/viewport.h"
#include "util/arena.h"
#include "util/buf.h"
#include "util/intern.h"

enum {
    PERF_SYN_DEFAULT_SAMPLES = 1001,
    PERF_SYN_MAX_SAMPLES = 1001,
    PERF_SYN_TRIALS = 3,
    PERF_SYN_VIEW_LINES = 200,
    PERF_SYN_EDIT_LINES = 100000,
    PERF_SYN_RULES = 6,
    PERF_SYN_CTXS = 3,
    PERF_SYN_DETECT_PATHS = 10000,
    PERF_SYN_BLOCK_BYTES = 64 * 1024,
    PERF_SYN_BLOCK_LINES = 100000,
    PERF_SYN_BLOCK_MAX_LINE_CALLS = 3,
    PERF_SYN_FIXTURE_COUNT = 15,
    PERF_SYN_S42_FIRST = 7,
    PERF_SYN_JSON_INDEX = 14,
    PERF_SYN_CASE_NAME_CAP = 64,
    PERF_SYN_THEME_ROWS = 50,
    PERF_SYN_THEME_COLS = 200,
    PERF_SYN_SCROLL_FRAMES = 240,
    PERF_SYN_START_SAMPLES = 101,
    PERF_SYN_WARM_CHILD_OK = 10
};

enum {
    CASE_TOY_LINE = 0,
    CASE_TOY_VIEW,
    CASE_TOY_EDIT,
    CASE_LINE_CAP,
    CASE_FROZEN_LINE_FIRST,
    CASE_FROZEN_LINE_LAST =
        CASE_FROZEN_LINE_FIRST + PERF_SYN_FIXTURE_COUNT - 1,
    CASE_FROZEN_EDIT_FIRST,
    CASE_FROZEN_EDIT_LAST =
        CASE_FROZEN_EDIT_FIRST + PERF_SYN_FIXTURE_COUNT - 1,
    CASE_VIEW_200_FIRST,
    CASE_VIEW_200_LAST = CASE_VIEW_200_FIRST + PERF_SYN_FIXTURE_COUNT - 1,
    CASE_VIEW_24_FIRST,
    CASE_VIEW_24_LAST = CASE_VIEW_24_FIRST + PERF_SYN_FIXTURE_COUNT - 1,
    CASE_THEME_SWITCH,
    CASE_MINIFIED_FIRST_PAINT,
    PERF_SYN_CASE_COUNT
};

#define PERF_SYN_DETECT_LIMIT_NS UINT64_C(5000000)
#define PERF_SYN_COMPILE_LIMIT_NS UINT64_C(3000000)
#define PERF_SYN_CACHE_LIMIT_NS UINT64_C(200000)
#define PERF_SYN_BLOCK_LIMIT_NS UINT64_C(5000000)
#define PERF_SYN_VIEW_200_LIMIT_NS UINT64_C(1500000)
#define PERF_SYN_VIEW_24_LIMIT_NS UINT64_C(300000)
#define PERF_SYN_THEME_LIMIT_NS UINT64_C(2000000)
#define PERF_SYN_MINIFIED_LIMIT_NS UINT64_C(20000000)
#define PERF_SYN_WARM_START_LIMIT_NS UINT64_C(20000000)
#define PERF_SYN_COMMENT_TOTAL_US UINT64_C(400000)
#define PERF_SYN_STATE_LIMIT_BYTES UINT64_C(204800)
#define PERF_SYN_SCROLL_MIN_FPS 120.0

typedef struct SynFixture {
    Arena arena;
    Interner aux;
    SynCtx ctx[PERF_SYN_CTXS];
    SynRule rule[PERF_SYN_RULES];
    SynDef def;
    SynEngine *engine;
} SynFixture;

typedef struct Timing {
    u64 median;
    u64 p99;
} Timing;

typedef struct PerfCase {
    const char *name;
    Timing measured;
    Timing baseline;
} PerfCase;

static volatile u64 perf_syn_sink;

typedef struct Source {
    u8 *data;
    size_t len;
} Source;

typedef struct FrozenSpec {
    const char *stem;
    const char *source_path;
    const char *definition_path;
    u64 lines;
    size_t bytes;
} FrozenSpec;

typedef struct FrozenFixture {
    const FrozenSpec *spec;
    Source source;
    Arena arena;
    DiagCtx dc;
    SynDef *def;
    SynEngine *engine;
    TextBuf *tb;
} FrozenFixture;

typedef struct FakeClock {
    i64 now;
    i64 step;
} FakeClock;

typedef struct PaintFixture {
    Ed ed;
    Buffer buffer;
    Buffer *bufptrs[1];
    Win win;
    TtyCaps caps;
} PaintFixture;

static const FrozenSpec frozen_specs[PERF_SYN_FIXTURE_COUNT] = {
    {"c", "tests/perf/fixtures/syn/c_kitchen.c", "runtime/syntax/c.fl",
     8000U, 244U * 1024U},
    {"comment_bomb", "tests/perf/fixtures/syn/c_comment_bomb.c",
     "runtime/syntax/c.fl", 40001U, 3U + 5U * 244U * 1024U},
    {"minified", "tests/perf/fixtures/syn/c_minified.c",
     "runtime/syntax/c.fl", 1U, 512U * 1024U},
    {"fletch", "tests/perf/fixtures/syn/fl_kitchen.fl",
     "runtime/syntax/fletch.fl", 2000U, 58U * 1024U},
    {"sh", "tests/perf/fixtures/syn/sh_kitchen.sh", "runtime/syntax/sh.fl",
     3000U, 92U * 1024U},
    {"make", "tests/perf/fixtures/syn/mk_kitchen.mk",
     "runtime/syntax/make.fl", 1200U, 36U * 1024U},
    {"markdown", "tests/perf/fixtures/syn/md_kitchen.md",
     "runtime/syntax/markdown.fl", 5000U, 160U * 1024U},
    {"python", "tests/perf/fixtures/syn/py_kitchen.py",
     "runtime/syntax/python.fl", 6000U, 190U * 1024U},
    {"rust", "tests/perf/fixtures/syn/rs_kitchen.rs",
     "runtime/syntax/rust.fl", 6000U, 210U * 1024U},
    {"go", "tests/perf/fixtures/syn/go_kitchen.go",
     "runtime/syntax/go.fl", 5000U, 150U * 1024U},
    {"typescript", "tests/perf/fixtures/syn/ts_kitchen.ts",
     "runtime/syntax/typescript.fl", 5000U, 165U * 1024U},
    {"fortran", "tests/perf/fixtures/syn/f90_kitchen.f90",
     "runtime/syntax/fortran.fl", 4000U, 140U * 1024U},
    {"fortran_fixed", "tests/perf/fixtures/syn/f77_kitchen.f",
     "runtime/syntax/fortran_fixed.fl", 4000U, 130U * 1024U},
    {"yaml", "tests/perf/fixtures/syn/yaml_kitchen.yml",
     "runtime/syntax/yaml.fl", 4000U, 96U * 1024U},
    {"json", "tests/perf/fixtures/syn/json_kitchen.json",
     "runtime/syntax/json.fl", 5001U, 2252U * 1024U}
};

static bool now_ns(u64 *out)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return false;
    *out = (u64)ts.tv_sec * UINT64_C(1000000000) + (u64)ts.tv_nsec;
    return true;
}

static bool current_rss_bytes(u64 *out)
{
#if defined(__linux__)
    FILE *file = fopen("/proc/self/statm", "r");
    unsigned long long total_pages;
    unsigned long long resident_pages;
    long page_size;

    if (file != NULL) {
        int count = fscanf(file, "%llu %llu", &total_pages,
                           &resident_pages);

        (void)total_pages;
        (void)fclose(file);
        page_size = sysconf(_SC_PAGESIZE);
        if (count == 2 && page_size > 0 &&
            resident_pages <= UINT64_MAX / (u64)page_size) {
            *out = (u64)resident_pages * (u64)page_size;
            return true;
        }
    }
#endif
    {
        struct rusage usage;

        if (getrusage(RUSAGE_SELF, &usage) != 0 || usage.ru_maxrss < 0)
            return false;
#if defined(__APPLE__)
        *out = (u64)usage.ru_maxrss;
#else
        if ((u64)usage.ru_maxrss > UINT64_MAX / 1024U)
            return false;
        *out = (u64)usage.ru_maxrss * 1024U;
#endif
    }
    return true;
}

static void stable_sort(u64 *values, size_t len)
{
    for (size_t i = 1U; i < len; i++) {
        u64 value = values[i];
        size_t at = i;

        while (at != 0U && values[at - 1U] > value) {
            values[at] = values[at - 1U];
            at--;
        }
        values[at] = value;
    }
}

static Timing timing_of(u64 *samples, size_t len)
{
    Timing result;
    size_t p99 = (len * 99U + 99U) / 100U;

    stable_sort(samples, len);
    if (p99 != 0U)
        p99--;
    result.median = samples[len / 2U];
    result.p99 = samples[p99];
    return result;
}

static Timing timing_of_trials(const Timing trials[PERF_SYN_TRIALS])
{
    u64 medians[PERF_SYN_TRIALS];
    u64 p99s[PERF_SYN_TRIALS];

    for (size_t i = 0U; i < PERF_SYN_TRIALS; i++) {
        medians[i] = trials[i].median;
        p99s[i] = trials[i].p99;
    }
    stable_sort(medians, PERF_SYN_TRIALS);
    stable_sort(p99s, PERF_SYN_TRIALS);
    return (Timing){medians[PERF_SYN_TRIALS / 2U],
                    p99s[PERF_SYN_TRIALS / 2U]};
}

static size_t sample_count(void)
{
    const char *text = getenv("YEW_SYN_PERF_SAMPLES");
    char *end;
    unsigned long count;

    if (text == NULL || *text == '\0')
        return PERF_SYN_DEFAULT_SAMPLES;
    count = strtoul(text, &end, 10);
    if (*end != '\0' || count < 3UL)
        return PERF_SYN_DEFAULT_SAMPLES;
    if (count > PERF_SYN_MAX_SAMPLES)
        count = PERF_SYN_MAX_SAMPLES;
    if ((count & 1UL) == 0UL)
        count--;
    return (size_t)count;
}

static bool read_source(const char *path, Source *source)
{
    FILE *file = fopen(path, "rb");
    long size;
    bool ok;

    (void)memset(source, 0, sizeof(*source));
    if (file == NULL || fseek(file, 0L, SEEK_END) != 0 ||
        (size = ftell(file)) < 0L || fseek(file, 0L, SEEK_SET) != 0) {
        if (file != NULL)
            (void)fclose(file);
        return false;
    }
    source->data = malloc(size == 0L ? 1U : (size_t)size);
    ok = source->data != NULL &&
         (size == 0L || fread(source->data, 1U, (size_t)size, file) ==
                        (size_t)size);
    if (fclose(file) != 0)
        ok = false;
    if (!ok) {
        free(source->data);
        source->data = NULL;
        return false;
    }
    source->len = (size_t)size;
    return true;
}

static u64 source_lines(const Source *source)
{
    u64 lines = 0U;

    for (size_t i = 0U; i < source->len; i++) {
        if (source->data[i] == (u8)'\n')
            lines++;
    }
    if (source->len != 0U && source->data[source->len - 1U] != (u8)'\n')
        lines++;
    return lines;
}

static void frozen_free(FrozenFixture *fixture);

static bool frozen_init(FrozenFixture *fixture, const FrozenSpec *spec)
{
    (void)memset(fixture, 0, sizeof(*fixture));
    fixture->spec = spec;
    arena_init(&fixture->arena);
    fl_diag_init(&fixture->dc, &fixture->arena);
    if (!read_source(spec->source_path, &fixture->source) ||
        fixture->source.len != spec->bytes ||
        source_lines(&fixture->source) != spec->lines)
        goto fail;
    fixture->def = yew_syn_def_load(&fixture->arena, &fixture->dc,
                                    spec->definition_path);
    if (fixture->def == NULL)
        goto fail;
    fixture->engine = yew_syn_engine_new(fixture->def);
    fixture->tb = yew_textbuf_from_bytes(fixture->source.data,
                                         fixture->source.len);
    if (fixture->engine == NULL || fixture->tb == NULL)
        goto fail;
    return true;
fail:
    frozen_free(fixture);
    return false;
}

static void frozen_free(FrozenFixture *fixture)
{
    yew_textbuf_free(fixture->tb);
    yew_syn_engine_free(fixture->engine);
    if (fixture->def != NULL)
        yew_syn_def_dispose(fixture->def);
    arena_free_all(&fixture->arena);
    free(fixture->source.data);
    (void)memset(fixture, 0, sizeof(*fixture));
}

static i64 fake_clock(void *ctx)
{
    FakeClock *clock = ctx;

    clock->now += clock->step;
    return clock->now;
}

static bool settle_all(SynBuf *syn, const TextBuf *tb, LineNo lo,
                       LineNo hi, i64 budget_us, u64 *total_us,
                       u64 *max_us, u64 *frames)
{
    SynSettleReport report;
    u64 calls = 0U;

    do {
        yew_syn_settle(syn, tb, lo, hi, budget_us, &report);
        if (total_us != NULL)
            *total_us += report.us;
        if (max_us != NULL && report.us > *max_us)
            *max_us = report.us;
        calls++;
        if (calls > 100000U)
            return false;
    } while (!report.fixpoint);
    if (frames != NULL)
        *frames += calls;
    return true;
}

static bool measure_detect(u64 *samples, size_t count)
{
    static const char *const paths[] = {
        "config.ini", "/tmp/.editorconfig", "unit.service",
        "archive.tar.xyz", "README", "settings.properties",
        "unknown.zzz", "desktop.desktop"
    };

    for (size_t sample = 0U; sample < count; sample++) {
        u64 start;
        u64 end;

        if (!now_ns(&start))
            return false;
        for (u32 i = 0U; i < PERF_SYN_DETECT_PATHS; i++) {
            u32 lang = yew_syn_lang_for(paths[i % YEW_ARRAY_LEN(paths)],
                                        NULL, 0U);

            perf_syn_sink += lang;
        }
        if (!now_ns(&end) || end < start)
            return false;
        samples[sample] = end - start;
    }
    return true;
}

static bool measure_compile(const Source *source, u64 *samples, size_t count)
{
    for (size_t sample = 0U; sample < count; sample++) {
        Arena arena;
        DiagCtx dc;
        SynDef *def;
        u32 nerr = 0U;
        u32 nwarn = 0U;
        u64 start;
        u64 end;

        arena_init(&arena);
        fl_diag_init(&dc, &arena);
        (void)fl_diag_add_file(&dc, "runtime/syntax/ini.fl",
                               (const char *)source->data, source->len);
        if (!now_ns(&start)) {
            arena_free_all(&arena);
            return false;
        }
        def = yew_syn_def_compile(&arena, &dc, source->data, source->len,
                                  0U, &nerr, &nwarn);
        if (!now_ns(&end) || end < start || def == NULL || nerr != 0U ||
            nwarn != 0U) {
            if (def != NULL)
                yew_syn_def_dispose(def);
            arena_free_all(&arena);
            return false;
        }
        samples[sample] = end - start;
        perf_syn_sink += def->nrules;
        yew_syn_def_dispose(def);
        arena_free_all(&arena);
    }
    return true;
}

static void cache_fixture_remove(const char *root)
{
    char path[512];

    yew_syn_cache_clear();
    (void)snprintf(path, sizeof(path), "%s/yew/syn", root);
    (void)rmdir(path);
    (void)snprintf(path, sizeof(path), "%s/yew", root);
    (void)rmdir(path);
    (void)rmdir(root);
}

static bool measure_cache(u64 *samples, size_t count)
{
    char root[] = "/tmp/yew-perf-syn-XXXXXX";
    const char *old_root = getenv("XDG_CACHE_HOME");
    const char *old_bypass = getenv("YEW_NO_SYN_CACHE");
    char *saved_root = old_root == NULL ? NULL : strdup(old_root);
    char *saved_bypass = old_bypass == NULL ? NULL : strdup(old_bypass);
    Arena warm_arena;
    DiagCtx warm_dc;
    SynDef *warm = NULL;
    bool ok = false;

    if ((old_root != NULL && saved_root == NULL) ||
        (old_bypass != NULL && saved_bypass == NULL) ||
        mkdtemp(root) == NULL || setenv("XDG_CACHE_HOME", root, 1) != 0 ||
        unsetenv("YEW_NO_SYN_CACHE") != 0)
        goto done;
    yew_syn_cache_set_bypass(false);
    arena_init(&warm_arena);
    fl_diag_init(&warm_dc, &warm_arena);
    warm = yew_syn_def_load(&warm_arena, &warm_dc,
                            "runtime/syntax/ini.fl");
    if (warm == NULL)
        goto warm_done;
    yew_syn_def_dispose(warm);
    warm = NULL;
    arena_free_all(&warm_arena);

    for (size_t sample = 0U; sample < count; sample++) {
        Arena arena;
        DiagCtx dc;
        SynDef *def;
        u64 start;
        u64 end;

        arena_init(&arena);
        fl_diag_init(&dc, &arena);
        if (!now_ns(&start)) {
            arena_free_all(&arena);
            goto done_cache;
        }
        def = yew_syn_def_load(&arena, &dc, "runtime/syntax/ini.fl");
        if (!now_ns(&end) || end < start || def == NULL) {
            if (def != NULL)
                yew_syn_def_dispose(def);
            arena_free_all(&arena);
            goto done_cache;
        }
        samples[sample] = end - start;
        perf_syn_sink += def->nrules;
        yew_syn_def_dispose(def);
        arena_free_all(&arena);
    }
    ok = true;
    goto done_cache;

warm_done:
    if (warm != NULL)
        yew_syn_def_dispose(warm);
    arena_free_all(&warm_arena);
done_cache:
    cache_fixture_remove(root);
done:
    if (saved_root != NULL)
        (void)setenv("XDG_CACHE_HOME", saved_root, 1);
    else
        (void)unsetenv("XDG_CACHE_HOME");
    if (saved_bypass != NULL)
        (void)setenv("YEW_NO_SYN_CACHE", saved_bypass, 1);
    else
        (void)unsetenv("YEW_NO_SYN_CACHE");
    free(saved_root);
    free(saved_bypass);
    return ok;
}

static int prime_all_syntax(void)
{
    yew_syn_cache_set_bypass(false);
    for (size_t i = 0U; i < yew_syn_builtin_langs_len; i++) {
        Arena arena;
        DiagCtx dc;
        SynDef *def;

        arena_init(&arena);
        fl_diag_init(&dc, &arena);
        def = yew_syn_def_load(&arena, &dc,
                               yew_syn_builtin_langs[i].source);
        if (def == NULL) {
            arena_free_all(&arena);
            return 1;
        }
        yew_syn_def_dispose(def);
        arena_free_all(&arena);
    }
    return 0;
}

static int warm_start_probe(void)
{
    u32 lang;
    u64 compiled;

    yew_syn_discovery_set_bypass(true);
    yew_syn_compile_count_reset();
    if (yew_syn_lang_count() != yew_syn_builtin_langs_len)
        return 1;
    lang = yew_syn_lang_for("probe.py", NULL, 0U);
    if (lang == YEW_LANG_NONE || yew_syn_engine_for(lang) == NULL)
        return 1;
    compiled = yew_syn_compile_count();
    if (compiled > 1U)
        return 1;
    return PERF_SYN_WARM_CHILD_OK + (int)compiled;
}

static bool run_probe_child(const char *self, const char *arg, int *code)
{
    pid_t pid = fork();
    pid_t waited;
    int status;

    if (pid < 0)
        return false;
    if (pid == 0) {
        (void)execl(self, self, arg, (char *)NULL);
        _exit(127);
    }
    do {
        waited = waitpid(pid, &status, 0);
    } while (waited < 0 && errno == EINTR);
    if (waited != pid || !WIFEXITED(status))
        return false;
    *code = WEXITSTATUS(status);
    return true;
}

static bool measure_warm_start(const char *self, u64 *samples,
                               size_t count, u64 *compiled_max)
{
    char root[] = "/tmp/yew-perf-start-XXXXXX";
    const char *old_cache = getenv("XDG_CACHE_HOME");
    const char *old_config = getenv("XDG_CONFIG_HOME");
    const char *old_bypass = getenv("YEW_NO_SYN_CACHE");
    char *saved_cache = old_cache == NULL ? NULL : strdup(old_cache);
    char *saved_config = old_config == NULL ? NULL : strdup(old_config);
    char *saved_bypass = old_bypass == NULL ? NULL : strdup(old_bypass);
    bool ok = false;
    bool root_created = false;
    int code;

    *compiled_max = 0U;
    if ((old_cache != NULL && saved_cache == NULL) ||
        (old_config != NULL && saved_config == NULL) ||
        (old_bypass != NULL && saved_bypass == NULL) ||
        mkdtemp(root) == NULL)
        goto done;
    root_created = true;
    if (setenv("XDG_CACHE_HOME", root, 1) != 0 ||
        setenv("XDG_CONFIG_HOME", root, 1) != 0 ||
        unsetenv("YEW_NO_SYN_CACHE") != 0)
        goto cleanup;
    yew_syn_cache_set_bypass(false);
    if (!run_probe_child(self, "--prime-all-syntax", &code) || code != 0)
        goto cleanup;
    for (size_t i = 0U; i < count; i++) {
        u64 start;
        u64 end;
        u64 compiled;

        if (!now_ns(&start) ||
            !run_probe_child(self, "--warm-start-probe", &code) ||
            !now_ns(&end) || end < start ||
            (code != PERF_SYN_WARM_CHILD_OK &&
             code != PERF_SYN_WARM_CHILD_OK + 1))
            goto cleanup;
        compiled = (u64)(code - PERF_SYN_WARM_CHILD_OK);
        if (compiled > *compiled_max)
            *compiled_max = compiled;
        samples[i] = end - start;
    }
    ok = true;
cleanup:
    if (root_created)
        cache_fixture_remove(root);
done:
    if (saved_cache != NULL)
        (void)setenv("XDG_CACHE_HOME", saved_cache, 1);
    else
        (void)unsetenv("XDG_CACHE_HOME");
    if (saved_config != NULL)
        (void)setenv("XDG_CONFIG_HOME", saved_config, 1);
    else
        (void)unsetenv("XDG_CONFIG_HOME");
    if (saved_bypass != NULL)
        (void)setenv("YEW_NO_SYN_CACHE", saved_bypass, 1);
    else
        (void)unsetenv("YEW_NO_SYN_CACHE");
    free(saved_cache);
    free(saved_config);
    free(saved_bypass);
    return ok;
}

static void first_add(u8 first[32], u8 byte)
{
    first[byte >> 3U] |= (u8)(1U << (byte & 7U));
}

static bool rule_init(SynFixture *fx, u32 at, const char *pattern,
                      u32 flags, u8 first, u8 attr, u8 op, u16 target)
{
    SynRule *rule = &fx->rule[at];

    (void)memset(rule, 0, sizeof(*rule));
    (void)memset(rule->caps, 0xff, sizeof(rule->caps));
    rule->re = yew_re_compile(&fx->arena, pattern, strlen(pattern), flags,
                              NULL);
    if (rule->re == NULL)
        return false;
    rule->attr = attr;
    rule->op = op;
    rule->nop = 1U;
    rule->target = target;
    first_add(rule->first, first);
    return true;
}

static bool fixture_init(SynFixture *fx)
{
    (void)memset(fx, 0, sizeof(*fx));
    arena_init(&fx->arena);
    interner_init(&fx->aux, &fx->arena);
    if (!rule_init(fx, 0U, "\"", YEW_RE_LITERAL, (u8)'\"',
                   YEW_ATTR_STRING, SYN_OP_PUSH, 1U) ||
        !rule_init(fx, 1U, "/*", YEW_RE_LITERAL, (u8)'/',
                   YEW_ATTR_COMMENT, SYN_OP_PUSH, 2U) ||
        !rule_init(fx, 2U, "if", YEW_RE_LITERAL, (u8)'i',
                   YEW_ATTR_KEYWORD_CONTROL, SYN_OP_STAY, 0U) ||
        !rule_init(fx, 3U, "\\\\.", 0U, (u8)'\\',
                   YEW_ATTR_STRING_ESCAPE, SYN_OP_STAY, 0U) ||
        !rule_init(fx, 4U, "\"", YEW_RE_LITERAL, (u8)'\"',
                   YEW_ATTR_STRING, SYN_OP_POP, 0U) ||
        !rule_init(fx, 5U, "*/", YEW_RE_LITERAL, (u8)'*',
                   YEW_ATTR_COMMENT, SYN_OP_POP, 0U)) {
        interner_free(&fx->aux);
        arena_free_all(&fx->arena);
        return false;
    }
    fx->ctx[0].first_rule = 0U;
    fx->ctx[0].nrules = 3U;
    fx->ctx[0].dflt_attr = YEW_ATTR_TEXT;
    first_add(fx->ctx[0].first, (u8)'\"');
    first_add(fx->ctx[0].first, (u8)'/');
    first_add(fx->ctx[0].first, (u8)'i');
    fx->ctx[1].first_rule = 3U;
    fx->ctx[1].nrules = 2U;
    fx->ctx[1].dflt_attr = YEW_ATTR_STRING;
    fx->ctx[1].at_eol = SYN_OP_POP;
    fx->ctx[1].eol_nop = 1U;
    first_add(fx->ctx[1].first, (u8)'\\');
    first_add(fx->ctx[1].first, (u8)'\"');
    fx->ctx[2].first_rule = 5U;
    fx->ctx[2].nrules = 1U;
    fx->ctx[2].dflt_attr = YEW_ATTR_COMMENT;
    first_add(fx->ctx[2].first, (u8)'*');
    fx->def.name = "perf-toy";
    fx->def.root = 0U;
    fx->def.nctxs = PERF_SYN_CTXS;
    fx->def.nrules = PERF_SYN_RULES;
    fx->def.ctxs = fx->ctx;
    fx->def.rules = fx->rule;
    fx->def.aux = &fx->aux;
    fx->engine = yew_syn_engine_new(&fx->def);
    if (fx->engine == NULL) {
        interner_free(&fx->aux);
        arena_free_all(&fx->arena);
        return false;
    }
    return true;
}

static void fixture_free(SynFixture *fx)
{
    yew_syn_engine_free(fx->engine);
    interner_free(&fx->aux);
    arena_free_all(&fx->arena);
}

static TextBuf *line_fixture(size_t lines)
{
    static const u8 row[] = "if value = \"text\\n\"; /* note */\n";
    size_t row_len = sizeof(row) - 1U;
    size_t len = lines * row_len;
    u8 *bytes = malloc(len == 0U ? 1U : len);

    if (bytes == NULL)
        return NULL;
    for (size_t i = 0U; i < lines; i++)
        (void)memcpy(bytes + i * row_len, row, row_len);
    return yew_textbuf_from_owned_bytes(bytes, len);
}

static bool measure_line(SynFixture *fx, u64 *samples, size_t count)
{
    static const u8 line[] =
        "if plain = \"one\\n two\"; /* comment */ if other = \"three\";";
    SynSpan spans[64];

    for (size_t i = 0U; i < count; i++) {
        SynLineOut out = {spans, 0U, 64U, 0U, 0U};
        u64 start;
        u64 end;

        if (!now_ns(&start))
            return false;
        yew_syn_line(fx->engine, YEW_SYN_STATE_ROOT, line,
                     (u32)(sizeof(line) - 1U), &out);
        if (!now_ns(&end) || end < start)
            return false;
        samples[i] = end - start;
        perf_syn_sink += out.n + out.exit_state;
    }
    return true;
}

static bool measure_viewport(SynFixture *fx, TextBuf *tb, u64 *samples,
                             size_t count)
{
    for (size_t i = 0U; i < count; i++) {
        SynBuf syn;
        SynSettleReport report;
        u64 start;
        u64 end;

        yew_syn_buf_init(&syn);
        yew_syn_buf_bind(&syn, fx->engine);
        yew_syn_attach(&syn, 1U, tb);
        if (!now_ns(&start)) {
            yew_syn_detach(&syn);
            return false;
        }
        yew_syn_settle(&syn, tb, LINENO(0U), LINENO(PERF_SYN_VIEW_LINES),
                       INT64_C(1000000000), &report);
        if (!now_ns(&end) || end < start || !report.fixpoint) {
            yew_syn_detach(&syn);
            return false;
        }
        samples[i] = end - start;
        perf_syn_sink += report.lines;
        yew_syn_detach(&syn);
    }
    return true;
}

static bool measure_frozen_viewport(FrozenFixture *fixture, u64 rows,
                                    u64 *samples, size_t count)
{
    size_t prefix = 0U;
    u64 lines = 0U;
    TextBuf *tb;

    while (prefix < fixture->source.len && lines < rows) {
        if (fixture->source.data[prefix++] == (u8)'\n')
            lines++;
    }
    if (lines < rows)
        prefix = fixture->source.len;
    tb = yew_textbuf_from_bytes(fixture->source.data, prefix);
    if (tb == NULL)
        return false;
    for (size_t i = 0U; i < count; i++) {
        SynBuf syn;
        SynSettleReport report;
        u64 start;
        u64 end;

        yew_syn_buf_init(&syn);
        yew_syn_buf_bind(&syn, fixture->engine);
        yew_syn_attach(&syn, 1U, tb);
        if (!now_ns(&start)) {
            yew_syn_detach(&syn);
            yew_textbuf_free(tb);
            return false;
        }
        yew_syn_settle(&syn, tb, LINENO(0U), LINENO(rows),
                       INT64_C(1000000000), &report);
        if (!now_ns(&end) || end < start ||
            (!report.fixpoint && report.lines < rows)) {
            yew_syn_detach(&syn);
            yew_textbuf_free(tb);
            return false;
        }
        samples[i] = end - start;
        perf_syn_sink += report.lines;
        yew_syn_detach(&syn);
    }
    yew_textbuf_free(tb);
    return true;
}

static bool measure_frozen_line(FrozenFixture *fixture, u64 *samples,
                                size_t count)
{
    size_t lo = 0U;
    u64 line = 0U;
    u32 entry_state = YEW_SYN_STATE_ROOT;
    SynSpan spans[YEW_SYN_MAX_SPANS];

    /* Sample the whole fixture evenly while carrying the real sequential
     * entry state through untimed rows.  Sequentially timing the first N rows
     * makes reduced-sample CI runs measure only the fixture prefix; forcing
     * root state on an arbitrary multiline row measures an impossible editor
     * state.  Use the median of three calls per sampled row so an isolated
     * scheduler preemption cannot manufacture an engine p99 regression. */
    for (size_t i = 0U; i < count; i++) {
        u64 target = (u64)i * fixture->spec->lines / (u64)count;
        u64 elapsed[3];
        size_t hi;
        u32 exit_state = entry_state;

        while (line < target) {
            SynLineOut skipped = {spans, 0U, YEW_SYN_MAX_SPANS, 0U, 0U};

            hi = lo;
            while (hi < fixture->source.len &&
                   fixture->source.data[hi] != (u8)'\n')
                hi++;
            if (hi - lo > UINT32_MAX)
                return false;
            yew_syn_line(fixture->engine, entry_state,
                         fixture->source.data + lo, (u32)(hi - lo),
                         &skipped);
            if (skipped.stop != YEW_SYN_STOP_OK &&
                skipped.stop != YEW_SYN_STOP_BYTES)
                return false;
            entry_state = skipped.exit_state;
            lo = hi < fixture->source.len ? hi + 1U : hi;
            line++;
        }
        hi = lo;
        while (hi < fixture->source.len &&
               fixture->source.data[hi] != (u8)'\n')
            hi++;
        if (hi - lo > UINT32_MAX)
            return false;
        for (size_t attempt = 0U; attempt < YEW_ARRAY_LEN(elapsed);
             attempt++) {
            SynLineOut out = {spans, 0U, YEW_SYN_MAX_SPANS, 0U, 0U};
            u64 start;
            u64 end;

            if (!now_ns(&start))
                return false;
            yew_syn_line(fixture->engine, entry_state,
                         fixture->source.data + lo, (u32)(hi - lo), &out);
            if (!now_ns(&end) || end < start ||
                (out.stop != YEW_SYN_STOP_OK &&
                 out.stop != YEW_SYN_STOP_BYTES))
                return false;
            elapsed[attempt] = end - start;
            exit_state = out.exit_state;
            perf_syn_sink += out.n + out.exit_state;
        }
        if (elapsed[0] > elapsed[1]) {
            u64 swap = elapsed[0];

            elapsed[0] = elapsed[1];
            elapsed[1] = swap;
        }
        if (elapsed[1] > elapsed[2]) {
            u64 swap = elapsed[1];

            elapsed[1] = elapsed[2];
            elapsed[2] = swap;
        }
        if (elapsed[0] > elapsed[1])
            elapsed[1] = elapsed[0];
        samples[i] = elapsed[1];
        entry_state = exit_state;
        lo = hi < fixture->source.len ? hi + 1U : hi;
        line++;
    }
    return true;
}

static bool measure_frozen_edit(FrozenFixture *fixture, u64 *samples,
                                size_t count)
{
    TextBuf *tb = yew_textbuf_from_bytes(fixture->source.data,
                                         fixture->source.len);
    SynBuf syn;
    bool ok = false;

    if (tb == NULL)
        return false;
    yew_syn_buf_init(&syn);
    yew_syn_buf_bind(&syn, fixture->engine);
    yew_syn_attach(&syn, 1U, tb);
    if (!settle_all(&syn, tb, LINENO(0U), LINENO(200U), INT64_MAX,
                    NULL, NULL, NULL))
        goto done;
    for (size_t i = 0U; i < count; i++) {
        static const u8 byte = (u8)'x';
        SynSettleReport report;
        SynSettleReport restore;
        u64 start;
        u64 end;

        yew_textbuf_insert(tb, BYTEOFF(4U), &byte, 1U);
        yew_syn_edit(&syn, LINENO(0U), 0U, 0U);
        if (!now_ns(&start))
            goto done;
        yew_syn_settle(&syn, tb, LINENO(0U), LINENO(200U), INT64_MAX,
                       &report);
        if (!now_ns(&end) || end < start || !report.fixpoint ||
            report.lines > 2U)
            goto done;
        samples[i] = end - start;
        perf_syn_sink += report.lines;
        yew_textbuf_delete(tb, (Span){4U, 5U});
        yew_syn_edit(&syn, LINENO(0U), 0U, 0U);
        yew_syn_settle(&syn, tb, LINENO(0U), LINENO(200U), INT64_MAX,
                       &restore);
        if (!restore.fixpoint || restore.lines > 2U)
            goto done;
    }
    ok = true;
done:
    yew_syn_detach(&syn);
    yew_textbuf_free(tb);
    return ok;
}

static bool check_comment_bomb(FrozenFixture *fixture,
                               u64 *first_frame_max_us,
                               u64 *first_frame_frames,
                               u64 *idle_total_us, u64 *idle_max_us,
                               u64 *idle_frames,
                               u64 *state_logical_bytes,
                               u64 *state_capacity_bytes, u64 *rss_growth,
                               u64 *wall_ns)
{
    static const u8 frozen_prefix[] = "/*";
    static const u8 paste[] = "/*";
    enum { VIEW_LO = 100, VIEW_HI = 124 };
    SynBuf syn;
    FakeClock clock = {0, YEW_SYN_FRAME_BUDGET_US / 2};
    SynSettleReport report;
    TextBuf *tb;
    u64 started;
    u64 ended;
    u64 rss_before;
    u64 rss_after;
    u64 base_lines;
    bool ok = false;

    *first_frame_max_us = 0U;
    *first_frame_frames = 0U;
    *idle_total_us = 0U;
    *idle_max_us = 0U;
    *idle_frames = 0U;
    _Static_assert(VIEW_HI < YEW_SYN_INJECTED_CLOCK_EVERY,
                   "comment-bomb view must precede the second clock sample");
    if (fixture->source.len < sizeof(frozen_prefix) - 1U ||
        memcmp(fixture->source.data, frozen_prefix,
               sizeof(frozen_prefix) - 1U) != 0)
        return false;
    tb = yew_textbuf_from_bytes(
        fixture->source.data + sizeof(frozen_prefix) - 1U,
        fixture->source.len - (sizeof(frozen_prefix) - 1U));
    if (tb == NULL)
        return false;
    base_lines = yew_textbuf_line_count(tb);
    if (!current_rss_bytes(&rss_before))
        goto done_tb;
    yew_syn_buf_init(&syn);
    yew_syn_buf_bind(&syn, fixture->engine);
    yew_syn_attach(&syn, 1U, tb);
    if (!settle_all(&syn, tb, LINENO(0U), LINENO(VIEW_HI), INT64_MAX,
                    NULL, NULL, NULL))
        goto done_syn;

    /* Exercise the actual pathological edit, rather than timing a cold
     * attach to an already-bombed fixture.  The frozen file supplies the
     * settled body; the benchmark pastes the unterminated opener itself. */
    yew_syn_buf_set_clock(&syn, fake_clock, &clock);
    yew_textbuf_insert(tb, BYTEOFF(0U), paste, sizeof(paste) - 1U);
    yew_syn_edit(&syn, LINENO(0U), 0U, 0U);
    if (yew_textbuf_len(tb) != fixture->source.len ||
        yew_textbuf_line_count(tb) != base_lines)
        goto done_syn;
    *state_logical_bytes = syn.entry.len * sizeof(*syn.entry.data);
    *state_capacity_bytes = syn.entry.cap * sizeof(*syn.entry.data);
    if (!now_ns(&started))
        goto done_syn;
    yew_syn_settle(&syn, tb, LINENO(VIEW_LO), LINENO(VIEW_HI),
                   YEW_SYN_FRAME_BUDGET_US, &report);
    *first_frame_max_us = report.us;
    *first_frame_frames = 1U;
    /* The first observation stays below budget.  The second, after line 256,
     * reaches exactly 1 ms: past the complete viewport, but far short of the
     * 40k-line fixpoint.  This makes the frame boundary a deterministic gate
     * rather than depending on host speed. */
    if (!report.hit_view || report.fixpoint ||
        report.us != YEW_SYN_FRAME_BUDGET_US ||
        syn.wave.v < VIEW_HI || syn.wave.v >= syn.entry.len)
        goto done_syn;
    for (u64 line = VIEW_LO; line < VIEW_HI; line++) {
        SynSpan spans[YEW_SYN_MAX_SPANS];
        SynLineOut out = {spans, 0U, YEW_ARRAY_LEN(spans), 0U, 0U};

        yew_syn_spans(&syn, tb, LINENO(line), &out);
        if (out.n == 0U || out.stop != YEW_SYN_STOP_OK)
            goto done_syn;
        for (u32 i = 0U; i < out.n; i++) {
            if (out.spans[i].attr != YEW_ATTR_COMMENT &&
                out.spans[i].attr != YEW_ATTR_COMMENT_DOC &&
                out.spans[i].attr != YEW_ATTR_COMMENT_TODO)
                goto done_syn;
        }
    }
    if (!settle_all(&syn, tb, LINENO(VIEW_LO), LINENO(VIEW_HI),
                    YEW_SYN_IDLE_BUDGET_US, idle_total_us, idle_max_us,
                    idle_frames)) {
        goto done_syn;
    }
    if (!now_ns(&ended) || ended < started ||
        !current_rss_bytes(&rss_after))
        goto done_syn;
    *rss_growth = rss_after > rss_before ? rss_after - rss_before : 0U;
    *wall_ns = ended - started;
    ok = true;
done_syn:
    yew_syn_detach(&syn);
done_tb:
    yew_textbuf_free(tb);
    return ok;
}

static bool measure_whole_settle(FrozenFixture *fixture, u64 *total_ns,
                                 u64 *max_frame_ns, u64 *frames)
{
    SynBuf syn;
    SynSettleReport report;

    *total_ns = 0U;
    *max_frame_ns = 0U;
    *frames = 0U;
    yew_syn_buf_init(&syn);
    yew_syn_buf_bind(&syn, fixture->engine);
    yew_syn_attach(&syn, 1U, fixture->tb);
    do {
        u64 start;
        u64 end;
        u64 elapsed;

        if (!now_ns(&start)) {
            yew_syn_detach(&syn);
            return false;
        }
        yew_syn_settle(&syn, fixture->tb, LINENO(0U), LINENO(200U),
                       YEW_SYN_FRAME_BUDGET_US, &report);
        if (!now_ns(&end) || end < start) {
            yew_syn_detach(&syn);
            return false;
        }
        elapsed = end - start;
        *total_ns += elapsed;
        if (elapsed > *max_frame_ns)
            *max_frame_ns = elapsed;
        (*frames)++;
        if (*frames > 100000U) {
            yew_syn_detach(&syn);
            return false;
        }
    } while (!report.fixpoint);
    yew_syn_detach(&syn);
    return true;
}

static void paint_free(PaintFixture *paint);

static bool paint_switch_theme(PaintFixture *paint, const char *name)
{
    char error[192];

    return yew_theme_set(&paint->ed, name, error, sizeof(error));
}

static bool paint_init(PaintFixture *paint, FrozenFixture *fixture,
                       u16 rows, u16 cols, bool wrap, bool load_file,
                       bool settle_syntax)
{
    Cursor cursor;

    (void)memset(paint, 0, sizeof(*paint));
    arena_init(&paint->ed.arena);
    interner_init(&paint->ed.interner, &paint->ed.arena);
    bytebuf_init(&paint->ed.frame);
    yew_theme_init(&paint->ed.theme);
    if (!yew_grid_init(&paint->ed.grid, &paint->ed.interner, rows, cols))
        goto fail;
    yew_filemeta_init(&paint->buffer.meta);
    if (load_file) {
        if (yew_file_load(fixture->spec->source_path, &paint->buffer.tb,
                          &paint->buffer.meta) != YEW_LOAD_OK)
            goto fail;
    } else {
        paint->buffer.tb = yew_textbuf_from_bytes(fixture->source.data,
                                                  fixture->source.len);
    }
    if (paint->buffer.tb == NULL)
        goto fail;
    paint->buffer.undo = yew_undo_new(paint->buffer.tb);
    yew_undo_mark_saved(paint->buffer.undo);
    paint->buffer.path = (char *)fixture->spec->source_path;
    paint->buffer.lang = (char *)fixture->spec->stem;
    paint->buffer.tabwidth = 4U;
    yew_syn_buf_init(&paint->buffer.syn);
    yew_syn_buf_bind(&paint->buffer.syn, fixture->engine);
    yew_syn_attach(&paint->buffer.syn, 1U, paint->buffer.tb);
    if (settle_syntax) {
        SynSettleReport report;

        yew_syn_settle(&paint->buffer.syn, paint->buffer.tb, LINENO(0U),
                       LINENO(rows), INT64_MAX, &report);
        if (!report.fixpoint)
            goto fail;
    }
    cursor.pos = settle_syntax ? yew_textbuf_line_start(
        paint->buffer.tb,
        LINENO(yew_textbuf_line_count(paint->buffer.tb) / 2U)) :
        BYTEOFF(0U);
    cursor.anchor = cursor.pos;
    cursor.goal_col = (GCol){0U};
    paint->win.buf = &paint->buffer;
    yew_cset_init(&paint->win.cs, cursor);
    yew_vp_init(&paint->win);
    paint->win.vp.wrap = wrap;
    paint->win.number_style = YEW_NUM_HYBRID;
    paint->win.syn_spans = calloc(YEW_SYN_MAX_SPANS,
                                  sizeof(*paint->win.syn_spans));
    paint->win.syn_spans_cap = YEW_SYN_MAX_SPANS;
    if (paint->win.syn_spans == NULL)
        goto fail;
    paint->ed.mode = YEW_MODE_L;
    paint->ed.prev_unit = YEW_MODE_L;
    paint->ed.win = &paint->win;
    paint->bufptrs[0] = &paint->buffer;
    paint->ed.ws.bufs = paint->bufptrs;
    paint->ed.ws.nbufs = 1U;
    paint->caps.truecolor = true;
    yew_render_init(&paint->ed.render, &paint->caps, NULL);
    paint->ed.render_ready = true;
    paint->ed.grid_ready = true;
    if (!paint_switch_theme(paint, "quiver-dark"))
        goto fail;
    yew_layout(&paint->ed);
    yew_draw_win(&paint->ed, &paint->win);
    yew_grid_mark_all(&paint->ed.grid);
    (void)yew_render_frame(&paint->ed.render, &paint->ed.grid,
                           &paint->ed.frame);
    yew_grid_flip(&paint->ed.grid);
    return true;
fail:
    paint_free(paint);
    return false;
}

static void paint_free(PaintFixture *paint)
{
    free(paint->win.syn_spans);
    yew_vp_free(&paint->win);
    yew_cset_free(&paint->win.cs);
    yew_syn_detach(&paint->buffer.syn);
    yew_undo_free(paint->buffer.undo);
    yew_textbuf_free(paint->buffer.tb);
    yew_filemeta_dispose(&paint->buffer.meta);
    yew_grid_free(&paint->ed.grid);
    yew_theme_free(&paint->ed.theme);
    free(paint->ed.theme_last_dark);
    free(paint->ed.theme_last_light);
    bytebuf_free(&paint->ed.frame);
    interner_free(&paint->ed.interner);
    arena_free_all(&paint->ed.arena);
}

static bool measure_theme_switch(FrozenFixture *fixture, u64 *samples,
                                 size_t count, u64 *max_line_calls)
{
    PaintFixture paint;
    bool ok = false;

    *max_line_calls = 0U;
    if (!paint_init(&paint, fixture, PERF_SYN_THEME_ROWS,
                    PERF_SYN_THEME_COLS, false, false, true))
        return false;
    for (size_t i = 0U; i < count; i++) {
        const char *name = (i & 1U) == 0U ? "quiver-light" : "quiver-dark";
        u64 start;
        u64 end;
        u64 calls;

        yew_syn_engine_reset_counters(fixture->engine);
        if (!now_ns(&start) || !paint_switch_theme(&paint, name))
            goto done;
        yew_draw_win(&paint.ed, &paint.win);
        paint.ed.frame.len = 0U;
        (void)yew_render_frame(&paint.ed.render, &paint.ed.grid,
                               &paint.ed.frame);
        yew_grid_flip(&paint.ed.grid);
        if (!now_ns(&end) || end < start)
            goto done;
        calls = yew_syn_engine_line_calls(fixture->engine);
        if (calls > *max_line_calls)
            *max_line_calls = calls;
        if (calls != 0U)
            goto done;
        samples[i] = end - start;
        perf_syn_sink += paint.ed.frame.len;
    }
    ok = true;
done:
    paint_free(&paint);
    return ok;
}

static bool measure_minified_first_paint(FrozenFixture *fixture,
                                         u64 *samples, size_t count)
{
    for (size_t i = 0U; i < count; i++) {
        PaintFixture paint;
        u64 start;
        u64 end;
        u64 calls;

        yew_syn_engine_reset_counters(fixture->engine);
        if (!now_ns(&start) ||
            !paint_init(&paint, fixture, 24U, 80U, false, true, false))
            return false;
        if (!now_ns(&end) || end < start) {
            paint_free(&paint);
            return false;
        }
        calls = yew_syn_engine_line_calls(fixture->engine);
        if (!paint.buffer.syn.settling ||
            paint.buffer.syn.settled_to.v >= paint.buffer.syn.entry.len ||
            calls == 0U || calls > 24U) {
            paint_free(&paint);
            return false;
        }
        samples[i] = end - start;
        perf_syn_sink += paint.ed.frame.len + calls;
        paint_free(&paint);
    }
    return true;
}

static bool measure_scroll(FrozenFixture *fixture, bool wrap, double *fps)
{
    PaintFixture paint;
    u64 start;
    u64 end;

    if (!paint_init(&paint, fixture, PERF_SYN_THEME_ROWS,
                    PERF_SYN_THEME_COLS, wrap, false, true))
        return false;
    if (!now_ns(&start)) {
        paint_free(&paint);
        return false;
    }
    for (u32 frame = 0U; frame < PERF_SYN_SCROLL_FRAMES; frame++) {
        yew_vp_scroll(&paint.win, 1);
        yew_vp_push_cursor(&paint.win);
        yew_draw_win(&paint.ed, &paint.win);
        yew_grid_mark_all(&paint.ed.grid);
        paint.ed.frame.len = 0U;
        perf_syn_sink += yew_render_frame(&paint.ed.render, &paint.ed.grid,
                                          &paint.ed.frame);
        yew_grid_flip(&paint.ed.grid);
    }
    if (!now_ns(&end) || end <= start) {
        paint_free(&paint);
        return false;
    }
    *fps = (double)PERF_SYN_SCROLL_FRAMES * 1000000000.0 /
           (double)(end - start);
    paint_free(&paint);
    return true;
}

static bool check_all_state_memory(FrozenFixture *fixtures,
                                   u64 *capacity_bytes, u64 *limit_bytes)
{
    SynBuf syn[PERF_SYN_FIXTURE_COUNT];
    size_t initialized = 0U;
    bool ok = false;

    *capacity_bytes = 0U;
    *limit_bytes = 0U;
    (void)memset(syn, 0, sizeof(syn));
    for (size_t i = 0U; i < PERF_SYN_FIXTURE_COUNT; i++) {
        u64 lines;
        u64 line_bytes;

        yew_syn_buf_init(&syn[i]);
        yew_syn_buf_bind(&syn[i], fixtures[i].engine);
        yew_syn_attach(&syn[i], 1U, fixtures[i].tb);
        initialized++;
        lines = yew_textbuf_line_count(fixtures[i].tb);
        if (!settle_all(&syn[i], fixtures[i].tb, LINENO(0U),
                        LINENO(lines), INT64_MAX, NULL, NULL, NULL) ||
            lines > UINT64_MAX / sizeof(*syn[i].entry.data))
            goto done;
        line_bytes = lines * sizeof(*syn[i].entry.data);
        if ((u64)syn[i].entry.cap >
                (UINT64_MAX - *capacity_bytes) /
                    sizeof(*syn[i].entry.data) ||
            line_bytes > UINT64_MAX - *limit_bytes ||
            fixtures[i].source.len / 100U >
                UINT64_MAX - *limit_bytes - line_bytes)
            goto done;
        *capacity_bytes +=
            (u64)syn[i].entry.cap * sizeof(*syn[i].entry.data);
        *limit_bytes += line_bytes + fixtures[i].source.len / 100U;
    }
    ok = true;
done:
    for (size_t i = 0U; i < initialized; i++)
        yew_syn_detach(&syn[i]);
    return ok;
}

static bool measure_edit(SynFixture *fx, TextBuf *tb, u64 *samples,
                         size_t count)
{
    SynBuf syn;

    yew_syn_buf_init(&syn);
    yew_syn_buf_bind(&syn, fx->engine);
    yew_syn_attach(&syn, 1U, tb);
    /* Every fixture row is balanced and therefore exits ROOT.  Seed that
     * proven state directly: this scenario times the post-edit fixpoint,
     * not a redundant 100k-line cold highlight (the viewport case owns
     * cold cost). */
    for (size_t i = 0U; i < syn.entry.len; i++)
        syn.entry.data[i] = YEW_SYN_STATE_ROOT;
    syn.wave = LINENO(syn.entry.len);
    syn.settled_to = LINENO(syn.entry.len);
    syn.must_reach = LINENO(0U);
    syn.settling = false;
    for (size_t i = 0U; i < count; i++) {
        static const u8 byte = (u8)'x';
        SynSettleReport report;
        SynSettleReport restore;
        u64 start;
        u64 end;

        yew_textbuf_insert(tb, BYTEOFF(2U), &byte, 1U);
        yew_syn_edit(&syn, LINENO(0U), 0U, 0U);
        if (!now_ns(&start)) {
            yew_syn_detach(&syn);
            return false;
        }
        yew_syn_settle(&syn, tb, LINENO(0U), LINENO(200U),
                       INT64_C(1000000000), &report);
        if (!now_ns(&end) || end < start || !report.fixpoint ||
            report.lines > 2U) {
            yew_syn_detach(&syn);
            return false;
        }
        samples[i] = end - start;
        perf_syn_sink += report.lines;
        yew_textbuf_delete(tb, (Span){2U, 3U});
        yew_syn_edit(&syn, LINENO(0U), 0U, 0U);
        yew_syn_settle(&syn, tb, LINENO(0U), LINENO(200U),
                       INT64_C(1000000000), &restore);
        if (!restore.fixpoint || restore.lines > 2U) {
            yew_syn_detach(&syn);
            return false;
        }
    }
    yew_syn_detach(&syn);
    return true;
}

static bool measure_cap(SynFixture *fx, u8 *line, u64 *samples,
                        size_t count)
{
    SynSpan span[1];

    for (size_t i = 0U; i < count; i++) {
        SynLineOut out = {span, 0U, 1U, 0U, 0U};
        u64 start;
        u64 end;

        if (!now_ns(&start))
            return false;
        yew_syn_line(fx->engine, YEW_SYN_STATE_ROOT, line, 512U * 1024U,
                     &out);
        if (!now_ns(&end) || end < start ||
            out.stop != YEW_SYN_STOP_BYTES ||
            out.exit_state != YEW_SYN_STATE_ROOT)
            return false;
        samples[i] = end - start;
        perf_syn_sink += out.stop;
    }
    return true;
}

static bool measure_block_provider(SynFixture *fx, u64 *samples,
                                   size_t count, u64 *max_line_calls)
{
    Buffer buf = {0};
    UnitCtx unit;
    SynSettleReport report;
    u8 *bytes = malloc(PERF_SYN_BLOCK_BYTES);
    Span span;
    bool ok = false;

    *max_line_calls = 0U;
    if (bytes == NULL)
        return false;
    bytes[0] = (u8)'"';
    (void)memset(bytes + 1U, 'a', PERF_SYN_BLOCK_BYTES - 2U);
    bytes[PERF_SYN_BLOCK_BYTES - 1U] = (u8)'"';
    buf.tb = yew_textbuf_from_owned_bytes(bytes, PERF_SYN_BLOCK_BYTES);
    if (buf.tb == NULL) {
        free(bytes);
        return false;
    }
    buf.lang = "perf-toy";
    buf.tabwidth = 4U;
    fx->ctx[0].flags = YEW_SYN_CTX_UNIT_SPAN;
    fx->ctx[1].flags = YEW_SYN_CTX_UNIT_ATOM;
    yew_syn_engine_set_def(fx->engine, &fx->def);
    yew_syn_buf_init(&buf.syn);
    yew_syn_buf_bind(&buf.syn, fx->engine);
    yew_syn_attach(&buf.syn, 1U, buf.tb);
    yew_syn_settle(&buf.syn, buf.tb, LINENO(0U), LINENO(1U),
                   INT64_C(1000000000), &report);
    if (!report.fixpoint)
        goto done;
    unit = (UnitCtx){buf.tb, &buf, NULL};
    yew_block_provider_syntax_install(true);

    /* Warm provider registration and allocator paths before sampling. */
    yew_syn_engine_reset_counters(fx->engine);
    if (!yew_block_level(&unit, BYTEOFF(PERF_SYN_BLOCK_BYTES / 2U),
                         0U, &span) || span.lo != 1U ||
        span.hi != PERF_SYN_BLOCK_BYTES ||
        yew_syn_engine_line_calls(fx->engine) >
            PERF_SYN_BLOCK_MAX_LINE_CALLS)
        goto provider_done;

    for (size_t i = 0U; i < count; i++) {
        u64 start;
        u64 end;
        u64 line_calls;

        yew_syn_engine_reset_counters(fx->engine);
        if (!now_ns(&start) ||
            !yew_block_level(&unit, BYTEOFF(PERF_SYN_BLOCK_BYTES / 2U),
                             0U, &span) ||
            !now_ns(&end) || end < start || span.lo != 1U ||
            span.hi != PERF_SYN_BLOCK_BYTES)
            goto provider_done;
        line_calls = yew_syn_engine_line_calls(fx->engine);
        if (line_calls > PERF_SYN_BLOCK_MAX_LINE_CALLS)
            goto provider_done;
        if (line_calls > *max_line_calls)
            *max_line_calls = line_calls;
        samples[i] = end - start;
        perf_syn_sink += span.lo + span.hi + line_calls;
    }
    ok = true;

provider_done:
    yew_block_provider_syntax_install(false);
done:
    yew_syn_detach(&buf.syn);
    yew_textbuf_free(buf.tb);
    return ok;
}

static bool measure_block_multiline(SynFixture *fx, u64 *samples,
                                    size_t count, u64 *max_line_calls)
{
    size_t len = 3U + ((size_t)PERF_SYN_BLOCK_LINES - 2U) * 2U + 2U;
    u8 *bytes = malloc(len);
    Buffer buf = {0};
    UnitCtx unit;
    SynSettleReport report;
    Span span;
    u64 at;
    bool ok = false;

    *max_line_calls = 0U;
    if (bytes == NULL)
        return false;
    (void)memcpy(bytes, "/*\n", 3U);
    for (u32 line = 1U; line + 1U < PERF_SYN_BLOCK_LINES; line++) {
        bytes[3U + ((size_t)line - 1U) * 2U] = (u8)'x';
        bytes[4U + ((size_t)line - 1U) * 2U] = (u8)'\n';
    }
    (void)memcpy(bytes + len - 2U, "*/", 2U);
    buf.tb = yew_textbuf_from_owned_bytes(bytes, len);
    if (buf.tb == NULL) {
        free(bytes);
        return false;
    }
    buf.lang = "perf-toy";
    buf.tabwidth = 4U;
    fx->ctx[2].flags = YEW_SYN_CTX_UNIT_ATOM;
    yew_syn_engine_set_def(fx->engine, &fx->def);
    yew_syn_buf_init(&buf.syn);
    yew_syn_buf_bind(&buf.syn, fx->engine);
    yew_syn_attach(&buf.syn, 1U, buf.tb);
    yew_syn_settle(&buf.syn, buf.tb, LINENO(0U),
                   LINENO(PERF_SYN_BLOCK_LINES), INT64_C(1000000000),
                   &report);
    if (!report.fixpoint)
        goto done;
    unit = (UnitCtx){buf.tb, &buf, NULL};
    at = 3U + ((u64)PERF_SYN_BLOCK_LINES / 2U - 1U) * 2U;
    yew_block_provider_syntax_install(true);

    yew_syn_engine_reset_counters(fx->engine);
    if (!yew_block_level(&unit, BYTEOFF(at), 0U, &span) ||
        span.lo != 2U || span.hi != len ||
        yew_syn_engine_line_calls(fx->engine) >
            PERF_SYN_BLOCK_MAX_LINE_CALLS)
        goto provider_done;

    for (size_t i = 0U; i < count; i++) {
        u64 start;
        u64 end;
        u64 line_calls;

        yew_syn_engine_reset_counters(fx->engine);
        if (!now_ns(&start) ||
            !yew_block_level(&unit, BYTEOFF(at), 0U, &span) ||
            !now_ns(&end) || end < start || span.lo != 2U ||
            span.hi != len)
            goto provider_done;
        line_calls = yew_syn_engine_line_calls(fx->engine);
        if (line_calls > PERF_SYN_BLOCK_MAX_LINE_CALLS)
            goto provider_done;
        if (line_calls > *max_line_calls)
            *max_line_calls = line_calls;
        samples[i] = end - start;
        perf_syn_sink += span.lo + span.hi + line_calls;
    }
    ok = true;

provider_done:
    yew_block_provider_syntax_install(false);
done:
    yew_syn_detach(&buf.syn);
    yew_textbuf_free(buf.tb);
    return ok;
}

static bool load_baselines(PerfCase *cases, size_t count)
{
    FILE *file = fopen("tests/perf/baselines/syn.txt", "r");
    char line[256];

    if (file == NULL) {
        (void)fprintf(stderr, "perf_syn: cannot read baseline: %s\n",
                      strerror(errno));
        return false;
    }
    while (fgets(line, sizeof(line), file) != NULL) {
        char name[64];
        unsigned long long median;
        unsigned long long p99;

        if (sscanf(line, "%63s %llu %llu", name, &median, &p99) != 3 ||
            median == 0U || p99 == 0U)
            continue;
        for (size_t i = 0U; i < count; i++) {
            if (strcmp(cases[i].name, name) == 0) {
                cases[i].baseline.median = (u64)median;
                cases[i].baseline.p99 = (u64)p99;
            }
        }
    }
    if (ferror(file) || fclose(file) != 0)
        return false;
    for (size_t i = 0U; i < count; i++) {
        if (cases[i].baseline.median == 0U || cases[i].baseline.p99 == 0U) {
            (void)fprintf(stderr, "perf_syn: missing baseline for %s\n",
                          cases[i].name);
            return false;
        }
    }
    return true;
}

static const char *frozen_case_stem(size_t fixture)
{
    return fixture == 1U ? "comment" : frozen_specs[fixture].stem;
}

static bool init_cases(
    PerfCase cases[PERF_SYN_CASE_COUNT],
    char names[PERF_SYN_FIXTURE_COUNT * 4U][PERF_SYN_CASE_NAME_CAP])
{
    (void)memset(cases, 0, sizeof(*cases) * PERF_SYN_CASE_COUNT);
    cases[CASE_TOY_LINE].name = "line";
    cases[CASE_TOY_VIEW].name = "viewport_cold_200";
    cases[CASE_TOY_EDIT].name = "edit_settle_100k";
    cases[CASE_LINE_CAP].name = "line_cap_512k";
    for (size_t i = 0U; i < PERF_SYN_FIXTURE_COUNT; i++) {
        const char *stem = frozen_case_stem(i);
        int line_n;
        int edit_n;
        int view_200_n;
        int view_24_n;

        line_n = snprintf(names[i], PERF_SYN_CASE_NAME_CAP,
                          i == 0U ? "c_kitchen_line" : "%s_line", stem);
        edit_n = snprintf(names[PERF_SYN_FIXTURE_COUNT + i],
                          PERF_SYN_CASE_NAME_CAP,
                          i == 0U ? "c_kitchen_edit" : "%s_edit", stem);
        view_200_n = snprintf(names[PERF_SYN_FIXTURE_COUNT * 2U + i],
                              PERF_SYN_CASE_NAME_CAP,
                              "viewport_200x100_%s", stem);
        view_24_n = snprintf(names[PERF_SYN_FIXTURE_COUNT * 3U + i],
                             PERF_SYN_CASE_NAME_CAP,
                             "viewport_80x24_%s", stem);
        if (line_n < 0 || line_n >= PERF_SYN_CASE_NAME_CAP || edit_n < 0 ||
            edit_n >= PERF_SYN_CASE_NAME_CAP || view_200_n < 0 ||
            view_200_n >= PERF_SYN_CASE_NAME_CAP || view_24_n < 0 ||
            view_24_n >= PERF_SYN_CASE_NAME_CAP)
            return false;
        cases[CASE_FROZEN_LINE_FIRST + i].name = names[i];
        cases[CASE_FROZEN_EDIT_FIRST + i].name =
            names[PERF_SYN_FIXTURE_COUNT + i];
        cases[CASE_VIEW_200_FIRST + i].name =
            names[PERF_SYN_FIXTURE_COUNT * 2U + i];
        cases[CASE_VIEW_24_FIRST + i].name =
            names[PERF_SYN_FIXTURE_COUNT * 3U + i];
    }
    cases[CASE_THEME_SWITCH].name = "theme_switch_200x50";
    cases[CASE_MINIFIED_FIRST_PAINT].name = "minified_first_paint";
    return true;
}

int main(int argc, char **argv)
{
    PerfCase cases[PERF_SYN_CASE_COUNT];
    char case_names[PERF_SYN_FIXTURE_COUNT * 4U][PERF_SYN_CASE_NAME_CAP];
    Timing case_trials[YEW_ARRAY_LEN(cases)][PERF_SYN_TRIALS];
    Timing detect_trials[PERF_SYN_TRIALS];
    Timing compile_trials[PERF_SYN_TRIALS];
    Timing cache_trials[PERF_SYN_TRIALS];
    Timing block_trials[PERF_SYN_TRIALS];
    Timing block_multiline_trials[PERF_SYN_TRIALS];
    size_t count = sample_count();
    size_t start_count = count < PERF_SYN_START_SAMPLES ?
                         count : PERF_SYN_START_SAMPLES;
    u64 *samples = calloc(count, sizeof(*samples));
    u64 *start_samples = calloc(start_count, sizeof(*start_samples));
    u8 *cap_line = malloc(512U * 1024U);
    TextBuf *viewport = line_fixture(PERF_SYN_VIEW_LINES - 1U);
    TextBuf *edit = line_fixture(PERF_SYN_EDIT_LINES - 1U);
    SynFixture fx;
    FrozenFixture frozen[PERF_SYN_FIXTURE_COUNT];
    size_t frozen_initialized = 0U;
    Source ini = {NULL, 0U};
    Timing detect = {0U, 0U};
    Timing compile = {0U, 0U};
    Timing cache = {0U, 0U};
    Timing warm_start = {0U, 0U};
    Timing block = {0U, 0U};
    Timing block_multiline = {0U, 0U};
    u64 block_line_calls = 0U;
    u64 block_multiline_calls = 0U;
    u64 theme_line_calls = 0U;
    u64 comment_first_max_us = 0U;
    u64 comment_first_frames = 0U;
    u64 comment_idle_total_us = 0U;
    u64 comment_idle_max_us = 0U;
    u64 comment_idle_frames = 0U;
    u64 comment_state_logical_bytes = 0U;
    u64 comment_state_capacity_bytes = 0U;
    u64 comment_state_rss_growth = 0U;
    u64 comment_wall_ns = 0U;
    u64 whole_total_ns = 0U;
    u64 whole_max_frame_ns = 0U;
    u64 whole_frames = 0U;
    double scroll_fps[PERF_SYN_FIXTURE_COUNT];
    double markdown_wrap_fps = 0.0;
    u64 all_state_capacity_bytes = 0U;
    u64 all_state_limit_bytes = 0U;
    u64 warm_start_compiled_max = 0U;
    bool regression_seen = false;
    int status = 0;

    if (argc == 2 && strcmp(argv[1], "--prime-all-syntax") == 0)
        return prime_all_syntax();
    if (argc == 2 && strcmp(argv[1], "--warm-start-probe") == 0)
        return warm_start_probe();
    if (argc != 1) {
        (void)fprintf(stderr, "perf_syn: unknown argument\n");
        return 2;
    }
    if (!init_cases(cases, case_names)) {
        (void)fprintf(stderr, "perf_syn: case name initialization failed\n");
        return 2;
    }
    (void)memset(case_trials, 0, sizeof(case_trials));
    (void)memset(detect_trials, 0, sizeof(detect_trials));
    (void)memset(compile_trials, 0, sizeof(compile_trials));
    (void)memset(cache_trials, 0, sizeof(cache_trials));
    (void)memset(block_trials, 0, sizeof(block_trials));
    (void)memset(block_multiline_trials, 0,
                 sizeof(block_multiline_trials));
    (void)memset(frozen, 0, sizeof(frozen));
    (void)memset(scroll_fps, 0, sizeof(scroll_fps));
    for (size_t i = 0U; i < PERF_SYN_FIXTURE_COUNT; i++) {
        if (!frozen_init(&frozen[i], &frozen_specs[i])) {
            (void)fprintf(stderr, "perf_syn: fixture '%s' failed\n",
                          frozen_specs[i].stem);
            break;
        }
        frozen_initialized++;
    }
    if (samples == NULL || start_samples == NULL || cap_line == NULL ||
        viewport == NULL ||
        edit == NULL || frozen_initialized != PERF_SYN_FIXTURE_COUNT ||
        !read_source("runtime/syntax/ini.fl", &ini) ||
        !fixture_init(&fx)) {
        (void)fprintf(stderr, "perf_syn: fixture allocation failed\n");
        free(samples);
        free(start_samples);
        free(cap_line);
        free(ini.data);
        yew_textbuf_free(viewport);
        yew_textbuf_free(edit);
        for (size_t i = 0U; i < frozen_initialized; i++)
            frozen_free(&frozen[i]);
        return 2;
    }
    (void)memset(cap_line, 'x', 512U * 1024U);
    for (size_t trial = 0U; trial < PERF_SYN_TRIALS && status == 0;
         trial++) {
        u64 trial_line_calls = 0U;
        u64 trial_multiline_calls = 0U;

        if (!measure_line(&fx, samples, count)) {
            (void)fprintf(stderr, "perf_syn: line measurement failed\n");
            status = 2;
        } else
            case_trials[0][trial] = timing_of(samples, count);
        if (status == 0 &&
            !measure_viewport(&fx, viewport, samples, count)) {
            (void)fprintf(stderr,
                          "perf_syn: viewport measurement failed\n");
            status = 2;
        } else if (status == 0)
            case_trials[1][trial] = timing_of(samples, count);
        if (status == 0 && !measure_edit(&fx, edit, samples, count)) {
            (void)fprintf(stderr, "perf_syn: edit measurement failed\n");
            status = 2;
        } else if (status == 0)
            case_trials[2][trial] = timing_of(samples, count);
        if (status == 0 && !measure_cap(&fx, cap_line, samples, count)) {
            (void)fprintf(stderr,
                          "perf_syn: line-cap measurement failed\n");
            status = 2;
        } else if (status == 0)
            case_trials[CASE_LINE_CAP][trial] = timing_of(samples, count);
        for (size_t i = 0U; i < PERF_SYN_FIXTURE_COUNT && status == 0; i++) {
            if (!measure_frozen_line(&frozen[i], samples, count)) {
                (void)fprintf(stderr,
                              "perf_syn: line %s measurement failed\n",
                              frozen[i].spec->stem);
                status = 2;
            } else {
                case_trials[CASE_FROZEN_LINE_FIRST + i][trial] =
                    timing_of(samples, count);
            }
        }
        for (size_t i = 0U; i < PERF_SYN_FIXTURE_COUNT && status == 0; i++) {
            if (!measure_frozen_edit(&frozen[i], samples, count)) {
                (void)fprintf(stderr,
                              "perf_syn: edit %s measurement failed\n",
                              frozen[i].spec->stem);
                status = 2;
            } else {
                case_trials[CASE_FROZEN_EDIT_FIRST + i][trial] =
                    timing_of(samples, count);
            }
        }
        for (size_t i = 0U; i < PERF_SYN_FIXTURE_COUNT && status == 0; i++) {
            if (!measure_frozen_viewport(&frozen[i], 200U, samples,
                                         count)) {
                (void)fprintf(stderr,
                              "perf_syn: 200-row %s measurement failed\n",
                              frozen[i].spec->stem);
                status = 2;
            } else {
                case_trials[CASE_VIEW_200_FIRST + i][trial] =
                    timing_of(samples, count);
            }
        }
        for (size_t i = 0U; i < PERF_SYN_FIXTURE_COUNT && status == 0; i++) {
            if (!measure_frozen_viewport(&frozen[i], 24U, samples,
                                         count)) {
                (void)fprintf(stderr,
                              "perf_syn: 24-row %s measurement failed\n",
                              frozen[i].spec->stem);
                status = 2;
            } else {
                case_trials[CASE_VIEW_24_FIRST + i][trial] =
                    timing_of(samples, count);
            }
        }
        if (status == 0 &&
            !measure_theme_switch(&frozen[0], samples, count,
                                  &trial_line_calls)) {
            (void)fprintf(stderr, "perf_syn: theme-switch measurement failed\n");
            status = 2;
        } else if (status == 0) {
            case_trials[CASE_THEME_SWITCH][trial] = timing_of(samples, count);
            if (trial_line_calls > theme_line_calls)
                theme_line_calls = trial_line_calls;
        }
        if (status == 0 &&
            !measure_minified_first_paint(&frozen[PERF_SYN_JSON_INDEX],
                                          samples, count)) {
            (void)fprintf(stderr,
                          "perf_syn: minified first-paint measurement failed\n");
            status = 2;
        } else if (status == 0) {
            case_trials[CASE_MINIFIED_FIRST_PAINT][trial] =
                timing_of(samples, count);
        }
        if (status == 0 && !measure_detect(samples, count)) {
            (void)fprintf(stderr,
                          "perf_syn: detection measurement failed\n");
            status = 2;
        } else if (status == 0)
            detect_trials[trial] = timing_of(samples, count);
        if (status == 0 && !measure_compile(&ini, samples, count)) {
            (void)fprintf(stderr,
                          "perf_syn: definition compile failed\n");
            status = 2;
        } else if (status == 0)
            compile_trials[trial] = timing_of(samples, count);
        if (status == 0 && !measure_cache(samples, count)) {
            (void)fprintf(stderr,
                          "perf_syn: cache-load measurement failed\n");
            status = 2;
        } else if (status == 0)
            cache_trials[trial] = timing_of(samples, count);
        if (status == 0 &&
            !measure_block_provider(&fx, samples, count,
                                    &trial_line_calls)) {
            (void)fprintf(stderr,
                          "perf_syn: block-provider measurement failed\n");
            status = 2;
        } else if (status == 0) {
            block_trials[trial] = timing_of(samples, count);
            if (trial_line_calls > block_line_calls)
                block_line_calls = trial_line_calls;
        }
        if (status == 0 &&
            !measure_block_multiline(&fx, samples, count,
                                     &trial_multiline_calls)) {
            (void)fprintf(
                stderr,
                "perf_syn: multiline block-provider measurement failed\n");
            status = 2;
        } else if (status == 0) {
            block_multiline_trials[trial] = timing_of(samples, count);
            if (trial_multiline_calls > block_multiline_calls)
                block_multiline_calls = trial_multiline_calls;
        }
    }
    if (status == 0) {
        for (size_t i = 0U; i < YEW_ARRAY_LEN(cases); i++)
            cases[i].measured = timing_of_trials(case_trials[i]);
        detect = timing_of_trials(detect_trials);
        compile = timing_of_trials(compile_trials);
        cache = timing_of_trials(cache_trials);
        block = timing_of_trials(block_trials);
        block_multiline = timing_of_trials(block_multiline_trials);
    }
    if (status == 0 &&
        !measure_warm_start(argv[0], start_samples, start_count,
                            &warm_start_compiled_max)) {
        (void)fprintf(stderr, "perf_syn: warm-start measurement failed\n");
        status = 2;
    } else if (status == 0) {
        warm_start = timing_of(start_samples, start_count);
    }

    if (status == 0 &&
        !check_comment_bomb(&frozen[1], &comment_first_max_us,
                            &comment_first_frames,
                            &comment_idle_total_us,
                            &comment_idle_max_us, &comment_idle_frames,
                            &comment_state_logical_bytes,
                            &comment_state_capacity_bytes,
                            &comment_state_rss_growth,
                            &comment_wall_ns)) {
        (void)fprintf(stderr, "perf_syn: comment-bomb frame check failed\n");
        status = 2;
    }
    if (status == 0 &&
        !measure_whole_settle(&frozen[PERF_SYN_JSON_INDEX], &whole_total_ns,
                              &whole_max_frame_ns, &whole_frames)) {
        (void)fprintf(stderr, "perf_syn: whole-file settle failed\n");
        status = 2;
    }
    for (size_t i = 0U; i < PERF_SYN_FIXTURE_COUNT && status == 0; i++) {
        if (!measure_scroll(&frozen[i], false, &scroll_fps[i])) {
            (void)fprintf(stderr, "perf_syn: %s scroll measurement failed\n",
                          frozen[i].spec->stem);
            status = 2;
        }
    }
    if (status == 0 &&
        !measure_scroll(&frozen[6], true, &markdown_wrap_fps)) {
        (void)fprintf(stderr, "perf_syn: markdown wrap measurement failed\n");
        status = 2;
    }
    if (status == 0 &&
        !check_all_state_memory(frozen, &all_state_capacity_bytes,
                                &all_state_limit_bytes)) {
        (void)fprintf(stderr, "perf_syn: state memory measurement failed\n");
        status = 2;
    }

    if (status == 0 && !load_baselines(cases, YEW_ARRAY_LEN(cases)))
        status = 2;
    for (size_t i = 0U; status == 0 && i < YEW_ARRAY_LEN(cases); i++) {
        u64 median_limit = cases[i].baseline.median +
                           cases[i].baseline.median / 5U;
        u64 p99_limit = cases[i].baseline.p99 + cases[i].baseline.p99 / 5U;
        bool regression = cases[i].measured.median > median_limit ||
                          cases[i].measured.p99 > p99_limit;

        if (i >= CASE_VIEW_200_FIRST && i <= CASE_VIEW_200_LAST)
            regression = regression ||
                         cases[i].measured.p99 > PERF_SYN_VIEW_200_LIMIT_NS;
        if (i >= CASE_VIEW_24_FIRST && i <= CASE_VIEW_24_LAST)
            regression = regression ||
                         cases[i].measured.p99 > PERF_SYN_VIEW_24_LIMIT_NS;
        if (i >= CASE_FROZEN_LINE_FIRST && i <= CASE_FROZEN_LINE_LAST)
            regression = regression || cases[i].measured.median > 3000U ||
                         cases[i].measured.p99 > 12000U;
        if (i >= CASE_FROZEN_EDIT_FIRST && i <= CASE_FROZEN_EDIT_LAST)
            regression = regression || cases[i].measured.p99 > 60000U;
        if (i == CASE_THEME_SWITCH)
            regression = regression ||
                         cases[i].measured.p99 > PERF_SYN_THEME_LIMIT_NS ||
                         theme_line_calls != 0U;
        if (i == CASE_MINIFIED_FIRST_PAINT)
            regression = regression ||
                         cases[i].measured.p99 > PERF_SYN_MINIFIED_LIMIT_NS;

        (void)printf("syn.%-20s median_ns=%llu p99_ns=%llu%s\n",
                     cases[i].name,
                     (unsigned long long)cases[i].measured.median,
                     (unsigned long long)cases[i].measured.p99,
                     regression ? " REGRESSION" : " ok");
        if (regression)
            regression_seen = true;
    }
    if (status != 2) {
        bool detect_regression = detect.median > PERF_SYN_DETECT_LIMIT_NS;
        bool compile_regression = compile.median > PERF_SYN_COMPILE_LIMIT_NS;
        bool cache_regression = cache.median > PERF_SYN_CACHE_LIMIT_NS;
        bool warm_start_regression =
            warm_start.p99 > PERF_SYN_WARM_START_LIMIT_NS ||
            warm_start_compiled_max > 1U;
        bool block_regression = block.p99 > PERF_SYN_BLOCK_LIMIT_NS;
        bool multiline_regression =
            block_multiline.p99 > PERF_SYN_BLOCK_LIMIT_NS;

        (void)printf("syn.%-20s median_ns=%llu p99_ns=%llu%s\n",
                     "detect_10000",
                     (unsigned long long)detect.median,
                     (unsigned long long)detect.p99,
                     detect_regression ? " REGRESSION" : " ok");
        (void)printf("syn.%-20s median_ns=%llu p99_ns=%llu%s\n",
                     "ini_compile_cold",
                     (unsigned long long)compile.median,
                     (unsigned long long)compile.p99,
                     compile_regression ? " REGRESSION" : " ok");
        (void)printf("syn.%-20s median_ns=%llu p99_ns=%llu%s\n",
                     "ini_cache_warm",
                     (unsigned long long)cache.median,
                     (unsigned long long)cache.p99,
                     cache_regression ? " REGRESSION" : " ok");
        (void)printf("syn.%-20s median_ns=%llu p99_ns=%llu "
                     "compiled_max=%llu%s\n", "all_defs_warm_start",
                     (unsigned long long)warm_start.median,
                     (unsigned long long)warm_start.p99,
                     (unsigned long long)warm_start_compiled_max,
                     warm_start_regression ? " REGRESSION" : " ok");
        (void)printf("syn.%-20s median_ns=%llu p99_ns=%llu "
                     "line_calls_max=%llu%s\n",
                     "block_provider_64k",
                     (unsigned long long)block.median,
                     (unsigned long long)block.p99,
                     (unsigned long long)block_line_calls,
                     block_regression ? " REGRESSION" : " ok");
        (void)printf("syn.%-20s median_ns=%llu p99_ns=%llu "
                     "line_calls_max=%llu%s\n",
                     "block_multiline_100k",
                     (unsigned long long)block_multiline.median,
                     (unsigned long long)block_multiline.p99,
                     (unsigned long long)block_multiline_calls,
                     multiline_regression ? " REGRESSION" : " ok");
        {
            bool comment_view_regression =
                comment_first_frames != 1U ||
                comment_first_max_us > YEW_SYN_FRAME_BUDGET_US;
            bool comment_idle_regression =
                comment_idle_frames == 0U ||
                comment_idle_max_us > YEW_SYN_IDLE_BUDGET_US ||
                comment_idle_total_us > PERF_SYN_COMMENT_TOTAL_US ||
                comment_wall_ns > UINT64_C(400000000);
            bool scroll_regression =
                markdown_wrap_fps < PERF_SYN_SCROLL_MIN_FPS;
            bool whole_regression = whole_total_ns > UINT64_C(45000000) ||
                                    whole_max_frame_ns > UINT64_C(1000000);
            bool state_regression =
                comment_state_capacity_bytes > PERF_SYN_STATE_LIMIT_BYTES ||
                comment_state_rss_growth > PERF_SYN_STATE_LIMIT_BYTES;
            bool all_state_regression =
                all_state_capacity_bytes > all_state_limit_bytes;

            for (size_t i = 0U; i < PERF_SYN_FIXTURE_COUNT; i++) {
                bool fixture_scroll_regression =
                    scroll_fps[i] < PERF_SYN_SCROLL_MIN_FPS;

                (void)printf("syn.scroll_%-13s fps=%.2f%s\n",
                             frozen[i].spec->stem, scroll_fps[i],
                             fixture_scroll_regression ? " REGRESSION" :
                                                         " ok");
                if (fixture_scroll_regression)
                    scroll_regression = true;
            }

            (void)printf("syn.%-20s frames=%llu fake_max_frame_us=%llu%s\n",
                         "comment_view",
                         (unsigned long long)comment_first_frames,
                         (unsigned long long)comment_first_max_us,
                         comment_view_regression ? " REGRESSION" : " ok");
            (void)printf("syn.%-20s frames=%llu fake_total_us=%llu "
                         "fake_max_frame_us=%llu wall_ns=%llu%s\n",
                         "comment_idle",
                         (unsigned long long)comment_idle_frames,
                         (unsigned long long)comment_idle_total_us,
                         (unsigned long long)comment_idle_max_us,
                         (unsigned long long)comment_wall_ns,
                         comment_idle_regression ? " REGRESSION" : " ok");
            (void)printf("syn.%-20s line_calls_max=%llu%s\n",
                         "theme_switch_calls",
                         (unsigned long long)theme_line_calls,
                         theme_line_calls == 0U ? " ok" : " REGRESSION");
            (void)printf("syn.%-20s wrap_fps=%.2f%s\n",
                         "markdown_scroll", markdown_wrap_fps,
                         scroll_regression ? " REGRESSION" : " ok");
            (void)printf("syn.%-20s frames=%llu total_ns=%llu "
                         "max_frame_ns=%llu%s\n", "whole_json_settle",
                         (unsigned long long)whole_frames,
                         (unsigned long long)whole_total_ns,
                         (unsigned long long)whole_max_frame_ns,
                         whole_regression ? " REGRESSION" : " ok");
            (void)printf("syn.%-20s logical_bytes=%llu capacity_bytes=%llu "
                         "rss_growth_bytes=%llu%s\n", "comment_state",
                         (unsigned long long)comment_state_logical_bytes,
                         (unsigned long long)comment_state_capacity_bytes,
                         (unsigned long long)comment_state_rss_growth,
                         state_regression ? " REGRESSION" : " ok");
            (void)printf("syn.%-20s capacity_bytes=%llu limit_bytes=%llu%s\n",
                         "all_fixture_state",
                         (unsigned long long)all_state_capacity_bytes,
                         (unsigned long long)all_state_limit_bytes,
                         all_state_regression ? " REGRESSION" : " ok");
            if (comment_view_regression || comment_idle_regression ||
                scroll_regression || whole_regression || state_regression ||
                all_state_regression)
                regression_seen = true;
        }
        if (detect_regression || compile_regression || cache_regression ||
            warm_start_regression || block_regression ||
            multiline_regression)
            regression_seen = true;
        if (regression_seen)
            status = 1;
    }
    if (status == 2)
        (void)fprintf(stderr, "perf_syn: measurement failed\n");
    fixture_free(&fx);
    yew_textbuf_free(viewport);
    yew_textbuf_free(edit);
    for (size_t i = 0U; i < frozen_initialized; i++)
        frozen_free(&frozen[i]);
    free(ini.data);
    free(cap_line);
    free(start_samples);
    free(samples);
    return status;
}
