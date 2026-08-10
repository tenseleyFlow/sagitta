/*
 * Sprint 31: the stdlib fuzzer.
 *
 * Every `str` and every `fmt` entry point, driven with the input bytes
 * as a STRING ARGUMENT and as a fmt.f TEMPLATE.
 *
 * NATIVES ARE CALLED THROUGH THE MODULE MAP, not through a hand-written
 * list.  A list goes stale the first time a function is added and the
 * gap is invisible -- walking the registered map means a new native is
 * fuzzed the day it lands, which is what DoD 3's "no gaps" is about.
 *
 * THE PROPERTY: no crash, no over-read, and every outcome is either a
 * value or a raise whose kind is one of §9's twelve.  A native that
 * returned false without setting an error, or set a kind outside the
 * closed set, is a native whose failure a `catch` cannot classify.
 *
 * The bytes are NOT cleaned before use.  Invalid UTF-8 is exactly the
 * input `str` promises to carry byte-for-byte, so feeding it only
 * well-formed text would fuzz the easy half.
 */
#include "fuzzlib.h"

#include <stdio.h>
#include <string.h>

#include "fl/gc.h"
#include "fl/std.h"
#include "fl/vm.h"
#include "util/arena.h"
#include "util/intern.h"

enum {
    /* Long enough for a multi-cluster subject, short enough that the
     * GC-stress iterations still finish: stress collects at EVERY
     * allocation and str.repeat is quadratic against it. */
    FL_STD_FUZZ_MAX_INPUT = 4096U,
    FL_STD_FUZZ_MAX_ARGS = 5U
};

/* §9.1, closed for 1.0.  Amendment A1 added "limit". */
static bool known_kind(const char *k)
{
    static const char *const KINDS[] = {
        "type", "arity", "name", "index", "key", "div",
        "capability", "io", "import", "motion", "user", "limit"
    };
    size_t i;

    for (i = 0U; i < sizeof(KINDS) / sizeof(KINDS[0]); i++) {
        if (strcmp(k, KINDS[i]) == 0)
            return true;
    }
    return false;
}

/* The `kind` of the in-flight error, or "" when there is none. */
static void err_kind(FlVm *vm, char *out, size_t cap)
{
    FlValue got = FL_NIL_V;
    FlStr *k;

    out[0] = '\0';
    if (vm->err.t != (u8)FL_MAP)
        return;
    k = fl_str_new(vm, "kind", 4U);
    if (!fl_map_get((FlMap *)vm->err.as.o, FL_OBJ_V(FL_STR, k), &got))
        return;
    if (got.t != (u8)FL_STR)
        return;
    (void)snprintf(out, cap, "%.*s", (int)((const FlStr *)got.as.o)->len,
                   ((const FlStr *)got.as.o)->b);
}

/*
 * One call, and the property check around it.
 *
 * argv lives on the VM STACK because gc.h rule 1 says a native's
 * arguments do: a native that allocates -- and under stress every
 * allocation collects -- would otherwise have its arguments swept from
 * under it.
 */
static bool call_native(FlVm *vm, FlNative *nat, const FlValue *args,
                        u32 nargs, char *why, size_t why_cap)
{
    FlValue *slots = vm->sp;
    FlValue out = FL_NIL_V;
    bool ok;
    u32 i;

    for (i = 0U; i < nargs; i++)
        *vm->sp++ = args[i];
    vm->cur_native = nat->name_id;
    vm->err = FL_NIL_V;
    ok = nat->fn(vm, slots, nargs, &out);
    vm->sp = slots;
    if (ok)
        return true;
    {
        char kind[64];
        const char *nm = yew_intern_str(vm->in, nat->name_id);

        err_kind(vm, kind, sizeof(kind));
        if (kind[0] == '\0') {
            (void)snprintf(why, why_cap, "%s failed without an error value",
                           nm == NULL ? "?" : nm);
            return false;
        }
        if (!known_kind(kind)) {
            (void)snprintf(why, why_cap, "%s raised unknown kind '%s'",
                           nm == NULL ? "?" : nm, kind);
            return false;
        }
    }
    return true;
}

/*
 * Argument shapes, chosen by a byte of the input so that over a
 * campaign every native sees a string, an int and a list in every slot.
 *
 * A wrong-typed argument is a legitimate outcome -- kind "type" -- so
 * this deliberately does not try to satisfy each signature.  What it is
 * looking for is the call that reads past the end of something.
 */
static void build_args(FlVm *vm, u8 shape, FlValue subject, i64 n1, i64 n2,
                       FlValue small, FlValue list, FlValue *args)
{
    (void)vm;
    switch (shape % 4U) {
    case 0:
        args[0] = subject;
        args[1] = FL_INT_V(n1);
        args[2] = FL_INT_V(n2);
        args[3] = small;
        args[4] = FL_INT_V(n1);
        return;
    case 1:
        args[0] = subject;
        args[1] = small;
        args[2] = small;
        args[3] = FL_INT_V(n1);
        args[4] = FL_INT_V(n2);
        return;
    case 2:
        args[0] = list;
        args[1] = small;
        args[2] = subject;
        args[3] = FL_INT_V(n1);
        args[4] = FL_INT_V(n2);
        return;
    default:
        args[0] = subject;
        args[1] = subject;
        args[2] = subject;
        args[3] = subject;
        args[4] = subject;
        return;
    }
}

/* Walks one builtin module's map and calls every native in it. */
static bool sweep_module(FlVm *vm, const char *modname, u32 want,
                         const u8 *data, size_t len, char *why,
                         size_t why_cap)
{
    FlValue modv = FL_NIL_V;
    FlStr *key = fl_str_new(vm, modname, (u32)strlen(modname));
    FlMap *mod;
    u32 cursor = 0U;
    FlValue k;
    FlValue v;
    FlValue subject;
    FlValue small;
    FlValue listv;
    FlList *l;
    i64 n1;
    i64 n2;
    u32 called = 0U;
    u8 shape = len == 0U ? 0U : data[0];

    if (!fl_map_get(vm->builtins, FL_OBJ_V(FL_STR, key), &modv) ||
        modv.t != (u8)FL_MAP) {
        (void)snprintf(why, why_cap, "no builtin module %s", modname);
        return false;
    }
    mod = (FlMap *)modv.as.o;
    subject = FL_OBJ_V(FL_STR, fl_str_new(vm, (const char *)data, (u32)len));
    fl_gc_protect(vm, subject);
    /* Two indices from the input, kept in a range that straddles the
     * subject's ends so both the valid and the refused paths run. */
    n1 = len < 2U ? 0 : (i64)data[1] - 64;
    n2 = len < 3U ? 1 : (i64)data[2] - 64;
    small = FL_OBJ_V(FL_STR, fl_str_new(vm, len < 4U ? "a"
                                                     : (const char *)data + 3,
                                        len < 5U ? 1U : 2U));
    fl_gc_protect(vm, small);
    l = fl_list_new(vm);
    listv = FL_OBJ_V(FL_LIST, l);
    fl_gc_protect(vm, listv);
    (void)fl_list_push(vm, l, subject);
    (void)fl_list_push(vm, l, small);

    while (fl_map_iter(mod, &cursor, &k, &v)) {
        FlValue args[FL_STD_FUZZ_MAX_ARGS];
        FlNative *nat;
        u32 nargs;

        if (v.t != (u8)FL_NATIVE)
            continue;                 /* a constant, not a function */
        nat = (FlNative *)v.as.o;
        nargs = nat->min_ar;
        if (nargs > (u32)FL_STD_FUZZ_MAX_ARGS)
            nargs = (u32)FL_STD_FUZZ_MAX_ARGS;
        called++;
        build_args(vm, shape, subject, n1, n2, small, listv, args);
        if (!call_native(vm, nat, args, nargs, why, why_cap)) {
            fl_gc_release(vm, 3U);
            return false;
        }
        /* And once at the maximum arity, so the optional arguments are
         * reached rather than left to a later sprint's fuzzer. */
        if (nat->max_ar != 255U && nat->max_ar != nat->min_ar) {
            nargs = nat->max_ar > (u32)FL_STD_FUZZ_MAX_ARGS
                        ? (u32)FL_STD_FUZZ_MAX_ARGS
                        : nat->max_ar;
            build_args(vm, (u8)(shape + 1U), subject, n1, n2, small, listv,
                       args);
            if (!call_native(vm, nat, args, nargs, why, why_cap)) {
                fl_gc_release(vm, 3U);
                return false;
            }
        }
        shape++;
    }
    fl_gc_release(vm, 3U);
    /*
     * A FUZZER THAT COVERS NOTHING PASSES EVERY ITERATION.  If the
     * module map were ever walked wrongly -- or a module lost its
     * table -- the sweep above would call zero natives and report ok
     * forever, which is the quietest way for a lane to stop working.
     */
    if (called < want) {
        (void)snprintf(why, why_cap, "%s: swept %u natives, expected %u",
                       modname, (unsigned)called, (unsigned)want);
        return false;
    }
    return true;
}

/*
 * fmt.f with the input as its TEMPLATE.
 *
 * The template is data, never a format: the property is that a
 * malformed one ends in a raise rather than in a conversion.  The
 * sprint's testing section says every outcome is a string or a `"type"`
 * raise; width and result caps legitimately raise `"limit"` and a
 * missing argument `"index"`, so the assertion is the closed kind set
 * with those three the ones actually reachable here.
 */
static bool sweep_template(FlVm *vm, const u8 *data, size_t len, char *why,
                           size_t why_cap)
{
    FlValue modv = FL_NIL_V;
    FlStr *key = fl_str_new(vm, "fmt", 3U);
    FlValue fv = FL_NIL_V;
    FlStr *fkey = fl_str_new(vm, "f", 1U);
    FlValue args[4];
    FlValue *slots;
    FlValue out = FL_NIL_V;
    FlNative *nat;
    bool ok;

    if (!fl_map_get(vm->builtins, FL_OBJ_V(FL_STR, key), &modv) ||
        modv.t != (u8)FL_MAP)
        return true;
    if (!fl_map_get((FlMap *)modv.as.o, FL_OBJ_V(FL_STR, fkey), &fv) ||
        fv.t != (u8)FL_NATIVE)
        return true;
    nat = (FlNative *)fv.as.o;
    args[0] = FL_OBJ_V(FL_STR,
                       fl_str_new(vm, (const char *)data, (u32)len));
    args[1] = FL_INT_V(42);
    args[2] = FL_OBJ_V(FL_STR, fl_str_new(vm, "arg", 3U));
    args[3] = FL_FLOAT_V(1.5);
    slots = vm->sp;
    {
        u32 i;

        for (i = 0U; i < 4U; i++)
            *vm->sp++ = args[i];
    }
    vm->cur_native = nat->name_id;
    vm->err = FL_NIL_V;
    ok = nat->fn(vm, slots, 4U, &out);
    vm->sp = slots;
    if (ok) {
        if (out.t != (u8)FL_STR) {
            (void)snprintf(why, why_cap, "fmt.f returned %s, not a string",
                           fl_type_name((FlType)out.t));
            return false;
        }
        return true;
    }
    {
        char kind[64];

        err_kind(vm, kind, sizeof(kind));
        if (!known_kind(kind)) {
            (void)snprintf(why, why_cap, "fmt.f raised unknown kind '%s'",
                           kind);
            return false;
        }
    }
    return true;
}

static bool check_fl_std(const u8 *data, size_t len, char *why,
                         size_t why_cap)
{
    Arena arena;
    Interner in;
    DiagCtx dc;
    FlVm vm;
    bool ok;

    if (len > (size_t)FL_STD_FUZZ_MAX_INPUT)
        len = (size_t)FL_STD_FUZZ_MAX_INPUT;

    arena_init(&arena);
    interner_init(&in, &arena);
    fl_diag_init(&dc, &arena);
    fl_vm_init(&vm, &arena, &in, &dc);
    /*
     * GC STRESS on a slice of the campaign.  Under stress the collector
     * runs at EVERY allocation, so a native building an intermediate
     * without fl_gc_protect fails here and nowhere else -- that lane is
     * the enforcement mechanism for s30's rule 2, and a fuzzer that
     * never turned it on would be testing the easy configuration.
     *
     * Not every iteration, because it is roughly thirty times slower
     * and the budget buys coverage first.
     */
    if (len != 0U && (data[0] & 3U) == 0U)
        vm.gc.stress = true;
    fl_std_register(&vm);
    /* All four grants: io is not swept here -- it would touch the
     * filesystem -- but the origin should not be the reason a call
     * fails, or the fuzzer would only ever see "capability". */
    vm.root_origin.kind = (u8)FL_ORIGIN_CLI;
    vm.root_origin.caps = (u32)FL_CAP_FS_READ | (u32)FL_CAP_FS_WRITE |
                          (u32)FL_CAP_SHELL | (u32)FL_CAP_NET;

    /* The counts are the sprint's tables; a native added without being
     * fuzzed shows up here rather than in a later campaign. */
    ok = sweep_module(&vm, "str", 30U, data, len, why, why_cap);
    if (ok)
        ok = sweep_module(&vm, "fmt", 7U, data, len, why, why_cap);
    if (ok)
        ok = sweep_template(&vm, data, len, why, why_cap);

    fl_vm_free(&vm);
    interner_free(&in);
    arena_free_all(&arena);
    return ok;
}

int main(int argc, char **argv)
{
    return yew_fuzz_main(argc, argv, "fuzz_fl_std",
                         "tests/fuzz/corpus/fl_std", check_fl_std);
}
