/*
 * Sprint 32 §7: the "did you mean" metric and its ordering.
 *
 * ORDERING IS ASSERTED EXACTLY, never as a set.  The determinism lane
 * byte-compares two runs of the same script, and suggestion ordering is
 * the part that rots first: a comparator that loses its last tie-break
 * still produces the right SET every time and a different LINE half the
 * time.
 */
#include "harness.h"

#include <stdio.h>
#include <string.h>

#include "fl/suggest.h"

/* A candidate list as "name" or "name@band", band 0..3 per FlScopeBand.
 * Unqualified means a global, which is the common case. */
static void add_all(FlSuggest *s, const char *const *names, size_t n)
{
    size_t i;

    for (i = 0U; i < n; i++) {
        const char *at = strchr(names[i], '@');
        u32 len = (u32)(at == NULL ? strlen(names[i])
                                   : (size_t)(at - names[i]));
        FlScopeBand band = at == NULL
                               ? FL_SCOPE_GLOBAL
                               : (FlScopeBand)(at[1] - '0');

        fl_suggest_add(s, names[i], len, band);
    }
}

static void check(const char *typo, const char *const *names, size_t n,
                  const char *want)
{
    FlSuggest s;
    Bytebuf out;
    char got[512];
    size_t k;

    fl_suggest_reset(&s);
    add_all(&s, names, n);
    bytebuf_init(&out);
    (void)fl_suggest_render(&s, typo, (u32)strlen(typo), &out);
    k = out.len < sizeof(got) - 1U ? out.len : sizeof(got) - 1U;
    if (k != 0U)
        (void)memcpy(got, out.data, k);
    got[k] = '\0';
    if (strcmp(got, want) != 0)
        (void)fprintf(stderr, "typo '%s':\n  want |%s|\n  got  |%s|\n",
                      typo, want, got);
    SAG_ASSERT_EQ_STR(got, want);
    bytebuf_free(&out);
}

#define CHECK(typo, want, ...)                                                \
    do {                                                                      \
        static const char *const names_[] = {__VA_ARGS__};                    \
                                                                              \
        check((typo), names_, SAG_ARRAY_LEN(names_), (want));                 \
    } while (0)

void test_fl_didyoumean_metric_costs(void)
{
    /* Scaled by two, so the case-only case can be a half in integers. */
    SAG_ASSERT_EQ_U64(fl_suggest_distance("abc", 3U, "abc", 3U), 0U);
    SAG_ASSERT_EQ_U64(fl_suggest_distance("abc", 3U, "abd", 3U), 2U);
    SAG_ASSERT_EQ_U64(fl_suggest_distance("abc", 3U, "ab", 2U), 2U);
    SAG_ASSERT_EQ_U64(fl_suggest_distance("ab", 2U, "abc", 3U), 2U);
    /* A transposition is ONE edit, not two substitutions -- the whole
     * reason this is Damerau and not Levenshtein. */
    SAG_ASSERT_EQ_U64(fl_suggest_distance("lenght", 6U, "length", 6U), 2U);
    SAG_ASSERT_EQ_U64(fl_suggest_distance("ab", 2U, "ba", 2U), 2U);
    /* Case-only substitution is half. */
    SAG_ASSERT_EQ_U64(fl_suggest_distance("Abc", 3U, "abc", 3U), 1U);
    SAG_ASSERT_EQ_U64(fl_suggest_distance("ABC", 3U, "abc", 3U), 3U);
    /* Longer than the DP bound is not scored at all. */
    {
        char big[FL_SUGGEST_MAX_LEN + 2U];

        (void)memset(big, 'a', sizeof(big));
        SAG_ASSERT_EQ_U64(fl_suggest_distance(big, (u32)sizeof(big), "a", 1U),
                          (u64)(u32)-1);
    }
}

void test_fl_didyoumean_length_floor(void)
{
    /*
     * The floor is on the CANDIDATE.  At length two everything is
     * within one edit of everything, and "did you mean 'if'?" for `in`
     * is noise that teaches users to ignore the feature.
     */
    CHECK("in", "", "if");
    CHECK("in", "", "is");
    CHECK("xy", "", "ab");
    /* At three, one edit is allowed and two are not. */
    CHECK("abd", "did you mean 'abc'?", "abc");
    CHECK("axd", "", "abc");
    /* At four and up, two edits are allowed. */
    CHECK("abxy", "did you mean 'abcd'?", "abcd");
    CHECK("axxy", "", "abcd");
}

void test_fl_didyoumean_real_typos(void)
{
    CHECK("lenght", "did you mean 'length'?", "length");
    CHECK("recieve", "did you mean 'receive'?", "receive");
    CHECK("tabwith", "did you mean 'tabwidth'?", "tabwidth");
    CHECK("Tabwidth", "did you mean 'tabwidth'?", "tabwidth");
    CHECK("slcie", "did you mean 'slice'?", "slice");
    CHECK("uper", "did you mean 'upper'?", "upper");
    CHECK("splt", "did you mean 'split'?", "split");
    /* An exact match is not a typo: the caller has a different problem
     * and a suggestion would be nonsense. */
    CHECK("length", "", "length");
    /* Nothing close enough says nothing at all. */
    CHECK("qqqqqq", "", "length", "upper", "slice");
}

void test_fl_didyoumean_orders_by_distance_then_scope(void)
{
    /*
     * Distance first.  `total` is one edit from `totl`, `totals` two.
     */
    CHECK("totl", "did you mean 'total'? (or 'totals')", "totals", "total");
    /*
     * Then SCOPE PROXIMITY, at equal distance: a shadowed local is what
     * the user most likely meant.  Both are one edit from `xat`.
     */
    CHECK("xat", "did you mean 'cat'? (or 'bat')", "bat@2", "cat@0");
    CHECK("xat", "did you mean 'cat'? (or 'bat', 'hat')",
          "bat@2", "cat@0", "hat@3");
    /* Then byte-lexicographic, so two globals at one edit are ordered
     * by their bytes and not by the order they were added. */
    CHECK("xat", "did you mean 'bat'? (or 'cat', 'hat')",
          "hat", "cat", "bat");
    /* At most three are printed however many clear the bar. */
    CHECK("xat", "did you mean 'bat'? (or 'cat', 'eat')",
          "bat", "cat", "eat", "fat", "hat", "mat", "oat");
}

void test_fl_didyoumean_ignores_out_of_scope_names(void)
{
    FlSuggest s;
    Bytebuf out;

    /*
     * THE NEGATIVE CASE, and the reason the candidate set is per-site.
     *
     * `_helper` is one edit from `_helpr` and is NOT offered, because
     * the caller never added it.  Scoring against every interned string
     * in the process -- the easy implementation, since the Interner is
     * right there -- suggests private helpers, module-internal names
     * and string literals.
     */
    fl_suggest_reset(&s);
    fl_suggest_add(&s, "total", 5U, FL_SCOPE_GLOBAL);
    bytebuf_init(&out);
    SAG_ASSERT_EQ_U64((u64)fl_suggest_render(&s, "_helpr", 6U, &out), 0U);
    SAG_ASSERT_EQ_U64((u64)out.len, 0U);
    bytebuf_free(&out);
}

void test_fl_didyoumean_bounds_the_candidate_set(void)
{
    FlSuggest s;
    Bytebuf out;
    char names[600][8];
    u32 i;

    /*
     * Past the cap the set is prefix-filtered on the first byte, and if
     * that does not bring it under, nothing is suggested.  A suggestion
     * is a courtesy, not a reason to spend milliseconds.
     */
    fl_suggest_reset(&s);
    for (i = 0U; i < 600U; i++) {
        (void)snprintf(names[i], sizeof(names[i]), "n%04u", (unsigned)i);
        fl_suggest_add(&s, names[i], (u32)strlen(names[i]), FL_SCOPE_GLOBAL);
    }
    SAG_ASSERT(s.full);
    SAG_ASSERT_EQ_U64((u64)s.n, (u64)FL_SUGGEST_MAX_CANDIDATES);
    bytebuf_init(&out);
    /* Every name starts with 'n', so the prefix filter removes nothing
     * and the whole set is refused rather than scored. */
    SAG_ASSERT_EQ_U64((u64)fl_suggest_render(&s, "n0001x", 6U, &out), 0U);
    bytebuf_free(&out);

    /* With a typo whose first byte matches almost nothing, the filter
     * brings it under the cap and a suggestion appears. */
    fl_suggest_reset(&s);
    for (i = 0U; i < 600U; i++) {
        (void)snprintf(names[i], sizeof(names[i]), "%c%03u",
                       i == 0U ? 'z' : 'n', (unsigned)i);
        fl_suggest_add(&s, names[i], (u32)strlen(names[i]), FL_SCOPE_GLOBAL);
    }
    bytebuf_init(&out);
    SAG_ASSERT_EQ_U64((u64)fl_suggest_render(&s, "z00x", 4U, &out), 1U);
    bytebuf_free(&out);
}
