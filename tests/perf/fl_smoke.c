/*
 * Sprint 30 DoD 5 and DoD 12: the Fletch differential-dispatch driver
 * and the perf smoke.
 *
 * One tool for both, because they share a corpus and disagreeing about
 * what "every fixture" means is how the two gates drift apart.
 *
 *   fl_smoke --trace   the executed-opcode trace of every fixture, as
 *                      hex.  Built with FL_VM_TRACE=1 under each
 *                      FL_CGOTO setting and byte-compared; that
 *                      comparison IS the differential-dispatch gate.
 *   fl_smoke --perf    wall-clock numbers for the three programs the
 *                      sprint names.  Numbers only -- s33 owns the
 *                      gates, and this exists so s33 starts from
 *                      measured ground rather than from a guess.
 *
 * The trace mode prints no timings and the perf mode prints no trace:
 * a timing in a byte-compared file makes the comparison fail on every
 * run, which is the fastest way to get a gate switched off.
 */
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "fl/compile.h"
#include "fl/gc.h"
#include "fl/opcodes.h"
#include "fl/parse.h"
#include "fl/vm.h"
#include "util/arena.h"
#include "util/intern.h"

/* Both flags default to off, and the driver reports which build it is
 * rather than assuming: a parity run that silently compared two
 * identical builds would pass forever. */
#ifndef FL_COMPUTED_GOTO
#  define FL_COMPUTED_GOTO 0
#endif
#ifndef FL_VM_TRACE
#  define FL_VM_TRACE 0
#endif

typedef struct Fixture {
    const char *name;
    const char *src;
} Fixture;

#if FL_VM_TRACE
/*
 * The trace corpus.  Every construct that compiles to a distinct opcode
 * sequence, kept small enough that a divergence is readable in the diff
 * rather than buried in a megabyte of loop iterations.
 *
 * Compiled only into the tracing build: without it the array is dead,
 * and -Werror=unused-const-variable is right to say so.
 */
static const Fixture TRACE_FIXTURES[] = {
    {"arith",     "return 1 + 2 * 3 - 4 / 2 % 3\n"},
    {"compare",   "return 1 < 2\n"},
    {"floats",    "return 1 + 2.5\n"},
    {"strings",   "let a = \"ab\" + \"cd\"\nreturn a == \"abcd\"\n"},
    {"truthy",    "if 0 { return 1 }\nreturn 2\n"},
    {"shortcut",  "let a = nil or 3\nlet b = a and 4\nreturn b\n"},
    {"ifelse",    "if false { return 1 } else { return 2 }\n"},
    {"whileloop", "let n = 0\nwhile n < 5 { n = n + 1 }\nreturn n\n"},
    {"forlist",   "let t = 0\nfor x in [1,2,3] { t = t + x }\nreturn t\n"},
    {"formap",    "let t = 0\nfor k, v in {a:1, b:2} { t = t + v }\n"
                  "return t\n"},
    {"breakcont", "let t = 0\n"
                  "for x in [1,2,3,4] { if x == 2 { continue }\n"
                  "if x == 4 { break }\nt = t + x }\nreturn t\n"},
    {"globals",   "let g = 1\ng = g + 1\nreturn g\n"},
    {"locals",    "fn f() { let a = 1\nlet b\na = a + 2\nreturn a }\n"
                  "return f()\n"},
    {"closure",   "fn c(s) { let n = s\nreturn fn() { n = n + 1\n"
                  "return n } }\nlet nx = c(8)\nlet a = nx()\n"
                  "return nx()\n"},
    {"recursion", "fn fib(n) { if n < 2 { return n }\n"
                  "return fib(n - 1) + fib(n - 2) }\nreturn fib(10)\n"},
    {"listconcat","let l = []\nfor x in [1,2,3] { l = l + [x] }\n"
                  "return l[2]\n"},
    {"indexing",  "let l = [1,2]\nl[0] = 9\nlet m = {a: 1}\nm.a = 2\n"
                  "return l[0] + m.a\n"},
    {"trycatch",  "try { return 1 / 0 } catch e { return 7 }\n"},
    {"trynoraise","try { return 5 } catch e { return 9 }\n"},
    {"motion",    "try { edit { @[ 2v i\"!\" del ] } }\n"
                  "catch e { return 1 }\nreturn 0\n"},
    {"spec14",    "fn clamp(x, lo, hi) {\n"
                  "    if x < lo { return lo }\n"
                  "    else if x > hi { return hi }\n"
                  "    return x\n"
                  "}\n"
                  "fn counter(start) {\n"
                  "    let n = start\n"
                  "    return fn() { n = n + 1; return n }\n"
                  "}\n"
                  "let next = counter(clamp(9, 0, 8))\n"
                  "while next() < 10 { }\n"
                  "let nums = [1, 2.5, 0x10]\n"
                  "let total = 0\n"
                  "for x in nums {\n"
                  "    if x == 2.5 { continue }\n"
                  "    total = total + x\n"
                  "}\n"
                  "return total\n"}
};
#endif /* FL_VM_TRACE */

/* The three programs the sprint names for the perf smoke. */
static const Fixture PERF_FIXTURES[] = {
    {"fib24",     "fn fib(n) { if n < 2 { return n }\n"
                  "return fib(n - 1) + fib(n - 2) }\nreturn fib(24)\n"},
    {"map10k",    "let m = {}\nlet i = 0\n"
                  "while i < 10000 { m[i] = i\ni = i + 1 }\nreturn i\n"},
    {"motion100k","let i = 0\n"
                  "while i < 100000 {\n"
                  "  try { edit { @[ 2v ] } } catch e { }\n"
                  "  i = i + 1\n"
                  "}\nreturn i\n"}
};

typedef struct Run {
    Arena arena;
    Interner in;
    DiagCtx dc;
    FlVm vm;
} Run;

static void quiet(void *ctx, FlDiagLevel level, FlSpan sp,
                  const char *msg, const char *rendered)
{
    (void)ctx;
    (void)level;
    (void)sp;
    (void)rendered;
    (void)fprintf(stderr, "diag: %s\n", msg);
}

static void run_open(Run *r)
{
    (void)memset(r, 0, sizeof(*r));
    arena_init(&r->arena);
    interner_init(&r->in, &r->arena);
    fl_diag_init(&r->dc, &r->arena);
    fl_diag_set_sink(&r->dc, quiet, NULL);
    fl_vm_init(&r->vm, &r->arena, &r->in, &r->dc);
}

static void run_close(Run *r)
{
    fl_vm_free(&r->vm);
    interner_free(&r->in);
    arena_free_all(&r->arena);
}

/* Compiles and runs one fixture; returns false only if it failed to
 * compile, since several fixtures raise on purpose. */
static bool run_fixture(Run *r, const Fixture *f, FlValue *out)
{
    FlProgram p;
    FlFn *fn;
    FlOrigin origin = {0U, 0U};
    size_t n = strlen(f->src);

    (void)fl_diag_add_file(&r->dc, f->name, f->src, n);
    p = fl_parse(&r->arena, &r->dc, &r->in, f->src, n, 0U);
    if (p.had_error)
        return false;
    fn = fl_compile(&r->vm, &r->dc, &p, 0U, origin);
    if (fn == NULL)
        return false;
    (void)fl_vm_run(&r->vm, fn, out);
    return true;
}

/* Prints a value in a form that is stable across runs and machines --
 * no pointers, no addresses, no float formatting beyond what the
 * fixtures actually produce. */
static void print_value(FlValue v)
{
    switch ((FlType)v.t) {
    case FL_NIL:   (void)printf("nil"); return;
    case FL_BOOL:  (void)printf("%s", v.as.b ? "true" : "false"); return;
    case FL_INT:   (void)printf("%lld", (long long)v.as.i); return;
    case FL_FLOAT: (void)printf("%.17g", v.as.f); return;
    case FL_STR: {
        const FlStr *s = (const FlStr *)v.as.o;

        (void)printf("\"%.*s\"", (int)s->len, s->b);
        return;
    }
    default:
        (void)printf("<%s>", fl_type_name((FlType)v.t));
        return;
    }
}

static int do_trace(void)
{
#if !FL_VM_TRACE
    (void)fprintf(stderr,
                  "fl_smoke: built without FL_VM_TRACE=1, so there is no "
                  "trace to print.\n");
    return 2;
#else
    size_t i;

    (void)printf("# fletch executed-opcode traces\n");
    (void)printf("# dispatch: %s\n",
                 FL_COMPUTED_GOTO ? "computed-goto" : "switch");
    (void)printf("#\n");
    (void)printf("# DoD 5: this file must be BYTE-IDENTICAL between the\n");
    (void)printf("# two dispatch modes, the header line above excepted --\n");
    (void)printf("# it is the one line that is allowed to differ, and the\n");
    (void)printf("# make target strips it before comparing.\n");
    for (i = 0U; i < sizeof(TRACE_FIXTURES) / sizeof(*TRACE_FIXTURES);
         i++) {
        Run r;
        FlValue out = FL_NIL_V;
        size_t k;

        run_open(&r);
        if (!run_fixture(&r, &TRACE_FIXTURES[i], &out)) {
            (void)fprintf(stderr, "fl_smoke: %s failed to compile\n",
                          TRACE_FIXTURES[i].name);
            run_close(&r);
            return 1;
        }
        (void)printf("\n== %s (%u ops) -> ", TRACE_FIXTURES[i].name,
                     (unsigned)r.vm.trace.len);
        print_value(out);
        (void)printf("\n");
        for (k = 0U; k < r.vm.trace.len; k++) {
            (void)printf("%s%s", fl_op_name((FlOp)r.vm.trace.data[k]),
                         (k + 1U) % 6U == 0U ? "\n" : " ");
        }
        if (r.vm.trace.len % 6U != 0U)
            (void)printf("\n");
        run_close(&r);
    }
    return 0;
#endif
}

static double now_ms(void)
{
    struct timespec ts;

    (void)clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1.0e6;
}

static int do_perf(int reps)
{
    size_t i;

    (void)printf("# fletch perf smoke -- NUMBERS ONLY, s33 owns the "
                 "gates\n");
    (void)printf("# dispatch: %s\n",
                 FL_COMPUTED_GOTO ? "computed-goto" : "switch");
    (void)printf("# reps: %d (best of, to drop scheduler noise)\n", reps);
    for (i = 0U; i < sizeof(PERF_FIXTURES) / sizeof(*PERF_FIXTURES); i++) {
        double best = -1.0;
        int rep;
        u64 collections = 0U;
        FlValue out = FL_NIL_V;

        for (rep = 0; rep < reps; rep++) {
            Run r;
            double t0;
            double dt;

            run_open(&r);
            t0 = now_ms();
            if (!run_fixture(&r, &PERF_FIXTURES[i], &out)) {
                (void)fprintf(stderr, "fl_smoke: %s failed to compile\n",
                              PERF_FIXTURES[i].name);
                run_close(&r);
                return 1;
            }
            dt = now_ms() - t0;
            collections = r.vm.gc.collections;
            if (best < 0.0 || dt < best)
                best = dt;
            run_close(&r);
        }
        (void)printf("%-12s %8.3f ms   gc=%llu   -> ",
                     PERF_FIXTURES[i].name, best,
                     (unsigned long long)collections);
        print_value(out);
        (void)printf("\n");
    }
    return 0;
}

int main(int argc, char **argv)
{
    int reps = 5;

    if (argc >= 2 && strcmp(argv[1], "--trace") == 0)
        return do_trace();
    if (argc >= 2 && strcmp(argv[1], "--perf") == 0) {
        if (argc >= 3)
            reps = atoi(argv[2]);
        if (reps < 1)
            reps = 1;
        return do_perf(reps);
    }
    (void)fprintf(stderr, "usage: fl_smoke --trace | --perf [reps]\n");
    return 2;
}
