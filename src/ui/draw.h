#ifndef YEW_UI_DRAW_H
#define YEW_UI_DRAW_H

#include "util/base.h"

typedef struct Ed Ed;
typedef struct Win Win;

void yew_draw_document_rows(Ed *ed, Win *w, u16 lo, u16 hi);
void yew_draw_footer(Ed *ed, Win *w);
void yew_draw_cursor(Ed *ed, Win *w);
/* Draws the complete Sprint 15 viewport and footer. */
void yew_draw_win(Ed *ed, Win *w);

/* Sprint 22 §7: draws every leaf, then the borders their split nodes
 * own, registering a region for each with the SAME rect it drew. */
void yew_draw_panes(Ed *ed);
bool yew_draw_pane_is_focused(const Ed *ed, const Win *w);

#endif
