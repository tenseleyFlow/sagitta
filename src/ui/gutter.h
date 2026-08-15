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

typedef enum SignKind {
    YEW_SIGN_DIAG = 0,
    YEW_SIGN_GIT,
    YEW_SIGN_SHADOW,
    YEW_SIGN_NKIND
} SignKind;

typedef struct GutterSign {
    const u8 *glyph;
    u8 nbytes;
    const char *role;
    u8 attrs;
} GutterSign;

typedef struct GutterSignLine {
    LineNo line;
    GutterSign sign[YEW_SIGN_NKIND];
    u8 mask;
} GutterSignLine;

typedef struct GutterSigns {
    GutterSignLine *v;
    u32 len;
    u32 cap;
} GutterSigns;

u16 yew_gutter_width(const Win *w);
u16 yew_gutter_width_for(u64 line_count, NumStyle style);
size_t yew_gutter_number(char *dst, size_t cap, NumStyle style,
                         LineNo line, LineNo cursor_line,
                         bool continuation);
void yew_gutter_draw(Ed *ed, Win *w, u16 lo, u16 hi);
/* Sign bytes and role names are borrowed until the next clear/free. */
void yew_gutter_sign_set(Win *w, LineNo line, SignKind kind,
                         const GutterSign *sign);
void yew_gutter_signs_clear(Win *w, LineNo lo, LineNo hi);
void yew_gutter_sign_clear_kind(Win *w, LineNo lo, LineNo hi,
                                SignKind kind);
void yew_gutter_signs_free(Win *w);

#endif
