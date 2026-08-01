#ifndef SAG_EDIT_MULTICURSOR_H
#define SAG_EDIT_MULTICURSOR_H

#include <stdbool.h>

#include "text/cursor.h"
#include "util/vec.h"

VEC_DECL(SagCursorVec, Cursor);

typedef struct CursorSet {
    SagCursorVec curs;
    u32 primary;
} CursorSet;

void sag_cset_init(CursorSet *cs, Cursor primary);
void sag_cset_free(CursorSet *cs);
bool sag_cset_add(CursorSet *cs, Cursor c);
void sag_cset_remove_all_but_primary(CursorSet *cs);
void sag_cset_normalize(const TextBuf *tb, CursorSet *cs);
void sag_cset_adjust(CursorSet *cs, u8 op, ByteOff at, u64 len);

/* Internal-invariant and pre-Sprint-17 editing-boundary checks. */
void sag_cset_check(const CursorSet *cs);
void sag_cset_require_single_edit(const CursorSet *cs);

#endif
