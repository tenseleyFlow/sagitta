#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "search/regex.h"
#include "syn/defs.h"
#include "syn/engine.h"
#include "text/piece.h"
#include "util/arena.h"
#include "util/intern.h"

enum {
    PERF_SYN_DEFAULT_SAMPLES = 101,
    PERF_SYN_MAX_SAMPLES = 1001,
    PERF_SYN_VIEW_LINES = 200,
    PERF_SYN_EDIT_LINES = 100000,
    PERF_SYN_RULES = 6,
    PERF_SYN_CTXS = 3,
    PERF_SYN_DETECT_PATHS = 10000
};

#define PERF_SYN_DETECT_LIMIT_NS UINT64_C(5000000)
#define PERF_SYN_COMPILE_LIMIT_NS UINT64_C(3000000)
#define PERF_SYN_CACHE_LIMIT_NS UINT64_C(200000)

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

static bool now_ns(u64 *out)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return false;
    *out = (u64)ts.tv_sec * UINT64_C(1000000000) + (u64)ts.tv_nsec;
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

int main(void)
{
    PerfCase cases[] = {
        {"line", {0U, 0U}, {0U, 0U}},
        {"viewport_cold_200", {0U, 0U}, {0U, 0U}},
        {"edit_settle_100k", {0U, 0U}, {0U, 0U}},
        {"line_cap_512k", {0U, 0U}, {0U, 0U}}
    };
    size_t count = sample_count();
    u64 *samples = calloc(count, sizeof(*samples));
    u8 *cap_line = malloc(512U * 1024U);
    TextBuf *viewport = line_fixture(PERF_SYN_VIEW_LINES - 1U);
    TextBuf *edit = line_fixture(PERF_SYN_EDIT_LINES - 1U);
    SynFixture fx;
    Source ini = {NULL, 0U};
    Timing detect = {0U, 0U};
    Timing compile = {0U, 0U};
    Timing cache = {0U, 0U};
    int status = 0;

    if (samples == NULL || cap_line == NULL || viewport == NULL ||
        edit == NULL || !read_source("runtime/syntax/ini.fl", &ini) ||
        !fixture_init(&fx)) {
        (void)fprintf(stderr, "perf_syn: fixture allocation failed\n");
        free(samples);
        free(cap_line);
        free(ini.data);
        yew_textbuf_free(viewport);
        yew_textbuf_free(edit);
        return 2;
    }
    (void)memset(cap_line, 'x', 512U * 1024U);
    if (!measure_line(&fx, samples, count)) {
        (void)fprintf(stderr, "perf_syn: line measurement failed\n");
        status = 2;
    } else
        cases[0].measured = timing_of(samples, count);
    if (status == 0 && !measure_viewport(&fx, viewport, samples, count)) {
        (void)fprintf(stderr, "perf_syn: viewport measurement failed\n");
        status = 2;
    } else if (status == 0)
        cases[1].measured = timing_of(samples, count);
    if (status == 0 && !measure_edit(&fx, edit, samples, count)) {
        (void)fprintf(stderr, "perf_syn: edit measurement failed\n");
        status = 2;
    } else if (status == 0)
        cases[2].measured = timing_of(samples, count);
    if (status == 0 && !measure_cap(&fx, cap_line, samples, count)) {
        (void)fprintf(stderr, "perf_syn: line-cap measurement failed\n");
        status = 2;
    } else if (status == 0)
        cases[3].measured = timing_of(samples, count);
    if (status == 0 && !measure_detect(samples, count)) {
        (void)fprintf(stderr, "perf_syn: detection measurement failed\n");
        status = 2;
    } else if (status == 0)
        detect = timing_of(samples, count);
    if (status == 0 && !measure_compile(&ini, samples, count)) {
        (void)fprintf(stderr, "perf_syn: definition compile failed\n");
        status = 2;
    } else if (status == 0)
        compile = timing_of(samples, count);
    if (status == 0 && !measure_cache(samples, count)) {
        (void)fprintf(stderr, "perf_syn: cache-load measurement failed\n");
        status = 2;
    } else if (status == 0)
        cache = timing_of(samples, count);

    if (status == 0 && !load_baselines(cases, YEW_ARRAY_LEN(cases)))
        status = 2;
    for (size_t i = 0U; status == 0 && i < YEW_ARRAY_LEN(cases); i++) {
        u64 median_limit = cases[i].baseline.median +
                           cases[i].baseline.median / 5U;
        u64 p99_limit = cases[i].baseline.p99 + cases[i].baseline.p99 / 5U;
        bool regression = cases[i].measured.median > median_limit ||
                          cases[i].measured.p99 > p99_limit;

        (void)printf("syn.%-20s median_ns=%llu p99_ns=%llu%s\n",
                     cases[i].name,
                     (unsigned long long)cases[i].measured.median,
                     (unsigned long long)cases[i].measured.p99,
                     regression ? " REGRESSION" : " ok");
        if (regression)
            status = 1;
    }
    if (status != 2) {
        bool detect_regression = detect.median > PERF_SYN_DETECT_LIMIT_NS;
        bool compile_regression = compile.median > PERF_SYN_COMPILE_LIMIT_NS;
        bool cache_regression = cache.median > PERF_SYN_CACHE_LIMIT_NS;

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
        if (detect_regression || compile_regression || cache_regression)
            status = 1;
    }
    if (status == 2)
        (void)fprintf(stderr, "perf_syn: measurement failed\n");
    fixture_free(&fx);
    yew_textbuf_free(viewport);
    yew_textbuf_free(edit);
    free(ini.data);
    free(cap_line);
    free(samples);
    return status;
}
