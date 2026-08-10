/*
 * Sprint 20 DoD 6: the differential campaign.
 *
 * Random bytes rarely form an interesting pattern, so patterns are
 * GENERATED over the §2 grammar — weighted, depth-capped, always
 * terminating — and inputs are drawn from each pattern's own alphabet so
 * matches are frequent rather than astronomically rare.
 *
 * Every case runs against both a contiguous buffer and a piece tree
 * built by random insertion.  That second backing is not redundancy: a
 * literal straddling two pieces is missed only on buffers that have been
 * edited, so a contiguous-only campaign would report all-clear on
 * exactly the bug §7 warns about.
 */
#include "fuzzlib.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "re_ref.h"
#include "search/regex.h"
#include "text/piece.h"
#include "util/arena.h"

typedef struct Rng {
    u64 s;
} Rng;

static u32 rng_next(Rng *r)
{
    /* xorshift64*: deterministic, so a failing seed reproduces exactly. */
    u64 x = r->s;

    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    r->s = x;
    return (u32)((x * 0x2545F4914F6CDD1DULL) >> 32);
}

static u32 rng_below(Rng *r, u32 n)
{
    return n == 0U ? 0U : rng_next(r) % n;
}

/* The alphabet patterns and inputs share, kept tiny so matches happen. */
static const char alphabet[] = "abc";

typedef struct Gen {
    char buf[256];
    size_t len;
    Rng *rng;
    u32 groups;
} Gen;

static void emit(Gen *g, const char *s)
{
    size_t n = strlen(s);

    if (g->len + n < sizeof(g->buf)) {
        (void)memcpy(g->buf + g->len, s, n);
        g->len += n;
    }
}

static void emit_ch(Gen *g, char c)
{
    if (g->len + 1U < sizeof(g->buf))
        g->buf[g->len++] = c;
}

/*
 * The generator tracks NULLABILITY — whether a construct can match the
 * empty string — and never puts * or + around something nullable.
 *
 * Why: an empty loop body is the one place a backtracker and an NFA
 * simulation legitimately disagree.  A backtracker takes the empty
 * iteration, sees no progress and stops; the NFA's empty path loops back
 * into a state already in the thread list, dies there, and only longer
 * parses survive.  Both are defensible leftmost-first readings, and the
 * engine's answer is pinned in the golden table with that reasoning.
 * Comparing them here would assert the oracle's reading over a decision
 * already made on purpose, so the campaign simply does not generate the
 * ambiguous shape — and keeps its teeth for everything else.
 */
static bool gen_alt(Gen *g, u32 depth);

static bool gen_atom(Gen *g, u32 depth)
{
    /* Depth cap is what guarantees termination — every recursive arm is
     * gated on it, so a generated pattern is always finite. */
    u32 pick = rng_below(g->rng, depth == 0U ? 5U : 9U);

    switch (pick) {
    case 0:
    case 1:
    case 2:
        emit_ch(g, alphabet[rng_below(g->rng, 3U)]);
        return false;
    case 3:
        emit_ch(g, '.');
        return false;
    case 4: {
        /* A small class over the shared alphabet. */
        emit_ch(g, '[');
        if (rng_below(g->rng, 4U) == 0U)
            emit_ch(g, '^');
        emit_ch(g, alphabet[rng_below(g->rng, 3U)]);
        if (rng_below(g->rng, 2U) == 0U) {
            emit_ch(g, '-');
            emit_ch(g, 'c');
        }
        emit_ch(g, ']');
        return false;
    }
    case 5:
        emit_ch(g, '^');
        return true; /* zero-width */
    case 6:
        emit_ch(g, '$');
        return true;
    case 7:
        emit(g, rng_below(g->rng, 2U) == 0U ? "\\b" : "\\B");
        return true;
    default:
        break;
    }
    /* A group; capturing only while slots remain, so the oracle's
     * 8-group ceiling is never the thing that differs. */
    {
        bool nullable;

        emit_ch(g, '(');
        if (g->groups + 1U >= YEW_REF_MAX_GROUPS ||
            rng_below(g->rng, 3U) == 0U)
            emit(g, "?:");
        else
            g->groups++;
        nullable = gen_alt(g, depth - 1U);
        emit_ch(g, ')');
        return nullable;
    }
}

static bool gen_piece(Gen *g, u32 depth)
{
    bool nullable = gen_atom(g, depth);
    u32 pick;

    /* A nullable atom takes only '?', which cannot loop. */
    pick = nullable ? 4U + rng_below(g->rng, 4U) : rng_below(g->rng, 8U);
    switch (pick) {
    case 0:
        emit_ch(g, '*');
        return true;
    case 1:
        emit_ch(g, '+');
        return nullable;
    case 2:
        emit_ch(g, '?');
        return true;
    case 3:
        emit(g, rng_below(g->rng, 2U) == 0U ? "*?" : "+?");
        return true;
    default:
        break; /* unquantified */
    }
    return nullable;
}

static bool gen_concat(Gen *g, u32 depth)
{
    u32 n = 1U + rng_below(g->rng, 3U);
    u32 i;
    bool nullable = true;

    /* A concatenation is nullable only if every piece is. */
    for (i = 0U; i < n; i++)
        nullable = gen_piece(g, depth) && nullable;
    return nullable;
}

static bool gen_alt(Gen *g, u32 depth)
{
    u32 n = 1U + rng_below(g->rng, depth == 0U ? 1U : 2U);
    u32 i;
    bool nullable = false;

    /* An alternation is nullable if ANY branch is. */
    for (i = 0U; i < n; i++) {
        if (i != 0U)
            emit_ch(g, '|');
        nullable = gen_concat(g, depth) || nullable;
    }
    return nullable;
}

static size_t gen_pattern(Rng *rng, char *out, size_t cap)
{
    Gen g;

    (void)memset(&g, 0, sizeof(g));
    g.rng = rng;
    g.groups = 0U;
    (void)gen_alt(&g, 2U);
    if (g.len >= cap)
        g.len = cap - 1U;
    (void)memcpy(out, g.buf, g.len);
    out[g.len] = '\0';
    return g.len;
}

static size_t gen_input(Rng *rng, u8 *out, size_t cap)
{
    size_t n = (size_t)rng_below(rng, 12U);
    size_t i;

    for (i = 0U; i < n && i < cap; i++) {
        u32 pick = rng_below(rng, 12U);

        if (pick < 8U)
            out[i] = (u8)alphabet[rng_below(rng, 3U)];
        else if (pick == 8U)
            out[i] = (u8)'\n';
        else if (pick == 9U)
            out[i] = (u8)' ';
        else if (pick == 10U)
            out[i] = (u8)'d'; /* outside the alphabet, so classes bite */
        else
            out[i] = (u8)0xFFU; /* invalid byte: the escape path */
    }
    return i;
}

/* Builds the same bytes as a fragmented piece tree. */
static TextBuf *tb_of(const u8 *bytes, size_t len, Rng *rng)
{
    TextBuf *tb = yew_textbuf_new();
    size_t i;

    for (i = 0U; i < len; i++) {
        /* Insert at a random existing position so pieces interleave. */
        u64 at = (u64)rng_below(rng, (u32)i + 1U);

        yew_textbuf_insert(tb, BYTEOFF(at), bytes + i, 1U);
    }
    /* The tree now holds a permutation, so read it back as the truth. */
    return tb;
}

static void materialize(const TextBuf *tb, u8 *out, size_t cap,
                        size_t *len_out)
{
    TextIter it;
    u64 at = 0U;
    u64 end = yew_textbuf_len(tb);
    size_t n = 0U;

    while (at < end && n < cap) {
        const u8 *chunk = NULL;
        size_t got = 0U;

        if (!yew_textiter_begin(&it, tb, BYTEOFF(at)) ||
            !yew_textiter_chunk(&it, tb, &chunk, &got) || got == 0U)
            break;
        if ((u64)got > end - at)
            got = (size_t)(end - at);
        if (n + got > cap)
            got = cap - n;
        (void)memcpy(out + n, chunk, got);
        n += got;
        at += (u64)got;
    }
    *len_out = n;
}

static u64 skipped;
static u64 compared;

static bool one_case(Rng *rng, char *why, size_t why_cap)
{
    char pat[256];
    u8 raw[64];
    u8 text[64];
    size_t patlen = gen_pattern(rng, pat, sizeof(pat));
    size_t rawlen = gen_input(rng, raw, sizeof(raw));
    size_t len;
    TextBuf *tb = tb_of(raw, rawlen, rng);
    Arena arena;
    YewRe *re;
    YewReInput in;
    YewReMatch m;
    YewRefMatch ref;
    YewRefResult want;
    bool got;

    /* The piece tree's contents are the permutation, so both engines
     * must be asked about THAT, not about `raw`. */
    materialize(tb, text, sizeof(text), &len);

    arena_init(&arena);
    re = yew_re_compile(&arena, pat, patlen, 0U, NULL);
    if (re == NULL) {
        /* The generator can emit something the engine rejects (a bare
         * `^*`, say); the oracle is not consulted about those. */
        skipped++;
        arena_free_all(&arena);
        yew_textbuf_free(tb);
        return true;
    }
    want = yew_ref_search(pat, patlen, text, len, 0U, &ref);
    if (want == YEW_REF_UNKNOWN) {
        /* Budget exhausted or a construct outside the shared subset —
         * skip, never fail: the oracle running out of road says nothing
         * about the engine. */
        skipped++;
        arena_free_all(&arena);
        yew_textbuf_free(tb);
        return true;
    }

    in = yew_re_input_bytes(text, (u64)len);
    (void)memset(&m, 0, sizeof(m));
    got = yew_re_search(re, &in, BYTEOFF(0U), &m);
    if (got != (want == YEW_REF_MATCH)) {
        (void)snprintf(why, why_cap,
                       "/%s/ on %zu bytes: engine says %s, oracle says %s",
                       pat, len, got ? "match" : "no match",
                       want == YEW_REF_MATCH ? "match" : "no match");
        goto fail;
    }
    if (got) {
        if (m.g[0].lo != ref.lo[0] || m.g[0].hi != ref.hi[0]) {
            char shown[80];
            size_t k;
            size_t w = 0U;

            for (k = 0U; k < len && w + 4U < sizeof(shown); k++) {
                if (text[k] >= 0x20U && text[k] < 0x7FU)
                    shown[w++] = (char)text[k];
                else
                    w += (size_t)snprintf(shown + w, sizeof(shown) - w,
                                          "\\x%02X", (unsigned)text[k]);
            }
            shown[w] = '\0';
            (void)snprintf(why, why_cap,
                           "/%s/ on \"%s\": span %llu..%llu, oracle "
                           "%llu..%llu",
                           pat, shown, (unsigned long long)m.g[0].lo,
                           (unsigned long long)m.g[0].hi,
                           (unsigned long long)ref.lo[0],
                           (unsigned long long)ref.hi[0]);
            goto fail;
        }
    }
    /* The boolean engines must agree with the span engine. */
    {
        bool tested = yew_re_test(re, &in, BYTEOFF(0U));

        if (tested != got) {
            (void)snprintf(why, why_cap,
                           "/%s/: search says %s but test says %s", pat,
                           got ? "match" : "no match",
                           tested ? "match" : "no match");
            goto fail;
        }
    }
    /* And the piece-tree backing must agree with the flat one — this is
     * the chunk-carry check. */
    {
        YewReInput tin = yew_re_input_textbuf(tb);
        YewReMatch tm;
        bool tgot;

        (void)memset(&tm, 0, sizeof(tm));
        tgot = yew_re_search(re, &tin, BYTEOFF(0U), &tm);
        if (tgot != got ||
            (got && (tm.g[0].lo != m.g[0].lo || tm.g[0].hi != m.g[0].hi))) {
            (void)snprintf(why, why_cap,
                           "/%s/: flat and piece-tree backings disagree",
                           pat);
            goto fail;
        }
    }
    compared++;
    arena_free_all(&arena);
    yew_textbuf_free(tb);
    return true;

fail:
    arena_free_all(&arena);
    yew_textbuf_free(tb);
    return false;
}

/*
 * The fuzzlib harness feeds bytes; here they only seed the generator, so
 * a corpus entry is a seed rather than a pattern.  That keeps every case
 * reproducible from a single number.
 */
static bool check_re_diff(const u8 *data, size_t len, char *why,
                          size_t why_cap)
{
    Rng rng;
    size_t i;
    u32 rounds;

    rng.s = 0x9E3779B97F4A7C15ULL;
    for (i = 0U; i < len; i++) {
        rng.s ^= (u64)data[i] + 0x9E3779B97F4A7C15ULL + (rng.s << 6) +
                 (rng.s >> 2);
    }
    if (rng.s == 0U)
        rng.s = 1U;
    /* Several cases per harness input keeps the pattern/input mix wide
     * without needing a huge corpus. */
    for (rounds = 0U; rounds < 16U; rounds++) {
        if (!one_case(&rng, why, why_cap))
            return false;
    }
    return true;
}

int main(int argc, char **argv)
{
    int status = yew_fuzz_main(argc, argv, "fuzz_re_diff", NULL,
                               check_re_diff);

    /* DoD 6 wants the skip rate reported, not hidden: a campaign that
     * skips most of its cases is not the coverage it appears to be. */
    if (compared + skipped != 0U) {
        (void)printf("fuzz_re_diff: %llu compared, %llu skipped (%.1f%%)\n",
                     (unsigned long long)compared,
                     (unsigned long long)skipped,
                     100.0 * (double)skipped /
                         (double)(compared + skipped));
    }
    return status;
}
