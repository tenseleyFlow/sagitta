#ifndef YEW_UNICODE_COORDS_H
#define YEW_UNICODE_COORDS_H

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

typedef struct YewTextCluster {
    Span bytes;
    u32 base_cp;
    u32 cells;
    bool tab;
} YewTextCluster;

/* Decode the grapheme cluster beginning at `at` without flattening the
 * piece table. `at` must be a grapheme boundary inside `span`. */
bool yew_text_cluster_next(const TextBuf *tb, Span span, ByteOff at,
                           YewTextCluster *out);

GCol yew_off_to_gcol(const TextBuf *tb, Span line, ByteOff pos);
ByteOff yew_gcol_to_off(const TextBuf *tb, Span line, GCol g);
CharCol yew_off_to_charcol(const TextBuf *tb, Span line, ByteOff pos);
CCol yew_off_to_ccol(const TextBuf *tb, Span line, ByteOff pos, u32 tabw);
ByteOff yew_ccol_to_off(const TextBuf *tb, Span line, CCol c, u32 tabw);
/* Block-paste coordinate helpers. The offset variant chooses content-end
 * when the requested cell lies beyond the line; shortfall is the number of
 * real padding spaces needed after round-left. */
ByteOff yew_ccol_to_off_padded(const TextBuf *tb, Span line, CCol c,
                               u32 tabw);
CCol yew_ccol_max(CCol left, CCol right);
u64 yew_ccol_shortfall(CCol target, CCol landed);
u32 yew_tab_cells(CCol at, u32 tabw);

ByteOff yew_grapheme_next(const TextBuf *tb, ByteOff pos);
ByteOff yew_grapheme_prev(const TextBuf *tb, ByteOff pos);
bool yew_is_grapheme_boundary(const TextBuf *tb, ByteOff pos);

/* Cursor fast paths: pos must already satisfy the boundary invariant. */
ByteOff yew_grapheme_next_boundary(const TextBuf *tb, ByteOff pos);
ByteOff yew_grapheme_prev_boundary(const TextBuf *tb, ByteOff pos);

#endif
