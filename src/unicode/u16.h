#ifndef YEW_UNICODE_U16_H
#define YEW_UNICODE_U16_H

#include "text/piece.h"

/* UTF-16 code units from the start of one logical line. */
typedef struct {
    u64 v;
} U16Col;

#define U16COL(x) ((U16Col){(x)})

U16Col yew_off_to_u16col(const TextBuf *tb, Span line, ByteOff pos);
/* Values inside an astral scalar and values beyond content clamp backward. */
ByteOff yew_u16col_to_off(const TextBuf *tb, Span line, U16Col col);

#endif
