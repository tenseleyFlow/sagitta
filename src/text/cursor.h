#ifndef SAG_TEXT_CURSOR_H
#define SAG_TEXT_CURSOR_H

#include <stdint.h>

#include "text/piece.h"
#include "unicode/coords.h"

#define SAG_GCOL_EOL UINT64_MAX

typedef struct Cursor {
    ByteOff pos;
    GCol goal_col;
    ByteOff anchor;
    GCol motion_col;       /* transient actual column after vertical move */
    u8 motion_col_valid;
} Cursor;

_Static_assert(sizeof(Cursor) <= 40U, "cursor bloat");

void sag_cursor_left(const TextBuf *tb, Cursor *c);
void sag_cursor_right(const TextBuf *tb, Cursor *c);
void sag_cursor_up(const TextBuf *tb, Cursor *c);
void sag_cursor_down(const TextBuf *tb, Cursor *c);
void sag_cursor_line_home(const TextBuf *tb, Cursor *c);
void sag_cursor_line_end(const TextBuf *tb, Cursor *c);
void sag_cursor_buf_home(const TextBuf *tb, Cursor *c);
void sag_cursor_buf_end(const TextBuf *tb, Cursor *c);
void sag_cursor_clamp(const TextBuf *tb, Cursor *c);

#endif
