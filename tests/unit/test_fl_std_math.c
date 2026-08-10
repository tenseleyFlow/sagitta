/*
 * Sprint 31 deliverable 5: the `math` module.
 *
 * COVERAGE  (function -> test; every row of §5's table, every error kind)
 * --------
 * math.abs    type-preserving    math_keeps_the_type_it_was_given
 * math.min    variadic           math_keeps_the_type_it_was_given
 * math.max    variadic           math_keeps_the_type_it_was_given
 * math.clamp                     math_keeps_the_type_it_was_given
 * math.sign   always int         math_keeps_the_type_it_was_given
 * math.floor  type-preserving    math_rounds_toward_the_named_direction
 * math.ceil                      math_rounds_toward_the_named_direction
 * math.round  half away from 0   math_rounds_toward_the_named_direction
 * math.trunc                     math_rounds_toward_the_named_direction
 * math.int    "type" on nan/inf  math_converts_between_int_and_float
 * math.float                     math_converts_between_int_and_float
 * math.sqrt   nan on negative    math_transcendentals_follow_ieee
 * math.exp                       math_transcendentals_follow_ieee
 * math.log log2 log10            math_transcendentals_follow_ieee
 * math.pow                       math_transcendentals_follow_ieee
 * math.sin cos tan               math_transcendentals_follow_ieee
 * math.asin acos atan atan2      math_transcendentals_follow_ieee
 * math.hypot                     math_transcendentals_follow_ieee
 * math.is_nan is_inf             math_classifies_the_edges
 * pi e inf nan int_max int_min   math_classifies_the_edges
 * math.seed   determinism        math_random_is_deterministic
 * math.random range, int form    math_random_is_deterministic
 * math.random pinned sequence   math_random_is_deterministic
 *
 * A wrong-typed argument is worded once, by fl_arg_*, and asserted once
 * here rather than twenty-nine times.
 */
#include "flfix.h"

#define P "import math\nimport fmt\n"

void test_fl_math_keeps_the_type_it_was_given(void)
{
    FlFix f;

    flfix_open(&f);
    /*
     * TYPE-PRESERVING.  abs of an int is an int, so a config that does
     * arithmetic on line numbers does not silently acquire a float and
     * start printing "40.0" in the statusline.
     */
    FL_EQ(&f, P "return math.abs(-3)\n", "3");
    FL_EQ(&f, P "return fmt.str(math.abs(-3.5))\n", "3.5");
    FL_EQ(&f, P "return math.min(3, 1, 2)\n", "1");
    FL_EQ(&f, P "return math.max(3, 1, 2)\n", "3");
    FL_EQ(&f, P "return math.min(5)\n", "5");
    FL_EQ(&f, P "return math.clamp(9, 0, 8)\n", "8");
    FL_EQ(&f, P "return math.clamp(-1, 0, 8)\n", "0");
    FL_EQ(&f, P "return math.clamp(4, 0, 8)\n", "4");
    /* sign is always an int, whatever went in: it answers a
     * three-valued question, not an arithmetic one. */
    FL_EQ(&f, P "return math.sign(-2.5)\n", "-1");
    FL_EQ(&f, P "return math.sign(0)\n", "0");
    FL_EQ(&f, P "return math.sign(7)\n", "1");
    FL_EQ(&f, P "return math.abs(\"a\")\n",
          "!type: math.abs: argument 1 must be number, found str");
    flfix_close(&f);
}

void test_fl_math_rounds_toward_the_named_direction(void)
{
    FlFix f;

    flfix_open(&f);
    FL_EQ(&f, P "return fmt.str(math.floor(1.7))\n", "1.0");
    FL_EQ(&f, P "return fmt.str(math.floor(-1.2))\n", "-2.0");
    FL_EQ(&f, P "return fmt.str(math.ceil(1.2))\n", "2.0");
    FL_EQ(&f, P "return fmt.str(math.ceil(-1.7))\n", "-1.0");
    /* Half away from zero, which is what a reader expects of "round"
     * and what banker's rounding is not. */
    FL_EQ(&f, P "return fmt.str(math.round(1.5))\n", "2.0");
    FL_EQ(&f, P "return fmt.str(math.round(-1.5))\n", "-2.0");
    FL_EQ(&f, P "return fmt.str(math.round(2.5))\n", "3.0");
    /* trunc goes toward zero, matching int division in §4. */
    FL_EQ(&f, P "return fmt.str(math.trunc(1.7))\n", "1.0");
    FL_EQ(&f, P "return fmt.str(math.trunc(-1.7))\n", "-1.0");
    /* An int is already rounded, and stays an int. */
    FL_EQ(&f, P "return math.floor(3)\n", "3");
    FL_EQ(&f, P "return math.round(3)\n", "3");
    flfix_close(&f);
}

void test_fl_math_converts_between_int_and_float(void)
{
    FlFix f;

    flfix_open(&f);
    FL_EQ(&f, P "return math.int(3.9)\n", "3");
    FL_EQ(&f, P "return math.int(-3.9)\n", "-3");
    FL_EQ(&f, P "return math.int(4)\n", "4");
    FL_EQ(&f, P "return fmt.str(math.float(3))\n", "3.0");
    FL_EQ(&f, P "return fmt.str(math.float(3.5))\n", "3.5");
    /* nan and the infinities have no int, and a conversion that
     * invented one would put a garbage line number somewhere. */
    FL_EQ(&f, P "return math.int(math.nan)\n",
          "!type: math.int: nan has no integer value");
    FL_EQ(&f, P "return math.int(math.inf)\n",
          "!type: math.int: inf has no integer value");
    flfix_close(&f);
}

void test_fl_math_transcendentals_follow_ieee(void)
{
    FlFix f;

    flfix_open(&f);
    FL_EQ(&f, P "return fmt.str(math.sqrt(9))\n", "3.0");
    FL_EQ(&f, P "return math.is_nan(math.sqrt(0 - 1))\n", "true");
    FL_EQ(&f, P "return fmt.str(math.exp(0))\n", "1.0");
    FL_EQ(&f, P "return fmt.str(math.log(1))\n", "0.0");
    FL_EQ(&f, P "return fmt.str(math.log2(8))\n", "3.0");
    FL_EQ(&f, P "return fmt.str(math.log10(1000))\n", "3.0");
    FL_EQ(&f, P "return fmt.str(math.pow(2, 10))\n", "1024.0");
    FL_EQ(&f, P "return fmt.str(math.sin(0))\n", "0.0");
    FL_EQ(&f, P "return fmt.str(math.cos(0))\n", "1.0");
    FL_EQ(&f, P "return fmt.str(math.tan(0))\n", "0.0");
    FL_EQ(&f, P "return fmt.str(math.asin(0))\n", "0.0");
    FL_EQ(&f, P "return fmt.str(math.acos(1))\n", "0.0");
    FL_EQ(&f, P "return fmt.str(math.atan(0))\n", "0.0");
    FL_EQ(&f, P "return fmt.str(math.atan2(0, 1))\n", "0.0");
    FL_EQ(&f, P "return fmt.str(math.hypot(3, 4))\n", "5.0");
    /*
     * A domain error is a NAN, not a raise.  IEEE 754 says so and every
     * other language agrees; making it catchable would put a try around
     * arithmetic.
     */
    FL_EQ(&f, P "return math.is_nan(math.log(0 - 1))\n", "true");
    FL_EQ(&f, P "return math.is_nan(math.asin(2))\n", "true");
    FL_EQ(&f, P "return math.is_inf(math.log(0))\n", "true");
    flfix_close(&f);
}

void test_fl_math_classifies_the_edges(void)
{
    FlFix f;

    flfix_open(&f);
    FL_EQ(&f, P "return math.is_nan(math.nan)\n", "true");
    FL_EQ(&f, P "return math.is_nan(1.0)\n", "false");
    FL_EQ(&f, P "return math.is_nan(1)\n", "false");
    FL_EQ(&f, P "return math.is_inf(math.inf)\n", "true");
    FL_EQ(&f, P "return math.is_inf(0 - math.inf)\n", "true");
    FL_EQ(&f, P "return math.is_inf(1.0)\n", "false");
    /* The constants are fields of the module map, read by the same `.`
     * as the functions -- a module is an ordinary frozen map. */
    FL_EQ(&f, P "return math.pi > 3.14 and math.pi < 3.15\n", "true");
    FL_EQ(&f, P "return math.e > 2.71 and math.e < 2.72\n", "true");
    FL_EQ(&f, P "return math.int_max\n", "9223372036854775807");
    FL_EQ(&f, P "return math.int_min\n", "-9223372036854775808");
    /* Frozen, so a script cannot redefine pi for everyone else. */
    FL_EQ(&f, P "math.pi = 3\nreturn 1\n", "!type: object is frozen");
    flfix_close(&f);
}

void test_fl_math_random_is_deterministic(void)
{
    FlFix f;

    flfix_open(&f);
    /*
     * SAME SEED, SAME SEQUENCE.  A generator seeded from the clock
     * would make a config that shuffles anything unreproducible, and
     * invariant 5 does not have an exception for randomness.
     */
    FL_EQ(&f, P "import list\n"
                "math.seed(1)\nlet a = [math.random(), math.random()]\n"
                "math.seed(1)\nlet b = [math.random(), math.random()]\n"
                "return a[0] == b[0] and a[1] == b[1]\n", "true");
    /* Two draws from one seed differ, or it is not a generator. */
    FL_EQ(&f, P "math.seed(1)\nreturn math.random() != math.random()\n",
          "true");
    /* A different seed gives a different sequence. */
    FL_EQ(&f, P "math.seed(1)\nlet a = math.random()\n"
                "math.seed(2)\nreturn a != math.random()\n", "true");
    /* No arguments: a float in [0, 1). */
    FL_EQ(&f, P "math.seed(7)\nlet r = math.random()\n"
                "return r >= 0.0 and r < 1.0\n", "true");
    /*
     * THE COMMITTED GOLDEN, DoD 12.
     *
     * Same-seed-twice proves the generator is a function of its seed;
     * only a PINNED SEQUENCE catches a change to the generator itself,
     * which would silently reshuffle anything a config had recorded.
     *
     * Seeded explicitly rather than taken from the default, because
     * `rng_state` is a file-static and therefore process-global: the
     * pristine default sequence is only observable in a fresh process,
     * and this binary has run other tests first.  `yew fl` is where
     * that form is visible, and seeding here tests the same thing --
     * the generator's output for a known state.
     */
    flfix_close(&f);
    flfix_open(&f);
    FL_EQ(&f, P "import list\nimport str\n"
                "math.seed(1)\n"
                "let r = []\nlet i = 0\n"
                "while i < 5 { list.push(r, fmt.float(math.random()))\n"
                "  i = i + 1 }\n"
                "return str.join(r, \" \")\n",
          "0.28083505005035947 0.6711372530266764 0.7258461452833668 "
          "0.303529299965799 0.056176763098259475");

    /* Two arguments: an int in [lo, hi]. */
    FL_EQ(&f, P "math.seed(7)\nlet ok = true\nlet i = 0\n"
                "while i < 100 {\n"
                "  let r = math.random(3, 5)\n"
                "  if r < 3 or r > 5 { ok = false }\n"
                "  i = i + 1\n"
                "}\nreturn ok\n", "true");
    flfix_close(&f);
}
