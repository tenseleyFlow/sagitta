#ifndef SAG_EDIT_WORD_H
#define SAG_EDIT_WORD_H

#include <stdbool.h>

#include "edit/motion.h"

/* The UAX #29 oracle accepts codepoint boundaries, including invalid-byte
 * escape boundaries.  Editor motions below only expose grapheme boundaries. */
bool sag_word_boundary(const TextBuf *tb, ByteOff pos);

ByteOff sag_word_sub_next(UnitCtx *u, ByteOff p);
ByteOff sag_word_sub_prev(UnitCtx *u, ByteOff p);

#endif
