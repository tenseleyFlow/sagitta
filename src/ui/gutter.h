#ifndef YEW_UI_GUTTER_H
#define YEW_UI_GUTTER_H

#include <stdbool.h>
#include <stddef.h>

#include "text/coords.h"
#include "util/base.h"

typedef struct Ed Ed;
typedef struct Win Win;

typedef enum {
    YEW_NUM_NONE,
    YEW_NUM_ABS,
    YEW_NUM_REL,
    YEW_NUM_HYBRID
} NumStyle;

/* The two empty sign cells and number field are always reserved.  NONE
 * blanks the number field; it does not change document geometry. */
enum { YEW_GUTTER_SIGN_COLS = 2 };

u16 yew_gutter_width(const Win *w);
u16 yew_gutter_width_for(u64 line_count, NumStyle style);
size_t yew_gutter_number(char *dst, size_t cap, NumStyle style,
                         LineNo line, LineNo cursor_line,
                         bool continuation);
void yew_gutter_draw(Ed *ed, Win *w, u16 lo, u16 hi);

#endif
