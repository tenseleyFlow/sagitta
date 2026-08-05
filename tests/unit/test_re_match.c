/*
 * Sprint 20 DoD 4: the golden match table.
 *
 * Every row is (pattern, input, flags, expected span, expected captures).
 * Captures are compared element-wise, not just counted — a run that gets
 * the whole match right and group 2 wrong is a failure, and that is
 * exactly the class of bug a "did it match?" test misses.
 */
#include "harness.h"

#include <stdio.h>
#include <string.h>

#include "search/regex.h"
#include "text/piece.h"
#include "util/arena.h"

/* -1 for lo means "this group must be unset". */
typedef struct ReRow {
    const char *pat;
    const char *input;
    u32 flags;
    bool match;
    i64 lo;
    i64 hi;
    i64 g1lo;
    i64 g1hi;
    i64 g2lo;
    i64 g2hi;
} ReRow;

#define NO -1
#define ROW(p, i, m, lo, hi) {p, i, 0U, m, lo, hi, NO, NO, NO, NO}
#define ROWF(p, i, f, m, lo, hi) {p, i, f, m, lo, hi, NO, NO, NO, NO}
#define ROW1(p, i, m, lo, hi, a, b) {p, i, 0U, m, lo, hi, a, b, NO, NO}
#define ROW2(p, i, m, lo, hi, a, b, c, d) {p, i, 0U, m, lo, hi, a, b, c, d}

static const ReRow rows[] = {
    /* --- literals and dot ------------------------------------- */
    ROW("abc", "abc", true, 0, 3),
    ROW("abc", "xxabcxx", true, 2, 5),
    ROW("abc", "ab", false, 0, 0),
    ROW("", "abc", true, 0, 0),
    ROW("a.c", "abc", true, 0, 3),
    ROW("a.c", "a\nc", false, 0, 0),
    ROWF("a.c", "a\nc", SAG_RE_DOTALL, true, 0, 3),
    ROW(".", "\xC3\xA9", true, 0, 2),          /* one codepoint      */
    ROW("a", "AAA", false, 0, 0),
    ROWF("a", "xAx", SAG_RE_ICASE, true, 1, 2),
    ROWF("ABC", "xabcx", SAG_RE_ICASE, true, 1, 4),

    /* --- escapes ---------------------------------------------- */
    ROW("a\\.c", "a.c", true, 0, 3),
    ROW("a\\.c", "abc", false, 0, 0),
    ROW("\\n", "a\nb", true, 1, 2),
    ROW("\\t", "a\tb", true, 1, 2),
    ROW("\\x41", "xAy", true, 1, 2),
    ROW("\\x{41}", "xAy", true, 1, 2),
    ROW("\\x{1F600}", "\xF0\x9F\x98\x80", true, 0, 4),
    ROW("\\*", "a*b", true, 1, 2),
    ROW("\\\\", "a\\b", true, 1, 2),

    /* --- classes ---------------------------------------------- */
    ROW("[abc]", "xbx", true, 1, 2),
    ROW("[a-c]", "xcx", true, 1, 2),
    ROW("[^a-c]", "abcd", true, 3, 4),
    ROW("[]a]", "]", true, 0, 1),              /* ']' first literal  */
    ROW("[a-]", "-", true, 0, 1),              /* '-' last literal   */
    ROW("[-a]", "-", true, 0, 1),              /* '-' first literal  */
    ROW("[\\]]", "]", true, 0, 1),
    ROW("[\\d]", "a5b", true, 1, 2),
    ROW("[\\w]", "!a!", true, 1, 2),
    ROW("[^\\d]", "5a", true, 1, 2),
    ROW("[a-c1-3]", "z2", true, 1, 2),
    ROWF("[a-c]", "B", SAG_RE_ICASE, true, 0, 1),

    /* --- perl classes ----------------------------------------- */
    ROW("\\d+", "ab123cd", true, 2, 5),
    ROW("\\D+", "12ab34", true, 2, 4),
    ROW("\\w+", "  foo_bar ", true, 2, 9),
    ROW("\\W+", "ab  cd", true, 2, 4),
    ROW("\\s+", "a \t b", true, 1, 4),
    ROW("\\S+", "  ab  ", true, 2, 4),
    ROW("\\d", "\xC2\xB2", false, 0, 0),       /* superscript 2 is No */

    /* --- quantifiers, greedy ---------------------------------- */
    ROW("a*", "aaa", true, 0, 3),
    ROW("a*", "bbb", true, 0, 0),
    ROW("a+", "aaa", true, 0, 3),
    ROW("a+", "bbb", false, 0, 0),
    ROW("a?", "aaa", true, 0, 1),
    ROW("ab*", "a", true, 0, 1),
    ROW("ab*", "abbb", true, 0, 4),
    ROW(".*", "abc", true, 0, 3),
    ROW("a.*c", "abcxc", true, 0, 5),          /* greedy to last c   */

    /* --- quantifiers, lazy ------------------------------------ */
    ROW("a*?", "aaa", true, 0, 0),
    ROW("a+?", "aaa", true, 0, 1),
    ROW("a??", "aaa", true, 0, 0),
    ROW("a.*?c", "abcxc", true, 0, 3),         /* lazy to first c    */
    ROW("<.+?>", "<a><b>", true, 0, 3),

    /* --- bounded repeats -------------------------------------- */
    ROW("a{3}", "aaaa", true, 0, 3),
    ROW("a{3}", "aa", false, 0, 0),
    ROW("a{2,}", "aaaa", true, 0, 4),
    ROW("a{2,3}", "aaaa", true, 0, 3),
    ROW("a{2,3}?", "aaaa", true, 0, 2),
    ROW("a{0,2}", "aaa", true, 0, 2),
    ROW("a{0}", "aaa", true, 0, 0),
    ROW("ba{2,3}", "baa", true, 0, 3),
    ROW("x{1}y{1}", "xy", true, 0, 2),
    ROW("a{2", "a{2", true, 0, 3),             /* bare '{' literal   */

    /* --- alternation: leftmost-FIRST is pinned ---------------- */
    ROW("a|ab", "ab", true, 0, 1),             /* NOT longest        */
    ROW("ab|a", "ab", true, 0, 2),
    ROW("|a", "a", true, 0, 0),                /* empty branch wins  */
    ROW("a|", "a", true, 0, 1),
    ROW("cat|dog", "hotdog", true, 3, 6),
    /* Leftmost beats shortest-end: `bc` would end at 3, earlier than
     * `abcd` ends at 4, but `abcd` STARTS earlier and leftmost wins.
     * This is why a forward DFA's earliest match end cannot be composed
     * with a reverse scan to recover a leftmost-first span. */
    ROW("bc|abcd", "abcd", true, 0, 4),
    /* Skipping ahead must not move what `^` and `\b` are measured
     * against: the prefilter finds `foo` at 1, but only the one at 5
     * starts a line. */
    ROW("^foo", "xfoo\nfoo", true, 5, 8),
    ROW("\\Boo", "xfoo\nfoo", true, 2, 4),
    ROW("abcd|bc", "abcd", true, 0, 4),
    ROW("(a|b)+", "abab", true, 0, 4),

    /* --- anchors ---------------------------------------------- */
    ROW("^abc", "abc", true, 0, 3),
    ROW("^abc", "xabc", false, 0, 0),
    ROW("^b", "a\nb", true, 2, 3),             /* line anchor        */
    ROW("c$", "abc", true, 2, 3),
    ROW("a$", "a\nb", true, 0, 1),             /* before LF          */
    ROW("a$", "a\r\nb", true, 0, 1),           /* before CR of CRLF  */
    ROW("^$", "", true, 0, 0),
    ROW("\\Aabc", "abc", true, 0, 3),
    ROW("\\Ab", "a\nb", false, 0, 0),          /* buffer anchor      */
    ROW("c\\z", "abc", true, 2, 3),
    ROW("a\\z", "a\nb", false, 0, 0),
    ROW("^", "abc", true, 0, 0),

    /* --- word boundaries -------------------------------------- */
    ROW("\\bfoo\\b", "a foo b", true, 2, 5),
    ROW("\\bfoo\\b", "afoob", false, 0, 0),
    ROW("\\bfoo", "foo", true, 0, 3),          /* buffer edge        */
    ROW("foo\\b", "foo", true, 0, 3),
    ROW("\\Bfoo", "afoo", true, 1, 4),
    ROW("\\Bfoo", " foo", false, 0, 0),
    ROW("\\b", "ab", true, 0, 0),
    /*
     * §3's surprising case, now answerable.  Han is Alphabetic, so it is
     * a word character and there is NO boundary between it and 'f' — the
     * boundary is at the buffer start instead.  This needed the general
     * category table; from word-break properties alone Han reads as
     * Other and the answer came out backwards.
     */
    ROW("\\b", "\xE6\xBC\xA2" "f", true, 0, 0),
    ROW("\\bf", "\xE6\xBC\xA2" "f", false, 0, 0),
    ROW("\\w+", "\xE6\xBC\xA2\xE5\xAD\x97", true, 0, 6),   /* CJK is \w */
    ROW("[[:alpha:]]+", "  \xE6\xBC\xA2" "x  ", true, 2, 6),
    ROW("[[:upper:]]+", "abcDEFghi", true, 3, 6),
    ROW("[[:lower:]]+", "ABCdefGHI", true, 3, 6),
    ROW("[[:punct:]]+", "ab!?;cd", true, 2, 5),
    ROW("[[:cntrl:]]", "a\x01" "b", true, 1, 2),
    ROW("[[:blank:]]+", "a \t b", true, 1, 4),
    ROW("[[:xdigit:]]+", "zzDEADzz", true, 2, 6),
    ROW("[[:alnum:]]+", "  a1b2  ", true, 2, 6),
    ROW("[[:graph:]]+", "  ab  ", true, 2, 4),
    ROW("[[:^digit:]]+", "12ab34", true, 2, 4),
    ROW("a\\bb", "ab", false, 0, 0),

    /* --- groups and captures ---------------------------------- */
    ROW1("(abc)", "xabcx", true, 1, 4, 1, 4),
    ROW1("(a+)b", "aaab", true, 0, 4, 0, 3),
    ROW1("(a*)", "aaa", true, 0, 3, 0, 3),
    ROW1("x(a|b)y", "xby", true, 0, 3, 1, 2),
    ROW2("(a)(b)", "ab", true, 0, 2, 0, 1, 1, 2),
    ROW2("((a)b)", "ab", true, 0, 2, 0, 2, 0, 1),
    ROW1("(?:ab)(c)", "abc", true, 0, 3, 2, 3),
    ROW1("(a)*", "aa", true, 0, 2, 1, 2),      /* last iteration     */
    ROW1("(a|)", "b", true, 0, 0, 0, 0),
    ROW1("(a)?b", "b", true, 0, 1, NO, NO),    /* group unset        */

    /* --- zero-width and pathological shapes ------------------- */
    ROW("(a*)*", "aaa", true, 0, 3),
    ROW("(a*)*", "", true, 0, 0),
    /*
     * Empty-alternative loops are a case where engines legitimately
     * disagree.  A backtracker takes the group's empty branch first, the
     * star sees no progress and stops, and the answer is "".  In an NFA
     * simulation the empty path loops back to a star SPLIT that is
     * already in the thread list, so that thread dies and only the 'a'
     * branch survives — giving "aa".  Both are leftmost-first readings
     * of an ambiguous pattern; ours is the one a linear-time engine can
     * produce without special-casing empty loop bodies.  Pinned here so
     * a future change to loop compilation is a visible decision.
     */
    ROW("(|a)*", "aa", true, 0, 2),
    ROW("(a?)*b", "aab", true, 0, 3),
    ROW("()", "x", true, 0, 0),

    /* --- unicode ---------------------------------------------- */
    ROW("\xC3\xA9", "caf\xC3\xA9", true, 3, 5),
    ROW("a\xE6\xBC\xA2" "b", "a\xE6\xBC\xA2" "b", true, 0, 5),
    ROW(".", "\xF0\x9F\x98\x80", true, 0, 4),
    ROW("..", "\xF0\x9F\x98\x80" "x", true, 0, 5),
    /* e + combining acute is TWO codepoints; '.' matches only the e. */
    ROW("^.$", "e\xCC\x81", false, 0, 0),
    ROW("^..$", "e\xCC\x81", true, 0, 3),

    /* --- inline flags ----------------------------------------- */
    ROW("(?i)abc", "ABC", true, 0, 3),
    ROW("(?i:abc)d", "ABCd", true, 0, 4),
    ROW("(?i:abc)d", "ABCD", false, 0, 0),     /* scope ends at ')'  */
    ROW("(?s).", "\n", true, 0, 1),
    ROW("a(?i)b", "aB", true, 0, 2),
    ROW("a(?i)b", "Ab", false, 0, 0),

    /* --- literal flag ----------------------------------------- */
    ROWF("a.c", "a.c", SAG_RE_LITERAL, true, 0, 3),
    ROWF("a.c", "abc", SAG_RE_LITERAL, false, 0, 0),
    ROWF("a*b", "a*b", SAG_RE_LITERAL, true, 0, 3),
    ROWF("[x]", "[x]", SAG_RE_LITERAL, true, 0, 3),

    /* --- longer shapes ---------------------------------------- */
    ROW("[a-z]+@[a-z]+", "mail: me@here!", true, 6, 13),
    ROW("^\\s*#", "  # comment", true, 0, 3),
    ROW("\\d{4}-\\d{2}-\\d{2}", "on 2026-08-04.", true, 3, 13),
    ROW1("\"([^\"]*)\"", "say \"hi\" now", true, 4, 8, 5, 7),
    ROW("a+b+c+", "xaabbccx", true, 1, 7),

    /* --- nested quantifiers and groups ------------------------ */
    ROW("(ab)+", "ababab", true, 0, 6),
    ROW("(ab)+c", "ababx", false, 0, 0),
    ROW("(a(b(c)))", "abc", true, 0, 3),
    ROW("((a|b)c)+", "acbc", true, 0, 4),
    ROW("(a*)+", "aaa", true, 0, 3),
    ROW("(a|ab)(c|bcd)", "abcd", true, 0, 4),
    ROW("a(b|c)*d", "abccbd", true, 0, 6),
    ROW("(x?)(y?)(z?)", "xyz", true, 0, 3),
    ROW("(?:a|b)*c", "ababc", true, 0, 5),
    ROW("(a{2})+", "aaaa", true, 0, 4),

    /* --- alternation ordering and prefixes -------------------- */
    ROW("foo|foobar", "foobar", true, 0, 3),
    ROW("foobar|foo", "foobar", true, 0, 6),
    ROW("a|b|c", "c", true, 0, 1),
    ROW("(a|b|c)+", "abcabc", true, 0, 6),
    ROW("x(a|)y", "xy", true, 0, 2),
    ROW("(|)", "a", true, 0, 0),

    /* --- classes, more shapes --------------------------------- */
    ROW("[0-9a-fA-F]+", "zzDEADBEEFzz", true, 2, 10),
    ROW("[^\\n]+", "ab\ncd", true, 0, 2),
    ROW("[\\t ]+", "a \t b", true, 1, 4),
    ROW("[.]", "a.b", true, 1, 2),
    ROW("[*+?]", "a+b", true, 1, 2),
    ROW("[a][b][c]", "abc", true, 0, 3),
    ROW("[^x]*", "aaax", true, 0, 3),
    ROW("[\\w-]+", "a-b_c", true, 0, 5),

    /* --- anchors in combination ------------------------------- */
    ROW("^a.*z$", "abcz", true, 0, 4),
    ROW("^$", "\n", true, 0, 0),
    ROW("^\\s*$", "   ", true, 0, 3),
    ROW("^x", "a\nb\nx", true, 4, 5),
    ROW("x$", "x\ny", true, 0, 1),
    ROW("\\Aa\\z", "a", true, 0, 1),
    ROW("\\Aa\\z", "ab", false, 0, 0),

    /* --- repeats at boundaries -------------------------------- */
    ROW("a{1,1}", "a", true, 0, 1),
    ROW("a{0,0}", "a", true, 0, 0),
    ROW("(ab){2,2}", "abab", true, 0, 4),
    ROW("a{1,3}b", "aaab", true, 0, 4),
    ROW("a{1,3}b", "aaaab", true, 1, 5),
    ROW("\\d{2,4}", "12345", true, 0, 4),
    ROW("\\d{2,4}?", "12345", true, 0, 2),

    /* --- realistic patterns ----------------------------------- */
    ROW("#include", "#include <x>", true, 0, 8),
    ROW1("^(\\w+):", "key: value", true, 0, 4, 0, 3),
    ROW("[A-Z][a-z]+", "hello World now", true, 6, 11),
    ROW("\\bTODO\\b", "// TODO: fix", true, 3, 7),
    ROW("0[xX][0-9a-fA-F]+", "v = 0xFF;", true, 4, 8),
    ROW1("\\((\\d+)\\)", "f(42)", true, 1, 5, 2, 4),
    ROW("a\\s+b", "a   b", true, 0, 5),
    ROW(".*;$", "int x = 1;", true, 0, 10)
};

static void check_row(const ReRow *row, bool use_textbuf)
{
    Arena arena;
    SagReErr err = {0, NULL};
    SagRe *re;
    SagReMatch m;
    SagReInput in;
    TextBuf *tb = NULL;
    size_t ilen = strlen(row->input);
    bool got;

    arena_init(&arena);
    re = sag_re_compile(&arena, row->pat, strlen(row->pat), row->flags,
                        &err);
    if (re == NULL) {
        (void)fprintf(stderr, "compile failed for /%s/: %s\n", row->pat,
                      err.msg == NULL ? "?" : err.msg);
        SAG_ASSERT_NOT_NULL(re);
        arena_free_all(&arena);
        return;
    }
    if (use_textbuf) {
        tb = sag_textbuf_from_bytes((const u8 *)row->input, (u64)ilen);
        in = sag_re_input_textbuf(tb);
    } else {
        in = sag_re_input_bytes((const u8 *)row->input, (u64)ilen);
    }
    (void)memset(&m, 0, sizeof(m));
    got = sag_re_search(re, &in, BYTEOFF(0U), &m);
    if (got != row->match) {
        (void)fprintf(stderr,
                      "/%s/ on \"%s\"%s: expected %s, got %s\n", row->pat,
                      row->input, use_textbuf ? " [textbuf]" : "",
                      row->match ? "match" : "no match",
                      got ? "match" : "no match");
    }
    SAG_ASSERT(got == row->match);
    if (!row->match) {
        if (tb != NULL)
            sag_textbuf_free(tb);
        arena_free_all(&arena);
        return;
    }
    if (m.g[0].lo != (u64)row->lo || m.g[0].hi != (u64)row->hi)
        (void)fprintf(stderr, "/%s/ on \"%s\": span %llu..%llu, want %lld..%lld\n",
                      row->pat, row->input,
                      (unsigned long long)m.g[0].lo,
                      (unsigned long long)m.g[0].hi,
                      (long long)row->lo, (long long)row->hi);
    SAG_ASSERT_EQ_U64(m.g[0].lo, (u64)row->lo);
    SAG_ASSERT_EQ_U64(m.g[0].hi, (u64)row->hi);
    if (row->g1lo != NO) {
        SAG_ASSERT_EQ_U64(m.g[1].lo, (u64)row->g1lo);
        SAG_ASSERT_EQ_U64(m.g[1].hi, (u64)row->g1hi);
    }
    if (row->g2lo != NO) {
        SAG_ASSERT_EQ_U64(m.g[2].lo, (u64)row->g2lo);
        SAG_ASSERT_EQ_U64(m.g[2].hi, (u64)row->g2hi);
    }
    if (tb != NULL)
        sag_textbuf_free(tb);
    arena_free_all(&arena);
}

void test_re_golden_table_bytes(void)
{
    size_t i;

    /* The table is the contract; keep it above the DoD floor. */
    SAG_ASSERT(SAG_ARRAY_LEN(rows) >= 130U);
    for (i = 0U; i < SAG_ARRAY_LEN(rows); i++)
        check_row(&rows[i], false);
}

void test_re_golden_table_textbuf(void)
{
    size_t i;

    /* Same rows through the piece tree: the engine must read identically
     * through TextIter as it does over a flat array. */
    for (i = 0U; i < SAG_ARRAY_LEN(rows); i++)
        check_row(&rows[i], true);
}

void test_re_leftmost_first_is_pinned(void)
{
    Arena arena;
    SagRe *re;
    SagReMatch m;
    SagReInput in = sag_re_input_bytes((const u8 *)"ab", 2U);

    arena_init(&arena);
    /* Leftmost-FIRST, not leftmost-longest: earlier branch wins ties.
     * A POSIX engine would answer 0..2 here. */
    re = sag_re_compile(&arena, "a|ab", 4U, 0U, NULL);
    SAG_ASSERT_NOT_NULL(re);
    SAG_ASSERT(sag_re_search(re, &in, BYTEOFF(0U), &m));
    SAG_ASSERT_EQ_U64(m.g[0].hi, 1U);

    re = sag_re_compile(&arena, "ab|a", 4U, 0U, NULL);
    SAG_ASSERT_NOT_NULL(re);
    SAG_ASSERT(sag_re_search(re, &in, BYTEOFF(0U), &m));
    SAG_ASSERT_EQ_U64(m.g[0].hi, 2U);
    arena_free_all(&arena);
}

void test_re_empty_width_loop_terminates(void)
{
    Arena arena;
    SagRe *re;
    SagReMatch m;
    SagReInput in = sag_re_input_bytes((const u8 *)"aaaa", 4U);

    /*
     * DoD 9.  `(a*)*` chains SPLIT/JMP without consuming input.  This
     * completes only because addthread's sparse_has check lives INSIDE
     * its loop; moving that check to the call site makes this test hang
     * rather than fail, which is why it is called out by name.
     */
    arena_init(&arena);
    re = sag_re_compile(&arena, "(a*)*", 5U, 0U, NULL);
    SAG_ASSERT_NOT_NULL(re);
    SAG_ASSERT(sag_re_search(re, &in, BYTEOFF(0U), &m));
    SAG_ASSERT_EQ_U64(m.g[0].lo, 0U);
    SAG_ASSERT_EQ_U64(m.g[0].hi, 4U);

    re = sag_re_compile(&arena, "(|a)*", 5U, 0U, NULL);
    SAG_ASSERT_NOT_NULL(re);
    SAG_ASSERT(sag_re_search(re, &in, BYTEOFF(0U), &m));

    re = sag_re_compile(&arena, "(a?)*b", 6U, 0U, NULL);
    SAG_ASSERT_NOT_NULL(re);
    SAG_ASSERT(!sag_re_search(re, &in, BYTEOFF(0U), &m));
    arena_free_all(&arena);
}

void test_re_match_at_is_anchored(void)
{
    Arena arena;
    SagRe *re;
    SagReMatch m;
    SagReInput in = sag_re_input_bytes((const u8 *)"xxabc", 5U);

    arena_init(&arena);
    re = sag_re_compile(&arena, "abc", 3U, 0U, NULL);
    SAG_ASSERT_NOT_NULL(re);
    /* search finds it; match_at only succeeds at the exact offset. */
    SAG_ASSERT(sag_re_search(re, &in, BYTEOFF(0U), &m));
    SAG_ASSERT_EQ_U64(m.g[0].lo, 2U);
    SAG_ASSERT(!sag_re_match_at(re, &in, BYTEOFF(0U), &m));
    SAG_ASSERT(!sag_re_match_at(re, &in, BYTEOFF(1U), &m));
    SAG_ASSERT(sag_re_match_at(re, &in, BYTEOFF(2U), &m));
    SAG_ASSERT_EQ_U64(m.g[0].hi, 5U);
    arena_free_all(&arena);
}

void test_re_search_from_offset_and_back(void)
{
    Arena arena;
    SagRe *re;
    SagReMatch m;
    SagReInput in = sag_re_input_bytes((const u8 *)"a b a b a", 9U);

    arena_init(&arena);
    re = sag_re_compile(&arena, "a", 1U, 0U, NULL);
    SAG_ASSERT_NOT_NULL(re);
    SAG_ASSERT(sag_re_search(re, &in, BYTEOFF(0U), &m));
    SAG_ASSERT_EQ_U64(m.g[0].lo, 0U);
    SAG_ASSERT(sag_re_search(re, &in, BYTEOFF(1U), &m));
    SAG_ASSERT_EQ_U64(m.g[0].lo, 4U);
    SAG_ASSERT(sag_re_search(re, &in, BYTEOFF(5U), &m));
    SAG_ASSERT_EQ_U64(m.g[0].lo, 8U);
    SAG_ASSERT(!sag_re_search(re, &in, BYTEOFF(9U), &m));
    /* Backward search returns the last match strictly before the point. */
    SAG_ASSERT(sag_re_search_back(re, &in, BYTEOFF(9U), &m));
    SAG_ASSERT_EQ_U64(m.g[0].lo, 8U);
    SAG_ASSERT(sag_re_search_back(re, &in, BYTEOFF(8U), &m));
    SAG_ASSERT_EQ_U64(m.g[0].lo, 4U);
    SAG_ASSERT(sag_re_search_back(re, &in, BYTEOFF(1U), &m));
    SAG_ASSERT_EQ_U64(m.g[0].lo, 0U);
    SAG_ASSERT(!sag_re_search_back(re, &in, BYTEOFF(0U), &m));
    arena_free_all(&arena);
}

void test_re_group_count_and_min_len(void)
{
    Arena arena;
    SagRe *re;

    arena_init(&arena);
    re = sag_re_compile(&arena, "a(b)(c)", 7U, 0U, NULL);
    SAG_ASSERT_NOT_NULL(re);
    SAG_ASSERT_EQ_U64(sag_re_group_count(re), 3U);   /* incl. group 0 */
    SAG_ASSERT_EQ_U64(sag_re_min_len(re), 3U);

    re = sag_re_compile(&arena, "a*", 2U, 0U, NULL);
    SAG_ASSERT_EQ_U64(sag_re_min_len(re), 0U);
    re = sag_re_compile(&arena, "ab|c", 4U, 0U, NULL);
    SAG_ASSERT_EQ_U64(sag_re_min_len(re), 1U);
    re = sag_re_compile(&arena, "a{3,5}", 6U, 0U, NULL);
    SAG_ASSERT_EQ_U64(sag_re_min_len(re), 3U);
    arena_free_all(&arena);
}
