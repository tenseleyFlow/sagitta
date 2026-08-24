/*
 * Sprint 33 deliverable 5: the Fletch bench suite and its gates.
 *
 *   make bench-fletch                 run, print the table
 *   make bench-fletch PERF_GATE=1     also enforce the gates
 *   make bench-fletch BASELINE=dev    which baseline to compare
 *
 * METHOD, pinned so the numbers mean the same thing every run:
 *
 *   - one warmup iteration, discarded; then ELEVEN measured runs.
 *   - the reported figure is the MEDIAN, with the min recorded too.
 *     Median, not mean: one scheduler hiccup should not move the
 *     number, and the mean is the metric that makes people add
 *     `sleep` to their CI.
 *   - GC stress OFF, and math.random's seed is the fixed default, so
 *     the inputs are identical across runs.
 *   - clock_gettime(CLOCK_MONOTONIC).  No rdtsc, no libc timer that
 *     varies by target.
 *
 * THE REGRESSION GATE TAKES TWO CONDITIONS: a bench fails only when
 * BOTH median > baseline_median x 1.15 AND min > baseline_min x 1.10.
 * One noisy sample must not redden the build, and a real regression
 * moves the whole distribution rather than its tail.
 *
 * The five HARD-GATED benches fail additionally and unconditionally
 * on their absolute budget whatever the baseline says --
 * 00-decisions.md makes budgets gates, so a baseline that drifted past
 * 1 ms is not a defence.
 */
#define _POSIX_C_SOURCE 200809L

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "edit/bind.h"
#include "edit/ed.h"
#include "edit/option.h"
#include "fl/compile.h"
#include "fl/flapi.h"
#include "fl/flconf.h"
#include "fl/gc.h"
#include "fl/handle.h"
#include "fl/fltxn.h"
#include "fl/macrolib.h"
#include "fl/opcodes.h"
#include "fl/parse.h"
#include "fl/std.h"
#include "fl/trace.h"
#include "fl/vm.h"
#include "util/arena.h"
#include "util/buf.h"
#include "util/intern.h"
#include "util/sort.h"

#ifndef FL_COMPUTED_GOTO
#  define FL_COMPUTED_GOTO 0
#endif

enum {
    WARMUP = 1,
    REPS = 11,
    /* The absolute budgets, from 02-fletch.md requirement 7 and the
     * 20 ms cold-start share in 00-decisions.md. */
    GATE_STARTUP_NS = 2000000,        /* 2 ms   */
    GATE_CONFIG_NS = 1000000,         /* 1 ms   */
    GATE_CONFIG_RELOAD_NS = 5000000,  /* 5 ms   */
    GATE_MACROLIB_SCAN_NS = 5000000,  /* 5 ms   */
    CONFIG_RELOAD_BINDS = 500,
    MACROLIB_FILES = 200,
    MOTION_ITERS = 1000000,
    GATE_MOTION_NS_PER_OP = 1000,     /* 1 us   */
    QUERY_ITERS = 1000000,
    GATE_QUERY_NS = 300000000,        /* 300 ms */
    INSERT_ITERS = 100000,
    /* §10 of the DoD: the quadratic path is DOCUMENTED, not fixed. */
    CONCAT_N = 5000,
    CONCAT_RATIO_MIN = 20
};

static u64 now_ns(void)
{
    struct timespec ts;

    (void)clock_gettime(CLOCK_MONOTONIC, &ts);
    return (u64)ts.tv_sec * 1000000000ULL + (u64)ts.tv_nsec;
}

/* ---------------------------------------------------------------- */
/* One VM, one program                                              */
/* ---------------------------------------------------------------- */

typedef struct Env {
    Arena arena;
    Interner in;
    DiagCtx dc;
    FlVm vm;
} Env;

static void quiet(void *ctx, FlDiagLevel level, FlSpan sp, const char *msg,
                  const char *rendered)
{
    (void)ctx;
    (void)level;
    (void)sp;
    (void)rendered;
    /* A bench that printed diagnostics would bury the table; a bench
     * whose program failed to compile must still be LOUD, so the
     * message is kept and reported by the caller. */
    (void)fprintf(stderr, "perf_fletch: %s\n", msg == NULL ? "?" : msg);
}

static void env_open(Env *e)
{
    (void)memset(e, 0, sizeof(*e));
    arena_init(&e->arena);
    interner_init(&e->in, &e->arena);
    fl_diag_init(&e->dc, &e->arena);
    fl_diag_set_sink(&e->dc, quiet, e);
    fl_vm_init(&e->vm, &e->arena, &e->in, &e->dc);
    fl_std_register(&e->vm);
}

static void env_close(Env *e)
{
    fl_vm_free(&e->vm);
    interner_free(&e->in);
    arena_free_all(&e->arena);
}

/* Compiles and runs `src` in a FRESH VM.  Returns nanoseconds for the
 * whole parse-compile-run, or 0 having complained. */
static u64 time_program(const char *name, const char *src, bool anchor_cli,
                        u64 *gc_pause_out)
{
    Env e;
    FlProgram p;
    FlFn *fn;
    FlValue out = FL_NIL_V;
    FlOrigin origin;
    u64 t0;
    u64 dt;

    (void)memset(&origin, 0, sizeof(origin));
    origin.kind = (u8)FL_ORIGIN_CLI;
    origin.caps = anchor_cli ? (u32)FL_CAP_FS_READ : 0U;

    env_open(&e);
    t0 = now_ns();
    (void)fl_diag_add_file(&e.dc, name, src, strlen(src));
    p = fl_parse(&e.arena, &e.dc, &e.in, src, strlen(src), 0U);
    if (p.had_error) {
        env_close(&e);
        (void)fprintf(stderr, "perf_fletch: %s did not parse\n", name);
        exit(1);
    }
    fn = fl_compile(&e.vm, &e.dc, &p, 0U, origin);
    if (fn == NULL) {
        env_close(&e);
        (void)fprintf(stderr, "perf_fletch: %s did not compile\n", name);
        exit(1);
    }
    if (!fl_vm_run(&e.vm, fn, &out)) {
        env_close(&e);
        (void)fprintf(stderr, "perf_fletch: %s raised\n", name);
        exit(1);
    }
    dt = now_ns() - t0;
    if (gc_pause_out != NULL)
        *gc_pause_out = e.vm.gc.pause_max_ns;
    env_close(&e);
    return dt;
}

/* Parse and AST-build only -- no compile, no run. */
static u64 time_parse_only(const char *name, const char *src)
{
    Env e;
    FlProgram p;
    u64 t0;
    u64 dt;

    env_open(&e);
    t0 = now_ns();
    (void)fl_diag_add_file(&e.dc, name, src, strlen(src));
    p = fl_parse(&e.arena, &e.dc, &e.in, src, strlen(src), 0U);
    dt = now_ns() - t0;
    if (p.had_error) {
        env_close(&e);
        (void)fprintf(stderr, "perf_fletch: %s did not parse\n", name);
        exit(1);
    }
    env_close(&e);
    return dt;
}

/* main -> VM initialized -> exit, on an empty script.  This is the
 * Fletch share of the 20 ms cold start: everything `yew` pays before
 * a config's first statement runs. */
static u64 time_startup(void)
{
    Env e;
    u64 t0 = now_ns();

    env_open(&e);
    {
        u64 dt = now_ns() - t0;

        env_close(&e);
        return dt;
    }
}

/* ---------------------------------------------------------------- */
/* The programs                                                     */
/* ---------------------------------------------------------------- */

static const char SRC_FIB27[] =
    "fn fib(n) {\n"
    "  if n < 2 { return n }\n"
    "  return fib(n - 1) + fib(n - 2)\n"
    "}\n"
    "return fib(27)\n";

/*
 * MOTION DISPATCH against fl_host_null.
 *
 * PITFALL, and the bench file is where it belongs: this measures the
 * VM's share of a motion op -- dispatch, the host seam, the raise and
 * the unwind -- and NOT editing.  It is the right number for
 * 02-fletch.md requirement 7 and the wrong number to quote as
 * "yew executes a motion in N ns".  Sprint 34 adds a second row
 * against the real host, which is the one that answers that question.
 */
static const char SRC_MOTION[] =
    "let i = 0\n"
    "while i < 1000000 {\n"
    "  try { let x = @[ > ] } catch e { }\n"
    "  i = i + 1\n"
    "}\n"
    "return i\n";

/*
 * list.push, NOT `parts = parts + [x]`.
 *
 * The concatenating form allocates and copies a whole new list every
 * iteration, so building the fragments that way is quadratic and
 * dwarfs the str.join this bench exists to measure -- the number
 * would be a list benchmark wearing a string benchmark's name, and
 * the concat/join ratio below would read 1.0x for the wrong reason.
 */
static const char SRC_JOIN[] =
    "import str\n"
    "import list\n"
    "let parts = []\n"
    "let i = 0\n"
    "while i < 20000 {\n"
    "  list.push(parts, \"frag\")\n"
    "  i = i + 1\n"
    "}\n"
    "return str.len(str.join(parts, \"\"))\n";

static const char SRC_CONCAT[] =
    "import str\n"
    "let s = \"\"\n"
    "let i = 0\n"
    "while i < 5000 {\n"
    "  s = s + \"frag\"\n"
    "  i = i + 1\n"
    "}\n"
    "return str.len(s)\n";

/* The join twin at the SAME n, so the ratio below compares like with
 * like rather than 20 000 against 5 000. */
static const char SRC_JOIN_5K[] =
    "import str\n"
    "import list\n"
    "let parts = []\n"
    "let i = 0\n"
    "while i < 5000 {\n"
    "  list.push(parts, \"frag\")\n"
    "  i = i + 1\n"
    "}\n"
    "return str.len(str.join(parts, \"\"))\n";

/*
 * STRING keys, not int keys.
 *
 * An int key allocates nothing, so a 100 000-entry map built from
 * ints never triggers a collection and `gc_pause` reports 0 -- a
 * vacuous number sitting next to a real budget, which is worse than
 * no number at all.  String keys are also what a config's maps
 * actually hold.
 */
static const char SRC_MAP_HEAVY[] =
    "import map\n"
    "import fmt\n"
    "let m = {}\n"
    "let i = 0\n"
    "while i < 100000 {\n"
    "  map.set(m, fmt.int(i), i)\n"
    "  i = i + 1\n"
    "}\n"
    "let t = 0\n"
    "i = 0\n"
    "while i < 100000 {\n"
    "  t = t + map.get(m, fmt.int(i))\n"
    "  i = i + 1\n"
    "}\n"
    "return t\n";

static const char SRC_LIST_SORT[] =
    "import list\n"
    "import math\n"
    "math.seed(1)\n"
    "let l = []\n"
    "let i = 0\n"
    "while i < 100000 {\n"
    "  list.push(l, math.random(0, 1000000))\n"
    "  i = i + 1\n"
    "}\n"
    "list.sort(l)\n"
    "return list.len(l)\n";

/* ---------------------------------------------------------------- */
/* Timing                                                           */
/* ---------------------------------------------------------------- */

typedef struct Bench {
    const char *name;
    u64 median;
    u64 min;
    bool hard;             /* has an absolute budget                */
    /*
     * Recorded, never compared.  gc_pause is a single collection's
     * worst case rather than a throughput figure, and it swings by 2x
     * run to run on an idle machine -- regression-gating it would flap
     * the build weekly and teach everyone to ignore the perf lane.
     * Sprint 56 makes it a gate on stable hardware, where a p99 over
     * many more runs is meaningful.
     */
    bool report_only;
    u64 budget;            /* ns; 0 when hard is false              */
    const char *note;
} Bench;

static int cmp_u64(const void *a, const void *b, void *ctx)
{
    u64 x = *(const u64 *)a;
    u64 y = *(const u64 *)b;

    (void)ctx;
    return x < y ? -1 : (x > y ? 1 : 0);
}

typedef u64 (*Sample)(void *ud);

static void measure(Bench *b, Sample fn, void *ud)
{
    u64 v[REPS];
    int i;

    for (i = 0; i < WARMUP; i++)
        (void)fn(ud);
    for (i = 0; i < REPS; i++)
        v[i] = fn(ud);
    /* yew_sort_stable, not qsort: raw qsort is banned tree-wide
     * because its tie order is unspecified and invariant 5 is not. */
    yew_sort_stable(v, (size_t)REPS, sizeof(v[0]), cmp_u64, NULL);
    b->median = v[REPS / 2];
    b->min = v[0];
}

typedef struct ProgArg {
    const char *name;
    const char *src;
    u64 gc_pause;
} ProgArg;

static u64 sample_prog(void *ud)
{
    ProgArg *a = ud;
    u64 pause = 0U;
    u64 dt = time_program(a->name, a->src, false, &pause);

    a->gc_pause = pause;
    return dt;
}

/* The worst collection pause during ONE run of the program.  Sampled
 * like any other figure so its median and min mean what they do in
 * every other row, rather than being one run's outlier twice. */
static u64 sample_gc_pause(void *ud)
{
    ProgArg *a = ud;
    u64 pause = 0U;

    (void)time_program(a->name, a->src, false, &pause);
    return pause;
}

static u64 sample_parse(void *ud)
{
    ProgArg *a = ud;

    return time_parse_only(a->name, a->src);
}

static u64 sample_startup(void *ud)
{
    (void)ud;
    return time_startup();
}

typedef struct ConfigArg {
    const char *user_path;
} ConfigArg;

static u64 sample_config_reload(void *ud)
{
    ConfigArg *arg = ud;
    YewEdStartup startup = {0};
    Ed ed;
    CfgStatus status;
    u64 t0;
    u64 dt;

    startup.config_path = arg->user_path;
    startup.no_workspace_config = true;
    yew_ed_init(&ed);
    if (!yew_ed_open_scratch(&ed)) {
        (void)fprintf(stderr, "perf_fletch: cannot open config scratch\n");
        exit(1);
    }
    yew_config_init(&ed, &startup);
    status = yew_config_load_all(&ed, NULL);
    if (status != YEW_CFG_OK ||
        yew_bind_active_count(&ed) < CONFIG_RELOAD_BINDS) {
        (void)fprintf(stderr,
                      "perf_fletch: 500-bind config did not load\n");
        exit(1);
    }
    t0 = now_ns();
    status = yew_config_reload(&ed, NULL);
    dt = now_ns() - t0;
    if (status != YEW_CFG_OK) {
        (void)fprintf(stderr, "perf_fletch: config reload failed\n");
        exit(1);
    }
    yew_ed_free(&ed);
    return dt;
}

static void make_reload_fixture(char *path)
{
    static const char source[] =
        "let first = [\"a\", \"b\", \"c\", \"d\", \"e\", \"f\", \"g\", "
        "\"h\", \"i\", \"j\", \"k\", \"l\", \"m\", \"n\", \"o\", \"p\", "
        "\"q\", \"r\", \"s\", \"t\"]\n"
        "let second = [\"a\", \"b\", \"c\", \"d\", \"e\", \"f\", \"g\", "
        "\"h\", \"i\", \"j\", \"k\", \"l\", \"m\", \"n\", \"o\", \"p\", "
        "\"q\", \"r\", \"s\", \"t\", \"u\", \"v\", \"w\", \"x\", \"y\"]\n"
        "for a in first {\n"
        "  for b in second {\n"
        "    bind(\"L\", \"z \" + a + \" \" + b, \"ed.nop\")\n"
        "  }\n"
        "}\n";
    int fd = mkstemp(path);
    FILE *fp;

    if (fd < 0) {
        (void)fprintf(stderr, "perf_fletch: cannot create reload fixture\n");
        exit(1);
    }
    fp = fdopen(fd, "wb");
    if (fp == NULL) {
        (void)close(fd);
        (void)unlink(path);
        (void)fprintf(stderr, "perf_fletch: cannot open reload fixture\n");
        exit(1);
    }
    if (fwrite(source, 1U, sizeof(source) - 1U, fp) != sizeof(source) - 1U) {
        (void)fclose(fp);
        (void)unlink(path);
        (void)fprintf(stderr, "perf_fletch: cannot write reload fixture\n");
        exit(1);
    }
    if (fclose(fp) != 0) {
        (void)unlink(path);
        (void)fprintf(stderr, "perf_fletch: cannot close reload fixture\n");
        exit(1);
    }
}

typedef struct MacroLibArg {
    Ed ed;
    Arena diag_arena;
    DiagCtx dc;
    char root[160];
    bool ed_initialized;
    bool arena_initialized;
} MacroLibArg;

static void macrolib_path(const MacroLibArg *arg, u32 index, char *path,
                          size_t cap)
{
    (void)snprintf(path, cap, "%s/macro%03u.fl", arg->root,
                   (unsigned)index);
}

static void macrolib_fixture_close(MacroLibArg *arg)
{
    u32 i;

    if (arg->ed_initialized)
        yew_ed_free(&arg->ed);
    if (arg->arena_initialized)
        arena_free_all(&arg->diag_arena);
    for (i = 0U; i < MACROLIB_FILES; i++) {
        char path[256];

        macrolib_path(arg, i, path, sizeof(path));
        (void)unlink(path);
    }
    (void)rmdir(arg->root);
}

static void macrolib_fixture_open(MacroLibArg *arg)
{
    static const char source[] = "fn run() { return 1 }\n";
    OptVal value = {0};
    const char *err = NULL;
    u32 i;

    (void)memset(arg, 0, sizeof(*arg));
    (void)snprintf(arg->root, sizeof(arg->root),
                   "/tmp/yew-perf-macrolib.XXXXXX");
    if (mkdtemp(arg->root) == NULL) {
        (void)fprintf(stderr,
                      "perf_fletch: cannot create macro-library fixture\n");
        exit(1);
    }
    for (i = 0U; i < MACROLIB_FILES; i++) {
        char path[256];
        FILE *fp;
        bool wrote;
        bool closed;

        macrolib_path(arg, i, path, sizeof(path));
        fp = fopen(path, "wb");
        if (fp == NULL) {
            macrolib_fixture_close(arg);
            (void)fprintf(stderr,
                          "perf_fletch: cannot write macro-library fixture\n");
            exit(1);
        }
        wrote = fwrite(source, 1U, sizeof(source) - 1U, fp) ==
                sizeof(source) - 1U;
        closed = fclose(fp) == 0;
        if (!wrote || !closed) {
            macrolib_fixture_close(arg);
            (void)fprintf(stderr,
                          "perf_fletch: cannot write macro-library fixture\n");
            exit(1);
        }
    }
    arena_init(&arg->diag_arena);
    arg->arena_initialized = true;
    fl_diag_init(&arg->dc, &arg->diag_arena);
    fl_diag_set_sink(&arg->dc, quiet, arg);
    yew_ed_init(&arg->ed);
    arg->ed_initialized = true;
    if (!yew_ed_open_scratch(&arg->ed)) {
        macrolib_fixture_close(arg);
        (void)fprintf(stderr, "perf_fletch: cannot open macro scratch\n");
        exit(1);
    }
    value.type = (u8)YEW_OPT_STR;
    value.as.str.s = arg->root;
    value.as.str.len = (u32)strlen(arg->root);
    if (!yew_opt_set(&arg->ed, YEW_OPT_GLOBAL, "macro.dir", 9U, &value,
                     &err)) {
        macrolib_fixture_close(arg);
        (void)fprintf(stderr, "perf_fletch: cannot set macro.dir: %s\n",
                      err == NULL ? "?" : err);
        exit(1);
    }
}

static u64 sample_macrolib_scan(void *ud)
{
    MacroLibArg *arg = ud;
    u64 t0 = now_ns();
    u32 loaded = yew_macrolib_scan(&arg->ed, &arg->dc);
    u64 dt = now_ns() - t0;

    if (loaded != MACROLIB_FILES) {
        (void)fprintf(stderr,
                      "perf_fletch: macro scan loaded %u of %u files\n",
                      (unsigned)loaded, (unsigned)MACROLIB_FILES);
        macrolib_fixture_close(arg);
        exit(1);
    }
    return dt;
}

/* ---------------------------------------------------------------- */
/* Live editor API                                                  */
/* ---------------------------------------------------------------- */

typedef struct EditorEnv {
    Env fl;
    Ed ed;
    FlValue buf;
    FlValue cur;
} EditorEnv;

static void editor_env_open(EditorEnv *e)
{
    env_open(&e->fl);
    yew_ed_init(&e->ed);
    if (!yew_ed_open_scratch(&e->ed)) {
        (void)fprintf(stderr, "perf_fletch: cannot open scratch editor\n");
        exit(1);
    }
    fl_ed_attach(&e->fl.vm, &e->ed, &fl_host_editor);
    fl_api_init();
    e->buf = fl_h_buf_make(&e->ed, yew_ed_doc(&e->ed));
    e->cur = fl_h_cur_make(&e->ed, e->ed.win, e->ed.win->cs.primary);
}

static void editor_env_close(EditorEnv *e)
{
    fl_ed_detach(&e->fl.vm);
    yew_ed_free(&e->ed);
    env_close(&e->fl);
}

static void editor_fail(EditorEnv *e, const char *msg, u32 iteration)
{
    Bytebuf rendered;

    (void)fprintf(stderr, "perf_fletch: %s at %u\n", msg,
                  (unsigned)iteration);
    bytebuf_init(&rendered);
    fl_trace_render(&e->fl.vm, e->fl.vm.err, &rendered);
    if (rendered.len != 0U)
        (void)fwrite(rendered.data, 1U, rendered.len, stderr);
    bytebuf_free(&rendered);
    exit(1);
}

static u64 sample_config_program(void *ud)
{
    ProgArg *arg = ud;
    EditorEnv e;
    FlProgram program;
    FlOrigin origin;
    FlFn *fn;
    FlValue out = FL_NIL_V;
    u64 t0;
    u64 dt;

    editor_env_open(&e);
    origin = (FlOrigin){(u8)FL_ORIGIN_BUILTIN,
                        yew_intern(&e.fl.in, arg->name, strlen(arg->name)),
                        FL_CAP_ALL};
    yew_bind_batch_begin(&e.ed);
    t0 = now_ns();
    (void)fl_diag_add_file(&e.fl.dc, arg->name, arg->src, strlen(arg->src));
    program = fl_parse(&e.fl.arena, &e.fl.dc, &e.fl.in, arg->src,
                       strlen(arg->src), 0U);
    fn = program.had_error ? NULL :
         fl_compile(&e.fl.vm, &e.fl.dc, &program, 0U, origin);
    if (fn == NULL || !fl_vm_run(&e.fl.vm, fn, &out))
        editor_fail(&e, "runtime/init.fl failed", 0U);
    yew_bind_batch_end(&e.ed);
    dt = now_ns() - t0;
    editor_env_close(&e);
    return dt;
}

static u64 sample_query_cur_pos(void *ud)
{
    EditorEnv e;
    const FlBindDesc *query;
    FlValue out = FL_NIL_V;
    u64 t0;
    u32 i;

    (void)ud;
    editor_env_open(&e);
    query = fl_api_find("cur.pos", 7U);
    if (query == NULL) {
        (void)fprintf(stderr, "perf_fletch: cur.pos binding is missing\n");
        exit(1);
    }
    t0 = now_ns();
    for (i = 0U; i < QUERY_ITERS; i++) {
        if (!fl_api_invoke(&e.fl.vm, query, &e.cur, 1U, &out) ||
            out.t != (u8)FL_INT) {
            (void)fprintf(stderr, "perf_fletch: cur.pos query failed\n");
            exit(1);
        }
    }
    {
        u64 dt = now_ns() - t0;

        editor_env_close(&e);
        return dt;
    }
}

static u64 sample_insert_100k(void *ud)
{
    EditorEnv e;
    const FlBindDesc *insert;
    FlValue args[3];
    FlValue out = FL_NIL_V;
    u64 t0;
    u64 dt;
    u32 i;

    (void)ud;
    editor_env_open(&e);
    insert = fl_api_find("buf.insert", 10U);
    if (insert == NULL) {
        (void)fprintf(stderr, "perf_fletch: buf.insert binding is missing\n");
        exit(1);
    }
    args[0] = e.buf;
    args[2] = FL_OBJ_V(FL_STR, fl_str_new(&e.fl.vm, "x", 1U));
    fl_gc_protect(&e.fl.vm, args[2]);
    if (!e.fl.vm.host->run_begin(&e.fl.vm)) {
        (void)fprintf(stderr, "perf_fletch: insert transaction refused\n");
        exit(1);
    }
    t0 = now_ns();
    for (i = 0U; i < INSERT_ITERS; i++) {
        args[1] = FL_INT_V((i64)i);
        if (!fl_api_invoke(&e.fl.vm, insert, args, 3U, &out))
            editor_fail(&e, "buf.insert failed", i);
    }
    if (!e.fl.vm.host->run_end(&e.fl.vm, true)) {
        (void)fprintf(stderr, "perf_fletch: insert transaction did not close\n");
        exit(1);
    }
    dt = now_ns() - t0;
    if (yew_textbuf_len(yew_ed_doc(&e.ed)->tb) != INSERT_ITERS ||
        yew_ed_doc(&e.ed)->undo->nodes.len != 2U) {
        (void)fprintf(stderr,
                      "perf_fletch: 100k inserts were not one undo node\n");
        exit(1);
    }
    if (yew_ed_doc(&e.ed)->undo->bytes_live >
        yew_ed_doc(&e.ed)->undo->bytes_max) {
        (void)fprintf(stderr,
                      "perf_fletch: 100k inserts exceeded undo.bytes_max\n");
        exit(1);
    }
    fl_gc_release(&e.fl.vm, 1U);
    editor_env_close(&e);
    return dt;
}

static u64 time_editor_program(const char *name, const char *src)
{
    EditorEnv e;
    FlProgram p;
    FlFn *fn;
    FlValue out = FL_NIL_V;
    FlOrigin origin;
    u64 t0;
    u64 dt;

    (void)memset(&origin, 0, sizeof(origin));
    origin.kind = (u8)FL_ORIGIN_CLI;
    editor_env_open(&e);
    (void)fl_diag_add_file(&e.fl.dc, name, src, strlen(src));
    p = fl_parse(&e.fl.arena, &e.fl.dc, &e.fl.in, src, strlen(src), 0U);
    fn = p.had_error ? NULL : fl_compile(&e.fl.vm, &e.fl.dc, &p, 0U,
                                         origin);
    if (fn == NULL) {
        (void)fprintf(stderr, "perf_fletch: %s did not compile\n", name);
        exit(1);
    }
    t0 = now_ns();
    if (!fl_vm_run(&e.fl.vm, fn, &out)) {
        (void)fprintf(stderr, "perf_fletch: %s raised against editor\n",
                      name);
        exit(1);
    }
    dt = now_ns() - t0;
    editor_env_close(&e);
    return dt;
}

static u64 sample_editor_motion(void *ud)
{
    ProgArg *a = ud;

    return time_editor_program(a->name, a->src);
}

/* ---------------------------------------------------------------- */
/* Baselines                                                        */
/* ---------------------------------------------------------------- */

typedef struct BaseRow {
    char name[32];
    u64 median;
    u64 min;
    char why[128];
} BaseRow;

static BaseRow g_base[32];
static size_t g_nbase;

static bool load_baseline(const char *path)
{
    FILE *f = fopen(path, "r");
    char line[512];

    if (f == NULL)
        return false;
    while (fgets(line, (int)sizeof(line), f) != NULL) {
        char name[32];
        unsigned long long med;
        unsigned long long mn;
        int used = 0;

        if (line[0] == '#' || line[0] == '\n')
            continue;
        if (sscanf(line, "%31s %llu %llu %n", name, &med, &mn, &used) < 3)
            continue;
        if (g_nbase == YEW_ARRAY_LEN(g_base))
            break;
        (void)snprintf(g_base[g_nbase].name, sizeof(g_base[0].name), "%s",
                       name);
        g_base[g_nbase].median = (u64)med;
        g_base[g_nbase].min = (u64)mn;
        {
            char *nl;

            (void)snprintf(g_base[g_nbase].why, sizeof(g_base[0].why), "%s",
                           used > 0 ? line + used : "");
            nl = strchr(g_base[g_nbase].why, '\n');
            if (nl != NULL)
                *nl = '\0';
        }
        g_nbase++;
    }
    (void)fclose(f);
    return true;
}

/*
 * THE GATE'S RULE, in one place so it can be tested without a
 * stopwatch: a bench fails only when the median is more than 15% over
 * its baseline AND the min is more than 10% over.
 *
 * Integer arithmetic, so the thresholds are exact rather than
 * dependent on how a double rounds: +15% is +3/20 and +10% is +1/10.
 */
static bool regressed(u64 median, u64 min, u64 base_median, u64 base_min)
{
    return median > base_median + base_median / 20U * 3U &&
           min > base_min + base_min / 10U;
}

/*
 * DoD 9, proven WITHOUT MEASURING ANYTHING.
 *
 * A selftest that re-runs the benches and compares them against a
 * scaled baseline tests the machine's stability, not the gate: on a
 * box doing anything else, parse_only alone swings 80%, and the "5%
 * wobble must pass" case then fails for a reason that has nothing to
 * do with the rule under test.  Feeding known pairs through the rule
 * is what actually pins the arithmetic.
 */
static int selftest_gate(void)
{
    static const struct {
        u64 base_med;
        u64 base_min;
        u64 med;
        u64 min;
        bool want;
        const char *what;
    } CASES[] = {
        /* A seeded 20% slowdown fails: past both thresholds. */
        {1000000U, 900000U, 1200000U, 1080000U, true, "20% slower"},
        /* A seeded 5% wobble passes: past neither. */
        {1000000U, 900000U, 1050000U,  945000U, false, "5% slower"},
        /* Exactly at each threshold is NOT a regression -- the test is
         * strictly greater-than, so a bench sitting on its budget does
         * not flap in and out. */
        {1000000U, 900000U, 1150000U,  990000U, false, "exactly +15/+10"},
        /* ONE condition alone is never enough, in either direction:
         * that is the whole point of taking two. */
        {1000000U, 900000U, 1300000U,  905000U, false, "median only"},
        {1000000U, 900000U, 1010000U, 1200000U, false, "min only"},
        /* Faster than the baseline is never a failure. */
        {1000000U, 900000U,  500000U,  450000U, false, "twice as fast"},
        /* Both well past: the unambiguous regression. */
        {1000000U, 900000U, 2000000U, 1800000U, true, "twice as slow"}
    };
    size_t i;
    size_t bad = 0U;

    for (i = 0U; i < YEW_ARRAY_LEN(CASES); i++) {
        bool got = regressed(CASES[i].med, CASES[i].min, CASES[i].base_med,
                             CASES[i].base_min);

        if (got != CASES[i].want) {
            (void)printf("FAIL gate rule: %s -> %s, wanted %s\n",
                         CASES[i].what, got ? "regression" : "ok",
                         CASES[i].want ? "regression" : "ok");
            bad++;
        }
    }
    if (bad != 0U) {
        (void)printf("perf_fletch: %lu gate-rule cases wrong\n",
                     (unsigned long)bad);
        return 1;
    }
    (void)printf("perf_fletch: the two-condition gate rule behaves "
                 "(%lu cases)\n", (unsigned long)YEW_ARRAY_LEN(CASES));
    return 0;
}

static const BaseRow *baseline_for(const char *name)
{
    size_t i;

    for (i = 0U; i < g_nbase; i++) {
        if (strcmp(g_base[i].name, name) == 0)
            return &g_base[i];
    }
    return NULL;
}

/* ---------------------------------------------------------------- */

static Bench g_b[16];
static size_t g_nb;

static Bench *add(const char *name, bool hard, u64 budget, const char *note)
{
    if (g_nb == YEW_ARRAY_LEN(g_b))
        exit(2);
    (void)memset(&g_b[g_nb], 0, sizeof(g_b[0]));
    g_b[g_nb].name = name;
    g_b[g_nb].hard = hard;
    g_b[g_nb].budget = budget;
    g_b[g_nb].note = note;
    return &g_b[g_nb++];
}

int main(int argc, char **argv)
{
    const char *baseline = "dev";
    bool gate = false;
    bool budgets_only = false;
    int argi;
    size_t i;
    size_t failures = 0U;
    char bpath[512];
    u64 concat_median = 0U;
    u64 join5k_median = 0U;

    for (argi = 1; argi < argc; argi++) {
        if (strcmp(argv[argi], "--gate") == 0) {
            gate = true;
        } else if (strcmp(argv[argi], "--gate-budgets") == 0) {
            /*
             * The ABSOLUTE budgets only, with no baseline comparison.
             *
             * Every lane can run this: the hard gates have enough
             * headroom on the dev machine that a slower CI runner
             * still clears them, whereas comparing a runner's timings
             * against dev-machine baselines fails on hardware rather
             * than on a regression.  The full two-condition gate runs
             * on the designated perf runner, and Sprint 56 recalibrates
             * every row on the reference hardware.
             */
            gate = true;
            budgets_only = true;
        } else if (strcmp(argv[argi], "--selftest-gate") == 0) {
            return selftest_gate();
        } else if (strcmp(argv[argi], "--baseline") == 0 &&
                   argi + 1 < argc) {
            baseline = argv[++argi];
        } else {
            (void)fprintf(stderr,
                          "usage: perf_fletch [--gate|--gate-budgets] "
                          "[--baseline NAME]\n");
            return 2;
        }
    }
    if (getenv("FL_GC_STRESS") != NULL ||
        getenv("YEW_FL_GC_STRESS") != NULL) {
        /* The stress lane runs the collector on every allocation, so a
         * timing taken under it means nothing.  Refusing is better than
         * publishing a number 30x too slow next to a budget. */
        (void)fprintf(stderr,
                      "perf_fletch: Fletch GC stress is set; these "
                      "numbers would be meaningless\n");
        return 2;
    }

    {
        ProgArg a;
        ConfigArg config;
        Bench *b;
        char reload_path[] = "/tmp/yew-perf-config.XXXXXX";

        b = add("startup", true, (u64)GATE_STARTUP_NS,
                "fl_vm_init + fl_std_register");
        measure(b, sample_startup, NULL);

        {
            char *src;
            size_t len = 0U;
            FILE *f = fopen("runtime/init.fl", "rb");
            Bytebuf bb;

            if (f == NULL) {
                (void)fprintf(stderr, "perf_fletch: cannot read "
                                      "runtime/init.fl\n");
                return 2;
            }
            bytebuf_init(&bb);
            for (;;) {
                char buf[65536];
                size_t n = fread(buf, 1U, sizeof(buf), f);

                if (n != 0U)
                    bytebuf_append(&bb, buf, n);
                if (n != sizeof(buf))
                    break;
            }
            (void)fclose(f);
            len = bb.len;
            src = malloc(len + 1U);
            if (src == NULL)
                return 2;
            (void)memcpy(src, bb.data, len);
            src[len] = '\0';
            bytebuf_free(&bb);

            if (setenv("YEW_RUNTIME_DIR", "runtime", 1) != 0) {
                free(src);
                (void)fprintf(stderr,
                              "perf_fletch: cannot select runtime directory\n");
                return 2;
            }
            a.name = "runtime/init.fl";
            a.src = src;
            a.gc_pause = 0U;
            b = add("config_load", true, (u64)GATE_CONFIG_NS,
                    "parse + compile + run of shipped runtime/init.fl");
            measure(b, sample_config_program, &a);

            b = add("parse_only", false, 0U, "parse + AST build, same file");
            measure(b, sample_parse, &a);
            free(src);
        }

        make_reload_fixture(reload_path);
        config.user_path = reload_path;
        b = add("config_reload_500", true, (u64)GATE_CONFIG_RELOAD_NS,
                "real reload with 500 user binds plus shipped defaults");
        measure(b, sample_config_reload, &config);
        (void)unlink(reload_path);

        {
            MacroLibArg macrolib;

            macrolib_fixture_open(&macrolib);
            b = add("macrolib_scan_200", true,
                    (u64)GATE_MACROLIB_SCAN_NS,
                    "rescan + compile 200 valid macro files");
            measure(b, sample_macrolib_scan, &macrolib);
            macrolib_fixture_close(&macrolib);
        }

        a.name = "motion";
        a.src = SRC_MOTION;
        a.gc_pause = 0U;
        b = add("motion_dispatch", true,
                (u64)GATE_MOTION_NS_PER_OP * (u64)MOTION_ITERS,
                "1e6 FL_OP_MOTION against fl_host_null; NOT editing");
        measure(b, sample_prog, &a);

        a.name = "motion_editor";
        a.src = SRC_MOTION;
        b = add("motion_editor", true,
                (u64)GATE_MOTION_NS_PER_OP * (u64)MOTION_ITERS,
                "1e6-word motion block against a live scratch editor");
        measure(b, sample_editor_motion, &a);

        b = add("query_cur_pos", true, (u64)GATE_QUERY_NS,
                "1e6 cur.pos() calls against a live cursor handle");
        measure(b, sample_query_cur_pos, NULL);

        b = add("insert_100k", false, 0U,
                "100k buf.insert calls; one undo node and bytes in budget");
        b->report_only = true;
        b->median = sample_insert_100k(NULL);
        b->min = b->median;

        a.name = "fib27";
        a.src = SRC_FIB27;
        b = add("fib27", false, 0U, "call/return, frames, int arithmetic");
        measure(b, sample_prog, &a);

        a.name = "join";
        a.src = SRC_JOIN;
        b = add("strbuild_join", false, 0U, "20k fragments, one str.join");
        measure(b, sample_prog, &a);

        a.name = "concat";
        a.src = SRC_CONCAT;
        b = add("strbuild_concat", false, 0U,
                "the naive s = s + x loop, n=5000");
        measure(b, sample_prog, &a);
        concat_median = b->median;

        a.name = "join5k";
        a.src = SRC_JOIN_5K;
        {
            Bench tmp;

            (void)memset(&tmp, 0, sizeof(tmp));
            measure(&tmp, sample_prog, &a);
            join5k_median = tmp.median;
        }

        a.name = "map_heavy";
        a.src = SRC_MAP_HEAVY;
        a.gc_pause = 0U;
        b = add("map_heavy", false, 0U, "100k inserts then 100k lookups");
        measure(b, sample_prog, &a);

        a.name = "list_sort";
        a.src = SRC_LIST_SORT;
        b = add("list_sort", false, 0U, "list.sort over 100k random ints");
        measure(b, sample_prog, &a);

        a.name = "map_heavy";
        a.src = SRC_MAP_HEAVY;
        b = add("gc_pause", false, 0U,
                "worst single collection during map_heavy; recorded only");
        b->report_only = true;
        measure(b, sample_gc_pause, &a);
    }

    (void)snprintf(bpath, sizeof(bpath), "tests/perf/baselines/fletch_%s.txt",
                   baseline);
    (void)load_baseline(bpath);

    (void)printf("# bench            median_ns   min_ns    why\n");
    for (i = 0U; i < g_nb; i++) {
        (void)printf("%-18s %9llu %9llu    %s\n", g_b[i].name,
                     (unsigned long long)g_b[i].median,
                     (unsigned long long)g_b[i].min, g_b[i].note);
    }
    (void)printf("# cgoto=%d  reps=%d (median of %d, warmup %d discarded)\n",
                 FL_COMPUTED_GOTO, REPS, REPS, WARMUP);
    {
        /* DoD 10: the quadratic path is documented, not fixed. */
        double ratio = join5k_median == 0U
                           ? 0.0
                           : (double)concat_median / (double)join5k_median;

        (void)printf("# concat/join at n=%d: %.1fx (documented O(n^2); "
                     "asserted > %dx)\n", CONCAT_N, ratio,
                     CONCAT_RATIO_MIN);
        if (gate && ratio <= (double)CONCAT_RATIO_MIN) {
            (void)printf("FAIL concat/join is %.1fx, expected > %dx -- "
                         "either str.join regressed or `s = s + x` was "
                         "quietly made linear; both need a look\n",
                         ratio, CONCAT_RATIO_MIN);
            failures++;
        }
    }

    if (!gate) {
        (void)printf("# not gated (pass --gate / PERF_GATE=1 to enforce)\n");
        return 0;
    }

    for (i = 0U; i < g_nb; i++) {
        const Bench *b = &g_b[i];
        const BaseRow *base;

        /* The absolute budget first, and UNCONDITIONALLY: a baseline
         * that drifted past the budget is not a defence. */
        if (b->hard && b->median > b->budget) {
            (void)printf("FAIL %s: %llu ns exceeds the %llu ns budget "
                         "(02-fletch.md req 7 / 00-decisions.md)\n", b->name,
                         (unsigned long long)b->median,
                         (unsigned long long)b->budget);
            failures++;
        }
        if (budgets_only || b->report_only)
            continue;
        base = baseline_for(b->name);
        if (base == NULL) {
            (void)printf("FAIL %s: no row in %s (add one with a `why`)\n",
                         b->name, bpath);
            failures++;
            continue;
        }
        /* TWO CONDITIONS.  One noisy sample must not redden the build,
         * and a genuine regression moves the whole distribution. */
        if (regressed(b->median, b->min, base->median, base->min)) {
            (void)printf("FAIL %s: median %llu ns vs baseline %llu (+15%%) "
                         "AND min %llu vs %llu (+10%%)\n", b->name,
                         (unsigned long long)b->median,
                         (unsigned long long)base->median,
                         (unsigned long long)b->min,
                         (unsigned long long)base->min);
            failures++;
        }
    }

    if (failures != 0U) {
        (void)printf("perf_fletch: %lu failures\n", (unsigned long)failures);
        return 1;
    }
    (void)printf("perf_fletch: all gates green\n");
    return 0;
}
