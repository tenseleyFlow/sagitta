#ifndef SAG_UNICODE_COORDS_H
#define SAG_UNICODE_COORDS_H

#include "text/piece.h"

typedef struct {
    u64 v;
} CharCol;

typedef struct {
    u64 v;
} GCol;

typedef struct {
    u64 v;
} CCol;

GCol sag_off_to_gcol(const TextBuf *tb, Span line, ByteOff pos);
ByteOff sag_gcol_to_off(const TextBuf *tb, Span line, GCol g);
CharCol sag_off_to_charcol(const TextBuf *tb, Span line, ByteOff pos);
CCol sag_off_to_ccol(const TextBuf *tb, Span line, ByteOff pos, u32 tabw);
ByteOff sag_ccol_to_off(const TextBuf *tb, Span line, CCol c, u32 tabw);
/* Block-paste coordinate helpers. The offset variant chooses content-end
 * when the requested cell lies beyond the line; shortfall is the number of
 * real padding spaces needed after round-left. */
ByteOff sag_ccol_to_off_padded(const TextBuf *tb, Span line, CCol c,
                               u32 tabw);
CCol sag_ccol_max(CCol left, CCol right);
u64 sag_ccol_shortfall(CCol target, CCol landed);

ByteOff sag_grapheme_next(const TextBuf *tb, ByteOff pos);
ByteOff sag_grapheme_prev(const TextBuf *tb, ByteOff pos);
bool sag_is_grapheme_boundary(const TextBuf *tb, ByteOff pos);

/* Cursor fast paths: pos must already satisfy the boundary invariant. */
ByteOff sag_grapheme_next_boundary(const TextBuf *tb, ByteOff pos);
ByteOff sag_grapheme_prev_boundary(const TextBuf *tb, ByteOff pos);

#endif
