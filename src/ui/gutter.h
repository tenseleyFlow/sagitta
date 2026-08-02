#ifndef SAG_UI_GUTTER_H
#define SAG_UI_GUTTER_H

#include <stdbool.h>
#include <stddef.h>

#include "text/coords.h"
#include "util/base.h"

typedef struct Ed Ed;
typedef struct Win Win;

typedef enum {
    SAG_NUM_NONE,
    SAG_NUM_ABS,
    SAG_NUM_REL,
    SAG_NUM_HYBRID
} NumStyle;

/* The two empty sign cells and number field are always reserved.  NONE
 * blanks the number field; it does not change document geometry. */
enum { SAG_GUTTER_SIGN_COLS = 2 };

u16 sag_gutter_width(const Win *w);
u16 sag_gutter_width_for(u64 line_count, NumStyle style);
size_t sag_gutter_number(char *dst, size_t cap, NumStyle style,
                         LineNo line, LineNo cursor_line,
                         bool continuation);
void sag_gutter_draw(Ed *ed, Win *w, u16 lo, u16 hi);

#endif
