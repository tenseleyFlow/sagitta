/*
 * Sprint 18.5 §1: the fuzzy scorer under arbitrary bytes.
 *
 * yew_fz_score is the one place in this sprint that takes attacker-shaped
 * input (any pattern against any text) and writes into a FIXED-SIZE array
 * of u16 offsets.  The properties below are exactly the ones §5's
 * highlighting reads: if a position is wrong, the menu underlines the
 * wrong byte, and under a multi-byte cluster that is invariant 2 territory.
 */

#include "fuzzlib.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util/base.h"
#include "ws/finder.h"

enum {
    FZ_FUZZ_MAX_PAT = 40,
    FZ_FUZZ_MAX_TEXTS = 32
};

static char fz_fold_ref(char c)
{
    return c >= 'A' && c <= 'Z' ? (char)(c - 'A' + 'a') : c;
}

/*
 * Every recorded position must name a byte that folds equal to the
 * pattern byte it stands for, and they must ascend.  Both hold for the
 * greedy scan and for the exact/prefix fill, which is the point: one
 * property covers all three return paths.
 */
static bool check_positions(const char *pat, u32 plen, const char *text,
                            u32 tlen, const FzMatch *m, i32 score,
                            char *why, size_t why_cap)
{
    u32 cap = plen > (u32)YEW_FZ_MAX_POS ? (u32)YEW_FZ_MAX_POS : plen;
    u32 i;

    if (score == YEW_FZ_NO_MATCH) {
        if (m->n_pos != 0U) {
            (void)snprintf(why, why_cap,
                           "no-match left %u positions behind",
                           (unsigned)m->n_pos);
            return false;
        }
        return true;
    }
    if ((u32)m->n_pos > cap) {
        (void)snprintf(why, why_cap, "n_pos %u exceeds cap %u",
                       (unsigned)m->n_pos, (unsigned)cap);
        return false;
    }
    for (i = 0U; i < (u32)m->n_pos; i++) {
        u32 at = (u32)m->pos[i];

        if (at >= tlen) {
            (void)snprintf(why, why_cap,
                           "position %u is %u, past text length %u",
                           (unsigned)i, (unsigned)at, (unsigned)tlen);
            return false;
        }
        if (i != 0U && at <= (u32)m->pos[i - 1U]) {
            (void)snprintf(why, why_cap,
                           "positions %u,%u are not ascending",
                           (unsigned)m->pos[i - 1U], (unsigned)at);
            return false;
        }
        if (fz_fold_ref(text[at]) != fz_fold_ref(pat[i])) {
            (void)snprintf(why, why_cap,
                           "position %u points at %02x, pattern wants %02x",
                           (unsigned)i, (unsigned)(u8)text[at],
                           (unsigned)(u8)pat[i]);
            return false;
        }
    }
    return true;
}

static bool check_score(const char *pat, u32 plen, const char *text,
                        u32 tlen, char *why, size_t why_cap)
{
    FzMatch m = {0U, {0U}};
    FzMatch again = {0U, {0U}};
    i32 score = yew_fz_score(pat, plen, text, tlen, &m);
    i32 repeat = yew_fz_score(pat, plen, text, tlen, &again);
    i32 bare = yew_fz_score(pat, plen, text, tlen, NULL);

    /* Determinism is invariant 5's precondition: the menu order is part of
     * the rendered frame, so an unstable score is an unstable frame. */
    if (repeat != score || bare != score) {
        (void)snprintf(why, why_cap,
                       "score is not deterministic: %d, %d, %d",
                       (int)score, (int)repeat, (int)bare);
        return false;
    }
    if (memcmp(&m, &again, sizeof(m)) != 0) {
        (void)snprintf(why, why_cap, "positions are not deterministic");
        return false;
    }
    /* NULL is rejected BEFORE the empty pattern is honoured, so the
     * empty-pattern rule is a property of empty strings, not of NULL. */
    if (pat == NULL || text == NULL) {
        if (score != YEW_FZ_NO_MATCH) {
            (void)snprintf(why, why_cap, "NULL input scored %d",
                           (int)score);
            return false;
        }
        return true;
    }
    if (plen == 0U && score != 1) {
        (void)snprintf(why, why_cap, "empty pattern scored %d, wanted 1",
                       (int)score);
        return false;
    }
    if (plen > tlen && score != YEW_FZ_NO_MATCH) {
        (void)snprintf(why, why_cap,
                       "pattern longer than text scored %d", (int)score);
        return false;
    }
    return check_positions(pat, plen, text, tlen, &m, score, why, why_cap);
}

static bool check_rank(const char *pat, u32 plen, const char *const *text,
                       u32 n, bool path_mode, FzRanked *out,
                       char *why, size_t why_cap)
{
    u32 kept = yew_fz_rank(pat, plen, text, n, path_mode, out);
    u32 i;

    if (kept > n) {
        (void)snprintf(why, why_cap, "rank kept %u of %u candidates",
                       (unsigned)kept, (unsigned)n);
        return false;
    }
    for (i = 0U; i < kept; i++) {
        u32 j;

        if (out[i].idx >= n) {
            (void)snprintf(why, why_cap, "rank row %u names index %u",
                           (unsigned)i, (unsigned)out[i].idx);
            return false;
        }
        if (out[i].score == YEW_FZ_NO_MATCH) {
            (void)snprintf(why, why_cap, "rank kept a non-match at %u",
                           (unsigned)i);
            return false;
        }
        if (i != 0U && out[i - 1U].score < out[i].score) {
            (void)snprintf(why, why_cap,
                           "rank scores ascend at %u: %d then %d",
                           (unsigned)i, (int)out[i - 1U].score,
                           (int)out[i].score);
            return false;
        }
        /* Quadratic, but n is bounded by FZ_FUZZ_MAX_TEXTS: a duplicated
         * index would silently double a row in the menu. */
        for (j = 0U; j < i; j++) {
            if (out[j].idx == out[i].idx) {
                (void)snprintf(why, why_cap,
                               "rank returned index %u twice",
                               (unsigned)out[i].idx);
                return false;
            }
        }
    }
    return true;
}

static bool check_fuzzy(const u8 *data, size_t len, char *why,
                        size_t why_cap)
{
    const char *texts[FZ_FUZZ_MAX_TEXTS];
    FzRanked ranked[FZ_FUZZ_MAX_TEXTS];
    char pattern[FZ_FUZZ_MAX_PAT];
    u32 plen;
    u32 n = 0U;
    char *rest;
    size_t rest_len;
    size_t at;
    size_t start;
    bool ok = true;
    u32 i;

    /*
     * The first byte picks a pattern length out of the front of the
     * input; the remainder splits on '\n' into candidates.  Deriving both
     * from the same bytes is what lets the mutator find inputs where the
     * pattern is a prefix, a subsequence, and a near-miss of the text at
     * the same time -- the three branches that disagree.
     */
    if (len == 0U) {
        if (!check_score(NULL, 0U, NULL, 0U, why, why_cap))
            return false;
        return check_score("", 0U, "", 0U, why, why_cap);
    }
    plen = (u32)(data[0] % (FZ_FUZZ_MAX_PAT + 1U));
    if ((size_t)plen > len - 1U)
        plen = (u32)(len - 1U);
    if (plen != 0U)
        (void)memcpy(pattern, data + 1, plen);

    rest_len = len - 1U - plen;
    rest = malloc(rest_len + 1U);
    if (rest == NULL) {
        (void)snprintf(why, why_cap, "out of memory");
        return false;
    }
    if (rest_len != 0U)
        (void)memcpy(rest, data + 1 + plen, rest_len);
    rest[rest_len] = '\0';

    /* Candidates must be NUL-terminated for yew_fz_rank, so the split
     * bytes become terminators in place.  Embedded NULs in the input just
     * end a candidate early, which is a legitimate shape to feed. */
    start = 0U;
    for (at = 0U; at <= rest_len && n < FZ_FUZZ_MAX_TEXTS; at++) {
        if (at != rest_len && rest[at] != '\n')
            continue;
        rest[at] = '\0';
        texts[n++] = rest + start;
        start = at + 1U;
    }

    for (i = 0U; i < n && ok; i++)
        ok = check_score(pattern, plen, texts[i], (u32)strlen(texts[i]),
                         why, why_cap);
    /* The whole remainder as one candidate exercises the long-text
     * clamping paths that a '\n'-split corpus rarely reaches. */
    if (ok)
        ok = check_score(pattern, plen, rest, (u32)rest_len, why, why_cap);
    if (ok)
        ok = check_rank(pattern, plen, texts, n, false, ranked, why,
                        why_cap);
    if (ok)
        ok = check_rank(pattern, plen, texts, n, true, ranked, why,
                        why_cap);
    free(rest);
    return ok;
}

int main(int argc, char **argv)
{
    return yew_fuzz_main(argc, argv, "fuzz_fuzzy", NULL, check_fuzzy);
}
