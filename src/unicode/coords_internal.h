#ifndef SAGITTA_UNICODE_COORDS_INTERNAL_H
#define SAGITTA_UNICODE_COORDS_INTERNAL_H

#include "text/piece.h"

/* TextBuf construction and destruction own this private cache lifecycle. */
void sag_coords_index_seed(TextBuf *tb);
void sag_coords_index_dispose(TextBuf *tb);
void sag_coords_index_note_edit(TextBuf *tb, Span old_range,
                                u64 inserted_len, Span old_affected,
                                u64 old_gen);

#endif
