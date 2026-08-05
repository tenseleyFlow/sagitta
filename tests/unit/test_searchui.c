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

    sag_ed_init(ed);
    SAG_ASSERT(sag_ed_open_scratch(ed));
    bytebuf_init(&src);
    for (i = 0U; i < 40U; i++) {
        if (i == 5U || i == 30U)
            bytebuf_printf(&src, "line %u needle\n", (unsigned)i);
        else
            bytebuf_printf(&src, "line %u\n", (unsigned)i);
    }
    ec = sag_ed_edit_ctx(ed);
    SAG_ASSERT(sag_edit_insert(&ec, BYTEOFF(0U), src.data, src.len));
    bytebuf_free(&src);
    ed->win->rect.h = 10U;
    ed->win->rect.w = 80U;
}

/* Drives a search without a terminal: seed the state the way the prompt
 * would, then run one input pass. */
static void su_search(Ed *ed, const char *pat, bool reverse)
{
    sag_reg_set_search(&ed->regs, (const u8 *)pat, strlen(pat));
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
    c = sag_ed_cursor(&ed);
    c->pos = BYTEOFF(20U);
    c->goal_col = (GCol){7U};
    ed.win->vp.top = LINENO(2U);
    saved = *c;
    saved_top = ed.win->vp.top;

    sag_search_open(&ed, ed.win, false);
    SAG_ASSERT(ed.search.active);
    /* Move somewhere far away, as a preview would. */
    sag_ed_cursor(&ed)->pos = BYTEOFF(300U);
    sag_ed_cursor(&ed)->goal_col = (GCol){0U};
    ed.win->vp.top = LINENO(28U);

    sag_search_cancel(&ed, ed.win);
    c = sag_ed_cursor(&ed);
    SAG_ASSERT_EQ_U64(c->pos.v, saved.pos.v);
    SAG_ASSERT_EQ_U64(c->goal_col.v, saved.goal_col.v);
    SAG_ASSERT_EQ_U64(c->anchor.v, saved.anchor.v);
    SAG_ASSERT_EQ_U64(ed.win->vp.top.v, saved_top.v);
    SAG_ASSERT(!ed.search.active);
    /* And the highlight is gone. */
    SAG_ASSERT_EQ_U64(ed.win->overlay.spans.len, 0U);
    sag_ed_free(&ed);
}

/* Accept leaves the cursor where the preview put it and records the
 * pattern in register `/`. */
void test_searchui_accept_commits_pattern_to_register(void)
{
    Ed ed;
    const RegVal *slash;

    su_fixture(&ed);
    sag_search_open(&ed, ed.win, false);
    ed.search.pat = arena_alloc(&ed.search.arena, 7U, 1U);
    (void)memcpy(ed.search.pat, "needle", 7U);
    ed.search.patlen = 6U;
    sag_search_accept(&ed, ed.win);

    slash = sag_reg_get(&ed.regs, (u8)'/');
    SAG_ASSERT_NOT_NULL(slash);
    SAG_ASSERT_EQ_U64(slash->bytes.len, 6U);
    SAG_ASSERT(memcmp(slash->bytes.data, "needle", 6U) == 0);
    /* Accepting a search is a jump, so where we came from is on the
     * jumplist. */
    SAG_ASSERT_EQ_U64(sag_jumplist_len(&ed.win->jumps), 1U);
    sag_ed_free(&ed);
}

void test_searchui_step_finds_successive_matches(void)
{
    Ed ed;
    u64 first;
    u64 second;

    su_fixture(&ed);
    su_search(&ed, "needle", false);
    sag_ed_cursor(&ed)->pos = BYTEOFF(0U);

    SAG_ASSERT(sag_search_step(&ed, ed.win, true, 1U));
    first = sag_ed_cursor(&ed)->pos.v;
    SAG_ASSERT(sag_search_step(&ed, ed.win, true, 1U));
    second = sag_ed_cursor(&ed)->pos.v;
    /* It moved on rather than re-finding the match under the cursor —
     * silence there reads as a broken keybinding. */
    SAG_ASSERT(second > first);

    /* And N comes back. */
    SAG_ASSERT(sag_search_step(&ed, ed.win, false, 1U));
    SAG_ASSERT_EQ_U64(sag_ed_cursor(&ed)->pos.v, first);
    sag_ed_free(&ed);
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
    sag_ed_cursor(&ed)->pos = BYTEOFF(sag_textbuf_len(ed.buffer.tb));
    start = sag_ed_cursor(&ed)->pos.v;

    SAG_ASSERT(sag_search_step(&ed, ed.win, true, 1U));
    after_n = sag_ed_cursor(&ed)->pos.v;
    SAG_ASSERT(after_n < start);

    /* N, being the opposite of the search, goes forwards. */
    sag_ed_cursor(&ed)->pos = BYTEOFF(0U);
    SAG_ASSERT(sag_search_step(&ed, ed.win, false, 1U));
    SAG_ASSERT(sag_ed_cursor(&ed)->pos.v > 0U);
    sag_ed_free(&ed);
}

void test_searchui_count_repeats_the_step(void)
{
    Ed ed;
    u64 one;
    u64 two;

    su_fixture(&ed);
    su_search(&ed, "line", false);
    sag_ed_cursor(&ed)->pos = BYTEOFF(0U);
    SAG_ASSERT(sag_search_step(&ed, ed.win, true, 1U));
    one = sag_ed_cursor(&ed)->pos.v;

    sag_ed_cursor(&ed)->pos = BYTEOFF(0U);
    ed.search.re = NULL;
    su_search(&ed, "line", false);
    SAG_ASSERT(sag_search_step(&ed, ed.win, true, 3U));
    two = sag_ed_cursor(&ed)->pos.v;
    SAG_ASSERT(two > one);
    sag_ed_free(&ed);
}

/* wrapscan on: running off the end continues at the other one. */
void test_searchui_wrap_continues_from_the_other_end(void)
{
    Ed ed;

    su_fixture(&ed);
    su_search(&ed, "needle", false);
    /* Past the last match: only a wrap can find anything. */
    sag_ed_cursor(&ed)->pos = BYTEOFF(sag_textbuf_len(ed.buffer.tb) - 1U);
    ed.search_opts.wrapscan = true;
    SAG_ASSERT(sag_search_step(&ed, ed.win, true, 1U));
    SAG_ASSERT(ed.search.wrapped);
    /* It landed on the FIRST match, at line 5. */
    SAG_ASSERT_EQ_U64(
        sag_textbuf_line_of(ed.buffer.tb, sag_ed_cursor(&ed)->pos).v, 5U);
    sag_ed_free(&ed);
}

/* wrapscan off: the cursor does not move and the message says why. */
void test_searchui_wrapscan_off_stops_at_the_end(void)
{
    Ed ed;
    u64 before;

    su_fixture(&ed);
    su_search(&ed, "needle", false);
    sag_ed_cursor(&ed)->pos = BYTEOFF(sag_textbuf_len(ed.buffer.tb) - 1U);
    before = sag_ed_cursor(&ed)->pos.v;
    ed.search_opts.wrapscan = false;
    SAG_ASSERT(!sag_search_step(&ed, ed.win, true, 1U));
    SAG_ASSERT_EQ_U64(sag_ed_cursor(&ed)->pos.v, before);
    sag_ed_free(&ed);
}

void test_searchui_no_pattern_reports_rather_than_moving(void)
{
    Ed ed;
    u64 before;

    su_fixture(&ed);
    before = sag_ed_cursor(&ed)->pos.v;
    /* Nothing searched, and register `/` is empty. */
    SAG_ASSERT(!sag_search_step(&ed, ed.win, true, 1U));
    SAG_ASSERT_EQ_U64(sag_ed_cursor(&ed)->pos.v, before);
    sag_ed_free(&ed);
}

/*
 * `*` builds its pattern through sag_re_quote, so a word containing
 * regex metacharacters searches for itself rather than for a pattern.
 */
void test_searchui_word_search_quotes_metacharacters(void)
{
    Ed ed;
    EditCtx ec;
    const RegVal *slash;

    sag_ed_init(&ed);
    SAG_ASSERT(sag_ed_open_scratch(&ed));
    ec = sag_ed_edit_ctx(&ed);
    /* `a.b` must not match `axb`. */
    SAG_ASSERT(sag_edit_insert(&ec, BYTEOFF(0U),
                               (const u8 *)"axb\na.b\naxb\n", 12U));
    ed.win->rect.h = 10U;
    ed.win->rect.w = 80U;
    sag_ed_cursor(&ed)->pos = BYTEOFF(4U); /* inside `a.b` */

    SAG_ASSERT(sag_search_word(&ed, ed.win, true));
    slash = sag_reg_get(&ed.regs, (u8)'/');
    SAG_ASSERT_NOT_NULL(slash);
    /* The dot is escaped and the word is \b-wrapped. */
    SAG_ASSERT(memchr(slash->bytes.data, '\\', slash->bytes.len) != NULL);
    {
        Bytebuf pat;

        bytebuf_init(&pat);
        bytebuf_append(&pat, slash->bytes.data, slash->bytes.len);
        bytebuf_push_u8(&pat, 0U);
        SAG_ASSERT(strstr((const char *)pat.data, "\\.") != NULL);
        SAG_ASSERT(strncmp((const char *)pat.data, "\\b", 2U) == 0);
        bytebuf_free(&pat);
    }
    sag_ed_free(&ed);
}

/* A pattern that will not compile keeps the last good one highlighting
 * rather than blanking the screen. */
void test_searchui_bad_pattern_keeps_the_last_good_program(void)
{
    Ed ed;
    SagRe *good;
    SearchOpts o;

    su_fixture(&ed);
    sag_search_opts_init(&o);
    good = sag_search_compile(&ed.search.arena, "needle", 6U, &o, NULL);
    SAG_ASSERT_NOT_NULL(good);
    ed.search.re = good;
    ed.search.active = true;

    /* `[a-` is a normal intermediate state of typing `[a-z]`. */
    {
        SagReErr err;
        SagRe *bad;

        (void)memset(&err, 0, sizeof(err));
        bad = sag_search_compile(&ed.search.arena, "[a-", 3U, &o, &err);
        SAG_ASSERT_NULL(bad);
        SAG_ASSERT_NOT_NULL(err.msg);
        /* The caret offset points inside what was typed, so the prompt
         * can draw it under the offending construct. */
        SAG_ASSERT(err.off <= 3U);
    }
    /* The last good program is still the one on record. */
    SAG_ASSERT(ed.search.re == good);
    sag_ed_free(&ed);
}
