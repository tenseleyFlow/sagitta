/*
 * Sprint 31 deliverable 4: the `map` module.
 *
 * COVERAGE  (function -> test; every row of §4's table, every error kind)
 * --------
 * map.len      live entries only  map_len_counts_live_entries
 * map.get      default, "key"     map_reads_and_writes_by_key
 * map.set      "key", frozen      map_reads_and_writes_by_key
 * map.has                         map_reads_and_writes_by_key
 * map.remove   "key"              map_reads_and_writes_by_key
 * map.clear    frozen             map_len_counts_live_entries
 * map.keys     insertion order    map_iterates_in_insertion_order
 * map.values   insertion order    map_iterates_in_insertion_order
 * map.entries  insertion order    map_iterates_in_insertion_order
 * map.merge    a's order, b's val map_merges_copies_and_freezes
 * map.copy     shallow, unfrozen  map_merges_copies_and_freezes
 * map.freeze   in place, same map map_merges_copies_and_freezes
 */
#include "flfix.h"

#define P "import map\nimport fmt\n"

void test_fl_map_len_counts_live_entries(void)
{
    FlFix f;

    flfix_open(&f);
    FL_EQ(&f, P "return map.len({})\n", "0");
    FL_EQ(&f, P "return map.len({a: 1, b: 2})\n", "2");
    /*
     * LIVE entries.  A deleted key leaves a tombstone in the dense
     * array, and reporting the raw length would make len() grow every
     * time a key was deleted and re-added.
     */
    FL_EQ(&f, P "let m = {a: 1, b: 2}\nmap.remove(m, \"a\")\n"
                "map.set(m, \"a\", 9)\nreturn map.len(m)\n", "2");
    FL_EQ(&f, P "let m = {a: 1, b: 2}\nmap.clear(m)\nreturn map.len(m)\n", "0");
    /*
     * CLEAR AT SEVERAL SIZES, and this row is why.
     *
     * The two-entry case above passed while clear was a walk that
     * deleted as it went -- fl_map_del compacts once more than half
     * the entries are dead, which moves live entries down past the
     * cursor, so the tail survived.  n=2 is the one size where the
     * arithmetic happens to come out right; n=3 left one behind and
     * n=5 left two.  Sprint 33's conformance suite found it.
     */
    FL_EQ(&f, P "let m = {a: 1, b: 2, c: 3}\nmap.clear(m)\n"
                "return map.len(m)\n", "0");
    FL_EQ(&f, P "let m = {a: 1, b: 2, c: 3, d: 4, e: 5}\nmap.clear(m)\n"
                "return map.len(m)\n", "0");
    /* Cleared and reusable, not merely counted as empty: the index has
     * to be cleared with the entries or a stale slot resurrects a key. */
    FL_EQ(&f, P "let m = {a: 1, b: 2, c: 3}\nmap.clear(m)\n"
                "map.set(m, \"a\", 7)\nreturn fmt.repr(m)\n", "{a: 7}");
    FL_EQ(&f, P "let m = {a: 1, b: 2, c: 3}\nmap.clear(m)\n"
                "return map.has(m, \"c\")\n", "false");
    /* The builtin modules are frozen maps, so `math.pi = 3` and
     * `map.clear(math)` must both be errors a user sees. */
    FL_EQ(&f, P "import math\nreturn map.clear(math)\n",
          "!type: map.clear: the map is frozen");
    flfix_close(&f);
}

void test_fl_map_reads_and_writes_by_key(void)
{
    FlFix f;

    flfix_open(&f);
    FL_EQ(&f, P "return map.get({a: 1}, \"a\")\n", "1");
    FL_EQ(&f, P "return map.get({a: 1}, \"z\", 7)\n", "7");
    FL_EQ(&f, P "return map.get({a: 1}, \"z\")\n", "!key: map.get: no such key");
    FL_EQ(&f, P "return map.has({a: 1}, \"a\")\n", "true");
    FL_EQ(&f, P "return map.has({a: 1}, \"z\")\n", "false");
    FL_EQ(&f, P "let m = {}\nmap.set(m, \"k\", 5)\nreturn map.get(m, \"k\")\n",
          "5");
    FL_EQ(&f, P "let m = {a: 1}\nreturn map.remove(m, \"a\")\n", "1");
    FL_EQ(&f, P "let m = {a: 1}\nreturn map.remove(m, \"z\")\n",
          "!key: map.remove: no such key");
    /*
     * §4 closes the key set to string, int and bool.  Floats are out
     * because 0.1 + 0.2 would not find 0.3, and containers because a
     * key that moves after insertion is a lookup that silently misses.
     */
    FL_EQ(&f, P "return map.set({}, 1.5, 1)\n",
          "!key: map key must be string, int, or bool, found float");
    FL_EQ(&f, P "return map.get({}, [1])\n",
          "!key: map key must be string, int, or bool, found list");
    FL_EQ(&f, P "let m = {}\nmap.set(m, 7, \"i\")\nmap.set(m, true, \"b\")\n"
                "return map.get(m, 7) + map.get(m, true)\n", "ib");
    FL_EQ(&f, P "import math\nreturn map.set(math, \"pi\", 3)\n",
          "!type: map.set: the map is frozen");
    flfix_close(&f);
}

void test_fl_map_iterates_in_insertion_order(void)
{
    FlFix f;

    flfix_open(&f);
    /*
     * Insertion order, and deterministically so.  Invariant 5 forces
     * it: an order that depended on hashing would make a saved
     * workspace differ byte-for-byte between machines.
     */
    FL_EQ(&f, P "return fmt.repr(map.keys({b: 1, a: 2, c: 3}))\n",
          "[\"b\", \"a\", \"c\"]");
    FL_EQ(&f, P "return fmt.repr(map.values({b: 1, a: 2, c: 3}))\n",
          "[1, 2, 3]");
    FL_EQ(&f, P "return fmt.repr(map.entries({b: 1, a: 2}))\n",
          "[[\"b\", 1], [\"a\", 2]]");
    /*
     * A re-inserted deleted key APPENDS rather than returning to where
     * it was.  Pinned because the alternative -- reusing the tombstone
     * -- would make the order depend on deletion history.
     */
    FL_EQ(&f, P "let m = {a: 1, b: 2, c: 3}\nmap.remove(m, \"a\")\n"
                "map.set(m, \"a\", 9)\nreturn fmt.repr(map.keys(m))\n",
          "[\"b\", \"c\", \"a\"]");
    /* Overwriting an existing key keeps its position. */
    FL_EQ(&f, P "let m = {a: 1, b: 2}\nmap.set(m, \"a\", 9)\n"
                "return fmt.repr(map.entries(m))\n",
          "[[\"a\", 9], [\"b\", 2]]");
    FL_EQ(&f, P "return fmt.repr(map.keys({}))\n", "[]");
    flfix_close(&f);
}

void test_fl_map_merges_copies_and_freezes(void)
{
    FlFix f;

    flfix_open(&f);
    /*
     * a's order first, then b's NEW keys: a key in both keeps a's
     * position and takes b's value, which is what makes merge usable
     * for layering defaults under overrides without reshuffling.
     */
    FL_EQ(&f, P "return fmt.repr(map.merge({a: 1, b: 2}, {b: 9, c: 3}))\n",
          "{a: 1, b: 9, c: 3}");
    FL_EQ(&f, P "let a = {x: 1}\nlet m = map.merge(a, {})\n"
                "map.set(m, \"x\", 2)\nreturn map.get(a, \"x\")\n", "1");
    FL_EQ(&f, P "let a = {x: 1}\nlet c = map.copy(a)\n"
                "map.set(c, \"x\", 2)\nreturn map.get(a, \"x\")\n", "1");
    /* A copy exists to be modified: inheriting the frozen flag would
     * make copying a module produce another unwritable map. */
    FL_EQ(&f, P "import math\nlet c = map.copy(math)\n"
                "map.set(c, \"pi\", 3)\nreturn map.get(c, \"pi\")\n", "3");
    /* freeze is IN PLACE and returns the same map: freezing a value
     * someone else already holds is the point, and a defensive copy
     * would leave the original writable and the caller none the wiser. */
    FL_EQ(&f, P "let m = {a: 1}\nreturn map.freeze(m) == m\n", "true");
    FL_EQ(&f, P "let m = {a: 1}\nmap.freeze(m)\nreturn map.set(m, \"a\", 2)\n",
          "!type: map.set: the map is frozen");
    flfix_close(&f);
}
