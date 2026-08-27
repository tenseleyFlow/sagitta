/*
 * Sprint 21 §1/§2.  See searchui.h for what this layer owns.
 */
#include "search/searchui.h"

#include <string.h>

#include "edit/ed.h"
#include "edit/jumplist.h"
#include "edit/word.h"
#include "search/overlay.h"
#include "text/piece.h"
#include "ui/cmdline.h"
#include "ui/message.h"
#include "ui/win.h"
#include "unicode/coords.h"

enum { YEW_SEARCH_PREVIEW_CHUNK_BYTES = 1024U * 1024U };

typedef enum SearchPreviewResult {
    SEARCH_PREVIEW_FOUND,
    SEARCH_PREVIEW_PENDING,
    SEARCH_PREVIEW_MISS
} SearchPreviewResult;

void yew_search_opts_init(SearchOpts *o)
{
    if (o == NULL)
        return;
    o->ignorecase = false;
    o->smartcase = true;
    o->wrapscan = true;
    o->hlsearch = true;
}

bool yew_search_wants_icase(const YewRe *probe, const SearchOpts *o)
{
    bool ignorecase = o != NULL && o->ignorecase;
    bool smartcase = o == NULL || o->smartcase;

    /*
     * The two escapes win outright, and \C wins over \c when a pattern
     * somehow contains both: asking for exact case is the narrower
     * request, and a search that silently widens is the one that
     * surprises.
     */
    if (yew_re_forces_case(probe))
        return false;
    if (yew_re_forces_icase(probe))
        return true;
    if (!ignorecase)
        return false;
    if (!smartcase)
        return true;
    return !yew_re_has_upper_literal(probe);
}

YewRe *yew_search_compile(Arena *a, const char *pat, size_t len,
                          const SearchOpts *o, YewReErr *err)
{
    YewRe *probe;

    if (a == NULL || pat == NULL)
        return yew_re_compile(a, pat, len, 0U, err);
    /*
     * Two passes, because the answer to "should this be
     * case-insensitive" is inside the pattern.  The first pass is the
     * cheap one — it is the same work a failed compile would do anyway,
     * and Sprint 20's compiler is microseconds on a prompt-sized
     * pattern, which is what makes recompiling per keystroke viable at
     * all.
     */
    probe = yew_re_compile(a, pat, len, 0U, err);
    if (probe == NULL)
        return NULL;
    if (!yew_search_wants_icase(probe, o))
        return probe;
    return yew_re_compile(a, pat, len, YEW_RE_ICASE, err);
}

/* ---------------------------------------------------------------- */
/* Live search                                                      */
/* ---------------------------------------------------------------- */

void yew_search_state_init(SearchState *st)
{
    if (st == NULL)
        return;
    (void)memset(st, 0, sizeof(*st));
    arena_init(&st->arena);
}

void yew_search_state_free(SearchState *st)
{
    if (st == NULL)
        return;
    arena_free_all(&st->arena);
    (void)memset(st, 0, sizeof(*st));
}

static void search_capture_restore_point(Ed *ed, Win *w)
{
    const Cursor *c = yew_ed_cursor(ed);

    if (c != NULL) {
        ed->search.save_cur = *c;
        ed->search.origin = c->pos;
    }
    ed->search.save_top = w->vp.top;
    ed->search.save_top_sub = w->vp.top_sub;
}

void yew_search_open(Ed *ed, Win *w, bool reverse)
{
    if (ed == NULL || w == NULL)
        return;
    yew_search_preview_cancel(ed);
    search_capture_restore_point(ed, w);
    ed->search.reverse = reverse;
    ed->search.wrapped = false;
    ed->search.active = true;
    (void)memset(&ed->search.err, 0, sizeof(ed->search.err));
    yew_cmdline_open(ed, reverse ? YEW_PROMPT_SEARCH_B : YEW_PROMPT_SEARCH_F,
                     NULL);
}

/* Moves to `hit`, reporting whether the step wrapped. */
static void search_go(Ed *ed, Win *w, u64 hit)
{
    Cursor *c = yew_ed_cursor(ed);

    if (c != NULL) {
        c->pos = BYTEOFF(hit);
        c->goal_col = (GCol){0U};
    }
    yew_win_follow_cursor(w);
    yew_ed_damage_document(ed);
}

/*
 * One step of the engine's search, with wrap.  Returns false when there
 * is no match at all; `*wrapped` says whether the answer came from the
 * far end of the buffer.
 */
static bool search_find(const YewRe *re, const TextBuf *tb, u64 from,
                        bool backward, bool wrapscan, u64 *hit,
                        bool *wrapped)
{
    YewReInput in = yew_re_input_textbuf(tb);
    YewReMatch m;

    *wrapped = false;
    (void)memset(&m, 0, sizeof(m));
    if (backward) {
        if (from > 0U && yew_re_search_back(re, &in, BYTEOFF(from), &m)) {
            *hit = m.g[0].lo;
            return true;
        }
        if (!wrapscan)
            return false;
        if (!yew_re_search_back(re, &in, BYTEOFF(yew_textbuf_len(tb)), &m))
            return false;
        *wrapped = true;
        *hit = m.g[0].lo;
        return true;
    }
    if (yew_re_search(re, &in, BYTEOFF(from), &m)) {
        *hit = m.g[0].lo;
        return true;
    }
    if (!wrapscan)
        return false;
    if (!yew_re_search(re, &in, BYTEOFF(0U), &m))
        return false;
    *wrapped = true;
    *hit = m.g[0].lo;
    return true;
}

static void search_report_wrap(Ed *ed, bool backward)
{
    yew_msg(ed, YEW_MSG_INFO,
            backward ? "search hit TOP, continuing at BOTTOM"
                     : "search hit BOTTOM, continuing at TOP");
}

static void search_report_miss(Ed *ed, bool backward, bool wrapscan,
                               const char *pat, size_t patlen)
{
    if (!wrapscan) {
        yew_msg(ed, YEW_MSG_ERROR,
                backward ? "search hit TOP without match"
                         : "search hit BOTTOM without match");
        return;
    }
    yew_msg(ed, YEW_MSG_ERROR, "pattern not found: %.*s", (int)patlen, pat);
}

void yew_search_preview_cancel(Ed *ed)
{
    if (ed == NULL)
        return;
    if (ed->search.preview_timer != YEW_TIMER_NONE) {
        (void)yew_timer_cancel(&ed->timers, ed->search.preview_timer);
        ed->search.preview_timer = YEW_TIMER_NONE;
    }
    ed->search.preview_win_id = 0U;
    if (ed->search.preview_pending) {
        ed->search.preview_pending = false;
        ed->search.preview_remaining = 0U;
        yew_msg_clear(ed);
    }
}

void yew_search_preview_preempt(Ed *ed)
{
    if (ed == NULL)
        return;
    if (ed->search.preview_timer != YEW_TIMER_NONE) {
        (void)yew_timer_cancel(&ed->timers, ed->search.preview_timer);
        ed->search.preview_timer = YEW_TIMER_NONE;
    }
}

static SearchPreviewResult search_preview_slice(Ed *ed, Win *w);

static void search_preview_continue(Ed *ed, void *ctx)
{
    Win *w;
    SearchPreviewResult result;

    (void)ctx;
    if (ed == NULL)
        return;
    ed->search.preview_timer = YEW_TIMER_NONE;
    w = yew_ed_win_by_id(ed, ed->search.preview_win_id);
    if (w == NULL) {
        ed->search.preview_win_id = 0U;
        ed->search.preview_pending = false;
        ed->search.preview_remaining = 0U;
        yew_msg_clear(ed);
        return;
    }
    result = search_preview_slice(ed, w);
    if (result != SEARCH_PREVIEW_PENDING) {
        ed->search.preview_win_id = 0U;
        if (result == SEARCH_PREVIEW_MISS && ed->search.preview_pending) {
            ed->search.preview_pending = false;
            ed->search.preview_remaining = 0U;
            yew_msg_clear(ed);
        }
    }
}

static void search_preview_schedule(Ed *ed, Win *w)
{
    if (!ed->search.preview_pending) {
        ed->search.preview_pending = true;
        ed->search.preview_ui_seq++;
        yew_msg(ed, YEW_MSG_INFO, "searching %s%s",
                ed->search.preview_backward ? "backward" : "forward",
                (ed->search.preview_ui_seq & 1U) != 0U ? "." : "..");
    }
    ed->search.preview_win_id = w->id;
    ed->search.preview_timer = yew_timer_add(&ed->timers, ed->now_ms + 1,
                                              search_preview_continue, NULL);
}

static SearchPreviewResult search_preview_found(Ed *ed, Win *w, u64 hit)
{
    ed->search.wrapped = ed->search.preview_wrapped;
    search_go(ed, w, hit);
    if (ed->search.preview_wrapped)
        search_report_wrap(ed, ed->search.preview_backward);
    else if (ed->search.preview_remaining <= 1U)
        yew_msg_clear(ed);
    if (ed->search_opts.hlsearch)
        yew_overlay_refresh(ed, w, ed->search.re, ed->search.pat_gen,
                            YEW_OVERLAY_BUDGET_US);
    if (ed->search.preview_remaining > 1U) {
        const Cursor *c = yew_ed_cursor(ed);
        u64 len = yew_textbuf_len(w->buf->tb);
        u64 from;

        ed->search.preview_remaining--;
        if (c == NULL) {
            ed->search.preview_pending = false;
            ed->search.preview_remaining = 0U;
            return SEARCH_PREVIEW_MISS;
        }
        from = ed->search.preview_backward ? c->pos.v :
               yew_grapheme_next(w->buf->tb, c->pos).v;
        if (!ed->search.preview_backward && from <= c->pos.v)
            from = c->pos.v < len ? c->pos.v + 1U : len;
        ed->search.preview_at = from;
        ed->search.preview_stop = ed->search.preview_backward ? 0U : len;
        ed->search.preview_origin = from;
        ed->search.preview_wrapped = false;
        search_preview_schedule(ed, w);
        return SEARCH_PREVIEW_PENDING;
    }
    ed->search.preview_pending = false;
    ed->search.preview_remaining = 0U;
    return SEARCH_PREVIEW_FOUND;
}

static bool search_preview_wrap(Ed *ed, Win *w)
{
    u64 len = yew_textbuf_len(w->buf->tb);

    if (!ed->search.preview_wrapped && ed->search.preview_wrapscan &&
        ((!ed->search.preview_backward && ed->search.preview_origin > 0U) ||
         (ed->search.preview_backward &&
          ed->search.preview_origin < len))) {
        ed->search.preview_wrapped = true;
        ed->search.preview_at = ed->search.preview_backward ? len : 0U;
        ed->search.preview_stop = ed->search.preview_origin;
        return true;
    }
    ed->search.preview_pending = false;
    ed->search.preview_remaining = 0U;
    search_report_miss(ed, ed->search.preview_backward,
                       ed->search.preview_wrapscan, ed->search.pat,
                       ed->search.patlen);
    return false;
}

static SearchPreviewResult search_preview_slice(Ed *ed, Win *w)
{
    YewReInput in;
    YewReMatch match;
    u32 literal_bytes;
    u64 budget = YEW_SEARCH_PREVIEW_CHUNK_BYTES;
    u64 len;

    if (ed == NULL || w == NULL || w->buf == NULL || w->buf->tb == NULL ||
        ed->search.preview_gen != ed->search.pat_gen ||
        ed->search.preview_buf_gen != w->buf->tb->gen)
        return SEARCH_PREVIEW_MISS;
    literal_bytes = yew_re_whole_literal_bytes(ed->search.re);
    if (literal_bytes == 0U)
        return SEARCH_PREVIEW_MISS;
    len = yew_textbuf_len(w->buf->tb);
    in = yew_re_input_textbuf(w->buf->tb);

    for (;;) {
        (void)memset(&match, 0, sizeof(match));
        if (!ed->search.preview_backward) {
            u64 at = ed->search.preview_at;
            u64 span;
            u64 next;
            u64 scan_hi;
            u64 overlap = literal_bytes - 1U;

            if (at >= ed->search.preview_stop) {
                if (!search_preview_wrap(ed, w))
                    return SEARCH_PREVIEW_MISS;
                if (budget == 0U)
                    break;
                continue;
            }
            span = ed->search.preview_stop - at;
            if (span > budget)
                span = budget;
            next = at + span;
            scan_hi = overlap > len - next ? len : next + overlap;
            in.window.hi = scan_hi;
            if (yew_re_search(ed->search.re, &in, BYTEOFF(at), &match) &&
                match.g[0].lo < next &&
                match.g[0].lo < ed->search.preview_stop)
                return search_preview_found(ed, w, match.g[0].lo);
            ed->search.preview_at = next;
            budget -= span;
        } else {
            u64 before = ed->search.preview_at > len ? len :
                         ed->search.preview_at;
            u64 span;
            u64 lo;
            u64 overlap;

            if (before <= ed->search.preview_stop) {
                if (!search_preview_wrap(ed, w))
                    return SEARCH_PREVIEW_MISS;
                if (budget == 0U)
                    break;
                continue;
            }
            span = before - ed->search.preview_stop;
            if (span > budget)
                span = budget;
            lo = before - span;
            overlap = literal_bytes - 1U;
            in.window.lo = lo > ed->search.preview_stop + overlap ?
                           lo - overlap : ed->search.preview_stop;
            if (yew_re_search_back(ed->search.re, &in, BYTEOFF(before),
                                   &match) &&
                match.g[0].lo >= ed->search.preview_stop)
                return search_preview_found(ed, w, match.g[0].lo);
            ed->search.preview_at = lo;
            budget -= span;
        }
        if (budget == 0U)
            break;
    }
    search_preview_schedule(ed, w);
    return SEARCH_PREVIEW_PENDING;
}

static SearchPreviewResult search_preview_start(Ed *ed, Win *w, u64 from,
                                                bool backward,
                                                bool wrapscan, u32 count)
{
    u64 len = yew_textbuf_len(w->buf->tb);

    yew_search_preview_cancel(ed);
    if (from > len)
        from = len;
    ed->search.preview_at = from;
    ed->search.preview_stop = backward ? 0U : len;
    ed->search.preview_origin = from;
    ed->search.preview_buf_gen = w->buf->tb->gen;
    ed->search.preview_gen = ed->search.pat_gen;
    ed->search.preview_backward = backward;
    ed->search.preview_wrapped = false;
    ed->search.preview_wrapscan = wrapscan;
    ed->search.preview_remaining = count == 0U ? 1U : count;
    yew_msg_clear(ed);
    return search_preview_slice(ed, w);
}

void yew_search_input(Ed *ed, Win *w)
{
    Bytebuf text;
    YewReErr err;
    YewRe *re;
    u64 hit = 0U;
    bool wrapped = false;

    if (ed == NULL || w == NULL || !ed->search.active)
        return;
    yew_search_preview_cancel(ed);
    bytebuf_init(&text);
    yew_cmdline_text(ed, &text);
    if (text.len == 0U) {
        /* Empty is a UI state, not a regex program.  Handling it before
         * compilation matters because the engine intentionally rejects an
         * empty pattern while the prompt must still retire every artifact
         * left by the previous non-empty preview. */
        (void)memset(&ed->search.err, 0, sizeof(ed->search.err));
        ed->search.re = NULL;
        ed->search.pat = NULL;
        ed->search.patlen = 0U;
        ed->search.pat_gen++;
        yew_search_clear_highlight(ed, w);
        if (ed->search.count_timer != YEW_TIMER_NONE) {
            (void)yew_timer_cancel(&ed->timers, ed->search.count_timer);
            ed->search.count_timer = YEW_TIMER_NONE;
        }
        ed->search.count_win_id = 0U;
        w->overlay.count_total = 0U;
        w->overlay.count_capped = false;
        ed->footer_dirty = true;
        bytebuf_free(&text);
        return;
    }
    (void)memset(&err, 0, sizeof(err));
    re = yew_search_compile(&ed->search.arena, (const char *)text.data,
                            text.len, &ed->search_opts, &err);
    if (re == NULL) {
        /*
         * A half-typed pattern is the normal state of a prompt, not an
         * error condition: keep the LAST GOOD program highlighting and
         * put the caret under the offending construct.  Blanking the
         * screen because `[a-` is not yet `[a-z]` is the behaviour this
         * rule exists to prevent.
         */
        ed->search.err = err;
        yew_msg(ed, YEW_MSG_ERROR, "%s",
                err.msg != NULL ? err.msg : "bad pattern");
        bytebuf_free(&text);
        return;
    }
    (void)memset(&ed->search.err, 0, sizeof(ed->search.err));
    ed->search.re = re;
    ed->search.pat = arena_alloc(&ed->search.arena, text.len + 1U, 1U);
    (void)memcpy(ed->search.pat, text.data, text.len);
    ed->search.pat[text.len] = '\0';
    ed->search.patlen = text.len;
    ed->search.pat_gen++;
    if (yew_re_whole_literal_bytes(re) != 0U) {
        (void)search_preview_start(ed, w, ed->search.origin.v,
                                   ed->search.reverse,
                                   ed->search_opts.wrapscan, 1U);
    } else if (search_find(re, w->buf->tb, ed->search.origin.v,
                           ed->search.reverse, ed->search_opts.wrapscan,
                           &hit, &wrapped)) {
        ed->search.wrapped = wrapped;
        search_go(ed, w, hit);
        yew_msg_clear(ed);
    } else {
        /*
         * No match while typing keeps the previously previewed position
         * rather than throwing the user back: they are mid-word, and
         * the next keystroke may well match again.
         */
        yew_msg(ed, YEW_MSG_ERROR, "pattern not found: %.*s",
                (int)text.len, (const char *)text.data);
    }
    if (ed->search_opts.hlsearch)
        yew_overlay_refresh(ed, w, ed->search.re, ed->search.pat_gen,
                            YEW_OVERLAY_BUDGET_US);
    yew_search_schedule_count(ed, w);
    bytebuf_free(&text);
}

void yew_search_accept(Ed *ed, Win *w)
{
    bool pending;

    if (ed == NULL || w == NULL || !ed->search.active)
        return;
    pending = ed->search.preview_pending;
    if (!pending)
        yew_search_preview_cancel(ed);
    ed->search.active = false;
    /*
     * The prompt scheduled this count for a later idle tick.  Once Enter
     * commits the search, letting that timer survive makes its repaint land
     * during an unrelated subsequent key.  Finish the deliberately bounded
     * count in Enter's own frame instead, then retire the timer.
     */
    if (ed->search.count_timer != YEW_TIMER_NONE) {
        (void)yew_timer_cancel(&ed->timers, ed->search.count_timer);
        ed->search.count_timer = YEW_TIMER_NONE;
    }
    ed->search.count_win_id = 0U;
    if (ed->search.re != NULL && ed->search.patlen > 0U &&
        w->buf != NULL && w->buf->tb != NULL) {
        yew_overlay_count(&w->overlay, ed->search.re, w->buf->tb, 1000);
        ed->footer_dirty = true;
    }
    if (ed->search.pat != NULL && ed->search.patlen > 0U) {
        /* Register `/` is the pattern's home; `:s//` and `^R /` read it
         * from there rather than from this struct. */
        yew_reg_set_search(&ed->regs, (const u8 *)ed->search.pat,
                           ed->search.patlen);
        /* The jump is a jump: `origin` becomes a place to come back to. */
        yew_jump_push(w, ed->search.origin, ed->now_ms);
    }
    if (pending && ed->search.preview_timer == YEW_TIMER_NONE)
        search_preview_schedule(ed, w);
}

void yew_search_cancel(Ed *ed, Win *w)
{
    Cursor *c;

    if (ed == NULL || w == NULL || !ed->search.active)
        return;
    ed->search.active = false;
    c = yew_ed_cursor(ed);
    /*
     * Exact restore.  The cursor's goal column comes back too, so a
     * subsequent up/down aims where it did before the search; and the
     * viewport's top line comes back, so the window is not left
     * scrolled somewhere the user never chose.
     */
    if (c != NULL)
        *c = ed->search.save_cur;
    w->vp.top = ed->search.save_top;
    w->vp.top_sub = ed->search.save_top_sub;
    yew_search_clear_highlight(ed, w);
    /*
     * Cancel every trace of the preview, including the work that has
     * not happened yet.  A pending count timer would fire after the
     * restore and put a `[3/17]` badge on a statusline that had none
     * before the search — which is not "byte-identical", and is exactly
     * what the pty golden compares.
     */
    if (ed->search.count_timer != YEW_TIMER_NONE) {
        (void)yew_timer_cancel(&ed->timers, ed->search.count_timer);
        ed->search.count_timer = YEW_TIMER_NONE;
    }
    ed->search.count_win_id = 0U;
    yew_search_preview_cancel(ed);
    w->overlay.count_total = 0U;
    w->overlay.count_capped = false;
    ed->search.wrap_until_ms = 0;
    ed->search.wrapped = false;
    /* No search was accepted, so there is nothing for `n` to repeat.
     * Register `/` still holds the last ACCEPTED pattern, which is what
     * `n` falls back to. */
    ed->search.re = NULL;
    ed->search.pat = NULL;
    ed->search.patlen = 0U;
    yew_msg_clear(ed);
    yew_ed_damage_document(ed);
    ed->full_damage = true;
}

void yew_search_clear_highlight(Ed *ed, Win *w)
{
    if (ed == NULL || w == NULL)
        return;
    yew_overlay_refresh(ed, w, NULL, ed->search.pat_gen, 0);
}

bool yew_search_step(Ed *ed, Win *w, bool forward, u32 count)
{
    const RegVal *slash;
    YewRe *re;
    u32 i;
    u32 n = count == 0U ? 1U : count;
    bool any = false;

    if (ed == NULL || w == NULL || w->buf == NULL)
        return false;
    re = ed->search.re;
    if (re == NULL) {
        /* Nothing searched yet this session: fall back to register `/`,
         * which survives across prompts. */
        slash = yew_reg_get(&ed->regs, (u8)'/');
        if (slash == NULL || slash->bytes.len == 0U) {
            yew_msg(ed, YEW_MSG_ERROR, "no previous search pattern");
            return false;
        }
        re = yew_search_compile(&ed->search.arena,
                                (const char *)slash->bytes.data,
                                slash->bytes.len, &ed->search_opts, NULL);
        if (re == NULL)
            return false;
        ed->search.re = re;
        ed->search.pat = arena_alloc(&ed->search.arena,
                                     slash->bytes.len + 1U, 1U);
        (void)memcpy(ed->search.pat, slash->bytes.data, slash->bytes.len);
        ed->search.pat[slash->bytes.len] = '\0';
        ed->search.patlen = slash->bytes.len;
        ed->search.pat_gen++;
    }
    /*
     * `N` is the opposite of the SEARCH's direction, not an absolute
     * backwards: after `?foo`, `n` goes backwards and `N` forwards.
     */
    {
        bool backward = forward ? ed->search.reverse : !ed->search.reverse;

        if (yew_re_whole_literal_bytes(re) != 0U) {
            const Cursor *c = yew_ed_cursor(ed);
            u64 from;
            SearchPreviewResult result;

            if (c == NULL)
                return false;
            from = backward ? c->pos.v :
                   yew_grapheme_next(w->buf->tb, c->pos).v;
            if (!backward && from <= c->pos.v)
                from = c->pos.v + 1U;
            result = search_preview_start(ed, w, from, backward,
                                          ed->search_opts.wrapscan, n);
            if (result == SEARCH_PREVIEW_PENDING)
                return true;
            any = result == SEARCH_PREVIEW_FOUND;
        }
        for (i = 0U; i < n; i++) {
            const Cursor *c = yew_ed_cursor(ed);
            u64 from;
            u64 hit = 0U;
            bool wrapped = false;

            if (c == NULL || yew_re_whole_literal_bytes(re) != 0U)
                break;
            /*
             * Step off the current match before searching, or `n` finds
             * the match the cursor is already on and appears to do
             * nothing.
             */
            from = backward ? c->pos.v
                            : yew_grapheme_next(w->buf->tb, c->pos).v;
            if (!backward && from <= c->pos.v)
                from = c->pos.v + 1U;
            if (!search_find(re, w->buf->tb, from, backward,
                             ed->search_opts.wrapscan, &hit, &wrapped)) {
                if (!any)
                    search_report_miss(ed, backward,
                                       ed->search_opts.wrapscan,
                                       ed->search.pat,
                                       ed->search.patlen);
                break;
            }
            search_go(ed, w, hit);
            any = true;
            ed->search.wrapped = wrapped;
            if (wrapped)
                search_report_wrap(ed, backward);
        }
    }
    if (any && ed->search_opts.hlsearch)
        yew_overlay_refresh(ed, w, ed->search.re, ed->search.pat_gen,
                            YEW_OVERLAY_BUDGET_US);
    /* Moving between matches does not change the match total.  Recounting
     * here used to arm a whole-buffer idle pass after every n/N key and let
     * that work land in a later keypress frame. */
    /*
     * Exactly one match, and the cursor is on it: say so rather than
     * appearing to do nothing.  Silence here reads as a broken
     * keybinding.
     */
    if (any && w->overlay.count_total == 1U)
        yew_msg(ed, YEW_MSG_INFO, "1 match");
    return any;
}

/*
 * The word under the cursor, via Sprint 16's UAX #29 boundary oracle:
 * walk back to the boundary at or before the cursor, then forward to
 * the next one.  Using the oracle rather than a local "is this a letter"
 * test is what keeps `*` agreeing with `w`/`b` motion about where words
 * are — including for CJK, where the two deliberately differ from \w.
 */
static bool search_word_span(const TextBuf *tb, ByteOff pos, Span *out)
{
    u64 len = yew_textbuf_len(tb);
    u64 lo = pos.v;
    u64 hi = pos.v;

    if (len == 0U)
        return false;
    if (lo > len)
        lo = len;
    while (lo > 0U && !yew_word_boundary(tb, BYTEOFF(lo)))
        lo--;
    while (hi < len && !yew_word_boundary(tb, BYTEOFF(hi + 1U)))
        hi++;
    if (hi < len)
        hi++;
    out->lo = lo;
    out->hi = hi;
    return hi > lo;
}

/*
 * The idle pass.  It finishes any overlay scan the keystroke budget cut
 * short, then counts — in that order, because the highlight is what the
 * user is looking at and the number is what they glance at.
 */
static void search_idle(Ed *ed, void *ctx)
{
    Win *w;

    (void)ctx;
    if (ed == NULL)
        return;
    ed->search.count_timer = YEW_TIMER_NONE;
    w = yew_ed_win_by_id(ed, ed->search.count_win_id);
    ed->search.count_win_id = 0U;
    if (w == NULL || ed->search.re == NULL)
        return;
    if (!w->overlay.complete)
        yew_overlay_refresh(ed, w, ed->search.re, ed->search.pat_gen, 0);
    /* One millisecond, 16 KiB and 10 000 matches, whichever comes first.
     * The badge carries `+` whenever the result is partial.  The old 50 ms
     * slice could fire on the next input wake and was itself a visible
     * keypress stall. */
    yew_overlay_count(&w->overlay, ed->search.re, w->buf->tb, 1000);
    ed->footer_dirty = true;
}

void yew_search_schedule_count(Ed *ed, Win *w)
{
    if (ed == NULL || w == NULL || ed->search.re == NULL)
        return;
    if (ed->search.count_timer != YEW_TIMER_NONE)
        (void)yew_timer_cancel(&ed->timers, ed->search.count_timer);
    /* One tick out, so a burst of `n` presses schedules once rather
     * than counting between each. */
    ed->search.count_win_id = w->id;
    ed->search.count_timer = yew_timer_add(&ed->timers,
                                           ed->now_ms + 16, search_idle, NULL);
    if (ed->search.wrapped)
        ed->search.wrap_until_ms = ed->now_ms + 2000;
}

i64 yew_search_wrap_until(const Ed *ed)
{
    return ed == NULL ? 0 : ed->search.wrap_until_ms;
}

bool yew_search_word(Ed *ed, Win *w, bool forward)
{
    Bytebuf pat;
    Span word;
    const Cursor *c;
    u64 at;
    bool ok = false;

    if (ed == NULL || w == NULL || w->buf == NULL)
        return false;
    c = yew_ed_cursor(ed);
    if (c == NULL)
        return false;
    if (!search_word_span(w->buf->tb, c->pos, &word)) {
        yew_msg(ed, YEW_MSG_ERROR, "no word under the cursor");
        return false;
    }
    bytebuf_init(&pat);
    /*
     * `\b` + the QUOTED word + `\b`.  Quoting goes through the engine's
     * own yew_re_quote so there is exactly one implementation of
     * "escape this literal" — searching for `a.b` must not match `axb`.
     */
    bytebuf_append(&pat, "\\b", 2U);
    {
        Bytebuf raw;
        u64 i;

        bytebuf_init(&raw);
        for (i = word.lo; i < word.hi; i++) {
            u8 b = 0U;
            YewReInput in = yew_re_input_textbuf(w->buf->tb);

            if (yew_re_input_byte(&in, i, &b))
                bytebuf_push_u8(&raw, b);
        }
        yew_re_quote(&pat, raw.data, raw.len);
        bytebuf_free(&raw);
    }
    bytebuf_append(&pat, "\\b", 2U);

    ed->search.re = yew_search_compile(&ed->search.arena,
                                       (const char *)pat.data, pat.len,
                                       &ed->search_opts, NULL);
    if (ed->search.re != NULL) {
        ed->search.pat = arena_alloc(&ed->search.arena, pat.len + 1U, 1U);
        (void)memcpy(ed->search.pat, pat.data, pat.len);
        ed->search.pat[pat.len] = '\0';
        ed->search.patlen = pat.len;
        ed->search.pat_gen++;
        ed->search.reverse = !forward;
        yew_reg_set_search(&ed->regs, pat.data, pat.len);
        /* `*` is a jump, so where we stood is worth coming back to. */
        at = c->pos.v;
        yew_jump_push(w, BYTEOFF(at), ed->now_ms);
        ok = yew_search_step(ed, w, true, 1U);
    }
    bytebuf_free(&pat);
    return ok;
}
