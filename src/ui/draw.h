#ifndef SAG_UI_DRAW_H
#define SAG_UI_DRAW_H

#include "util/base.h"

typedef struct Ed Ed;
typedef struct Win Win;

void sag_draw_document_rows(Ed *ed, Win *w, u16 lo, u16 hi);
void sag_draw_footer(Ed *ed, Win *w);
void sag_draw_cursor(Ed *ed, Win *w);
/* Draws the complete Sprint 15 viewport and footer. */
void sag_draw_win(Ed *ed, Win *w);

/* Sprint 22 §7: draws every leaf, then the borders their split nodes
 * own, registering a region for each with the SAME rect it drew. */
void sag_draw_panes(Ed *ed);
bool sag_draw_pane_is_focused(const Ed *ed, const Win *w);

#endif
