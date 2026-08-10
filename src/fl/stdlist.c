/*
 * Sprint 31 deliverable 3: the `list` module.
 */
#include "fl/std.h"

#include <string.h>

#include "fl/gc.h"
#include "util/sort.h"

/* Resolves a possibly-negative index against `n`.  Negative counts from
 * the end, per the sprint's index rule. */
static bool idx_of(FlVm *vm, i64 i, u32 n, bool allow_end, u32 *out)
{
    i64 r = i < 0 ? (i64)n + i : i;

    if (r < 0 || r > (i64)n || (!allow_end && r == (i64)n))
        return fl_raise(vm, "index", "index %lld out of range for length %u",
                        (long long)i, (unsigned)n);
    *out = (u32)r;
    return true;
}

/* Structural mutation, which iteration watches. */
static void bump(FlList *l) { l->mods++; }

/* ---------------------------------------------------------------- */
/* Plain operations                                                 */
/* ---------------------------------------------------------------- */

static bool l_len(FlVm *vm, FlValue *a, u32 n, FlValue *out)
{
    FlList *l;

    (void)n;
    if (!fl_arg_list(vm, a, 0U, &l))
        return false;
    *out = FL_INT_V((i64)l->n);
    return true;
}

static bool l_push(FlVm *vm, FlValue *a, u32 n, FlValue *out)
{
    FlList *l;

    (void)n;
    if (!fl_arg_list(vm, a, 0U, &l))
        return false;
    (void)fl_list_push(vm, l, a[1]);
    bump(l);
    *out = FL_NIL_V;
    return true;
}

static bool l_pop(FlVm *vm, FlValue *a, u32 n, FlValue *out)
{
    FlList *l;

    (void)n;
    if (!fl_arg_list(vm, a, 0U, &l))
        return false;
    if (l->n == 0U)
        return fl_raise(vm, "index", "list.pop: the list is empty");
    *out = l->v[--l->n];
    bump(l);
    return true;
}

static bool l_insert(FlVm *vm, FlValue *a, u32 n, FlValue *out)
{
    FlList *l;
    i64 i;
    u32 at = 0U;      /* idx_of writes it; gcc cannot see that */

    (void)n;
    if (!fl_arg_list(vm, a, 0U, &l) || !fl_arg_int(vm, a, 1U, &i))
        return false;
    if (!idx_of(vm, i, l->n, true, &at))     /* == len appends */
        return false;
    (void)fl_list_push(vm, l, FL_NIL_V);     /* grow by one */
    (void)memmove(&l->v[at + 1U], &l->v[at],
                  (size_t)(l->n - at - 1U) * sizeof(l->v[0]));
    l->v[at] = a[2];
    bump(l);
    *out = FL_NIL_V;
    return true;
}

static bool l_remove(FlVm *vm, FlValue *a, u32 n, FlValue *out)
{
    FlList *l;
    i64 i;
    u32 at = 0U;      /* idx_of writes it; gcc cannot see that */

    (void)n;
    if (!fl_arg_list(vm, a, 0U, &l) || !fl_arg_int(vm, a, 1U, &i))
        return false;
    if (!idx_of(vm, i, l->n, false, &at))
        return false;
    *out = l->v[at];
    (void)memmove(&l->v[at], &l->v[at + 1U],
                  (size_t)(l->n - at - 1U) * sizeof(l->v[0]));
    l->n--;
    bump(l);
    return true;
}

static bool l_clear(FlVm *vm, FlValue *a, u32 n, FlValue *out)
{
    FlList *l;

    (void)n;
    if (!fl_arg_list(vm, a, 0U, &l))
        return false;
    l->n = 0U;
    bump(l);
    *out = FL_NIL_V;
    return true;
}

static bool l_get(FlVm *vm, FlValue *a, u32 n, FlValue *out)
{
    FlList *l;
    i64 i;
    u32 at = 0U;      /* idx_of writes it; gcc cannot see that */

    if (!fl_arg_list(vm, a, 0U, &l) || !fl_arg_int(vm, a, 1U, &i))
        return false;
    {
        i64 r = i < 0 ? (i64)l->n + i : i;

        if (r < 0 || r >= (i64)l->n) {
            /* A default turns the miss into a value; without one it is
             * an error, because silently returning nil is how an
             * off-by-one becomes a nil three functions later. */
            if (n >= 3U) {
                *out = a[2];
                return true;
            }
            return fl_raise(vm, "index",
                            "index %lld out of range for length %u",
                            (long long)i, (unsigned)l->n);
        }
        at = (u32)r;
    }
    *out = l->v[at];
    return true;
}

static bool l_index_of(FlVm *vm, FlValue *a, u32 n, FlValue *out)
{
    FlList *l;
    u32 i;

    (void)n;
    if (!fl_arg_list(vm, a, 0U, &l))
        return false;
    for (i = 0U; i < l->n; i++) {
        if (fl_equal(l->v[i], a[1])) {
            *out = FL_INT_V((i64)i);
            return true;
        }
    }
    *out = FL_INT_V(-1);
    return true;
}

static bool l_contains(FlVm *vm, FlValue *a, u32 n, FlValue *out)
{
    FlValue idx;

    if (!l_index_of(vm, a, n, &idx))
        return false;
    *out = FL_BOOL_V(idx.as.i >= 0);
    return true;
}

static bool l_slice(FlVm *vm, FlValue *a, u32 n, FlValue *out)
{
    FlList *l;
    i64 lo;
    i64 hi;
    u32 a0 = 0U;
    u32 a1 = 0U;
    FlList *r;
    u32 i;

    if (!fl_arg_list(vm, a, 0U, &l) || !fl_arg_int(vm, a, 1U, &lo))
        return false;
    hi = (i64)l->n;
    if (n >= 3U && !fl_arg_int(vm, a, 2U, &hi))
        return false;
    if (!idx_of(vm, lo, l->n, true, &a0) || !idx_of(vm, hi, l->n, true, &a1))
        return false;
    r = fl_list_new(vm);
    fl_gc_protect(vm, FL_OBJ_V(FL_LIST, r));
    for (i = a0; i < a1; i++)
        (void)fl_list_push(vm, r, l->v[i]);
    fl_gc_release(vm, 1U);
    *out = FL_OBJ_V(FL_LIST, r);
    return true;
}

static bool l_concat(FlVm *vm, FlValue *a, u32 n, FlValue *out)
{
    FlList *x;
    FlList *y;
    FlList *r;
    u32 i;

    (void)n;
    if (!fl_arg_list(vm, a, 0U, &x) || !fl_arg_list(vm, a, 1U, &y))
        return false;
    r = fl_list_new(vm);
    fl_gc_protect(vm, FL_OBJ_V(FL_LIST, r));
    for (i = 0U; i < x->n; i++)
        (void)fl_list_push(vm, r, x->v[i]);
    for (i = 0U; i < y->n; i++)
        (void)fl_list_push(vm, r, y->v[i]);
    fl_gc_release(vm, 1U);
    *out = FL_OBJ_V(FL_LIST, r);
    return true;
}

static bool l_reverse(FlVm *vm, FlValue *a, u32 n, FlValue *out)
{
    FlList *l;
    u32 i;

    (void)n;
    if (!fl_arg_list(vm, a, 0U, &l))
        return false;
    for (i = 0U; i + 1U < l->n - i; i++) {
        FlValue t = l->v[i];

        l->v[i] = l->v[l->n - 1U - i];
        l->v[l->n - 1U - i] = t;
    }
    /* In place and element-wise: no structural change, so `mods` does
     * not move and an iteration in progress stays valid. */
    *out = FL_NIL_V;
    return true;
}

static bool l_copy(FlVm *vm, FlValue *a, u32 n, FlValue *out)
{
    FlList *l;
    FlList *r;
    u32 i;

    (void)n;
    if (!fl_arg_list(vm, a, 0U, &l))
        return false;
    r = fl_list_new(vm);
    fl_gc_protect(vm, FL_OBJ_V(FL_LIST, r));
    for (i = 0U; i < l->n; i++)
        (void)fl_list_push(vm, r, l->v[i]);
    fl_gc_release(vm, 1U);
    *out = FL_OBJ_V(FL_LIST, r);
    return true;
}

/* ---------------------------------------------------------------- */
/* Sorting                                                          */
/* ---------------------------------------------------------------- */

/*
 * The type bands of the default order:
 *   nil < bool < int|float < str < list < map < fn < handle
 * Numbers share one band so 1 and 1.5 interleave numerically rather
 * than by tag.
 */
static int type_band(FlType t)
{
    switch (t) {
    case FL_NIL:   return 0;
    case FL_BOOL:  return 1;
    case FL_INT:
    case FL_FLOAT: return 2;
    case FL_STR:   return 3;
    case FL_LIST:  return 4;
    case FL_MAP:   return 5;
    case FL_FN:
    case FL_CLOSURE:
    case FL_NATIVE: return 6;
    default:       return 7;      /* the reserved handles */
    }
}

/*
 * A TOTAL order across mixed types, so sorting a mixed list is defined
 * rather than undefined.  Objects with no natural order fall back to
 * their insertion sequence number -- never their address, which would
 * differ per run and break invariant 5.
 */
static int default_cmp(FlValue a, FlValue b)
{
    int ba = type_band((FlType)a.t);
    int bb = type_band((FlType)b.t);

    if (ba != bb)
        return ba < bb ? -1 : 1;
    switch (ba) {
    case 0:
        return 0;
    case 1:
        return (int)a.as.b - (int)b.as.b;
    case 2: {
        double x = a.t == (u8)FL_INT ? (double)a.as.i : a.as.f;
        double y = b.t == (u8)FL_INT ? (double)b.as.i : b.as.f;

        /* NaN sorts equal to everything in its band; stability then
         * keeps it where it was, which beats a comparator that lies
         * about a < b and b < a simultaneously. */
        if (x < y) return -1;
        if (x > y) return 1;
        return 0;
    }
    case 3: {
        const FlStr *x = (const FlStr *)a.as.o;
        const FlStr *y = (const FlStr *)b.as.o;
        u32 m = x->len < y->len ? x->len : y->len;
        int c = m == 0U ? 0 : memcmp(x->b, y->b, m);

        if (c != 0)
            return c < 0 ? -1 : 1;
        return x->len == y->len ? 0 : (x->len < y->len ? -1 : 1);
    }
    default:
        if (a.as.o == b.as.o)
            return 0;
        return a.as.o->aux < b.as.o->aux ? -1 : 1;
    }
}

typedef struct SortCtx {
    FlVm *vm;
    FlValue cmp;        /* nil when using the default order */
    bool raised;
} SortCtx;

static int sort_cmp(const void *pa, const void *pb, void *ctx)
{
    SortCtx *sc = ctx;
    const FlValue *a = pa;
    const FlValue *b = pb;

    if (sc->raised)
        return 0;                  /* stop comparing once it failed */
    if (sc->cmp.t == (u8)FL_NIL)
        return default_cmp(*a, *b);
    {
        FlValue args[2];
        FlValue r = FL_NIL_V;

        args[0] = *a;
        args[1] = *b;
        if (!fl_call(sc->vm, sc->cmp, args, 2U, &r)) {
            sc->raised = true;
            return 0;
        }
        if (r.t != (u8)FL_INT) {
            (void)fl_raise(sc->vm, "type",
                           "list.sort: the comparator must return an int, "
                           "found %s", fl_type_name((FlType)r.t));
            sc->raised = true;
            return 0;
        }
        return r.as.i < 0 ? -1 : (r.as.i > 0 ? 1 : 0);
    }
}

static bool l_sort(FlVm *vm, FlValue *a, u32 n, FlValue *out)
{
    FlList *l;
    SortCtx sc;
    FlList *scratch;
    u32 i;

    if (!fl_arg_list(vm, a, 0U, &l))
        return false;
    sc.vm = vm;
    sc.cmp = FL_NIL_V;
    sc.raised = false;
    if (n >= 2U && !fl_arg_fn(vm, a, 1U, &sc.cmp))
        return false;
    if (l->n < 2U) {
        *out = FL_NIL_V;
        return true;
    }
    /*
     * Sorted into a SCRATCH list and committed only on success.
     *
     * A comparator can raise, and a half-sorted list left behind is the
     * kind of state that makes a macro's rollback meaningless -- the
     * user sees neither the old order nor a sorted one.  The scratch is
     * a real FlList so the collector can see the values during the
     * comparator's own allocations.
     */
    scratch = fl_list_new(vm);
    fl_gc_protect(vm, FL_OBJ_V(FL_LIST, scratch));
    for (i = 0U; i < l->n; i++)
        (void)fl_list_push(vm, scratch, l->v[i]);
    yew_sort_stable(scratch->v, scratch->n, sizeof(scratch->v[0]),
                    sort_cmp, &sc);
    if (!sc.raised) {
        for (i = 0U; i < l->n; i++)
            l->v[i] = scratch->v[i];
    }
    fl_gc_release(vm, 1U);
    if (sc.raised)
        return false;
    *out = FL_NIL_V;
    return true;
}

/* ---------------------------------------------------------------- */
/* Higher-order                                                     */
/* ---------------------------------------------------------------- */

static bool l_map(FlVm *vm, FlValue *a, u32 n, FlValue *out)
{
    FlList *l;
    FlValue f;
    FlList *r;
    u32 i;

    (void)n;
    if (!fl_arg_list(vm, a, 0U, &l) || !fl_arg_fn(vm, a, 1U, &f))
        return false;
    r = fl_list_new(vm);
    fl_gc_protect(vm, FL_OBJ_V(FL_LIST, r));
    for (i = 0U; i < l->n; i++) {
        FlValue v = FL_NIL_V;

        /* l->v[i] is read fresh each iteration: the callback may push
         * to the source list and reallocate its backing array, so a
         * cached pointer would dangle. */
        if (!fl_call(vm, f, &l->v[i], 1U, &v)) {
            fl_gc_release(vm, 1U);
            return false;
        }
        (void)fl_list_push(vm, r, v);
    }
    fl_gc_release(vm, 1U);
    *out = FL_OBJ_V(FL_LIST, r);
    return true;
}

static bool l_filter(FlVm *vm, FlValue *a, u32 n, FlValue *out)
{
    FlList *l;
    FlValue f;
    FlList *r;
    u32 i;

    (void)n;
    if (!fl_arg_list(vm, a, 0U, &l) || !fl_arg_fn(vm, a, 1U, &f))
        return false;
    r = fl_list_new(vm);
    fl_gc_protect(vm, FL_OBJ_V(FL_LIST, r));
    for (i = 0U; i < l->n; i++) {
        FlValue keep = FL_NIL_V;
        FlValue item = l->v[i];

        if (!fl_call(vm, f, &item, 1U, &keep)) {
            fl_gc_release(vm, 1U);
            return false;
        }
        if (fl_truthy(keep))
            (void)fl_list_push(vm, r, item);
    }
    fl_gc_release(vm, 1U);
    *out = FL_OBJ_V(FL_LIST, r);
    return true;
}

static bool l_reduce(FlVm *vm, FlValue *a, u32 n, FlValue *out)
{
    FlList *l;
    FlValue f;
    FlValue acc;
    u32 i;

    (void)n;
    if (!fl_arg_list(vm, a, 0U, &l) || !fl_arg_fn(vm, a, 1U, &f))
        return false;
    acc = a[2];
    /*
     * EXACTLY ONE protected slot, replaced each iteration.
     *
     * The accumulator lives only in this C local between calls, so it
     * needs rule-1 protection across the next one -- but protecting
     * per iteration without releasing would overflow the 32-slot temp
     * stack on the 33rd element and abort as a discipline violation,
     * which is a crash on a perfectly ordinary reduce.
     */
    fl_gc_protect(vm, acc);
    for (i = 0U; i < l->n; i++) {
        FlValue args[2];
        FlValue next = FL_NIL_V;

        args[0] = acc;
        args[1] = l->v[i];
        if (!fl_call(vm, f, args, 2U, &next)) {
            fl_gc_release(vm, 1U);
            return false;
        }
        /* Nothing allocates between the release and the protect. */
        fl_gc_release(vm, 1U);
        acc = next;
        fl_gc_protect(vm, acc);
    }
    fl_gc_release(vm, 1U);
    *out = acc;
    return true;
}

static bool any_all(FlVm *vm, FlValue *a, FlValue *out, bool want_all)
{
    FlList *l;
    FlValue f;
    u32 i;

    if (!fl_arg_list(vm, a, 0U, &l) || !fl_arg_fn(vm, a, 1U, &f))
        return false;
    for (i = 0U; i < l->n; i++) {
        FlValue r = FL_NIL_V;
        FlValue item = l->v[i];

        if (!fl_call(vm, f, &item, 1U, &r))
            return false;
        if (fl_truthy(r) != want_all) {   /* short-circuits */
            *out = FL_BOOL_V(!want_all);
            return true;
        }
    }
    *out = FL_BOOL_V(want_all);
    return true;
}

static bool l_any(FlVm *vm, FlValue *a, u32 n, FlValue *out)
{
    (void)n;
    return any_all(vm, a, out, false);
}

static bool l_all(FlVm *vm, FlValue *a, u32 n, FlValue *out)
{
    (void)n;
    return any_all(vm, a, out, true);
}

/* ---------------------------------------------------------------- */
/* The table                                                        */
/* ---------------------------------------------------------------- */

static const FlNativeDef LIST_DEFS[] = {
    {"len",      l_len,      1U, 1U, 0U, "(l) -> int"},
    {"push",     l_push,     2U, 2U, 0U, "(l, v) -> nil"},
    {"pop",      l_pop,      1U, 1U, 0U, "(l) -> v"},
    {"insert",   l_insert,   3U, 3U, 0U, "(l, i, v) -> nil"},
    {"remove",   l_remove,   2U, 2U, 0U, "(l, i) -> v"},
    {"clear",    l_clear,    1U, 1U, 0U, "(l) -> nil"},
    {"get",      l_get,      2U, 3U, 0U, "(l, i, [default]) -> v"},
    {"index_of", l_index_of, 2U, 2U, 0U, "(l, v) -> int"},
    {"contains", l_contains, 2U, 2U, 0U, "(l, v) -> bool"},
    {"slice",    l_slice,    2U, 3U, 0U, "(l, lo, [hi]) -> list"},
    {"concat",   l_concat,   2U, 2U, 0U, "(a, b) -> list"},
    {"reverse",  l_reverse,  1U, 1U, 0U, "(l) -> nil"},
    {"copy",     l_copy,     1U, 1U, 0U, "(l) -> list"},
    {"sort",     l_sort,     1U, 2U, 0U, "(l, [cmp]) -> nil"},
    {"map",      l_map,      2U, 2U, 0U, "(l, f) -> list"},
    {"filter",   l_filter,   2U, 2U, 0U, "(l, f) -> list"},
    {"reduce",   l_reduce,   3U, 3U, 0U, "(l, f, init) -> v"},
    {"any",      l_any,      2U, 2U, 0U, "(l, f) -> bool"},
    {"all",      l_all,      2U, 2U, 0U, "(l, f) -> bool"}
};

const FlModuleDef fl_mod_list = {
    "list", LIST_DEFS, (u32)YEW_ARRAY_LEN(LIST_DEFS), NULL, 0U
};
