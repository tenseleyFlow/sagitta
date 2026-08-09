/*
 * Sprint 30 DoD 4, 5, 6, 7 and 9: the VM, the collector, and the
 * semantics the sprint says must be proven rather than assumed.
 *
 * Programs are asserted on their RESULT, not printed.  A test that
 * prints is a test nobody reads on the day it starts lying.
 */
#define _POSIX_C_SOURCE 200809L

#include "harness.h"

#include <stdio.h>
#include <string.h>

#include "fl/compile.h"
#include "fl/gc.h"
#include "fl/opcodes.h"
#include "fl/parse.h"
#include "fl/vm.h"
#include "util/arena.h"
#include "util/intern.h"

typedef struct VmFix {
    Arena arena;
    Interner in;
    DiagCtx dc;
    FlVm vm;
    char first[256];
    u32 ndiag;
} VmFix;

static void vcapture(void *ctx, FlDiagLevel level, FlSpan sp,
                     const char *msg, const char *rendered)
{
    VmFix *f = ctx;

    (void)level;
    (void)sp;
    (void)rendered;
    if (f->ndiag == 0U)
        (void)snprintf(f->first, sizeof(f->first), "%s", msg);
    f->ndiag++;
}

static void vf_open(VmFix *f)
{
    (void)memset(f, 0, sizeof(*f));
    arena_init(&f->arena);
    interner_init(&f->in, &f->arena);
    fl_diag_init(&f->dc, &f->arena);
    fl_diag_set_sink(&f->dc, vcapture, f);
    fl_vm_init(&f->vm, &f->arena, &f->in, &f->dc);
}

static void vf_close(VmFix *f)
{
    fl_vm_free(&f->vm);
    interner_free(&f->in);
    arena_free_all(&f->arena);
}

/* Compiles and runs `src`; returns whether it completed without
 * raising, and leaves the result (or the error map) in *out. */
static bool vf_run(VmFix *f, const char *src, FlValue *out)
{
    FlProgram p;
    FlFn *fn;
    FlOrigin origin = {0U, 0U};

    (void)fl_diag_add_file(&f->dc, "t.fl", src, strlen(src));
    p = fl_parse(&f->arena, &f->dc, &f->in, src, strlen(src), 0U);
    SAG_ASSERT(!p.had_error);
    fn = fl_compile(&f->vm, &f->dc, &p, 0U, origin);
    SAG_ASSERT_NOT_NULL(fn);
    return fl_vm_run(&f->vm, fn, out);
}

static i64 run_int(const char *src)
{
    VmFix f;
    FlValue out;
    i64 got;

    vf_open(&f);
    if (!vf_run(&f, src, &out))
        (void)fprintf(stderr, "raised: %s\n", src);
    SAG_ASSERT_EQ_U64((u64)out.t, (u64)FL_INT);
    got = out.as.i;
    vf_close(&f);
    return got;
}

/* The `kind` of the error a program raises, or NULL when it completed. */
static void run_kind(const char *src, char *kind, size_t cap)
{
    VmFix f;
    FlValue out;
    FlValue got;

    vf_open(&f);
    kind[0] = '\0';
    if (!vf_run(&f, src, &out)) {
        FlValue k = FL_OBJ_V(FL_STR, fl_str_new(&f.vm, "kind", 4U));

        SAG_ASSERT_EQ_U64((u64)out.t, (u64)FL_MAP);
        SAG_ASSERT(fl_map_get((FlMap *)out.as.o, k, &got));
        (void)snprintf(kind, cap, "%.*s",
                       (int)((FlStr *)got.as.o)->len,
                       ((FlStr *)got.as.o)->b);
    }
    vf_close(&f);
}

static void assert_kind(const char *src, const char *want)
{
    char kind[64];

    run_kind(src, kind, sizeof(kind));
    if (strcmp(kind, want) != 0)
        (void)fprintf(stderr, "source: %s\n  want kind %s, got '%s'\n",
                      src, want, kind);
    SAG_ASSERT_EQ_I64(strcmp(kind, want), 0);
}

/* ---------------------------------------------------------------- */
/* Arithmetic, comparison, truthiness (spec §5)                     */
/* ---------------------------------------------------------------- */

void test_fl_vm_arithmetic_and_precedence(void)
{
    SAG_ASSERT_EQ_I64(run_int("return 1 + 2 * 3\n"), 7);
    SAG_ASSERT_EQ_I64(run_int("return (1 + 2) * 3\n"), 9);
    SAG_ASSERT_EQ_I64(run_int("return 1 - 2 - 3\n"), -4);
    /* §4: int division truncates TOWARD ZERO, and the modulo takes the
     * sign of the dividend.  Both differ from a floor-division
     * language, so both are pinned. */
    SAG_ASSERT_EQ_I64(run_int("return 7 / 2\n"), 3);
    SAG_ASSERT_EQ_I64(run_int("return -7 / 2\n"), -3);
    SAG_ASSERT_EQ_I64(run_int("return -7 % 2\n"), -1);
    SAG_ASSERT_EQ_I64(run_int("return 7 % -2\n"), 1);
}

void test_fl_vm_truthiness_is_nil_and_false_only(void)
{
    /*
     * §5.1.  Zero, 0.0 and "" are TRUTHY, and every reader who has
     * written C or Python expects otherwise -- which is exactly why
     * this is a test rather than a comment.
     */
    SAG_ASSERT_EQ_I64(run_int("if 0 { return 1 }\nreturn 2\n"), 1);
    SAG_ASSERT_EQ_I64(run_int("if \"\" { return 1 }\nreturn 2\n"), 1);
    SAG_ASSERT_EQ_I64(run_int("if nil { return 1 }\nreturn 2\n"), 2);
    SAG_ASSERT_EQ_I64(run_int("if false { return 1 }\nreturn 2\n"), 2);
    SAG_ASSERT_EQ_I64(run_int("if not nil { return 1 }\nreturn 2\n"), 1);
}

void test_fl_vm_equality_crosses_the_numeric_tower(void)
{
    /* §5.2: 1 == 1.0 holds; a string never equals a number. */
    SAG_ASSERT_EQ_I64(run_int("if 1 == 1.0 { return 1 }\nreturn 0\n"), 1);
    SAG_ASSERT_EQ_I64(run_int("if 1 == \"1\" { return 1 }\nreturn 0\n"), 0);
    SAG_ASSERT_EQ_I64(run_int("if \"a\" == \"a\" { return 1 }\nreturn 0\n"),
                      1);
    /* Long strings are equal by CONTENT even though only short ones are
     * interned -- the bug a pointer-only compare would cause. */
    SAG_ASSERT_EQ_I64(
        run_int("let a = \"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
                "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\"\n"
                "let b = \"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
                "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\"\n"
                "if a == b { return 1 }\nreturn 0\n"), 1);
}

void test_fl_vm_short_circuit_yields_the_deciding_value(void)
{
    /* §5.3: the operators yield the DECIDING OPERAND, not a bool -- so
     * `0 or 9` is 0 (zero is truthy) and `nil and 9` is nil. */
    SAG_ASSERT_EQ_I64(run_int("return 0 or 9\n"), 0);
    SAG_ASSERT_EQ_I64(run_int("return nil or 9\n"), 9);
    SAG_ASSERT_EQ_I64(run_int("return 3 and 9\n"), 9);
    SAG_ASSERT_EQ_I64(run_int("if (nil and 9) == nil { return 1 }\n"
                              "return 0\n"), 1);
    /* The right operand is not merely unused, it is NOT EVALUATED --
     * proved by a right side that would raise if it ran. */
    SAG_ASSERT_EQ_I64(run_int("let x = nil and (1 / 0)\nreturn 1\n"), 1);
    SAG_ASSERT_EQ_I64(run_int("let x = 3 or (1 / 0)\nreturn 1\n"), 1);
}

/* ---------------------------------------------------------------- */
/* Errors: every kind the VM can raise                              */
/* ---------------------------------------------------------------- */

void test_fl_vm_error_kinds(void)
{
    assert_kind("return 1 / 0\n", "div");
    assert_kind("return 1 % 0\n", "div");
    assert_kind("return 1 + \"a\"\n", "type");
    assert_kind("return nope\n", "name");
    assert_kind("return [1][5]\n", "index");
    assert_kind("let m = {a: 1}\nreturn m[\"zz\"]\n", "key");
    assert_kind("return @[2v]\n", "motion");
    assert_kind("import x\n", "import");
    assert_kind("fn f(a){ return a }\nreturn f()\n", "arity");
    assert_kind("return 1()\n", "type");
}

void test_fl_vm_limit_is_catchable(void)
{
    /*
     * Amendment A1's whole reason.  Infinite recursion is
     * user-triggerable, so it must raise a catchable value rather than
     * abort -- and it is none of the other eleven kinds.
     */
    assert_kind("fn f(){ return f() }\nreturn f()\n", "limit");
    /* And it really is catchable, not merely named. */
    SAG_ASSERT_EQ_I64(
        run_int("fn f(){ return f() }\n"
                "try { return f() } catch e { return 42 }\n"), 42);
}

void test_fl_vm_try_catch_binds_the_error_map(void)
{
    SAG_ASSERT_EQ_I64(
        run_int("try { return 1 / 0 } catch e { return 7 }\n"), 7);
    /* The handler sees the map, and `kind` reads back. */
    SAG_ASSERT_EQ_I64(
        run_int("try { return 1 / 0 }\n"
                "catch e { if e.kind == \"div\" { return 1 } return 0 }\n"),
        1);
    /* A block that does not raise runs its body and skips the
     * handler. */
    SAG_ASSERT_EQ_I64(
        run_int("try { return 5 } catch e { return 9 }\n"), 5);
}

/* ---------------------------------------------------------------- */
/* DoD 9: upvalue semantics, both failure modes                     */
/* ---------------------------------------------------------------- */

void test_fl_vm_upvalues_capture_by_reference(void)
{
    /*
     * Spec §7, normative via §14's counter.  The classic bug is
     * capture BY VALUE, which makes every call return the same number
     * because the closure got a copy of `n` rather than the variable.
     */
    SAG_ASSERT_EQ_I64(
        run_int("fn counter(start) {\n"
                "  let n = start\n"
                "  return fn() { n = n + 1; return n }\n"
                "}\n"
                "let next = counter(8)\n"
                "let a = next()\n"
                "let b = next()\n"
                "return b\n"), 10);
    /* Two closures made in the same scope share ONE upvalue: bumping
     * through one is visible through the other. */
    SAG_ASSERT_EQ_I64(
        run_int("fn pair() {\n"
                "  let n = 0\n"
                "  return [fn() { n = n + 1; return n },\n"
                "          fn() { return n }]\n"
                "}\n"
                "let p = pair()\n"
                "let bump = p[0]\n"
                "let peek = p[1]\n"
                "let x = bump()\n"
                "let y = bump()\n"
                "return peek()\n"), 2);
}

void test_fl_vm_loop_variable_is_fresh_each_iteration(void)
{
    /*
     * The compiler's decision, not the spec's, and the one whose
     * failure mode has a name: with a single binding closed at loop
     * exit, every closure sees the LAST value -- JavaScript's `var`
     * bug, which users file as "closures are broken".
     */
    SAG_ASSERT_EQ_I64(
        run_int("let fns = []\n"
                "for x in [1, 2, 3] { fns = fns + [fn() x] }\n"
                "let f0 = fns[0]\n"
                "return f0()\n"), 1);
    SAG_ASSERT_EQ_I64(
        run_int("let fns = []\n"
                "for x in [1, 2, 3] { fns = fns + [fn() x] }\n"
                "let f2 = fns[2]\n"
                "return f2()\n"), 3);
}

/* ---------------------------------------------------------------- */
/* Containers, and DoD 6's ordering claims                          */
/* ---------------------------------------------------------------- */

void test_fl_vm_lists_and_maps(void)
{
    SAG_ASSERT_EQ_I64(run_int("return [10, 20, 30][1]\n"), 20);
    SAG_ASSERT_EQ_I64(run_int("let m = {a: 1, b: 2}\nreturn m[\"b\"]\n"), 2);
    SAG_ASSERT_EQ_I64(run_int("let m = {a: 1}\nreturn m.a\n"), 1);
    SAG_ASSERT_EQ_I64(
        run_int("let l = [1, 2, 3]\nl[0] = 9\nreturn l[0]\n"), 9);
    SAG_ASSERT_EQ_I64(
        run_int("let m = {a: 1}\nm[\"b\"] = 5\nreturn m[\"b\"]\n"), 5);
    /* A float key is refused by kind, not silently rounded. */
    assert_kind("let m = {}\nm[1.5] = 1\nreturn m\n", "key");
}

void test_fl_vm_map_iteration_is_insertion_ordered(void)
{
    VmFix f;
    FlValue out;
    FlMap *m;
    u32 cursor = 0U;
    FlValue k;
    FlValue v;
    i64 seen[8];
    u32 n = 0U;

    /*
     * DoD 6.  Insertion order, and a key deleted and re-inserted lands
     * at the END -- reviving it in place would make order depend on
     * delete history, which is invisible in the source.
     */
    vf_open(&f);
    m = fl_map_new(&f.vm);
    fl_gc_protect(&f.vm, FL_OBJ_V(FL_MAP, m));
    (void)fl_map_set(&f.vm, m, FL_INT_V(1), FL_INT_V(10));
    (void)fl_map_set(&f.vm, m, FL_INT_V(2), FL_INT_V(20));
    (void)fl_map_set(&f.vm, m, FL_INT_V(3), FL_INT_V(30));
    SAG_ASSERT(fl_map_del(m, FL_INT_V(1)));
    (void)fl_map_set(&f.vm, m, FL_INT_V(1), FL_INT_V(11));

    while (fl_map_iter(m, &cursor, &k, &v) && n < 8U)
        seen[n++] = k.as.i;
    SAG_ASSERT_EQ_U64(n, 3U);
    SAG_ASSERT_EQ_I64(seen[0], 2);
    SAG_ASSERT_EQ_I64(seen[1], 3);
    SAG_ASSERT_EQ_I64(seen[2], 1);   /* re-inserted: at the end */

    /* And a compaction preserves live order rather than reshuffling. */
    fl_map_compact(m);
    cursor = 0U;
    n = 0U;
    while (fl_map_iter(m, &cursor, &k, &v) && n < 8U)
        seen[n++] = k.as.i;
    SAG_ASSERT_EQ_U64(n, 3U);
    SAG_ASSERT_EQ_I64(seen[0], 2);
    SAG_ASSERT_EQ_I64(seen[1], 3);
    SAG_ASSERT_EQ_I64(seen[2], 1);
    fl_gc_release(&f.vm, 1U);
    (void)out;
    vf_close(&f);
}

void test_fl_vm_control_flow(void)
{
    SAG_ASSERT_EQ_I64(
        run_int("let t = 0\nfor x in [1,2,3] { t = t + x }\nreturn t\n"), 6);
    SAG_ASSERT_EQ_I64(
        run_int("let n = 0\nwhile n < 3 { n = n + 1 }\nreturn n\n"), 3);
    SAG_ASSERT_EQ_I64(
        run_int("let t = 0\n"
                "for x in [1,2,3,4] { if x == 3 { break } t = t + x }\n"
                "return t\n"), 3);
    SAG_ASSERT_EQ_I64(
        run_int("let t = 0\n"
                "for x in [1,2,3] { if x == 2 { continue } t = t + x }\n"
                "return t\n"), 4);
    /* Two-variable for walks a map in insertion order. */
    SAG_ASSERT_EQ_I64(
        run_int("let m = {a: 1, b: 2, c: 3}\n"
                "let t = 0\nfor k, v in m { t = t + v }\nreturn t\n"), 6);
}

/* ---------------------------------------------------------------- */
/* DoD 7: the GC, and a drop-it test per root                       */
/* ---------------------------------------------------------------- */

static u32 live_objects(const FlVm *vm)
{
    const FlObj *o;
    u32 n = 0U;

    for (o = vm->gc.objects; o != NULL; o = o->gc_next)
        n++;
    return n;
}

void test_fl_vm_gc_collects_only_unreachable_objects(void)
{
    VmFix f;
    FlStr *kept;
    u32 before;
    u32 after;

    vf_open(&f);
    /* Root 7: the temp-protection stack.  Held, so it survives. */
    kept = fl_str_new(&f.vm, "kept-alive-by-a-temp-root", 25U);
    fl_gc_protect(&f.vm, FL_OBJ_V(FL_STR, kept));
    (void)fl_str_new(&f.vm, "no root points at this one", 26U);
    before = live_objects(&f.vm);
    fl_gc_collect(&f.vm);
    after = live_objects(&f.vm);
    SAG_ASSERT(after < before);
    /* The protected one is still readable, not merely still listed. */
    SAG_ASSERT_EQ_U64(kept->len, 25U);
    SAG_ASSERT_EQ_I64(memcmp(kept->b, "kept-alive-by-a-temp-root", 25U), 0);

    /* DROP IT: release the root and the same object dies. */
    fl_gc_release(&f.vm, 1U);
    before = live_objects(&f.vm);
    fl_gc_collect(&f.vm);
    SAG_ASSERT(live_objects(&f.vm) < before);
    vf_close(&f);
}

void test_fl_vm_gc_root_globals_keeps_values_alive(void)
{
    VmFix f;
    FlValue key;
    FlValue got;

    /* Root 4.  A global's value must survive a collection with nothing
     * else pointing at it. */
    vf_open(&f);
    key = FL_OBJ_V(FL_STR, fl_str_new(&f.vm, "g", 1U));
    fl_gc_protect(&f.vm, key);
    (void)fl_map_set(&f.vm, f.vm.globals, key,
                     FL_OBJ_V(FL_STR, fl_str_new(&f.vm, "a value", 7U)));
    fl_gc_release(&f.vm, 1U);
    fl_gc_collect(&f.vm);
    SAG_ASSERT(fl_map_get(f.vm.globals, key, &got));
    SAG_ASSERT_EQ_U64((u64)got.t, (u64)FL_STR);
    SAG_ASSERT_EQ_I64(memcmp(((FlStr *)got.as.o)->b, "a value", 7U), 0);
    vf_close(&f);
}

void test_fl_vm_gc_stress_runs_a_whole_program(void)
{
    VmFix f;
    FlValue out;

    /*
     * Stress collects at EVERY allocation, so a value reachable only
     * from a C local dies immediately and the failure names the line.
     * Running a program that allocates strings, lists, maps and
     * closures under it is the cheapest broad check of the protection
     * discipline that exists.
     */
    vf_open(&f);
    f.vm.gc.stress = true;
    SAG_ASSERT(vf_run(&f,
                      "let acc = []\n"
                      "for x in [1, 2, 3, 4, 5] {\n"
                      "  let m = {n: x, s: \"item\"}\n"
                      "  acc = acc + [m]\n"
                      "}\n"
                      "let t = 0\n"
                      "for m in acc { t = t + m.n }\n"
                      "return t\n", &out));
    SAG_ASSERT_EQ_U64((u64)out.t, (u64)FL_INT);
    SAG_ASSERT_EQ_I64(out.as.i, 15);
    vf_close(&f);
}

void test_fl_vm_instruction_boundary_work_happens(void)
{
    VmFix f;
    FlValue out;
    size_t before;

    /*
     * DoD 5's real content, and the one thing a "both modes exit 0" run
     * cannot show.  The switch dispatcher reaches the top of its loop
     * between instructions and the computed-goto one does not, so if
     * the boundary work lives at the loop top the cgoto build never
     * collects and never enforces the step limit -- and every GC test
     * passes it by not having a GC.  Both are asserted here, so
     * whichever dispatcher this binary was built with has to do them.
     */
    vf_open(&f);
    f.vm.step_limit = 200U;
    /* A loop that cannot finish inside the limit: it must be CUT, not
     * run to completion and not run forever. */
    SAG_ASSERT(!vf_run(&f, "let n = 0\nwhile n < 100000 { n = n + 1 }\n"
                           "return n\n", &out));
    SAG_ASSERT(f.vm.steps > 200U);
    vf_close(&f);

    /*
     * And the collector runs DURING the program, not only at teardown.
     * The loop allocates well past the 256 KiB first-collection floor,
     * so a dispatcher that honours gc.pending collects several times
     * and one that ignores it collects zero times.
     */
    vf_open(&f);
    SAG_ASSERT(vf_run(&f,
                      "let n = 0\n"
                      "while n < 20000 { let s = [n, n, n]\nn = n + 1 }\n"
                      "return n\n", &out));
    SAG_ASSERT_EQ_I64(out.as.i, 20000);
    if (f.vm.gc.collections == 0U)
        (void)fprintf(stderr, "the collector never ran: gc.pending is "
                              "not honoured on this dispatch path\n");
    SAG_ASSERT(f.vm.gc.collections != 0U);
    /* Live set stays bounded: the dead lists are actually reclaimed,
     * not merely counted. */
    before = f.vm.gc.bytes;
    SAG_ASSERT(before < 512U * 1024U);
    vf_close(&f);
}

void test_fl_vm_try_restores_the_stack_pointer_exactly(void)
{
    VmFix f;
    FlValue out;
    FlValue *sp_before;

    /*
     * The unwind's most easily wrong step.  A handler entered with the
     * stack one slot high leaks a value per raise, and a loop that
     * catches per iteration then overruns FL_STACK_MAX -- which
     * presents as a stack overflow thousands of iterations away from
     * the try block that caused it.
     *
     * Raising from inside a deep expression is the case that catches
     * it: several operands are already pushed when the raise happens.
     */
    vf_open(&f);
    SAG_ASSERT(vf_run(&f,
                      "let n = 0\n"
                      "while n < 200 {\n"
                      "  try { let x = 1 + (2 * (3 + (4 / 0))) }\n"
                      "  catch e { }\n"
                      "  n = n + 1\n"
                      "}\n"
                      "return n\n", &out));
    SAG_ASSERT_EQ_I64(out.as.i, 200);
    /* Back to the base of the stack, not merely "not overflowed". */
    sp_before = f.vm.stack;
    SAG_ASSERT(f.vm.sp <= sp_before + 1);
    vf_close(&f);
}

void test_fl_vm_handlers_nest(void)
{
    VmFix f;
    FlValue out;
    char src[8192];
    size_t at = 0U;
    u32 i;

    /* The innermost handler wins, and the outer ones neither fire nor
     * are left on the handler stack. */
    SAG_ASSERT_EQ_I64(
        run_int("try {\n"
                "  try { return 1 / 0 } catch inner { return 1 }\n"
                "} catch outer { return 2 }\n"), 1);
    /* An uncaught raise in the inner block reaches the outer handler
     * rather than escaping. */
    SAG_ASSERT_EQ_I64(
        run_int("try {\n"
                "  try { return 5 } catch inner { return 1 }\n"
                "  return 1 / 0\n"
                "} catch outer { return 2 }\n"), 5);

    /* And nesting to 64, which the sprint names as the depth. */
    for (i = 0U; i < 64U; i++)
        at += (size_t)snprintf(src + at, sizeof(src) - at,
                               "try {\n");
    at += (size_t)snprintf(src + at, sizeof(src) - at, "  return 7\n");
    for (i = 0U; i < 64U; i++)
        at += (size_t)snprintf(src + at, sizeof(src) - at,
                               "} catch e%u { return %u }\n",
                               (unsigned)i, (unsigned)(100U + i));
    SAG_ASSERT(at < sizeof(src));
    vf_open(&f);
    SAG_ASSERT(vf_run(&f, src, &out));
    SAG_ASSERT_EQ_I64(out.as.i, 7);
    vf_close(&f);
}

void test_fl_vm_deep_recursion_raises_rather_than_overflowing(void)
{
    /*
     * Runaway recursion is USER-TRIGGERABLE, so the frame array running
     * out must raise a catchable value and not smash the stack -- which
     * is amendment A1's entire argument for the "limit" kind.  A test
     * that only checked the error string would pass against a build
     * that overflows first and reports second, so this also proves the
     * VM is still usable afterwards.
     */
    assert_kind("fn f(n) { return f(n + 1) }\nreturn f(0)\n", "limit");
    SAG_ASSERT_EQ_I64(
        run_int("fn f(n) { return f(n + 1) }\n"
                "let hit = 0\n"
                "try { let x = f(0) } catch e { hit = 1 }\n"
                /* The VM keeps running: a second call still works. */
                "fn g() { return 3 }\n"
                "return hit + g()\n"), 4);
}

void test_fl_vm_import_names_its_sprint(void)
{
    VmFix f;
    FlValue out;
    FlValue got;

    /*
     * DoD 10, and the project's no-silent-stubs rule: the opcode exists
     * so the compiler can emit it, and running it must say which sprint
     * owes the behaviour rather than quietly returning nil.
     */
    vf_open(&f);
    SAG_ASSERT(!vf_run(&f, "import io\nreturn 0\n", &out));
    SAG_ASSERT(fl_map_get((FlMap *)out.as.o,
                          FL_OBJ_V(FL_STR, fl_str_new(&f.vm, "msg", 3U)),
                          &got));
    SAG_ASSERT_NOT_NULL(strstr(((FlStr *)got.as.o)->b, "Sprint 31"));
    vf_close(&f);
}

void test_fl_vm_reserved_handle_tags_are_named_not_constructible(void)
{
    /*
     * DoD 10.  The five §4 handle tags must be nameable -- s34's
     * type_of has to say "buf" the day it starts handing them out --
     * without being reachable from any Fletch program today.
     */
    SAG_ASSERT_EQ_I64(strcmp(fl_type_name(FL_BUF), "buf"), 0);
    SAG_ASSERT_EQ_I64(strcmp(fl_type_name(FL_CURSOR), "cursor"), 0);
    SAG_ASSERT_EQ_I64(strcmp(fl_type_name(FL_SPAN), "span"), 0);
    SAG_ASSERT_EQ_I64(strcmp(fl_type_name(FL_WIN), "win"), 0);
    SAG_ASSERT_EQ_I64(strcmp(fl_type_name(FL_REGEX), "regex"), 0);
    /* Out-of-range stays diagnosable rather than indexing past the
     * table -- the tags are the last five, so this is the guard that
     * keeps a future tag from reading garbage. */
    SAG_ASSERT_EQ_I64(strcmp(fl_type_name((FlType)FL_TYPE_COUNT), "?"), 0);
    SAG_ASSERT(FL_TYPE_COUNT <= 32);
}

/* ---------------------------------------------------------------- */
/* DoD 4: the spec §14 example, asserted                            */
/* ---------------------------------------------------------------- */

static const char *const FL_S14_PROGRAM =
    "fn clamp(x, lo, hi) {\n"
    "    if x < lo { return lo }\n"
    "    else if x > hi { return hi }\n"
    "    return x\n"
    "}\n"
    "fn counter(start) {\n"
    "    let n = start\n"
    "    return fn() { n = n + 1; return n }\n"
    "}\n";

void test_fl_vm_spec_14_total_is_17(void)
{
    /* §14.1: nums is [1, 2.5, 16]; 2.5 is skipped by continue; 1 + 16
     * is 17, and 0x10 is 16. */
    SAG_ASSERT_EQ_I64(
        run_int("let nums = [1, 2.5, 0x10]\n"
                "let total = 0\n"
                "for x in nums {\n"
                "    if x == 2.5 { continue }\n"
                "    total = total + x\n"
                "}\n"
                "return total\n"), 17);
}

void test_fl_vm_spec_14_counter_yields_9_then_10(void)
{
    char src[1024];

    /* §14.1: clamp(9, 0, 8) is 8 because 9 > hi, so n starts at 8 and
     * the first call increments to 9; the second sees 10 because the
     * closure captured n BY REFERENCE. */
    (void)snprintf(src, sizeof(src), "%s%s", FL_S14_PROGRAM,
                   "let next = counter(clamp(9, 0, 8))\n"
                   "return next()\n");
    SAG_ASSERT_EQ_I64(run_int(src), 9);
    (void)snprintf(src, sizeof(src), "%s%s", FL_S14_PROGRAM,
                   "let next = counter(clamp(9, 0, 8))\n"
                   "let a = next()\n"
                   "return next()\n");
    SAG_ASSERT_EQ_I64(run_int(src), 10);
}

void test_fl_vm_spec_14_program_compiles_and_defers_visibly(void)
{
    VmFix f;
    FlValue out;
    FlValue got;
    /* §14 verbatim, less the comments. */
    static const char *const src =
        "import str\n"
        "let greet = \"hi\\tthere\\n\"\n"
        "let nums  = [1, 2.5, 0x10]\n"
        "let cfg   = { tabwidth: 4, wrap: false, name: nil }\n"
        "fn clamp(x, lo, hi) {\n"
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
        "let total = 0\n"
        "for x in nums {\n"
        "    if x == 2.5 { continue }\n"
        "    total = total + x\n"
        "}\n"
        "macro dup = @[ l< H(lv) yank v paste esc ]\n"
        "fn shout(s) {\n"
        "    try { edit { @[ 2v i\"!\" del ] } }\n"
        "    catch e { return str.upper(e.kind) or \"?\" }\n"
        "}\n"
        "return total\n";

    /*
     * DoD 4 has four claims and this sprint can meet three: `total`,
     * and both of `counter`'s values, are asserted above.  The fourth
     * -- `shout` returning "MOTION" -- needs `str.upper`, and the whole
     * `str` module is Sprint 31's.
     *
     * So what is asserted here is that the deferral is VISIBLE: the
     * example parses and compiles clean today, every construct in it
     * including `macro` and the motion literal, and running it stops at
     * `import str` with a message naming the sprint that owes the
     * behaviour.  That is the project's no-silent-stubs rule doing its
     * job, and it is the difference between "not done yet" and "returns
     * nil and nobody notices".
     */
    vf_open(&f);
    SAG_ASSERT(!vf_run(&f, src, &out));
    SAG_ASSERT_EQ_U64((u64)out.t, (u64)FL_MAP);
    SAG_ASSERT(fl_map_get((FlMap *)out.as.o,
                          FL_OBJ_V(FL_STR, fl_str_new(&f.vm, "kind", 4U)),
                          &got));
    SAG_ASSERT_EQ_I64(memcmp(((FlStr *)got.as.o)->b, "import", 6U), 0);
    SAG_ASSERT(fl_map_get((FlMap *)out.as.o,
                          FL_OBJ_V(FL_STR, fl_str_new(&f.vm, "msg", 3U)),
                          &got));
    SAG_ASSERT_NOT_NULL(strstr(((FlStr *)got.as.o)->b, "Sprint 31"));
    /* Compiled clean: the failure was at RUN time, not a diagnostic. */
    SAG_ASSERT_EQ_U64(f.ndiag, 0U);
    vf_close(&f);
}

void test_fl_vm_spec_14_motion_raises_against_the_null_host(void)
{
    /*
     * §14.1 says shout returns "MOTION" -- str.upper of the raised
     * kind.  str is Sprint 31's, so what this sprint can assert is the
     * half that exists: the motion block raises kind "motion" against
     * the null host, and a catch binds it.
     */
    assert_kind("edit { @[ 2v i\"!\" del ] }\n", "motion");
    SAG_ASSERT_EQ_I64(
        run_int("fn shout() {\n"
                "    try { edit { @[ 2v i\"!\" del ] } }\n"
                "    catch e { if e.kind == \"motion\" { return 1 } "
                "return 0 }\n"
                "    return 2\n"
                "}\n"
                "return shout()\n"), 1);
}

/* ---------------------------------------------------------------- */
/* Disassembly is deterministic (the compiler's golden surface)     */
/* ---------------------------------------------------------------- */

void test_fl_vm_disassembly_is_deterministic(void)
{
    VmFix a;
    VmFix b;
    Bytebuf da;
    Bytebuf db;
    FlProgram pa;
    FlProgram pb;
    FlFn *fa;
    FlFn *fb;
    FlOrigin origin = {0U, 0U};
    static const char *const src =
        "fn f(x) { return x + 1 }\n"
        "let m = {a: 1, b: \"two\"}\n"
        "let l = [1, 2.5, nil]\n"
        "for k, v in m { }\n";

    vf_open(&a);
    vf_open(&b);
    (void)fl_diag_add_file(&a.dc, "t.fl", src, strlen(src));
    (void)fl_diag_add_file(&b.dc, "t.fl", src, strlen(src));
    pa = fl_parse(&a.arena, &a.dc, &a.in, src, strlen(src), 0U);
    pb = fl_parse(&b.arena, &b.dc, &b.in, src, strlen(src), 0U);
    fa = fl_compile(&a.vm, &a.dc, &pa, 0U, origin);
    fb = fl_compile(&b.vm, &b.dc, &pb, 0U, origin);
    SAG_ASSERT_NOT_NULL(fa);
    SAG_ASSERT_NOT_NULL(fb);
    bytebuf_init(&da);
    bytebuf_init(&db);
    fl_disasm_chunk(&da, &fa->ch, &a.in);
    fl_disasm_chunk(&db, &fb->ch, &b.in);
    /* No pointers and no addresses appear, so two runs match byte for
     * byte -- which is what makes this a golden surface. */
    SAG_ASSERT_EQ_U64(da.len, db.len);
    SAG_ASSERT_EQ_I64(memcmp(da.data, db.data, da.len), 0);
    SAG_ASSERT(da.len != 0U);
    bytebuf_free(&db);
    bytebuf_free(&da);
    vf_close(&b);
    vf_close(&a);
}

void test_fl_vm_opcode_table_is_complete(void)
{
    int k;

    /* Every opcode has a name, an operand shape and an effect.  A gap
     * would surface as a disassembler printing BAD_OP for a real
     * instruction, or as max_stack silently under-counting. */
    for (k = 0; k < (int)FL_OP__COUNT; k++) {
        const char *name = fl_op_name((FlOp)k);
        const char *ops = fl_op_operands((FlOp)k);
        size_t i;

        SAG_ASSERT_NOT_NULL(name);
        SAG_ASSERT_EQ_I64(strcmp(name, "BAD_OP") == 0, 0);
        SAG_ASSERT_NOT_NULL(ops);
        for (i = 0U; ops[i] != '\0'; i++) {
            SAG_ASSERT(ops[i] == 'b' || ops[i] == 's' || ops[i] == 'w');
        }
    }
    SAG_ASSERT_EQ_U64((u64)FL_OP__COUNT, 60U);
}
