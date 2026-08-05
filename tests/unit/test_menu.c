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

    sag_menu_init(&m, NULL);
    sag_menu_reset(&m, items_of(before, 3U), 3U, (Span){0U, 0U});

    /* Choose "beta" at row 1. */
    SAG_ASSERT(sag_menu_move(&m, 1, false));
    SAG_ASSERT(sag_menu_move(&m, 1, false));
    SAG_ASSERT_EQ_STR(sag_menu_selected(&m)->text, "beta");
    SAG_ASSERT_EQ_I64(m.sel, 1);

    /*
     * One more keystroke reorders the list.  Held by index, the
     * selection would slide onto "alpha" a fraction of a second before
     * Enter; held by identity it stays on what the user is looking at.
     */
    sag_menu_reset(&m, items_of(after, 3U), 3U, (Span){0U, 0U});
    SAG_ASSERT_EQ_I64(m.sel, 2);
    SAG_ASSERT_EQ_STR(sag_menu_selected(&m)->text, "beta");
    SAG_ASSERT(m.explicit_sel);

    sag_menu_free(&m);
}

void test_menu_lost_selection_falls_to_nothing_not_to_row_zero(void)
{
    static const char *const before[] = {"alpha", "beta"};
    static const char *const after[] = {"alpha", "gamma"};
    Menu m;

    sag_menu_init(&m, NULL);
    sag_menu_reset(&m, items_of(before, 2U), 2U, (Span){0U, 0U});
    SAG_ASSERT(sag_menu_move(&m, 1, false));
    SAG_ASSERT(sag_menu_move(&m, 1, false));
    SAG_ASSERT_EQ_STR(sag_menu_selected(&m)->text, "beta");

    /*
     * "beta" left the filtered set.  Falling to row 0 would leave a
     * selection the user never made, and §6's Enter rule would then
     * accept "alpha" instead of executing the line.
     */
    sag_menu_reset(&m, items_of(after, 2U), 2U, (Span){0U, 0U});
    SAG_ASSERT_EQ_I64(m.sel, -1);
    SAG_ASSERT(!m.explicit_sel);
    SAG_ASSERT_NULL(sag_menu_selected(&m));

    sag_menu_free(&m);
}

void test_menu_filtering_alone_never_makes_a_selection_explicit(void)
{
    static const char *const rows[] = {"alpha", "beta"};
    Menu m;

    sag_menu_init(&m, NULL);
    /* Ranking put "alpha" first, but the user has chosen nothing. */
    sag_menu_reset(&m, items_of(rows, 2U), 2U, (Span){0U, 0U});
    SAG_ASSERT_EQ_I64(m.sel, -1);
    SAG_ASSERT(!m.explicit_sel);
    SAG_ASSERT_NULL(sag_menu_selected(&m));

    /* Only a move makes it explicit. */
    SAG_ASSERT(sag_menu_move(&m, 1, false));
    SAG_ASSERT(m.explicit_sel);
    SAG_ASSERT_EQ_I64(m.sel, 0);

    sag_menu_free(&m);
}

void test_menu_move_wraps_and_enters_from_either_end(void)
{
    static const char *const rows[] = {"a", "b", "c"};
    MenuSpec wrap = {NULL, 5U, false, true, 0U};
    MenuSpec nowrap = {NULL, 5U, false, false, 0U};
    Menu m;

    sag_menu_init(&m, &wrap);
    sag_menu_reset(&m, items_of(rows, 3U), 3U, (Span){0U, 0U});
    /* Backwards out of "nothing selected" enters at the LAST row, so
     * S-Tab reaches the bottom of the list in one press. */
    SAG_ASSERT(sag_menu_move(&m, -1, false));
    SAG_ASSERT_EQ_I64(m.sel, 2);
    SAG_ASSERT(sag_menu_move(&m, 1, false));
    SAG_ASSERT_EQ_I64(m.sel, 0); /* wrapped */
    SAG_ASSERT(sag_menu_move(&m, -1, false));
    SAG_ASSERT_EQ_I64(m.sel, 2); /* wrapped back */
    sag_menu_free(&m);

    sag_menu_init(&m, &nowrap);
    sag_menu_reset(&m, items_of(rows, 3U), 3U, (Span){0U, 0U});
    SAG_ASSERT(sag_menu_move(&m, 1, false));
    SAG_ASSERT(sag_menu_move(&m, 5, false));
    SAG_ASSERT_EQ_I64(m.sel, 2); /* clamped, not wrapped */
    sag_menu_free(&m);
}

void test_menu_scrolls_to_keep_the_selection_visible(void)
{
    static const char *const rows[] = {"r0", "r1", "r2", "r3", "r4",
                                       "r5", "r6", "r7"};
    MenuSpec spec = {NULL, 3U, false, false, 0U};
    Menu m;
    u16 i;

    sag_menu_init(&m, &spec);
    sag_menu_reset(&m, items_of(rows, 8U), 8U, (Span){0U, 0U});
    /* Sprint 18's menu showed the first five rows and offered no way to
     * reach the sixth; this one scrolls. */
    SAG_ASSERT_EQ_U64(sag_menu_rows(&m, 24U), 3U);
    SAG_ASSERT_EQ_U64(sag_menu_rows(&m, 2U), 2U); /* clipped by area */

    for (i = 0U; i < 8U; i++)
        SAG_ASSERT(sag_menu_move(&m, 1, false));
    SAG_ASSERT_EQ_I64(m.sel, 7);
    sag_menu_free(&m);
}

void test_menu_page_moves_by_the_visible_row_count(void)
{
    static const char *const rows[] = {"r0", "r1", "r2", "r3", "r4",
                                       "r5", "r6", "r7"};
    MenuSpec spec = {NULL, 3U, false, false, 0U};
    Menu m;

    sag_menu_init(&m, &spec);
    sag_menu_reset(&m, items_of(rows, 8U), 8U, (Span){0U, 0U});
    SAG_ASSERT(sag_menu_move(&m, 1, false));
    SAG_ASSERT_EQ_I64(m.sel, 0);
    SAG_ASSERT(sag_menu_move(&m, 1, true));
    SAG_ASSERT_EQ_I64(m.sel, 3);
    SAG_ASSERT(sag_menu_move(&m, 1, true));
    SAG_ASSERT_EQ_I64(m.sel, 6);
    SAG_ASSERT(sag_menu_move(&m, -1, true));
    SAG_ASSERT_EQ_I64(m.sel, 3);
    sag_menu_free(&m);
}

void test_menu_dismiss_and_empty_are_inert(void)
{
    static const char *const rows[] = {"a"};
    Menu m;

    sag_menu_init(&m, NULL);
    SAG_ASSERT(!sag_menu_move(&m, 1, false)); /* nothing to move through */
    SAG_ASSERT_EQ_U64(sag_menu_rows(&m, 24U), 0U);
    SAG_ASSERT_NULL(sag_menu_selected(&m));

    sag_menu_reset(&m, items_of(rows, 1U), 1U, (Span){0U, 0U});
    SAG_ASSERT(sag_menu_move(&m, 1, false));
    sag_menu_dismiss(&m);
    SAG_ASSERT_EQ_I64(m.sel, -1);
    SAG_ASSERT(!m.explicit_sel);
    SAG_ASSERT_EQ_U64(m.items.len, 0U);
    sag_menu_free(&m);
}
