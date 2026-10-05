#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
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
#include "util/log.h"
#include "util/prof.h"

#include "perf_policy.h"
#include "perf_runtime.h"

/* The benchmark intentionally exercises degradation paths millions of times.
 * Those warnings are useful in the editor and harmful in a measurement
 * process: synchronous persistent logging perturbs timings and used to grow a
 * developer's normal yew log by hundreds of megabytes.  Keep all benchmark
 * logging process-local: discard the expected degradation warning and preserve
 * unexpected warnings and errors on stderr.  Child probes inherit the
 * installed sink across fork(). */
static void benchmark_log_write(void *user, YewLogLevel level, const char *msg)
{
    static const char expected_warning[] =
        "syntax highlighting degraded for buffer";

    (void)user;
    if (level == YEW_LOG_WARN && strcmp(msg, expected_warning) == 0)
        return;
    if (level >= YEW_LOG_WARN)
        (void)fprintf(stderr, "perf_syn: %s\n", msg);
}

static void isolate_benchmark_logging(void)
{
    static const YewLogSink sink = {benchmark_log_write, NULL};

    yew_log_set_sink(&sink);
}

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
    PERF_SYN_FIXTURE_COUNT = 26,
    PERF_SYN_S42_5_FIRST = 19,
    PERF_SYN_MAKE_INDEX = 5,
    PERF_SYN_MARKDOWN_INDEX = 6,
    PERF_SYN_JSON_INDEX = 14,
    PERF_SYN_MD_EMBED_INDEX = 15,
    PERF_SYN_HTML_EMBED_INDEX = 16,
    PERF_SYN_CASE_NAME_CAP = 64,
    PERF_SYN_THEME_ROWS = 50,
    PERF_SYN_THEME_COLS = 200,
    PERF_SYN_SCROLL_FRAMES = 240,
    PERF_SYN_START_SAMPLES = 101,
    PERF_SYN_TRUNCATED_REPLAYS = 256,
    PERF_SYN_HTML_SCAN_TRIALS = 9,
    PERF_SYN_HTML_SCAN_REPLAYS = 32,
    PERF_SYN_HTML_BODY_LINES = 800,
    PERF_SYN_WARM_CHILD_OK = 10,
    PERF_SYN_SCROLL_PROFILE_CASES = 8,
    PERF_SYN_SCROLL_FRAME_BUDGET_NS = 8333333,
    PERF_SYN_SCROLL_RENDER_LIMIT_PERMILLE = 250,
    PERF_SYN_SCROLL_SYN_LIMIT_PERMILLE = 180
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

#define PERF_SYN_DETECT_HARD_LIMIT_NS UINT64_C(5000000)
#define PERF_SYN_DETECT_P99_LIMIT_NS UINT64_C(1200000)
#define PERF_SYN_COMPILE_LIMIT_NS UINT64_C(3000000)
#define PERF_SYN_CACHE_LIMIT_NS UINT64_C(200000)
#define PERF_SYN_BLOCK_LIMIT_NS UINT64_C(5000000)
#define PERF_SYN_VIEW_200_LIMIT_NS UINT64_C(1500000)
#define PERF_SYN_VIEW_24_LIMIT_NS UINT64_C(300000)
#define PERF_SYN_THEME_LIMIT_NS UINT64_C(2000000)
#define PERF_SYN_MINIFIED_LIMIT_NS UINT64_C(20000000)
#define PERF_SYN_WARM_START_LIMIT_NS UINT64_C(20000000)
#define PERF_SYN_LIST_MEDIAN_LIMIT_NS UINT64_C(2000000)
#define PERF_SYN_LIST_P99_LIMIT_NS UINT64_C(5000000)
#define PERF_SYN_COMPILE_ALL_LIMIT_NS UINT64_C(100000000)
#define PERF_SYN_WARM_ALL_LIMIT_NS UINT64_C(15000000)
#define PERF_SYN_COMMENT_TOTAL_US UINT64_C(400000)
#define PERF_SYN_STATE_LIMIT_BYTES UINT64_C(204800)
#define PERF_SYN_RUNTIME_LIMIT_BYTES UINT64_C(1572864)
#define PERF_SYN_ENTRY_LIMIT_BYTES UINT64_C(4)
#define PERF_SYN_HTML_RATIO_BASE UINT64_C(10000)
#define PERF_SYN_HTML_RATIO_LIMIT UINT64_C(10800)
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

typedef struct HtmlScanRow {
    size_t lo;
    u32 len;
    u32 embedded_entry;
    u32 embedded_exit;
    u32 direct_entry;
    u32 direct_exit;
    u32 embedded_spans;
    u32 direct_spans;
    u8 guest;
} HtmlScanRow;

typedef struct HtmlScanTrial {
    u64 embedded_ns;
    u64 plain_ns;
    u64 ratio_bp;
} HtmlScanTrial;

typedef enum PerfSynGateMode {
    PERF_SYN_GATE_BUDGETS,
    PERF_SYN_GATE_FULL
} PerfSynGateMode;

static volatile u64 perf_syn_sink;

/*
 * Every printed row carries one SynCheck, and the advisory policy of
 * tests/perf/perf_policy.h reaches only its timing and throughput parts.  A
 * deterministic breach (a call, load, frame or tick count, a byte or state
 * size, a verdict on the injected settle clock) fails in every mode; a timing
 * breach fails in strict mode, and in advisory mode only beyond the shared
 * sanity bound.
 */
typedef struct SynCheck {
    bool correctness;
    bool timing;
    bool insane;
} SynCheck;

static void syn_check_correct(SynCheck *check, bool breach)
{
    if (breach)
        check->correctness = true;
}

/* A whole operation (a detection batch, a child process, a summed settle):
 * zero is a broken clock, never a pass. */
static void syn_check_timing(SynCheck *check, u64 value, u64 budget)
{
    if (value > budget)
        check->timing = true;
    if (!yew_perf_timing_sane(value, budget))
        check->insane = true;
}

/* A quantile of per-call samples, or an amortized difference: a coarse clock
 * may legitimately read zero, so only the advisory ceiling applies. */
static void syn_check_sample(SynCheck *check, u64 value, u64 budget)
{
    if (value > budget)
        check->timing = true;
    if (yew_perf_sample_failed(value, budget, true))
        check->insane = true;
}

static void syn_check_throughput(SynCheck *check, double value,
                                 double minimum)
{
    if (!(value >= minimum))
        check->timing = true;
    if (!yew_perf_throughput_sane(value, minimum))
        check->insane = true;
}

static bool syn_check_failed(const SynCheck *check, bool advisory)
{
    return check->correctness || check->insane ||
           (check->timing && !advisory);
}

static const char *syn_check_verdict(const SynCheck *check, bool advisory)
{
    if (check->correctness)
        return " REGRESSION";
    if (check->insane)
        return " SANITY-FAIL";
    if (check->timing)
        return advisory ? " WARN" : " REGRESSION";
    return " ok";
}

static bool gate_uses_baseline(PerfSynGateMode mode)
{
    return mode == PERF_SYN_GATE_FULL;
}

/* The per-case decision.  Baseline-relative limits exist only in full mode
 * and, like the absolute budgets, are timing; theme_line_calls is a
 * deterministic count. */
static SynCheck case_check(size_t i, const PerfCase *c, PerfSynGateMode mode,
                           u64 theme_line_calls)
{
    SynCheck check = {false, false, false};

    if (gate_uses_baseline(mode)) {
        syn_check_sample(&check, c->measured.median,
                         c->baseline.median + c->baseline.median / 5U);
        syn_check_sample(&check, c->measured.p99,
                         c->baseline.p99 + c->baseline.p99 / 5U);
    }
    if (i >= CASE_VIEW_200_FIRST && i <= CASE_VIEW_200_LAST)
        syn_check_sample(&check, c->measured.p99,
                         PERF_SYN_VIEW_200_LIMIT_NS);
    if (i >= CASE_VIEW_24_FIRST && i <= CASE_VIEW_24_LAST)
        syn_check_sample(&check, c->measured.p99, PERF_SYN_VIEW_24_LIMIT_NS);
    if (i >= CASE_FROZEN_LINE_FIRST && i <= CASE_FROZEN_LINE_LAST) {
        bool markdown_embed =
            i == CASE_FROZEN_LINE_FIRST + PERF_SYN_MD_EMBED_INDEX;

        syn_check_sample(&check, c->measured.median,
                         markdown_embed ? 3500U : 3000U);
        syn_check_sample(&check, c->measured.p99,
                         markdown_embed ? 14000U : 12000U);
    }
    if (i >= CASE_FROZEN_EDIT_FIRST && i <= CASE_FROZEN_EDIT_LAST)
        syn_check_sample(&check, c->measured.p99, 60000U);
    if (i == CASE_THEME_SWITCH) {
        syn_check_sample(&check, c->measured.p99, PERF_SYN_THEME_LIMIT_NS);
        syn_check_correct(&check, theme_line_calls != 0U);
    }
    if (i == CASE_MINIFIED_FIRST_PAINT)
        syn_check_sample(&check, c->measured.p99,
                         PERF_SYN_MINIFIED_LIMIT_NS);
    return check;
}

static int selftest_gate(void)
{
    /* A 24-row viewport case: absolute p99 budget 300000 ns; the baseline
     * {1000, 1000} puts the relative limits at 1200 ns. */
    static const struct {
        PerfSynGateMode mode;
        Timing measured;
        Timing baseline;
        bool want;
        const char *what;
    } cases[] = {
        {PERF_SYN_GATE_BUDGETS, {100000U, 100000U}, {100000U, 1000000U},
         false, "budgets clean"},
        {PERF_SYN_GATE_BUDGETS, {100000U, 300001U}, {100000U, 1000000U},
         true, "budget exceeded"},
        {PERF_SYN_GATE_BUDGETS, {100000U, 100000U}, {1000U, 1000U},
         false, "relative ignored"},
        {PERF_SYN_GATE_BUDGETS, {100000U, 300001U}, {1000U, 1000U},
         true, "both in budgets mode"},
        {PERF_SYN_GATE_FULL, {100000U, 100000U}, {100000U, 1000000U},
         false, "full clean"},
        {PERF_SYN_GATE_FULL, {100000U, 300001U}, {100000U, 1000000U},
         true, "full budget exceeded"},
        {PERF_SYN_GATE_FULL, {100000U, 100000U}, {1000U, 1000U},
         true, "full relative exceeded"},
        {PERF_SYN_GATE_FULL, {100000U, 300001U}, {1000U, 1000U},
         true, "both in full mode"}
    };
    size_t failures = 0U;

    for (size_t i = 0U; i < YEW_ARRAY_LEN(cases); i++) {
        PerfCase c = {"selftest", cases[i].measured, cases[i].baseline};
        SynCheck check = case_check(CASE_VIEW_24_FIRST, &c, cases[i].mode,
                                    0U);
        bool got = syn_check_failed(&check, false);

        if (got != cases[i].want) {
            (void)printf("FAIL gate rule: %s -> %s, wanted %s\n",
                         cases[i].what, got ? "failure" : "pass",
                         cases[i].want ? "failure" : "pass");
            failures++;
        }
    }
    if (gate_uses_baseline(PERF_SYN_GATE_BUDGETS) ||
        !gate_uses_baseline(PERF_SYN_GATE_FULL)) {
        (void)printf("FAIL gate rule: baseline selection\n");
        failures++;
    }
    if (failures != 0U) {
        (void)printf("perf_syn: %lu gate-rule cases wrong\n",
                     (unsigned long)failures);
        return 1;
    }
    (void)printf("perf_syn: gate modes behave (%lu cases)\n",
                 (unsigned long)YEW_ARRAY_LEN(cases));
    return 0;
}

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

typedef struct ScrollProfile {
    const char *name;
    u64 total_ns;
    u64 render_ns;
    u64 syn_ns;
    u64 fps_milli;
    u64 render_permille;
    u64 syn_permille;
    u64 render_work_permille;
    u64 syn_work_permille;
    u32 frames;
} ScrollProfile;

typedef enum ScrollProfilePart {
    SCROLL_PART_THROUGHPUT,
    SCROLL_PART_RENDER_SHARE,
    SCROLL_PART_SYNTAX_SHARE
} ScrollProfilePart;

typedef struct ScrollProfileCase {
    const char *name;
    bool markdown;
    bool wrap;
    u16 rows;
    u16 cols;
} ScrollProfileCase;

static const ScrollProfileCase scroll_profile_cases[] = {
    {"md_nowrap_80x24", true, false, 24U, 80U},
    {"md_wrap_80x24", true, true, 24U, 80U},
    {"md_nowrap_200x60", true, false, 60U, 200U},
    {"md_wrap_200x60", true, true, 60U, 200U},
    {"plain10k_nowrap_80x24", false, false, 24U, 80U},
    {"plain10k_wrap_80x24", false, true, 24U, 80U},
    {"plain10k_nowrap_200x60", false, false, 60U, 200U},
    {"plain10k_wrap_200x60", false, true, 60U, 200U}
};

_Static_assert(YEW_ARRAY_LEN(scroll_profile_cases) ==
               PERF_SYN_SCROLL_PROFILE_CASES,
               "scroll profile case count");

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
     "runtime/syntax/json.fl", 5001U, 2252U * 1024U},
    {"md_embed", "tests/perf/fixtures/syn/md_embed.md",
     "runtime/syntax/markdown.fl", 6000U, 190U * 1024U},
    {"html_embed", "tests/perf/fixtures/syn/html_embed.html",
     "runtime/syntax/html.fl", 4000U, 130U * 1024U},
    {"sh_subst", "tests/perf/fixtures/syn/sh_subst.sh",
     "runtime/syntax/sh.fl", 2000U, 62U * 1024U},
    {"tsx_embed", "tests/perf/fixtures/syn/tsx_embed.tsx",
     "runtime/syntax/typescript.fl", 3000U, 110U * 1024U},
    {"wolf", "tests/perf/fixtures/syn/wolf_kitchen.lu",
     "runtime/syntax/wolf.fl", 5000U, 256U * 1024U},
    {"cpp", "tests/perf/fixtures/syn/native_systems.cpp",
     "runtime/syntax/cpp.fl", 6000U, 320U * 1024U},
    {"kotlin", "tests/perf/fixtures/syn/native_vm.kt",
     "runtime/syntax/kotlin.fl", 5000U, 240U * 1024U},
    {"ruby", "tests/perf/fixtures/syn/native_script.rb",
     "runtime/syntax/ruby.fl", 5000U, 240U * 1024U},
    {"haskell", "tests/perf/fixtures/syn/native_functional.hs",
     "runtime/syntax/haskell.fl", 5000U, 240U * 1024U},
    {"xml", "tests/perf/fixtures/syn/native_data.xml",
     "runtime/syntax/xml.fl", 5000U, 300U * 1024U},
    {"hcl", "tests/perf/fixtures/syn/native_build.hcl",
     "runtime/syntax/hcl.fl", 5000U, 240U * 1024U}
};

static bool now_ns(u64 *out)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return false;
    *out = (u64)ts.tv_sec * UINT64_C(1000000000) + (u64)ts.tv_nsec;
    return true;
}

static bool now_cpu_ns(u64 *out)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts) != 0)
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

static u64 resident_count(const SynEngine *engine)
{
    u64 count = 0U;

    while (count < YEW_SYN_RESIDENT_MAX &&
           yew_syn_engine_def_at(engine, (u8)count) != NULL)
        count++;
    return count;
}

static bool prime_frozen_embeds(FrozenFixture *fixture, u64 *idle_ticks,
                                u64 *loads, u64 *max_pump_ns)
{
    SynBuf syn;
    u64 before;

    *idle_ticks = 0U;
    *loads = 0U;
    *max_pump_ns = 0U;
    yew_syn_buf_init(&syn);
    yew_syn_buf_bind(&syn, fixture->engine);
    yew_syn_attach(&syn, 1U, fixture->tb);
    /* Keystroke-budget settle discovers every pending site without allowing
     * the idle-only loader to run.  Each explicit pump below is one idle
     * tick and loads at most one definition. */
    if (!settle_all(&syn, fixture->tb, LINENO(0U),
                    LINENO(fixture->spec->lines),
                    YEW_SYN_FRAME_BUDGET_US,
                    NULL, NULL, NULL)) {
        yew_syn_detach(&syn);
        return false;
    }
    before = resident_count(fixture->engine);
    while (*idle_ticks < 8U) {
        u64 start;
        u64 end;

        if (!now_ns(&start)) {
            yew_syn_detach(&syn);
            return false;
        }
        if (!yew_syn_embed_pump(&syn, fixture->engine,
                                YEW_SYN_EMBED_LOAD_BUDGET_US)) {
            if (!now_ns(&end) || end < start) {
                yew_syn_detach(&syn);
                return false;
            }
            break;
        }
        if (!now_ns(&end) || end < start) {
            yew_syn_detach(&syn);
            return false;
        }
        if (end - start > *max_pump_ns)
            *max_pump_ns = end - start;
        (*idle_ticks)++;
        if (!settle_all(&syn, fixture->tb, LINENO(0U),
                        LINENO(fixture->spec->lines),
                        YEW_SYN_FRAME_BUDGET_US,
                        NULL, NULL, NULL)) {
            yew_syn_detach(&syn);
            return false;
        }
    }
    *loads = resident_count(fixture->engine) - before;
    yew_syn_detach(&syn);
    return true;
}

typedef struct LegacyLineProbeRow {
    size_t lo;
    u32 len;
    u32 line;
    u32 entry_state;
    u32 exit_state;
    u32 spans;
    u64 cpu_ns;
} LegacyLineProbeRow;

static int probe_legacy_line(const char *stem, bool resident)
{
    enum {
        PROBE_ROWS = 200,
        PROBE_BATCHES = 31,
        PROBE_REPLAYS = 64,
        PROBE_ROW_REPLAYS = 257
    };
    LegacyLineProbeRow rows[PROBE_ROWS];
    u64 batches[PROBE_BATCHES];
    FrozenFixture fixture;
    SynSpan spans[YEW_SYN_MAX_SPANS];
    const FrozenSpec *spec = NULL;
    u64 idle_ticks = 0U;
    u64 loads = 0U;
    u64 max_pump_ns = 0U;
    size_t nrows = 0U;
    size_t lo = 0U;
    u32 state = YEW_SYN_STATE_ROOT;
    int status = 2;

    for (size_t i = 0U; i < PERF_SYN_MD_EMBED_INDEX; i++) {
        if (strcmp(frozen_specs[i].stem, stem) == 0) {
            spec = &frozen_specs[i];
            break;
        }
    }
    if (spec == NULL) {
        (void)fprintf(stderr, "perf_syn: unknown legacy stem '%s'\n", stem);
        return 2;
    }
    yew_syn_discovery_set_bypass(true);
    if (!frozen_init(&fixture, spec) ||
        (resident &&
         !prime_frozen_embeds(&fixture, &idle_ticks, &loads,
                              &max_pump_ns))) {
        (void)fprintf(stderr, "perf_syn: legacy probe setup failed\n");
        if (fixture.spec != NULL)
            frozen_free(&fixture);
        return 2;
    }
    while (nrows < PROBE_ROWS && lo < fixture.source.len) {
        LegacyLineProbeRow *row = &rows[nrows];
        SynLineOut out = {spans, 0U, YEW_ARRAY_LEN(spans), 0U, 0U};
        size_t hi = lo;

        while (hi < fixture.source.len &&
               fixture.source.data[hi] != (u8)'\n')
            hi++;
        if (hi - lo > UINT32_MAX)
            goto done;
        row->lo = lo;
        row->len = (u32)(hi - lo);
        row->line = (u32)nrows + 1U;
        row->entry_state = state;
        yew_syn_line(fixture.engine, state, fixture.source.data + lo,
                     row->len, &out);
        if (out.stop != YEW_SYN_STOP_OK && out.stop != YEW_SYN_STOP_BYTES)
            goto done;
        row->exit_state = out.exit_state;
        row->spans = out.n;
        row->cpu_ns = 0U;
        state = out.exit_state;
        perf_syn_sink += out.n + out.exit_state;
        lo = hi < fixture.source.len ? hi + 1U : hi;
        nrows++;
    }
    if (nrows == 0U)
        goto done;
    for (size_t warmup = 0U; warmup < 3U; warmup++) {
        for (size_t row = 0U; row < nrows; row++) {
            SynLineOut out = {spans, 0U, YEW_ARRAY_LEN(spans), 0U, 0U};

            yew_syn_line(fixture.engine, rows[row].entry_state,
                         fixture.source.data + rows[row].lo, rows[row].len,
                         &out);
            if (out.exit_state != rows[row].exit_state ||
                out.n != rows[row].spans)
                goto done;
            perf_syn_sink += out.n + out.exit_state;
        }
    }
    for (size_t batch = 0U; batch < PROBE_BATCHES; batch++) {
        u64 start;
        u64 end;

        if (!now_cpu_ns(&start))
            goto done;
        for (size_t replay = 0U; replay < PROBE_REPLAYS; replay++) {
            for (size_t row = 0U; row < nrows; row++) {
                SynLineOut out = {spans, 0U, YEW_ARRAY_LEN(spans), 0U, 0U};

                yew_syn_line(fixture.engine, rows[row].entry_state,
                             fixture.source.data + rows[row].lo,
                             rows[row].len, &out);
                if (out.exit_state != rows[row].exit_state ||
                    out.n != rows[row].spans)
                    goto done;
                perf_syn_sink += out.n + out.exit_state;
            }
        }
        if (!now_cpu_ns(&end) || end < start)
            goto done;
        batches[batch] = (end - start) /
            ((u64)PROBE_REPLAYS * (u64)nrows);
    }
    for (size_t row = 0U; row < nrows; row++) {
        u64 start;
        u64 end;

        if (!now_cpu_ns(&start))
            goto done;
        for (size_t replay = 0U; replay < PROBE_ROW_REPLAYS; replay++) {
            SynLineOut out = {spans, 0U, YEW_ARRAY_LEN(spans), 0U, 0U};

            yew_syn_line(fixture.engine, rows[row].entry_state,
                         fixture.source.data + rows[row].lo, rows[row].len,
                         &out);
            if (out.exit_state != rows[row].exit_state ||
                out.n != rows[row].spans)
                goto done;
            perf_syn_sink += out.n + out.exit_state;
        }
        if (!now_cpu_ns(&end) || end < start)
            goto done;
        rows[row].cpu_ns = (end - start) / PROBE_ROW_REPLAYS;
    }
    stable_sort(batches, YEW_ARRAY_LEN(batches));
    (void)printf("probe.legacy_line stem=%s resident=%u ticks=%llu "
                 "loads=%llu max_pump_ns=%llu rows=%lu "
                 "cpu_ns_per_line=%llu\n",
                 stem, resident ? 1U : 0U,
                 (unsigned long long)idle_ticks,
                 (unsigned long long)loads,
                 (unsigned long long)max_pump_ns,
                 (unsigned long)nrows,
                 (unsigned long long)batches[PROBE_BATCHES / 2U]);
    for (size_t rank = 0U; rank < 8U && rank < nrows; rank++) {
        size_t hottest = rank;

        for (size_t row = rank + 1U; row < nrows; row++) {
            if (rows[row].cpu_ns > rows[hottest].cpu_ns)
                hottest = row;
        }
        if (hottest != rank) {
            LegacyLineProbeRow swap = rows[rank];

            rows[rank] = rows[hottest];
            rows[hottest] = swap;
        }
        (void)printf("probe.legacy_hot rank=%lu line=%lu bytes=%u "
                     "entry=%u exit=%u spans=%u cpu_ns=%llu\n",
                     (unsigned long)(rank + 1U),
                     (unsigned long)rows[rank].line,
                     (unsigned)rows[rank].len,
                     (unsigned)rows[rank].entry_state,
                     (unsigned)rows[rank].exit_state,
                     (unsigned)rows[rank].spans,
                     (unsigned long long)rows[rank].cpu_ns);
    }
    status = 0;
done:
    if (status != 0)
        (void)fprintf(stderr, "perf_syn: legacy probe replay failed\n");
    frozen_free(&fixture);
    return status;
}

static bool measure_detect(u64 *samples, size_t count)
{
    static const char *const paths[] = {
        "config.ini", "/tmp/.editorconfig", "unit.service",
        "archive.tar.xyz", "README", "settings.properties",
        "unknown.zzz", "desktop.desktop"
    };

    for (size_t sample = 0U; sample < count; sample++) {
        u64 detected = 0U;
        u64 start;
        u64 end;

        if (!now_ns(&start))
            return false;
        for (u32 i = 0U; i < PERF_SYN_DETECT_PATHS; i++) {
            u32 lang = yew_syn_lang_for(paths[i % YEW_ARRAY_LEN(paths)],
                                        NULL, 0U);

            detected += lang;
        }
        if (!now_ns(&end) || end < start)
            return false;
        samples[sample] = end - start;
        perf_syn_sink += detected;
    }
    return true;
}

static int detect_probe(void)
{
    u64 samples[PERF_SYN_TRIALS];
    Timing measured;
    SynCheck check = {false, false, false};
    bool advisory = yew_perf_advisory();

    yew_syn_discovery_set_bypass(true);
    if (!measure_detect(samples, YEW_ARRAY_LEN(samples)))
        return 2;
    measured = timing_of(samples, YEW_ARRAY_LEN(samples));
    syn_check_timing(&check, measured.p99, PERF_SYN_DETECT_P99_LIMIT_NS);
    (void)printf("syn.detect_10000         median_ns=%llu p99_ns=%llu%s\n",
                 (unsigned long long)measured.median,
                 (unsigned long long)measured.p99,
                 syn_check_verdict(&check, advisory));
    return syn_check_failed(&check, advisory) ? 1 : 0;
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

static char *sibling_yew_path(const char *self)
{
    const char *slash = strrchr(self, '/');
    size_t prefix = slash == NULL ? 0U : (size_t)(slash - self) + 1U;
    char *path = malloc(prefix + sizeof("yew"));

    if (path == NULL)
        return NULL;
    if (prefix != 0U)
        (void)memcpy(path, self, prefix);
    (void)memcpy(path + prefix, "yew", sizeof("yew"));
    return path;
}

static bool run_yew_child(const char *yew, const char *arg1,
                          const char *arg2, const char *arg3, int *code)
{
    pid_t pid = fork();
    pid_t waited;
    int status;

    if (pid < 0)
        return false;
    if (pid == 0) {
        int null_fd = open("/dev/null", O_WRONLY);

        if (null_fd < 0 || dup2(null_fd, STDOUT_FILENO) < 0 ||
            dup2(null_fd, STDERR_FILENO) < 0)
            _exit(127);
        if (null_fd > STDERR_FILENO)
            (void)close(null_fd);
        (void)execl(yew, yew, arg1, arg2, arg3, (char *)NULL);
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

static bool measure_all_pack_loads(const char *self, u64 *cold_ns,
                                   u64 *warm_ns)
{
    char root[] = "/tmp/yew-perf-pack-XXXXXX";
    const char *old_root = getenv("XDG_CACHE_HOME");
    const char *old_config = getenv("XDG_CONFIG_HOME");
    const char *old_bypass = getenv("YEW_NO_SYN_CACHE");
    char *saved_root = old_root == NULL ? NULL : strdup(old_root);
    char *saved_config = old_config == NULL ? NULL : strdup(old_config);
    char *saved_bypass = old_bypass == NULL ? NULL : strdup(old_bypass);
    char *yew = sibling_yew_path(self);
    u64 start;
    u64 end;
    int code;
    bool ok = false;

    *cold_ns = 0U;
    *warm_ns = 0U;
    if ((old_root != NULL && saved_root == NULL) || yew == NULL ||
        (old_config != NULL && saved_config == NULL) ||
        (old_bypass != NULL && saved_bypass == NULL) ||
        mkdtemp(root) == NULL || setenv("XDG_CACHE_HOME", root, 1) != 0 ||
        setenv("XDG_CONFIG_HOME", root, 1) != 0 ||
        unsetenv("YEW_NO_SYN_CACHE") != 0)
        goto done;
    yew_syn_cache_set_bypass(false);
    if (!now_ns(&start) ||
        !run_yew_child(yew, "syn", "compile", "--all", &code) ||
        !now_ns(&end) || end < start || code != 0)
        goto cleanup;
    *cold_ns = end - start;
    if (!now_ns(&start) ||
        !run_yew_child(yew, "syn", "compile", "--all", &code) ||
        !now_ns(&end) || end < start || code != 0)
        goto cleanup;
    *warm_ns = end - start;
    ok = true;
cleanup:
    cache_fixture_remove(root);
done:
    if (saved_root != NULL)
        (void)setenv("XDG_CACHE_HOME", saved_root, 1);
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
    free(saved_root);
    free(saved_config);
    free(saved_bypass);
    free(yew);
    return ok;
}

static bool measure_clean_list(const char *self, u64 *samples,
                               size_t count, u64 *compiled)
{
    char *yew = sibling_yew_path(self);
    int code;
    bool ok = false;

    *compiled = 0U;
    if (yew == NULL || access(yew, X_OK) != 0 ||
        !run_yew_child(yew, "--clean", "syn", "list", &code) || code != 0)
        goto done;
    for (size_t i = 0U; i < count; i++) {
        u64 start;
        u64 end;

        if (!now_ns(&start) ||
            !run_yew_child(yew, "--clean", "syn", "list", &code) ||
            !now_ns(&end) || end < start || code != 0)
            goto done;
        samples[i] = end - start;
    }
    yew_syn_discovery_set_bypass(true);
    yew_syn_compile_count_reset();
    if (yew_syn_lang_count() != yew_syn_builtin_langs_len)
        goto done;
    for (size_t i = 0U; i < yew_syn_builtin_langs_len; i++) {
        if (yew_syn_lang_desc(yew_syn_builtin_langs[i].id) == NULL)
            goto done;
    }
    *compiled = yew_syn_compile_count();
    ok = true;
done:
    free(yew);
    return ok;
}

static bool measure_runtime_data_size(u64 *bytes)
{
    *bytes = 0U;
    for (size_t i = 0U; i < yew_syn_builtin_langs_len; i++) {
        struct stat st;

        if (stat(yew_syn_builtin_langs[i].source, &st) != 0 ||
            !S_ISREG(st.st_mode) || st.st_size < 0 ||
            (u64)st.st_size > UINT64_MAX - *bytes)
            return false;
        *bytes += (u64)st.st_size;
    }
    return true;
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
     * state.  Retain the median of three wall-clock calls for interactive
     * latency.  A truncated line is cheaper than the wall clock itself, so
     * only that case uses a batched process-CPU measurement. */
    for (size_t i = 0U; i < count; i++) {
        u64 target = (u64)i * fixture->spec->lines / (u64)count;
        u64 elapsed[3];
        size_t hi;
        u32 exit_state = entry_state;

        /* A fixture may contain fewer rows than the requested sample count.
         * Repeated targets must replay from the real sequential state instead
         * of advancing past EOF and timing synthetic empty lines. */
        if (target < line) {
            lo = 0U;
            line = 0U;
            entry_state = YEW_SYN_STATE_ROOT;
            exit_state = entry_state;
        }
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
            bool truncated = hi - lo > YEW_SYN_LINE_BYTE_CAP;
            u32 replays = truncated ? PERF_SYN_TRUNCATED_REPLAYS : 1U;
            u64 start;
            u64 end;

            if (!(truncated ? now_cpu_ns(&start) : now_ns(&start)))
                return false;
            for (u32 replay = 0U; replay < replays; replay++) {
                yew_syn_line(fixture->engine, entry_state,
                             fixture->source.data + lo, (u32)(hi - lo),
                             &out);
                if (out.stop != YEW_SYN_STOP_OK &&
                    out.stop != YEW_SYN_STOP_BYTES)
                    return false;
                exit_state = out.exit_state;
                perf_syn_sink += out.n + out.exit_state;
            }
            if (!(truncated ? now_cpu_ns(&end) : now_ns(&end)) ||
                end < start)
                return false;
            elapsed[attempt] = (end - start) / replays;
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

static bool html_scan_rows(FrozenFixture *fixture, SynEngine *guest[2],
                           HtmlScanRow *rows, size_t cap, size_t *count)
{
    u32 embedded_state = YEW_SYN_STATE_ROOT;
    u32 direct_state = YEW_SYN_STATE_ROOT;
    size_t lo = 0U;
    u64 line_no = 0U;
    size_t n = 0U;

    while (lo < fixture->source.len) {
        SynSpan spans[YEW_SYN_MAX_SPANS];
        SynLineOut embedded = {spans, 0U, YEW_ARRAY_LEN(spans), 0U, 0U};
        SynLineOut direct = {spans, 0U, YEW_ARRAY_LEN(spans), 0U, 0U};
        size_t hi = lo;
        u64 row = line_no % 20U;
        bool body = row == 2U || row == 3U || row == 6U || row == 7U;
        size_t guest_index = row >= 6U ? 1U : 0U;

        while (hi < fixture->source.len &&
               fixture->source.data[hi] != (u8)'\n')
            hi++;
        if (hi - lo > UINT32_MAX)
            return false;
        if (body && n >= cap)
            return false;
        if (body) {
            if (row == 2U || row == 6U)
                direct_state = YEW_SYN_STATE_ROOT;
            rows[n].lo = lo;
            rows[n].len = (u32)(hi - lo);
            rows[n].guest = (u8)guest_index;
            rows[n].embedded_entry = embedded_state;
            rows[n].direct_entry = direct_state;
        }
        yew_syn_line(fixture->engine, embedded_state,
                     fixture->source.data + lo, (u32)(hi - lo), &embedded);
        if (embedded.stop != YEW_SYN_STOP_OK &&
            embedded.stop != YEW_SYN_STOP_BYTES)
            return false;
        embedded_state = embedded.exit_state;
        if (body) {
            yew_syn_line(guest[guest_index], direct_state,
                         fixture->source.data + lo, (u32)(hi - lo), &direct);
            if (direct.stop != YEW_SYN_STOP_OK &&
                direct.stop != YEW_SYN_STOP_BYTES)
                return false;
            direct_state = direct.exit_state;
            rows[n].embedded_exit = embedded.exit_state;
            rows[n].direct_exit = direct.exit_state;
            rows[n].embedded_spans = embedded.n;
            rows[n].direct_spans = direct.n;
            perf_syn_sink += embedded.n + embedded.exit_state + direct.n +
                             direct.exit_state;
            n++;
        } else {
            perf_syn_sink += embedded.n + embedded.exit_state;
        }
        lo = hi < fixture->source.len ? hi + 1U : hi;
        line_no++;
    }
    *count = n;
    return n == PERF_SYN_HTML_BODY_LINES;
}

static bool html_scan_verify(FrozenFixture *fixture, SynEngine *guest[2],
                             const HtmlScanRow *rows, size_t count)
{
    for (size_t i = 0U; i < count; i++) {
        SynSpan spans[YEW_SYN_MAX_SPANS];
        SynLineOut embedded = {spans, 0U, YEW_ARRAY_LEN(spans), 0U, 0U};
        SynLineOut direct = {spans, 0U, YEW_ARRAY_LEN(spans), 0U, 0U};
        const HtmlScanRow *row = &rows[i];

        yew_syn_line(fixture->engine, row->embedded_entry,
                     fixture->source.data + row->lo, row->len, &embedded);
        yew_syn_line(guest[row->guest], row->direct_entry,
                     fixture->source.data + row->lo, row->len, &direct);
        if (embedded.stop != YEW_SYN_STOP_OK ||
            direct.stop != YEW_SYN_STOP_OK ||
            embedded.exit_state != row->embedded_exit ||
            direct.exit_state != row->direct_exit ||
            embedded.n != row->embedded_spans ||
            direct.n != row->direct_spans)
            return false;
        perf_syn_sink += embedded.n + embedded.exit_state + direct.n +
                         direct.exit_state;
    }
    return true;
}

static bool html_scan_arm(FrozenFixture *fixture, SynEngine *guest[2],
                          const HtmlScanRow *rows, size_t count,
                          bool embedded, u64 *elapsed_ns)
{
    u64 start;
    u64 end;

    if (!now_cpu_ns(&start))
        return false;
    for (size_t replay = 0U; replay < PERF_SYN_HTML_SCAN_REPLAYS; replay++) {
        for (size_t i = 0U; i < count; i++) {
            SynSpan spans[YEW_SYN_MAX_SPANS];
            const HtmlScanRow *row = &rows[i];
            SynLineOut out = {spans, 0U, YEW_ARRAY_LEN(spans), 0U, 0U};

            yew_syn_line(embedded ? fixture->engine : guest[row->guest],
                         embedded ? row->embedded_entry : row->direct_entry,
                         fixture->source.data + row->lo, row->len, &out);
            perf_syn_sink += out.n + out.exit_state;
        }
    }
    if (!now_cpu_ns(&end) || end < start)
        return false;
    *elapsed_ns = (end - start) / PERF_SYN_HTML_SCAN_REPLAYS;
    return true;
}

static void html_scan_sort(HtmlScanTrial *trials, size_t count)
{
    for (size_t i = 1U; i < count; i++) {
        HtmlScanTrial value = trials[i];
        size_t at = i;

        while (at != 0U && trials[at - 1U].ratio_bp > value.ratio_bp) {
            trials[at] = trials[at - 1U];
            at--;
        }
        trials[at] = value;
    }
}

static bool measure_html_scan_pair(FrozenFixture *fixture,
                                   u64 *embedded_ns, u64 *plain_ns,
                                   u64 *ratio_bp)
{
    HtmlScanRow rows[PERF_SYN_HTML_BODY_LINES];
    HtmlScanTrial trials[PERF_SYN_HTML_SCAN_TRIALS];
    u32 javascript = yew_syn_lang_named("javascript");
    u32 css = yew_syn_lang_named("css");
    SynEngine *guest[2] = {yew_syn_engine_for(javascript),
                           yew_syn_engine_for(css)};
    size_t count = 0U;

    if (guest[0] == NULL || guest[1] == NULL ||
        !html_scan_rows(fixture, guest, rows, YEW_ARRAY_LEN(rows), &count) ||
        !html_scan_verify(fixture, guest, rows, count))
        return false;
    for (size_t trial = 0U; trial < YEW_ARRAY_LEN(trials); trial++) {
        u64 *arms[2] = {&trials[trial].embedded_ns,
                        &trials[trial].plain_ns};

        for (size_t step = 0U; step < 2U; step++) {
            size_t which = (trial + step) & 1U;

            if (!html_scan_arm(fixture, guest, rows, count, which == 0U,
                               arms[which]))
                return false;
        }
        if (trials[trial].plain_ns == 0U ||
            trials[trial].embedded_ns >
                UINT64_MAX / PERF_SYN_HTML_RATIO_BASE)
            return false;
        trials[trial].ratio_bp =
            (trials[trial].embedded_ns * PERF_SYN_HTML_RATIO_BASE +
             trials[trial].plain_ns / 2U) / trials[trial].plain_ns;
    }
    html_scan_sort(trials, YEW_ARRAY_LEN(trials));
    *embedded_ns = trials[YEW_ARRAY_LEN(trials) / 2U].embedded_ns;
    *plain_ns = trials[YEW_ARRAY_LEN(trials) / 2U].plain_ns;
    *ratio_bp = trials[YEW_ARRAY_LEN(trials) / 2U].ratio_bp;
    return true;
}

static bool measure_definition_switch(FrozenFixture *fixture,
                                      u64 *per_line_ns)
{
    static const char *const names[] = {
        "c", "javascript", "typescript", "python",
        "rust", "go", "sh", "html"
    };
    enum { BODY_LINES = 600U * 3U };
    SynEngine *guest[YEW_ARRAY_LEN(names)];
    u64 embedded[PERF_SYN_TRIALS];
    u64 plain[PERF_SYN_TRIALS];

    for (size_t i = 0U; i < YEW_ARRAY_LEN(names); i++) {
        guest[i] = yew_syn_engine_for(yew_syn_lang_named(names[i]));
        if (guest[i] == NULL)
            return false;
    }
    for (size_t trial = 0U; trial < PERF_SYN_TRIALS; trial++) {
        u64 *results[2] = {&embedded[trial], &plain[trial]};

        for (size_t step = 0U; step < 2U; step++) {
            size_t which = (trial + step) & 1U;
            u32 master_state = YEW_SYN_STATE_ROOT;
            u32 guest_state = YEW_SYN_STATE_ROOT;
            size_t lo = 0U;
            u64 line = 0U;
            u64 body_lines = 0U;

            *results[which] = 0U;
            while (lo < fixture->source.len) {
                SynSpan spans[YEW_SYN_MAX_SPANS];
                SynLineOut out = {.spans = spans,
                                  .cap = YEW_ARRAY_LEN(spans)};
                size_t hi = lo;
                u64 row = line % 10U;
                bool body = row >= 6U && row <= 8U;
                size_t guest_index = (size_t)((line / 10U) %
                                              YEW_ARRAY_LEN(guest));
                u64 start = 0U;
                u64 end = 0U;

                while (hi < fixture->source.len &&
                       fixture->source.data[hi] != (u8)'\n')
                    hi++;
                if (hi - lo > UINT32_MAX)
                    return false;
                if (which != 0U && !body) {
                    lo = hi < fixture->source.len ? hi + 1U : hi;
                    line++;
                    continue;
                }
                if (which != 0U && row == 6U)
                    guest_state = YEW_SYN_STATE_ROOT;
                if (body && !now_ns(&start))
                    return false;
                yew_syn_line(which == 0U ? fixture->engine :
                                               guest[guest_index],
                             which == 0U ? master_state : guest_state,
                             fixture->source.data + lo, (u32)(hi - lo),
                             &out);
                if (body && (!now_ns(&end) || end < start))
                    return false;
                if (out.stop != YEW_SYN_STOP_OK &&
                    out.stop != YEW_SYN_STOP_BYTES)
                    return false;
                if (body) {
                    *results[which] += end - start;
                    body_lines++;
                }
                if (which == 0U)
                    master_state = out.exit_state;
                else
                    guest_state = out.exit_state;
                perf_syn_sink += out.n + out.exit_state;
                lo = hi < fixture->source.len ? hi + 1U : hi;
                line++;
            }
            if (body_lines != BODY_LINES)
                return false;
        }
    }
    stable_sort(embedded, YEW_ARRAY_LEN(embedded));
    stable_sort(plain, YEW_ARRAY_LEN(plain));
    *per_line_ns = embedded[PERF_SYN_TRIALS / 2U] >
                           plain[PERF_SYN_TRIALS / 2U]
                       ? (embedded[PERF_SYN_TRIALS / 2U] -
                          plain[PERF_SYN_TRIALS / 2U]) / BODY_LINES
                       : 0U;
    return true;
}

static bool measure_frozen_edit(FrozenFixture *fixture, u64 *samples,
                                size_t count)
{
    TextBuf *tb = yew_textbuf_from_bytes(fixture->source.data,
                                         fixture->source.len);
    SynBuf syn;
    size_t edit_at = 4U;
    size_t line_start = 0U;
    u64 edit_line = 0U;
    bool embedded_fixture = false;
    bool ok = false;

    if (tb == NULL)
        return false;
    if (strcmp(fixture->spec->stem, "md_embed") == 0) {
        edit_line = 6U;
        embedded_fixture = true;
    } else if (strcmp(fixture->spec->stem, "html_embed") == 0) {
        edit_line = 2U;
        embedded_fixture = true;
    } else if (strcmp(fixture->spec->stem, "sh_subst") == 0) {
        edit_at = 8U;
        embedded_fixture = true;
    } else if (strcmp(fixture->spec->stem, "tsx_embed") == 0) {
        const u8 *tick = memchr(fixture->source.data, '`',
                               fixture->source.len);

        if (tick == NULL) {
            yew_textbuf_free(tb);
            return false;
        }
        edit_at = (size_t)(tick - fixture->source.data) + 1U;
        embedded_fixture = true;
    }
    if (edit_line != 0U) {
        u64 line = 0U;

        line_start = 0U;
        while (line_start < fixture->source.len && line < edit_line) {
            if (fixture->source.data[line_start++] == (u8)'\n')
                line++;
        }
        if (line != edit_line || line_start + 4U >= fixture->source.len) {
            yew_textbuf_free(tb);
            return false;
        }
        edit_at = line_start + 4U;
    }
    yew_syn_buf_init(&syn);
    yew_syn_buf_bind(&syn, fixture->engine);
    yew_syn_attach(&syn, 1U, tb);
    if (!settle_all(&syn, tb, LINENO(0U), LINENO(200U), INT64_MAX,
                    NULL, NULL, NULL))
        goto done;
    if (embedded_fixture) {
        SynState stack;
        size_t line_end = line_start;

        while (line_end < fixture->source.len &&
               fixture->source.data[line_end] != (u8)'\n')
            line_end++;
        if (edit_line >= syn.entry.len || line_end - line_start > UINT32_MAX ||
            !yew_syn_stack_at(fixture->engine, syn.entry.data[edit_line],
                              fixture->source.data + line_start,
                              (u32)(line_end - line_start),
                              (u32)(edit_at - line_start), &stack) ||
            stack.ndef <= 1U)
            goto done;
    }
    for (size_t i = 0U; i < count; i++) {
        static const u8 byte = (u8)'x';
        SynSettleReport report;
        SynSettleReport restore;
        u64 start;
        u64 end;

        yew_textbuf_insert(tb, BYTEOFF(edit_at), &byte, 1U);
        yew_syn_edit(&syn, LINENO(edit_line), 0U, 0U);
        if (!now_ns(&start))
            goto done;
        /* A keystroke receives the frame budget.  The idle embedded-language
         * pump is measured separately and must not contaminate edit latency. */
        yew_syn_settle(&syn, tb, LINENO(edit_line), LINENO(200U),
                       YEW_SYN_FRAME_BUDGET_US, &report);
        if (!now_ns(&end) || end < start || !report.fixpoint ||
            report.lines > 2U)
            goto done;
        samples[i] = end - start;
        perf_syn_sink += report.lines;
        yew_textbuf_delete(tb, (Span){edit_at, edit_at + 1U});
        yew_syn_edit(&syn, LINENO(edit_line), 0U, 0U);
        yew_syn_settle(&syn, tb, LINENO(edit_line), LINENO(200U),
                       YEW_SYN_FRAME_BUDGET_US, &restore);
        if (!restore.fixpoint || restore.lines > 2U)
            goto done;
    }
    ok = true;
done:
    yew_syn_detach(&syn);
    yew_textbuf_free(tb);
    return ok;
}

static int probe_legacy_edit(const char *stem)
{
    enum { PROBE_EDIT_SAMPLES = 1001 };
    FrozenFixture fixture;
    const FrozenSpec *spec = NULL;
    u64 *samples = NULL;
    Timing timing;
    int status = 2;

    for (size_t i = 0U; i < PERF_SYN_MD_EMBED_INDEX; i++) {
        if (strcmp(frozen_specs[i].stem, stem) == 0) {
            spec = &frozen_specs[i];
            break;
        }
    }
    if (spec == NULL) {
        (void)fprintf(stderr, "perf_syn: unknown legacy stem '%s'\n", stem);
        return 2;
    }
    yew_syn_discovery_set_bypass(true);
    if (!frozen_init(&fixture, spec)) {
        (void)fprintf(stderr, "perf_syn: legacy edit probe setup failed\n");
        return 2;
    }
    samples = calloc(PROBE_EDIT_SAMPLES, sizeof(*samples));
    if (samples != NULL &&
        measure_frozen_edit(&fixture, samples, PROBE_EDIT_SAMPLES)) {
        timing = timing_of(samples, PROBE_EDIT_SAMPLES);
        (void)printf("probe.legacy_edit stem=%s resident=0 samples=%u "
                     "median_ns=%llu p99_ns=%llu\n",
                     stem, (unsigned)PROBE_EDIT_SAMPLES,
                     (unsigned long long)timing.median,
                     (unsigned long long)timing.p99);
        status = 0;
    } else {
        (void)fprintf(stderr, "perf_syn: legacy edit probe failed\n");
    }
    free(samples);
    frozen_free(&fixture);
    return status;
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
    cursor.goal_col = (CCol){0U};
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

static bool plain_scroll_fixture_init(FrozenFixture *fixture,
                                      FrozenSpec *spec)
{
    static const u8 row[] =
        "plain text row for full-stack scroll measurement\n";
    const size_t row_len = sizeof(row) - 1U;
    const size_t lines = 10000U;
    size_t len;

    (void)memset(fixture, 0, sizeof(*fixture));
    (void)memset(spec, 0, sizeof(*spec));
    if (lines > SIZE_MAX / row_len)
        return false;
    len = lines * row_len;
    fixture->source.data = malloc(len);
    if (fixture->source.data == NULL)
        return false;
    for (size_t i = 0U; i < lines; i++)
        (void)memcpy(fixture->source.data + i * row_len, row, row_len);
    fixture->source.len = len;
    spec->stem = "plain";
    spec->source_path = "<generated-plain-10kloc>";
    spec->definition_path = NULL;
    spec->lines = lines;
    spec->bytes = len;
    fixture->spec = spec;
    return fixture->source.len == spec->bytes &&
           source_lines(&fixture->source) == spec->lines;
}

static const ProfFrame *scroll_prof_frame(const Prof *prof, u32 at)
{
    u32 oldest = (prof->head + prof->cap - prof->n) % prof->cap;

    return &prof->ring[(oldest + at) % prof->cap];
}

static bool measure_scroll_profile(FrozenFixture *fixture, const char *name,
                                   u16 rows, u16 cols, bool wrap,
                                   ScrollProfile *result)
{
    PaintFixture paint;

    (void)memset(result, 0, sizeof(*result));
    result->name = name;
    if (!paint_init(&paint, fixture, rows, cols, wrap, false, true))
        return false;
    yew_prof_init(&paint.ed.prof, &paint.ed.arena, true);
    for (u32 frame = 0U; frame < PERF_SYN_SCROLL_FRAMES; frame++) {
        u32 bytes_out;

        yew_prof_frame_begin(&paint.ed.prof);
        yew_prof_phase(&paint.ed.prof, YEW_PH_DISPATCH);
        yew_vp_scroll(&paint.win, 1);
        yew_vp_push_cursor(&paint.win);
        yew_prof_phase(&paint.ed.prof, YEW_PH_SYN);
        if (paint.buffer.syn.settling) {
            SynSettleReport report;
            LineNo lo = yew_win_view_top(&paint.win);
            LineNo hi = yew_vp_last_visible_line(&paint.win);

            if (hi.v != UINT64_MAX)
                hi.v++;
            yew_syn_settle(&paint.buffer.syn, paint.buffer.tb, lo, hi,
                           YEW_SYN_FRAME_BUDGET_US, &report);
        }
        yew_prof_phase(&paint.ed.prof, YEW_PH_LAYOUT);
        yew_prof_phase(&paint.ed.prof, YEW_PH_RENDER);
        yew_draw_win(&paint.ed, &paint.win);
        yew_grid_mark_all(&paint.ed.grid);
        paint.ed.frame.len = 0U;
        perf_syn_sink += yew_render_frame(&paint.ed.render, &paint.ed.grid,
                                          &paint.ed.frame);
        bytes_out = paint.ed.frame.len > UINT32_MAX ? UINT32_MAX :
                                                       (u32)paint.ed.frame.len;
        yew_prof_phase(&paint.ed.prof, YEW_PH_WRITE);
        yew_grid_flip(&paint.ed.grid);
        yew_prof_frame_end(&paint.ed.prof, 1U, bytes_out,
                           YEW_PF_FULL_DAMAGE);
    }
    result->frames = paint.ed.prof.n;
    for (u32 i = 0U; i < paint.ed.prof.n; i++) {
        const ProfFrame *frame = scroll_prof_frame(&paint.ed.prof, i);

        if (UINT64_MAX - result->total_ns < frame->total_ns ||
            UINT64_MAX - result->render_ns <
                frame->ph_ns[YEW_PH_RENDER] ||
            UINT64_MAX - result->syn_ns < frame->ph_ns[YEW_PH_SYN]) {
            paint_free(&paint);
            return false;
        }
        result->total_ns += frame->total_ns;
        result->render_ns += frame->ph_ns[YEW_PH_RENDER];
        result->syn_ns += frame->ph_ns[YEW_PH_SYN];
    }
    if (result->frames != PERF_SYN_SCROLL_FRAMES || result->total_ns == 0U) {
        paint_free(&paint);
        return false;
    }
    result->fps_milli = (u64)result->frames * UINT64_C(1000000000000) /
                        result->total_ns;
    /* s05/s41 define these shares against the fixed 120-fps whole-frame
     * budget, not against an unpaced benchmark's active CPU time.  Keep
     * the latter as diagnostic work shares, but never gate on them. */
    result->render_permille = result->render_ns * 1000U /
        ((u64)result->frames * PERF_SYN_SCROLL_FRAME_BUDGET_NS);
    result->syn_permille = result->syn_ns * 1000U /
        ((u64)result->frames * PERF_SYN_SCROLL_FRAME_BUDGET_NS);
    result->render_work_permille = result->render_ns * 1000U /
                                   result->total_ns;
    result->syn_work_permille = result->syn_ns * 1000U /
                                result->total_ns;
    paint_free(&paint);
    return true;
}

static bool measure_scroll_profiles(FrozenFixture *markdown,
                                    FrozenFixture *plain,
                                    ScrollProfile *profiles)
{
    for (size_t i = 0U; i < YEW_ARRAY_LEN(scroll_profile_cases); i++) {
        const ScrollProfileCase *profile = &scroll_profile_cases[i];
        FrozenFixture *fixture = profile->markdown ? markdown : plain;

        if (!measure_scroll_profile(fixture, profile->name, profile->rows,
                                    profile->cols, profile->wrap,
                                    &profiles[i])) {
            (void)fprintf(stderr, "perf_syn: scroll profile '%s' failed\n",
                          profile->name);
            return false;
        }
    }
    return true;
}

/* Phase shares are measured nanoseconds over a fixed frame budget, so they
 * scale with machine speed exactly like the throughput they accompany. */
static SynCheck scroll_profile_check(const ScrollProfile *profile,
                                     ScrollProfilePart part)
{
    SynCheck check = {false, false, false};

    switch (part) {
    case SCROLL_PART_THROUGHPUT:
        syn_check_throughput(&check, (double)profile->fps_milli / 1000.0,
                             PERF_SYN_SCROLL_MIN_FPS);
        break;
    case SCROLL_PART_RENDER_SHARE:
        syn_check_sample(&check, profile->render_permille,
                         PERF_SYN_SCROLL_RENDER_LIMIT_PERMILLE);
        break;
    case SCROLL_PART_SYNTAX_SHARE:
        syn_check_sample(&check, profile->syn_permille,
                         PERF_SYN_SCROLL_SYN_LIMIT_PERMILLE);
        break;
    }
    return check;
}

/* Prints every profile row; returns true when any row fails the gate. */
static bool report_scroll_profiles(const ScrollProfile *profiles,
                                   bool advisory)
{
    bool failed = false;

    for (size_t i = 0U; i < YEW_ARRAY_LEN(scroll_profile_cases); i++) {
        const ScrollProfile *profile = &profiles[i];
        SynCheck throughput =
            scroll_profile_check(profile, SCROLL_PART_THROUGHPUT);
        SynCheck render =
            scroll_profile_check(profile, SCROLL_PART_RENDER_SHARE);
        SynCheck syntax =
            scroll_profile_check(profile, SCROLL_PART_SYNTAX_SHARE);

        (void)printf(
            "syn.scroll.throughput_%-20s frames=%u "
            "fps_milli=%llu active_total_ns=%llu%s\n",
            profile->name, (unsigned)profile->frames,
            (unsigned long long)profile->fps_milli,
            (unsigned long long)profile->total_ns,
            syn_check_verdict(&throughput, advisory));
        (void)printf(
            "syn.scroll.render_share_%-17s phase_ns=%llu "
            "budget_ns=%llu share_permille=%llu "
            "work_permille=%llu limit_permille=%u%s\n",
            profile->name, (unsigned long long)profile->render_ns,
            (unsigned long long)((u64)profile->frames *
                PERF_SYN_SCROLL_FRAME_BUDGET_NS),
            (unsigned long long)profile->render_permille,
            (unsigned long long)profile->render_work_permille,
            (unsigned)PERF_SYN_SCROLL_RENDER_LIMIT_PERMILLE,
            syn_check_verdict(&render, advisory));
        (void)printf(
            "syn.scroll.syntax_share_%-17s phase_ns=%llu "
            "budget_ns=%llu share_permille=%llu "
            "work_permille=%llu limit_permille=%u%s\n",
            profile->name, (unsigned long long)profile->syn_ns,
            (unsigned long long)((u64)profile->frames *
                PERF_SYN_SCROLL_FRAME_BUDGET_NS),
            (unsigned long long)profile->syn_permille,
            (unsigned long long)profile->syn_work_permille,
            (unsigned)PERF_SYN_SCROLL_SYN_LIMIT_PERMILLE,
            syn_check_verdict(&syntax, advisory));
        if (syn_check_failed(&throughput, advisory) ||
            syn_check_failed(&render, advisory) ||
            syn_check_failed(&syntax, advisory))
            failed = true;
    }
    return failed;
}

static int run_scroll_profile_gate(void)
{
    FrozenFixture markdown = {0};
    FrozenFixture plain = {0};
    FrozenSpec plain_spec = {0};
    ScrollProfile profiles[PERF_SYN_SCROLL_PROFILE_CASES] = {{0}};
    bool advisory = yew_perf_advisory();
    bool markdown_ready = false;
    int status = 0;

    (void)printf("perf_syn: mode %s\n", yew_perf_mode(advisory));
    yew_syn_discovery_set_bypass(true);
    if (!frozen_init(&markdown, &frozen_specs[PERF_SYN_MARKDOWN_INDEX])) {
        (void)fputs("perf_syn: markdown scroll fixture failed\n", stderr);
        status = 2;
    } else {
        markdown_ready = true;
    }
    if (status == 0 && !plain_scroll_fixture_init(&plain, &plain_spec)) {
        (void)fputs("perf_syn: plain scroll fixture failed\n", stderr);
        status = 2;
    }
    if (status == 0 &&
        !measure_scroll_profiles(&markdown, &plain, profiles))
        status = 2;
    if (status == 0 && report_scroll_profiles(profiles, advisory))
        status = 1;
    if (markdown_ready)
        frozen_free(&markdown);
    free(plain.source.data);
    return status;
}

static bool check_all_state_memory(FrozenFixture *fixtures,
                                   u64 *capacity_bytes, u64 *limit_bytes)
{
    SynBuf syn[PERF_SYN_FIXTURE_COUNT];
    size_t initialized = 0U;
    bool ok = false;

    *capacity_bytes = 0U;
    *limit_bytes = 0U;
    if (sizeof(*syn[0].entry.data) > PERF_SYN_ENTRY_LIMIT_BYTES)
        return false;
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
        line_bytes = lines * PERF_SYN_ENTRY_LIMIT_BYTES;
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
                       YEW_SYN_FRAME_BUDGET_US, &report);
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
                       YEW_SYN_FRAME_BUDGET_US, &restore);
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

/* Every whole-run result that a summary row judges. */
typedef struct SynSummary {
    Timing detect;
    Timing compile;
    Timing cache;
    Timing warm_start;
    Timing clean_list;
    Timing block;
    Timing block_multiline;
    Timing make_embed_line;
    Timing make_embed_view;
    u64 block_line_calls;
    u64 block_multiline_calls;
    u64 theme_line_calls;
    u64 comment_first_max_us;
    u64 comment_first_frames;
    u64 comment_idle_total_us;
    u64 comment_idle_max_us;
    u64 comment_idle_frames;
    u64 comment_state_logical_bytes;
    u64 comment_state_capacity_bytes;
    u64 comment_state_rss_growth;
    u64 comment_wall_ns;
    u64 whole_total_ns;
    u64 whole_max_frame_ns;
    u64 whole_frames;
    double markdown_wrap_fps;
    u64 all_state_capacity_bytes;
    u64 all_state_limit_bytes;
    u64 warm_start_compiled_max;
    u64 clean_list_compiled;
    u64 compile_all_cold_ns;
    u64 all_warm_load_ns;
    u64 runtime_data_bytes;
    u64 md_embed_idle_ticks;
    u64 md_embed_loads;
    u64 md_embed_pump_max_ns;
    u64 md_embed_states;
    u64 make_embed_idle_ticks;
    u64 make_embed_loads;
    u64 make_embed_pump_max_ns;
    u64 html_embed_scan_ns;
    u64 html_plain_scan_ns;
    u64 html_scan_ratio_bp;
    u64 definition_switch_ns;
} SynSummary;

typedef enum SynRow {
    SYN_ROW_DETECT,
    SYN_ROW_COMPILE,
    SYN_ROW_CACHE,
    SYN_ROW_WARM_START,
    SYN_ROW_CLEAN_LIST,
    SYN_ROW_COMPILE_ALL,
    SYN_ROW_WARM_ALL,
    SYN_ROW_RUNTIME_SIZE,
    SYN_ROW_BLOCK,
    SYN_ROW_BLOCK_MULTILINE,
    SYN_ROW_COMMENT_VIEW,
    SYN_ROW_COMMENT_IDLE,
    SYN_ROW_THEME_CALLS,
    SYN_ROW_MARKDOWN_SCROLL,
    SYN_ROW_WHOLE_SETTLE,
    SYN_ROW_COMMENT_STATE,
    SYN_ROW_ALL_STATE,
    SYN_ROW_MD_EMBED,
    SYN_ROW_MAKE_EMBED,
    SYN_ROW_HTML_SCAN,
    SYN_ROW_DEFINITION_SWITCH,
    SYN_ROW_COUNT
} SynRow;

/*
 * The summary-row decisions.  Correctness: compiled-definition counts,
 * runtime data bytes, provider line calls, frame counts and microseconds on
 * the injected settle clock, state capacity and RSS growth, embed load and
 * idle-tick counts, retained state count.  Everything measured on a real
 * clock is timing, including the html embedded/plain ratio: machine speed
 * mostly cancels out of it, but its 8 percent headroom sits inside a shared
 * runner's noise.
 */
static SynCheck summary_check(const SynSummary *s, SynRow row)
{
    SynCheck check = {false, false, false};

    switch (row) {
    case SYN_ROW_DETECT:
        syn_check_timing(&check, s->detect.median,
                         PERF_SYN_DETECT_HARD_LIMIT_NS);
        syn_check_timing(&check, s->detect.p99,
                         PERF_SYN_DETECT_P99_LIMIT_NS);
        break;
    case SYN_ROW_COMPILE:
        syn_check_sample(&check, s->compile.median,
                         PERF_SYN_COMPILE_LIMIT_NS);
        break;
    case SYN_ROW_CACHE:
        syn_check_sample(&check, s->cache.median, PERF_SYN_CACHE_LIMIT_NS);
        break;
    case SYN_ROW_WARM_START:
        syn_check_timing(&check, s->warm_start.p99,
                         PERF_SYN_WARM_START_LIMIT_NS);
        syn_check_correct(&check, s->warm_start_compiled_max > 1U);
        break;
    case SYN_ROW_CLEAN_LIST:
        syn_check_timing(&check, s->clean_list.median,
                         PERF_SYN_LIST_MEDIAN_LIMIT_NS);
        syn_check_timing(&check, s->clean_list.p99,
                         PERF_SYN_LIST_P99_LIMIT_NS);
        syn_check_correct(&check, s->clean_list_compiled != 0U);
        break;
    case SYN_ROW_COMPILE_ALL:
        syn_check_timing(&check, s->compile_all_cold_ns,
                         PERF_SYN_COMPILE_ALL_LIMIT_NS);
        break;
    case SYN_ROW_WARM_ALL:
        syn_check_timing(&check, s->all_warm_load_ns,
                         PERF_SYN_WARM_ALL_LIMIT_NS);
        break;
    case SYN_ROW_RUNTIME_SIZE:
        syn_check_correct(&check, s->runtime_data_bytes >
                                      PERF_SYN_RUNTIME_LIMIT_BYTES);
        break;
    case SYN_ROW_BLOCK:
        syn_check_sample(&check, s->block.p99, PERF_SYN_BLOCK_LIMIT_NS);
        syn_check_correct(&check, s->block_line_calls >
                                      PERF_SYN_BLOCK_MAX_LINE_CALLS);
        break;
    case SYN_ROW_BLOCK_MULTILINE:
        syn_check_sample(&check, s->block_multiline.p99,
                         PERF_SYN_BLOCK_LIMIT_NS);
        syn_check_correct(&check, s->block_multiline_calls >
                                      PERF_SYN_BLOCK_MAX_LINE_CALLS);
        break;
    case SYN_ROW_COMMENT_VIEW:
        syn_check_correct(&check,
                          s->comment_first_frames != 1U ||
                              s->comment_first_max_us >
                                  YEW_SYN_FRAME_BUDGET_US);
        break;
    case SYN_ROW_COMMENT_IDLE:
        syn_check_correct(&check,
                          s->comment_idle_frames == 0U ||
                              s->comment_idle_max_us >
                                  YEW_SYN_IDLE_BUDGET_US ||
                              s->comment_idle_total_us >
                                  PERF_SYN_COMMENT_TOTAL_US);
        syn_check_timing(&check, s->comment_wall_ns, UINT64_C(400000000));
        break;
    case SYN_ROW_THEME_CALLS:
        syn_check_correct(&check, s->theme_line_calls != 0U);
        break;
    case SYN_ROW_MARKDOWN_SCROLL:
        syn_check_throughput(&check, s->markdown_wrap_fps,
                             PERF_SYN_SCROLL_MIN_FPS);
        break;
    case SYN_ROW_WHOLE_SETTLE:
        syn_check_timing(&check, s->whole_total_ns, UINT64_C(45000000));
        syn_check_sample(&check, s->whole_max_frame_ns, UINT64_C(1000000));
        break;
    case SYN_ROW_COMMENT_STATE:
        syn_check_correct(&check,
                          s->comment_state_capacity_bytes >
                                  PERF_SYN_STATE_LIMIT_BYTES ||
                              s->comment_state_rss_growth >
                                  PERF_SYN_STATE_LIMIT_BYTES);
        break;
    case SYN_ROW_ALL_STATE:
        syn_check_correct(&check, s->all_state_capacity_bytes >
                                      s->all_state_limit_bytes);
        break;
    case SYN_ROW_MD_EMBED:
        syn_check_correct(&check, s->md_embed_idle_ticks > 8U ||
                                      s->md_embed_loads != 8U ||
                                      s->md_embed_states > 2500U);
        syn_check_sample(&check, s->md_embed_pump_max_ns,
                         UINT64_C(2000000));
        break;
    case SYN_ROW_MAKE_EMBED:
        syn_check_correct(&check, s->make_embed_idle_ticks > 1U ||
                                      s->make_embed_loads != 1U);
        syn_check_sample(&check, s->make_embed_pump_max_ns,
                         UINT64_C(2000000));
        syn_check_sample(&check, s->make_embed_line.median, 3000U);
        syn_check_sample(&check, s->make_embed_line.p99, 12000U);
        syn_check_sample(&check, s->make_embed_view.p99,
                         PERF_SYN_VIEW_200_LIMIT_NS);
        break;
    case SYN_ROW_HTML_SCAN:
        syn_check_timing(&check, s->html_scan_ratio_bp,
                         PERF_SYN_HTML_RATIO_LIMIT);
        if (s->html_plain_scan_ns == 0U)
            check.insane = true;
        break;
    case SYN_ROW_DEFINITION_SWITCH:
        syn_check_sample(&check, s->definition_switch_ns, 250U);
        break;
    case SYN_ROW_COUNT:
        break;
    }
    return check;
}

/* A summary in which every row passes in both modes. */
static void selftest_clean_summary(SynSummary *s)
{
    (void)memset(s, 0, sizeof(*s));
    s->detect = (Timing){1000U, 1000U};
    s->compile = (Timing){1000U, 1000U};
    s->cache = (Timing){1000U, 1000U};
    s->warm_start = (Timing){1000U, 1000U};
    s->clean_list = (Timing){1000U, 1000U};
    s->block = (Timing){1000U, 1000U};
    s->block_multiline = (Timing){1000U, 1000U};
    s->make_embed_line = (Timing){1000U, 1000U};
    s->make_embed_view = (Timing){1000U, 1000U};
    s->block_line_calls = PERF_SYN_BLOCK_MAX_LINE_CALLS;
    s->block_multiline_calls = PERF_SYN_BLOCK_MAX_LINE_CALLS;
    s->comment_first_max_us = YEW_SYN_FRAME_BUDGET_US;
    s->comment_first_frames = 1U;
    s->comment_idle_total_us = PERF_SYN_COMMENT_TOTAL_US;
    s->comment_idle_max_us = YEW_SYN_IDLE_BUDGET_US;
    s->comment_idle_frames = 1U;
    s->comment_state_capacity_bytes = PERF_SYN_STATE_LIMIT_BYTES;
    s->comment_state_rss_growth = PERF_SYN_STATE_LIMIT_BYTES;
    s->comment_wall_ns = 1000U;
    s->whole_total_ns = 1000U;
    s->whole_frames = 1U;
    s->markdown_wrap_fps = PERF_SYN_SCROLL_MIN_FPS;
    s->all_state_capacity_bytes = 1000U;
    s->all_state_limit_bytes = 1000U;
    s->warm_start_compiled_max = 1U;
    s->compile_all_cold_ns = 1000U;
    s->all_warm_load_ns = 1000U;
    s->runtime_data_bytes = PERF_SYN_RUNTIME_LIMIT_BYTES;
    s->md_embed_idle_ticks = 8U;
    s->md_embed_loads = 8U;
    s->md_embed_states = 2500U;
    s->make_embed_idle_ticks = 1U;
    s->make_embed_loads = 1U;
    s->html_embed_scan_ns = 1000U;
    s->html_plain_scan_ns = 1000U;
    s->html_scan_ratio_bp = PERF_SYN_HTML_RATIO_LIMIT;
}

typedef enum SelftestKind {
    SELFTEST_TIMING,       /* strict fails, advisory warns */
    SELFTEST_SANITY,       /* fails in both modes */
    SELFTEST_CORRECTNESS   /* fails in both modes, never a warning */
} SelftestKind;

/* Applies breach `which` to a clean summary; false once past the last. */
static bool selftest_breach(size_t which, SynSummary *s, SynRow *row,
                            SelftestKind *kind)
{
    const u64 x = YEW_PERF_ADVISORY_SANITY_MULTIPLIER;

    *kind = SELFTEST_CORRECTNESS;
    switch (which) {
    case 0U:
        s->runtime_data_bytes = PERF_SYN_RUNTIME_LIMIT_BYTES + 1U;
        *row = SYN_ROW_RUNTIME_SIZE;
        return true;
    case 1U:
        s->theme_line_calls = 1U;
        *row = SYN_ROW_THEME_CALLS;
        return true;
    case 2U:
        s->warm_start_compiled_max = 2U;
        *row = SYN_ROW_WARM_START;
        return true;
    case 3U:
        s->clean_list_compiled = 1U;
        *row = SYN_ROW_CLEAN_LIST;
        return true;
    case 4U:
        s->block_line_calls = PERF_SYN_BLOCK_MAX_LINE_CALLS + 1U;
        *row = SYN_ROW_BLOCK;
        return true;
    case 5U:
        s->block_multiline_calls = PERF_SYN_BLOCK_MAX_LINE_CALLS + 1U;
        *row = SYN_ROW_BLOCK_MULTILINE;
        return true;
    case 6U:
        s->comment_first_frames = 2U;
        *row = SYN_ROW_COMMENT_VIEW;
        return true;
    case 7U:
        s->comment_first_max_us = YEW_SYN_FRAME_BUDGET_US + 1U;
        *row = SYN_ROW_COMMENT_VIEW;
        return true;
    case 8U:
        s->comment_idle_frames = 0U;
        *row = SYN_ROW_COMMENT_IDLE;
        return true;
    case 9U:
        s->comment_idle_max_us = YEW_SYN_IDLE_BUDGET_US + 1U;
        *row = SYN_ROW_COMMENT_IDLE;
        return true;
    case 10U:
        s->comment_idle_total_us = PERF_SYN_COMMENT_TOTAL_US + 1U;
        *row = SYN_ROW_COMMENT_IDLE;
        return true;
    case 11U:
        s->comment_state_capacity_bytes = PERF_SYN_STATE_LIMIT_BYTES + 1U;
        *row = SYN_ROW_COMMENT_STATE;
        return true;
    case 12U:
        s->comment_state_rss_growth = PERF_SYN_STATE_LIMIT_BYTES + 1U;
        *row = SYN_ROW_COMMENT_STATE;
        return true;
    case 13U:
        s->all_state_capacity_bytes = s->all_state_limit_bytes + 1U;
        *row = SYN_ROW_ALL_STATE;
        return true;
    case 14U:
        s->md_embed_loads = 7U;
        *row = SYN_ROW_MD_EMBED;
        return true;
    case 15U:
        s->md_embed_idle_ticks = 9U;
        *row = SYN_ROW_MD_EMBED;
        return true;
    case 16U:
        s->md_embed_states = 2501U;
        *row = SYN_ROW_MD_EMBED;
        return true;
    case 17U:
        s->make_embed_loads = 0U;
        *row = SYN_ROW_MAKE_EMBED;
        return true;
    case 18U:
        s->make_embed_idle_ticks = 2U;
        *row = SYN_ROW_MAKE_EMBED;
        return true;
    default:
        break;
    }
    *kind = SELFTEST_TIMING;
    switch (which) {
    case 19U:
        s->detect.p99 = PERF_SYN_DETECT_P99_LIMIT_NS + 1U;
        *row = SYN_ROW_DETECT;
        return true;
    case 20U:
        s->compile.median = PERF_SYN_COMPILE_LIMIT_NS + 1U;
        *row = SYN_ROW_COMPILE;
        return true;
    case 21U:
        s->cache.median = PERF_SYN_CACHE_LIMIT_NS + 1U;
        *row = SYN_ROW_CACHE;
        return true;
    case 22U:
        s->warm_start.p99 = PERF_SYN_WARM_START_LIMIT_NS + 1U;
        *row = SYN_ROW_WARM_START;
        return true;
    case 23U:
        s->clean_list.median = PERF_SYN_LIST_MEDIAN_LIMIT_NS + 1U;
        *row = SYN_ROW_CLEAN_LIST;
        return true;
    case 24U:
        s->compile_all_cold_ns = PERF_SYN_COMPILE_ALL_LIMIT_NS + 1U;
        *row = SYN_ROW_COMPILE_ALL;
        return true;
    case 25U:
        s->all_warm_load_ns = PERF_SYN_WARM_ALL_LIMIT_NS + 1U;
        *row = SYN_ROW_WARM_ALL;
        return true;
    case 26U:
        s->block_multiline.p99 = PERF_SYN_BLOCK_LIMIT_NS + 1U;
        *row = SYN_ROW_BLOCK_MULTILINE;
        return true;
    case 27U:
        s->comment_wall_ns = UINT64_C(400000001);
        *row = SYN_ROW_COMMENT_IDLE;
        return true;
    case 28U:
        s->markdown_wrap_fps = PERF_SYN_SCROLL_MIN_FPS - 1.0;
        *row = SYN_ROW_MARKDOWN_SCROLL;
        return true;
    case 29U:
        s->whole_max_frame_ns = UINT64_C(1000001);
        *row = SYN_ROW_WHOLE_SETTLE;
        return true;
    case 30U:
        s->md_embed_pump_max_ns = UINT64_C(2000001);
        *row = SYN_ROW_MD_EMBED;
        return true;
    case 31U:
        s->make_embed_view.p99 = PERF_SYN_VIEW_200_LIMIT_NS + 1U;
        *row = SYN_ROW_MAKE_EMBED;
        return true;
    case 32U:
        s->html_scan_ratio_bp = PERF_SYN_HTML_RATIO_LIMIT + 1U;
        *row = SYN_ROW_HTML_SCAN;
        return true;
    case 33U:
        s->definition_switch_ns = 251U;
        *row = SYN_ROW_DEFINITION_SWITCH;
        return true;
    default:
        break;
    }
    *kind = SELFTEST_SANITY;
    switch (which) {
    case 34U:
        s->detect.median = 0U;
        *row = SYN_ROW_DETECT;
        return true;
    case 35U:
        s->compile_all_cold_ns = PERF_SYN_COMPILE_ALL_LIMIT_NS * x + 1U;
        *row = SYN_ROW_COMPILE_ALL;
        return true;
    case 36U:
        s->block.p99 = PERF_SYN_BLOCK_LIMIT_NS * x + 1U;
        *row = SYN_ROW_BLOCK;
        return true;
    case 37U:
        s->markdown_wrap_fps = PERF_SYN_SCROLL_MIN_FPS / (double)x - 0.01;
        *row = SYN_ROW_MARKDOWN_SCROLL;
        return true;
    case 38U:
        s->whole_total_ns = 0U;
        *row = SYN_ROW_WHOLE_SETTLE;
        return true;
    case 39U:
        s->html_plain_scan_ns = 0U;
        *row = SYN_ROW_HTML_SCAN;
        return true;
    case 40U:
        s->definition_switch_ns = 250U * x + 1U;
        *row = SYN_ROW_DEFINITION_SWITCH;
        return true;
    default:
        return false;
    }
}

static bool selftest_expect(const char *what, const SynCheck *check,
                            bool strict_fails, bool advisory_fails,
                            const char *advisory_verdict)
{
    bool ok = syn_check_failed(check, false) == strict_fails &&
              syn_check_failed(check, true) == advisory_fails &&
              strcmp(syn_check_verdict(check, true), advisory_verdict) == 0;

    if (!ok)
        (void)printf("FAIL policy: %s -> strict %s, advisory %s%s\n", what,
                     syn_check_failed(check, false) ? "fails" : "passes",
                     syn_check_failed(check, true) ? "fails" : "passes",
                     syn_check_verdict(check, true));
    return ok;
}

static int selftest_policy(void)
{
    const u64 budget = PERF_SYN_VIEW_24_LIMIT_NS;
    const u64 ceiling = budget * YEW_PERF_ADVISORY_SANITY_MULTIPLIER;
    const double min_fps = PERF_SYN_SCROLL_MIN_FPS;
    const double x = (double)YEW_PERF_ADVISORY_SANITY_MULTIPLIER;
    size_t failures = 0U;
    size_t checks = 0U;
    SynSummary clean;
    ScrollProfile profile;
    PerfCase theme = {"theme_switch_200x50", {1000U, 1000U}, {0U, 0U}};

    /* The helpers at, just over, and beyond the advisory ceiling. */
    {
        static const struct {
            u64 value;
            bool sample;
            bool strict_fails;
            bool advisory_fails;
            const char *verdict;
        } cases[] = {
            {1U, false, false, false, " ok"},
            {0U, false, true, true, " SANITY-FAIL"},
            {0U, true, false, false, " ok"},
            {PERF_SYN_VIEW_24_LIMIT_NS, false, false, false, " ok"},
            {PERF_SYN_VIEW_24_LIMIT_NS + 1U, false, true, false, " WARN"},
            {PERF_SYN_VIEW_24_LIMIT_NS + 1U, true, true, false, " WARN"},
            {PERF_SYN_VIEW_24_LIMIT_NS * 100U, false, true, false, " WARN"},
            {PERF_SYN_VIEW_24_LIMIT_NS * 100U, true, true, false, " WARN"},
            {PERF_SYN_VIEW_24_LIMIT_NS * 100U + 1U, false, true, true,
             " SANITY-FAIL"},
            {PERF_SYN_VIEW_24_LIMIT_NS * 100U + 1U, true, true, true,
             " SANITY-FAIL"}
        };

        _Static_assert(YEW_PERF_ADVISORY_SANITY_MULTIPLIER == 100,
                       "selftest table assumes the 100x sanity bound");
        for (size_t i = 0U; i < YEW_ARRAY_LEN(cases); i++) {
            SynCheck check = {false, false, false};

            if (cases[i].sample)
                syn_check_sample(&check, cases[i].value, budget);
            else
                syn_check_timing(&check, cases[i].value, budget);
            checks++;
            if (!selftest_expect(cases[i].sample ? "sample" : "timing",
                                 &check, cases[i].strict_fails,
                                 cases[i].advisory_fails, cases[i].verdict))
                failures++;
        }
        if (ceiling != PERF_SYN_VIEW_24_LIMIT_NS * 100U)
            failures++;
    }
    {
        static const struct {
            double scale;
            double offset;
            bool strict_fails;
            bool advisory_fails;
            const char *verdict;
        } cases[] = {
            {1.0, 0.0, false, false, " ok"},
            {1.0, -0.01, true, false, " WARN"},
            {0.01, 0.0, true, false, " WARN"},
            {0.01, -0.001, true, true, " SANITY-FAIL"},
            {0.0, 0.0, true, true, " SANITY-FAIL"}
        };

        for (size_t i = 0U; i < YEW_ARRAY_LEN(cases); i++) {
            SynCheck check = {false, false, false};

            syn_check_throughput(&check,
                                 min_fps * cases[i].scale + cases[i].offset,
                                 min_fps);
            checks++;
            if (!selftest_expect("throughput", &check,
                                 cases[i].strict_fails,
                                 cases[i].advisory_fails, cases[i].verdict))
                failures++;
        }
    }
    {
        SynCheck check = {false, false, false};

        syn_check_correct(&check, true);
        checks++;
        if (!selftest_expect("correctness", &check, true, true,
                             " REGRESSION"))
            failures++;
        syn_check_sample(&check, budget + 1U, budget);
        checks++;
        if (!selftest_expect("correctness with timing", &check, true, true,
                             " REGRESSION"))
            failures++;
    }

    /* The per-case decision: theme_line_calls is hard, the p99 timing. */
    {
        SynCheck check = case_check(CASE_THEME_SWITCH, &theme,
                                    PERF_SYN_GATE_BUDGETS, 1U);

        checks += 4U;
        if (!selftest_expect("theme_line_calls", &check, true, true,
                             " REGRESSION"))
            failures++;
        theme.measured.p99 = PERF_SYN_THEME_LIMIT_NS + 1U;
        check = case_check(CASE_THEME_SWITCH, &theme, PERF_SYN_GATE_BUDGETS,
                           0U);
        if (!selftest_expect("theme p99", &check, true, false, " WARN"))
            failures++;
        theme.measured.p99 = PERF_SYN_THEME_LIMIT_NS *
                             YEW_PERF_ADVISORY_SANITY_MULTIPLIER + 1U;
        check = case_check(CASE_THEME_SWITCH, &theme, PERF_SYN_GATE_BUDGETS,
                           0U);
        if (!selftest_expect("theme p99 beyond sanity", &check, true, true,
                             " SANITY-FAIL"))
            failures++;
        theme.measured = (Timing){0U, 1000U};
        theme.baseline = (Timing){65U, 70U};
        check = case_check(CASE_LINE_CAP, &theme, PERF_SYN_GATE_FULL, 0U);
        if (!selftest_expect("relative p99", &check, true, false, " WARN"))
            failures++;
    }

    /* The scroll-profile decision: fps and both phase shares are timing. */
    (void)memset(&profile, 0, sizeof(profile));
    profile.fps_milli = (u64)(min_fps * 1000.0);
    {
        SynCheck check;

        checks += 4U;
        for (int part = SCROLL_PART_THROUGHPUT;
             part <= SCROLL_PART_SYNTAX_SHARE; part++) {
            check = scroll_profile_check(&profile, (ScrollProfilePart)part);
            if (!selftest_expect("scroll profile clean", &check, false,
                                 false, " ok"))
                failures++;
        }
        profile.render_permille = PERF_SYN_SCROLL_RENDER_LIMIT_PERMILLE + 1U;
        check = scroll_profile_check(&profile, SCROLL_PART_RENDER_SHARE);
        if (!selftest_expect("render share", &check, true, false, " WARN"))
            failures++;
        profile.syn_permille = PERF_SYN_SCROLL_SYN_LIMIT_PERMILLE *
                               YEW_PERF_ADVISORY_SANITY_MULTIPLIER + 1U;
        check = scroll_profile_check(&profile, SCROLL_PART_SYNTAX_SHARE);
        checks++;
        if (!selftest_expect("syntax share beyond sanity", &check, true,
                             true, " SANITY-FAIL"))
            failures++;
        profile.fps_milli = (u64)(min_fps / x * 1000.0) - 1U;
        check = scroll_profile_check(&profile, SCROLL_PART_THROUGHPUT);
        checks++;
        if (!selftest_expect("scroll fps beyond sanity", &check, true, true,
                             " SANITY-FAIL"))
            failures++;
    }

    /* Every summary row, clean and then with each breach in turn. */
    selftest_clean_summary(&clean);
    for (int row = 0; row < SYN_ROW_COUNT; row++) {
        SynCheck check = summary_check(&clean, (SynRow)row);

        checks++;
        if (!selftest_expect("clean summary row", &check, false, false,
                             " ok"))
            failures++;
    }
    for (size_t which = 0U;; which++) {
        SynSummary s = clean;
        SynRow row = SYN_ROW_COUNT;
        SelftestKind kind;
        SynCheck check;
        char what[64];

        if (!selftest_breach(which, &s, &row, &kind))
            break;
        check = summary_check(&s, row);
        (void)snprintf(what, sizeof(what), "summary breach %lu",
                       (unsigned long)which);
        checks++;
        if (!selftest_expect(what, &check, true, kind != SELFTEST_TIMING,
                             kind == SELFTEST_TIMING ? " WARN" :
                             kind == SELFTEST_SANITY ? " SANITY-FAIL" :
                                                       " REGRESSION"))
            failures++;
    }

    if (failures != 0U) {
        (void)printf("perf-syn-policy: %lu of %lu checks wrong\n",
                     (unsigned long)failures, (unsigned long)checks);
        return 1;
    }
    (void)printf("perf-syn-policy: strict/advisory/sanity/correctness ok "
                 "(%lu checks)\n", (unsigned long)checks);
    return 0;
}

int main(int argc, char **argv)
{
    PerfSynGateMode gate_mode = PERF_SYN_GATE_FULL;

    if (!yew_perf_runtime_pin("perf_syn"))
        return 2;

    isolate_benchmark_logging();

    if (argc == 2 && strcmp(argv[1], "--gate-scroll-s56") == 0)
        return run_scroll_profile_gate();
    if (argc == 2 && strcmp(argv[1], "--prime-all-syntax") == 0)
        return prime_all_syntax();
    if (argc == 2 && strcmp(argv[1], "--warm-start-probe") == 0)
        return warm_start_probe();
    if (argc == 2 && strcmp(argv[1], "--detect-probe") == 0)
        return detect_probe();
    if (argc == 2 && strcmp(argv[1], "--selftest-gate") == 0)
        return selftest_gate();
    if (argc == 2 && strcmp(argv[1], "--selftest-policy") == 0)
        return selftest_policy();
    if (argc == 2 && strncmp(argv[1], "--probe-legacy-line=", 20U) == 0)
        return probe_legacy_line(argv[1] + 20U, false);
    if (argc == 2 && strncmp(argv[1], "--probe-resident-line=", 22U) == 0)
        return probe_legacy_line(argv[1] + 22U, true);
    if (argc == 2 && strncmp(argv[1], "--probe-legacy-edit=", 20U) == 0)
        return probe_legacy_edit(argv[1] + 20U);
    if (argc == 2 && strcmp(argv[1], "--gate-budgets") == 0)
        gate_mode = PERF_SYN_GATE_BUDGETS;
    else if (argc == 2 && strcmp(argv[1], "--gate") == 0)
        gate_mode = PERF_SYN_GATE_FULL;
    else if (argc != 1) {
        (void)fprintf(stderr,
                      "usage: perf_syn [--gate|--gate-budgets|"
                      "--gate-scroll-s56|"
                      "--selftest-gate|--selftest-policy|"
                      "--probe-legacy-line=STEM|"
                      "--probe-resident-line=STEM|"
                      "--probe-legacy-edit=STEM]\n");
        return 2;
    }

    PerfCase cases[PERF_SYN_CASE_COUNT];
    char case_names[PERF_SYN_FIXTURE_COUNT * 4U][PERF_SYN_CASE_NAME_CAP];
    Timing case_trials[YEW_ARRAY_LEN(cases)][PERF_SYN_TRIALS];
    Timing detect_trials[PERF_SYN_TRIALS];
    Timing compile_trials[PERF_SYN_TRIALS];
    Timing cache_trials[PERF_SYN_TRIALS];
    Timing block_trials[PERF_SYN_TRIALS];
    Timing block_multiline_trials[PERF_SYN_TRIALS];
    Timing make_embed_line_trials[PERF_SYN_TRIALS];
    Timing make_embed_view_trials[PERF_SYN_TRIALS];
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
    FrozenFixture plain_scroll;
    FrozenSpec plain_scroll_spec;
    ScrollProfile scroll_profile[PERF_SYN_SCROLL_PROFILE_CASES];
    size_t frozen_initialized = 0U;
    Source ini = {NULL, 0U};
    double scroll_fps[PERF_SYN_FIXTURE_COUNT];
    SynSummary r;
    bool regression_seen = false;
    bool advisory = yew_perf_advisory();
    int status = 0;

    (void)printf("perf_syn: mode %s\n", yew_perf_mode(advisory));
    yew_syn_discovery_set_bypass(true);
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
    (void)memset(make_embed_line_trials, 0,
                 sizeof(make_embed_line_trials));
    (void)memset(make_embed_view_trials, 0,
                 sizeof(make_embed_view_trials));
    (void)memset(frozen, 0, sizeof(frozen));
    (void)memset(&plain_scroll, 0, sizeof(plain_scroll));
    (void)memset(&plain_scroll_spec, 0, sizeof(plain_scroll_spec));
    (void)memset(scroll_profile, 0, sizeof(scroll_profile));
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
    if (!plain_scroll_fixture_init(&plain_scroll, &plain_scroll_spec)) {
        (void)fprintf(stderr,
                      "perf_syn: plain scroll fixture allocation failed\n");
        status = 2;
    }
    /* The original s41/s42 fixtures are the frozen non-resident path: their
     * unchanged baselines prove that adding embed-capable state does not tax
     * an unloaded guest.  The four s41.5 fixtures and all later fixtures are
     * the resident-guest lane and carry the embed workload and budgets. */
    for (size_t i = PERF_SYN_MD_EMBED_INDEX;
         i < PERF_SYN_FIXTURE_COUNT; i++) {
        u64 idle_ticks;
        u64 loads;
        u64 max_pump_ns;

        if (!prime_frozen_embeds(&frozen[i], &idle_ticks, &loads,
                                 &max_pump_ns)) {
            (void)fprintf(stderr, "perf_syn: embed prime '%s' failed\n",
                          frozen[i].spec->stem);
            status = 2;
            break;
        }
        if (i == PERF_SYN_MD_EMBED_INDEX) {
            r.md_embed_idle_ticks = idle_ticks;
            r.md_embed_loads = loads;
            r.md_embed_pump_max_ns = max_pump_ns;
            r.md_embed_states = yew_syn_state_count(
                yew_syn_engine_states(frozen[i].engine));
        }
    }
    if (status == 0 &&
        !measure_html_scan_pair(&frozen[PERF_SYN_HTML_EMBED_INDEX],
                                &r.html_embed_scan_ns,
                                &r.html_plain_scan_ns,
                                &r.html_scan_ratio_bp)) {
        (void)fprintf(stderr, "perf_syn: html inline scan pair failed\n");
        status = 2;
    }
    if (status == 0 &&
        !measure_definition_switch(&frozen[PERF_SYN_MD_EMBED_INDEX],
                                   &r.definition_switch_ns)) {
        (void)fprintf(stderr,
                      "perf_syn: definition-switch measurement failed\n");
        status = 2;
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
            if (trial_line_calls > r.theme_line_calls)
                r.theme_line_calls = trial_line_calls;
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
            if (trial_line_calls > r.block_line_calls)
                r.block_line_calls = trial_line_calls;
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
            if (trial_multiline_calls > r.block_multiline_calls)
                r.block_multiline_calls = trial_multiline_calls;
        }
    }
    if (status == 0) {
        for (size_t i = 0U; i < YEW_ARRAY_LEN(cases); i++)
            cases[i].measured = timing_of_trials(case_trials[i]);
        r.detect = timing_of_trials(detect_trials);
        r.compile = timing_of_trials(compile_trials);
        r.cache = timing_of_trials(cache_trials);
        r.block = timing_of_trials(block_trials);
        r.block_multiline = timing_of_trials(block_multiline_trials);
    }
    /* The legacy Make rows above retain their unchanged, guest-unloaded
     * baseline.  Prime that same engine only after those timings, then gate
     * the real Make -> shell resident path independently. */
    if (status == 0 &&
        !prime_frozen_embeds(&frozen[PERF_SYN_MAKE_INDEX],
                             &r.make_embed_idle_ticks, &r.make_embed_loads,
                             &r.make_embed_pump_max_ns)) {
        (void)fprintf(stderr, "perf_syn: Make resident prime failed\n");
        status = 2;
    }
    for (size_t trial = 0U; trial < PERF_SYN_TRIALS && status == 0;
         trial++) {
        if (!measure_frozen_line(&frozen[PERF_SYN_MAKE_INDEX], samples,
                                 count)) {
            (void)fprintf(stderr,
                          "perf_syn: Make resident line measurement failed\n");
            status = 2;
        } else {
            make_embed_line_trials[trial] = timing_of(samples, count);
        }
        if (status == 0 &&
            !measure_frozen_viewport(&frozen[PERF_SYN_MAKE_INDEX], 200U,
                                     samples, count)) {
            (void)fprintf(stderr,
                          "perf_syn: Make resident view measurement failed\n");
            status = 2;
        } else if (status == 0) {
            make_embed_view_trials[trial] = timing_of(samples, count);
        }
    }
    if (status == 0) {
        r.make_embed_line = timing_of_trials(make_embed_line_trials);
        r.make_embed_view = timing_of_trials(make_embed_view_trials);
    }
    if (status == 0 &&
        !measure_warm_start(argv[0], start_samples, start_count,
                            &r.warm_start_compiled_max)) {
        (void)fprintf(stderr, "perf_syn: warm-start measurement failed\n");
        status = 2;
    } else if (status == 0) {
        r.warm_start = timing_of(start_samples, start_count);
    }
    if (status == 0 &&
        !measure_clean_list(argv[0], start_samples, start_count,
                            &r.clean_list_compiled)) {
        (void)fprintf(stderr, "perf_syn: clean-list measurement failed\n");
        status = 2;
    } else if (status == 0) {
        r.clean_list = timing_of(start_samples, start_count);
    }
    if (status == 0 &&
        !measure_all_pack_loads(argv[0], &r.compile_all_cold_ns,
                                &r.all_warm_load_ns)) {
        (void)fprintf(stderr, "perf_syn: pack-load measurement failed\n");
        status = 2;
    }
    if (status == 0 && !measure_runtime_data_size(&r.runtime_data_bytes)) {
        (void)fprintf(stderr, "perf_syn: runtime-size measurement failed\n");
        status = 2;
    }

    if (status == 0 &&
        !check_comment_bomb(&frozen[1], &r.comment_first_max_us,
                            &r.comment_first_frames,
                            &r.comment_idle_total_us,
                            &r.comment_idle_max_us, &r.comment_idle_frames,
                            &r.comment_state_logical_bytes,
                            &r.comment_state_capacity_bytes,
                            &r.comment_state_rss_growth,
                            &r.comment_wall_ns)) {
        (void)fprintf(stderr, "perf_syn: comment-bomb frame check failed\n");
        status = 2;
    }
    if (status == 0 &&
        !measure_whole_settle(&frozen[PERF_SYN_JSON_INDEX], &r.whole_total_ns,
                              &r.whole_max_frame_ns, &r.whole_frames)) {
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
        !measure_scroll(&frozen[PERF_SYN_MARKDOWN_INDEX], true,
                        &r.markdown_wrap_fps)) {
        (void)fprintf(stderr, "perf_syn: markdown wrap measurement failed\n");
        status = 2;
    }
    if (status == 0 &&
        !measure_scroll_profiles(&frozen[PERF_SYN_MARKDOWN_INDEX],
                                 &plain_scroll, scroll_profile))
        status = 2;
    if (status == 0 &&
        !check_all_state_memory(frozen, &r.all_state_capacity_bytes,
                                &r.all_state_limit_bytes)) {
        (void)fprintf(stderr, "perf_syn: state memory measurement failed\n");
        status = 2;
    }

    if (status == 0 && gate_uses_baseline(gate_mode) &&
        !load_baselines(cases, YEW_ARRAY_LEN(cases)))
        status = 2;
    for (size_t i = 0U; status == 0 && i < YEW_ARRAY_LEN(cases); i++) {
        SynCheck check = case_check(i, &cases[i], gate_mode,
                                    r.theme_line_calls);

        (void)printf("syn.%-20s median_ns=%llu p99_ns=%llu%s\n",
                     cases[i].name,
                     (unsigned long long)cases[i].measured.median,
                     (unsigned long long)cases[i].measured.p99,
                     syn_check_verdict(&check, advisory));
        if (syn_check_failed(&check, advisory))
            regression_seen = true;
    }
    if (status != 2) {
        const char *verdict[SYN_ROW_COUNT];

        for (int i = 0; i < SYN_ROW_COUNT; i++) {
            SynCheck check = summary_check(&r, (SynRow)i);

            verdict[i] = syn_check_verdict(&check, advisory);
            if (syn_check_failed(&check, advisory))
                regression_seen = true;
        }
        (void)printf("syn.%-20s median_ns=%llu p99_ns=%llu%s\n",
                     "detect_10000",
                     (unsigned long long)r.detect.median,
                     (unsigned long long)r.detect.p99,
                     verdict[SYN_ROW_DETECT]);
        (void)printf("syn.%-20s median_ns=%llu p99_ns=%llu%s\n",
                     "ini_compile_cold",
                     (unsigned long long)r.compile.median,
                     (unsigned long long)r.compile.p99,
                     verdict[SYN_ROW_COMPILE]);
        (void)printf("syn.%-20s median_ns=%llu p99_ns=%llu%s\n",
                     "ini_cache_warm",
                     (unsigned long long)r.cache.median,
                     (unsigned long long)r.cache.p99,
                     verdict[SYN_ROW_CACHE]);
        (void)printf("syn.%-20s median_ns=%llu p99_ns=%llu "
                     "compiled_max=%llu%s\n", "all_defs_warm_start",
                     (unsigned long long)r.warm_start.median,
                     (unsigned long long)r.warm_start.p99,
                     (unsigned long long)r.warm_start_compiled_max,
                     verdict[SYN_ROW_WARM_START]);
        (void)printf("syn.%-20s median_ns=%llu p99_ns=%llu "
                     "compiled=%llu%s\n", "clean_list_48",
                     (unsigned long long)r.clean_list.median,
                     (unsigned long long)r.clean_list.p99,
                     (unsigned long long)r.clean_list_compiled,
                     verdict[SYN_ROW_CLEAN_LIST]);
        (void)printf("syn.%-20s total_ns=%llu%s\n", "compile_all_cold",
                     (unsigned long long)r.compile_all_cold_ns,
                     verdict[SYN_ROW_COMPILE_ALL]);
        (void)printf("syn.%-20s total_ns=%llu%s\n", "all_warm_loads",
                     (unsigned long long)r.all_warm_load_ns,
                     verdict[SYN_ROW_WARM_ALL]);
        (void)printf("syn.%-20s bytes=%llu limit=%llu%s\n",
                     "runtime_syntax_data",
                     (unsigned long long)r.runtime_data_bytes,
                     (unsigned long long)PERF_SYN_RUNTIME_LIMIT_BYTES,
                     verdict[SYN_ROW_RUNTIME_SIZE]);
        (void)printf("syn.%-20s median_ns=%llu p99_ns=%llu "
                     "line_calls_max=%llu%s\n",
                     "block_provider_64k",
                     (unsigned long long)r.block.median,
                     (unsigned long long)r.block.p99,
                     (unsigned long long)r.block_line_calls,
                     verdict[SYN_ROW_BLOCK]);
        (void)printf("syn.%-20s median_ns=%llu p99_ns=%llu "
                     "line_calls_max=%llu%s\n",
                     "block_multiline_100k",
                     (unsigned long long)r.block_multiline.median,
                     (unsigned long long)r.block_multiline.p99,
                     (unsigned long long)r.block_multiline_calls,
                     verdict[SYN_ROW_BLOCK_MULTILINE]);
        {
            for (size_t i = 0U; i < PERF_SYN_FIXTURE_COUNT; i++) {
                SynCheck fps = {false, false, false};

                syn_check_throughput(&fps, scroll_fps[i],
                                     PERF_SYN_SCROLL_MIN_FPS);
                (void)printf("syn.scroll_%-13s fps=%.2f%s\n",
                             frozen[i].spec->stem, scroll_fps[i],
                             syn_check_verdict(&fps, advisory));
                if (syn_check_failed(&fps, advisory))
                    regression_seen = true;
            }
            if (report_scroll_profiles(scroll_profile, advisory))
                regression_seen = true;
            {
                size_t worst_line = PERF_SYN_S42_5_FIRST;
                size_t worst_edit = PERF_SYN_S42_5_FIRST;
                size_t worst_view = PERF_SYN_S42_5_FIRST;
                size_t worst_scroll = PERF_SYN_S42_5_FIRST;

                for (size_t i = PERF_SYN_S42_5_FIRST + 1U;
                     i < PERF_SYN_FIXTURE_COUNT; i++) {
                    if (cases[CASE_FROZEN_LINE_FIRST + i].measured.p99 >
                        cases[CASE_FROZEN_LINE_FIRST + worst_line]
                            .measured.p99)
                        worst_line = i;
                    if (cases[CASE_FROZEN_EDIT_FIRST + i].measured.p99 >
                        cases[CASE_FROZEN_EDIT_FIRST + worst_edit]
                            .measured.p99)
                        worst_edit = i;
                    if (cases[CASE_VIEW_200_FIRST + i].measured.p99 >
                        cases[CASE_VIEW_200_FIRST + worst_view]
                            .measured.p99)
                        worst_view = i;
                    if (scroll_fps[i] < scroll_fps[worst_scroll])
                        worst_scroll = i;
                }
                (void)printf(
                    "syn.pack_worst           line=%s edit=%s "
                    "viewport=%s scroll=%s\n",
                    frozen_specs[worst_line].stem,
                    frozen_specs[worst_edit].stem,
                    frozen_specs[worst_view].stem,
                    frozen_specs[worst_scroll].stem);
            }

            (void)printf("syn.%-20s frames=%llu fake_max_frame_us=%llu%s\n",
                         "comment_view",
                         (unsigned long long)r.comment_first_frames,
                         (unsigned long long)r.comment_first_max_us,
                         verdict[SYN_ROW_COMMENT_VIEW]);
            (void)printf("syn.%-20s frames=%llu fake_total_us=%llu "
                         "fake_max_frame_us=%llu wall_ns=%llu%s\n",
                         "comment_idle",
                         (unsigned long long)r.comment_idle_frames,
                         (unsigned long long)r.comment_idle_total_us,
                         (unsigned long long)r.comment_idle_max_us,
                         (unsigned long long)r.comment_wall_ns,
                         verdict[SYN_ROW_COMMENT_IDLE]);
            (void)printf("syn.%-20s line_calls_max=%llu%s\n",
                         "theme_switch_calls",
                         (unsigned long long)r.theme_line_calls,
                         verdict[SYN_ROW_THEME_CALLS]);
            (void)printf("syn.%-20s wrap_fps=%.2f%s\n",
                         "markdown_scroll", r.markdown_wrap_fps,
                         verdict[SYN_ROW_MARKDOWN_SCROLL]);
            (void)printf("syn.%-20s frames=%llu total_ns=%llu "
                         "max_frame_ns=%llu%s\n", "whole_json_settle",
                         (unsigned long long)r.whole_frames,
                         (unsigned long long)r.whole_total_ns,
                         (unsigned long long)r.whole_max_frame_ns,
                         verdict[SYN_ROW_WHOLE_SETTLE]);
            (void)printf("syn.%-20s logical_bytes=%llu capacity_bytes=%llu "
                         "rss_growth_bytes=%llu%s\n", "comment_state",
                         (unsigned long long)r.comment_state_logical_bytes,
                         (unsigned long long)r.comment_state_capacity_bytes,
                         (unsigned long long)r.comment_state_rss_growth,
                         verdict[SYN_ROW_COMMENT_STATE]);
            (void)printf("syn.%-20s capacity_bytes=%llu limit_bytes=%llu%s\n",
                         "all_fixture_state",
                         (unsigned long long)r.all_state_capacity_bytes,
                         (unsigned long long)r.all_state_limit_bytes,
                         verdict[SYN_ROW_ALL_STATE]);
            (void)printf("syn.%-20s idle_ticks=%llu loads=%llu "
                         "states=%llu max_pump_ns=%llu%s\n",
                         "md_embed_pump",
                         (unsigned long long)r.md_embed_idle_ticks,
                         (unsigned long long)r.md_embed_loads,
                         (unsigned long long)r.md_embed_states,
                         (unsigned long long)r.md_embed_pump_max_ns,
                         verdict[SYN_ROW_MD_EMBED]);
            (void)printf("syn.%-20s line_median_ns=%llu line_p99_ns=%llu "
                         "view_p99_ns=%llu idle_ticks=%llu loads=%llu "
                         "max_pump_ns=%llu%s\n",
                         "make_embed_resident",
                         (unsigned long long)r.make_embed_line.median,
                         (unsigned long long)r.make_embed_line.p99,
                         (unsigned long long)r.make_embed_view.p99,
                         (unsigned long long)r.make_embed_idle_ticks,
                         (unsigned long long)r.make_embed_loads,
                         (unsigned long long)r.make_embed_pump_max_ns,
                         verdict[SYN_ROW_MAKE_EMBED]);
            (void)printf("syn.%-20s embedded_ns=%llu plain_ns=%llu "
                         "ratio_bp=%llu%s\n",
                         "html_inline_scan",
                         (unsigned long long)r.html_embed_scan_ns,
                         (unsigned long long)r.html_plain_scan_ns,
                         (unsigned long long)r.html_scan_ratio_bp,
                         verdict[SYN_ROW_HTML_SCAN]);
            (void)printf("syn.%-20s amortized_ns=%llu%s\n",
                         "definition_switch",
                         (unsigned long long)r.definition_switch_ns,
                         verdict[SYN_ROW_DEFINITION_SWITCH]);
        }
        status = regression_seen ? 1 : 0;
    }
    if (status == 2)
        (void)fprintf(stderr, "perf_syn: measurement failed\n");
    fixture_free(&fx);
    yew_textbuf_free(viewport);
    yew_textbuf_free(edit);
    for (size_t i = 0U; i < frozen_initialized; i++)
        frozen_free(&frozen[i]);
    free(plain_scroll.source.data);
    free(ini.data);
    free(cap_line);
    free(start_samples);
    free(samples);
    return status;
}
