/*
 * Sprint 31 deliverable 5: the `math` module.
 *
 * Float results follow IEEE 754 and DO NOT RAISE: math.sqrt(-1.0) is
 * NaN, math.log(0.0) is -inf.  The only arithmetic errors in Fletch are
 * integer / and % by zero (spec §4, kind "div").  Raising on NaN would
 * mean every numeric script needed a try block around arithmetic that
 * is perfectly well-defined.
 */
#include "fl/std.h"

#include <math.h>

#include "fl/gc.h"

/* ---------------------------------------------------------------- */
/* Shape helpers                                                    */
/* ---------------------------------------------------------------- */

static FlValue num(double d) { return FL_FLOAT_V(d); }

/* int in, int out; float in, float out.  floor/ceil/round/trunc pass an
 * int through unchanged rather than turning it into a float, so
 * `math.floor(3)` stays usable as an index. */
static bool same_shape(FlVm *vm, FlValue *a, FlValue *out,
                       double (*f)(double))
{
    double x;

    if (a[0].t == (u8)FL_INT) {
        *out = a[0];
        return true;
    }
    if (!fl_arg_num(vm, a, 0U, &x))
        return false;
    *out = num(f(x));
    return true;
}

static bool one_double(FlVm *vm, FlValue *a, FlValue *out,
                       double (*f)(double))
{
    double x;

    if (!fl_arg_num(vm, a, 0U, &x))
        return false;
    *out = num(f(x));
    return true;
}

/* ---------------------------------------------------------------- */
/* The functions                                                    */
/* ---------------------------------------------------------------- */

static bool m_abs(FlVm *vm, FlValue *a, u32 n, FlValue *out)
{
    double x;

    (void)n;
    if (a[0].t == (u8)FL_INT) {
        i64 v = a[0].as.i;

        /* abs(INT_MIN) WRAPS, per §4's two's-complement arithmetic.
         * Raising here would make abs the only arithmetic operation
         * with an error path, for one input nobody passes on purpose. */
        *out = FL_INT_V(v < 0 ? (i64)(0U - (u64)v) : v);
        return true;
    }
    if (!fl_arg_num(vm, a, 0U, &x))
        return false;
    *out = num(fabs(x));
    return true;
}

/* min/max keep int-ness when every argument is an int, so
 * `math.min(a, b)` stays an index. */
static bool minmax(FlVm *vm, FlValue *a, u32 n, FlValue *out, bool want_max)
{
    u32 i;
    bool all_int = true;
    double best = 0.0;
    i64 besti = 0;

    for (i = 0U; i < n; i++) {
        double x;

        if (a[i].t != (u8)FL_INT)
            all_int = false;
        if (!fl_arg_num(vm, a, i, &x))
            return false;
        if (i == 0U || (want_max ? x > best : x < best)) {
            best = x;
            besti = a[i].t == (u8)FL_INT ? a[i].as.i : (i64)x;
        }
    }
    *out = all_int ? FL_INT_V(besti) : num(best);
    return true;
}

static bool m_min(FlVm *vm, FlValue *a, u32 n, FlValue *out)
{
    return minmax(vm, a, n, out, false);
}

static bool m_max(FlVm *vm, FlValue *a, u32 n, FlValue *out)
{
    return minmax(vm, a, n, out, true);
}

static bool m_clamp(FlVm *vm, FlValue *a, u32 n, FlValue *out)
{
    double x;
    double lo;
    double hi;

    (void)n;
    if (!fl_arg_num(vm, a, 0U, &x) || !fl_arg_num(vm, a, 1U, &lo) ||
        !fl_arg_num(vm, a, 2U, &hi))
        return false;
    /* lo > hi has no defined answer -- returning either bound would be
     * a guess at which argument the caller got wrong. */
    if (lo > hi)
        return fl_raise(vm, "type", "math.clamp: lo must not exceed hi");
    if (x < lo) {
        *out = a[1];
        return true;
    }
    if (x > hi) {
        *out = a[2];
        return true;
    }
    *out = a[0];
    return true;
}

static bool m_sign(FlVm *vm, FlValue *a, u32 n, FlValue *out)
{
    double x;

    (void)n;
    if (!fl_arg_num(vm, a, 0U, &x))
        return false;
    /* NaN has no sign; 0 is the only answer that is not a lie. */
    *out = FL_INT_V(x != x ? 0 : (x > 0.0 ? 1 : (x < 0.0 ? -1 : 0)));
    return true;
}

#define FL_MATH_SHAPE(NAME, FN)                                           \
    static bool NAME(FlVm *vm, FlValue *a, u32 n, FlValue *out)           \
    {                                                                     \
        (void)n;                                                          \
        return same_shape(vm, a, out, FN);                                \
    }
FL_MATH_SHAPE(m_floor, floor)
FL_MATH_SHAPE(m_ceil, ceil)
FL_MATH_SHAPE(m_round, round)
FL_MATH_SHAPE(m_trunc, trunc)
#undef FL_MATH_SHAPE

#define FL_MATH_D(NAME, FN)                                               \
    static bool NAME(FlVm *vm, FlValue *a, u32 n, FlValue *out)           \
    {                                                                     \
        (void)n;                                                          \
        return one_double(vm, a, out, FN);                                \
    }
FL_MATH_D(m_sqrt, sqrt)
FL_MATH_D(m_exp, exp)
FL_MATH_D(m_log, log)
FL_MATH_D(m_log2, log2)
FL_MATH_D(m_log10, log10)
FL_MATH_D(m_sin, sin)
FL_MATH_D(m_cos, cos)
FL_MATH_D(m_tan, tan)
FL_MATH_D(m_asin, asin)
FL_MATH_D(m_acos, acos)
FL_MATH_D(m_atan, atan)
#undef FL_MATH_D

static bool m_int(FlVm *vm, FlValue *a, u32 n, FlValue *out)
{
    double x;

    (void)n;
    if (a[0].t == (u8)FL_INT) {
        *out = a[0];
        return true;
    }
    if (!fl_arg_num(vm, a, 0U, &x))
        return false;
    /*
     * NaN, the infinities and anything past i64 have no integer value,
     * and C's conversion is UNDEFINED for all of them -- so this is a
     * check, not politeness.  The bound is written as a double
     * comparison because (double)INT64_MAX rounds UP to 2^63.
     */
    if (x != x || x >= 9223372036854775808.0 || x < -9223372036854775808.0)
        return fl_raise(vm, "type",
                        "math.int: %g has no integer value", x);
    *out = FL_INT_V((i64)x);
    return true;
}

static bool m_float(FlVm *vm, FlValue *a, u32 n, FlValue *out)
{
    double x;

    (void)n;
    if (!fl_arg_num(vm, a, 0U, &x))
        return false;
    *out = num(x);
    return true;
}

static bool m_pow(FlVm *vm, FlValue *a, u32 n, FlValue *out)
{
    double x;
    double y;

    (void)n;
    if (!fl_arg_num(vm, a, 0U, &x) || !fl_arg_num(vm, a, 1U, &y))
        return false;
    /* ALWAYS float, even for int arguments: 2**64 does not fit an i64,
     * and a pow that sometimes overflows into a wrapped int is worse
     * than one that is predictably floating point. */
    *out = num(pow(x, y));
    return true;
}

static bool m_atan2(FlVm *vm, FlValue *a, u32 n, FlValue *out)
{
    double y;
    double x;

    (void)n;
    if (!fl_arg_num(vm, a, 0U, &y) || !fl_arg_num(vm, a, 1U, &x))
        return false;
    *out = num(atan2(y, x));
    return true;
}

static bool m_hypot(FlVm *vm, FlValue *a, u32 n, FlValue *out)
{
    double x;
    double y;

    (void)n;
    if (!fl_arg_num(vm, a, 0U, &x) || !fl_arg_num(vm, a, 1U, &y))
        return false;
    *out = num(hypot(x, y));
    return true;
}

static bool m_is_nan(FlVm *vm, FlValue *a, u32 n, FlValue *out)
{
    double x;

    (void)n;
    if (a[0].t == (u8)FL_INT) {
        *out = FL_BOOL_V(false);
        return true;
    }
    if (!fl_arg_num(vm, a, 0U, &x))
        return false;
    *out = FL_BOOL_V(x != x);
    return true;
}

static bool m_is_inf(FlVm *vm, FlValue *a, u32 n, FlValue *out)
{
    double x;

    (void)n;
    if (a[0].t == (u8)FL_INT) {
        *out = FL_BOOL_V(false);
        return true;
    }
    if (!fl_arg_num(vm, a, 0U, &x))
        return false;
    *out = FL_BOOL_V(x > 1.7976931348623157e308 ||
                     x < -1.7976931348623157e308);
    return true;
}

/* ---------------------------------------------------------------- */
/* The PRNG                                                         */
/* ---------------------------------------------------------------- */

/*
 * xorshift64*, and THE DEFAULT SEED IS A FIXED CONSTANT, never the
 * clock.
 *
 * Invariant 5: a config or a syntax definition that calls random must
 * behave identically on two machines and across the determinism lane's
 * double run.  Seeding from time() would make every such script a
 * different program on every launch, and the failure would show up as
 * an unreproducible rendering bug rather than as anything to do with
 * randomness.  Anyone who wants nondeterminism calls math.seed
 * explicitly and owns the consequence.
 */
static u64 rng_state = 0x2545F4914F6CDD1DULL;

static u64 rng_next(void)
{
    u64 x = rng_state;

    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    rng_state = x;
    return x * 0x2545F4914F6CDD1DULL;
}

static bool m_seed(FlVm *vm, FlValue *a, u32 n, FlValue *out)
{
    i64 s;

    (void)n;
    if (!fl_arg_int(vm, a, 0U, &s))
        return false;
    /* Zero is a fixed point of xorshift, so it is mapped away rather
     * than left to produce an all-zero stream nobody would debug. */
    rng_state = (u64)s == 0U ? 0x9E3779B97F4A7C15ULL : (u64)s;
    *out = FL_NIL_V;
    return true;
}

static bool m_random(FlVm *vm, FlValue *a, u32 n, FlValue *out)
{
    if (n == 0U) {
        /* 53 bits into [0,1): the mantissa's width, so every
         * representable double in the range is reachable and none is
         * reachable twice as often. */
        *out = num((double)(rng_next() >> 11) * (1.0 / 9007199254740992.0));
        return true;
    }
    if (n != 2U)
        return fl_raise(vm, "arity",
                        "math.random expects 0 or 2 arguments, got %u",
                        (unsigned)n);
    {
        i64 lo;
        i64 hi;
        u64 span;

        if (!fl_arg_int(vm, a, 0U, &lo) || !fl_arg_int(vm, a, 1U, &hi))
            return false;
        if (lo > hi)
            return fl_raise(vm, "type",
                            "math.random: lo must not exceed hi");
        span = (u64)hi - (u64)lo + 1U;      /* inclusive, cannot overflow
                                             * to 0 unless the full range
                                             * is asked for */
        *out = FL_INT_V(span == 0U ? (i64)rng_next()
                                   : lo + (i64)(rng_next() % span));
        return true;
    }
}

/* ---------------------------------------------------------------- */
/* The table                                                        */
/* ---------------------------------------------------------------- */

static const FlNativeDef MATH_DEFS[] = {
    {"abs",    m_abs,    1U, 1U, 0U, "(n) -> n"},
    {"min",    m_min,    1U, FL_VARIADIC, 0U, "(a, b, ...) -> n"},
    {"max",    m_max,    1U, FL_VARIADIC, 0U, "(a, b, ...) -> n"},
    {"clamp",  m_clamp,  3U, 3U, 0U, "(x, lo, hi) -> n"},
    {"sign",   m_sign,   1U, 1U, 0U, "(n) -> int"},
    {"floor",  m_floor,  1U, 1U, 0U, "(n) -> n"},
    {"ceil",   m_ceil,   1U, 1U, 0U, "(n) -> n"},
    {"round",  m_round,  1U, 1U, 0U, "(n) -> n"},
    {"trunc",  m_trunc,  1U, 1U, 0U, "(n) -> n"},
    {"int",    m_int,    1U, 1U, 0U, "(n) -> int"},
    {"float",  m_float,  1U, 1U, 0U, "(n) -> float"},
    {"sqrt",   m_sqrt,   1U, 1U, 0U, "(n) -> float"},
    {"exp",    m_exp,    1U, 1U, 0U, "(n) -> float"},
    {"log",    m_log,    1U, 1U, 0U, "(n) -> float"},
    {"log2",   m_log2,   1U, 1U, 0U, "(n) -> float"},
    {"log10",  m_log10,  1U, 1U, 0U, "(n) -> float"},
    {"pow",    m_pow,    2U, 2U, 0U, "(a, b) -> float"},
    {"sin",    m_sin,    1U, 1U, 0U, "(n) -> float"},
    {"cos",    m_cos,    1U, 1U, 0U, "(n) -> float"},
    {"tan",    m_tan,    1U, 1U, 0U, "(n) -> float"},
    {"asin",   m_asin,   1U, 1U, 0U, "(n) -> float"},
    {"acos",   m_acos,   1U, 1U, 0U, "(n) -> float"},
    {"atan",   m_atan,   1U, 1U, 0U, "(n) -> float"},
    {"atan2",  m_atan2,  2U, 2U, 0U, "(y, x) -> float"},
    {"hypot",  m_hypot,  2U, 2U, 0U, "(x, y) -> float"},
    {"is_nan", m_is_nan, 1U, 1U, 0U, "(n) -> bool"},
    {"is_inf", m_is_inf, 1U, 1U, 0U, "(n) -> bool"},
    {"seed",   m_seed,   1U, 1U, 0U, "(n) -> nil"},
    {"random", m_random, 0U, 2U, 0U, "([lo, hi]) -> float|int"}
};

/*
 * Constants, not zero-argument functions: `math.pi` reads as a value
 * because it is one, and spec §4's `.` already does the work.
 *
 * HUGE_VAL and NAN rather than arithmetic: `1e308 * 10.0` in a static
 * initializer is an overflowing constant expression, which is a
 * diagnosable constraint violation, and `0.0 / 0.0` is not a constant
 * expression at all.  The math.h macros are the portable spellings.
 */
static const FlConstDef MATH_CONSTS[] = {
    {"pi",      {(u8)FL_FLOAT, 0U, 0U, 0U, {.f = 3.14159265358979323846}}},
    {"e",       {(u8)FL_FLOAT, 0U, 0U, 0U, {.f = 2.71828182845904523536}}},
    {"inf",     {(u8)FL_FLOAT, 0U, 0U, 0U, {.f = HUGE_VAL}}},
    {"nan",     {(u8)FL_FLOAT, 0U, 0U, 0U, {.f = (double)NAN}}},
    {"int_max", {(u8)FL_INT,   0U, 0U, 0U, {.i = 9223372036854775807LL}}},
    {"int_min", {(u8)FL_INT,   0U, 0U, 0U,
                 {.i = -9223372036854775807LL - 1LL}}}
};

const FlModuleDef fl_mod_math = {
    "math", MATH_DEFS, (u32)YEW_ARRAY_LEN(MATH_DEFS),
    MATH_CONSTS, (u32)YEW_ARRAY_LEN(MATH_CONSTS)
};
