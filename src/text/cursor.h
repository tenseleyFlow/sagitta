#ifndef YEW_TEXT_CURSOR_H
#define YEW_TEXT_CURSOR_H

#include <stdint.h>

#include "text/piece.h"
#include "unicode/coords.h"

#define YEW_GCOL_EOL UINT64_MAX

typedef struct Cursor {
    ByteOff pos;
    GCol goal_col;
    ByteOff anchor;
} Cursor;

_Static_assert(sizeof(Cursor) == 24U, "cursor layout changed");

void yew_cursor_left(const TextBuf *tb, Cursor *c);
void yew_cursor_right(const TextBuf *tb, Cursor *c);
void yew_cursor_up(const TextBuf *tb, Cursor *c);
void yew_cursor_down(const TextBuf *tb, Cursor *c);
void yew_cursor_line_home(const TextBuf *tb, Cursor *c);
void yew_cursor_line_end(const TextBuf *tb, Cursor *c);
void yew_cursor_buf_home(const TextBuf *tb, Cursor *c);
void yew_cursor_buf_end(const TextBuf *tb, Cursor *c);
void yew_cursor_clamp(const TextBuf *tb, Cursor *c);

#endif
