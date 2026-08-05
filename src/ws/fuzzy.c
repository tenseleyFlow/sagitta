/*
 * Sprint 18.5 §1.  See ws/finder.h for the scoring table and the three
 * deliberate divergences from the Fortran original.
 */
#include "ws/finder.h"

#include <string.h>

#include "util/sort.h"

/*
 * ASCII-only folding.  Full Unicode folding of both strings on every
 * keystroke across a large candidate set is not free, and would need a
 * normalization pass to be correct rather than merely slower.  A
 * non-ASCII filename is therefore findable case-sensitively and matches
 * itself exactly -- pinned and tested rather than discovered.
 */
static char fz_fold(char c)
{
    return c >= 'A' && c <= 'Z' ? (char)(c + ('a' - 'A')) : c;
}

static bool fz_eq_fold(const char *pat, u32 plen, const char *text, u32 tlen)
{
    u32 i;

    if (plen != tlen)
        return false;
    for (i = 0U; i < plen; i++) {
        if (fz_fold(pat[i]) != fz_fold(text[i]))
            return false;
    }
    return true;
}

static bool fz_pfx_fold(const char *pat, u32 plen, const char *text)
{
    u32 i;

    for (i = 0U; i < plen; i++) {
        if (fz_fold(pat[i]) != fz_fold(text[i]))
            return false;
    }
    return true;
}

static bool fz_boundary(char c)
{
    return c == '/' || c == '_' || c == '-' || c == '.';
}

/*
 * The exact and prefix rows return before the scan, so they never reach
 * the loop that records positions -- and a prefix match is the COMMON
 * case, which would leave the most-shown rows with nothing highlighted.
 * Filling 0..plen-1 here changes no score and is what makes §5's match
 * highlighting mean anything.
 */
static void fz_fill_prefix_pos(FzMatch *m, u32 plen)
{
    u32 i;
    u32 n = plen > (u32)SAG_FZ_MAX_POS ? (u32)SAG_FZ_MAX_POS : plen;

    if (m == NULL)
        return;
    for (i = 0U; i < n; i++)
        m->pos[i] = (u16)i;
    m->n_pos = (u16)n;
}

static i32 fz_no_match(FzMatch *m)
{
    if (m != NULL)
        m->n_pos = 0U;
    return SAG_FZ_NO_MATCH;
}

i32 sag_fz_score(const char *pat, u32 plen, const char *text, u32 tlen,
                 FzMatch *m)
{
    /*
     * Accumulated in i64 and clamped once at the end.  A 4 KiB path
     * matched consecutively runs to hundreds of millions, and signed
     * overflow is undefined -- the fuzzer feeds exactly those inputs.
     */
    i64 score = 0;
    u32 pi = 0U;
    u32 ti;
    u32 run = 0U;
    bool started = false;

    if (m != NULL)
        m->n_pos = 0U;
    if (pat == NULL || text == NULL)
        return fz_no_match(m);
    if (plen == 0U)
        return 1; /* everything matches; source order is preserved */
    if (plen > tlen)
        return fz_no_match(m); /* cheap reject before any scanning */
    if (fz_eq_fold(pat, plen, text, tlen)) {
        fz_fill_prefix_pos(m, plen);
        return 10000;
    }
    if (fz_pfx_fold(pat, plen, text)) {
        fz_fill_prefix_pos(m, plen);
        return 5000;
    }
    for (ti = 0U; ti < tlen && pi < plen; ti++) {
        if (fz_fold(text[ti]) != fz_fold(pat[pi])) {
            run = 0U;
            if (started)
                score -= 1; /* gap, counted only after the first match */
            continue;
        }
        started = true;
        score += 100;
        run++;
        /* The 2nd char of a run is worth +100, the 3rd +150, and so on;
         * the first char of a run gets no run bonus. */
        if (run >= 2U)
            score += 50 * (i64)run;
        if (ti == 0U)
            score += 200;
        else if (fz_boundary(text[ti - 1U]))
            score += 150;
        if (m != NULL && m->n_pos < (u16)SAG_FZ_MAX_POS)
            m->pos[m->n_pos++] = (u16)ti;
        pi++;
    }
    if (pi < plen)
        return fz_no_match(m);
    score -= (i64)tlen;
    if (score <= (i64)SAG_FZ_NO_MATCH)
        return SAG_FZ_NO_MATCH + 1;
    if (score > (i64)INT32_MAX)
        return INT32_MAX;
    return (i32)score;
}

/*
 * The last path component, ignoring trailing slashes.
 *
 * Sprint 26's walk yields file paths, so it could scan for the last '/'
 * and stop.  This sprint's caller is the PATH completion source, which
 * marks a directory by appending '/' -- and a bare last-slash scan gives
 * "archive/" the empty basename, so the directory the user is typing
 * toward loses to every file beside it.  One helper, one behaviour.
 *
 * Not POSIX basename(3): that may mutate its argument.
 */
static void fz_basename(const char *text, u32 tlen, u32 *off, u32 *len)
{
    u32 end = tlen;
    u32 start;

    while (end > 0U && text[end - 1U] == '/')
        end--;
    start = end;
    while (start > 0U && text[start - 1U] != '/')
        start--;
    *off = start;
    *len = end - start;
}

static void fz_shift_pos(FzMatch *m, u32 off)
{
    u16 i;

    if (off == 0U)
        return;
    for (i = 0U; i < m->n_pos; i++) {
        u32 at = (u32)m->pos[i] + off;

        m->pos[i] = at > (u32)UINT16_MAX ? (u16)UINT16_MAX : (u16)at;
    }
}

typedef struct FzSortCtx {
    const char *const *text;
    bool empty_pattern;
} FzSortCtx;

/*
 * Descending by score; ties by shorter text, then memcmp of the bytes.
 * Deterministic across libcs, which is what the goldens depend on.
 *
 * The empty pattern is the one exception, and the two rules genuinely
 * collide there: the table says an empty pattern preserves SOURCE order,
 * while the tie-break would re-sort every candidate alphabetically.  The
 * tie-break exists to pin an order that would otherwise vary between
 * libcs -- with no pattern there is nothing to rank and the source's own
 * order is already deterministic, so the table wins.  In practice this
 * is what keeps a bare `:` menu grouped by command domain instead of
 * alphabetised into noise.
 */
static int fz_rank_cmp(const void *left, const void *right, void *ctx)
{
    const FzRanked *a = left;
    const FzRanked *b = right;
    const FzSortCtx *sc = ctx;
    const char *ta;
    const char *tb;
    size_t la;
    size_t lb;
    size_t n;
    int by_bytes;

    if (a->score != b->score)
        return a->score > b->score ? -1 : 1;
    if (sc->empty_pattern)
        return 0; /* stable sort -> source order survives */
    ta = sc->text[a->idx];
    tb = sc->text[b->idx];
    la = strlen(ta);
    lb = strlen(tb);
    if (la != lb)
        return la < lb ? -1 : 1;
    n = la;
    by_bytes = memcmp(ta, tb, n);
    if (by_bytes != 0)
        return by_bytes < 0 ? -1 : 1;
    return 0;
}

u32 sag_fz_rank(const char *pat, u32 plen, const char *const *text, u32 n,
                bool path_mode, FzRanked *out)
{
    FzSortCtx ctx;
    u32 kept = 0U;
    u32 i;

    if (text == NULL || out == NULL)
        return 0U;
    for (i = 0U; i < n; i++) {
        FzMatch base_m;
        /* Only written under path_mode, and only read on a branch that
         * path_mode reaches -- initialized anyway so no compiler in the
         * gcc/clang matrix can call it maybe-uninitialized under -Werror. */
        FzMatch path_m = {0U, {0U}};
        const char *candidate = text[i];
        u32 tlen;
        u32 boff;
        u32 blen;
        i32 sb;
        i32 sp = SAG_FZ_NO_MATCH;
        i32 key;

        if (candidate == NULL)
            continue;
        tlen = (u32)strlen(candidate);
        /* Command names, buffer labels, and undo descriptions are not
         * paths: scoring their last '.'-or-'/' segment would rank
         * `ed.file.write` on "write" alone and put every *.write command
         * in a tie.  basename() applies to paths only. */
        if (path_mode) {
            fz_basename(candidate, tlen, &boff, &blen);
        } else {
            boff = 0U;
            blen = tlen;
        }
        sb = sag_fz_score(pat, plen, candidate + boff, blen, &base_m);
        if (path_mode)
            sp = sag_fz_score(pat, plen, candidate, tlen, &path_m);
        if (sb == SAG_FZ_NO_MATCH && sp == SAG_FZ_NO_MATCH)
            continue;
        if (sb >= 5000) {
            key = sb > INT32_MAX - SAG_FZ_BASENAME_TIER
                      ? INT32_MAX
                      : sb + (i32)SAG_FZ_BASENAME_TIER;
            fz_shift_pos(&base_m, boff);
            out[kept].m = base_m;
        } else if (sb != SAG_FZ_NO_MATCH && sb >= sp) {
            key = sb;
            fz_shift_pos(&base_m, boff);
            out[kept].m = base_m;
        } else {
            key = sp;
            out[kept].m = path_m;
        }
        out[kept].idx = i;
        out[kept].score = key;
        kept++;
    }
    ctx.text = text;
    ctx.empty_pattern = plen == 0U;
    sag_sort_stable(out, kept, sizeof(*out), fz_rank_cmp, &ctx);
    return kept;
}
