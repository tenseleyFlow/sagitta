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

ByteOff sag_grapheme_next(const TextBuf *tb, ByteOff pos);
ByteOff sag_grapheme_prev(const TextBuf *tb, ByteOff pos);
bool sag_is_grapheme_boundary(const TextBuf *tb, ByteOff pos);

#endif
