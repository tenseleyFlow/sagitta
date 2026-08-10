#ifndef YEW_SYN_THEME_H
#define YEW_SYN_THEME_H

#include "syn/attr.h"
#include "term/grid.h"

typedef struct ThemeEnt {
    YewColor fg;
    YewColor bg;
    u16 attrs;
} ThemeEnt;

/* Sprint 39's debug table: comments are dim, every other entry is default. */
const ThemeEnt *yew_theme_table(void);

/* The external theme format and loader are deliberately deferred. */
void yew_theme_load(const char *path);

#endif
