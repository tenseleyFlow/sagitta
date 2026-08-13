/*
 * Sprint 26 §7: incremental filtering.
 *
 * Named test_narrow rather than test_filter because Sprint 19 already
 * owns that name for the shell region filter — two unrelated things
 * called "filter", and the test binary has one flat namespace.
 *
 * THE TEST THAT MATTERS IS THE EQUIVALENCE ONE (DoD 7).
 *
 * Narrowing on append is only legal because a subsequence match cannot
 * appear by adding a character to the pattern — so rescoring just the
 * current set gives the same answer as rescoring everything.  That is an
 * argument, and arguments are wrong all the time.  The property test
 * runs 10 000 random cases and demands the narrowed result equal a full
 * rescan EXACTLY.
 *
 * A narrowing bug is silent in the worst way: the list shows fewer files
 * than it should, and nobody notices a file that is not there.  There is
 * no error, no warning, and nothing on screen to see.
 */
#define _POSIX_C_SOURCE 200809L

#include "harness.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ui/filter.h"
#include "ui/picker.h"

/* ---------------------------------------------------------------- */
/* Fixtures                                                         */
/* ---------------------------------------------------------------- */

typedef struct NwFix {
    PickItem *items;
    char *text;
    u32 n;
} NwFix;

static void nw_free(NwFix *f)
{
    free(f->items);
    free(f->text);
    (void)memset(f, 0, sizeof(*f));
}

/* A deterministic pseudo-random corpus of path-shaped labels. */
static void nw_make(NwFix *f, u32 n, u64 seed)
{
    static const char *const dirs[] = {"src", "src/ui", "src/ws", "tests",
                                       "tests/unit", "docs", "vendor/lib"};
    static const char *const stems[] = {"tabs", "picker", "filter", "walk",
                                        "state", "undo", "grid", "alpha",
                                        "beta", "gamma"};
    static const char *const exts[] = {"c", "h", "md", "txt"};
    u64 rng = seed;
    u32 i;
    size_t at = 0U;
    size_t cap = (size_t)n * 64U;

    f->n = n;
    f->items = yew_xreallocarray(NULL, n, sizeof(*f->items));
    f->text = yew_xmalloc(cap);
    for (i = 0U; i < n; i++) {
        int wrote;

        rng = rng * 6364136223846793005ULL + 1442695040888963407ULL;
        wrote = snprintf(f->text + at, cap - at, "%s/%s%u.%s",
                         dirs[(rng >> 33) % 7U], stems[(rng >> 21) % 10U],
                         (unsigned)((rng >> 11) % 1000U),
                         exts[(rng >> 7) % 4U]);
        YEW_ASSERT(wrote > 0);
        (void)memset(&f->items[i], 0, sizeof(f->items[i]));
        f->items[i].label = f->text + at;
        f->items[i].payload = (i32)i;
        at += (size_t)wrote + 1U;
    }
}

/* A full rescan from scratch, which is what narrowing must equal. */
static u32 nw_full(NwFix *f, bool path_mode, const char *pat, u32 plen,
                   FzRanked *out, u32 max)
{
    FilterState fresh;
    u32 n;

    yew_filter_init(&fresh);
    yew_filter_reset(&fresh, f->items, f->n, 0U);
    /* Applied in ONE step from the empty pattern, so it cannot narrow
     * and must scan everything. */
    (void)yew_filter_apply(&fresh, f->items, f->n, path_mode, pat, plen, 0);
    n = yew_filter_top(&fresh, f->items, path_mode, out, max);
    yew_filter_free(&fresh);
    return n;
}

/* ---------------------------------------------------------------- */
/* THE property (DoD 7)                                             */
/* ---------------------------------------------------------------- */

/*
 * For 10 000 random (pattern, appended-char) pairs over a 5 000-item
 * set, narrowing equals a full rescan exactly — same matched count,
 * same visible order, same scores.
 */
void test_narrow_equals_a_full_rescan(void)
{
    static const char alphabet[] = "abcdefgpstuw./0123456789";
    NwFix f;
    FilterState fs;
    u64 rng = 0x9E3779B97F4A7C15ULL;
    u32 trial;
    u32 extensions = 0U;
    u32 narrowed_cases = 0U;

    nw_make(&f, 5000U, 12345U);
    yew_filter_init(&fs);
    for (trial = 0U; trial < 10000U; trial++) {
        char pat[8];
        u32 plen;
        u32 k;
        FzRanked got[YEW_FILTER_TOPK];
        FzRanked want[YEW_FILTER_TOPK];
        u32 n_got;
        u32 n_want;
        u32 before_append;

        /* A base pattern of 0-3 characters... */
        rng = rng * 6364136223846793005ULL + 1442695040888963407ULL;
        plen = (u32)((rng >> 33) % 4U);
        for (k = 0U; k < plen; k++) {
            rng = rng * 6364136223846793005ULL + 1442695040888963407ULL;
            pat[k] = alphabet[(rng >> 29) % (sizeof(alphabet) - 1U)];
        }
        pat[plen] = '\0';

        /* ...applied fresh, then EXTENDED by one character. */
        yew_filter_reset(&fs, f.items, f.n, 0U);
        (void)yew_filter_apply(&fs, f.items, f.n, true, pat, plen, 0);
        rng = rng * 6364136223846793005ULL + 1442695040888963407ULL;
        pat[plen] = alphabet[(rng >> 29) % (sizeof(alphabet) - 1U)];
        plen++;
        pat[plen] = '\0';
        before_append = yew_filter_matched(&fs);
        yew_filter_scored_reset();
        (void)yew_filter_apply(&fs, f.items, f.n, true, pat, plen, 0);
        /*
         * It scored EXACTLY the candidate set — no more, no fewer.
         *
         * The obvious check, "fewer than the whole set", is wrong twice
         * over: extending the empty pattern legitimately scores
         * everything because the empty pattern matched everything, and
         * so does a base pattern that happens to match every item.
         * Both narrowed correctly.  Comparing against the set SIZE says
         * what the optimization actually claims, and still fails
         * loudly against an implementation that quietly rescans.
         */
        YEW_ASSERT_EQ_U64(yew_filter_scored(), (u64)before_append);
        extensions++;
        if (before_append < f.n)
            narrowed_cases++;

        n_got = yew_filter_top(&fs, f.items, true, got, YEW_FILTER_TOPK);
        n_want = nw_full(&f, true, pat, plen, want, YEW_FILTER_TOPK);

        if (n_got != n_want) {
            (void)fprintf(stderr,
                          "pattern '%s': narrowed %u rows, full %u\n", pat,
                          (unsigned)n_got, (unsigned)n_want);
        }
        YEW_ASSERT_EQ_U64(n_got, n_want);
        for (k = 0U; k < n_got; k++) {
            if (got[k].idx != want[k].idx ||
                got[k].score != want[k].score) {
                (void)fprintf(stderr,
                              "pattern '%s' row %u: narrowed idx=%u "
                              "score=%d, full idx=%u score=%d\n",
                              pat, (unsigned)k, (unsigned)got[k].idx,
                              (int)got[k].score, (unsigned)want[k].idx,
                              (int)want[k].score);
            }
            YEW_ASSERT_EQ_U64(got[k].idx, want[k].idx);
            YEW_ASSERT_EQ_I64(got[k].score, want[k].score);
        }
    }
    /*
     * And in the overwhelming majority of trials the candidate set was
     * genuinely smaller than the corpus, so the per-trial equality
     * above is a statement about NARROWING rather than about two full
     * rescans agreeing with each other.
     */
    YEW_ASSERT_EQ_U64(extensions, 10000U);
    YEW_ASSERT(narrowed_cases > 7000U);
    yew_filter_free(&fs);
    nw_free(&f);
}

/* ---------------------------------------------------------------- */
/* Narrowing does less work                                         */
/* ---------------------------------------------------------------- */

/*
 * The whole point: appending a character rescores only what already
 * matched.  Asserted by COUNTING, because a narrowed pass and a full
 * one produce the same list and differ only in cost.
 */
void test_narrow_append_scores_only_the_candidate_set(void)
{
    NwFix f;
    FilterState fs;
    u32 after_first;

    nw_make(&f, 5000U, 999U);
    yew_filter_init(&fs);
    yew_filter_reset(&fs, f.items, f.n, 0U);

    /* First character: everything is scored, because the empty pattern
     * matched everything. */
    yew_filter_scored_reset();
    (void)yew_filter_apply(&fs, f.items, f.n, true, "s", 1U, 0);
    YEW_ASSERT_EQ_U64(yew_filter_scored(), (u64)f.n);
    after_first = yew_filter_matched(&fs);
    YEW_ASSERT(after_first > 0U);
    YEW_ASSERT(after_first < f.n);

    /* Second: only the survivors. */
    yew_filter_scored_reset();
    (void)yew_filter_apply(&fs, f.items, f.n, true, "st", 2U, 0);
    YEW_ASSERT_EQ_U64(yew_filter_scored(), (u64)after_first);
    /* And the set only shrinks. */
    YEW_ASSERT(yew_filter_matched(&fs) <= after_first);

    yew_filter_free(&fs);
    nw_free(&f);
}

/*
 * BACKSPACE cannot narrow — the new pattern is not an extension — so it
 * rescans everything.  Getting this wrong would silently keep files out
 * of the list after a correction, which is the same invisible failure
 * as a bad narrow.
 */
void test_narrow_backspace_rescans_everything(void)
{
    NwFix f;
    FilterState fs;
    u32 narrow_matched;

    nw_make(&f, 2000U, 4242U);
    yew_filter_init(&fs);
    yew_filter_reset(&fs, f.items, f.n, 0U);
    (void)yew_filter_apply(&fs, f.items, f.n, true, "sta", 3U, 0);
    narrow_matched = yew_filter_matched(&fs);

    yew_filter_scored_reset();
    (void)yew_filter_apply(&fs, f.items, f.n, true, "st", 2U, 0);
    /* The whole set, not the narrowed one. */
    YEW_ASSERT_EQ_U64(yew_filter_scored(), (u64)f.n);
    /* And the list GREW back, which is the observable half. */
    YEW_ASSERT(yew_filter_matched(&fs) >= narrow_matched);
    yew_filter_free(&fs);
    nw_free(&f);
}

/* A mid-line edit is not an extension either, however similar it
 * looks. */
void test_narrow_mid_edit_rescans_everything(void)
{
    NwFix f;
    FilterState fs;

    nw_make(&f, 1000U, 7U);
    yew_filter_init(&fs);
    yew_filter_reset(&fs, f.items, f.n, 0U);
    (void)yew_filter_apply(&fs, f.items, f.n, true, "abc", 3U, 0);
    yew_filter_scored_reset();
    /* Same length, different bytes. */
    (void)yew_filter_apply(&fs, f.items, f.n, true, "abd", 3U, 0);
    YEW_ASSERT_EQ_U64(yew_filter_scored(), (u64)f.n);
    yew_filter_free(&fs);
    nw_free(&f);
}

/* ---------------------------------------------------------------- */
/* Slicing                                                          */
/* ---------------------------------------------------------------- */

/*
 * A sliced rescan reaches the same answer as an unsliced one.
 *
 * If slicing could change the result, a filter that happened to finish
 * in one frame would show a different list than the same filter under
 * load — a bug that only appears on someone else's machine.
 */
void test_narrow_sliced_rescan_equals_unsliced(void)
{
    NwFix f;
    FilterState sliced;
    FzRanked got[YEW_FILTER_TOPK];
    FzRanked want[YEW_FILTER_TOPK];
    u32 n_got;
    u32 n_want;
    u32 steps = 0U;
    u32 k;

    nw_make(&f, 20000U, 31337U);
    yew_filter_init(&sliced);
    yew_filter_reset(&sliced, f.items, f.n, 0U);
    /* A 1 us budget forces many slices. */
    (void)yew_filter_apply(&sliced, f.items, f.n, true, "sc", 2U, 1);
    while (yew_filter_step(&sliced, f.items, true, 1)) {
        if (++steps >= 100000U)
            break;
    }
    YEW_ASSERT(steps < 100000U);
    n_got = yew_filter_top(&sliced, f.items, true, got, YEW_FILTER_TOPK);
    n_want = nw_full(&f, true, "sc", 2U, want, YEW_FILTER_TOPK);
    YEW_ASSERT_EQ_U64(n_got, n_want);
    for (k = 0U; k < n_got; k++) {
        YEW_ASSERT_EQ_U64(got[k].idx, want[k].idx);
        YEW_ASSERT_EQ_I64(got[k].score, want[k].score);
    }
    yew_filter_free(&sliced);
    nw_free(&f);
}

/*
 * Mid-pass the matched count is meaningful and only GROWS, which is
 * what lets the footer show a number while ` scanning…` is up.
 *
 * Stated in terms of the public claim rather than an internal cursor:
 * either pass can be the one in flight — the first keystroke narrows
 * from the empty set, a backspace rescans — and a test that watched
 * `scan_at` was really testing which branch it happened to take.
 */
void test_narrow_partial_scan_has_a_usable_count(void)
{
    NwFix f;
    FilterState fs;
    FzRanked partial[YEW_FILTER_TOPK];
    FzRanked whole[YEW_FILTER_TOPK];
    u32 seen_partial;
    u32 last = 0U;
    u32 steps = 0U;
    u32 n_partial;
    u32 n_whole;
    u32 k;
    bool monotone = true;
    bool pending;

    nw_make(&f, 50000U, 5U);
    yew_filter_init(&fs);
    yew_filter_reset(&fs, f.items, f.n, 0U);
    /* A 1 us budget stops almost immediately. */
    pending = !yew_filter_apply(&fs, f.items, f.n, true, "s", 1U, 1);
    YEW_ASSERT(pending);
    if (pending) {
        seen_partial = yew_filter_matched(&fs);
        /* Partway through: a usable count, and never more matches than
         * the corpus holds. */
        YEW_ASSERT(seen_partial <= f.n);
        /* The window is drawable mid-pass — that is the whole point of
         * showing a partial list rather than a spinner. */
        n_partial = yew_filter_top(&fs, f.items, true, partial,
                                   YEW_FILTER_TOPK);
        YEW_ASSERT(n_partial <= (u32)YEW_FILTER_TOPK);
        last = seen_partial;
        while (yew_filter_step(&fs, f.items, true, 1)) {
            u32 now_matched = yew_filter_matched(&fs);

            /* Monotone: a count that went backwards would make the
             * footer flicker downward as the scan progressed. */
            monotone = monotone && now_matched >= last;
            last = now_matched;
            if (++steps >= 200000U)
                break;
        }
    }
    YEW_ASSERT(monotone);
    YEW_ASSERT(steps < 200000U);
    /* And the finished answer equals an unsliced one. */
    n_whole = nw_full(&f, true, "s", 1U, whole, YEW_FILTER_TOPK);
    n_partial = yew_filter_top(&fs, f.items, true, partial,
                               YEW_FILTER_TOPK);
    YEW_ASSERT_EQ_U64(n_partial, n_whole);
    for (k = 0U; k < n_whole; k++)
        YEW_ASSERT_EQ_U64(partial[k].idx, whole[k].idx);
    yew_filter_free(&fs);
    nw_free(&f);
}

/* ---------------------------------------------------------------- */
/* Ordering, and agreement with yew_fz_rank                         */
/* ---------------------------------------------------------------- */

/*
 * The top-k agrees with yew_fz_rank, which sorts.
 *
 * Two orderings of one list is the drift §7 avoids by having only one
 * place that orders anything — so where they overlap they must agree,
 * or the picker and the tests describe different programs.
 */
void test_narrow_top_agrees_with_yew_fz_rank(void)
{
    NwFix f;
    FilterState fs;
    FzRanked top[YEW_FILTER_TOPK];
    FzRanked *ranked;
    const char **labels;
    u32 n_top;
    u32 n_rank;
    u32 i;

    nw_make(&f, 500U, 2024U);
    yew_filter_init(&fs);
    yew_filter_reset(&fs, f.items, f.n, 0U);
    (void)yew_filter_apply(&fs, f.items, f.n, true, "tab", 3U, 0);
    n_top = yew_filter_top(&fs, f.items, true, top, YEW_FILTER_TOPK);

    labels = yew_xreallocarray(NULL, f.n, sizeof(*labels));
    ranked = yew_xreallocarray(NULL, f.n, sizeof(*ranked));
    for (i = 0U; i < f.n; i++)
        labels[i] = f.items[i].label;
    n_rank = yew_fz_rank("tab", 3U, labels, f.n, true, ranked);

    YEW_ASSERT_EQ_U64(yew_filter_matched(&fs), n_rank);
    for (i = 0U; i < n_top; i++) {
        YEW_ASSERT_EQ_U64(top[i].idx, ranked[i].idx);
        YEW_ASSERT_EQ_I64(top[i].score, ranked[i].score);
    }
    free(labels);
    free(ranked);
    yew_filter_free(&fs);
    nw_free(&f);
}

/*
 * The empty pattern keeps everything IN SOURCE ORDER.
 *
 * Everything scores the same, so any tie-break at all reorders the whole
 * list.  This is not cosmetic: callers rely on row 0 being the first
 * item they supplied — the undo picker's "row 0 is the root" stopped
 * being true when a length tie-break slipped in here, and the failure
 * surfaced two files away as a document that would not travel to the
 * state the test asked for.
 */
void test_narrow_empty_pattern_keeps_source_order(void)
{
    NwFix f;
    FilterState fs;
    FzRanked top[YEW_FILTER_TOPK];
    u32 n;
    u32 i;

    nw_make(&f, 100U, 11U);
    yew_filter_init(&fs);
    yew_filter_reset(&fs, f.items, f.n, 0U);
    YEW_ASSERT_EQ_U64(yew_filter_matched(&fs), 100U);
    n = yew_filter_top(&fs, f.items, true, top, YEW_FILTER_TOPK);
    YEW_ASSERT_EQ_U64(n, (u64)YEW_FILTER_TOPK);
    for (i = 0U; i < n; i++) {
        /* Every score is the empty-pattern score... */
        YEW_ASSERT_EQ_I64(top[i].score, 1);
        /* ...and the order is exactly the order they were given in,
         * which the score assertion alone cannot say. */
        YEW_ASSERT_EQ_U64(top[i].idx, i);
    }
    yew_filter_free(&fs);
    nw_free(&f);
}

/*
 * And the empty pattern agrees with yew_fz_rank, which has the same
 * special case for the same reason — two orderings of one list is the
 * drift §7 exists to avoid.
 */
void test_narrow_empty_pattern_agrees_with_yew_fz_rank(void)
{
    NwFix f;
    FilterState fs;
    FzRanked top[YEW_FILTER_TOPK];
    FzRanked *ranked;
    const char **labels;
    u32 n_top;
    u32 i;

    nw_make(&f, 200U, 77U);
    yew_filter_init(&fs);
    yew_filter_reset(&fs, f.items, f.n, 0U);
    n_top = yew_filter_top(&fs, f.items, true, top, YEW_FILTER_TOPK);

    labels = yew_xreallocarray(NULL, f.n, sizeof(*labels));
    ranked = yew_xreallocarray(NULL, f.n, sizeof(*ranked));
    for (i = 0U; i < f.n; i++)
        labels[i] = f.items[i].label;
    (void)yew_fz_rank("", 0U, labels, f.n, true, ranked);
    for (i = 0U; i < n_top; i++)
        YEW_ASSERT_EQ_U64(top[i].idx, ranked[i].idx);
    free(labels);
    free(ranked);
    yew_filter_free(&fs);
    nw_free(&f);
}

/*
 * A changed item count forces a rescan rather than reusing indices into
 * a table that has moved underneath them.
 */
void test_narrow_item_count_change_forces_a_rescan(void)
{
    NwFix f;
    FilterState fs;

    nw_make(&f, 100U, 3U);
    yew_filter_init(&fs);
    yew_filter_reset(&fs, f.items, f.n, 0U);
    (void)yew_filter_apply(&fs, f.items, f.n, true, "s", 1U, 0);
    yew_filter_scored_reset();
    /* Same pattern, fewer items: not an extension of anything. */
    (void)yew_filter_apply(&fs, f.items, 50U, true, "s", 1U, 0);
    YEW_ASSERT_EQ_U64(yew_filter_scored(), 50U);
    YEW_ASSERT(yew_filter_matched(&fs) <= 50U);
    yew_filter_free(&fs);
    nw_free(&f);
}

/* Degenerate inputs do not crash. */
void test_narrow_degenerate_inputs(void)
{
    FilterState fs;
    FzRanked out[4];

    yew_filter_init(&fs);
    yew_filter_init(NULL);
    yew_filter_free(NULL);
    yew_filter_reset(NULL, NULL, 0U, 0U);
    yew_filter_reset(&fs, NULL, 0U, 0U);
    YEW_ASSERT(yew_filter_apply(NULL, NULL, 0U, false, "a", 1U, 0));
    YEW_ASSERT(!yew_filter_step(NULL, NULL, false, 0));
    YEW_ASSERT_EQ_U64(yew_filter_matched(NULL), 0U);
    YEW_ASSERT_EQ_U64(yew_filter_top(NULL, NULL, false, out, 4U), 0U);
    yew_filter_free(&fs);
}
