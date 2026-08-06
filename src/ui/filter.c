/*
 * Sprint 26 §7.  See filter.h for the measurement every decision here
 * follows from.
 */
#define _POSIX_C_SOURCE 200809L

#include "ui/filter.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "util/log.h"

static u64 g_scored;

u64 sag_filter_scored(void)
{
    return g_scored;
}

void sag_filter_scored_reset(void)
{
    g_scored = 0U;
}

static i64 now_us(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return 0;
    return (i64)ts.tv_sec * 1000000 + (i64)ts.tv_nsec / 1000;
}

void sag_filter_init(FilterState *f)
{
    if (f != NULL)
        (void)memset(f, 0, sizeof(*f));
}

void sag_filter_free(FilterState *f)
{
    if (f == NULL)
        return;
    free(f->cand);
    free(f->score);
    (void)memset(f, 0, sizeof(*f));
}

static void reserve(FilterState *f, u32 n)
{
    if (f->cap >= n)
        return;
    f->cand = sag_xreallocarray(f->cand, n, sizeof(*f->cand));
    f->score = sag_xreallocarray(f->score, n, sizeof(*f->score));
    f->cap = n;
}

void sag_filter_reset(FilterState *f, const PickItem *items, u32 n,
                      u32 src_gen)
{
    u32 i;

    if (f == NULL)
        return;
    /* Reserved ONCE, here.  No keystroke allocates. */
    reserve(f, n);
    f->n_total = n;
    f->src_gen = src_gen;
    f->plen = 0U;
    f->pat[0] = '\0';
    f->scan_at = 0U;
    f->scanning = false;
    f->n_cand = 0U;
    if (items == NULL)
        return;
    /*
     * The empty pattern matches everything, and the lengths are taken
     * here — once — so the scoring loop never calls strlen.
     */
    for (i = 0U; i < n; i++) {
        f->cand[i].idx = i;
        f->cand[i].len = items[i].label == NULL
                             ? 0U
                             : (u32)strlen(items[i].label);
        f->score[i] = 1;
    }
    f->n_cand = n;
}

/*
 * The scoring key for one candidate: §2's two-pass basename rule,
 * without the sort sag_fz_rank does.
 */
static i32 score_one(const PickItem *items, const FilterCand *c,
                     bool path_mode, const char *pat, u32 plen)
{
    const char *text = items[c->idx].label;
    i32 sb;
    i32 sp = SAG_FZ_NO_MATCH;
    u32 boff = 0U;
    u32 blen = c->len;

    g_scored++;
    if (text == NULL)
        return SAG_FZ_NO_MATCH;
    if (path_mode) {
        u32 end = c->len;
        u32 start;

        while (end > 0U && text[end - 1U] == '/')
            end--;
        start = end;
        while (start > 0U && text[start - 1U] != '/')
            start--;
        boff = start;
        blen = end - start;
    }
    sb = sag_fz_score(pat, plen, text + boff, blen, NULL);
    if (path_mode)
        sp = sag_fz_score(pat, plen, text, c->len, NULL);
    if (sb == SAG_FZ_NO_MATCH && sp == SAG_FZ_NO_MATCH)
        return SAG_FZ_NO_MATCH;
    /* A basename hit at prefix strength or better outranks every fuzzy
     * path match, globally — which is why `src` selects `src/` and not
     * `src/file.c`. */
    if (sb >= 5000) {
        return sb > INT32_MAX - (i32)SAG_FZ_BASENAME_TIER
                   ? INT32_MAX
                   : sb + (i32)SAG_FZ_BASENAME_TIER;
    }
    if (sb != SAG_FZ_NO_MATCH && sb >= sp)
        return sb;
    return sp;
}

/* Rescores the CURRENT set in place, dropping what no longer matches. */
static void narrow(FilterState *f, const PickItem *items, bool path_mode)
{
    u32 kept = 0U;
    u32 i;

    for (i = 0U; i < f->n_cand; i++) {
        i32 s = score_one(items, &f->cand[i], path_mode, f->pat, f->plen);

        if (s == SAG_FZ_NO_MATCH)
            continue;
        f->cand[kept] = f->cand[i];
        f->score[kept] = s;
        kept++;
    }
    f->n_cand = kept;
}

/* True when `pat` has the filter's current pattern as a STRICT prefix. */
static bool extends(const FilterState *f, const char *pat, u32 plen)
{
    if (plen <= f->plen)
        return false;
    if (f->plen == 0U)
        return true;
    return memcmp(f->pat, pat, (size_t)f->plen) == 0;
}

bool sag_filter_apply(FilterState *f, const PickItem *items, u32 n,
                      bool path_mode, const char *pat, u32 plen,
                      i64 budget_us)
{
    bool can_narrow;

    if (f == NULL || items == NULL)
        return true;
    if (plen > (u32)SAG_FILTER_PAT_MAX - 1U)
        plen = (u32)SAG_FILTER_PAT_MAX - 1U;
    /*
     * The item source changed under us, so nothing the old set knew is
     * still true.  Cheaper to notice here than to hand back a list of
     * indices into a table that has moved.
     */
    if (n != f->n_total) {
        sag_filter_reset(f, items, n, f->src_gen);
        can_narrow = plen == 0U;
    } else {
        can_narrow = !f->scanning && extends(f, pat, plen);
    }
    if (plen > 0U)
        (void)memcpy(f->pat, pat, (size_t)plen);
    f->pat[plen] = '\0';
    f->plen = plen;

    if (can_narrow) {
        /*
         * THE fast path, and it is exact: a subsequence match cannot
         * appear by adding a character to the pattern, so nothing
         * outside the current set could have started matching.
         */
        narrow(f, items, path_mode);
        f->scanning = false;
        f->scan_at = f->n_total;
        return true;
    }
    /*
     * Backspace or a mid-line edit.  The old set is useless — the new
     * pattern is not an extension of anything — so this is a full
     * rescan, sliced if the caller gave us a budget.
     */
    f->n_cand = 0U;
    f->scan_at = 0U;
    f->scanning = true;
    return !sag_filter_step(f, items, path_mode, budget_us);
}

bool sag_filter_step(FilterState *f, const PickItem *items, bool path_mode,
                     i64 budget_us)
{
    i64 started = budget_us > 0 ? now_us() : 0;
    u32 checked = 0U;

    if (f == NULL || items == NULL || !f->scanning)
        return false;
    while (f->scan_at < f->n_total) {
        FilterCand c;
        i32 s;

        c.idx = f->scan_at;
        c.len = items[f->scan_at].label == NULL
                    ? 0U
                    : (u32)strlen(items[f->scan_at].label);
        s = score_one(items, &c, path_mode, f->pat, f->plen);
        f->scan_at++;
        if (s != SAG_FZ_NO_MATCH) {
            f->cand[f->n_cand] = c;
            f->score[f->n_cand] = s;
            f->n_cand++;
        }
        /*
         * The clock is consulted every 256 candidates rather than every
         * one: at 200 ns of work per candidate, a clock_gettime per
         * item would cost more than the scoring it is timing.
         */
        checked++;
        if (budget_us > 0 && (checked & 0xFFU) == 0U &&
            now_us() - started >= budget_us)
            return true;
    }
    f->scanning = false;
    return false;
}

u32 sag_filter_matched(const FilterState *f)
{
    return f == NULL ? 0U : f->n_cand;
}

/*
 * The visible window, and the ONLY place an order exists.
 *
 * Bounded insertion into `max` slots: one pass over the matches, no
 * allocation, and no sort of a set whose tail nobody will ever see.
 */
u32 sag_filter_top(const FilterState *f, const PickItem *items,
                   bool path_mode, FzRanked *out, u32 max)
{
    u32 held = 0U;
    u32 i;

    if (f == NULL || items == NULL || out == NULL || max == 0U)
        return 0U;
    for (i = 0U; i < f->n_cand; i++) {
        i32 s = f->score[i];
        u32 at;
        u32 k;

        /* Where this score belongs among the ones already held. */
        at = held;
        while (at > 0U) {
            const FzRanked *prev = &out[at - 1U];
            bool better;

            if (prev->score != s) {
                better = s > prev->score;
            } else if (f->plen == 0U) {
                /*
                 * THE EMPTY PATTERN PRESERVES SOURCE ORDER.
                 *
                 * Everything scores the same, so any tie-break at all
                 * would reorder the whole list — and sag_fz_rank has
                 * the same special case for the same reason.  Without
                 * it a freshly opened picker showed its candidates
                 * sorted by length, which put row 0 somewhere the
                 * caller never expected: the undo picker's "row 0 is
                 * the root" stopped being true.
                 */
                better = false;
            } else {
                /*
                 * Ties break by shorter text, then by bytes — the same
                 * order sag_fz_rank uses, so the two agree.  A tie
                 * broken by array position would depend on walk order
                 * and leak the filesystem into the list (invariant 5).
                 */
                u32 prev_len = (u32)strlen(items[prev->idx].label);

                if (prev_len != f->cand[i].len)
                    better = f->cand[i].len < prev_len;
                else
                    better = strcmp(items[f->cand[i].idx].label,
                                    items[prev->idx].label) < 0;
            }
            if (!better)
                break;
            at--;
        }
        if (at >= max)
            continue;
        /* Shift the tail down; `max` is 32, so this is a handful of
         * moves and never a sort. */
        for (k = held < max ? held : max - 1U; k > at; k--)
            out[k] = out[k - 1U];
        out[at].idx = f->cand[i].idx;
        out[at].score = s;
        /*
         * Positions are recomputed only for what is DRAWN.  Capturing
         * them during scoring would mean 100 000 FzMatch structs for
         * the twenty the user sees.
         */
        {
            const char *text = items[f->cand[i].idx].label;
            u32 boff = 0U;
            u32 blen = f->cand[i].len;
            FzMatch m;
            i32 sb;

            if (path_mode) {
                u32 end = f->cand[i].len;
                u32 start;

                while (end > 0U && text[end - 1U] == '/')
                    end--;
                start = end;
                while (start > 0U && text[start - 1U] != '/')
                    start--;
                boff = start;
                blen = end - start;
            }
            sb = sag_fz_score(f->pat, f->plen, text + boff, blen, &m);
            if (sb >= 5000 || !path_mode) {
                u32 j;

                for (j = 0U; j < m.n_pos; j++)
                    m.pos[j] = (u16)(m.pos[j] + boff);
                out[at].m = m;
            } else {
                (void)sag_fz_score(f->pat, f->plen, text, f->cand[i].len,
                                   &m);
                out[at].m = m;
            }
        }
        if (held < max)
            held++;
    }
    return held;
}
