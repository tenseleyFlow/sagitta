/*
 * Sprint 20 DoD 5 + 12: the engines must agree.
 *
 * The DFA exists to be fast, and a fast engine that quietly disagrees
 * with the slow one is worse than no fast engine at all: a search would
 * return different answers depending on which path the dispatcher took,
 * and the difference would show up as "sometimes it doesn't find it".
 * So disagreement here is a hard failure, never a tolerance.
 */
#include "harness.h"

#include <stdio.h>
#include <string.h>

#include "search/regex_internal.h"
#include "text/piece.h"
#include "util/arena.h"

/* Patterns spanning every construct the DFA has to reason about:
 * anchors and boundaries (which live in the state key), classes,
 * alternation, both quantifier flavours, and shapes with no match. */
static const char *const patterns[] = {
    "abc", "a.c", "a*", "a+", "a?", "a|b", "ab|a", "a|ab",
    "[a-z]+", "[^a-z]+", "\\d+", "\\w+", "\\s+", "\\W",
    "^abc", "abc$", "^$", "^", "$", "\\Aabc", "abc\\z",
    "\\bfoo\\b", "\\Bfoo", "\\bfoo", "foo\\b", "\\b", "\\B",
    "(a)(b)", "(?:ab)+", "(a|b)*c", "a{2,3}", "a{3}", "a{0,2}",
    "a*?", "a+?", "<.+?>", "(a*)*", "(|a)*", "()", "",
    "\\d{4}-\\d{2}", "[A-Z][a-z]+", "x+y+z+", "zzz", "q",
    "a.*c", "a.*?c", ".*", ".", "[[:alpha:]]+", "[[:digit:]]+"
};

static const char *const inputs[] = {
    "", "a", "b", "abc", "aaa", "aab", "abab", "xxabcxx",
    "foo", "a foo b", "afoob", " foo ", "foo\n", "\nfoo",
    "a\nb", "a\r\nb", "\n", "\r\n", "  ", "\t x",
    "hello World", "2026-08-04", "A1b2C3", "...", "!!!",
    "aaaaaaaaaaaaaaaaaaaa", "abcabcabcabc", "zzz", "q",
    "\xE6\xBC\xA2\xE5\xAD\x97", "caf\xC3\xA9", "e\xCC\x81",
    "\xF0\x9F\x98\x80", "x\xF0\x9F\x98\x80y", "a\xFF" "b"
};

/* Builds the same content as a piece tree so both backings are covered. */
static TextBuf *tb_of(const char *text)
{
    TextBuf *tb = sag_textbuf_new();
    size_t i;
    size_t len = strlen(text);

    /* Interleaved inserts fragment the tree, which is the shape that
     * catches chunk-boundary bugs in either engine. */
    for (i = 0U; i < len; i += 2U)
        sag_textbuf_insert(tb, BYTEOFF((u64)(i / 2U)),
                           (const u8 *)text + i, 1U);
    for (i = 1U; i < len; i += 2U)
        sag_textbuf_insert(tb, BYTEOFF((u64)i), (const u8 *)text + i, 1U);
    return tb;
}

void test_re_engines_agree_on_match_or_not(void)
{
    size_t p;
    size_t i;
    u32 checked = 0U;
    u32 gave_up = 0U;

    for (p = 0U; p < SAG_ARRAY_LEN(patterns); p++) {
        Arena arena;
        SagRe *re;

        arena_init(&arena);
        re = sag_re_compile(&arena, patterns[p], strlen(patterns[p]), 0U,
                            NULL);
        SAG_ASSERT_NOT_NULL(re);
        if (re == NULL) {
            arena_free_all(&arena);
            continue;
        }
        for (i = 0U; i < SAG_ARRAY_LEN(inputs); i++) {
            SagReInput in = sag_re_input_bytes((const u8 *)inputs[i],
                                               (u64)strlen(inputs[i]));
            bool vm = sag_re_search(re, &in, BYTEOFF(0U), NULL);
            int dfa = sag_re_dfa_test(re, &in, 0U);

            if (dfa == SAG_DFA_GIVE_UP) {
                /* A give-up is legal — it means "use the VM" — but it
                 * must be rare on ordinary patterns, so it is counted. */
                gave_up++;
                continue;
            }
            if ((dfa == SAG_DFA_YES) != vm)
                (void)fprintf(stderr,
                              "/%s/ on \"%s\": VM says %s, DFA says %s\n",
                              patterns[p], inputs[i],
                              vm ? "match" : "no match",
                              dfa == SAG_DFA_YES ? "match" : "no match");
            SAG_ASSERT((dfa == SAG_DFA_YES) == vm);
            checked++;
        }
        arena_free_all(&arena);
    }
    /* The cross product is the point: a handful of cases would not
     * exercise the state cache at all. */
    SAG_ASSERT(checked > 1000U);
    SAG_ASSERT_EQ_U64(gave_up, 0U);
}

void test_re_engines_agree_over_piece_trees(void)
{
    size_t p;
    size_t i;

    /* Same agreement, but reading through TextIter, where chunk
     * boundaries fall in the middle of matches. */
    for (p = 0U; p < SAG_ARRAY_LEN(patterns); p++) {
        Arena arena;
        SagRe *re;

        arena_init(&arena);
        re = sag_re_compile(&arena, patterns[p], strlen(patterns[p]), 0U,
                            NULL);
        if (re == NULL) {
            arena_free_all(&arena);
            continue;
        }
        for (i = 0U; i < SAG_ARRAY_LEN(inputs); i++) {
            TextBuf *tb = tb_of(inputs[i]);
            SagReInput in = sag_re_input_textbuf(tb);
            bool vm = sag_re_search(re, &in, BYTEOFF(0U), NULL);
            int dfa = sag_re_dfa_test(re, &in, 0U);

            if (dfa != SAG_DFA_GIVE_UP)
                SAG_ASSERT((dfa == SAG_DFA_YES) == vm);
            sag_textbuf_free(tb);
        }
        arena_free_all(&arena);
    }
}

void test_re_engines_agree_from_offsets(void)
{
    static const char text[] = "abc abc abc";
    Arena arena;
    SagRe *re;
    u64 from;

    /* Agreement must hold from every start offset, not just zero — the
     * DFA rebuilds its context flags at each entry point. */
    arena_init(&arena);
    re = sag_re_compile(&arena, "\\babc\\b", 7U, 0U, NULL);
    SAG_ASSERT_NOT_NULL(re);
    for (from = 0U; from <= strlen(text); from++) {
        SagReInput in = sag_re_input_bytes((const u8 *)text,
                                           (u64)strlen(text));
        bool vm = sag_re_search(re, &in, BYTEOFF(from), NULL);
        int dfa = sag_re_dfa_test(re, &in, from);

        if (dfa != SAG_DFA_GIVE_UP)
            SAG_ASSERT((dfa == SAG_DFA_YES) == vm);
    }
    arena_free_all(&arena);
}

void test_re_dfa_survives_cache_pressure(void)
{
    Arena arena;
    SagRe *re;
    char pat[1024];
    size_t at = 0U;
    int i;
    u8 hay[4096];
    SagReInput in;
    bool vm;
    int dfa;

    /*
     * DoD 12.  A pattern with many distinct states over varied input
     * pushes the state cache; whatever it does — cache, flush, or give
     * up — it must never disagree with the VM about the answer.  That is
     * the property that makes flushing safe: the cache is an
     * accelerator, never a source of truth.
     */
    arena_init(&arena);
    for (i = 0; i < 60; i++)
        at += (size_t)snprintf(pat + at, sizeof(pat) - at, "[%c-%c]?",
                               'a' + (i % 20), 'a' + (i % 20) + 3);
    re = sag_re_compile(&arena, pat, at, 0U, NULL);
    SAG_ASSERT_NOT_NULL(re);
    for (i = 0; i < (int)sizeof(hay); i++)
        hay[i] = (u8)('a' + (i % 26));
    in = sag_re_input_bytes(hay, (u64)sizeof(hay));
    vm = sag_re_search(re, &in, BYTEOFF(0U), NULL);
    dfa = sag_re_dfa_test(re, &in, 0U);
    /* Either it kept up, or it said "use the VM" — never a wrong answer. */
    SAG_ASSERT(dfa == SAG_DFA_GIVE_UP || (dfa == SAG_DFA_YES) == vm);
    arena_free_all(&arena);
}

void test_re_dfa_handles_pathological_patterns(void)
{
    static const char *const nasty[] = {
        "a(a*)*b", "(a|a)*b", "(a+)+b", "(a*)*b"
    };
    size_t p;
    u8 hay[2048];

    /* The DFA must be linear too — it is the engine `sag_re_test` runs,
     * so a blow-up here would reach the user through Sprint 21's
     * incremental search just as surely as one in the VM. */
    (void)memset(hay, 'a', sizeof(hay));
    for (p = 0U; p < SAG_ARRAY_LEN(nasty); p++) {
        Arena arena;
        SagRe *re;
        SagReInput in = sag_re_input_bytes(hay, (u64)sizeof(hay));
        int dfa;

        arena_init(&arena);
        re = sag_re_compile(&arena, nasty[p], strlen(nasty[p]), 0U, NULL);
        SAG_ASSERT_NOT_NULL(re);
        dfa = sag_re_dfa_test(re, &in, 0U);
        /* No 'b' anywhere, so the answer is no — and it must arrive. */
        SAG_ASSERT(dfa == SAG_DFA_NO || dfa == SAG_DFA_GIVE_UP);
        arena_free_all(&arena);
    }
}

/*
 * DoD 5's span half.  The boolean tests above prove the DFA agrees
 * about WHETHER there is a match; this proves the dispatcher's
 * DFA-assisted skip-ahead (§6c) does not move WHERE the match is.
 *
 * The reference is the Pike VM run straight from the window start with
 * no skip, no prefilter and no DFA — the slow path that defines the
 * answer.  Every span the fast path produces has to be that span,
 * byte for byte, including the group captures.
 */
void test_re_engines_agree_on_spans(void)
{
    size_t p;
    size_t i;
    u32 checked = 0U;
    u32 skipped_ahead = 0U;

    for (p = 0U; p < SAG_ARRAY_LEN(patterns); p++) {
        Arena arena;
        SagRe *re;

        arena_init(&arena);
        re = sag_re_compile(&arena, patterns[p], strlen(patterns[p]), 0U,
                            NULL);
        SAG_ASSERT_NOT_NULL(re);
        if (re == NULL) {
            arena_free_all(&arena);
            continue;
        }
        if (re->max_len != UINT32_MAX)
            skipped_ahead++;
        for (i = 0U; i < SAG_ARRAY_LEN(inputs); i++) {
            u64 len = (u64)strlen(inputs[i]);
            u64 from;

            /* Every start offset, not just 0: the skip-ahead computes
             * its window relative to where the search began, and an
             * off-by-one there would only show up mid-buffer. */
            for (from = 0U; from <= len; from++) {
                SagReInput in = sag_re_input_bytes((const u8 *)inputs[i],
                                                   len);
                SagReMatch fast;
                SagReMatch slow;
                bool fast_hit;
                bool slow_hit;
                u32 g;

                (void)memset(&fast, 0, sizeof(fast));
                (void)memset(&slow, 0, sizeof(slow));
                fast_hit = sag_re_search(re, &in, BYTEOFF(from), &fast);
                slow_hit = sag_re_pike_run_ex(re, &in, from, false, &slow);
                if (fast_hit != slow_hit)
                    (void)fprintf(stderr,
                                  "/%s/ on \"%s\" from %llu: search says "
                                  "%s, VM says %s\n",
                                  patterns[p], inputs[i],
                                  (unsigned long long)from,
                                  fast_hit ? "match" : "no match",
                                  slow_hit ? "match" : "no match");
                SAG_ASSERT_EQ_U64((u64)fast_hit, (u64)slow_hit);
                if (!slow_hit) {
                    checked++;
                    continue;
                }
                if (fast.g[0].lo != slow.g[0].lo ||
                    fast.g[0].hi != slow.g[0].hi)
                    (void)fprintf(stderr,
                                  "/%s/ on \"%s\" from %llu: search span "
                                  "%llu..%llu, VM span %llu..%llu\n",
                                  patterns[p], inputs[i],
                                  (unsigned long long)from,
                                  (unsigned long long)fast.g[0].lo,
                                  (unsigned long long)fast.g[0].hi,
                                  (unsigned long long)slow.g[0].lo,
                                  (unsigned long long)slow.g[0].hi);
                SAG_ASSERT_EQ_U64(fast.g[0].lo, slow.g[0].lo);
                SAG_ASSERT_EQ_U64(fast.g[0].hi, slow.g[0].hi);
                SAG_ASSERT_EQ_U64(fast.ngroups, slow.ngroups);
                for (g = 0U; g < slow.ngroups; g++) {
                    SAG_ASSERT_EQ_U64(fast.g[g].lo, slow.g[g].lo);
                    SAG_ASSERT_EQ_U64(fast.g[g].hi, slow.g[g].hi);
                }
                checked++;
            }
        }
        arena_free_all(&arena);
    }
    SAG_ASSERT(checked > 5000U);
    /* If nothing in the corpus were length-bounded the skip-ahead
     * would never run and this test would prove nothing about it. */
    SAG_ASSERT(skipped_ahead > 5U);
}
