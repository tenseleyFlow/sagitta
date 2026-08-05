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

void sag_search_opts_init(SearchOpts *o)
{
    if (o == NULL)
        return;
    o->ignorecase = false;
    o->smartcase = true;
    o->wrapscan = true;
    o->hlsearch = true;
}

bool sag_search_wants_icase(const SagRe *probe, const SearchOpts *o)
{
    bool ignorecase = o != NULL && o->ignorecase;
    bool smartcase = o == NULL || o->smartcase;

    /*
     * The two escapes win outright, and \C wins over \c when a pattern
     * somehow contains both: asking for exact case is the narrower
     * request, and a search that silently widens is the one that
     * surprises.
     */
    if (sag_re_forces_case(probe))
        return false;
    if (sag_re_forces_icase(probe))
        return true;
    if (!ignorecase)
        return false;
    if (!smartcase)
        return true;
    return !sag_re_has_upper_literal(probe);
}

SagRe *sag_search_compile(Arena *a, const char *pat, size_t len,
                          const SearchOpts *o, SagReErr *err)
{
    SagRe *probe;

    if (a == NULL || pat == NULL)
        return sag_re_compile(a, pat, len, 0U, err);
    /*
     * Two passes, because the answer to "should this be
     * case-insensitive" is inside the pattern.  The first pass is the
     * cheap one — it is the same work a failed compile would do anyway,
     * and Sprint 20's compiler is microseconds on a prompt-sized
     * pattern, which is what makes recompiling per keystroke viable at
     * all.
     */
    probe = sag_re_compile(a, pat, len, 0U, err);
    if (probe == NULL)
        return NULL;
    if (!sag_search_wants_icase(probe, o))
        return probe;
    return sag_re_compile(a, pat, len, SAG_RE_ICASE, err);
}

/* ---------------------------------------------------------------- */
/* Live search                                                      */
/* ---------------------------------------------------------------- */

void sag_search_state_init(SearchState *st)
{
    if (st == NULL)
        return;
    (void)memset(st, 0, sizeof(*st));
    arena_init(&st->arena);
}

void sag_search_state_free(SearchState *st)
{
    if (st == NULL)
        return;
    arena_free_all(&st->arena);
    (void)memset(st, 0, sizeof(*st));
}

static void search_capture_restore_point(Ed *ed, Win *w)
{
    const Cursor *c = sag_ed_cursor(ed);

    if (c != NULL) {
        ed->search.save_cur = *c;
        ed->search.origin = c->pos;
    }
    ed->search.save_top = w->vp.top;
    ed->search.save_top_sub = w->vp.top_sub;
}

void sag_search_open(Ed *ed, Win *w, bool reverse)
{
    if (ed == NULL || w == NULL)
        return;
    search_capture_restore_point(ed, w);
    ed->search.reverse = reverse;
    ed->search.wrapped = false;
    ed->search.active = true;
    (void)memset(&ed->search.err, 0, sizeof(ed->search.err));
    sag_cmdline_open(ed, reverse ? SAG_PROMPT_SEARCH_B : SAG_PROMPT_SEARCH_F,
                     NULL);
}

/* Moves to `hit`, reporting whether the step wrapped. */
static void search_go(Ed *ed, Win *w, u64 hit)
{
    Cursor *c = sag_ed_cursor(ed);

    if (c != NULL) {
        c->pos = BYTEOFF(hit);
        c->goal_col = (GCol){0U};
    }
    sag_win_follow_cursor(w);
    sag_ed_damage_document(ed);
}

/*
 * One step of the engine's search, with wrap.  Returns false when there
 * is no match at all; `*wrapped` says whether the answer came from the
 * far end of the buffer.
 */
static bool search_find(const SagRe *re, const TextBuf *tb, u64 from,
                        bool backward, bool wrapscan, u64 *hit,
                        bool *wrapped)
{
    SagReInput in = sag_re_input_textbuf(tb);
    SagReMatch m;

    *wrapped = false;
    (void)memset(&m, 0, sizeof(m));
    if (backward) {
        if (from > 0U && sag_re_search_back(re, &in, BYTEOFF(from), &m)) {
            *hit = m.g[0].lo;
            return true;
        }
        if (!wrapscan)
            return false;
        if (!sag_re_search_back(re, &in, BYTEOFF(sag_textbuf_len(tb)), &m))
            return false;
        *wrapped = true;
        *hit = m.g[0].lo;
        return true;
    }
    if (sag_re_search(re, &in, BYTEOFF(from), &m)) {
        *hit = m.g[0].lo;
        return true;
    }
    if (!wrapscan)
        return false;
    if (!sag_re_search(re, &in, BYTEOFF(0U), &m))
        return false;
    *wrapped = true;
    *hit = m.g[0].lo;
    return true;
}

static void search_report_wrap(Ed *ed, bool backward)
{
    sag_msg(ed, SAG_MSG_INFO,
            backward ? "search hit TOP, continuing at BOTTOM"
                     : "search hit BOTTOM, continuing at TOP");
}

static void search_report_miss(Ed *ed, bool backward, bool wrapscan,
                               const char *pat, size_t patlen)
{
    if (!wrapscan) {
        sag_msg(ed, SAG_MSG_ERROR,
                backward ? "search hit TOP without match"
                         : "search hit BOTTOM without match");
        return;
    }
    sag_msg(ed, SAG_MSG_ERROR, "pattern not found: %.*s", (int)patlen, pat);
}

void sag_search_input(Ed *ed, Win *w)
{
    Bytebuf text;
    SagReErr err;
    SagRe *re;
    u64 hit = 0U;
    bool wrapped = false;

    if (ed == NULL || w == NULL || !ed->search.active)
        return;
    bytebuf_init(&text);
    sag_cmdline_text(ed, &text);
    (void)memset(&err, 0, sizeof(err));
    re = sag_search_compile(&ed->search.arena, (const char *)text.data,
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
        sag_msg(ed, SAG_MSG_ERROR, "%s",
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
    if (text.len == 0U) {
        /* An empty prompt previews nothing and highlights nothing, but
         * the restore point stays live. */
        sag_search_clear_highlight(ed, w);
        bytebuf_free(&text);
        return;
    }
    if (search_find(re, w->buf->tb, ed->search.origin.v,
                    ed->search.reverse, ed->search_opts.wrapscan, &hit,
                    &wrapped)) {
        ed->search.wrapped = wrapped;
        search_go(ed, w, hit);
        sag_msg_clear(ed);
    } else {
        /*
         * No match while typing keeps the previously previewed position
         * rather than throwing the user back: they are mid-word, and
         * the next keystroke may well match again.
         */
        sag_msg(ed, SAG_MSG_ERROR, "pattern not found: %.*s",
                (int)text.len, (const char *)text.data);
    }
    if (ed->search_opts.hlsearch)
        sag_overlay_refresh(ed, w, ed->search.re, ed->search.pat_gen,
                            SAG_OVERLAY_BUDGET_US);
    bytebuf_free(&text);
}

void sag_search_accept(Ed *ed, Win *w)
{
    if (ed == NULL || w == NULL || !ed->search.active)
        return;
    ed->search.active = false;
    if (ed->search.pat != NULL && ed->search.patlen > 0U) {
        /* Register `/` is the pattern's home; `:s//` and `^R /` read it
         * from there rather than from this struct. */
        sag_reg_set_search(&ed->regs, (const u8 *)ed->search.pat,
                           ed->search.patlen);
        /* The jump is a jump: `origin` becomes a place to come back to. */
        sag_jump_push(w, ed->search.origin, ed->now_ms);
    }
}

void sag_search_cancel(Ed *ed, Win *w)
{
    Cursor *c;

    if (ed == NULL || w == NULL || !ed->search.active)
        return;
    ed->search.active = false;
    c = sag_ed_cursor(ed);
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
    sag_search_clear_highlight(ed, w);
    sag_msg_clear(ed);
    sag_ed_damage_document(ed);
    ed->full_damage = true;
}

void sag_search_clear_highlight(Ed *ed, Win *w)
{
    if (ed == NULL || w == NULL)
        return;
    sag_overlay_refresh(ed, w, NULL, ed->search.pat_gen, 0);
}

bool sag_search_step(Ed *ed, Win *w, bool forward, u32 count)
{
    const RegVal *slash;
    SagRe *re;
    u32 i;
    u32 n = count == 0U ? 1U : count;
    bool any = false;

    if (ed == NULL || w == NULL || w->buf == NULL)
        return false;
    re = ed->search.re;
    if (re == NULL) {
        /* Nothing searched yet this session: fall back to register `/`,
         * which survives across prompts. */
        slash = sag_reg_get(&ed->regs, (u8)'/');
        if (slash == NULL || slash->bytes.len == 0U) {
            sag_msg(ed, SAG_MSG_ERROR, "no previous search pattern");
            return false;
        }
        re = sag_search_compile(&ed->search.arena,
                                (const char *)slash->bytes.data,
                                slash->bytes.len, &ed->search_opts, NULL);
        if (re == NULL)
            return false;
        ed->search.re = re;
        ed->search.pat_gen++;
    }
    /*
     * `N` is the opposite of the SEARCH's direction, not an absolute
     * backwards: after `?foo`, `n` goes backwards and `N` forwards.
     */
    {
        bool backward = ed->search.reverse ? forward : !forward;

        backward = forward ? ed->search.reverse : !ed->search.reverse;
        for (i = 0U; i < n; i++) {
            const Cursor *c = sag_ed_cursor(ed);
            u64 from;
            u64 hit = 0U;
            bool wrapped = false;

            if (c == NULL)
                break;
            /*
             * Step off the current match before searching, or `n` finds
             * the match the cursor is already on and appears to do
             * nothing.
             */
            from = backward ? c->pos.v
                            : sag_grapheme_next(w->buf->tb, c->pos).v;
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
        sag_overlay_refresh(ed, w, ed->search.re, ed->search.pat_gen,
                            SAG_OVERLAY_BUDGET_US);
    /*
     * Exactly one match, and the cursor is on it: say so rather than
     * appearing to do nothing.  Silence here reads as a broken
     * keybinding.
     */
    if (any && w->overlay.count_total == 1U)
        sag_msg(ed, SAG_MSG_INFO, "1 match");
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
    u64 len = sag_textbuf_len(tb);
    u64 lo = pos.v;
    u64 hi = pos.v;

    if (len == 0U)
        return false;
    if (lo > len)
        lo = len;
    while (lo > 0U && !sag_word_boundary(tb, BYTEOFF(lo)))
        lo--;
    while (hi < len && !sag_word_boundary(tb, BYTEOFF(hi + 1U)))
        hi++;
    if (hi < len)
        hi++;
    out->lo = lo;
    out->hi = hi;
    return hi > lo;
}

bool sag_search_word(Ed *ed, Win *w, bool forward)
{
    Bytebuf pat;
    Span word;
    const Cursor *c;
    u64 at;
    bool ok = false;

    if (ed == NULL || w == NULL || w->buf == NULL)
        return false;
    c = sag_ed_cursor(ed);
    if (c == NULL)
        return false;
    if (!search_word_span(w->buf->tb, c->pos, &word)) {
        sag_msg(ed, SAG_MSG_ERROR, "no word under the cursor");
        return false;
    }
    bytebuf_init(&pat);
    /*
     * `\b` + the QUOTED word + `\b`.  Quoting goes through the engine's
     * own sag_re_quote so there is exactly one implementation of
     * "escape this literal" — searching for `a.b` must not match `axb`.
     */
    bytebuf_append(&pat, "\\b", 2U);
    {
        Bytebuf raw;
        u64 i;

        bytebuf_init(&raw);
        for (i = word.lo; i < word.hi; i++) {
            u8 b = 0U;
            SagReInput in = sag_re_input_textbuf(w->buf->tb);

            if (sag_re_input_byte(&in, i, &b))
                bytebuf_push_u8(&raw, b);
        }
        sag_re_quote(&pat, raw.data, raw.len);
        bytebuf_free(&raw);
    }
    bytebuf_append(&pat, "\\b", 2U);

    ed->search.re = sag_search_compile(&ed->search.arena,
                                       (const char *)pat.data, pat.len,
                                       &ed->search_opts, NULL);
    if (ed->search.re != NULL) {
        ed->search.pat = arena_alloc(&ed->search.arena, pat.len + 1U, 1U);
        (void)memcpy(ed->search.pat, pat.data, pat.len);
        ed->search.pat[pat.len] = '\0';
        ed->search.patlen = pat.len;
        ed->search.pat_gen++;
        ed->search.reverse = !forward;
        sag_reg_set_search(&ed->regs, pat.data, pat.len);
        /* `*` is a jump, so where we stood is worth coming back to. */
        at = c->pos.v;
        sag_jump_push(w, BYTEOFF(at), ed->now_ms);
        ok = sag_search_step(ed, w, true, 1U);
    }
    bytebuf_free(&pat);
    return ok;
}
