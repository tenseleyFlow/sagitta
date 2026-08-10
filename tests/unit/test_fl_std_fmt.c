/*
 * Sprint 31 deliverable 6: the `fmt` module.
 *
 * COVERAGE  (function -> test; every row of §6's table, every error kind)
 * --------
 * fmt.f     literals, {{ }}    fmt_f_copies_text_and_unescapes_braces
 * fmt.f     positional, named  fmt_f_resolves_arguments_three_ways
 * fmt.f     width/align/fill   fmt_f_aligns_in_display_cells
 * fmt.f     s d x X o b f e %  fmt_f_renders_every_type_letter
 * fmt.f     ?                  fmt_f_renders_every_type_letter
 * fmt.f     sign, zero pad     fmt_f_signs_and_zero_pads_numbers
 * fmt.f     "type"             fmt_f_refuses_a_malformed_directive
 * fmt.f     "index"            fmt_f_refuses_a_malformed_directive
 * fmt.f     "limit"            fmt_f_refuses_a_malformed_directive
 * fmt.str   scalars, bare str  fmt_str_is_display_and_never_raises
 * fmt.str   containers, cycles fmt_str_is_display_and_never_raises
 * fmt.repr  round-trips        fmt_repr_round_trips_through_the_literal_parser
 * fmt.repr  500 generated, deep fmt_repr_round_trips_500_generated_values
 * fmt.repr  escapes            fmt_repr_escapes_exactly_the_s25_table
 * fmt.repr  pretty form        fmt_repr_pretty_is_the_s25_workspace_format
 * fmt.repr  "type"             fmt_repr_raises_on_what_12_cannot_spell
 * fmt.int   base, width        fmt_int_hex_and_float_render_scalars
 * fmt.int   "type" / "limit"   fmt_int_hex_and_float_render_scalars
 * fmt.hex   width              fmt_int_hex_and_float_render_scalars
 * fmt.float prec, shortest     fmt_int_hex_and_float_render_scalars
 * fmt.float "type"             fmt_int_hex_and_float_render_scalars
 * fmt.pad   align, fill, cells fmt_pad_pads_in_cells
 * fmt.pad   "type" / "limit"   fmt_pad_pads_in_cells
 */
#include "flfix.h"

#include <stdio.h>
#include <string.h>

#include "fl/gc.h"
#include "fl/parse.h"
#include "util/intern.h"

#define P "import fmt\n"

void test_fl_fmt_f_copies_text_and_unescapes_braces(void)
{
    FlFix f;

    flfix_open(&f);
    FL_EQ(&f, P "return fmt.f(\"plain\")\n", "plain");
    FL_EQ(&f, P "return fmt.f(\"{{}}\")\n", "{}");
    FL_EQ(&f, P "return fmt.f(\"a{}b\", 1)\n", "a1b");
    FL_EQ(&f, P "return fmt.f(\"{} {} {}\", 1, 2, 3)\n", "1 2 3");
    flfix_close(&f);
}

void test_fl_fmt_f_resolves_arguments_three_ways(void)
{
    FlFix f;

    flfix_open(&f);
    /* Automatic, explicit and named.  The auto counter is independent
     * of the explicit indices, so mixing them is defined. */
    FL_EQ(&f, P "return fmt.f(\"{1} {0}\", \"a\", \"b\")\n", "b a");
    FL_EQ(&f, P "return fmt.f(\"{name}!\", {name: \"yew\"})\n", "yew!");
    FL_EQ(&f, P "return fmt.f(\"{0}{0}\", \"x\")\n", "xx");
    flfix_close(&f);
}

void test_fl_fmt_f_aligns_in_display_cells(void)
{
    FlFix f;

    flfix_open(&f);
    FL_EQ(&f, P "return fmt.f(\"[{:>5}]\", \"ab\")\n", "[   ab]");
    FL_EQ(&f, P "return fmt.f(\"[{:<5}]\", \"ab\")\n", "[ab   ]");
    /* The odd cell goes RIGHT, so centring a growing string moves it
     * one way only. */
    FL_EQ(&f, P "return fmt.f(\"[{:^5}]\", \"ab\")\n", "[ ab  ]");
    FL_EQ(&f, P "return fmt.f(\"[{:*^5}]\", \"ab\")\n", "[*ab**]");
    /* A fill that is itself an align character: only the second
     * character can tell the two readings apart. */
    FL_EQ(&f, P "return fmt.f(\"[{:^^5}]\", \"ab\")\n", "[^ab^^]");
    /*
     * CELLS, not bytes and not clusters.  Two CJK ideographs are six
     * bytes, two clusters and FOUR columns, and only the last number
     * lines a terminal up.
     */
    FL_EQ(&f, P "return fmt.f(\"[{:>10}]\", \"\\u{6F22}\\u{5B57}\")\n",
          "[      \xe6\xbc\xa2\xe5\xad\x97]");
    /* A value narrower than the field is untouched, never truncated. */
    FL_EQ(&f, P "return fmt.f(\"[{:>2}]\", \"abcd\")\n", "[abcd]");
    flfix_close(&f);
}

void test_fl_fmt_f_renders_every_type_letter(void)
{
    FlFix f;

    flfix_open(&f);
    FL_EQ(&f, P "return fmt.f(\"{:s}\", \"a\")\n", "a");
    FL_EQ(&f, P "return fmt.f(\"{:d}\", 42)\n", "42");
    /* §6: `d` takes a float only when it is integral -- rounding would
     * move a line number by one and never say so. */
    FL_EQ(&f, P "return fmt.f(\"{:d}\", 3.0)\n", "3");
    FL_EQ(&f, P "return fmt.f(\"{:d}\", 3.5)\n",
          "!type: fmt.f: {:d} at byte 0 needs an integral value");
    FL_EQ(&f, P "return fmt.f(\"{:x}\", 255)\n", "ff");
    FL_EQ(&f, P "return fmt.f(\"{:X}\", 255)\n", "FF");
    FL_EQ(&f, P "return fmt.f(\"{:o}\", 8)\n", "10");
    FL_EQ(&f, P "return fmt.f(\"{:b}\", 5)\n", "101");
    FL_EQ(&f, P "return fmt.f(\"{:.2f}\", 1.5)\n", "1.50");
    FL_EQ(&f, P "return fmt.f(\"{:.2e}\", 1234.0)\n", "1.23e+03");
    FL_EQ(&f, P "return fmt.f(\"{:.1%}\", 0.5)\n", "50.0%");
    /* `?` is repr, so a string comes back quoted. */
    FL_EQ(&f, P "return fmt.f(\"{:?}\", \"a\")\n", "\"a\"");
    FL_EQ(&f, P "return fmt.f(\"{:x}\", \"a\")\n",
          "!type: fmt.f: {:x} at byte 0 needs an int, found str");
    FL_EQ(&f, P "return fmt.f(\"{:f}\", \"a\")\n",
          "!type: fmt.f: {:f} at byte 0 needs a number, found str");
    flfix_close(&f);
}

void test_fl_fmt_f_signs_and_zero_pads_numbers(void)
{
    FlFix f;

    flfix_open(&f);
    FL_EQ(&f, P "return fmt.f(\"{:+d}\", 42)\n", "+42");
    FL_EQ(&f, P "return fmt.f(\"{: d}\", 42)\n", " 42");
    FL_EQ(&f, P "return fmt.f(\"{:+d}\", -42)\n", "-42");
    /* The zeros go AFTER the sign: -0042, never 00-42. */
    FL_EQ(&f, P "return fmt.f(\"{:08.3f}\", -1.5)\n", "-001.500");
    /*
     * "Numeric" is about the VALUE, not only the directive: a column of
     * integers written `{:5}` must line up, or it silently misaligns
     * whenever the writer forgets `:d`.
     */
    FL_EQ(&f, P "return fmt.f(\"[{:5}]\", 42)\n", "[   42]");
    FL_EQ(&f, P "return fmt.f(\"[{:<5}]\", 42)\n", "[42   ]");
    FL_EQ(&f, P "return fmt.f(\"[{:5}]\", \"ab\")\n", "[ab   ]");
    flfix_close(&f);
}

void test_fl_fmt_f_refuses_a_malformed_directive(void)
{
    FlFix f;

    flfix_open(&f);
    /*
     * A raise, never the bad directive as literal text.  A statusline
     * reading `{tabwidth` is a bug report either way, and the raise
     * names the byte offset.
     */
    FL_EQ(&f, P "return fmt.f(\"{:zz}\", 1)\n",
          "!type: fmt.f: bad directive \"{:zz}\" at byte 0");
    FL_EQ(&f, P "return fmt.f(\"ab {\")\n",
          "!type: fmt.f: bad directive \"{\" at byte 3");
    FL_EQ(&f, P "return fmt.f(\"ab }\")\n",
          "!type: fmt.f: bad directive \"}\" at byte 3");
    FL_EQ(&f, P "return fmt.f(\"{}\")\n",
          "!index: fmt.f: directive at byte 0 wants argument 0, 0 given");
    FL_EQ(&f, P "return fmt.f(\"{x}\", {y: 1})\n",
          "!index: fmt.f: no key \"x\" in argument 1");
    FL_EQ(&f, P "return fmt.f(\"{x}\", 1)\n",
          "!type: fmt.f: directive at byte 0 names \"x\", so argument 1 must "
          "be map, found int");
    /* A pad wider than a million cells is a runaway, not alignment. */
    FL_EQ(&f, P "return fmt.f(\"{:99999999}\", 1)\n",
          "!limit: fmt.f: field width at byte 0 exceeds 1048576 cells");
    flfix_close(&f);
}

void test_fl_fmt_str_is_display_and_never_raises(void)
{
    FlFix f;

    flfix_open(&f);
    FL_EQ(&f, P "return fmt.str(nil)\n", "nil");
    FL_EQ(&f, P "return fmt.str(true)\n", "true");
    FL_EQ(&f, P "return fmt.str(42)\n", "42");
    FL_EQ(&f, P "return fmt.str(-7)\n", "-7");
    /* A float always keeps its point, so 3.0 does not read back an
     * int and change the value's type. */
    FL_EQ(&f, P "return fmt.str(3.0)\n", "3.0");
    FL_EQ(&f, P "return fmt.str(0.1)\n", "0.1");
    FL_EQ(&f, P "return fmt.str(1.0e20)\n", "1.0e+20");
    /* Bare at the ROOT, quoted inside a container -- the quotes are
     * what keep ["a, b"] distinguishable from ["a", "b"]. */
    FL_EQ(&f, P "return fmt.str(\"hi\")\n", "hi");
    FL_EQ(&f, P "return fmt.str([1, \"a\", nil])\n", "[1, \"a\", nil]");
    FL_EQ(&f, P "return fmt.str({a: 1, b: [2]})\n", "{a: 1, b: [2]}");
    FL_EQ(&f, P "return fmt.str({})\n", "{}");
    FL_EQ(&f, P "return fmt.str([])\n", "[]");
    /* Display is best-effort BY DESIGN: a cycle elides rather than
     * raising, and a function prints as itself. */
    FL_EQ(&f, P "let l = [1]\nl[0] = l\nreturn fmt.str(l)\n", "[[...]]");
    FL_EQ(&f, P "return fmt.str(fmt.str)\n", "<fn fmt.str>");
    flfix_close(&f);
}

/*
 * THE repr LAW: the output re-reads as an equal value, or repr raises.
 *
 * Asserted here by parsing the output back with fl_parse_literal -- the
 * §12 entry point -- rather than by eyeballing the text.  A serializer
 * whose output only LOOKS right is one that fails on the day a state
 * file is loaded.
 */
static void repr_parses(FlFix *f, const char *expr)
{
    char src[1024];
    char out[4096];
    FlNode *n;
    u32 before = f->ndiag;

    (void)snprintf(src, sizeof(src), "import fmt\nreturn fmt.repr(%s)\n",
                   expr);
    flfix_run(f, src, out, sizeof(out));
    YEW_ASSERT(out[0] != '!');
    n = fl_parse_literal(&f->arena, &f->dc, &f->in, out, strlen(out), 0U);
    if (n == NULL)
        (void)fprintf(stderr, "repr did not re-read: %s\n", out);
    YEW_ASSERT_NOT_NULL(n);
    YEW_ASSERT_EQ_U64((u64)f->ndiag, (u64)before);
}

void test_fl_fmt_repr_round_trips_through_the_literal_parser(void)
{
    FlFix f;

    flfix_open(&f);
    repr_parses(&f, "nil");
    repr_parses(&f, "true");
    repr_parses(&f, "0");
    repr_parses(&f, "-1");
    repr_parses(&f, "0.1");
    repr_parses(&f, "3.0");
    repr_parses(&f, "1.0e20");
    repr_parses(&f, "0.0 - 0.0");
    repr_parses(&f, "\"\"");
    repr_parses(&f, "\"a\\nb\\t\\\"c\\\"\"");
    repr_parses(&f, "[]");
    repr_parses(&f, "{}");
    repr_parses(&f, "[1, [2, [3, []]]]");
    repr_parses(&f, "{a: 1, \"b c\": [nil, true], \"9\": {}}");
    repr_parses(&f, "{1: \"one\", 2: [2.5]}");
    flfix_close(&f);
}

void test_fl_fmt_repr_escapes_exactly_the_s25_table(void)
{
    FlFix f;

    flfix_open(&f);
    /*
     * `"` `\` `\n` `\t` `\r` `\0`, bytes below 0x20 and 0x7F as \xNN in
     * lowercase, and EVERY other byte verbatim.  Byte-oriented: valid
     * UTF-8 rides through unmangled and so does an invalid byte, which
     * is invariant 2 at the serializer.
     */
    FL_EQ(&f, P "return fmt.repr(\"a\\nb\")\n", "\"a\\nb\"");
    FL_EQ(&f, P "return fmt.repr(\"q\\\"\\\\\")\n", "\"q\\\"\\\\\"");
    FL_EQ(&f, P "return fmt.repr(\"\\t\\r\")\n", "\"\\t\\r\"");
    FL_EQ(&f, P "return fmt.repr(\"\\x01\\x7f\")\n", "\"\\x01\\x7f\"");
    FL_EQ(&f, P "return fmt.repr(\"a\\x80b\")\n", "\"a\x80" "b\"");
    FL_EQ(&f, P "return fmt.repr(\"\\u{6F22}\")\n", "\"\xe6\xbc\xa2\"");
    /* A key that lexes back as an IDENT is bare; anything else is
     * quoted, because `{nil: 1}` is a parse error and not the map it
     * looks like. */
    FL_EQ(&f, P "return fmt.repr({a: 1, \"b c\": 2, 3: 4})\n",
          "{a: 1, \"b c\": 2, 3: 4}");
    FL_EQ(&f, P "return fmt.repr({\"nil\": 1})\n", "{\"nil\": 1}");
    FL_EQ(&f, P "return fmt.repr({\"_x9\": 1})\n", "{_x9: 1}");
    flfix_close(&f);
}

void test_fl_fmt_repr_pretty_is_the_s25_workspace_format(void)
{
    FlFix f;

    flfix_open(&f);
    /*
     * Two spaces per level, one entry per line, `key: value`, a
     * trailing comma after EVERY element, closers on their own line at
     * the parent's indent.  These are Sprint 25's FORMAT rules, not
     * style: Sprint 36 swaps this in under the hand-written emitter and
     * must reproduce its corpus byte for byte.
     */
    FL_EQ(&f, P "return fmt.repr([1, [2, 3]], 2)\n",
          "[\n  1,\n  [\n    2,\n    3,\n  ],\n]");
    FL_EQ(&f, P "return fmt.repr({a: {b: 1}}, 2)\n",
          "{\n  a: {\n    b: 1,\n  },\n}");
    FL_EQ(&f, P "return fmt.repr([], 2)\n", "[\n]");
    FL_EQ(&f, P "return fmt.repr([1], 4)\n", "[\n    1,\n]");
    FL_EQ(&f, P "return fmt.repr(1, 20)\n",
          "!type: fmt.repr: indent must be 0..16, found 20");
    flfix_close(&f);
}

void test_fl_fmt_repr_raises_on_what_12_cannot_spell(void)
{
    FlFix f;

    flfix_open(&f);
    /* §12's grammar is narrower than §4's value space in five places,
     * and each raises rather than emitting text that will not parse. */
    FL_EQ(&f, P "return fmt.repr(fmt.repr)\n",
          "!type: fmt.repr: a fn has no pure-literal form");
    FL_EQ(&f, P "let l = [1]\nl[0] = l\nreturn fmt.repr(l)\n",
          "!type: fmt.repr: cycle at [0]");
    FL_EQ(&f, P "let m = {tabs: [{}]}\nm.tabs[0].parent = m\n"
              "return fmt.repr(m)\n",
          "!type: fmt.repr: cycle at .tabs[0].parent");
    FL_EQ(&f, P "import math\nreturn fmt.repr(math.nan)\n",
          "!type: fmt.repr: nan has no float literal");
    FL_EQ(&f, P "import math\nreturn fmt.repr(math.inf)\n",
          "!type: fmt.repr: inf has no float literal");
    FL_EQ(&f, P "import math\nreturn fmt.repr(math.int_min)\n",
          "!type: fmt.repr: -9223372036854775808 has no integer literal; "
          "its magnitude is one past the i64 maximum");
    FL_EQ(&f, P "import map\nlet m = {}\nmap.set(m, true, 1)\n"
              "return fmt.repr(m)\n",
          "!type: fmt.repr: map key true is a bool, which pure-literal "
          "syntax cannot spell");
    FL_EQ(&f, P "import map\nlet m = {}\nmap.set(m, 0 - 1, 1)\n"
              "return fmt.repr(m)\n",
          "!type: fmt.repr: map key -1 is negative, which pure-literal "
          "syntax cannot spell");
    /* Display shows all of them rather than refusing. */
    FL_EQ(&f, P "import map\nlet m = {}\nmap.set(m, true, 1)\n"
              "return fmt.str(m)\n", "{true: 1}");
    flfix_close(&f);
}

void test_fl_fmt_int_hex_and_float_render_scalars(void)
{
    FlFix f;

    flfix_open(&f);
    FL_EQ(&f, P "return fmt.int(255)\n", "255");
    FL_EQ(&f, P "return fmt.int(255, 16)\n", "ff");
    FL_EQ(&f, P "return fmt.int(255, 2)\n", "11111111");
    FL_EQ(&f, P "return fmt.int(35, 36)\n", "z");
    /* The zeros go after the sign here too, because fmt.hex routes
     * through this body and the two must not disagree. */
    FL_EQ(&f, P "return fmt.int(-5, 10, 4)\n", "-005");
    FL_EQ(&f, P "return fmt.int(5, 40)\n",
          "!type: fmt.int: base must be 2..36, found 40");
    FL_EQ(&f, P "return fmt.int(5, 10, 99999999)\n",
          "!limit: fmt.int: width must be 0..1048576, found 99999999");
    FL_EQ(&f, P "return fmt.hex(255)\n", "ff");
    FL_EQ(&f, P "return fmt.hex(255, 6)\n", "0000ff");
    /* A message must name the function the USER called. */
    FL_EQ(&f, P "return fmt.hex(5, 99999999)\n",
          "!limit: fmt.hex: width must be 0..1048576, found 99999999");
    /* Shortest text that reads back as the same double. */
    FL_EQ(&f, P "return fmt.float(1.0 / 3.0)\n", "0.3333333333333333");
    FL_EQ(&f, P "return fmt.float(1.5, 3)\n", "1.500");
    FL_EQ(&f, P "return fmt.float(2)\n", "2.0");
    FL_EQ(&f, P "return fmt.float(1.5, 99)\n",
          "!type: fmt.float: prec must be 0..17, found 99");
    flfix_close(&f);
}

void test_fl_fmt_pad_pads_in_cells(void)
{
    FlFix f;

    flfix_open(&f);
    FL_EQ(&f, P "return fmt.pad(\"ab\", 5)\n", "ab   ");
    FL_EQ(&f, P "return fmt.pad(\"ab\", 5, \">\")\n", "   ab");
    FL_EQ(&f, P "return fmt.pad(\"ab\", 5, \"^\", \".\")\n", ".ab..");
    FL_EQ(&f, P "return fmt.pad(\"ab\", 1)\n", "ab");
    /* Cells again: the ideographs are four columns wide. */
    FL_EQ(&f, P "return fmt.pad(\"\\u{6F22}\\u{5B57}\", 6, \">\")\n",
          "  \xe6\xbc\xa2\xe5\xad\x97");
    FL_EQ(&f, P "return fmt.pad(\"ab\", 5, \"!\")\n",
          "!type: fmt.pad: align must be \"<\", \">\" or \"^\"");
    /* A fill of more than one cluster has no single width to tile
     * with, so it is refused rather than silently truncated. */
    FL_EQ(&f, P "return fmt.pad(\"ab\", 5, \"<\", \"xy\")\n",
          "!type: fmt.pad: fill must be one grapheme cluster");
    FL_EQ(&f, P "return fmt.pad(\"ab\", 99999999)\n",
          "!limit: fmt.pad: width must be 0..1048576, found 99999999");
    flfix_close(&f);
}

/* ---------------------------------------------------------------- */
/* DoD 8: 500 generated values, compared DEEPLY                     */
/* ---------------------------------------------------------------- */

/*
 * xorshift64*, seeded fixed.  A generator taken from the clock would
 * make a failure unreproducible, and a round-trip bug found once and
 * never again is a bug still in the tree.
 */
static u64 gen_state = 0x9E3779B97F4A7C15ULL;

static u64 gen_next(void)
{
    gen_state ^= gen_state >> 12;
    gen_state ^= gen_state << 25;
    gen_state ^= gen_state >> 27;
    return gen_state * 0x2545F4914F6CDD1DULL;
}

static u32 gen_below(u32 n) { return (u32)(gen_next() % (u64)n); }

/* A random string, invalid bytes and NULs included -- the escapes are
 * exactly what the round trip has to survive. */
static FlValue gen_str(FlVm *vm)
{
    char buf[24];
    u32 n = gen_below((u32)sizeof(buf));
    u32 i;

    for (i = 0U; i < n; i++)
        buf[i] = (char)(u8)(gen_next() & 0xFFU);
    return FL_OBJ_V(FL_STR, fl_str_new(vm, buf, n));
}

/*
 * A value inside §12's grammar.
 *
 * Deliberately excludes the five shapes repr refuses -- non-finite
 * floats, INT64_MIN, bool and negative-int map keys, anything callable
 * -- because those are asserted to RAISE in
 * fmt_repr_raises_on_what_12_cannot_spell.  Mixing them in here would
 * test the refusal twice and the round trip not at all.
 */
static FlValue gen_value(FlVm *vm, u32 depth)
{
    u32 pick = gen_below(depth == 0U ? 5U : 7U);

    switch (pick) {
    case 0: return FL_NIL_V;
    case 1: return FL_BOOL_V((gen_next() & 1U) != 0U);
    case 2: return FL_INT_V((i64)gen_next() | 1);   /* never INT64_MIN */
    case 3: {
        double d;
        u64 bits = gen_next();

        (void)memcpy(&d, &bits, sizeof(d));
        /* Finite only; repr has no spelling for the rest. */
        if (d != d || d > 1.7976931348623157e308 ||
            d < -1.7976931348623157e308)
            d = (double)(i64)(gen_next() & 0xFFFFU) / 8.0;
        return FL_FLOAT_V(d);
    }
    case 4: return gen_str(vm);
    case 5: {
        FlList *l = fl_list_new(vm);
        u32 n = gen_below(4U);
        u32 i;

        fl_gc_protect(vm, FL_OBJ_V(FL_LIST, l));
        for (i = 0U; i < n; i++)
            (void)fl_list_push(vm, l, gen_value(vm, depth - 1U));
        fl_gc_release(vm, 1U);
        return FL_OBJ_V(FL_LIST, l);
    }
    default: {
        FlMap *m = fl_map_new(vm);
        u32 n = gen_below(4U);
        u32 i;

        fl_gc_protect(vm, FL_OBJ_V(FL_MAP, m));
        for (i = 0U; i < n; i++) {
            /* String and NON-NEGATIVE int keys: §12's pl_entry has no
             * sign and no bool. */
            FlValue k = (gen_next() & 1U) != 0U
                            ? gen_str(vm)
                            : FL_INT_V((i64)(gen_next() & 0xFFFFU));

            (void)fl_map_set(vm, m, k, gen_value(vm, depth - 1U));
        }
        fl_gc_release(vm, 1U);
        return FL_OBJ_V(FL_MAP, m);
    }
    }
}

/* The parsed literal, back as a value.  Only §12's productions appear,
 * which is what makes this a dozen lines rather than an evaluator. */
static bool ast_to_value(FlFix *f, const FlNode *n, FlValue *out)
{
    if (n == NULL)
        return false;
    switch ((FlAstKind)n->kind) {
    case FL_A_LIT:
        switch ((FlLitKind)n->as.lit.lit) {
        case FL_L_NIL:   *out = FL_NIL_V; return true;
        case FL_L_BOOL:  *out = FL_BOOL_V(n->as.lit.v.b); return true;
        case FL_L_INT:   *out = FL_INT_V(n->as.lit.v.i); return true;
        case FL_L_FLOAT: *out = FL_FLOAT_V(n->as.lit.v.f); return true;
        default: {
            u32 id = n->as.lit.v.str_id;
            const char *b = yew_intern_str(&f->in, id);

            *out = FL_OBJ_V(FL_STR,
                            fl_str_new(&f->vm, b == NULL ? "" : b,
                                       (u32)yew_intern_len(&f->in, id)));
            return true;
        }
        }
    case FL_A_LIST: {
        FlList *l = fl_list_new(&f->vm);
        u32 i;

        fl_gc_protect(&f->vm, FL_OBJ_V(FL_LIST, l));
        for (i = 0U; i < n->as.list.n; i++) {
            FlValue v;

            if (!ast_to_value(f, n->as.list.items[i], &v)) {
                fl_gc_release(&f->vm, 1U);
                return false;
            }
            (void)fl_list_push(&f->vm, l, v);
        }
        fl_gc_release(&f->vm, 1U);
        *out = FL_OBJ_V(FL_LIST, l);
        return true;
    }
    case FL_A_MAP: {
        FlMap *m = fl_map_new(&f->vm);
        u32 i;

        fl_gc_protect(&f->vm, FL_OBJ_V(FL_MAP, m));
        for (i = 0U; i < n->as.map.n; i++) {
            FlValue k;
            FlValue v;

            if (!ast_to_value(f, n->as.map.keys[i], &k) ||
                !ast_to_value(f, n->as.map.vals[i], &v)) {
                fl_gc_release(&f->vm, 1U);
                return false;
            }
            (void)fl_map_set(&f->vm, m, k, v);
        }
        fl_gc_release(&f->vm, 1U);
        *out = FL_OBJ_V(FL_MAP, m);
        return true;
    }
    default:
        return false;
    }
}

/* fl_equal compares containers by REFERENCE, so the round trip needs
 * its own recursion -- and it checks map ORDER, because insertion order
 * is part of what repr promises to preserve. */
static bool deep_equal(FlValue a, FlValue b)
{
    if (a.t != b.t)
        return false;
    switch ((FlType)a.t) {
    case FL_LIST: {
        const FlList *x = (const FlList *)a.as.o;
        const FlList *y = (const FlList *)b.as.o;
        u32 i;

        if (x->n != y->n)
            return false;
        for (i = 0U; i < x->n; i++) {
            if (!deep_equal(x->v[i], y->v[i]))
                return false;
        }
        return true;
    }
    case FL_MAP: {
        FlMap *x = (FlMap *)a.as.o;
        FlMap *y = (FlMap *)b.as.o;
        u32 cx = 0U;
        u32 cy = 0U;
        FlValue kx;
        FlValue vx;
        FlValue ky;
        FlValue vy;

        if (fl_map_count(x) != fl_map_count(y))
            return false;
        while (fl_map_iter(x, &cx, &kx, &vx)) {
            if (!fl_map_iter(y, &cy, &ky, &vy))
                return false;
            if (!deep_equal(kx, ky) || !deep_equal(vx, vy))
                return false;
        }
        return true;
    }
    case FL_FLOAT: {
        u64 p;
        u64 q;

        /* Bit-exact, so -0.0 does not pass for 0.0: repr writes them
         * differently and the round trip must keep them apart. */
        (void)memcpy(&p, &a.as.f, sizeof(p));
        (void)memcpy(&q, &b.as.f, sizeof(q));
        return p == q;
    }
    default:
        return fl_equal(a, b);
    }
}

void test_fl_fmt_repr_round_trips_500_generated_values(void)
{
    FlFix f;
    u32 round;

    flfix_open(&f);
    gen_state = 0x9E3779B97F4A7C15ULL;
    for (round = 0U; round < 500U; round++) {
        FlValue v;
        FlValue back = FL_NIL_V;
        FlValue out = FL_NIL_V;
        FlNode *node;
        Bytebuf text;
        u32 before = f.ndiag;

        v = gen_value(&f.vm, 3U);
        fl_gc_protect(&f.vm, v);
        bytebuf_init(&text);
        /* The serializer directly: a Fletch program would have to
         * escape arbitrary generated bytes into source first, which
         * would test the escaping rather than the round trip. */
        if (!fl_fmt_repr(&f.vm, &text, v)) {
            bytebuf_free(&text);
            fl_gc_release(&f.vm, 1U);
            YEW_ASSERT(false);
            continue;
        }
        node = fl_parse_literal(&f.arena, &f.dc, &f.in,
                                (const char *)text.data, text.len, 0U);
        if (node == NULL)
            (void)fprintf(stderr, "round %u did not re-read: %.*s\n",
                          (unsigned)round, (int)text.len,
                          (const char *)text.data);
        YEW_ASSERT_NOT_NULL(node);
        YEW_ASSERT_EQ_U64((u64)f.ndiag, (u64)before);
        YEW_ASSERT(ast_to_value(&f, node, &back));
        if (!deep_equal(v, back))
            (void)fprintf(stderr, "round %u did not compare equal: %.*s\n",
                          (unsigned)round, (int)text.len,
                          (const char *)text.data);
        YEW_ASSERT(deep_equal(v, back));
        (void)out;
        bytebuf_free(&text);
        fl_gc_release(&f.vm, 1U);
    }
    flfix_close(&f);
}
