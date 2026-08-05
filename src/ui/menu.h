#ifndef SAG_UI_MENU_H
#define SAG_UI_MENU_H

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
} Menu;

void sag_menu_init(Menu *m, const MenuSpec *spec);
void sag_menu_free(Menu *m);

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
void sag_menu_reset(Menu *m, Vec_CompItem items, u32 total, Span replace);

/* Moves the selection by `delta` rows (or pages), marking it explicit.
 * Returns false when there is nothing to move through. */
bool sag_menu_move(Menu *m, i32 delta, bool page);
/* Rows the menu would draw in an area `height` cells tall. */
u16 sag_menu_rows(const Menu *m, u16 height);
const CompItem *sag_menu_selected(const Menu *m);
void sag_menu_dismiss(Menu *m);

/*
 * Draws bottom-aligned inside `area` and registers one
 * SAG_REGION_MENU_ROW per drawn row from the same Rect it drew with,
 * plus a SAG_REGION_BLOCK over the whole list so a click on a gap does
 * not fall through to the pane beneath (Sprint 22's law).
 */
void sag_menu_draw(Ed *ed, Menu *m, Rect area, const SagUiStyle *style);

#endif
