#ifndef SAG_UI_DRAW_H
#define SAG_UI_DRAW_H

typedef struct Ed Ed;
typedef struct Win Win;

/* Draws the document rectangle and Sprint 14's one-row footer. */
void sag_draw_win(Ed *ed, Win *w);

#endif
