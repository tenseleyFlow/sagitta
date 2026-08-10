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
