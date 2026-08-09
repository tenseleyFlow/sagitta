/*
 * Sprint 31 deliverable 8: the `re` module.
 *
 * COVERAGE  (function -> test; every row of §8's table, every error kind)
 * --------
 * re.test        flags i s l      re_test_answers_yes_or_no
 * re.test        "type"           re_a_bad_pattern_or_flag_raises_type
 * re.find        map shape, from  re_find_returns_a_match_map
 * re.find        "index"          re_find_returns_a_match_map
 * re.find        groups, nil      re_groups_are_numbered_like_the_template
 * re.find_all    non-overlapping  re_find_all_scans_without_looping
 * re.find_all    empty-match rule re_find_all_scans_without_looping
 * re.find_all    limit argument   re_find_all_scans_without_looping
 * re.replace     $0 $9 ${12} $$   re_replace_expands_the_template
 * re.replace     "type"/"index"   re_replace_refuses_a_bad_template
 * re.replace_fn  callback         re_replace_fn_calls_back_into_fletch
 * re.replace_fn  "type"           re_replace_fn_calls_back_into_fletch
 * re.split       separators drop  re_split_drops_the_separators
 * re.escape      quotes metachars re_escape_round_trips_through_test
 * the LRU past 64 distinct pats   re_the_compile_cache_survives_a_full_cycle
 * s20's golden table, same spans  re_replays_the_s20_golden_table
 * empty match over 1 MiB          re_terminates_on_a_large_subject
 *
 * The `arity` kind listed for replace_fn is the VM's, checked at CALL
 * against the native table before the native runs; test_fl_vm covers it
 * once for all ~200 natives rather than once per function.
 */
#include "flfix.h"
#include "re_golden.h"

#include <stdio.h>
#include <string.h>

#define P "import re\n"
#define PF "import re\nimport fmt\n"

void test_fl_re_test_answers_yes_or_no(void)
{
    FlFix f;

    flfix_open(&f);
    FL_EQ(&f, P "return re.test(\"abc\", \"b\")\n", "true");
    FL_EQ(&f, P "return re.test(\"abc\", \"z\")\n", "false");
    FL_EQ(&f, P "return re.test(\"ABC\", \"b\", \"i\")\n", "true");
    /* `s` lets `.` cross a newline; `l` makes the whole pattern
     * literal text. */
    FL_EQ(&f, P "return re.test(\"a\\nb\", \"a.b\", \"s\")\n", "true");
    FL_EQ(&f, P "return re.test(\"a\\nb\", \"a.b\")\n", "false");
    FL_EQ(&f, P "return re.test(\"a.c\", \"a.c\", \"l\")\n", "true");
    FL_EQ(&f, P "return re.test(\"abc\", \"a.c\", \"l\")\n", "false");
    flfix_close(&f);
}

void test_fl_re_a_bad_pattern_or_flag_raises_type(void)
{
    FlFix f;

    flfix_open(&f);
    /*
     * s20's own message and its pattern offset, so the wording a user
     * sees here matches the search prompt's.  Kind "type" rather than a
     * new kind: a malformed pattern is a bad value.
     */
    FL_EQ(&f, P "return re.test(\"a\", \"(\")\n",
          "!type: regex: unterminated group at pattern offset 0");
    /* A typo'd flag is NAMED, not ignored -- silently dropping it would
     * make a case-insensitive search quietly case-sensitive. */
    FL_EQ(&f, P "return re.test(\"a\", \"a\", \"q\")\n",
          "!type: re: unknown flag 'q'; the flags are i, s and l");
    flfix_close(&f);
}

void test_fl_re_find_returns_a_match_map(void)
{
    FlFix f;

    flfix_open(&f);
    /* Fixed, insertion-ordered keys; lo and hi are BYTE offsets, which
     * is what the engine reports and what str.slice_bytes takes. */
    FL_EQ(&f, PF "return fmt.repr(re.find(\"hello\", \"l+\"))\n",
          "{lo: 2, hi: 4, text: \"ll\", groups: [{lo: 2, hi: 4, "
          "text: \"ll\"}]}");
    FL_EQ(&f, P "return re.find(\"hello\", \"z\")\n", "nil");
    FL_EQ(&f, PF "return fmt.repr(re.find(\"hello\", \"l\", nil, 3))\n",
          "{lo: 3, hi: 4, text: \"l\", groups: [{lo: 3, hi: 4, "
          "text: \"l\"}]}");
    FL_EQ(&f, P "return re.find(\"ab\", \"a\", nil, 9)\n",
          "!index: re.find: byte 9 is outside a subject of 2 bytes");
    flfix_close(&f);
}

void test_fl_re_groups_are_numbered_like_the_template(void)
{
    FlFix f;

    flfix_open(&f);
    /* groups[i] is exactly what $i expands to, group 0 included: one
     * numbering for the map and the template beats two that differ. */
    FL_EQ(&f, PF "return fmt.repr(re.find(\"a1\", "
                 "\"([a-z])([0-9])\").groups)\n",
          "[{lo: 0, hi: 2, text: \"a1\"}, {lo: 0, hi: 1, text: \"a\"}, "
          "{lo: 1, hi: 2, text: \"1\"}]");
    /* A group that did not participate is nil. */
    FL_EQ(&f, PF "return fmt.repr(re.find(\"xb\", \"x(q)?(b)\").groups[1])\n",
          "nil");
    FL_EQ(&f, PF "return fmt.repr(re.find(\"xb\", \"x(q)?(b)\").groups[2])\n",
          "{lo: 1, hi: 2, text: \"b\"}");
    flfix_close(&f);
}

void test_fl_re_find_all_scans_without_looping(void)
{
    FlFix f;

    flfix_open(&f);
    FL_EQ(&f, PF "import list\nreturn fmt.repr(list.map("
                 "re.find_all(\"a1b22c\", \"[0-9]+\"), "
                 "fn(m) { return m.text }))\n", "[\"1\", \"22\"]");
    /*
     * THE EMPTY-MATCH RULE.  After a zero-width match the scan resumes
     * at the next CODEPOINT, and a zero-width match at the very end
     * ends it -- which is what makes this terminate at all.
     */
    FL_EQ(&f, P "import list\nreturn list.len(re.find_all(\"abc\", \"x*\"))\n",
          "4");
    /* The next codepoint, not the next byte: resuming mid-sequence
     * would let a match start inside a character. */
    FL_EQ(&f, P "import list\n"
                "return list.len(re.find_all(\"\\u{6F22}\\u{5B57}\", \"x*\"))\n",
          "3");
    FL_EQ(&f, P "import list\n"
                "return list.len(re.find_all(\"aaaa\", \"a\", nil, 2))\n", "2");
    flfix_close(&f);
}

void test_fl_re_replace_expands_the_template(void)
{
    FlFix f;

    flfix_open(&f);
    FL_EQ(&f, P "return re.replace(\"a1b\", \"[0-9]\", \"#\")\n", "a#b");
    FL_EQ(&f, P "return re.replace(\"a1b\", \"([a-z])([0-9])\", \"$2$1\")\n",
          "1ab");
    /* ${N} for a reference followed by a digit that is not part of it. */
    FL_EQ(&f, P "return re.replace(\"a1b\", \"([a-z])([0-9])\", \"${2}x\")\n",
          "1xb");
    FL_EQ(&f, P "return re.replace(\"ab\", \"b\", \"$$\")\n", "a$");
    /* A group that did not participate expands to nothing. */
    FL_EQ(&f, P "return re.replace(\"ab\", \"a(q)?\", \"[$1]\")\n", "[]b");
    /* A zero-width match copies the codepoint it stepped over, or the
     * subject would lose a character at every empty match. */
    FL_EQ(&f, P "return re.replace(\"abc\", \"x*\", \"-\")\n", "-a-b-c-");
    FL_EQ(&f, P "return re.replace(\"aaa\", \"a\", \"b\", nil, 2)\n", "bba");
    flfix_close(&f);
}

void test_fl_re_replace_refuses_a_bad_template(void)
{
    FlFix f;

    flfix_open(&f);
    /*
     * A `$` followed by anything else RAISES rather than passing
     * through.  Silently emitting a literal dollar is how a `$1` typo
     * becomes invisible data loss in a whole-file replace: the run
     * reports success and the text is wrong.
     */
    FL_EQ(&f, P "return re.replace(\"ab\", \"a\", \"$\")\n",
          "!type: re: the template ends in '$' at byte 0");
    FL_EQ(&f, P "return re.replace(\"ab\", \"a\", \"$z\")\n",
          "!type: re: '$' is followed by 'z' at byte 0; write '$$' for a "
          "literal dollar");
    FL_EQ(&f, P "return re.replace(\"ab\", \"a\", \"${x}\")\n",
          "!type: re: expected ${N} at byte 0");
    FL_EQ(&f, P "return re.replace(\"ab\", \"a\", \"$3\")\n",
          "!index: re: the template names group 3, the pattern has 1");
    flfix_close(&f);
}

void test_fl_re_replace_fn_calls_back_into_fletch(void)
{
    FlFix f;

    flfix_open(&f);
    FL_EQ(&f, P "return re.replace_fn(\"a1b\", \"[0-9]\", "
                "fn(m) { return \"<\" + m.text + \">\" })\n", "a<1>b");
    /* The callback may itself use `re`; the scan's pin on the compile
     * cache is what keeps the outer program alive across it. */
    FL_EQ(&f, P "return re.replace_fn(\"a1b\", \"[0-9]\", "
                "fn(m) { if re.test(m.text, \"[0-9]\") { return \"N\" } "
                "return \"?\" })\n", "aNb");
    FL_EQ(&f, P "return re.replace_fn(\"a\", \"a\", fn(m) { return 1 })\n",
          "!type: re.replace_fn: the function must return str, found int");
    flfix_close(&f);
}

void test_fl_re_split_drops_the_separators(void)
{
    FlFix f;

    flfix_open(&f);
    FL_EQ(&f, PF "return fmt.repr(re.split(\"a,b,,c\", \",\"))\n",
          "[\"a\", \"b\", \"\", \"c\"]");
    FL_EQ(&f, PF "return fmt.repr(re.split(\"a1b22c\", \"[0-9]+\"))\n",
          "[\"a\", \"b\", \"c\"]");
    /* A zero-width separator: the piece runs to where the scan resumes,
     * and the one at the very end is not emitted twice. */
    FL_EQ(&f, PF "return fmt.repr(re.split(\"abc\", \"x*\"))\n",
          "[\"a\", \"b\", \"c\", \"\"]");
    FL_EQ(&f, PF "return fmt.repr(re.split(\"a,b,c\", \",\", nil, 1))\n",
          "[\"a\", \"b,c\"]");
    /* Capture groups are not included in the pieces. */
    FL_EQ(&f, PF "return fmt.repr(re.split(\"a1b\", \"([0-9])\"))\n",
          "[\"a\", \"b\"]");
    flfix_close(&f);
}

void test_fl_re_escape_round_trips_through_test(void)
{
    FlFix f;

    flfix_open(&f);
    FL_EQ(&f, P "return re.escape(\"a.b*c\")\n", "a\\.b\\*c");
    /* s21's own escaper, not a second one: `*`, `#`, `:s//` and this
     * all have to agree about what a metacharacter is. */
    FL_EQ(&f, P "return re.test(\"a.b*c\", re.escape(\"a.b*c\"))\n", "true");
    FL_EQ(&f, P "return re.test(\"axbyc\", re.escape(\"a.b*c\"))\n", "false");
    flfix_close(&f);
}

void test_fl_re_the_compile_cache_survives_a_full_cycle(void)
{
    FlFix f;

    flfix_open(&f);
    /*
     * Past 64 distinct patterns the LRU cycles and the arena is reset.
     * The answers must not change -- a cache that returned a stale or
     * freed program would show up here and nowhere else.
     */
    FL_EQ(&f, P "import str\n"
                "let ok = true\n"
                "let i = 0\n"
                "while i < 200 {\n"
                "  let p = \"x\" + str.repeat(\"y\", i % 40) + \"z\"\n"
                "  if re.test(p, p, \"l\") == false { ok = false }\n"
                "  if re.test(\"no\", p) == true { ok = false }\n"
                "  i = i + 1\n"
                "}\n"
                "return ok\n", "true");
    /* Invalid bytes in the subject survive a replace untouched: a
     * script can regex a binary file without mangling it. */
    FL_EQ(&f, P "return re.replace(\"a\\x80b\", \"a\", \"A\")\n",
          "A\x80" "b");
    flfix_close(&f);
}

/* Every byte as \xNN, so a pattern's own backslashes and quotes reach
 * the engine unmangled by the Fletch lexer on the way. */
static void re_escape(char *out, size_t cap, const char *b)
{
    size_t at = 0U;

    while (*b != '\0' && at + 5U < cap) {
        at += (size_t)snprintf(out + at, cap - at, "\\x%02x",
                               (unsigned)(u8)*b);
        b++;
    }
    out[at] = '\0';
}

/*
 * DoD 9: s20's golden table, replayed through re.find, with IDENTICAL
 * spans.
 *
 * The same rows the engine itself is checked against -- included from
 * one header, because "identical" means against one table and not
 * against a copy of it that drifts.  What this catches is the `re`
 * module getting the binding wrong: an off-by-one on the subject
 * window, a flag dropped, a match map built from the wrong group.
 */
void test_fl_re_replays_the_s20_golden_table(void)
{
    FlFix f;
    size_t i;
    size_t replayed = 0U;

    flfix_open(&f);
    for (i = 0U; i < SAG_ARRAY_LEN(rows); i++) {
        const ReRow *r = &rows[i];
        char pat[2048];
        char inp[2048];
        char src[8192];
        char want[256];
        char flags[8];
        size_t at = 0U;

        /* The rows carry s20's flag bits; `re` spells them as letters. */
        if ((r->flags & (u32)SAG_RE_ICASE) != 0U)
            flags[at++] = 'i';
        if ((r->flags & (u32)SAG_RE_DOTALL) != 0U)
            flags[at++] = 's';
        if ((r->flags & (u32)SAG_RE_LITERAL) != 0U)
            flags[at++] = 'l';
        flags[at] = '\0';
        re_escape(pat, sizeof(pat), r->pat);
        re_escape(inp, sizeof(inp), r->input);
        (void)snprintf(src, sizeof(src),
                       "import re\nimport fmt\n"
                       "let m = re.find(\"%s\", \"%s\", \"%s\")\n"
                       "if m == nil { return \"none\" }\n"
                       "return fmt.f(\"{}..{}\", m.lo, m.hi)\n",
                       inp, pat, flags);
        if (r->match)
            (void)snprintf(want, sizeof(want), "%lld..%lld",
                           (long long)r->lo, (long long)r->hi);
        else
            (void)snprintf(want, sizeof(want), "none");
        FL_EQ(&f, src, want);
        replayed++;
    }
    /* The table is the contract; s20 keeps it above 130 rows. */
    SAG_ASSERT(replayed >= 130U);
    flfix_close(&f);
}

/*
 * DoD 9's other half: an empty-matching pattern over a 1 MiB subject
 * TERMINATES, and loses nothing.
 *
 * The rule is tested above on three characters; this is the rule at
 * size, which is where a scanner that advanced by a byte instead of a
 * codepoint, or not at all, stops being a hang you can reason about and
 * becomes one you wait for.
 */
void test_fl_re_terminates_on_a_large_subject(void)
{
    FlFix f;

    flfix_open(&f);
    FL_EQ(&f, "import re\nimport str\n"
              "let s = str.repeat(\"ab\", 524288)\n"
              "return str.len_bytes(re.replace(s, \"x*\", \"\"))\n",
          "1048576");
    /* And with a pattern that matches, over the same 1 MiB. */
    FL_EQ(&f, "import re\nimport str\n"
              "let s = str.repeat(\"ab\", 524288)\n"
              "return str.len_bytes(re.replace(s, \"b\", \"\"))\n",
          "524288");
    flfix_close(&f);
}
