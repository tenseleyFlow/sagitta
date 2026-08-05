/*
 * Sprint 21 §3.  See overlay.h for why the scan is bounded twice.
 */
#define _POSIX_C_SOURCE 200809L

#include "search/overlay.h"

#include <string.h>
#include <time.h>

#include "edit/ed.h"
#include "text/piece.h"
#include "ui/win.h"
#include "unicode/coords.h"
#include "util/log.h"

void sag_overlay_init(MatchOverlay *ov)
{
    if (ov == NULL)
        return;
    (void)memset(ov, 0, sizeof(*ov));
    ov->cur_index = -1;
}

void sag_overlay_free(MatchOverlay *ov)
{
    if (ov == NULL)
        return;
    SpanVec_free(&ov->spans);
    (void)memset(ov, 0, sizeof(*ov));
    ov->cur_index = -1;
}

void sag_overlay_invalidate(MatchOverlay *ov)
{
    if (ov == NULL)
        return;
    ov->spans.len = 0U;
    ov->scanned.lo = 0U;
    ov->scanned.hi = 0U;
    ov->cur_index = -1;
    ov->complete = false;
}

static i64 now_us(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return 0;
    return (i64)ts.tv_sec * INT64_C(1000000) + ts.tv_nsec / 1000;
}

/*
 * The scan window: the visible lines, extended by the look-ahead in
 * each direction.  Two viewport heights is the useful amount — it makes
 * one page of scrolling free — and 64 KiB is the ceiling that keeps a
 * file of very long lines from turning "two screens" into megabytes.
 */
/*
 * Two ranges, and the difference between them is the whole point of the
 * look-ahead:
 *
 *   vis   the visible lines — what must be covered to draw correctly
 *   want  vis plus the look-ahead — what a scan actually covers
 *
 * Reuse is judged against `vis`, never against `want`.  Judging it
 * against `want` was the first version of this and it silently
 * defeated the feature: `want` grows by exactly as much as the viewport
 * moves, so every scroll fell outside the previous `want` and rescanned,
 * which is the behaviour the look-ahead exists to prevent.
 */
static Span visible_window(const Win *w, const TextBuf *tb)
{
    u64 len = sag_textbuf_len(tb);
    u64 nlines = sag_textbuf_line_count(tb);
    LineNo top = sag_win_view_top(w);
    u64 height = w->rect.h == 0U ? 1U : (u64)w->rect.h;
    u64 last = top.v + height;
    Span vis;

    if (nlines == 0U) {
        vis.lo = 0U;
        vis.hi = 0U;
        return vis;
    }
    if (last >= nlines)
        last = nlines - 1U;
    vis.lo = sag_textbuf_line_start(tb, top).v;
    vis.hi = last + 1U < nlines
             ? sag_textbuf_line_start(tb, LINENO(last + 1U)).v
             : len;
    return vis;
}

static Span scan_window(const Win *w, const TextBuf *tb)
{
    u64 len = sag_textbuf_len(tb);
    u64 nlines = sag_textbuf_line_count(tb);
    LineNo top = sag_win_view_top(w);
    u64 height = w->rect.h == 0U ? 1U : (u64)w->rect.h;
    u64 last = top.v + height;
    Span vis;
    u64 look;
    Span out;

    if (nlines == 0U) {
        out.lo = 0U;
        out.hi = 0U;
        return out;
    }
    if (last >= nlines)
        last = nlines - 1U;
    vis.lo = sag_textbuf_line_start(tb, top).v;
    vis.hi = last + 1U < nlines ? sag_textbuf_line_start(tb,
                                                         LINENO(last + 1U)).v
                                : len;
    look = vis.hi > vis.lo ? (vis.hi - vis.lo) * 2U : 0U;
    if (look > (u64)SAG_SEARCH_LOOKAHEAD_MAX)
        look = SAG_SEARCH_LOOKAHEAD_MAX;
    out.lo = vis.lo > look ? vis.lo - look : 0U;
    out.hi = vis.hi + look < len ? vis.hi + look : len;
    return out;
}

/* Damages every line touched by `span`. */
static void damage_span(Ed *ed, const TextBuf *tb, Span span)
{
    LineNo lo = sag_textbuf_line_of(tb, BYTEOFF(span.lo));
    LineNo hi = sag_textbuf_line_of(tb, BYTEOFF(span.hi > span.lo
                                                ? span.hi - 1U
                                                : span.lo));
    u64 line;

    for (line = lo.v; line <= hi.v; line++)
        sag_ed_damage_line(ed, LINENO(line), false);
}

/*
 * Damage by DIFFING the old and new span lists, not by damaging the
 * viewport.  Adding one character to a search pattern usually changes
 * the highlight on a handful of lines; repainting two hundred because
 * one of them changed is the difference between a search prompt that
 * feels instant and one that flickers.
 */
static void damage_diff(Ed *ed, const TextBuf *tb, const SpanVec *before,
                        const SpanVec *after)
{
    size_t i = 0U;
    size_t j = 0U;

    while (i < before->len || j < after->len) {
        if (i < before->len && j < after->len &&
            before->data[i].lo == after->data[j].lo &&
            before->data[i].hi == after->data[j].hi) {
            i++;
            j++;
            continue;
        }
        if (j >= after->len ||
            (i < before->len && before->data[i].lo < after->data[j].lo)) {
            damage_span(ed, tb, before->data[i]);
            i++;
        } else {
            damage_span(ed, tb, after->data[j]);
            j++;
        }
    }
}

void sag_overlay_refresh(Ed *ed, Win *w, const SagRe *re, u32 pat_gen,
                         i64 budget_us)
{
    MatchOverlay *ov;
    const TextBuf *tb;
    SagReInput in;
    Span want;
    SpanVec before;
    i64 deadline;
    u64 scanned_bytes = 0U;
    u64 at;

    if (ed == NULL || w == NULL || w->buf == NULL || w->buf->tb == NULL)
        return;
    ov = &w->overlay;
    tb = w->buf->tb;
    if (re == NULL) {
        if (ov->spans.len != 0U) {
            SpanVec empty = {0};

            damage_diff(ed, tb, &ov->spans, &empty);
            sag_overlay_invalidate(ov);
        }
        return;
    }
    /*
     * Invalidation.  A recompiled pattern or an edited buffer makes
     * every span suspect; a scroll does not, which is the whole point
     * of keeping `scanned` separate from the viewport.
     */
    if (ov->pat_gen != pat_gen || ov->buf_gen != tb->gen) {
        ov->spans.len = 0U;
        ov->scanned.lo = 0U;
        ov->scanned.hi = 0U;
        ov->complete = false;
        ov->pat_gen = pat_gen;
        ov->buf_gen = tb->gen;
    }
    want = scan_window(w, tb);
    {
        Span vis = visible_window(w, tb);

        if (ov->complete && ov->scanned.lo <= vis.lo &&
            ov->scanned.hi >= vis.hi)
            return; /* the scroll stayed inside what we already know */
    }

    (void)memset(&before, 0, sizeof(before));
    SpanVec_reserve(&before, ov->spans.len);
    if (ov->spans.len > 0U)
        (void)memcpy(before.data, ov->spans.data,
                     ov->spans.len * sizeof(*before.data));
    before.len = ov->spans.len;

    ov->spans.len = 0U;
    in = sag_re_input_textbuf(tb);
    deadline = budget_us > 0 ? now_us() + budget_us : 0;
    at = want.lo;
    /*
     * Start at a line boundary: a scan that begins mid-line would judge
     * `^` against the window edge rather than the line, the same trap
     * the Sprint 20 dispatcher hit.  sag_re_search keeps the window at
     * the whole buffer, so anchors stay correct; only the loop is
     * bounded.
     */
    at = sag_textbuf_line_start(tb, sag_textbuf_line_of(tb,
                                                        BYTEOFF(at))).v;
    ov->scanned.lo = at;
    ov->complete = true;
    /*
     * Bound the ENGINE's window, not just this loop.
     *
     * The first version left the window at the whole buffer and dropped
     * matches past want.hi after the fact — which meant every keystroke
     * that had no match on screen scanned to the end of the file to
     * find one it would then throw away.  On a 1 GB buffer that is the
     * exact whole-file scan §3 forbids, and the latency gate measured
     * it at 73 ms against a 5 ms budget.
     *
     * Narrowing window.hi is safe in a way narrowing window.lo is not:
     * `lo` is what `^` and `\b` look BACKWARD to, and moving it lies
     * about text that exists.  Moving `hi` can only make the engine
     * believe text ends early, which affects `$` and `\z` at exactly
     * that offset — handled below.
     */
    if (want.hi < sag_textbuf_len(tb))
        in.window.hi = want.hi;
    for (;;) {
        SagReMatch m;

        (void)memset(&m, 0, sizeof(m));
        if (at > sag_textbuf_len(tb))
            break;
        if (!sag_re_search(re, &in, BYTEOFF(at), &m))
            break;
        if (m.g[0].lo >= want.hi)
            break;
        /*
         * A match ending exactly at the narrowed edge may only match
         * because the engine thinks the buffer ends there — `foo\z`
         * would fire on any window boundary.  Drop it: the edge sits
         * inside the look-ahead, so it is not on screen, and a scroll
         * that brings it into view rescans with a wider window and
         * finds it honestly.
         */
        if (m.g[0].hi == want.hi && want.hi < sag_textbuf_len(tb))
            break;
        SpanVec_push(&ov->spans, m.g[0]);
        scanned_bytes = m.g[0].hi > want.lo ? m.g[0].hi - want.lo : 0U;
        if (m.g[0].hi == m.g[0].lo) {
            ByteOff next = sag_grapheme_next(tb, BYTEOFF(m.g[0].hi));

            if (next.v <= m.g[0].hi)
                break;
            at = next.v;
        } else {
            at = m.g[0].hi;
        }
        /*
         * Budget check between matches, not inside them: cutting a scan
         * mid-match would record a span the pattern did not produce.
         * An exhausted budget keeps what was found, marks the overlay
         * incomplete, and lets the idle timer finish the rest.
         */
        if (scanned_bytes >= (u64)SAG_OVERLAY_BUDGET_BYTES ||
            (deadline != 0 && now_us() >= deadline)) {
            ov->complete = false;
            break;
        }
    }
    ov->scanned.hi = ov->complete ? want.hi : at;
    damage_diff(ed, tb, &before, &ov->spans);
    SpanVec_free(&before);

    /* Which match the cursor is standing on, for the `search_current`
     * style and the `[3/17]` numerator. */
    ov->cur_index = -1;
    {
        const Cursor *c = sag_ed_cursor(ed);
        size_t i;

        if (c != NULL) {
            for (i = 0U; i < ov->spans.len; i++) {
                if (c->pos.v >= ov->spans.data[i].lo &&
                    c->pos.v < ov->spans.data[i].hi) {
                    ov->cur_index = (i32)i;
                    break;
                }
                /* A zero-width match sits exactly at the cursor. */
                if (ov->spans.data[i].lo == ov->spans.data[i].hi &&
                    c->pos.v == ov->spans.data[i].lo) {
                    ov->cur_index = (i32)i;
                    break;
                }
            }
        }
    }
}

void sag_overlay_count(MatchOverlay *ov, const SagRe *re, const TextBuf *tb,
                       i64 budget_us)
{
    SagReInput in;
    i64 deadline;
    u64 at = 0U;
    u32 n = 0U;

    if (ov == NULL || re == NULL || tb == NULL)
        return;
    ov->count_total = 0U;
    ov->count_capped = false;
    in = sag_re_input_textbuf(tb);
    deadline = budget_us > 0 ? now_us() + budget_us : 0;
    for (;;) {
        SagReMatch m;

        (void)memset(&m, 0, sizeof(m));
        if (at > sag_textbuf_len(tb))
            break;
        if (!sag_re_search(re, &in, BYTEOFF(at), &m))
            break;
        n++;
        /*
         * The cap is the point.  An unbounded counter is exactly the
         * feature that makes a big-file editor feel broken: the number
         * nobody reads past "lots" costs a full scan to produce.
         */
        if (n >= (u32)SAG_SEARCH_COUNT_MAX) {
            ov->count_capped = true;
            break;
        }
        if ((n & 0xFFU) == 0U && deadline != 0 && now_us() >= deadline) {
            ov->count_capped = true;
            break;
        }
        if (m.g[0].hi == m.g[0].lo) {
            ByteOff next = sag_grapheme_next(tb, BYTEOFF(m.g[0].hi));

            if (next.v <= m.g[0].hi)
                break;
            at = next.v;
        } else {
            at = m.g[0].hi;
        }
    }
    ov->count_total = n;
}
