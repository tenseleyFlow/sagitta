#ifndef SAG_UI_FILTER_H
#define SAG_UI_FILTER_H

/*
 * Sprint 26 §7: incremental filtering inside the keystroke budget.
 *
 * THE NUMBERS THIS IS DESIGNED AGAINST.  100 000 candidates, mean path
 * 40 bytes.  sag_fz_score is one allocation-free pass over the text at
 * roughly 200 ns per candidate, so a FULL rescan is about 20 ms — four
 * times over the 5 ms keypress-to-paint gate (invariant 4).  Every
 * decision below follows from that one measurement.
 *
 * NARROWING ON APPEND IS EXACT, NOT AN APPROXIMATION.  When the new
 * pattern has the old one as a strict prefix, only the candidates that
 * already matched can still match: a subsequence match cannot appear by
 * adding a character to the pattern.  So rescoring just the current set
 * is not a heuristic that is usually right, it is the same answer for
 * less work — which is what DoD 7 proves against a full rescan over
 * 10 000 random cases.
 *
 * THE CANDIDATE SET IS NEVER SORTED.  At most twenty rows are shown, so
 * bounded insertion into a twenty-slot top-k beats sorting 100 000
 * elements by two orders of magnitude, and only the visible window
 * needs an order at all.  sag_fz_rank still exists and still sorts; it
 * is for tests and for callers with a small n.
 *
 * NO strlen IN THE INNER LOOP and no per-candidate allocation: lengths
 * are stored beside the paths and both vectors are reserved once at
 * open.  At 100 000 candidates a strlen per keystroke is another pass
 * over every byte, for a number we already knew.
 */

#include "ui/picker.h"
#include "util/base.h"
#include "ws/finder.h"

enum {
    SAG_FILTER_PAT_MAX = 256,
    /* The visible window.  Nothing below the top-k is ordered because
     * nothing below it is drawn. */
    SAG_FILTER_TOPK = 32
};

/*
 * A candidate: the item's index and its length, kept together so the
 * scoring loop never asks the string how long it is.
 */
typedef struct FilterCand {
    u32 idx;
    u32 len;
} FilterCand;

typedef struct FilterState {
    /* Item indices currently matching, and their scores. */
    FilterCand *cand;
    i32 *score;
    u32 n_cand;
    u32 cap;

    char pat[SAG_FILTER_PAT_MAX];
    u32 plen;

    /*
     * The item source's generation.  A background refresh bumps it and
     * forces a full rescan at the next keystroke, so a stale narrowed
     * set can never outlive the list it was narrowed from.
     */
    u32 src_gen;
    /* Resume cursor for a sliced full rescan; n_total when complete. */
    u32 scan_at;
    u32 n_total;
    bool scanning;
} FilterState;

void sag_filter_init(FilterState *f);
void sag_filter_free(FilterState *f);

/*
 * Points the filter at `n` items and resets it.  Reserves both vectors
 * once, here, so no keystroke ever allocates.
 */
void sag_filter_reset(FilterState *f, const PickItem *items, u32 n,
                      u32 src_gen);

/*
 * Applies `pat`.  Returns true when the result is COMPLETE; false when
 * a sliced rescan is still running and the caller should keep calling
 * sag_filter_step (and show ` scanning…`).
 *
 * `budget_us` bounds the work done here; 0 means run to completion,
 * which is what tests and small lists use.
 */
bool sag_filter_apply(FilterState *f, const PickItem *items, u32 n,
                      bool path_mode, const char *pat, u32 plen,
                      i64 budget_us);

/* Continues a sliced rescan.  False when it is finished. */
bool sag_filter_step(FilterState *f, const PickItem *items,
                     bool path_mode, i64 budget_us);

/*
 * The visible window, ordered.  Fills at most `max` entries and returns
 * how many — the only place an order is computed, because it is the
 * only place one is seen.
 */
u32 sag_filter_top(const FilterState *f, const PickItem *items,
                   bool path_mode, FzRanked *out, u32 max);

/* Matches found so far.  Meaningful mid-scan, which is why the footer
 * can show a count while ` scanning…` is up. */
u32 sag_filter_matched(const FilterState *f);

/*
 * Test hook: candidates SCORED since the last reset.  The narrowing
 * claim is about work avoided, and counting is the only way to tell a
 * narrowed pass from a full one that happened to agree.
 */
u64 sag_filter_scored(void);
void sag_filter_scored_reset(void);

#endif
