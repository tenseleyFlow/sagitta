/*
 * Sprint 20 §6: the search entry points.
 *
 * Three engines, one answer.  The literal prefilter handles the common
 * case (a plain string) without entering the VM at all; the lazy DFA
 * decides match-or-not and bounds where a match can start; the Pike VM
 * produces every span and capture, and is the correctness reference the
 * other two are held to — test_re_engines_agree_on_spans checks the
 * dispatcher against a bare VM run on every pattern, input and start
 * offset in its corpus.
 */
#include "search/regex_internal.h"

#include <string.h>

#include "unicode/utf8.h"
#include "util/log.h"

bool yew_re_pike_run(const YewRe *re, const YewReInput *in, u64 start,
                     YewReMatch *out);

/* Reads one byte, chunk-aware.  Kept local so callers cannot accidentally
 * grow it into a "give me the whole buffer" helper (§1's law). */
static bool byte_at(const YewReInput *in, TextIter *it, u64 off, u8 *out)
{
    const u8 *chunk = NULL;
    u64 n = 0U;

    if (off >= in->window.hi)
        return false;
    if (in->tb == NULL) {
        if (off >= in->len)
            return false;
        *out = in->bytes[off];
        return true;
    }
    if (!yew_textiter_begin(it, in->tb, BYTEOFF(off)) ||
        !yew_textiter_chunk(it, in->tb, &chunk, &n) || n == 0U)
        return false;
    *out = chunk[0];
    return true;
}

/*
 * Public single-byte read, for callers that need a few bytes out of a
 * known-small span (Sprint 21 expands replacement templates this way).
 * It re-seeks per call, so it is O(log n) per byte — correct for a
 * match-sized span and quite wrong for a scan, which is why the engine
 * itself uses the cursor above instead.
 */
bool yew_re_input_byte(const YewReInput *in, u64 off, u8 *out)
{
    TextIter it;

    if (in == NULL || out == NULL)
        return false;
    return byte_at(in, &it, off, out);
}

/* Bytes a lead byte claims, or 0 when it is not a lead. */
static u64 lead_len(u8 b)
{
    if ((b & 0x80U) == 0U)
        return 1U;
    if ((b & 0xE0U) == 0xC0U)
        return 2U;
    if ((b & 0xF0U) == 0xE0U)
        return 3U;
    if ((b & 0xF8U) == 0xF0U)
        return 4U;
    return 0U;
}

static bool is_cp_start(const YewReInput *in, u64 off)
{
    TextIter it;
    u8 b;
    u64 back;

    if (off == 0U)
        return true;
    if (!byte_at(in, &it, off, &b))
        return true;
    /* A BMH candidate knows no encoding and can land mid-sequence;
     * omitting this check produces phantom matches inside multibyte
     * text (§7). */
    if ((b & 0xC0U) != 0x80U)
        return true;
    /*
     * It LOOKS like a continuation byte — but a stray 0x9F with no lead
     * in front of it is its own codepoint (a U+DC80-range escape), and
     * rejecting it would make a search for that byte fail on exactly the
     * binary files the escape policy exists to support.  So check
     * whether a real lead actually claims this offset.
     */
    for (back = 1U; back < YEW_UTF8_MAX && back <= off; back++) {
        u8 lead;
        u64 span;

        if (!byte_at(in, &it, off - back, &lead))
            break;
        if ((lead & 0xC0U) == 0x80U)
            continue; /* another continuation; keep walking back */
        span = lead_len(lead);
        /* A lead covering this offset means we are genuinely inside a
         * sequence; anything else means the byte stands alone. */
        return span == 0U || back >= span;
    }
    return true;
}

/* Advances one codepoint from `off`. */
static u64 next_cp_off(const YewReInput *in, u64 off)
{
    TextIter it;
    u8 b;
    u64 step;

    if (!byte_at(in, &it, off, &b))
        return off + 1U;
    if ((b & 0x80U) == 0U)
        step = 1U;
    else if ((b & 0xE0U) == 0xC0U)
        step = 2U;
    else if ((b & 0xF0U) == 0xE0U)
        step = 3U;
    else if ((b & 0xF8U) == 0xF0U)
        step = 4U;
    else
        step = 1U;
    return off + step;
}

/*
 * Scans the literal prefilter over the input one TextIter chunk at a
 * time, carrying n-1 bytes across chunk boundaries.
 *
 * Pitfall the carry exists for: without it a literal straddling two
 * pieces is missed — which happens only on buffers that have been
 * edited, only sometimes, and never in a test built from one contiguous
 * array.  The differential fuzzer builds its buffers by random insertion
 * precisely to hit this.
 */
static u64 prefilter_find(const YewRe *re, const YewReInput *in, u64 from)
{
    const ReLit *l = &re->lit;
    u64 at = from;

    if (l->kind == RE_LIT_NONE || l->n == 0U)
        return from;
    if (in->tb == NULL) {
        u64 hit;

        if (from >= in->window.hi)
            return UINT64_MAX;
        hit = yew_lit_find(l, in->bytes + from, in->window.hi - from);
        return hit == UINT64_MAX ? UINT64_MAX : from + hit;
    }
    while (at < in->window.hi) {
        TextIter it;
        const u8 *chunk = NULL;
        u64 n = 0U;
        u64 span;
        u64 hit;
        u8 carry[64];
        u64 carry_n;

        if (!yew_textiter_begin(&it, in->tb, BYTEOFF(at)) ||
            !yew_textiter_chunk(&it, in->tb, &chunk, &n) || n == 0U)
            return UINT64_MAX;
        span = n;
        if (at + span > in->window.hi)
            span = in->window.hi - at;
        hit = yew_lit_find(l, chunk, span);
        if (hit != UINT64_MAX)
            return at + hit;
        /* Straddle window: the last n-1 bytes of this chunk plus the
         * first n-1 of the next. */
        carry_n = 0U;
        if (l->n > 1U && span >= 1U) {
            u64 back = l->n - 1U < span ? (u64)(l->n - 1U) : span;
            u64 base = at + span - back;
            u64 want = (u64)(l->n - 1U) * 2U;
            u64 k;

            if (want > sizeof(carry))
                want = sizeof(carry);
            for (k = 0U; k < want && base + k < in->window.hi; k++) {
                TextIter probe;
                u8 b;

                if (!byte_at(in, &probe, base + k, &b))
                    break;
                carry[carry_n++] = b;
            }
            if (carry_n >= l->n) {
                u64 chit = yew_lit_find(l, carry, carry_n);

                if (chit != UINT64_MAX)
                    return base + chit;
            }
        }
        at += span;
    }
    return UINT64_MAX;
}

bool yew_re_match_at(const YewRe *re, const YewReInput *in, ByteOff at,
                     YewReMatch *out)
{
    if (re == NULL || in == NULL)
        return false;
    return yew_re_pike_run(re, in, at.v, out);
}

bool yew_re_match_at_ws(YewReWorkspace *workspace, const YewRe *re,
                        const YewReInput *in, ByteOff at, YewReMatch *out)
{
    if (workspace == NULL || re == NULL || in == NULL)
        return false;
    return yew_re_pike_run_ws(workspace, re, in, at.v, true, out);
}

bool yew_re_search(const YewRe *re, const YewReInput *in, ByteOff from,
                   YewReMatch *out)
{
    u64 at;

    if (re == NULL || in == NULL)
        return false;
    at = from.v < in->window.lo ? in->window.lo : from.v;

    /*
     * Whole-pattern literal: the VM is never entered.  This is the
     * common case in an editor — most searches are a plain string — and
     * it runs at memchr/BMH speed.
     */
    if (re->lit.kind == RE_LIT_WHOLE) {
        while (at <= in->window.hi) {
            u64 hit = prefilter_find(re, in, at);

            if (hit == UINT64_MAX || hit + re->lit.n > in->window.hi)
                return false;
            if (!is_cp_start(in, hit)) {
                at = hit + 1U;
                continue;
            }
            if (out != NULL) {
                (void)memset(out, 0, sizeof(*out));
                out->ngroups = 1U;
                out->g[0].lo = hit;
                out->g[0].hi = hit + re->lit.n;
            }
            return true;
        }
        return false;
    }

    /*
     * Otherwise: use the prefilter ONLY to skip the dead zone before the
     * first candidate, then hand the rest to one unanchored VM pass.
     *
     * Re-running an anchored match at every prefilter hit looks like an
     * optimization and is a trap: for `a(a*)*b` the extracted literal is
     * the single byte "a", so in an all-'a' buffer every position is a
     * hit and the "fast path" is O(n^2).  That is what made the
     * pathological gate hang rather than merely run slowly.  The VM
     * seeds a start thread at each position inside its single pass, so
     * the scan stays O(n*m) no matter how dense the candidates are.
     */
    if (re->lit.kind != RE_LIT_NONE) {
        u64 first = prefilter_find(re, in, at);

        if (first == UINT64_MAX || first > in->window.hi)
            return false;
        at = first;
    }
    /*
     * §6c: span recovery.  A forward DFA reports where a match ENDS, so
     * on its own it cannot produce a span — and the sprint's suggested
     * composition (reverse-scan from that end to find the start) does
     * not survive our semantics.  Leftmost beats earliest-end: on
     * "abcd", /bc|abcd/ has its earliest end at 3 (from `bc` at 1..3),
     * but the answer is 0..4, which no scan seeded at 3 can reach.
     * test_re_match pins both orders of that pattern.
     *
     * So the DFA locates a REGION and the VM still produces the answer,
     * which is the shape §6's own with-groups dispatcher row describes.
     * Two uses, both sound:
     *
     *   NO   nothing matches ahead, so return without entering the VM
     *        at all.  This is the whole-file miss, and it is the case
     *        that used to cost a full VM pass over the buffer.
     *   YES  the earliest end `e` bounds the leftmost start: that match
     *        [s*, E*) has E* >= e and consumes at most max_len
     *        codepoints, so s* >= e - 4*max_len.  Seed the VM there and
     *        it still sees s* — provided max_len is bounded, which is
     *        why an unbounded pattern (`\w+`) keeps the plain scan.
     */
    {
        u64 end = 0U;
        int verdict = yew_re_dfa_find_end(re, in, at, &end);

        if (verdict == YEW_DFA_NO)
            return false;
        if (verdict == YEW_DFA_YES && is_cp_start(in, at) &&
            re->max_len != UINT32_MAX &&
            re->max_len <= YEW_RE_SPAN_WINDOW / 4U) {
            u64 span = (u64)re->max_len * 4U;

            if (end > at + span) {
                u64 to = end - span;
                u64 floor = to > (u64)YEW_UTF8_MAX ?
                            to - (u64)YEW_UTF8_MAX : in->window.lo;

                /*
                 * Start and finish on codepoint boundaries, or not at
                 * all — hence the is_cp_start guard above as well.
                 *
                 * Both engines derive `\b` and `^` context by decoding
                 * backwards from wherever they start, and inside a
                 * sequence that decode is ambiguous: over "漢字",
                 * offset 2 reads as the tail of 漢 when you walk back
                 * to it, and as a lone invalid byte when you step onto
                 * it from 1.  A scan that begins mid-sequence therefore
                 * sees different context for the rest of the run than
                 * any aligned scan does, and /\B/ genuinely matches at
                 * offset 6 from 5 but not from 6.  Neither answer is
                 * wrong; they are answers to different questions, so
                 * the skip declines to restate the question.  Walking
                 * back to a boundary only widens the window, which is
                 * always sound.
                 */
                while (to > floor && !is_cp_start(in, to))
                    to--;
                if (to > at && is_cp_start(in, to))
                    at = to;
            }
        }
        /* GIVE_UP: the cache thrashed.  `at` is unchanged, so the scan
         * below is exactly what it would have been. */
    }
    /*
     * Start the scan at `at`, but hand the VM the ORIGINAL window.
     * window.lo is what `^`, `\A` and `\b` are measured against, and
     * narrowing it to the skip point tells the VM that text begins
     * where we merely started looking.
     *
     * This line used to narrow it and was still correct, because the
     * only thing that moved `at` was the prefilter and collect_literal
     * stops at a leading assertion — so an assertion-sensitive pattern
     * had no literal and never skipped.  The DFA skip above moves `at`
     * for every pattern, which makes the narrowing reachable; /\B/ over
     * "漢字" flipped to a false match the moment it was.
     */
    return yew_re_pike_run_ex(re, in, at, false, out);
}

bool yew_re_test(const YewRe *re, const YewReInput *in, ByteOff from)
{
    int verdict;

    if (re == NULL || in == NULL)
        return false;
    /* §6's dispatcher row: prefilter, then the lazy DFA.  A
     * whole-pattern literal is answered by the prefilter alone. */
    if (re->lit.kind == RE_LIT_WHOLE)
        return yew_re_search(re, in, from, NULL);
    verdict = yew_re_dfa_test(re, in, from.v);
    if (verdict != YEW_DFA_GIVE_UP)
        return verdict == YEW_DFA_YES;
    /* The cache thrashed: finish on the VM rather than rebuild every
     * state per character, which is slower than never having cached. */
    return yew_re_search(re, in, from, NULL);
}

u32 yew_re_whole_literal_bytes(const YewRe *re)
{
    return re != NULL && re->lit.kind == RE_LIT_WHOLE ? re->lit.n : 0U;
}

/*
 * Backward search: scan forward inside a bounded window that walks
 * backwards, keeping the last match.  Each window is one yew_re_search,
 * so the DFA skip-ahead above applies inside it too.
 *
 * This is the shape a reverse engine would have replaced; see the rprog
 * comment in regex_internal.h for why it does not.
 */
bool yew_re_search_back(const YewRe *re, const YewReInput *in,
                        ByteOff before, YewReMatch *out)
{
    enum { WINDOW = 256U * 1024U };
    u64 hi = before.v > in->window.hi ? in->window.hi : before.v;
    u64 lo;

    if (re == NULL || in == NULL || hi <= in->window.lo)
        return false;
    for (;;) {
        YewReInput sub = *in;
        YewReMatch best;
        YewReMatch cur;
        bool found = false;
        u64 at;

        u64 scan_lo;

        lo = hi > in->window.lo + WINDOW ? hi - WINDOW : in->window.lo;
        scan_lo = lo;
        if (re->lit.kind == RE_LIT_WHOLE && re->lit.n > 1U) {
            u64 overlap = (u64)re->lit.n - 1U;

            scan_lo = lo > in->window.lo + overlap ? lo - overlap :
                      in->window.lo;
        }
        sub.window.lo = scan_lo;
        /*
         * A whole literal has no anchor semantics to preserve outside this
         * backward-search window.  Keeping the caller's far `window.hi`
         * made the final failed probe after the last local match scan all
         * the way to the end of a huge buffer before we discarded it for
         * starting at or beyond `hi`.  Bound literals to the window we are
         * actually asking about; regex programs retain the original high
         * edge because \z and related context are measured against it.
         */
        if (re->lit.kind == RE_LIT_WHOLE) {
            u64 overlap = re->lit.n > 1U ? (u64)re->lit.n - 1U : 0U;

            sub.window.hi = overlap > in->window.hi - hi ?
                            in->window.hi : hi + overlap;
        } else {
            sub.window.hi = in->window.hi;
        }
        at = scan_lo;
        (void)memset(&best, 0, sizeof(best));
        while (at < hi) {
            if (!yew_re_search(re, &sub, BYTEOFF(at), &cur))
                break;
            if (cur.g[0].lo >= hi)
                break;
            best = cur;
            found = true;
            at = cur.g[0].hi > cur.g[0].lo ? cur.g[0].hi :
                 next_cp_off(in, cur.g[0].lo);
        }
        if (found) {
            if (out != NULL)
                *out = best;
            return true;
        }
        if (lo == in->window.lo)
            return false;
        hi = lo;
    }
}
