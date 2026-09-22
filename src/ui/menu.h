#ifndef YEW_UI_MENU_H
#define YEW_UI_MENU_H

/*
 * Sprint 18.5 §5: the ranked-list widget.
 *
 * Extracted from cmdline.c so Sprint 26's list picker is an INSTANCE of
 * this rather than a second implementation of it.  What lives here is
 * everything that is true of any ranked list: the selection model, the
 * scroll window, match highlighting, and the clickable rows.  What stays
 * in cmdline.c is what is specific to a command line -- where the menu
 * sits, what a row means when accepted, and how the prompt text changes.
 */

#include <stdbool.h>

#include "term/grid.h"
#include "ui/cmdcomp.h"
#include "ui/layout.h"
#include "ui/statusline.h"
#include "util/base.h"

typedef struct Ed Ed;

typedef struct MenuSpec {
    /* NULL draws the list inline (the command line's menu); Sprint 26's
     * picker sets one and gets a framed box. */
    const char *title;
    u16 max_rows;   /* visible rows; 0 = as many as the area allows */
    bool ghost;     /* the host may preview the selection inline    */
    bool wrap;      /* selection wraps at both ends                 */
    u16 detail_col; /* cell column the detail text starts at        */
} MenuSpec;

typedef struct Menu {
    MenuSpec spec;
    Vec_CompItem items;
    /*
     * -1 means nothing is selected.  `explicit_sel` says the user CHOSE
     * this row (Tab, C-n/C-p, a page key, a click) rather than the
     * filter merely having ranked it first -- which is what §6's Enter
     * rule turns on.  Filtering never sets it.
     */
    i32 sel;
    bool explicit_sel;
    /*
     * Sprint 57.17 §2: the list holds keyboard FOCUS -- the arrows are
     * the list's rather than the host's.  Distinct from `explicit_sel`,
     * which records that a row was chosen by ANY means (Tab, a click,
     * arrowing in); focus records who the next arrow belongs to, which
     * is what lets `<up>` be the pager when the list has it and history
     * when it does not.
     */
    bool focus;
    u32 top; /* first visible row */
    Span replace;
    u32 total; /* pre-cap match count, for the footer */
    char *stem; /* restored when the menu is dismissed */
    /*
     * A private copy of the selected row's text, so identity survives a
     * refilter that RESET the arena the items' strings live in.  Reading
     * it back out of `items` at reset time would be a use-after-free the
     * moment the filter re-enumerated.
     */
    char *held;
    bool scanning;
    /*
     * Sprint 57.24 §5.5: a completion generator has been asked and has
     * not answered.  The footer shows `…` -- STATE, set by the host from
     * the filter, so the same state always renders the same cells.
     */
    bool pending;
} Menu;

void yew_menu_init(Menu *m, const MenuSpec *spec);
void yew_menu_free(Menu *m);

/*
 * Install a freshly ranked set.  `items` is taken by value -- the menu
 * owns the vector afterwards, and the strings stay owned by whatever
 * arena the completion filter allocated them from.
 *
 * Selection is carried across by IDENTITY, never by row index: one more
 * character reorders the list, and an index-held selection then slides
 * onto a different row a fraction of a second before Enter.  If the held
 * item is gone from the new set the selection falls to "nothing
 * selected" and NOT to row 0 -- row 0 would be an unexplicit selection
 * that §6's Enter rule would treat as a choice the user never made.
 */
void yew_menu_reset(Menu *m, Vec_CompItem items, u32 total, Span replace);

/*
 * Moves the selection by `delta` rows (or pages), marking it explicit,
 * and scrolls `top` so the new selection stays visible.  Returns false
 * when there is nothing to move through.
 *
 * Sprint 57.17 §3: the scroll is the SAME rule the draw applies, so a
 * move and the paint that follows it cannot disagree about `top`.  The
 * move only knows `spec.max_rows`; the draw refines it with the height
 * it actually got.
 */
bool yew_menu_move(Menu *m, i32 delta, bool page);
/* Selects a row outright -- what a click does.  Explicit, like any other
 * deliberate move.  False when the index is not a row. */
bool yew_menu_select(Menu *m, i32 index);
/*
 * Scrolls the visible window WITHOUT moving the selection, which is what
 * a wheel does: looking around a list is not the same as choosing in it,
 * and §6's Enter rule turns on that difference.
 */
bool yew_menu_scroll(Menu *m, i32 delta, u16 height);
/* Rows the menu would draw in an area `height` cells tall. */
u16 yew_menu_rows(const Menu *m, u16 height);
/*
 * Sprint 57.17 §3: how many of those rows carry CANDIDATES.  One fewer
 * than `yew_menu_rows` whenever the window does not reach the end of the
 * list, because the last row is then the `… and N more` tail.
 */
u16 yew_menu_candidate_rows(const Menu *m, u16 height);
/*
 * Candidates hidden below the window -- the N the tail row counts.  Zero
 * means no tail is drawn.  Exposed so a test (and a second host) can ask
 * the question the draw answers, instead of re-deriving it.
 */
u32 yew_menu_hidden(const Menu *m, u16 height);
const CompItem *yew_menu_selected(const Menu *m);
void yew_menu_dismiss(Menu *m);

/*
 * Sprint 57.17 §2: the pager's focus, usable by any host.
 *
 * `focus` takes the arrows for the list, selecting the first row when
 * nothing is selected yet; false when there is nothing to focus.
 * `blur` hands the arrows back but leaves the selection alone -- what
 * editing the host's text does.  `unselect` hands them back AND drops
 * the choice, leaving the rows on screen with nothing chosen, which is
 * what leaving the pager upwards does: §6's Enter rule must then see
 * no choice at all.
 */
bool yew_menu_focus(Menu *m);
void yew_menu_blur(Menu *m);
void yew_menu_unselect(Menu *m);
bool yew_menu_focused(const Menu *m);

/*
 * Draws bottom-aligned inside `area` and registers one
 * YEW_REGION_MENU_ROW per drawn CANDIDATE row from the same Rect it drew
 * with, plus a YEW_REGION_BLOCK over the whole list so a click on a gap
 * does not fall through to the pane beneath (Sprint 22's law).  The tail
 * row gets no MENU_ROW: it names no candidate, so a click on it must do
 * nothing rather than select whatever row index it happens to sit at.
 *
 * Sprint 57.17 §3 -- THE LAST DRAWN ROW CARRIES EXACTLY ONE COUNT.
 *
 * When the window does not reach the end of the list that row is the
 * tail, `… and N more`, N being the candidates hidden below it, and the
 * right-aligned footer is not drawn.  Otherwise the row is an ordinary
 * candidate and the footer carries the precise count ("%u/%u",
 * "%u+ of %u").  Never both: a tail counting what is BELOW the window
 * beside a footer counting the WHOLE ranked set reads as two counts
 * disagreeing, and a golden would then pin the disagreement.
 *
 * "scanning…" is the one exception, and it is not a count: while the
 * set is still arriving the footer wins and no tail is drawn, because a
 * tail over a list that is still growing would be a lie by the time it
 * was read.
 *
 * The cost, recorded so it is a choice and not an accident: while the
 * window is scrolled the precise "%u/%u" position is not on screen.
 * Keeping it would put "3/82" beside "… and 78 more" -- two totals of
 * two different things on one row, which is the confusion the rule
 * exists to prevent, and making them agree would mean teaching the
 * footer a second denominator for the capped case.  The tail's N is
 * measured from `items` and `top` alone, so there is nothing for a
 * later change to make it disagree WITH.
 */
void yew_menu_draw(Ed *ed, Menu *m, Rect area, const YewUiStyle *style);

#endif
