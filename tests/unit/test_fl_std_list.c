/*
 * Sprint 31 deliverable 3: the `list` module.
 *
 * COVERAGE  (function -> test; every row of §3's table, every error kind)
 * --------
 * list.len                        list_grows_and_shrinks
 * list.push                       list_grows_and_shrinks
 * list.pop       "index" on empty list_grows_and_shrinks
 * list.insert    "index"          list_grows_and_shrinks
 * list.remove    "index"          list_grows_and_shrinks
 * list.clear                      list_grows_and_shrinks
 * list.get       default, "index" list_reads_without_mutating
 * list.index_of  -1 on a miss     list_reads_without_mutating
 * list.contains                   list_reads_without_mutating
 * list.slice     bounds, "index"  list_reads_without_mutating
 * list.concat                     list_reads_without_mutating
 * list.reverse   in place         list_reads_without_mutating
 * list.copy      shallow          list_reads_without_mutating
 * list.sort      stable, total    list_sorts_stably_over_a_total_order
 * list.sort      comparator       list_sorts_stably_over_a_total_order
 * list.map                        list_walks_with_a_function
 * list.filter                     list_walks_with_a_function
 * list.reduce                     list_walks_with_a_function
 * list.any                        list_walks_with_a_function
 * list.all                        list_walks_with_a_function
 * iteration guard                 list_refuses_a_reshape_mid_walk
 */
#include "flfix.h"

#define P "import list\nimport fmt\n"

void test_fl_list_grows_and_shrinks(void)
{
    FlFix f;

    flfix_open(&f);
    FL_EQ(&f, P "return list.len([])\n", "0");
    FL_EQ(&f, P "let l = []\nlist.push(l, 1)\nlist.push(l, 2)\n"
                "return fmt.repr(l)\n", "[1, 2]");
    FL_EQ(&f, P "let l = [1, 2]\nreturn list.pop(l)\n", "2");
    FL_EQ(&f, P "return list.pop([])\n", "!index: list.pop: the list is empty");
    FL_EQ(&f, P "let l = [1, 3]\nlist.insert(l, 1, 2)\nreturn fmt.repr(l)\n",
          "[1, 2, 3]");
    /* Inserting AT the length appends; past it is an error, because a
     * silently clamped index hides an off-by-one forever. */
    FL_EQ(&f, P "let l = [1]\nlist.insert(l, 1, 2)\nreturn fmt.repr(l)\n",
          "[1, 2]");
    FL_EQ(&f, P "return list.insert([1], 5, 2)\n",
          "!index: index 5 out of range for length 1");
    FL_EQ(&f, P "let l = [1, 2, 3]\nreturn list.remove(l, 1)\n", "2");
    FL_EQ(&f, P "return list.remove([1], 9)\n",
          "!index: index 9 out of range for length 1");
    FL_EQ(&f, P "let l = [1, 2]\nlist.clear(l)\nreturn list.len(l)\n", "0");
    flfix_close(&f);
}

void test_fl_list_reads_without_mutating(void)
{
    FlFix f;

    flfix_open(&f);
    FL_EQ(&f, P "return list.get([1, 2], 1)\n", "2");
    FL_EQ(&f, P "return list.get([1], 9, \"d\")\n", "d");
    FL_EQ(&f, P "return list.get([1], 9)\n",
          "!index: index 9 out of range for length 1");
    /* -1 on a miss rather than a raise: "where is it" has a natural
     * answer for "nowhere", and index_of is used in conditions. */
    FL_EQ(&f, P "return list.index_of([1, 2, 1], 1)\n", "0");
    FL_EQ(&f, P "return list.index_of([1], 9)\n", "-1");
    FL_EQ(&f, P "return list.contains([1, 2], 2)\n", "true");
    FL_EQ(&f, P "return list.contains([1, 2], 9)\n", "false");
    /* §5.2 equality, so an int finds a float of the same value. */
    FL_EQ(&f, P "return list.contains([1, 2], 2.0)\n", "true");
    FL_EQ(&f, P "return fmt.repr(list.slice([1, 2, 3, 4], 1, 3))\n", "[2, 3]");
    FL_EQ(&f, P "return fmt.repr(list.slice([1, 2, 3], 1))\n", "[2, 3]");
    /* An empty tail is legal; a bound past the end is NOT clamped but
     * refused, for the same reason insert refuses one -- a silently
     * shortened slice hides an off-by-one forever. */
    FL_EQ(&f, P "return fmt.repr(list.slice([1, 2], 2))\n", "[]");
    FL_EQ(&f, P "return list.slice([1, 2], 0, 99)\n",
          "!index: index 99 out of range for length 2");
    FL_EQ(&f, P "return fmt.repr(list.concat([1], [2, 3]))\n", "[1, 2, 3]");
    FL_EQ(&f, P "let l = [1, 2, 3]\nlist.reverse(l)\nreturn fmt.repr(l)\n",
          "[3, 2, 1]");
    /* Shallow: the copy is a new list holding the same values. */
    FL_EQ(&f, P "let a = [1]\nlet b = list.copy(a)\nlist.push(b, 2)\n"
                "return list.len(a)\n", "1");
    flfix_close(&f);
}

void test_fl_list_sorts_stably_over_a_total_order(void)
{
    FlFix f;

    flfix_open(&f);
    FL_EQ(&f, P "let l = [3, 1, 2]\nlist.sort(l)\nreturn fmt.repr(l)\n",
          "[1, 2, 3]");
    FL_EQ(&f, P "let l = [\"b\", \"a\"]\nlist.sort(l)\nreturn fmt.repr(l)\n",
          "[\"a\", \"b\"]");
    /*
     * A TOTAL order over mixed types, so sorting never raises: values
     * order by type band first, then by value.  Ordering
     * otherwise-incomparable objects by ADDRESS would differ between
     * runs and break invariant 5, so the collector's allocation
     * sequence is the tie-breaker instead.
     */
    FL_EQ(&f, P "let l = [\"s\", 1, nil, true]\nlist.sort(l)\n"
                "return fmt.repr(l)\n", "[nil, true, 1, \"s\"]");
    /* A comparator returning <0, 0, >0. */
    FL_EQ(&f, P "let l = [1, 2, 3]\n"
                "list.sort(l, fn(a, b) { return b - a })\n"
                "return fmt.repr(l)\n", "[3, 2, 1]");
    /*
     * STABLE: equal elements keep their relative order.  `qsort` is
     * banned project-wide for exactly this reason, and a sort keyed on
     * one field of a pair is where the difference shows.
     */
    FL_EQ(&f, P "let l = [[1, \"a\"], [0, \"b\"], [1, \"c\"], [0, \"d\"]]\n"
                "list.sort(l, fn(x, y) { return x[0] - y[0] })\n"
                "return fmt.repr(l)\n",
          "[[0, \"b\"], [0, \"d\"], [1, \"a\"], [1, \"c\"]]");
    flfix_close(&f);
}

void test_fl_list_walks_with_a_function(void)
{
    FlFix f;

    flfix_open(&f);
    FL_EQ(&f, P "return fmt.repr(list.map([1, 2], fn(x) { return x * 2 }))\n",
          "[2, 4]");
    FL_EQ(&f, P "return fmt.repr(list.filter([1, 2, 3], "
                "fn(x) { return x > 1 }))\n", "[2, 3]");
    FL_EQ(&f, P "return list.reduce([1, 2, 3], "
                "fn(a, x) { return a + x }, 0)\n", "6");
    FL_EQ(&f, P "return list.any([1, 2], fn(x) { return x > 1 })\n", "true");
    FL_EQ(&f, P "return list.any([1, 2], fn(x) { return x > 9 })\n", "false");
    FL_EQ(&f, P "return list.all([1, 2], fn(x) { return x > 0 })\n", "true");
    FL_EQ(&f, P "return list.all([1, 2], fn(x) { return x > 1 })\n", "false");
    /* Vacuous truth, because `all` of nothing is what every fold
     * gives and the alternative surprises somebody every time. */
    FL_EQ(&f, P "return list.all([], fn(x) { return false })\n", "true");
    FL_EQ(&f, P "return list.any([], fn(x) { return true })\n", "false");
    /* A raise inside the callback propagates rather than being
     * swallowed into a half-built result. */
    FL_EQ(&f, P "return list.map([1], fn(x) { return x + \"a\" })\n",
          "!type: cannot add int and str");
    flfix_close(&f);
}

void test_fl_list_refuses_a_reshape_mid_walk(void)
{
    FlFix f;

    flfix_open(&f);
    /*
     * A container reshaped underneath an iteration is refused with kind
     * "index" -- not "type", which a caller cannot tell from an
     * argument error.  Element assignment is NOT a reshape, so a loop
     * that rewrites in place keeps working.
     */
    FL_EQ(&f, P "let l = [1, 2, 3]\nfor x in l { list.push(l, x) }\nreturn 1\n",
          "!index: list modified during iteration");
    FL_EQ(&f, P "let l = [1, 2, 3]\nfor x in l { l[0] = 9 }\nreturn l[0]\n",
          "9");
    flfix_close(&f);
}
