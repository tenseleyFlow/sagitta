/* Sprint 32 §7: "did you mean".  The contract is in suggest.h. */
#include "fl/suggest.h"

#include <string.h>

#include "util/sort.h"

/* ASCII only, and deliberately: the locale-aware ones are banned
 * project-wide, and a case fold that moved with LANG would make the
 * same typo suggest differently on two machines. */
static u8 lc(u8 c)
{
    return (c >= (u8)'A' && c <= (u8)'Z') ? (u8)(c + 32) : c;
}

void fl_suggest_reset(FlSuggest *s)
{
    s->n = 0U;
    s->full = false;
}

void fl_suggest_add(FlSuggest *s, const char *name, u32 len, FlScopeBand band)
{
    if (name == NULL || len == 0U)
        return;
    if (s->n >= (u32)FL_SUGGEST_MAX_CANDIDATES) {
        s->full = true;
        return;
    }
    s->v[s->n].name = name;
    s->v[s->n].len = len;
    s->v[s->n].band = (u8)band;
    s->v[s->n].dist = 0U;
    s->n++;
}

/*
 * OSA Damerau-Levenshtein over scaled costs, with an early exit.
 *
 * Three rows, because a transposition needs the row before last.  The
 * early exit is on the running row MINIMUM: once every cell in a row
 * exceeds the threshold, no later row can come back under it, and the
 * remaining work is wasted on a candidate that will not be shown.
 */
static u32 osa(const char *a, u32 alen, const char *b, u32 blen, u32 cutoff)
{
    u32 prev2[FL_SUGGEST_MAX_LEN + 1U];
    u32 prev[FL_SUGGEST_MAX_LEN + 1U];
    u32 cur[FL_SUGGEST_MAX_LEN + 1U];
    u32 i;
    u32 j;

    for (j = 0U; j <= blen; j++) {
        prev[j] = j * 2U;
        /* Never read at i == 1 -- the transposition arm is guarded --
         * but initialised so the compiler need not prove that. */
        prev2[j] = 0U;
    }
    for (i = 1U; i <= alen; i++) {
        u32 rowmin;

        cur[0] = i * 2U;
        rowmin = cur[0];
        for (j = 1U; j <= blen; j++) {
            u8 ca = (u8)a[i - 1U];
            u8 cb = (u8)b[j - 1U];
            u32 sub;
            u32 best;

            if (ca == cb)
                sub = 0U;
            else if (lc(ca) == lc(cb))
                sub = 1U;      /* the half-cost case */
            else
                sub = 2U;
            best = prev[j - 1U] + sub;
            if (prev[j] + 2U < best)
                best = prev[j] + 2U;
            if (cur[j - 1U] + 2U < best)
                best = cur[j - 1U] + 2U;
            if (i > 1U && j > 1U && lc(ca) == lc((u8)b[j - 2U]) &&
                lc((u8)a[i - 2U]) == lc(cb)) {
                /* Adjacent transposition, the whole reason this is
                 * Damerau rather than Levenshtein. */
                if (prev2[j - 2U] + 2U < best)
                    best = prev2[j - 2U] + 2U;
            }
            cur[j] = best;
            if (best < rowmin)
                rowmin = best;
        }
        if (rowmin > cutoff)
            return (u32)-1;
        (void)memcpy(prev2, prev, ((size_t)blen + 1U) * sizeof(prev[0]));
        (void)memcpy(prev, cur, ((size_t)blen + 1U) * sizeof(cur[0]));
    }
    return alen == 0U ? blen * 2U : prev[blen];
}

u32 fl_suggest_distance(const char *a, u32 alen, const char *b, u32 blen)
{
    if (alen > (u32)FL_SUGGEST_MAX_LEN || blen > (u32)FL_SUGGEST_MAX_LEN)
        return (u32)-1;
    return osa(a, alen, b, blen, (u32)-1);
}

/* The acceptance threshold, by CANDIDATE length. */
static u32 threshold(u32 candlen)
{
    if (candlen <= 2U)
        return 0U;         /* never suggest */
    if (candlen == 3U)
        return 2U;         /* one edit */
    return 4U;             /* two edits */
}

static int cand_cmp(const void *x, const void *y, void *ctx)
{
    const FlCand *a = x;
    const FlCand *b = y;
    u32 n;
    int c;

    (void)ctx;
    if (a->dist != b->dist)
        return a->dist < b->dist ? -1 : 1;
    /* Scope proximity: a shadowed local is what the user most likely
     * meant, so an equally-distant local beats a global. */
    if (a->band != b->band)
        return a->band < b->band ? -1 : 1;
    n = a->len < b->len ? a->len : b->len;
    c = n == 0U ? 0 : memcmp(a->name, b->name, (size_t)n);
    if (c != 0)
        return c;
    return a->len < b->len ? -1 : (a->len > b->len ? 1 : 0);
}

u32 fl_suggest_render(FlSuggest *s, const char *typo, u32 typolen,
                      Bytebuf *out)
{
    u32 keep = 0U;
    u32 i;
    u32 shown;

    if (typolen == 0U || typolen > (u32)FL_SUGGEST_MAX_LEN)
        return 0U;
    /*
     * Over the cap, prefix-filter on the first byte case-insensitively
     * before scoring; if that does not bring it under, say nothing.
     * The bound is the point -- a suggestion is a courtesy.
     */
    if (s->full) {
        u32 w = 0U;

        for (i = 0U; i < s->n; i++) {
            if (lc((u8)s->v[i].name[0]) == lc((u8)typo[0]))
                s->v[w++] = s->v[i];
        }
        s->n = w;
        if (w >= (u32)FL_SUGGEST_MAX_CANDIDATES)
            return 0U;
    }
    for (i = 0U; i < s->n; i++) {
        u32 lim = threshold(s->v[i].len);
        u32 d;

        if (lim == 0U || s->v[i].len > (u32)FL_SUGGEST_MAX_LEN)
            continue;
        /* An exact match is not a typo; the caller has a different
         * problem and a suggestion would be nonsense. */
        if (s->v[i].len == typolen &&
            memcmp(s->v[i].name, typo, (size_t)typolen) == 0)
            continue;
        d = osa(typo, typolen, s->v[i].name, s->v[i].len, lim);
        if (d > lim)
            continue;
        s->v[i].dist = d;
        s->v[keep++] = s->v[i];
    }
    if (keep == 0U)
        return 0U;
    yew_sort_stable(s->v, keep, sizeof(s->v[0]), cand_cmp, NULL);
    shown = keep < (u32)FL_SUGGEST_MAX_SHOWN ? keep
                                             : (u32)FL_SUGGEST_MAX_SHOWN;
    bytebuf_append(out, "did you mean '", 14U);
    bytebuf_append(out, s->v[0].name, (size_t)s->v[0].len);
    bytebuf_append(out, "'?", 2U);
    if (shown > 1U) {
        bytebuf_append(out, " (or ", 5U);
        for (i = 1U; i < shown; i++) {
            if (i > 1U)
                bytebuf_append(out, ", ", 2U);
            bytebuf_push_u8(out, (u8)'\'');
            bytebuf_append(out, s->v[i].name, (size_t)s->v[i].len);
            bytebuf_push_u8(out, (u8)'\'');
        }
        bytebuf_push_u8(out, (u8)')');
    }
    return shown;
}
