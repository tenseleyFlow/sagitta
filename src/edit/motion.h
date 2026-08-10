#ifndef YEW_EDIT_MOTION_H
#define YEW_EDIT_MOTION_H

#include <stdbool.h>

#include "edit/mode.h"
#include "text/piece.h"

typedef struct Buffer Buffer;
typedef struct Win Win;

enum { YEW_SEL_DEPTH = 16 };

typedef struct SelStack {
    Span s[YEW_SEL_DEPTH];
    u8 n;
} SelStack;

typedef struct UnitCtx {
    const TextBuf *tb;
    const Buffer *buf;
    Win *win;
} UnitCtx;

typedef struct UnitOps {
    const char *name;
    ByteOff (*next)(UnitCtx *u, ByteOff p, bool alt);
    ByteOff (*prev)(UnitCtx *u, ByteOff p, bool alt);
    ByteOff (*home)(UnitCtx *u, ByteOff p, bool alt);
    ByteOff (*end)(UnitCtx *u, ByteOff p, bool alt);
    Span (*span)(UnitCtx *u, ByteOff p, bool alt);
} UnitOps;

extern const UnitOps yew_unit_line;
extern const UnitOps yew_unit_word;
extern const UnitOps yew_unit_block;
extern const UnitOps yew_unit_char;

const UnitOps *yew_unit_of_mode(Mode mode);
void yew_selstack_clear(Win *win);

#endif
