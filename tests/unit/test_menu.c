/*
 * Sprint 18.5 §5: the ranked-list widget's selection model and scroll
 * window.  Drawing is covered by the pty goldens; what is unit-tested
 * here is the state machine underneath, because its failure mode is
 * silent -- the user opens the wrong thing and blames themselves.
 */
#include "harness.h"

#include <string.h>

#include "ui/menu.h"

static Vec_CompItem items_of(const char *const *text, size_t n)
{
    Vec_CompItem v = {0};
    size_t i;

    for (i = 0U; i < n; i++) {
        CompItem item;

        (void)memset(&item, 0, sizeof(item));
        item.text = text[i];
        item.match = text[i];
        item.score = (i32)(100 - (i32)i);
        Vec_CompItem_push(&v, item);
    }
    return v;
}

void test_menu_selection_survives_a_refilter_by_identity(void)
{
    static const char *const before[] = {"alpha", "beta", "gamma"};
    static const char *const after[] = {"gamma", "alpha", "beta"};
    Menu m;

    yew_menu_init(&m, NULL);
    yew_menu_reset(&m, items_of(before, 3U), 3U, (Span){0U, 0U});

    /* Choose "beta" at row 1. */
    YEW_ASSERT(yew_menu_move(&m, 1, false));
    YEW_ASSERT(yew_menu_move(&m, 1, false));
    YEW_ASSERT_EQ_STR(yew_menu_selected(&m)->text, "beta");
    YEW_ASSERT_EQ_I64(m.sel, 1);

    /*
     * One more keystroke reorders the list.  Held by index, the
     * selection would slide onto "alpha" a fraction of a second before
     * Enter; held by identity it stays on what the user is looking at.
     */
    yew_menu_reset(&m, items_of(after, 3U), 3U, (Span){0U, 0U});
    YEW_ASSERT_EQ_I64(m.sel, 2);
    YEW_ASSERT_EQ_STR(yew_menu_selected(&m)->text, "beta");
    YEW_ASSERT(m.explicit_sel);

    yew_menu_free(&m);
}

void test_menu_lost_selection_falls_to_nothing_not_to_row_zero(void)
{
    static const char *const before[] = {"alpha", "beta"};
    static const char *const after[] = {"alpha", "gamma"};
    Menu m;

    yew_menu_init(&m, NULL);
    yew_menu_reset(&m, items_of(before, 2U), 2U, (Span){0U, 0U});
    YEW_ASSERT(yew_menu_move(&m, 1, false));
    YEW_ASSERT(yew_menu_move(&m, 1, false));
    YEW_ASSERT_EQ_STR(yew_menu_selected(&m)->text, "beta");

    /*
     * "beta" left the filtered set.  Falling to row 0 would leave a
     * selection the user never made, and §6's Enter rule would then
     * accept "alpha" instead of executing the line.
     */
    yew_menu_reset(&m, items_of(after, 2U), 2U, (Span){0U, 0U});
    YEW_ASSERT_EQ_I64(m.sel, -1);
    YEW_ASSERT(!m.explicit_sel);
    YEW_ASSERT_NULL(yew_menu_selected(&m));

    yew_menu_free(&m);
}

void test_menu_filtering_alone_never_makes_a_selection_explicit(void)
{
    static const char *const rows[] = {"alpha", "beta"};
    Menu m;

    yew_menu_init(&m, NULL);
    /* Ranking put "alpha" first, but the user has chosen nothing. */
    yew_menu_reset(&m, items_of(rows, 2U), 2U, (Span){0U, 0U});
    YEW_ASSERT_EQ_I64(m.sel, -1);
    YEW_ASSERT(!m.explicit_sel);
    YEW_ASSERT_NULL(yew_menu_selected(&m));

    /* Only a move makes it explicit. */
    YEW_ASSERT(yew_menu_move(&m, 1, false));
    YEW_ASSERT(m.explicit_sel);
    YEW_ASSERT_EQ_I64(m.sel, 0);

    yew_menu_free(&m);
}

void test_menu_move_wraps_and_enters_from_either_end(void)
{
    static const char *const rows[] = {"a", "b", "c"};
    MenuSpec wrap = {NULL, 5U, false, true, 0U};
    MenuSpec nowrap = {NULL, 5U, false, false, 0U};
    Menu m;

    yew_menu_init(&m, &wrap);
    yew_menu_reset(&m, items_of(rows, 3U), 3U, (Span){0U, 0U});
    /* Backwards out of "nothing selected" enters at the LAST row, so
     * S-Tab reaches the bottom of the list in one press. */
    YEW_ASSERT(yew_menu_move(&m, -1, false));
    YEW_ASSERT_EQ_I64(m.sel, 2);
    YEW_ASSERT(yew_menu_move(&m, 1, false));
    YEW_ASSERT_EQ_I64(m.sel, 0); /* wrapped */
    YEW_ASSERT(yew_menu_move(&m, -1, false));
    YEW_ASSERT_EQ_I64(m.sel, 2); /* wrapped back */
    yew_menu_free(&m);

    yew_menu_init(&m, &nowrap);
    yew_menu_reset(&m, items_of(rows, 3U), 3U, (Span){0U, 0U});
    YEW_ASSERT(yew_menu_move(&m, 1, false));
    YEW_ASSERT(yew_menu_move(&m, 5, false));
    YEW_ASSERT_EQ_I64(m.sel, 2); /* clamped, not wrapped */
    yew_menu_free(&m);
}

void test_menu_scrolls_to_keep_the_selection_visible(void)
{
    static const char *const rows[] = {"r0", "r1", "r2", "r3", "r4",
                                       "r5", "r6", "r7"};
    MenuSpec spec = {NULL, 3U, false, false, 0U};
    Menu m;
    u16 i;

    yew_menu_init(&m, &spec);
    yew_menu_reset(&m, items_of(rows, 8U), 8U, (Span){0U, 0U});
    /* Sprint 18's menu showed the first five rows and offered no way to
     * reach the sixth; this one scrolls. */
    YEW_ASSERT_EQ_U64(yew_menu_rows(&m, 24U), 3U);
    YEW_ASSERT_EQ_U64(yew_menu_rows(&m, 2U), 2U); /* clipped by area */

    for (i = 0U; i < 8U; i++)
        YEW_ASSERT(yew_menu_move(&m, 1, false));
    YEW_ASSERT_EQ_I64(m.sel, 7);
    yew_menu_free(&m);
}

void test_menu_page_moves_by_the_visible_row_count(void)
{
    static const char *const rows[] = {"r0", "r1", "r2", "r3", "r4",
                                       "r5", "r6", "r7"};
    MenuSpec spec = {NULL, 3U, false, false, 0U};
    Menu m;

    yew_menu_init(&m, &spec);
    yew_menu_reset(&m, items_of(rows, 8U), 8U, (Span){0U, 0U});
    YEW_ASSERT(yew_menu_move(&m, 1, false));
    YEW_ASSERT_EQ_I64(m.sel, 0);
    YEW_ASSERT(yew_menu_move(&m, 1, true));
    YEW_ASSERT_EQ_I64(m.sel, 3);
    YEW_ASSERT(yew_menu_move(&m, 1, true));
    YEW_ASSERT_EQ_I64(m.sel, 6);
    YEW_ASSERT(yew_menu_move(&m, -1, true));
    YEW_ASSERT_EQ_I64(m.sel, 3);
    yew_menu_free(&m);
}

void test_menu_dismiss_and_empty_are_inert(void)
{
    static const char *const rows[] = {"a"};
    Menu m;

    yew_menu_init(&m, NULL);
    YEW_ASSERT(!yew_menu_move(&m, 1, false)); /* nothing to move through */
    YEW_ASSERT_EQ_U64(yew_menu_rows(&m, 24U), 0U);
    YEW_ASSERT_NULL(yew_menu_selected(&m));

    yew_menu_reset(&m, items_of(rows, 1U), 1U, (Span){0U, 0U});
    YEW_ASSERT(yew_menu_move(&m, 1, false));
    yew_menu_dismiss(&m);
    YEW_ASSERT_EQ_I64(m.sel, -1);
    YEW_ASSERT(!m.explicit_sel);
    YEW_ASSERT_EQ_U64(m.items.len, 0U);
    yew_menu_free(&m);
}

/*
 * Sprint 57.17 §3: the window scrolls under the selection, and it scrolls
 * by the CANDIDATE count rather than the row count -- the last row is the
 * `… and N more` tail whenever anything is left below it.
 */
void test_menu_pager_scrolls_and_keeps_an_honest_tail(void)
{
    static const char *const rows[] = {"r0", "r1", "r2", "r3", "r4",
                                       "r5", "r6", "r7"};
    MenuSpec spec = {NULL, 3U, false, false, 0U};
    Menu m;

    yew_menu_init(&m, &spec);
    yew_menu_reset(&m, items_of(rows, 8U), 8U, (Span){0U, 0U});
    YEW_ASSERT_EQ_U64(yew_menu_rows(&m, 24U), 3U);
    /* Nothing selected, window at the top: two candidates and a tail
     * counting the six rows the window cannot reach. */
    YEW_ASSERT_EQ_U64(yew_menu_candidate_rows(&m, 24U), 2U);
    YEW_ASSERT_EQ_U64(yew_menu_hidden(&m, 24U), 6U);

    /* r0, r1 are visible; moving onto what would be the tail scrolls by
     * one and keeps the tail rather than hiding the selection. */
    YEW_ASSERT(yew_menu_move(&m, 1, false));
    YEW_ASSERT_EQ_I64(m.sel, 0);
    YEW_ASSERT_EQ_U64(m.top, 0U);
    YEW_ASSERT(yew_menu_move(&m, 1, false));
    YEW_ASSERT_EQ_I64(m.sel, 1);
    YEW_ASSERT_EQ_U64(m.top, 0U);
    YEW_ASSERT(yew_menu_move(&m, 1, false));
    YEW_ASSERT_EQ_I64(m.sel, 2);
    YEW_ASSERT_EQ_U64(m.top, 1U);
    YEW_ASSERT_EQ_U64(yew_menu_hidden(&m, 24U), 5U);

    /* At the very bottom the window reaches the end, so there is no
     * tail and all three rows are candidates -- otherwise the last row
     * of the list could never be shown. */
    while (m.sel < 7)
        YEW_ASSERT(yew_menu_move(&m, 1, false));
    YEW_ASSERT_EQ_I64(m.sel, 7);
    YEW_ASSERT_EQ_U64(m.top, 5U);
    YEW_ASSERT_EQ_U64(yew_menu_candidate_rows(&m, 24U), 3U);
    YEW_ASSERT_EQ_U64(yew_menu_hidden(&m, 24U), 0U);

    /*
     * Back up.  Two rows up is still inside the window, so nothing
     * scrolls -- the window only moves when it has to.
     */
    YEW_ASSERT(yew_menu_move(&m, -1, false));
    YEW_ASSERT(yew_menu_move(&m, -1, false));
    YEW_ASSERT_EQ_I64(m.sel, 5);
    YEW_ASSERT_EQ_U64(m.top, 5U);
    YEW_ASSERT_EQ_U64(yew_menu_hidden(&m, 24U), 0U);

    /* One more and the window follows upwards, and the tail comes back
     * because rows are hidden below again. */
    YEW_ASSERT(yew_menu_move(&m, -1, false));
    YEW_ASSERT_EQ_I64(m.sel, 4);
    YEW_ASSERT_EQ_U64(m.top, 4U);
    YEW_ASSERT_EQ_U64(yew_menu_candidate_rows(&m, 24U), 2U);
    YEW_ASSERT_EQ_U64(yew_menu_hidden(&m, 24U), 2U);

    /* The tail is never a candidate: whatever is selected, it is a real
     * row of the list and inside the candidate window. */
    YEW_ASSERT_NOT_NULL(yew_menu_selected(&m));
    YEW_ASSERT_EQ_STR(yew_menu_selected(&m)->text, "r4");
    YEW_ASSERT((u32)m.sel <
               m.top + yew_menu_candidate_rows(&m, 24U));
    yew_menu_free(&m);
}

/* A list that fits shows no tail at all. */
void test_menu_no_tail_when_everything_fits(void)
{
    static const char *const rows[] = {"a", "b", "c"};
    MenuSpec spec = {NULL, 5U, false, false, 0U};
    Menu m;

    yew_menu_init(&m, &spec);
    yew_menu_reset(&m, items_of(rows, 3U), 3U, (Span){0U, 0U});
    YEW_ASSERT_EQ_U64(yew_menu_rows(&m, 24U), 3U);
    YEW_ASSERT_EQ_U64(yew_menu_candidate_rows(&m, 24U), 3U);
    YEW_ASSERT_EQ_U64(yew_menu_hidden(&m, 24U), 0U);
    yew_menu_free(&m);
}

/*
 * Sprint 57.17 §2: focus is who the arrows belong to, and it is not the
 * same question as whether a row was chosen.
 */
void test_menu_focus_takes_the_arrows_and_gives_them_back(void)
{
    static const char *const rows[] = {"a", "b", "c"};
    Menu m;

    yew_menu_init(&m, NULL);
    /* Nothing to focus. */
    YEW_ASSERT(!yew_menu_focus(&m));
    YEW_ASSERT(!yew_menu_focused(&m));

    yew_menu_reset(&m, items_of(rows, 3U), 3U, (Span){0U, 0U});
    /* A list on screen is not a focused list. */
    YEW_ASSERT(!yew_menu_focused(&m));
    YEW_ASSERT(yew_menu_focus(&m));
    YEW_ASSERT(yew_menu_focused(&m));
    /* Entering lands on the best match and counts as a choice, which is
     * what makes §6's Enter rule accept it. */
    YEW_ASSERT_EQ_I64(m.sel, 0);
    YEW_ASSERT(m.explicit_sel);

    /* Blur hands the arrows back and leaves the choice standing -- what
     * editing the host's text does. */
    yew_menu_blur(&m);
    YEW_ASSERT(!yew_menu_focused(&m));
    YEW_ASSERT(m.explicit_sel);
    YEW_ASSERT_EQ_I64(m.sel, 0);

    /* Focusing again keeps the row that was already chosen. */
    YEW_ASSERT(yew_menu_move(&m, 1, false));
    YEW_ASSERT_EQ_I64(m.sel, 1);
    yew_menu_blur(&m);
    YEW_ASSERT(yew_menu_focus(&m));
    YEW_ASSERT_EQ_I64(m.sel, 1);

    /* Unselect drops the choice as well, leaving the rows on screen with
     * nothing chosen -- leaving the pager upwards. */
    yew_menu_unselect(&m);
    YEW_ASSERT(!yew_menu_focused(&m));
    YEW_ASSERT(!m.explicit_sel);
    YEW_ASSERT_EQ_I64(m.sel, -1);
    YEW_ASSERT_EQ_U64(m.items.len, 3U);
    YEW_ASSERT_NULL(yew_menu_selected(&m));

    /* Dismissing takes the rows with it. */
    YEW_ASSERT(yew_menu_focus(&m));
    yew_menu_dismiss(&m);
    YEW_ASSERT(!yew_menu_focused(&m));
    YEW_ASSERT(!m.focus);
    yew_menu_free(&m);
}

/* Focus cannot survive losing the row it was on: a refilter that drops
 * the held item leaves nothing for the arrows to move. */
void test_menu_focus_dies_with_a_lost_selection(void)
{
    static const char *const before[] = {"alpha", "beta"};
    static const char *const after[] = {"alpha", "gamma"};
    Menu m;

    yew_menu_init(&m, NULL);
    yew_menu_reset(&m, items_of(before, 2U), 2U, (Span){0U, 0U});
    YEW_ASSERT(yew_menu_move(&m, 1, false));
    YEW_ASSERT(yew_menu_move(&m, 1, false));
    YEW_ASSERT(yew_menu_focus(&m));
    YEW_ASSERT_EQ_STR(yew_menu_selected(&m)->text, "beta");

    yew_menu_reset(&m, items_of(after, 2U), 2U, (Span){0U, 0U});
    YEW_ASSERT_EQ_I64(m.sel, -1);
    YEW_ASSERT(!m.focus);
    YEW_ASSERT(!yew_menu_focused(&m));

    /* A refilter the selection SURVIVES keeps focus where it was. */
    yew_menu_reset(&m, items_of(before, 2U), 2U, (Span){0U, 0U});
    YEW_ASSERT(yew_menu_move(&m, 1, false));
    YEW_ASSERT(yew_menu_focus(&m));
    yew_menu_reset(&m, items_of(before, 2U), 2U, (Span){0U, 0U});
    YEW_ASSERT(yew_menu_focused(&m));
    YEW_ASSERT_EQ_STR(yew_menu_selected(&m)->text, "alpha");
    yew_menu_free(&m);
}
