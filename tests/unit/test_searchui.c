/*
 * Sprint 21 §1/§2 / DoD 3: the live search surface.
 *
 * The pty golden compares whole grids before and after a cancel; this
 * file asserts the same property structurally, which is what tells you
 * WHICH field was not restored when the golden goes red.
 */
#include "harness.h"

#include <stdio.h>
#include <string.h>

#include "edit/ed.h"
#include "search/searchui.h"
#include "text/piece.h"
#include "text/register.h"

/* 40 lines: "line N needle" on 5 and 30, "line N" elsewhere. */
static void su_fixture(Ed *ed)
{
    EditCtx ec;
    Bytebuf src;
    u32 i;

    yew_ed_init(ed);
    YEW_ASSERT(yew_ed_open_scratch(ed));
    bytebuf_init(&src);
    for (i = 0U; i < 40U; i++) {
        if (i == 5U || i == 30U)
            bytebuf_printf(&src, "line %u needle\n", (unsigned)i);
        else
            bytebuf_printf(&src, "line %u\n", (unsigned)i);
    }
    ec = yew_ed_edit_ctx(ed);
    YEW_ASSERT(yew_edit_insert(&ec, BYTEOFF(0U), src.data, src.len));
    bytebuf_free(&src);
    ed->win->rect.h = 10U;
    ed->win->rect.w = 80U;
}

/* Drives a search without a terminal: seed the state the way the prompt
 * would, then run one input pass. */
static void su_search(Ed *ed, const char *pat, bool reverse)
{
    yew_reg_set_search(&ed->regs, (const u8 *)pat, strlen(pat));
    ed->search.re = NULL;
    ed->search.reverse = reverse;
    ed->search.pat = NULL;
    ed->search.patlen = 0U;
}

static void su_large_fixture(Ed *ed, size_t len, size_t needle_at)
{
    EditCtx ec;
    Bytebuf src;

    YEW_ASSERT(needle_at <= len);
    YEW_ASSERT(len - needle_at >= 6U);
    yew_ed_init(ed);
    YEW_ASSERT(yew_ed_open_scratch(ed));
    bytebuf_init(&src);
    bytebuf_reserve(&src, len);
    (void)memset(src.data, 'x', len);
    (void)memcpy(src.data + needle_at, "needle", 6U);
    src.len = len;
    ec = yew_ed_edit_ctx(ed);
    YEW_ASSERT(yew_edit_insert(&ec, BYTEOFF(0U), src.data, src.len));
    bytebuf_free(&src);
    ed->win->rect.h = 10U;
    ed->win->rect.w = 80U;
}

static void su_finish_preview(Ed *ed)
{
    u32 turns = 0U;

    while (ed->search.preview_timer != YEW_TIMER_NONE) {
        YEW_ASSERT(turns++ < 64U);
        ed->now_ms++;
        yew_timers_fire(&ed->timers, ed, ed->now_ms);
    }
}

/*
 * DoD 3, structurally.  Cancel must restore the cursor, its GOAL
 * COLUMN, and the viewport's top line.  Restoring only the cursor is
 * the bug this exists to catch: the user presses `/`, looks, changes
 * their mind, and the window is still scrolled somewhere else.
 */
void test_searchui_cancel_restores_cursor_goal_and_viewport(void)
{
    Ed ed;
    Cursor *c;
    Cursor saved;
    LineNo saved_top;

    su_fixture(&ed);
    c = yew_ed_cursor(&ed);
    c->pos = BYTEOFF(20U);
    c->goal_col = (GCol){7U};
    ed.win->vp.top = LINENO(2U);
    saved = *c;
    saved_top = ed.win->vp.top;

    yew_search_open(&ed, ed.win, false);
    YEW_ASSERT(ed.search.active);
    /* Move somewhere far away, as a preview would. */
    yew_ed_cursor(&ed)->pos = BYTEOFF(300U);
    yew_ed_cursor(&ed)->goal_col = (GCol){0U};
    ed.win->vp.top = LINENO(28U);

    yew_search_cancel(&ed, ed.win);
    c = yew_ed_cursor(&ed);
    YEW_ASSERT_EQ_U64(c->pos.v, saved.pos.v);
    YEW_ASSERT_EQ_U64(c->goal_col.v, saved.goal_col.v);
    YEW_ASSERT_EQ_U64(c->anchor.v, saved.anchor.v);
    YEW_ASSERT_EQ_U64(ed.win->vp.top.v, saved_top.v);
    YEW_ASSERT(!ed.search.active);
    /* And the highlight is gone. */
    YEW_ASSERT_EQ_U64(ed.win->overlay.spans.len, 0U);
    yew_ed_free(&ed);
}

/* Accept leaves the cursor where the preview put it and records the
 * pattern in register `/`. */
void test_searchui_accept_commits_pattern_to_register(void)
{
    Ed ed;
    const RegVal *slash;

    su_fixture(&ed);
    yew_search_open(&ed, ed.win, false);
    ed.search.pat = arena_alloc(&ed.search.arena, 7U, 1U);
    (void)memcpy(ed.search.pat, "needle", 7U);
    ed.search.patlen = 6U;
    yew_search_accept(&ed, ed.win);

    slash = yew_reg_get(&ed.regs, (u8)'/');
    YEW_ASSERT_NOT_NULL(slash);
    YEW_ASSERT_EQ_U64(slash->bytes.len, 6U);
    YEW_ASSERT(memcmp(slash->bytes.data, "needle", 6U) == 0);
    /* Accepting a search is a jump, so where we came from is on the
     * jumplist. */
    YEW_ASSERT_EQ_U64(yew_jumplist_len(&ed.win->jumps), 1U);
    yew_ed_free(&ed);
}

void test_searchui_accept_retires_deferred_count(void)
{
    Ed ed;

    su_fixture(&ed);
    yew_search_open(&ed, ed.win, false);
    ed.search.re = yew_search_compile(&ed.search.arena, "needle", 6U,
                                      &ed.search_opts, NULL);
    YEW_ASSERT_NOT_NULL(ed.search.re);
    ed.search.pat = arena_alloc(&ed.search.arena, 7U, 1U);
    (void)memcpy(ed.search.pat, "needle", 7U);
    ed.search.patlen = 6U;
    yew_search_schedule_count(&ed, ed.win);
    YEW_ASSERT(ed.search.count_timer != YEW_TIMER_NONE);
    YEW_ASSERT_EQ_U64(ed.search.count_win_id, ed.win->id);

    yew_search_accept(&ed, ed.win);

    YEW_ASSERT_EQ_U64(ed.search.count_timer, YEW_TIMER_NONE);
    YEW_ASSERT_EQ_U64(ed.search.count_win_id, 0U);
    YEW_ASSERT_EQ_U64(ed.win->overlay.count_total, 2U);
    YEW_ASSERT(!ed.win->overlay.count_capped);
    yew_ed_free(&ed);
}

void test_searchui_step_finds_successive_matches(void)
{
    Ed ed;
    u64 first;
    u64 second;

    su_fixture(&ed);
    su_search(&ed, "needle", false);
    yew_ed_cursor(&ed)->pos = BYTEOFF(0U);

    YEW_ASSERT(yew_search_step(&ed, ed.win, true, 1U));
    first = yew_ed_cursor(&ed)->pos.v;
    YEW_ASSERT(yew_search_step(&ed, ed.win, true, 1U));
    second = yew_ed_cursor(&ed)->pos.v;
    /* It moved on rather than re-finding the match under the cursor —
     * silence there reads as a broken keybinding. */
    YEW_ASSERT(second > first);

    /* And N comes back. */
    YEW_ASSERT(yew_search_step(&ed, ed.win, false, 1U));
    YEW_ASSERT_EQ_U64(yew_ed_cursor(&ed)->pos.v, first);
    yew_ed_free(&ed);
}

/*
 * The pinned direction rule: `n` repeats in the SEARCH's direction, so
 * after `?foo` an `n` goes backwards.  Getting this wrong makes `?`
 * feel like `/`.
 */
void test_searchui_n_is_relative_to_the_search_direction(void)
{
    Ed ed;
    u64 start;
    u64 after_n;

    su_fixture(&ed);
    su_search(&ed, "needle", true); /* a `?` search */
    /* Stand after both matches so a backwards step has somewhere to go. */
    yew_ed_cursor(&ed)->pos = BYTEOFF(yew_textbuf_len(ed.buffer.tb));
    start = yew_ed_cursor(&ed)->pos.v;

    YEW_ASSERT(yew_search_step(&ed, ed.win, true, 1U));
    after_n = yew_ed_cursor(&ed)->pos.v;
    YEW_ASSERT(after_n < start);

    /* N, being the opposite of the search, goes forwards. */
    yew_ed_cursor(&ed)->pos = BYTEOFF(0U);
    YEW_ASSERT(yew_search_step(&ed, ed.win, false, 1U));
    YEW_ASSERT(yew_ed_cursor(&ed)->pos.v > 0U);
    yew_ed_free(&ed);
}

void test_searchui_count_repeats_the_step(void)
{
    Ed ed;
    u64 one;
    u64 two;

    su_fixture(&ed);
    su_search(&ed, "line", false);
    yew_ed_cursor(&ed)->pos = BYTEOFF(0U);
    YEW_ASSERT(yew_search_step(&ed, ed.win, true, 1U));
    one = yew_ed_cursor(&ed)->pos.v;

    yew_ed_cursor(&ed)->pos = BYTEOFF(0U);
    ed.search.re = NULL;
    su_search(&ed, "line", false);
    YEW_ASSERT(yew_search_step(&ed, ed.win, true, 3U));
    su_finish_preview(&ed);
    two = yew_ed_cursor(&ed)->pos.v;
    YEW_ASSERT(two > one);
    yew_ed_free(&ed);
}

/* wrapscan on: running off the end continues at the other one. */
void test_searchui_wrap_continues_from_the_other_end(void)
{
    Ed ed;

    su_fixture(&ed);
    su_search(&ed, "needle", false);
    /* Past the last match: only a wrap can find anything. */
    yew_ed_cursor(&ed)->pos = BYTEOFF(yew_textbuf_len(ed.buffer.tb) - 1U);
    ed.search_opts.wrapscan = true;
    YEW_ASSERT(yew_search_step(&ed, ed.win, true, 1U));
    YEW_ASSERT(ed.search.wrapped);
    /* It landed on the FIRST match, at line 5. */
    YEW_ASSERT_EQ_U64(
        yew_textbuf_line_of(ed.buffer.tb, yew_ed_cursor(&ed)->pos).v, 5U);
    yew_ed_free(&ed);
}

/* wrapscan off: the cursor does not move and the message says why. */
void test_searchui_wrapscan_off_stops_at_the_end(void)
{
    Ed ed;
    u64 before;

    su_fixture(&ed);
    su_search(&ed, "needle", false);
    yew_ed_cursor(&ed)->pos = BYTEOFF(yew_textbuf_len(ed.buffer.tb) - 1U);
    before = yew_ed_cursor(&ed)->pos.v;
    ed.search_opts.wrapscan = false;
    YEW_ASSERT(!yew_search_step(&ed, ed.win, true, 1U));
    YEW_ASSERT_EQ_U64(yew_ed_cursor(&ed)->pos.v, before);
    yew_ed_free(&ed);
}

void test_searchui_no_pattern_reports_rather_than_moving(void)
{
    Ed ed;
    u64 before;

    su_fixture(&ed);
    before = yew_ed_cursor(&ed)->pos.v;
    /* Nothing searched, and register `/` is empty. */
    YEW_ASSERT(!yew_search_step(&ed, ed.win, true, 1U));
    YEW_ASSERT_EQ_U64(yew_ed_cursor(&ed)->pos.v, before);
    yew_ed_free(&ed);
}

/*
 * `*` builds its pattern through yew_re_quote, so a word containing
 * regex metacharacters searches for itself rather than for a pattern.
 */
void test_searchui_word_search_quotes_metacharacters(void)
{
    Ed ed;
    EditCtx ec;
    const RegVal *slash;

    yew_ed_init(&ed);
    YEW_ASSERT(yew_ed_open_scratch(&ed));
    ec = yew_ed_edit_ctx(&ed);
    /* `a.b` must not match `axb`. */
    YEW_ASSERT(yew_edit_insert(&ec, BYTEOFF(0U),
                               (const u8 *)"axb\na.b\naxb\n", 12U));
    ed.win->rect.h = 10U;
    ed.win->rect.w = 80U;
    yew_ed_cursor(&ed)->pos = BYTEOFF(4U); /* inside `a.b` */

    YEW_ASSERT(yew_search_word(&ed, ed.win, true));
    slash = yew_reg_get(&ed.regs, (u8)'/');
    YEW_ASSERT_NOT_NULL(slash);
    /* The dot is escaped and the word is \b-wrapped. */
    YEW_ASSERT(memchr(slash->bytes.data, '\\', slash->bytes.len) != NULL);
    {
        Bytebuf pat;

        bytebuf_init(&pat);
        bytebuf_append(&pat, slash->bytes.data, slash->bytes.len);
        bytebuf_push_u8(&pat, 0U);
        YEW_ASSERT(strstr((const char *)pat.data, "\\.") != NULL);
        YEW_ASSERT(strncmp((const char *)pat.data, "\\b", 2U) == 0);
        bytebuf_free(&pat);
    }
    yew_ed_free(&ed);
}

/* A pattern that will not compile keeps the last good one highlighting
 * rather than blanking the screen. */
void test_searchui_bad_pattern_keeps_the_last_good_program(void)
{
    Ed ed;
    YewRe *good;
    SearchOpts o;

    su_fixture(&ed);
    yew_search_opts_init(&o);
    good = yew_search_compile(&ed.search.arena, "needle", 6U, &o, NULL);
    YEW_ASSERT_NOT_NULL(good);
    ed.search.re = good;
    ed.search.active = true;

    /* `[a-` is a normal intermediate state of typing `[a-z]`. */
    {
        YewReErr err;
        YewRe *bad;

        (void)memset(&err, 0, sizeof(err));
        bad = yew_search_compile(&ed.search.arena, "[a-", 3U, &o, &err);
        YEW_ASSERT_NULL(bad);
        YEW_ASSERT_NOT_NULL(err.msg);
        /* The caret offset points inside what was typed, so the prompt
         * can draw it under the offending construct. */
        YEW_ASSERT(err.off <= 3U);
    }
    /* The last good program is still the one on record. */
    YEW_ASSERT(ed.search.re == good);
    yew_ed_free(&ed);
}

void test_searchui_literal_preview_continues_in_bounded_slices(void)
{
    enum { CHUNK = 1024U * 1024U };
    Ed ed;
    const size_t hit = CHUNK + 32U;

    su_large_fixture(&ed, CHUNK + 128U, hit);
    su_search(&ed, "needle", false);
    yew_ed_cursor(&ed)->pos = BYTEOFF(0U);

    YEW_ASSERT(yew_search_step(&ed, ed.win, true, 1U));
    YEW_ASSERT_EQ_U64(yew_ed_cursor(&ed)->pos.v, 0U);
    YEW_ASSERT(ed.search.preview_timer != YEW_TIMER_NONE);
    YEW_ASSERT_EQ_U64(ed.search.preview_win_id, ed.win->id);
    YEW_ASSERT(ed.search.preview_pending);
    YEW_ASSERT(ed.msg.active);

    su_finish_preview(&ed);
    YEW_ASSERT_EQ_U64(yew_ed_cursor(&ed)->pos.v, hit);
    YEW_ASSERT_EQ_U64(ed.search.preview_timer, YEW_TIMER_NONE);
    YEW_ASSERT_EQ_U64(ed.search.preview_win_id, 0U);
    YEW_ASSERT(!ed.search.preview_pending);
    YEW_ASSERT(!ed.msg.active);
    yew_ed_free(&ed);
}

void test_searchui_accept_resumes_pending_preview(void)
{
    enum { CHUNK = 1024U * 1024U };
    Ed ed;
    const size_t hit = CHUNK + 32U;

    su_large_fixture(&ed, CHUNK + 128U, hit);
    yew_search_open(&ed, ed.win, false);
    su_search(&ed, "needle", false);
    yew_ed_cursor(&ed)->pos = BYTEOFF(0U);
    YEW_ASSERT(yew_search_step(&ed, ed.win, true, 1U));
    YEW_ASSERT(ed.search.preview_timer != YEW_TIMER_NONE);
    YEW_ASSERT(ed.search.preview_pending);

    /* Raw Enter arrives before terminal decoding.  It must preempt the
     * timer without throwing away the search Enter is about to accept. */
    yew_search_preview_preempt(&ed);
    YEW_ASSERT_EQ_U64(ed.search.preview_timer, YEW_TIMER_NONE);
    YEW_ASSERT(ed.search.preview_pending);
    yew_search_accept(&ed, ed.win);

    YEW_ASSERT(ed.search.preview_timer != YEW_TIMER_NONE);
    YEW_ASSERT_EQ_U64(ed.search.preview_win_id, ed.win->id);
    YEW_ASSERT(ed.search.preview_pending);
    ed.now_ms++;
    yew_timers_fire(&ed.timers, &ed, ed.now_ms);
    YEW_ASSERT_EQ_U64(yew_ed_cursor(&ed)->pos.v, hit);
    YEW_ASSERT_EQ_U64(ed.search.preview_timer, YEW_TIMER_NONE);
    YEW_ASSERT(!ed.search.preview_pending);
    yew_ed_free(&ed);
}

void test_searchui_literal_preview_finds_across_a_slice_edge(void)
{
    enum { CHUNK = 1024U * 1024U };
    Ed ed;
    const size_t hit = CHUNK - 2U;

    su_large_fixture(&ed, CHUNK + 64U, hit);
    su_search(&ed, "needle", false);
    yew_ed_cursor(&ed)->pos = BYTEOFF(0U);

    YEW_ASSERT(yew_search_step(&ed, ed.win, true, 1U));
    YEW_ASSERT_EQ_U64(yew_ed_cursor(&ed)->pos.v, hit);
    YEW_ASSERT_EQ_U64(ed.search.preview_timer, YEW_TIMER_NONE);
    yew_ed_free(&ed);
}

void test_searchui_backward_literal_finds_across_slice_edges(void)
{
    enum {
        CHUNK = 1024U * 1024U,
        BACK_WINDOW = 256U * 1024U
    };
    Ed ed;
    const size_t hit = 128U + BACK_WINDOW - 2U;

    /* With this length, the first 1 MiB backward slice begins at byte 128;
     * the hit also straddles yew_re_search_back's internal 256 KiB edge. */
    su_large_fixture(&ed, CHUNK + 128U, hit);
    su_search(&ed, "needle", true);
    yew_ed_cursor(&ed)->pos = BYTEOFF(CHUNK + 128U);

    YEW_ASSERT(yew_search_step(&ed, ed.win, true, 1U));
    YEW_ASSERT_EQ_U64(yew_ed_cursor(&ed)->pos.v, hit);
    YEW_ASSERT_EQ_U64(ed.search.preview_timer, YEW_TIMER_NONE);
    yew_ed_free(&ed);

    /* The search origin is also the high edge of the first outer 1 MiB
     * preview slice.  A match is selected by its start offset, so clipping
     * the literal at that edge must not hide a match that begins before it. */
    su_large_fixture(&ed, CHUNK + 128U, CHUNK - 2U);
    su_search(&ed, "needle", true);
    yew_ed_cursor(&ed)->pos = BYTEOFF(CHUNK);

    YEW_ASSERT(yew_search_step(&ed, ed.win, true, 1U));
    YEW_ASSERT_EQ_U64(yew_ed_cursor(&ed)->pos.v, CHUNK - 2U);
    YEW_ASSERT_EQ_U64(ed.search.preview_timer, YEW_TIMER_NONE);
    yew_ed_free(&ed);
}

void test_searchui_counted_literal_continues_without_blocking(void)
{
    enum { CHUNK = 1024U * 1024U };
    Ed ed;
    EditCtx ec;
    const size_t late = CHUNK + 32U;

    su_large_fixture(&ed, CHUNK + 128U, late);
    ec = yew_ed_edit_ctx(&ed);
    YEW_ASSERT(yew_edit_insert(&ec, BYTEOFF(12U),
                               (const u8 *)"needle", 6U));
    su_search(&ed, "needle", false);
    yew_ed_cursor(&ed)->pos = BYTEOFF(0U);

    YEW_ASSERT(yew_search_step(&ed, ed.win, true, 2U));
    YEW_ASSERT_EQ_U64(yew_ed_cursor(&ed)->pos.v, 12U);
    YEW_ASSERT(ed.search.preview_pending);
    YEW_ASSERT(ed.search.preview_timer != YEW_TIMER_NONE);
    su_finish_preview(&ed);
    YEW_ASSERT_EQ_U64(yew_ed_cursor(&ed)->pos.v, late + 6U);
    YEW_ASSERT(!ed.search.preview_pending);
    yew_ed_free(&ed);
}

void test_searchui_empty_input_retires_count_state(void)
{
    Ed ed;

    su_fixture(&ed);
    yew_search_open(&ed, ed.win, false);
    ed.search.re = yew_search_compile(&ed.search.arena, "needle", 6U,
                                      &ed.search_opts, NULL);
    YEW_ASSERT_NOT_NULL(ed.search.re);
    yew_search_schedule_count(&ed, ed.win);
    ed.win->overlay.count_total = 2U;
    ed.win->overlay.count_capped = true;

    /* The newly opened prompt is empty. */
    yew_search_input(&ed, ed.win);
    YEW_ASSERT_EQ_U64(ed.search.count_timer, YEW_TIMER_NONE);
    YEW_ASSERT_EQ_U64(ed.search.count_win_id, 0U);
    YEW_ASSERT_EQ_U64(ed.win->overlay.count_total, 0U);
    YEW_ASSERT(!ed.win->overlay.count_capped);
    yew_ed_free(&ed);
}

void test_searchui_new_literal_cancels_a_pending_preview(void)
{
    enum { CHUNK = 1024U * 1024U };
    Ed ed;
    EditCtx ec;
    const u8 early[] = "early";

    su_large_fixture(&ed, CHUNK + 128U, CHUNK + 32U);
    ec = yew_ed_edit_ctx(&ed);
    YEW_ASSERT(yew_edit_insert(&ec, BYTEOFF(12U), early,
                               sizeof(early) - 1U));
    su_search(&ed, "needle", false);
    yew_ed_cursor(&ed)->pos = BYTEOFF(0U);
    YEW_ASSERT(yew_search_step(&ed, ed.win, true, 1U));
    YEW_ASSERT(ed.search.preview_timer != YEW_TIMER_NONE);

    su_search(&ed, "early", false);
    YEW_ASSERT(yew_search_step(&ed, ed.win, true, 1U));
    YEW_ASSERT_EQ_U64(yew_ed_cursor(&ed)->pos.v, 12U);
    YEW_ASSERT_EQ_U64(ed.search.preview_timer, YEW_TIMER_NONE);

    ed.now_ms++;
    yew_timers_fire(&ed.timers, &ed, ed.now_ms);
    YEW_ASSERT_EQ_U64(yew_ed_cursor(&ed)->pos.v, 12U);
    yew_ed_free(&ed);
}

void test_searchui_later_key_cancels_a_pending_preview(void)
{
    enum { CHUNK = 1024U * 1024U };
    Ed ed;
    Key escape = {0};

    su_large_fixture(&ed, CHUNK + 128U, CHUNK + 32U);
    su_search(&ed, "needle", false);
    yew_ed_cursor(&ed)->pos = BYTEOFF(0U);
    YEW_ASSERT(yew_search_step(&ed, ed.win, true, 1U));
    YEW_ASSERT(ed.search.preview_timer != YEW_TIMER_NONE);

    escape.kind = YEW_EV_KEY;
    escape.ev = YEW_KEY_PRESS;
    escape.code = YEW_KEY_ESCAPE;
    yew_ed_handle_key(&ed, escape, ed.now_ms);
    YEW_ASSERT_EQ_U64(ed.search.preview_timer, YEW_TIMER_NONE);
    YEW_ASSERT(!ed.search.preview_pending);
    YEW_ASSERT(!ed.msg.active);

    ed.now_ms++;
    yew_timers_fire(&ed.timers, &ed, ed.now_ms);
    YEW_ASSERT_EQ_U64(yew_ed_cursor(&ed)->pos.v, 0U);
    yew_ed_free(&ed);
}
